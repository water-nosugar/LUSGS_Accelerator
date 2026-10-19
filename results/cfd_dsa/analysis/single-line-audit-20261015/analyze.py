#!/usr/bin/env python3
"""Reconstruct issue, activity, ownership and phase metrics from recorded traces."""
import csv
import configparser
import json
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
CAMPAIGNS=REPO/'results/cfd_dsa/campaigns'
sys.path.insert(0,str(REPO/'projects/cfd_dsa/tools'))
from check_single_line_c import fields
from check_single_line_opt import union_length


def distribution(values):
    if not values:return dict(count=0)
    return dict(count=len(values),minimum=min(values),mean=statistics.mean(values),
                median=statistics.median(values),maximum=max(values))


def occupancy(intervals,start,end,capacity):
    events=Counter();area=0
    for lo,hi in intervals:
        lo=max(start,lo);hi=min(end,hi)
        if hi>lo:events[lo]+=1;events[hi]-=1;area+=hi-lo
    live=peak=full=0;previous=start
    for cycle,delta in sorted(events.items()):
        if live>=capacity:full+=cycle-previous
        live+=delta;peak=max(peak,live);previous=cycle
    assert live==0 and peak<=capacity
    return dict(average=area/(end-start),peak=peak,capacity=capacity,
                occupied_slot_pct=100*area/((end-start)*capacity),full_cycles=full)


def parse_stats(path):
    result={}
    for line in path.read_text().splitlines():
        parts=line.split()
        if len(parts)>1:
            try:result[parts[0]]=float(parts[1])
            except ValueError:pass
    return result


