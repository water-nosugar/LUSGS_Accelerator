#!/usr/bin/env python3
"""Summarize final same-binary comparisons and reproduce selected settings."""
import json
import csv
import sys
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
ROOT=REPO/'results/cfd_dsa/campaigns'
sys.path.insert(0,str(REPO/'projects/cfd_dsa/tools'))
from check_single_line_c import fields
from check_single_line_opt import union_length


def last_phase(campaign,case,row):
    path=ROOT/campaign/case
    text=(path/'simout.txt').read_text()
    s=fields(text,'CFD_LINE_SCHEDULE')[-1]
    start,end=int(s['backward_start']),int(s['backward_done'])
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as src:
        operations=list(csv.DictReader(src))
    intervals=[]
    for r in operations:
        lo=max(start,int(r['cycle']));hi=min(end,int(r['complete_cycle']))
        if hi>lo:intervals.append((lo,hi))
    active=union_length(intervals)
    wait=int(row['storage'][-1]['data_wait'])
    other=end-start-active-wait
    assert other>=0
    bank_summary=fields(text,'CFD_ONCHIP_SUMMARY')[-1]
    words=fields(text,'CFD_ONCHIP_BANK')
    samples=int(bank_summary['samples']);banks=int(bank_summary['banks'])
    return dict(last_window=row['windows'][-1],
        backward=dict(total=end-start,math_activity=active,tile_dma_wait=wait,other_idle=other),
        lifetime=fields(text,'CFD_LINE_LIFETIME')[-1],onchip=bank_summary,
        bank_read_util_pct=100*sum(int(r['read_words']) for r in words)/(samples*banks*int(bank_summary['read_ports'])),
        bank_write_util_pct=100*sum(int(r['write_words']) for r in words)/(samples*banks*int(bank_summary['write_ports'])))


def load(campaign):
    return json.loads((ROOT/campaign/'optimization.json').read_text())


def metrics(row):
    windows=row['windows'];cycles=sum(w['cycles'] for w in windows)
    util={p:sum(w['issued'].get(p,0) for w in windows)/(cycles*(1/int(row['config']['--coeff-cluster-div-ii']) if p=='div' else 5))*100 for p in ('mul','add','div')}
    return dict(cycles=row['cycles'],controller_cycles=cycles,issue_util_pct=util,
        empty_pct=sum(w['empty_cycles'] for w in windows)/cycles*100,
        peak_bytes=max(int(s['reserved'])+int(s['resident'])+int(s['cache_bytes']) for s in row['storage']),
        reload_bytes=sum(int(s['read_completed']) for s in row['storage']),error=row['max_abs'])


def table(headers,rows):
    return '\n'.join(['| '+' | '.join(headers)+' |','| '+' | '.join(['---']*len(headers))+' |',
                      *['| '+' | '.join(map(str,row))+' |' for row in rows]])+'\n'


def main():
    stages=load('single-line-round2-verified-stages-20261012')
    selection=load('single-line-round2-selection-20261012')
    arithmetic=load('single-line-round2-arithmetic-20261012')
    assert len({r['binary'] for d in (stages,selection,arithmetic) for r in d.values()})==1
    profiles=['cold-direct','cold-inverse','reuse-direct','reuse-inverse'];winners={};comparisons={}
    for p in profiles:
        names=[n for n in selection if n.startswith(p+'-b') and '-banked-' not in n]
        n=min(names,key=lambda n:selection[n]['cycles']);winners[p]=n
        baseline=selection[p+'-banked-l5'];best=selection[n]
        assert baseline['hashes']==best['hashes']
        stage8=next(r for n,r in stages.items() if n.startswith('l8-'+p+'-'))
        assert stage8['hashes']==best['hashes']
        comparisons[p]=dict(case=n,baseline=metrics(baseline),optimized=metrics(best),
            fixed_banks=metrics(stage8),reduction_pct=100*(1-best['cycles']/baseline['cycles']),config=best['config'],
            details=last_phase('single-line-round2-selection-20261012',n,best),
            baseline_details=last_phase('single-line-round2-selection-20261012',p+'-banked-l5',baseline),
            four_bank_cycles=selection[p+'-b4-t8-q64']['cycles'])
    suite=json.loads((REPO/'projects/cfd_dsa/suites/single-line-round2-selection.json').read_text())
    recommended=dict(campaign='single-line-round2-recommended',common_args=suite['common_args'],
        cases=[c for c in suite['cases'] if c['name'] in winners.values()])
    (REPO/'projects/cfd_dsa/suites/single-line-round2-recommended.json').write_text(json.dumps(recommended,indent=2)+'\n')
    data=dict(comparisons=comparisons,stages={n:metrics(r) for n,r in stages.items()},
              arithmetic={n:metrics(r) for n,r in arithmetic.items()},binary=next(iter(stages.values()))['binary'])
    (OUT/'summary.json').write_text(json.dumps(data,indent=2)+'\n')
    doc=['# 第二轮优化结果\n',
         'N=17；cold=一轮；reuse=一次重建和六次命中。下表基线与优化版均包含 SPM bank/端口/仲裁与真实 timing DMA，算术簇固定5/5/1、latency=3/4/12、divider II=4。\n',
         table(['场景','banked level5 基线','固定bank配置 level8','重新选参数后','较banked基线减少','峰值容量'],
               [[p,c['baseline']['cycles'],c['fixed_banks']['cycles'],c['optimized']['cycles'],f'{c["reduction_pct"]:.2f}%',f'{c["optimized"]["peak_bytes"]} / 196608 B'] for p,c in comparisons.items()]),
         '固定bank配置为4banks、1R/1W、粒度8B、DMA16、tile4；选参允许bank与DMA资源变化，不代表面积相同。\n',
         table(['场景','bank数','tile','DMA并发','SPM队列','实测case'],
               [[p,c['config']['--coeff-spm-banks'],c['config']['--coeff-line-tile-cells'],c['config']['--coeff-spm-outstanding'],c['config']['--coeff-onchip-queue-depth'],c['case']] for p,c in comparisons.items()]),
         '## 消融：原有寄存器操作数假设与新SPM模型分开统计\n',
         table(['级别','cold direct','cold inverse','reuse direct','reuse inverse'],
               [[level,*[next(r['cycles'] for n,r in stages.items() if n.startswith(f'l{level}-'+p+'-')) for p in profiles]] for level in [4,5,6,7,8]]),
         '4→7 保持原数据操作数假设，展示生命周期、tile留存和LU组执行的收益；8增加banked SPM访问约束，新增延迟与容量不得隐去。\n',
         '## 改变硬件吞吐/延迟的独立诊断\n',
         table(['case','整轮周期','最大绝对误差'],[[n,r['cycles'],r['max_abs']] for n,r in arithmetic.items()]),
         '改变divider II或加法延迟需要实际实现/综合证据。这些参数不属于固定延迟推荐配置的免费收益。\n']
    (OUT/'results.md').write_text('\n'.join(doc))
    print('ROUND2_ANALYSIS_PASS',winners)


if __name__=='__main__':main()
