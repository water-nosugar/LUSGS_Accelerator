#!/usr/bin/env python3
"""Reconstruct DMA credit occupancy and logical context lifetimes from trace."""
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import re
import statistics

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
CASES=json.loads((OUT/'metrics.json').read_text())['cases']

def fields(text,prefix):
    return [dict(re.findall(r'(\w+)=(\S+)',line)) for line in text.splitlines()
            if line.startswith(prefix+' ')]

def occupancy(intervals,start,end,capacity):
    changes=Counter()
    for a,b in intervals:
        a,b=max(a,start),min(b,end)
        if a<b: changes[a]+=1;changes[b]-=1
    current=0;previous=start;area=0;full=0;peak=0
    for t,d in sorted(changes.items()):
        area+=current*(t-previous)
        full+=(t-previous) if current==capacity else 0
        current+=d;peak=max(peak,current);previous=t
    assert current==0 and peak<=capacity,(current,peak,capacity)
    return dict(slot_cycles=area,mean=area/(end-start),peak=peak,
                capacity=capacity,full_cycles=full,full_pct=100*full/(end-start),
                utilization_pct=100*area/(capacity*(end-start)))

def analyze(name):
    case=CASES[name];path=REPO/case['source'];text=(path/'simout.txt').read_text()
    packets={};children=defaultdict(dict);columns=defaultdict(list);handoffs=[]
    stages={'REQUEST_ALLOC','INPUT_START','INPUT_SLOT_RELEASE','LU_ISSUE','LU_COMPLETE',
            'COEFF3_ISSUE','COEFF3_COMPLETE','REQUEST_COMPLETE','REQUEST_AUTO_RETIRE'}
    def packet(pid,event,cycle,category,size):
        p=packets.setdefault(int(pid),dict(category=category,size=int(size)))
        assert p['category']==category and p['size']==int(size)
        assert event not in p
        p[event]=int(cycle)
    with (path/'coeff_preprocess_trace.csv').open() as src:
        for r in csv.DictReader(src):
            stage=r['stage'];rid=r['requestId']
            if stage in ('SPM_PACKET_ISSUE','SPM_PACKET_RESPONSE'):
                packet(r['packetId'],'issue' if stage.endswith('ISSUE') else 'response',
                       r['cycle'],r['requester'],r['size'])
            if stage in stages:
                children[rid][stage]=int(r['cycle']);children[rid]['cell']=int(r['cell'])
            if stage=='COLUMN_FMA_ISSUE':
                columns[rid,r['rhsBatch']].append(int(r['cycle']))
            if stage=='HANDOFF_PRODUCE':
                handoffs.append(dict(cell=int(r['cell']),cycle=int(r['cycle'])))
    for prefix in ('CFD_INTERFACE_DMA','CFD_CACHE_DMA','CFD_TILE_DMA'):
        for r in fields(text,prefix):
            packet(r['packet'],r['event'],r['cycle'],r.get('kind','tile'),r['bytes'])
    for p in packets.values():
        assert 'issue' in p and 'response' in p and p['response']>=p['issue']
    # Final dq packet timestamps are absent from the old trace. They start at
    # backward_start, so the forward occupancy reconstructed here is complete.
    missing=case['dma']['issued']-len(packets)
    assert missing==sum(int(w['schedule']['output_packets_issued']) for w in case['sweeps'])
    tile_events=fields(text,'CFD_TILE_EVENT');cache_events=fields(text,'CFD_CACHE_DMA')
    results=[]
    for w in case['sweeps']:
        start=w['start'];back=int(w['schedule']['backward_start']);end=w['end_exclusive']
        subset=[p for p in packets.values() if start<=p['issue']<back]
        assert all(p['response']<back for p in subset)
        dma=occupancy([(p['issue'],p['response']) for p in subset],start,back,
                      int(case['config']['--coeff-spm-outstanding']))
        by_kind=defaultdict(int)
        for p in subset:by_kind[p['category']]+=p['response']-p['issue']
        assert sum(by_kind.values())==dma['slot_cycles']
        cc=[c for c in children.values() if start<=c['REQUEST_ALLOC']<end]
        contexts={}
        for label,a,b,capacity in (
            ('input_slots','INPUT_START','INPUT_SLOT_RELEASE',int(case['config']['--coeff-input-slots'])),
            ('lu_contexts','LU_ISSUE','LU_COMPLETE',int(case['config']['--lu5-count'])),
            ('solve_contexts','COEFF3_ISSUE','COEFF3_COMPLETE',int(case['config']['--coeff3-count'])),
            ('request_window','REQUEST_ALLOC','REQUEST_AUTO_RETIRE',int(case['config']['--coeff-stream-window']))):
            intervals=[(c[a],c[b]) for c in cc if a in c and b in c]
            contexts[label]=occupancy(intervals,start,end,capacity)
        commits=[]
        for t in tile_events:
            if t['token']!=w['token'] or t['event']!='forward_commit' or not cc:continue
            first,count=int(t['first']),int(t['cells'])
            ready=max(c['REQUEST_COMPLETE'] for c in cc if first<=c['cell']<first+count)
            commits.append(dict(first=first,data_compute_complete=ready,
                                commit=int(t['cycle']),record_gated_delay=int(t['cycle'])-ready))
        rs=dict(token=w['token'],cache_hit=w['cache_hit'],forward_dma=dma,
                dma_slot_cycles_by_kind=dict(by_kind),contexts=contexts,tile_commits=commits,
                record_retire_waits=[c['REQUEST_AUTO_RETIRE']-c['REQUEST_COMPLETE'] for c in cc])
        if w['cache_hit'] and case['config']['--coeff-base-solve']=='inverse':
            ce=[r for r in cache_events if r['token']==w['token']]
            spans={};cells=[]
            for i in range(int(case['config']['--lusgs-cells'])):
                one={}
                for kind in ('rhs','y','ubar'):
                    v=[r for r in ce if int(r['cell'])==i and r['kind']==kind]
                    if not v:continue
                    a=min(int(r['cycle']) for r in v if r['event']=='issue')
                    b=max(int(r['cycle']) for r in v if r['event']=='response')
                    one[kind]=(a,b);spans[kind]=spans.get(kind,0)+b-a
                assert one['rhs'][1]<=one['y'][0]
                if 'ubar' in one:assert one['y'][1]<=one['ubar'][0]
                cells.append(one)
            assert all(max(cells[i][k][1] for k in cells[i])<=cells[i+1]['rhs'][0]
                       for i in range(len(cells)-1))
            compute=sum(c['y'][0]-c['rhs'][1] for c in cells)
            desc=max(p['response'] for p in subset if p['category']=='descriptor')
            forward=back-desc
            rs['cached_forward_decomposition']=dict(after_descriptor_cycles=forward,
                transfer_spans=spans,between_rhs_response_and_y_issue=compute,
                other_control_gaps=forward-sum(spans.values())-compute,cells=cells)
        results.append(rs)
    gaps=Counter(b-a for times in columns.values() for a,b in zip(times,times[1:]))
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as src:
        divisions=[int(r['cycle']) for r in csv.DictReader(src) if r['pool']=='div']
    div_gaps=Counter(b-a for a,b in zip(divisions,divisions[1:]))
    return dict(sweeps=results,ordered_child_column_issue_gaps=dict(gaps),handoffs=handoffs,
                child_lifetimes=list(children.values()),divider_issue_gap_histogram=dict(div_gaps))

