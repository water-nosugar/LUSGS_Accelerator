#!/usr/bin/env python3
"""Generate the optimization tables from accepted, same-binary experiments."""
import json
from pathlib import Path
import sys
from html import escape

OUT = Path(__file__).resolve().parent
REPO = OUT.parents[3]
ROOT = REPO/'results/cfd_dsa/campaigns'
sys.path.insert(0, str(REPO/'projects/cfd_dsa/tools'))
from check_single_line_c import fields


def load(name):
    return json.loads((ROOT/f'single-line-opt-{name}-20261012'/'optimization.json').read_text())


def metrics(row):
    windows = row['windows']
    cycles = sum(w['cycles'] for w in windows)
    issued = {p: sum(w['issued'].get(p, 0) for w in windows) for p in ('mul','add','div')}
    return dict(whole_cycles=row['cycles'], controller_cycles=cycles,
        issue_utilization_pct={p: 100*v/(cycles*(.25 if p=='div' else 5)) for p,v in issued.items()},
        empty_pct=100*sum(w['empty_cycles'] for w in windows)/cycles,
        peak_bytes=max(int(s['reserved'])+int(s['resident'])+int(s['cache_bytes']) for s in row['storage']),
        max_abs=row['max_abs'], hashes=row['hashes'])


def table(headers, rows):
    return '\n'.join(['| '+' | '.join(headers)+' |', '| '+' | '.join(['---']*len(headers))+' |',
                      *['| '+' | '.join(map(str,r))+' |' for r in rows]])+'\n'