def analyze(campaign,case):
    root=CAMPAIGNS/campaign;path=root/case
    measured=json.loads((root/'optimization.json').read_text())[case]
    cfg=measured['config'];text=(path/'simout.txt').read_text()
    with (path/'coeff_preprocess_trace.csv').open() as src:trace=list(csv.DictReader(src))
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as src:cluster=list(csv.DictReader(src))
    schedules=fields(text,'CFD_LINE_SCHEDULE');interfaces=fields(text,'CFD_INTERFACE_DMA')
    accesses=fields(text,'CFD_ONCHIP_ACCESS');onchip_starts=fields(text,'CFD_ONCHIP_START')
    onchip_done={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_COMPLETE')}
    onchip_cancel={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_CANCEL')}
    activations={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_ACTIVATE')}
    requests=defaultdict(dict)
    for r in trace:
        if r['stage'] in ('REQUEST_ALLOC','INPUT_START','INPUT_TOKEN','INPUT_SLOT_RELEASE',
                          'LU_ISSUE','LU_COMPLETE','COEFF3_ISSUE','COEFF3_COMPLETE',
                          'REQUEST_COMPLETE','REQUEST_DETACH','REQUEST_AUTO_RETIRE','HANDOFF_PRODUCE'):
            requests[r['requestId']][r['stage']]=int(r['cycle'])
    windows=[]
    for index,s in enumerate(schedules):
        token=s['token'];start=min(int(r['cycle']) for r in interfaces if r['token']==token and r['kind']=='descriptor')
        end=int(s['visible_done'])+1;duration=end-start
        assert duration==measured['windows'][index]['cycles']
        selected=[r for r in cluster if start<=int(r['cycle'])<end]
        active_intervals=[(int(r['cycle']),int(r['complete_cycle'])) for r in selected]
        busy=union_length(active_intervals)
        first_op=min(int(r['cycle']) for r in selected)
        backward=int(s['backward_start']);done=int(s['backward_done'])
        phase={}
        for name,lo,hi in [('forward',start,backward),('backward',backward,done),('tail',done,end)]:
            intervals=[(max(lo,a),min(hi,b)) for a,b in active_intervals if min(hi,b)>max(lo,a)]
            phase[name]=dict(cycles=hi-lo,active=union_length(intervals),empty=hi-lo-union_length(intervals))
        phase['startup']=first_op-start
        assert sum(phase[name]['cycles'] for name in ('forward','backward','tail'))==duration
        assert sum(phase[name]['empty'] for name in ('forward','backward','tail'))==duration-busy
        pools={}
        for pool in ('mul','add','div'):
            rows=[r for r in selected if r['pool']==pool];issues=Counter()
            for r in rows:issues[int(r['cycle'])]+=int(r['lanes'])
            lanes=1 if pool=='div' else 5
            ii=int(cfg['--coeff-cluster-div-ii']) if pool=='div' else 1
            active=union_length([(int(r['cycle']),int(r['complete_cycle'])) for r in rows])
            issued=sum(issues.values())
            pools[pool]=dict(issued=issued,issue_pct=100*issued*ii/(duration*lanes),
                active_pct=100*active/duration,issue_cycles=len(issues),
                mean_width_when_issued=issued/len(issues) if issues else 0,
                width_histogram=dict(Counter(issues.values())),no_issue_cycles=duration-len(issues),
                callers=dict(Counter({caller:sum(int(r['lanes']) for r in rows if r['caller']==caller)
                                      for caller in sorted({r['caller'] for r in rows})})))
            assert issued==measured['windows'][index]['issued'].get(pool,0)
        owners={};residency={}
        for name,a,b,capacity in [
            ('input','INPUT_START','INPUT_SLOT_RELEASE',int(cfg['--coeff-input-slots'])),
            ('lu','LU_ISSUE','LU_COMPLETE',int(cfg['--lu5-count'])),
            ('solve','COEFF3_ISSUE','COEFF3_COMPLETE',int(cfg['--coeff3-count'])),
            ('request','REQUEST_ALLOC','REQUEST_DETACH',int(cfg['--coeff-stream-window'])),
            ('completion','REQUEST_DETACH','REQUEST_AUTO_RETIRE',int(cfg.get('--coeff-completion-depth',8)))]:
            pairs=[(r[a],r[b]) for r in requests.values() if a in r and b in r]
            owners[name]=occupancy(pairs,start,end,capacity)
            residency[name]=distribution([hi-lo for lo,hi in pairs if start<=lo<end])
        residency['input_wait_for_lu']=distribution([r['INPUT_SLOT_RELEASE']-r['INPUT_TOKEN'] for r in requests.values()
            if 'INPUT_SLOT_RELEASE' in r and 'INPUT_TOKEN' in r and start<=r['INPUT_TOKEN']<end])
        residency['input_to_ready']=distribution([r['INPUT_TOKEN']-r['INPUT_START'] for r in requests.values()
            if 'INPUT_TOKEN' in r and start<=r['INPUT_START']<end])
        local_access=[r for r in accesses if start<=int(r['cycle'])<end]
        banks=int(cfg['--coeff-spm-banks']);bank={}
        for op,port in [('R','--coeff-spm-read-ports'),('W','--coeff-spm-write-ports')]:
            rows=[r for r in local_access if r['op']==op];counts=Counter(int(r['bank']) for r in rows)
            cycles=Counter(int(r['cycle']) for r in rows)
            bank[op]=dict(words=len(rows),util_pct=100*len(rows)/(duration*banks*int(cfg[port])),
                active_cycles=len(cycles),full_bandwidth_cycles=sum(v==banks*int(cfg[port]) for v in cycles.values()),
                per_bank=dict(counts))
        jobs=[r for r in onchip_starts if start<=int(r['cycle'])<end and r['id'] in onchip_done]
        service=defaultdict(list)
        for r in jobs:
            service[f"{r['op']}:priority{r['priority']}:words{r['words']}:stride{r['stride']}"] .append(
                onchip_done[r['id']]-activations.get(r['id'],int(r['cycle'])))
        job_intervals=[(int(r['cycle']),onchip_done.get(r['id'],onchip_cancel.get(r['id']))) for r in onchip_starts]
        assert all(b is not None for a,b in job_intervals)
        bank['queue']=occupancy(job_intervals,start,end,int(cfg.get('--coeff-onchip-queue-depth',64)))
        bank['service_cycles']={k:distribution(v) for k,v in service.items()}
        traffic=Counter();packets=Counter();latencies=defaultdict(list)
        spm=[r for r in trace if start<=int(r['cycle'])<end and r['stage']=='SPM_PACKET_ISSUE']
        spm_response={r['packetId']:int(r['cycle']) for r in trace if r['stage']=='SPM_PACKET_RESPONSE'}
        for r in spm:
            kind='input' if r['requester']=='CoeffInput' else 'coefficient_output'
            traffic[kind]+=int(r['size']);packets[kind]+=1
            latencies[kind].append(spm_response[r['packetId']]-int(r['cycle']))
        for prefix in ('CFD_INTERFACE_DMA','CFD_TILE_DMA','CFD_CACHE_DMA'):
            records=[r for r in fields(text,prefix) if r['token']==token]
            responses={r['packet']:int(r['cycle']) for r in records if r['event']=='response'}
            for r in records:
                if r['event']!='issue':continue
                kind=r.get('kind','tile_reload')
                traffic[kind]+=int(r['bytes']);packets[kind]+=1
                latencies[kind].append(responses[r['packet']]-int(r['cycle']))
        traffic['final_output']=int(s['output_bytes_issued']);packets['final_output']=int(s['output_packets_issued'])
        desc_end=max(int(r['cycle']) for r in interfaces if r['token']==token and r['kind']=='descriptor' and r['event']=='response')
        status_start=min(int(r['cycle']) for r in interfaces if r['token']==token and r['kind']=='status' and r['event']=='issue')
        handoffs=[int(r['cycle']) for r in trace if r['stage']=='HANDOFF_PRODUCE' and start<=int(r['cycle'])<end]
        releases=[int(r['cycle']) for r in fields(text,'CFD_WARM_INPUT_RELEASE') if r['token']==token]
        numerical_forward=max(releases or handoffs)
        forward_boundary_activity=union_length([(max(numerical_forward,a),min(backward,b))
            for a,b in active_intervals if min(backward,b)>max(numerical_forward,a)])
        phase['forward_boundary_empty']=backward-numerical_forward-forward_boundary_activity
        phase['forward_middle_empty']=phase['forward']['empty']-phase['startup']-phase['forward_boundary_empty']
        assert phase['forward_middle_empty']>=0
        feedback=[int(r['finish']) for r in fields(text,'CFD_LINE_FEEDBACK') if r['token']==token]
        bounds=[backward,*feedback];back_steps=[]
        for lo,hi in zip(bounds,bounds[1:]):
            ops=[r for r in selected if lo<=int(r['cycle'])<hi]
            first=min(int(r['cycle']) for r in ops)
            reads=[r for r in jobs if lo<=int(r['cycle'])<first and r['op']=='R']
            back_steps.append(dict(start=lo,finish=hi,first_arithmetic=first,bubble=first-lo,
                reads=[dict(id=r['id'],start=int(r['cycle']),done=onchip_done[r['id']],stride=int(r['stride'])) for r in reads]))
        storage=measured['storage'][index]
        assert phase['backward']['empty']==sum(r['bubble'] for r in back_steps)
        feedback_latency=int(cfg['--coeff-cluster-mul-lat'])+6*int(cfg['--coeff-cluster-add-lat'])
        assert all(r['finish']-r['first_arithmetic']==feedback_latency for r in back_steps)
        windows.append(dict(token=token,cache_hit=int(s['cache_hit']),start=start,end=end,cycles=duration,
            phase=phase,arithmetic_empty_pct=100*(duration-busy)/duration,pools=pools,
            owners=owners,residency=residency,spm=bank,traffic_bytes=dict(traffic),packets=dict(packets),
            visible_packet_latency={k:distribution(v) for k,v in latencies.items()},
            descriptor_wait=desc_end-start,after_descriptor_to_first_op=first_op-desc_end,
            last_forward_value=numerical_forward,forward_value_to_backward=backward-numerical_forward,
            finish_to_output_response=int(s['output_last_response'])-done,
            output_response_to_status_issue=status_start-int(s['output_last_response']),
            status_response_latency=int(s['visible_done'])-status_start,
            backward_steps=back_steps,feedback_gap_histogram=dict(Counter(b-a for a,b in zip(bounds,bounds[1:]))),
            tile_dma_wait=int(storage['data_wait']),storage=storage,schedule=s,
            lifetime=next(r for r in fields(text,'CFD_LINE_LIFETIME') if r['token']==token)))
    stats=parse_stats(path/'stats.txt');dma={k.rsplit('.',1)[1]:v for k,v in stats.items() if 'local_spm.coeffDma' in k}
    ini=configparser.ConfigParser(interpolation=None);ini.read(path/'config.ini')
    bridge=dict(ini['system.coeff_dma_cache'])
    bridge={k:bridge[k] for k in ('size','assoc','tag_latency','data_latency','response_latency','mshrs','tgts_per_mshr')}
    bridge['clock_ticks']=int(ini['system.clk_domain']['clock'])
    bridge['stats']={k:v for k,v in stats.items() if k.startswith('system.coeff_dma_cache.') and
        (k.endswith('::total') and any(x in k for x in ('demandHits','demandMisses','overallAccesses','overallMissRate','overallAvgMissLatency')) or
         'blockedCycles::' in k or 'blockedCauses::' in k)}
    assert sum(sum(w['packets'].values()) for w in windows)==dma['coeffDmaIssued']==dma['coeffDmaCompleted']
    assert sum(sum(w['traffic_bytes'].values()) for w in windows)==dma['coeffDmaReadBytes']+dma['coeffDmaWriteBytes']
    return dict(campaign=campaign,case=case,binary=measured['binary'],config=cfg,
        total_roi_cycles=measured['cycles'],controller_cycles=sum(w['cycles'] for w in windows),
        guest_outside_controller=measured['cycles']-sum(w['cycles'] for w in windows),windows=windows,
        dma_stats=dma,dma_bridge=bridge,completion_queue=fields(text,'CFD_COMPLETION_QUEUE')[-1],
        rhs_rows=fields(text,'CFD_RHS_ROWS')[-1],cluster_buffer=fields(text,'CFD_CLUSTER_BUFFER')[-1],
        onchip_summary=fields(text,'CFD_ONCHIP_SUMMARY')[-1],
        issue_diag=fields(text,'CFD_ISSUE_DIAG')[-3:],scheduler_diag=fields(text,'CFD_SCHED_DIAG')[-10:])


def main():
    selection='single-line-round2-selection-20261012'
    sources=[(selection,c) for c in ('cold-direct-b4-t8-q64','cold-inverse-b8-t8-q64',
                                   'reuse-direct-b8-t8-q64','reuse-inverse-b8-t8-q64')]
    long_root=CAMPAIGNS/'single-line-audit-long-20261015'
    if (long_root/'optimization.json').exists():
        sources.extend(('single-line-audit-long-20261015',c) for c in json.loads((long_root/'optimization.json').read_text()))
    data={case:analyze(campaign,case) for campaign,case in sources}
    (OUT/'details.json').write_text(json.dumps(data,indent=2)+'\n')
    sensitivity_root=CAMPAIGNS/'single-line-audit-throughput-20261015'
    if (sensitivity_root/'optimization.json').exists():
        sensitivity={case:analyze(sensitivity_root.name,case)
                     for case in json.loads((sensitivity_root/'optimization.json').read_text())}
        for name,r in sensitivity.items():
            baseline=data[name.removesuffix('-ii2').removesuffix('-add2')]
            original=json.loads((CAMPAIGNS/baseline['campaign']/'optimization.json').read_text())[baseline['case']]
            changed=json.loads((sensitivity_root/'optimization.json').read_text())[name]
            assert original['hashes']==changed['hashes']
            assert r['binary']==baseline['binary']
            differences={k:(baseline['config'].get(k),r['config'].get(k)) for k in baseline['config'].keys()|r['config'].keys()
                         if baseline['config'].get(k)!=r['config'].get(k)}
            assert set(differences)==({'--coeff-cluster-div-ii'} if name.endswith('-ii2') else {'--coeff-cluster-add-lat'})
            r['config_difference']=differences
            r['reduction_pct']=100*(1-r['total_roi_cycles']/baseline['total_roi_cycles'])
        (OUT/'sensitivity.json').write_text(json.dumps(sensitivity,indent=2)+'\n')
    for name,r in data.items():
        w=r['windows'][-1]
        print(name,'ROI',r['total_roi_cycles'],'window',w['cycles'],'phases',w['phase'],
              'last-y-to-back',w['forward_value_to_backward'],'back bubbles',w['feedback_gap_histogram'])
        print('  utilization', {p:(round(v['issue_pct'],2),round(v['active_pct'],2),round(v['mean_width_when_issued'],2)) for p,v in w['pools'].items()})
        print('  contexts',w['owners'],'SPM', {p:round(w['spm'][p]['util_pct'],2) for p in ['R','W']},'traffic',w['traffic_bytes'])
    print('AUDIT_ANALYSIS_PASS',len(data),'cases',sum(len(r['windows']) for r in data.values()),'sweeps')


if __name__=='__main__':main()
