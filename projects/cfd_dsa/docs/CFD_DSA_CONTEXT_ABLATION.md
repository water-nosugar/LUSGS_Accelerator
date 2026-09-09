# LU/TRSM上下文消融与资源统计口径

后续诊断轮见[调度拒绝与背压诊断](CFD_DSA_SCHEDULER_DIAGNOSTICS.md)，
该轮沿用本页固定资源配置，不改变仲裁策略。

本轮只做配置消融和观测，不改变调度顺序、算术资源、数值运算、ISA或descriptor ABI。
serial主路径、base前瞻、early backward保持不变；split和reciprocal不参与本轮。

## 实验设计

`projects/cfd_dsa/suites/coeff-context-ablation.json`固定全部资源参数，仅改变
`--lu5-count`和`--coeff3-count`，各取1、2、4，完整3×3矩阵。
两组工作负载均1×17、window8：cold为1sweep/interval1，mixed为3sweeps/interval2
（重建→命中→重建）。另有cold observer-off对照，只关闭新增观测开关。

当前event实现中的这两个count是活动request额度，算术FU另由
`lu5-div-count/lu5-mulsub-count/coeff3-div-count/coeff-mulsub-count`控制。
本轮这些FU count全部保持1，LU除法12/II12、TRSM除法12/II4、乘法3、减法4、
ColumnFma4/II1。pending depth、input slots、输出buffer、SPM及drain参数全部固定。
增加上下文仍需状态存储，不能称同面积硬件实验。

## 新统计接口

配置`--coeff-resource-stats=1`，通过`GEM5_CFD_RESOURCE_STATS`进入控制器，默认关闭。
核心实现：`src/arch/arm/cfd_coeff_preprocess_controller.cc:3955`的
`beginResourceStats`与同文件3979的`endResourceStats`。

每个控制器tick在完成旧事件后、发射新任务前采样可用slot，在发射后统计pending。
每个资源池每tick只采样一次，不随request/RHS数量重复计数。控制器静止时输出累计
`CFD_RESOURCE_STATS`快照；多sweep必须取每个pool的最后一条，不能累加快照。
分母是本进程的controller-active采样窗口，不是guest总周期，也不含控制器停机期间的
验证/打印时间。旧descriptor累计字段不改，兼容既有回归。

| 字段 | 精确定义 |
| --- | --- |
| samples | 采样tick数 |
| eligible_slots | tick开始时nextIssue<=now的FU槽数累计 |
| issued | 成功发射的操作数 |
| issue_cycles | 至少成功发射一次的tick数 |
| blocked_cycles | 没有成功发射，但至少调用一次issue申请的tick数 |
| no_attempt_cycles | 没有调用issue申请的tick数；不能推断全系统没有ready工作 |
| active_cycles | 发射后存在至少一个在途操作的tick数，最多等于samples |
| occupancy_sum/max | 每tick在途操作数的累计/峰值，允许大于物理FU数 |

三个发射分类互斥且覆盖samples：`issue_cycles+blocked_cycles+no_attempt_cycles=samples`。
某tick成功发射但其他申请被拒绝，归入issue_cycles，不重复算blocked_cycles。

推荐分别报告：

- 发射速率=`issued/samples`，单位operations/controller-cycle。
- 可用slot使用率=`issued/eligible_slots`，衡量被观测到的可发射机会使用情况；
  它不是以固定峰值带宽为分母的利用率，II造成的占用已排除在eligible之外。
- 流水活跃比例=`active_cycles/samples`，只回答流水是否非空。
- 平均在途占用=`occupancy_sum/samples`，不能叫利用率。

例如lat12/II4可同时有多个除法在途，occupancy_sum会叠加，active_cycles不会。
旧`eventCoeffDividerUtilization`使用叠加busy和idle构造的比值，作为legacy字段保留，
不再用它判断物理资源是否“90%饱和”。新接口是独立的校正口径，不篡改历史数据。

## 运行和核对

```bash
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 \
  lusgs-step2 trsv5 decode-exclusive
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/coeff-context-ablation.json \
  --campaign context-ablation-20260909
python3 projects/cfd_dsa/tools/check_context_ablation.py \
  results/cfd_dsa/campaigns/context-ablation-20260909
```

检查同一二进制/非上下文配置、full PASS、各sweep结果位模式摘要、资源参数和操作数，
以及采样守恒、issued<=eligible、active<=samples。无cancel且静止时还检查
`occupancy_sum=issued*latency`，防止在途周期漏计或重复计。

