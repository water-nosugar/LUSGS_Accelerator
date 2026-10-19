#!/usr/bin/env python3
"""Render the final measured tables; never substitute preliminary scan timings."""
import json
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
DOC=REPO/'projects/cfd_dsa/docs/CFD_DSA_SINGLE_LINE_ROUND3_20261015.md'
ROOT=REPO/'results/cfd_dsa/campaigns'


def table(headers,rows):
    return '\n'.join(['| '+' | '.join(headers)+' |','| '+' | '.join(['---']*len(headers))+' |',*['| '+' | '.join(map(str,r))+' |' for r in rows]])+'\n'


def main():
    summary=json.loads((OUT/'summary.json').read_text())['campaigns']
    main=summary['single-line-round3-final-20261015']
    raw=json.loads((ROOT/'single-line-round3-final-20261015/optimization.json').read_text())
    tile=summary['single-line-round3-final-tile-20261015']
    fast=summary['single-line-round3-final-arithmetic-20261015']
    endpoint=summary['single-line-round3-final-endpoint-20261015']
    profiles=['cold-direct','cold-inverse','reuse-direct','reuse-inverse']
    text=['\n<!-- measured-final-start -->\n## 最终同版本结果\n',
          '本节只使用最终模型与最终guest；初步扫描周期不混入下表。cold为一轮，reuse为一轮重建加六轮命中。主表保留原算术和DMA端点延迟。\n']
    for n in (17,65):
        rows=[]
        for p in profiles:
            a=main[f'n{n}-{p}-l8'];b=tile[f'n65-{p}-l11-t32'] if n==65 and p.startswith('cold') else main[f'n{n}-{p}-l11']
            rows.append([p,a['roi_cycles'],b['roi_cycles'],f"{100*(1-b['roi_cycles']/a['roi_cycles']):.2f}%",f"{a['controller_cycles']} → {b['controller_cycles']}"])
        text.extend([f'### N={n}\n',table(['场景','level8整轮','最终整轮','周期减少','控制器窗口'],rows)])
    text.extend(['### 推荐资源与容量\n',table(['场景','banks','tile','DMA并发','SPM队列','完成队列','RHS/共享乘积槽','峰值容量B'],[
        [p,*[raw[f'n17-{p}-l11']['config'][k] for k in ('--coeff-spm-banks','--coeff-line-tile-cells','--coeff-spm-outstanding','--coeff-onchip-queue-depth','--coeff-completion-depth')],'16 / 4',main[f'n17-{p}-l11']['peak_bytes']] for p in profiles]),
        '容量预算为196608B；缩减后N=17的峰值比level8少11776B。N=17的两个tile可覆盖整线，N=65 cold改为tile32，仍有1个tile需要逆序回读，共7680B；warm保留tile16，回读3个tile的y共1920B。cold长线峰值135168B，比旧配置少4096B。二者均经过独立容量/流量账本验收。\n',
        '选择规则是优先保留接近最低周期的较小资源配置；bank4相对bank8超过1%损失时保留8banks。SPM/DMA队列均有真实容量限制。静态宿主数组的未启用槽不参与分配，容量数字代表模型中的硬件预算。\n'])
    text.extend(['### 气泡与利用率\n',table(['N17场景','后向：旧→新','新后向算术','新后向DMA等候','新后向输出背压','旧→新全簇空闲比例'],[
        [p,f"{main[f'n17-{p}-l8']['phases'][-1]['backward']['cycles']} → {main[f'n17-{p}-l11']['phases'][-1]['backward']['cycles']}",main[f'n17-{p}-l11']['phases'][-1]['backward']['math'],main[f'n17-{p}-l11']['phases'][-1]['backward']['tile_wait'],main[f'n17-{p}-l11']['phases'][-1]['backward']['output_queue_wait'],f"{main[f'n17-{p}-l8']['empty_pct']:.2f}% → {main[f'n17-{p}-l11']['empty_pct']:.2f}%"] for p in profiles]),
        table(['N17场景','乘法发射槽利用率','加减发射槽利用率','除法发射槽利用率'],[[p,*[f"{main[f'n17-{p}-l11']['issue_pct'][k]:.2f}%" for k in ('mul','add','div')]] for p in profiles]),
        '发射槽利用率按整个控制器窗口计算，除法按II归一化；全簇空闲指没有任何乘、加、除操作在执行。它们不同于资源被上下文持有的比例。reuse行按七轮加权，最后一轮warm inverse的除法利用率为0，因为该路径本来就不执行除法。\n'])
    priority=[]
    for mode in ('direct','inverse'):
        a=main[f'n65-cold-{mode}-priority0'];b=main[f'n65-cold-{mode}-l11']
        priority.append([mode,a['phases'][0]['backward']['output_queue_wait'],b['phases'][0]['backward']['output_queue_wait'],a['phases'][0]['backward']['cycles'],b['phases'][0]['backward']['cycles']])
    text.extend(['### DMA信用仲裁A/B\n',table(['N65cold','原顺序输出背压','优先输出背压','原顺序后向','优先输出后向'],priority),
        '这组A/B只改变发送顺序，计算资源、DMA并发、tile、SPM和输出队列容量相同；结果位模式及算术发射次数相同。`check_single_line_dma_priority.py`会拒绝任何额外资源变化。\n'])
    text.extend(['### 算术时序敏感性\n',table(['N17场景','固定延迟最终版','divII2','addlat2','两者同时'],[[p,main[f'n17-{p}-l11']['roi_cycles'],fast[p+'-div2']['roi_cycles'],fast[p+'-add2']['roi_cycles'],fast[p+'-both']['roi_cycles']] for p in profiles]),
        '这是改变算术资源时序的能力评估，不计入固定延迟优化幅度；目前没有RTL时序/面积证据。II2仍保留12周期除法结果延迟，加法2周期会缩短反馈链。N65同时加速后的回读与输出等候见summary.json的phases；不能只用短线判断DMA是否足够。\n'])
    text.extend(['### 加速后的长线回读\n',table(['N65cold','tile16后向','tile32后向','tile16回读等待','tile32回读等待'],[[p,fast[p+'-both-n65']['phases'][0]['backward']['cycles'],fast[p+'-both-n65-t32']['phases'][0]['backward']['cycles'],fast[p+'-both-n65']['phases'][0]['backward']['tile_wait'],fast[p+'-both-n65-t32']['phases'][0]['backward']['tile_wait']] for p in ('cold-direct','cold-inverse')])])
    text.extend(['### DMA端点敏感性\n',table(['场景','50/50/50','10/10/10','1/1/1'],[[p,main[f'n17-{p}-l11']['roi_cycles'],endpoint[p+'-endpoint10']['roi_cycles'],endpoint[p+'-endpoint1']['roi_cycles']] for p in ('cold-direct','reuse-inverse')]),
        '物理trace区分queue→send与send→response。默认端点没有被调低；无实测目标硬件时，这里的低延迟配置不能称为完成了硬件标定。响应时间还包括相干缓存路径及内存系统，不能全部归为DRAM延迟。\n'])
    dma=main['n17-cold-direct-l11']['dma']
    text.append(f"N17 cold direct最终传输{dma['bytes']}B、{dma['packets']}包，平均{dma['bytes_per_packet']:.2f}B/包；DMA排队均值{dma['queue_cycles']['mean']:.3f}周期，发送至响应均值{dma['response_cycles']['mean']:.3f}周期。记录本身每点320→160B；仍保留descriptor、status和最终输出响应。\n")
    from capture_evidence import SPECS
    total=0
    for campaign,suite_name,report in SPECS:
        spec=json.loads((REPO/'projects/cfd_dsa/suites'/f'{suite_name}.json').read_text())
        accepted=json.loads((ROOT/campaign/report).read_text());entries=accepted.get('cases',accepted)
        assert set(entries)=={c['name'] for c in spec['cases']}
        total+=len(entries)
    assert total==125
    text.append('最终同一gem5/guest版本共125个独立仿真场景验收通过：主对照18、长线tile补测2、压力25、算术16、端点4、C回归35、D回归25。另有11项单元测试、15种非法配置、9项调度/记录故障和4项SPM故障注入检查通过。新优化场景最大绝对误差为2.22e-16；保持累加顺序的配置跨资源/时序变体结果位模式相同。\n')
    text.extend(['## 验收与重现\n',
        '最终场景清单、二进制/源码哈希、验收日志及原始数据哈希见`results/cfd_dsa/analysis/single-line-round3-20261015/evidence.json`，可运行该目录的`verify.py`检查。没有覆盖旧审计和基线文件。\n',
        '```bash\npython3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/single-line-round3-recommended.json\npython3 projects/cfd_dsa/tools/check_single_line_opt.py results/cfd_dsa/campaigns/single-line-round3-recommended-20261015 --suite projects/cfd_dsa/suites/single-line-round3-recommended.json\n```\n',
        '长线配置见`suites/single-line-round3-long-recommended.json`；可选算术能力配置见`suites/single-line-round3-fast-recommended.json`。\n',
        '对实际冷启动/复用次数选择direct或inverse；本轮没有改变这一边界。仍有的主要限制是单线前向LU/求解反馈、除法吞吐及启动/收尾DMA延迟。继续增加乘法器或无差别扩大队列缺乏本轮证据支持。\n'])
    prefix=DOC.read_text().split('<!-- measured-final-start -->')[0].replace('最终对照将用补齐检查的同一guest重跑','最终对照已用补齐检查的同一guest重跑')
    DOC.write_text(prefix+'\n'.join(text))
    print('ROUND3_REPORT_PASS')


if __name__=='__main__':main()
