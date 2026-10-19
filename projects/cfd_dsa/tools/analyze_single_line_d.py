#!/usr/bin/env python3
"""Compare verified one-line storage/overlap runs and select measured modes."""
import argparse
import csv
import json
from pathlib import Path

from check_single_line_c import fields


def compare_modes(rows):
    result=[]
    for name,direct in rows.items():
        if not name.endswith('-direct'): continue
        peer=name.removesuffix('-direct')+'-inverse'
        if peer not in rows: continue
        inverse=rows[peer]
        dc,ic=dict(direct['config']),dict(inverse['config'])
        dc.pop('--coeff-base-solve');ic.pop('--coeff-base-solve')
        # A larger guard timeout does not change an otherwise completed run.
        dc.pop('--max-ticks',None);ic.pop('--max-ticks',None)
        if dc!=ic or direct['binary']!=inverse['binary']:
            raise ValueError('mode comparison changed resources or binary')
        n=int(dc['--lusgs-cells']);t=int(dc['--coeff-line-tile-cells'])
        k=int(dc['--coefficient-update-interval'])
        d,i=direct['cycles'],inverse['cycles']
        result.append(dict(profile=name.removesuffix('-direct'),cells=n,tile=t,
            update_interval=k,window=int(dc['--coeff-stream-window']),
            spm_bytes=int(dc['--coeff-line-spm-bytes']),
            outstanding=int(dc['--coeff-spm-outstanding']),
            inverse_cycles=i,direct_cycles=d,direct_change_pct=100*(d/i-1),
            selected='direct' if d<i else 'inverse',
            rebuilds=direct['rebuilds'],reuses=direct['reuses'],
            max_abs=max(direct['max_abs'],inverse['max_abs'])))
    return result


def intersect_size(a,b):
    # All sets are bounded by simulated duration, used only in offline analysis.
    return len(a & b)


def overlap(root,rows):
    results={}
    for mode in ('inverse','direct'):
        names=[f'overlap-{mode}-b{b}-w{w}' for b,w in ((1,40),(2,40),(1,64),(2,64))]
        baseline=rows[names[0]]
        for name in names:
            row=rows[name]
            if row['hashes']!=baseline['hashes']:
                raise ValueError('prefetch/coalescing changed ordered FP64 output')
            path=root/name;text=(path/'simout.txt').read_text()
            storage=row['storage'][0]
            line=fields(text,'CFD_LINE_SCHEDULE')[0]
            bs,be=int(line['backward_start']),int(line['backward_done'])
            tile_dma=fields(text,'CFD_TILE_DMA')
            starts={r['packet']:int(r['cycle']) for r in tile_dma if r['event']=='issue'}
            dma_active=set()
            for r in tile_dma:
                if r['event']=='response':dma_active.update(range(max(bs,starts[r['packet']]),min(be,int(r['cycle']))))
            compute=set()
            with (path/'coeff_preprocess_trace.csv.cluster.csv').open(newline='') as src:
                for r in csv.DictReader(src):
                    if int(r['caller']) in (7,8,9):
                        compute.update(range(max(bs,int(r['cycle'])),min(be,int(r['complete_cycle']))))
            events=fields(text,'CFD_TILE_EVENT')
            first_commit=int(next(e for e in events if e['event']=='forward_commit')['cycle'])
            t=int(row['config']['--coeff-line-tile-cells'])
            with (path/'coeff_preprocess_trace.csv').open(newline='') as src:
                next_inputs=[int(r['cycle']) for r in csv.DictReader(src)
                    if r['stage']=='INPUT_START' and int(r['cell'])>=t]
            forward_prefetch=bool(next_inputs and min(next_inputs)<first_commit)
            reverse_overlap=intersect_size(dma_active,compute)
            if row['config'].get('--coeff-line-tile-buffers','1')=='2' and \
                    (not forward_prefetch or not reverse_overlap):
                raise ValueError('double buffers did not overlap input/reverse DMA and compute')
            results[name]=dict(cycles=row['cycles'],change_pct=100*(row['cycles']/baseline['cycles']-1),
                reload_packets=int(storage['packets_issued']),data_wait=int(storage['data_wait']),
                tile_stall_events=int(storage['stalls']),
                output_backpressure=int(line['output_queue_stalls']),
                forward_prefetch=forward_prefetch,reverse_overlap_cycles=reverse_overlap,
                peak_bytes=int(storage['reserved'])+int(storage['resident'])+int(storage['cache_bytes']))
    return results


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode-report',type=Path,required=True)
    parser.add_argument('--optimization-report',type=Path)
    parser.add_argument('--tuned-mode-report',type=Path)
    parser.add_argument('--tuning-report',type=Path)
    parser.add_argument('--optimization-root',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--cells',type=int);parser.add_argument('--tile',type=int)
    parser.add_argument('--update-interval',type=int);parser.add_argument('--window',type=int,default=8)
    parser.add_argument('--outstanding',type=int,default=4)
    args=parser.parse_args()
    modes=compare_modes(json.loads(args.mode_report.read_text()))
    if args.tuned_mode_report:
        modes+=compare_modes(json.loads(args.tuned_mode_report.read_text()))
    result={'scope':'measured single-line event cycles; fixed shared 5/5/1 cluster; no extrapolation',
            'mode_comparisons':modes}
    if args.optimization_report:
        result['overlap']=overlap(args.optimization_root,json.loads(args.optimization_report.read_text()))
    if args.tuning_report:
        tuning=json.loads(args.tuning_report.read_text())
        baseline=json.loads(args.optimization_report.read_text())
        comparisons={}
        for name,row in tuning.items():
            mode=row['config']['--coeff-base-solve']
            base=baseline['overlap-'+mode+'-b2-w64']
            if row['hashes']!=base['hashes']:
                raise ValueError('DMA/context/tile tuning changed ordered output')
            memory=row['storage'][0]
            comparisons[name]=dict(cycles=row['cycles'],
                change_pct=100*(row['cycles']/base['cycles']-1),
                outstanding=int(row['config']['--coeff-spm-outstanding']),
                tile=int(row['config']['--coeff-line-tile-cells']),
                contexts=int(row['config']['--lu5-count']),
                data_wait=int(memory['data_wait']),
                peak_bytes=int(memory['reserved'])+int(memory['resident'])+int(memory['cache_bytes']))
        result['cold_tuning']=comparisons
    if args.cells is not None:
        matched=[r for r in modes if (r['cells'],r['tile'],r['update_interval'],r['window'],r['outstanding'])==
                 (args.cells,args.tile,args.update_interval,args.window,args.outstanding)]
        if len(matched)!=1:parser.error('supply an exact measured cells/tile/update-interval/window/outstanding tuple')
        result['recommendation']=matched[0]
    if args.output:args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
