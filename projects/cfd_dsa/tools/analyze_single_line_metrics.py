#!/usr/bin/env python3
"""Recompute round3 results from accepted, same-binary campaigns and raw traces."""
import csv
import json
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO=Path(__file__).resolve().parents[3]
ROOT=REPO/'results/cfd_dsa/campaigns'
sys.path.insert(0,str(REPO/'projects/cfd_dsa/tools'))
from check_single_line_c import fields
from check_single_line_opt import union_length
from single_line_metrics import bank_utilization, phase_breakdown


def distribution(values):
    if not values:return {'count':0}
    return dict(count=len(values),minimum=min(values),mean=statistics.mean(values),maximum=max(values))


def load(campaign):
    return json.loads((ROOT/campaign/'optimization.json').read_text())


def metrics(campaign,name,row):
    path=ROOT/campaign/name;text=(path/'simout.txt').read_text();cfg=row['config']
    schedules=fields(text,'CFD_LINE_SCHEDULE');interfaces=fields(text,'CFD_INTERFACE_DMA')
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as f:ops=list(csv.DictReader(f))
    ready=fields(text,'CFD_LOCAL_READY');phases=[]
    for w,s in zip(row['windows'],schedules):
        token=s['token'];end=int(s['visible_done'])+1;start=end-w['cycles']
        back=int(s['backward_start']);done=int(s['backward_done'])
        selected=[r for r in ops if start<=int(r['cycle'])<end]
        intervals=[(int(r['cycle']),int(r['complete_cycle'])) for r in selected]
        def active(lo,hi):return union_length([(max(lo,a),min(hi,b)) for a,b in intervals if min(hi,b)>max(lo,a)])
        status=min(int(r['cycle']) for r in interfaces if r['token']==token and r['kind']=='status' and r['event']=='issue')
        local=[int(r['cycle']) for r in ready if r['token']==token]
        storage=next(r for r in row['storage'] if r['token']==token)
        summary=[r for r in interfaces if r['token']==token and r['kind']=='line-summary']
        summary_issue=min((int(r['cycle']) for r in summary if r['event']=='issue'),default=None)
        summary_response=max((int(r['cycle']) for r in summary if r['event']=='response'),default=None)
        phases.append(dict(window=w,backward=phase_breakdown(back,done,intervals,int(storage['data_wait']),int(s['output_queue_stalls'])),
            startup=min((a for a,b in intervals),default=start)-start,
            numeric_ready_to_backward=back-max(local) if local else None,
            completion_tail=end-done,status_response=end-1-status,
            summary_issue_cycle=summary_issue,summary_response_cycle=summary_response,
            summary_hidden_by_backward=summary_response<=done if summary_response is not None else None))
    samples=sum(w['cycles'] for w in row['windows'])
    issued={p:sum(w['issued'].get(p,0) for w in row['windows']) for p in ('mul','add','div')}
    dma=fields(text,'CFD_DMA_PACKET');packets={};send={};response={}
    clock=json.loads((path/'config.json').read_text())['system']['clk_domain']['clock'][0]
    for r in dma:
        if r['event']=='queue':packets[r['packet']]=r
        elif r['event']=='send':send[r['packet']]=int(r['tick'])
        elif r['event']=='response':response[r['packet']]=int(r['tick'])
    dma_summary={}
    if packets:
        dma_summary=dict(packets=len(packets),bytes=sum(int(r['bytes']) for r in packets.values()),
            bytes_per_packet=sum(int(r['bytes']) for r in packets.values())/len(packets),
            queue_cycles=distribution([(send[k]-int(r['tick']))/clock for k,r in packets.items()]),
            response_cycles=distribution([(response[k]-send[k])/clock for k in packets]))
    banks=fields(text,'CFD_ONCHIP_BANK');onchip=fields(text,'CFD_ONCHIP_SUMMARY')[-1]
    bank=bank_utilization(banks,samples,int(onchip['banks']),int(onchip['read_ports']),int(onchip['write_ports']))
    return dict(roi_cycles=row['cycles'],controller_cycles=samples,
        issue_pct={p:100*issued[p]*(int(cfg['--coeff-cluster-div-ii']) if p=='div' else 1)/(samples*(1 if p=='div' else 5)) for p in issued},
        empty_pct=100*sum(w['empty_cycles'] for w in row['windows'])/samples,
        peak_bytes=max(int(s['reserved'])+int(s['resident'])+int(s['cache_bytes']) for s in row['storage']),
        reload_bytes=sum(int(s['read_completed']) for s in row['storage']),max_abs=row['max_abs'],
        bank_read_pct=bank['read_pct'],
        bank_write_pct=bank['write_pct'],
        onchip=onchip,rhs=fields(text,'CFD_RHS_ROWS')[-1],completion=fields(text,'CFD_COMPLETION_QUEUE')[-1],
        phases=phases,dma=dma_summary)


def main():
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaigns",nargs="+")
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    campaigns=args.campaigns
    summaries={};binaries=set();hash_groups=defaultdict(list)
    for c in campaigns:
        data=load(c);binaries.update(r['binary'] for r in data.values())
        summaries[c]={n:metrics(c,n,r) for n,r in data.items() if r['config'].get('--coeff-cache-contract-test')!='1'}
        for n,r in data.items():
            cfg=r['config']
            key=tuple(cfg.get(k) for k in ('--coeff-base-solve','--lusgs-cells','--bench-iters','--coefficient-update-interval','--coeff-line-mvm-model','--coeff-input-pattern','--coeff-input-seed'))
            hash_groups[key].append((c,n,r['hashes']))

    for rows in hash_groups.values():
        assert all(r[2]==rows[0][2] for r in rows), ('FP64 changed across resource/timing variants',[(r[0],r[1]) for r in rows])
    assert len(binaries)==1,'mixed final gem5 binaries'
    args.output.write_text(json.dumps(dict(schema_version=2,binary=next(iter(binaries)),campaigns=summaries),indent=2)+'\n')
    print('SINGLE_LINE_METRICS_PASS',sum(len(r) for r in summaries.values()),'cases')


if __name__=='__main__':main()