names=['overlap-direct-b2-w64','overlap-inverse-b2-w64','out16-direct','out16-inverse',
       'n17-t4-k7-o16-direct','n17-t4-k7-o16-inverse']
result={name:analyze(name) for name in names}
# Independent exclusivity check of backward arithmetic/tile/output intervals.
for case in CASES.values():
    for w in case['sweeps']:
        assert w['backward']['duration']==w['backward']['any_pipeline_active_cycles']+int(w['storage']['data_wait'])+int(w['schedule']['output_queue_stalls'])
assert CASES['out16-direct']['sweeps'][0]['issued']==dict(mul=4850,add=5010,div=1055)
assert CASES['out16-direct']['all_pipelines_empty_cycles']==7393
assert CASES['out16-inverse']['all_pipelines_empty_cycles']==7813
(OUT/'details.json').write_text(json.dumps(result,indent=2)+'\n')
for name,r in result.items():
    for w in r['sweeps'][:2]:
        print(name,'hit',w['cache_hit'],'dma',w['forward_dma'],
              'contexts%',{k:round(v['utilization_pct'],2) for k,v in w['contexts'].items()})
        if 'cached_forward_decomposition' in w:
            print('cached-forward',{k:v for k,v in w['cached_forward_decomposition'].items() if k!='cells'})
print('DETAIL_AUDIT_PASS: six cases; backward exclusive decomposition verified for all 88 sweeps')