def figure(comparisons):
    svg=['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="660" viewBox="0 0 1100 660">',
         '<rect width="1100" height="660" fill="#f8fafc"/>',
         '<style>text{font-family:DejaVu Sans,sans-serif;fill:#0f172a} .small{font-size:13px} .title{font-size:22px;font-weight:600}</style>',
         '<text x="36" y="38" class="title">Single-line LU-SGS: same 5 mul / 5 add / 1 div, divider II=4</text>',
         '<text x="36" y="66" class="small">N=17; cold=1 sweep; reuse=1 rebuild + 6 cache hits; timing DMA; bounded buffers</text>']
    colors=['#64748b','#0f766e']
    for i,(profile,r) in enumerate(comparisons.items()):
        x=36+(i%2)*535; y=112+(i//2)*252
        svg.append(f'<text x="{x}" y="{y}" font-size="18" font-weight="600">{escape(profile)}: {r["cycle_reduction_pct"]:.1f}% fewer whole-sweep cycles</text>')
        maximum=r['baseline']['whole_cycles']
        for j,(name,value) in enumerate([('Baseline',maximum),('Optimized',r['optimized']['whole_cycles'])]):
            by=y+27+j*45; width=350*value/maximum
            svg.extend([f'<text x="{x}" y="{by+19}" class="small">{name}</text>',
                        f'<rect x="{x+86}" y="{by}" width="{width:.1f}" height="27" rx="4" fill="{colors[j]}"/>',
                        f'<text x="{x+92+width:.1f}" y="{by+19}" class="small">{value:,}</text>'])
        b,o=r['baseline'],r['optimized']
        svg.append(f'<text x="{x}" y="{y+141}" class="small">Arithmetic empty: {b["empty_pct"]:.1f}% → {o["empty_pct"]:.1f}%</text>')
        svg.append(f'<text x="{x}" y="{y+164}" class="small">Issue utilization mul/add/div: {o["issue_utilization_pct"]["mul"]:.1f}% / {o["issue_utilization_pct"]["add"]:.1f}% / {o["issue_utilization_pct"]["div"]:.1f}%</text>')
    svg.append('<text x="36" y="635" class="small">Utilization uses descriptor-to-visible-status windows. Extra RHS buffer, when enabled: 4 KiB, capacity charged.</text></svg>')
    (OUT/'comparison.svg').write_text('\n'.join(svg))


def main():
    stages, tuning, scheduler, divider, selection = [load(n) for n in
        ('final-stages','tuning','scheduler','divider','selection')]
    assert len({r['binary'] for d in (stages,tuning,scheduler,divider,selection) for r in d.values()})==1
    profiles=['cold-direct','cold-inverse','reuse-direct','reuse-inverse']
    selected={p:min((n for n in selection if n.startswith(p+'-')),key=lambda n:selection[n]['cycles']) for p in profiles}
    comparisons={}
    for profile,name in selected.items():
        b,o=stages['l0-'+profile],selection[name]
        assert b['hashes']==o['hashes'], 'selected settings changed FP64 bits'
        comparisons[profile]=dict(case=name,baseline=metrics(b),optimized=metrics(o),
            cycle_reduction_pct=100*(1-o['cycles']/b['cycles']),speedup=b['cycles']/o['cycles'],
            config=o['config'])
    summary=dict(comparisons=comparisons,stages={n:metrics(r) for n,r in stages.items()},
        tuning_cycles={n:r['cycles'] for n,r in tuning.items()},
        scheduler_cycles={n:r['cycles'] for n,r in scheduler.items()},
        divider_cycles={n:r['cycles'] for n,r in divider.items()})
    (OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    source=json.loads((REPO/'projects/cfd_dsa/suites/single-line-opt-selection.json').read_text())
    recommended=dict(campaign='single-line-opt-recommended',common_args=source['common_args'],
        cases=[c for c in source['cases'] if c['name'] in selected.values()])
    (REPO/'projects/cfd_dsa/suites/single-line-opt-recommended.json').write_text(json.dumps(recommended,indent=2)+'\n')
    doc=['# 优化结果（同一最终二进制）\n',
         '17 点；cold=一轮，reuse=一次重建后六次复用。主表固定 5/5/1 算术单元与除法 II=4。整轮周期与控制器窗口的利用率分开统计。\n',
         table(['路径','基线周期','推荐周期','减少','加速比','算术全空闲','mul/add/div 发射利用率','容量峰值'],
            [[p,r['baseline']['whole_cycles'],r['optimized']['whole_cycles'],f'{r["cycle_reduction_pct"]:.2f}%',f'{r["speedup"]:.2f}×',
              f'{r["baseline"]["empty_pct"]:.2f}% → {r["optimized"]["empty_pct"]:.2f}%',
              '/'.join(f'{r["optimized"]["issue_utilization_pct"][q]:.2f}%' for q in ('mul','add','div')),
              f'{r["optimized"]["peak_bytes"]}B / 196608B'] for p,r in comparisons.items()]),
         '## 最终推荐参数\n',
         '共同参数：opt-level=4、split、rhs-frontier、LU=2、输入槽=2、window=8、双 tile 缓冲、tile DMA 宽度64B、DMA outstanding=16、输出队列深度8；mul/add/div 延迟3/4/12、divider II=4。\n',
         table(['路径','row-parallel','tile','solve 上下文','实测 case'],
               [[p,r['config']['--coeff-rhs-row-parallel'],r['config']['--coeff-line-tile-cells'],r['config']['--coeff3-count'],r['case']] for p,r in comparisons.items()]),
         '推荐 suite 直接使用这些已测参数。默认 opt-level=0，row-parallel=0 保留原路径。\n',
         '## 顺序消融\n',
         table(['阶段','cold direct','cold inverse','reuse direct','reuse inverse'],
               [[level,*[stages[level+'-'+p]['cycles'] for p in profiles]] for level in ['l0','l1','l2','l3','l4','l4rows']]),
         'l4rows 为 l4 + RHS 行并行（额外4KiB）。此表为2/2上下文、tile4、frontier-aware、输出队列4；推荐配置的变化另列，不把参数收益混入消融。\n',
         '## 除法 II 与 reciprocal（独立实验）\n',
         table(['路径','divide II=4','divide II=2','divide II=1','reciprocal II=4'],
               [[p,stages['l4rows-'+p]['cycles'],divider[p+'-ii2']['cycles'],divider[p+'-ii1']['cycles'],divider[p+'-reciprocal']['cycles']] for p in profiles]),
         'II=2/1 改变峰值硬件吞吐，并非固定计算能力主表。inverse cold 受益明显；direct 的收益小且有退化。没有综合面积/功耗/频率数据，不据此推荐更高吞吐 divider。\n',
         '## 局部证据\n']
    for n in ['l0-cold-direct','l4rows-cold-direct','l0-reuse-inverse','l4-reuse-inverse']:
        r=stages[n]
        text=(ROOT/'single-line-opt-final-stages-20261012'/n/'simout.txt').read_text()
        interface=fields(text,'CFD_INTERFACE_SUMMARY')[-1]
        doc.append(f'- {n}: 最后一轮控制器 {r["windows"][-1]["cycles"]} 周期；记录 {interface["record_bytes"]}B；列 handoff 间隔 {r["windows"][0]["handoff_gap_histogram"]}；warm 槽/重叠 {r["windows"][-1]["warm"]}。\n')
    doc.extend(['\n详见 [实现与模型边界](../../../../projects/cfd_dsa/docs/CFD_DSA_SINGLE_LINE_OPTIMIZATION_20261012.md)。\n',
                '\n![周期对照](comparison.png)\n'])
    (OUT/'results.md').write_text('\n'.join(doc))
    figure(comparisons)
    print('ANALYSIS_PASS', {p:r['optimized']['whole_cycles'] for p,r in comparisons.items()})


if __name__=='__main__':
    main()
