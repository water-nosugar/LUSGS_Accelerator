#!/usr/bin/env python3
"""Validate optimization ownership, ordered arithmetic and same-resource pairs."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from check_single_line_storage import check_case
from check_single_line_c import fields
from check_single_line_onchip import check_onchip
from check_single_line_dma_packets import check_dma_packets


def union_length(intervals):
    end = -1
    total = 0
    for lo, hi in sorted(intervals):
        total += max(0, hi-max(end, lo))
        end = max(end, hi)
    return total


def analyze(path, row):
    text = (path/'simout.txt').read_text()
    config = row['config']
    level = int(config.get('--coeff-line-opt-level', '0'))
    with (path/'coeff_preprocess_trace.csv').open() as src:
        trace = list(csv.DictReader(src))
    check_onchip(text,config,row['storage'],trace)
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as src:
        cluster = list(csv.DictReader(src))
    schedule = fields(text, 'CFD_LINE_SCHEDULE')
    interface = fields(text, 'CFD_INTERFACE_DMA')
    cache = fields(text, 'CFD_CACHE_DMA')
    fixture = fields(text, 'CFD_CACHE_CONTRACT')
    pools = {p['pool']:p for p in fields(text, 'CFD_RESOURCE_STATS')}
    if config.get('--coeff-rhs-row-parallel','0') == '1':
        rows = fields(text, 'CFD_RHS_ROWS')[-1]
        capacity=int(config.get('--coeff-rhs-product-depth','60'))
        assert int(rows['capacity'])==capacity and int(rows['bytes'])==capacity*64+256
        assert int(rows['live'])==0 and int(rows['issued'])==int(rows['completed'])
        assert 0 <= int(rows['peak']) <= capacity
        assert int(rows['issued']) == 8*sum(r['stage']=='RHS_COMPLETE' for r in trace)
    if level>=5:
        for lifetime in [r for r in fields(text,'CFD_LINE_LIFETIME') if r.get('phase')=='complete']:
            assert int(lifetime['records_pending'])==0
            assert int(lifetime['warm_output_peak'])<=int(lifetime['warm_output_depth'])
        queue=fields(text,'CFD_COMPLETION_QUEUE')[-1]
        depth=int(config.get('--coeff-completion-depth','8'))
        assert int(queue['depth'])==depth and int(queue['bytes'])==depth*(512 if level>=11 else 1024)
        assert int(queue['live'])==0 and int(queue['issued'])==int(queue['completed'])
        assert int(queue['peak'])<=depth
        assert int(queue['issued'])==sum(r['stage']=='REQUEST_DETACH' for r in trace)
    if level>=11:
        def digests(prefix):
            rows=fields(text,prefix)
            values={(r['request'],r['generation']):r['hash'] for r in rows}
            assert len(values)==len(rows)
            return values
        source=digests('CFD_RECORD_DIGEST_SOURCE')
        guest=digests('CFD_RECORD_DIGEST_GUEST')
        assert source==guest and len(source)==int(queue['issued']), 'packed record roundtrip changed fields'
        check_dma_packets(text,path,config)
    completed = defaultdict(list)
    issued = Counter()
    events = defaultdict(dict)
    for r in trace:
        if r['stage'] in ('REQUEST_ALLOC','REQUEST_COMPLETE','REQUEST_AUTO_RETIRE',
                           'TILE_DATA_COMMIT','REQUEST_DETACH','DRAIN_DQSTAR_VECTOR_DONE','DRAIN_UBAR_DONE'):
            events[r['requestId']][r['stage']] = int(r['cycle'])
        key = (r['requestId'], r['rhsBatch'])
        if r['stage']=='COLUMN_FMA_ISSUE':
            issued[key,int(r['rhsColumn'])] += 1
        if r['stage']=='COLUMN_FMA_COMPLETE':
            completed[key].append(int(r['rhsColumn']))
    for key, sequence in completed.items():
        assert sequence==list(range(5)), (path.name, 'unordered column sum', key, sequence)
        assert all(issued[key,i]==1 for i in range(5))
    assert sum(issued.values())==sum(len(v) for v in completed.values())
    if level>=2:
        for request,e in events.items():
            assert e['TILE_DATA_COMMIT']==e['REQUEST_COMPLETE']<=e['REQUEST_AUTO_RETIRE']
            assert e['TILE_DATA_COMMIT']>=max(e.get('DRAIN_DQSTAR_VECTOR_DONE',0),e.get('DRAIN_UBAR_DONE',0))
            if level>=5:
                assert e['TILE_DATA_COMMIT']<=e['REQUEST_DETACH']<=e['REQUEST_AUTO_RETIRE']
    windows=[]
    for sweep, s in enumerate(schedule):
        token=s['token']
        start=min(int(p['cycle']) for p in interface if p['token']==token and p['kind']=='descriptor')
        end=int(s['visible_done'])+1
        ops=[r for r in cluster if start<=int(r['cycle'])<end]
        count=Counter()
        intervals=[]
        for r in ops:
            count[r['pool']]+=int(r['lanes'])
            intervals.append((int(r['cycle']),int(r['complete_cycle'])))
        duration=end-start
        inputs=[int(r['cell']) for r in trace if r['stage']=='INPUT_START' and start<=int(r['cycle'])<end]
        if level>=1:
            assert inputs==sorted(inputs), (path.name, 'non-frontier input admission',inputs)
        bp=[p for p in fields(text,'CFD_TILE_DMA') if p['token']==token]
        if level>=3 and int(s['cache_hit']):
            assert not any(p['region']=='1' for p in bp), 'cached Ubar reloaded through DMA'
        warm=dict()
        mode = fixture[sweep]['mode'] if fixture else config['--coeff-base-solve']
        cells = int(fixture[sweep]['cells']) if fixture else int(config['--lusgs-cells'])
        if level>=3 and int(s['cache_hit']) and mode=='inverse':
            packets=[p for p in cache if p['token']==token]
            n=cells;lifetimes=[];prefetched=0;write_overlap=0
            rstarts=[];ystarts=[];yends=[]
            for i in range(n):
                one=[p for p in packets if int(p['cell'])==i]
                assert not any(p['kind']=='ubar' for p in one)
                ra=min(int(p['cycle']) for p in one if p['kind']=='rhs' and p['event']=='issue')
                rb=max(int(p['cycle']) for p in one if p['kind']=='rhs' and p['event']=='response')
                ya=min(int(p['cycle']) for p in one if p['kind']=='y' and p['event']=='issue')
                yb=max(int(p['cycle']) for p in one if p['kind']=='y' and p['event']=='response')
                assert ra<=rb<=ya<=yb<=int(s['visible_done'] if level>=10 else s['backward_start'])
                rstarts.append(ra);ystarts.append(ya);yends.append(yb)
                if level>=5:
                    releases=[p for p in fields(text,'CFD_WARM_INPUT_RELEASE') if p['token']==token and int(p['cell'])==i]
                    assert len(releases)==1 and rb<=int(releases[0]['cycle'])<=ya
                    lifetimes.append((ra,int(releases[0]['cycle'])))
                else:
                    lifetimes.append((ra,yb))
            changes=Counter()
            for a,b in lifetimes:changes[a]+=1;changes[b]-=1
            live=peak=0
            for _,change in sorted(changes.items()):live+=change;peak=max(peak,live)
            assert live==0 and peak<=min(8 if level>=5 else 4,int(config['--coeff-stream-window']))
            prefetched=sum(rstarts[i+1]<yends[i] for i in range(n-1))
            write_overlap=sum(ystarts[i+1]<yends[i] for i in range(n-1))
            if n>1 and int(config['--coeff-stream-window'])>1:
                assert prefetched>0, 'warm RHS prefetch did not occur'
            warm=dict(slot_peak=peak,rhs_prefetch_pairs=prefetched,write_overlap_pairs=write_overlap)
        handoffs=sorted((int(r['cell']),int(r['cycle'])) for r in trace if
                         r['stage']=='HANDOFF_PRODUCE' and start<=int(r['cycle'])<end)
        gaps=Counter(b[1]-a[1] for a,b in zip(handoffs,handoffs[1:]))
        windows.append(dict(token=token,cache_hit=int(s['cache_hit']),cycles=duration,
            issued=dict(count),issue_utilization_pct={p:100*count[p]*(int(config['--coeff-cluster-div-ii']) if p=='div' else 1)/(duration*(1 if p=='div' else 5)) for p in ('mul','add','div')},
            empty_cycles=duration-union_length(intervals),
            empty_pct=100*(duration-union_length(intervals))/duration,
            backward_cycles=int(s['backward_done'])-int(s['backward_start']),
            input_order=inputs,warm=warm,handoff_gap_histogram=dict(gaps)))
    samples=int(pools['shared_mul']['samples'])
    assert sum(w['cycles'] for w in windows)==samples
    for p in ('mul','add','div'):
        assert sum(w['issued'].get(p,0) for w in windows)==int(pools['shared_'+p]['issued'])
    return dict(cycles=row['cycles'],hashes=row['hashes'],max_abs=row['max_abs'],
                binary=row['binary'],config=config,windows=windows,
                pools=pools,storage=row['storage'],cluster_buffer=fields(text,'CFD_CLUSTER_BUFFER')[-1])


def check(root,suite_path,report):
    suite=json.loads(suite_path.read_text());results={};groups=defaultdict(list)
    for case in suite['cases']:
        path=root/case['name']
        result=analyze(path,check_case(path));results[case['name']]=result
        config=dict(result['config']);config.pop('--coeff-line-opt-level',None)
        config.pop('--coeff-rhs-row-parallel',None)
        groups[json.dumps(config,sort_keys=True)].append(case['name'])
        print(case['name'],'cycles',result['cycles'],'error',result['max_abs'])
    assert len({r['binary'] for r in results.values()})==1, 'mixed gem5 binaries'
    for names in groups.values():
        baseline=results[names[0]]['hashes']
        assert all(results[n]['hashes']==baseline for n in names), ('optimization changed FP64 order',names)
    if report:report.write_text(json.dumps(results,indent=2)+'\n')
    print('SINGLE_LINE_OPT_CHECK_PASS',len(results),'cases',sum(len(r['windows']) for r in results.values()),'sweeps')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('campaign',type=Path);parser.add_argument('--suite',type=Path,required=True)
    parser.add_argument('--report',type=Path)
    args=parser.parse_args();check(args.campaign,args.suite,args.report)