observer-off的结果必须一致；周期差原样报告，超过8周期则检查失败并要求调查，
不能隐藏采样或guest轮询对测量边界的影响。正式结论须基于本轮结果，不能提前承诺
上下文越多越快。全局ready调度、多base上下文、DMA协议修复均不属于本轮修改。

## 本轮实测结果（2026-09-09）

证据目录：`results/cfd_dsa/campaigns/context-ablation-20260909/`。
每个`cold-lX-tY`或`mixed-lX-tY`子目录保存对应manifest及simout。
以下取benchmark的`streamingStep2.totalCycles`，不是simTicks，也不是各重叠阶段周期之和。

| LU上下文 | TRSM上下文 | cold周期 | 相对1/1变化 | mixed周期 | 相对1/1变化 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 9478 | 0.00% | 18202 | 0.00% |
| 1 | 2 | 9262 | -2.28% | 17768 | -2.38% |
| 1 | 4 | 9262 | -2.28% | 17768 | -2.38% |
| 2 | 1 | 9616 | +1.46% | 18476 | +1.51% |
| 2 | 2 | 7918 | -16.46% | 15082 | -17.14% |
| 2 | 4 | 7864 | -17.03% | 14974 | -17.73% |
| 4 | 1 | 9838 | +3.80% | 18920 | +3.94% |
| 4 | 2 | 7960 | -16.02% | 15166 | -16.68% |
| 4 | 4 | 7980 | -15.81% | 15208 | -16.45% |

负数表示周期减少。2/4的cold与mixed加速比分别约1.205×和1.216×。
2/2相对2/4仅多54/108周期；2/4进一步减少0.68%/0.72%。
建议后续以2/2作为较少上下文的实验基线、2/4作为性能参照，保留1/1控制组。
本轮不修改生产默认值；没有面积综合结果，不能声称2/2是面积最优。

### 增加上下文是否填补空泡？

以下是cold最后一条各pool累计快照；所有配置的资源数量、延迟、II与操作数通过自动核对。

| 指标 | 1/1 | 2/2 | 2/4 |
| --- | ---: | ---: | ---: |
| controller采样周期 | 7468 | 5907 | 5854 |
| coeff_mul发射操作数 | 4900 | 4900 | 4900 |
| coeff_mul无申请周期 | 2568 | 1007 | 954 |
| coeff_mul发射率（操作/周期） | 0.6561 | 0.8295 | 0.8370 |
| coeff_div流水活跃比例 | 72.35% | 87.66% | 85.94% |
| LU乘法最大在途操作数 | 1 | 2 | 2 |

结论：在本轮配置和规模下，联合增加上下文确实提高共享流水利用程度。
相同4900次系数乘法的无申请周期由2568降至954；LU乘法仍只有一个物理流水单元，
但最大在途操作从1增至2，显示不同上下文的操作能够交错进入同一流水。
这不是增加乘法器，也不是改变FP64运算顺序获得的加速。

不能把所有no_attempt周期都称为“全局没有ready任务”；它还包含当前调度器未申请的情况。
2/2除法流水活跃比例高于2/4，却不是最快，进一步说明单一活跃比例不等于端到端性能。
TRSM仅1上下文时增加LU上下文反而退化；精确归因到哪个仲裁/背压分支仍待确认，
不能仅凭本轮汇总统计指定根因。4/4也未胜出，因此不存在上下文单调增益结论。

### 校验结果与边界

- gem5 ARM构建、三个guest目标构建通过。
- 18个矩阵用例及1个observer-off用例全部通过full validation；
  同组每个sweep的结果摘要一致，非上下文配置、二进制摘要及各pool操作数一致。
- 检查器输出`CONTEXT_ABLATION_CHECK_PASS`，包括每周期分类守恒和在途周期守恒。
- observer-off为9480周期、on为9478周期，差-2周期；数值结果一致。
  差异的精确来源待确认；不宣称严格零测量扰动，也不据此解释千周期级收益。
- decode-exclusive、standalone TRSV5、legacy Step2三个smoke通过，证据目录
  `results/cfd_dsa/campaigns/context-stats-smoke-20260909/`。
- Python语法、JSON、文档路径和格式检查通过；未运行全部历史性能campaign。

适用范围仅本轮1×17、serial、divide、固定资源及cold/mixed工作负载。
未验证长网格线、多line、真实DMA和所有系数更新策略；不外推这些场景的加速比。
新增统计不解决既存DMA多sweep dirty-rebuild一致性问题。
下一轮应在保留本轮控制组的前提下，记录ready任务被跳过及资源拒绝原因，
再判断是否需要全局ready仲裁；不应直接继续增加上下文或算术单元。
