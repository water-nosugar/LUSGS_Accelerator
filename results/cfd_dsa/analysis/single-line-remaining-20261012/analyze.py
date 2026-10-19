#!/usr/bin/env python3
"""Audit remaining stalls using accepted traces; never modify model settings."""
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path
import statistics
import sys

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
sys.path.insert(0,str(REPO/'projects/cfd_dsa/tools'))
from check_single_line_c import fields
from check_single_line_opt import union_length


def summarize(values):
    return dict(mean=statistics.mean(values),median=statistics.median(values),max=max(values)) if values else {}


def phase(ops,a,b,ii):
    issue={p:Counter() for p in ('mul','add','div')}
    active={p:[] for p in issue};callers=Counter();spans=[]
    for r in ops:
        c,e,l,p=r['cycle'],r['complete_cycle'],r['lanes'],r['pool']
        if a<=c<b:
            issue[p][c]+=l;callers[str(r['caller'])]+=l
        if c<b and e>a:
            span=(max(a,c),min(b,e));spans.append(span);active[p].append(span)
    duration=b-a
    pools={}
    for p,counts in issue.items():
        issued=sum(counts.values())
        assert max(counts.values(),default=0)<=(1 if p=='div' else 5)
        pools[p]=dict(issued=issued,issue_cycles=len(counts),
            issue_util_pct=100*issued*(ii if p=='div' else 1)/(duration*(1 if p=='div' else 5)),
            active_pct=100*union_length(active[p])/duration,
            mean_lanes_on_issue=issued/len(counts) if counts else 0,
            issue_width_hist=dict(Counter(counts.values())))
    return dict(cycles=duration,pools=pools,caller_work=dict(callers),
                empty_cycles=duration-union_length(spans))


def audit(path,row):
    text=(path/'simout.txt').read_text()
    trace=list(csv.DictReader((path/'coeff_preprocess_trace.csv').open()))
    ops=list(csv.DictReader((path/'coeff_preprocess_trace.csv.cluster.csv').open()))
    for r in ops:
        for k in ('cycle','complete_cycle','lanes','caller'):r[k]=int(r[k])
    for r in trace:r['cycle']=int(r['cycle'])
    interface=fields(text,'CFD_INTERFACE_DMA');storage=fields(text,'CFD_LINE_STORAGE')
    schedules=fields(text,'CFD_LINE_SCHEDULE');windows=[]
    for index,s in enumerate(schedules):
        token=s['token'];packets=[p for p in interface if p['token']==token]
        a=min(int(p['cycle']) for p in packets if p['kind']=='descriptor')
        b=int(s['backward_start']);c=int(s['backward_done']);d=int(s['visible_done'])+1
        local=[r for r in trace if a<=r['cycle']<d];events=defaultdict(dict)
        for r in local:
            if r['stage'] in ('REQUEST_ALLOC','INPUT_START','INPUT_TOKEN','LU_ISSUE','LU_COMPLETE',
                 'COEFF3_ISSUE','COEFF3_COMPLETE','REQUEST_COMPLETE','REQUEST_AUTO_RETIRE'):
                events[r['requestId']][r['stage']]=r['cycle']
        latency={}
        for start,end in [('INPUT_START','INPUT_TOKEN'),('INPUT_TOKEN','LU_ISSUE'),
                         ('LU_ISSUE','LU_COMPLETE'),('COEFF3_ISSUE','COEFF3_COMPLETE'),
                         ('REQUEST_COMPLETE','REQUEST_AUTO_RETIRE')]:
            latency[start+'__'+end]=summarize([e[end]-e[start] for e in events.values() if start in e and end in e])
        updates=defaultdict(lambda:[0,0]);allocs=[]
        for e in events.values():
            if 'REQUEST_ALLOC' not in e:continue
            updates[e['REQUEST_ALLOC']][0]+=1;allocs.append(e['REQUEST_ALLOC'])
            updates[e['REQUEST_COMPLETE']][1]+=1
            updates[e['REQUEST_AUTO_RETIRE']][0]-=1
            updates[e['REQUEST_AUTO_RETIRE']][1]-=1
        live=terminal=peak=pressure=0;previous=a
        for time,change in sorted(updates.items()):
            if live==int(row['config']['--coeff-stream-window']) and terminal and previous<max(allocs):
                pressure+=min(time,max(allocs))-previous
            live+=change[0];terminal+=change[1];peak=max(peak,terminal);previous=time
        assert live==terminal==0
        phases={name:phase(ops,x,y,int(row['config']['--coeff-cluster-div-ii'])) for name,x,y in
                 [('forward',a,b),('backward',b,c),('completion',c,d),('whole',a,d)]}
        data_wait=int(storage[index]['data_wait'])
        # The selected/probe split backward chain has no arithmetic overlap
        # with other producers: these disjoint categories must close exactly.
        math=phases['backward']['cycles']-phases['backward']['empty_cycles']
        remaining=c-b-math-data_wait
        assert remaining>=0
        windows.append(dict(token=token,hit=int(s['cache_hit']),phases=phases,latencies=latency,
            backward=dict(math_active=math,tile_wait=data_wait,other_empty=remaining,
                          output_queue_stalls=int(s['output_queue_stalls'])),
            terminal_peak=peak,full_window_with_terminal_pressure=pressure,
            schedule=s,storage=storage[index]))
    assert sum(w['phases']['whole']['cycles'] for w in windows)==int(row['pools']['shared_mul']['samples'])
    warm=[w for w in windows if w['hit']]
    return dict(whole_cycles=row['cycles'],outside_controller_cycles=row['cycles']-sum(w['phases']['whole']['cycles'] for w in windows),
        windows=windows,warm_mean_cycles=statistics.mean(w['phases']['whole']['cycles'] for w in warm) if warm else None,
        row_buffer=fields(text,'CFD_RHS_ROWS')[-1],product_buffer=fields(text,'CFD_CLUSTER_BUFFER')[-1],
        issue_diagnostics=fields(text,'CFD_ISSUE_DIAG')[-3:])


def main():
    oldroot=REPO/'results/cfd_dsa/campaigns/single-line-opt-selection-20261012'
    old=json.loads((oldroot/'optimization.json').read_text());results={}
    selected={'cold-direct':'cold-direct-r1-t4','cold-inverse':'cold-inverse-r1-t4',
              'reuse-direct':'reuse-direct-r1-t4-s4','reuse-inverse':'reuse-inverse-r1-t4'}
    for profile,name in selected.items():results[profile]=audit(oldroot/name,old[name])
    for suite in ('compute','size'):
        root=REPO/f'results/cfd_dsa/campaigns/single-line-remaining-{suite}-20261012'
        rows=json.loads((OUT/(suite+'-report.json')).read_text())
        for name,row in rows.items():
            assert row['binary']==old[selected['cold-direct']]['binary']
            if suite=='compute':
                profile=next(p for p in selected if name.startswith(p+'-'))
                assert row['hashes']==old[selected[profile]]['hashes'], 'diagnostic timing changed numerical bits'
            results[name]=audit(root/name,row)
    (OUT/'details.json').write_text(json.dumps(results,indent=2)+'\n')
    for name,r in results.items():
        w=r['windows'][-1]
        print(name,'whole',r['whole_cycles'],'last controller',w['phases']['whole']['cycles'],
              'phase cycles',[w['phases'][p]['cycles'] for p in ('forward','backward','completion')],
              'backward',w['backward'],'terminal pressure',w['full_window_with_terminal_pressure'],
              'warm mean',r['warm_mean_cycles'])
    print('REMAINING_BOTTLENECK_AUDIT_PASS',len(results),'cases')


if __name__=='__main__':main()
