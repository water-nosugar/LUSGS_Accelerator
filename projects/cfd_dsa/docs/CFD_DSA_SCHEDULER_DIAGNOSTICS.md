# LU/TRSM调度拒绝与背压诊断

## 本轮边界

以2/2为实验基线、2/4为性能参照；继续使用
`projects/cfd_dsa/suites/coeff-context-ablation.json`的固定资源serial/divide配置。
不改变request遍历、RHS排序、发射策略、算术延迟、ISA、descriptor ABI或默认参数。
新增观测复用默认关闭的`--coeff-resource-stats=1`开关。

## 统计定义

`src/arch/arm/cfd_coeff_preprocess_controller.cc`中`issue()`输出每资源池的
`CFD_ISSUE_DIAG`；`processSolve()`、上下文准入及`enqueueDrain()`输出
`CFD_SCHED_DIAG`。均在控制器静止时输出累计快照，读取最后一条，不能跨快照求和。

代码定位（本轮版本）：FU申请在同文件918行；候选依赖检查3134行、候选遍历3251行、
未选中计数3446行；上下文额度检查2199/2482行；输出队列拒绝3492/3498行；
累计打印4047/4055行。后续代码编辑后应以函数名重新定位。

| 字段/原因 | 单位和含义 |
| --- | --- |
| attempts | FU申请次数，含成功和拒绝 |
| rejected | 申请时所有FU槽的nextIssue均大于当前周期 |
| rejected_after_issue | rejected的子集；该池本tick已经成功发射至少一次，随后又拒绝申请 |
| rhs_candidate | RHS依赖检查通过后进入本request候选列表的访问次数，包含Done收尾 |
| rhs_selected | 候选成功发射或执行Done收尾次数 |
| rhs_candidate_not_selected | 已访问候选但未成功选择次数 |
| rhs_aborted_unvisited | request失败导致剩余候选未访问次数 |
| rhs_inflight_dependency | RHS已有在途操作，未进入候选列表 |
| rhs_lu_step_dependency | RHS等待所需LU step，未进入候选列表 |
| rhs_reciprocal_dependency | 候选访问时等待共享reciprocal；本轮divide模式应为0 |
| input_slot_full | 等待输入slot的request被拒次数 |
| lu_context_full | 输入已就绪但LU上下文额度用完的request访问次数 |
| trsm_context_full | 满足early/normal准入条件但TRSM上下文额度用完次数 |
| output_ring_full | enqueueDrain因队列加在途总数达到outputDepth而拒绝次数 |
| drain_queue_full | 总输出额度允许，但等待队列达到drainQueueDepth而拒绝次数 |

这些是事件/访问次数，不是互斥的墙钟周期。多个RHS在同一周期被拒绝会计多次；
同一输出被不同调用点重试也会计多次。不能相加后从totalCycles扣除来估算收益。
未打印的reason按0处理。`rejected_after_issue`不能单独证明请求间不公平：
它也包含同一request内多个RHS竞争一个FU槽。

自动核对以下守恒关系：

```text
attempts = issued + rejected
rhs_candidate = rhs_selected + rhs_candidate_not_selected + rhs_aborted_unvisited
divide、正常完成时：
rhs_candidate_not_selected = coeff_div/mul/sub的rejected之和
```

## 为什么不立即改全局ready仲裁

当前`processSolve()`对每个活动request生成候选列表后，遍历全部候选；
失败request外没有“成功选择一次就退出”的break。因而需要区分：

- 活动request内，候选已经访问，只是共享FU不能再次发射；
- request尚未获准进入活动上下文，因此根本没有进入RHS调度；
- 固定request遍历顺序影响谁先拿到槽，进而影响关键路径。

本轮计数能区分前两类及资源争用，但尚不能把每次拒绝映射为端到端损失，
也没有全局反事实仲裁器证明换序收益。不能将FU拒绝总数称为“漏调度任务数”。
SPM内部bank/port/outstanding细分继续使用已有统计；这里不宣称覆盖所有DMA背压。

## 复现

```bash
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 \
  lusgs-step2 trsv5 decode-exclusive
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/coeff-context-ablation.json \
  --campaign scheduler-diagnostics-20260909
python3 projects/cfd_dsa/tools/check_context_ablation.py \
  results/cfd_dsa/campaigns/scheduler-diagnostics-20260909
python3 projects/cfd_dsa/tools/check_scheduler_diagnostics.py \
  results/cfd_dsa/campaigns/scheduler-diagnostics-20260909
```

## 本轮结果（2026-09-09）

证据位于上述campaign的`cold-l2-t2`、`cold-l2-t4`、`mixed-l2-t2`、
`mixed-l2-t4`目录的`simout.txt`与`manifest.json`；总周期来自
`streamingStep2.totalCycles`。cold为1×17单sweep，mixed为同规模3sweeps、interval2。

| 指标 | cold 2/2 | cold 2/4 | mixed 2/2 | mixed 2/4 |
| --- | ---: | ---: | ---: | ---: |
| 总周期 | 7916 | 7864 | 15080 | 14974 |
| ready候选访问 | 83429 | 174260 | 166858 | 348520 |
| 成功选择（含Done收尾） | 11270 | 11270 | 22540 | 22540 |
| 候选未选中 | 72159 | 162990 | 144318 | 325980 |
| 系数除法拒绝 | 52861 | 95975 | 105722 | 191950 |
| 系数乘法拒绝 | 19298 | 67015 | 38596 | 134030 |
| TRSM上下文满 | 7285 | 1962 | 14570 | 3924 |
| LU上下文满 | 4961 | 4574 | 9922 | 9148 |
| 输入slot满 | 4827 | 4701 | 9654 | 9402 |
| 等待LU step | 6499 | 11843 | 12998 | 23686 |
| 输出ring/队列满 | 0/0 | 0/0 | 0/0 | 0/0 |

除总周期外，表中均为累计事件/访问次数，不是独占周期。
四个重点用例均满足候选守恒、FU申请守恒，且未选中候选全部对应coeff FU拒绝；
没有失败中止造成的候选漏访问。背压计数与原有eventStall汇总一致。
这不等价于证明所有未准入request都没有可运行任务。

2/4降低TRSM准入等待，却增加了共享FU竞争；冷启动最长RHS等待
（`eventRhsMaxWaitCycles`）由432增至1043。它比2/2只减少52周期（0.66%），
mixed减少106周期（0.70%）。不能把拒绝次数上升解释为吞吐一定下降，
也不能将其直接视为可以由全局队列消除的周期。

冷启动系数减法、ColumnFma与combine拒绝均为0；mixed中ColumnFma拒绝15次，
两个配置相同。这是本轮工作负载证据，不是这些单元永不成为关键路径的证明。
与上一轮相比2/2的benchmark总周期少2，2/4一致；本轮没有调度改动，
该微小差异不作为加速成果，精确来源仍待确认。

## 下一步决策

暂不直接替换全局ready仲裁，不增FU或上下文。保持2/2，优先进行可切换的
跨request关键前沿优先/aging实验，与原request顺序对照：

1. 将候选等待按request、DInv/Lbar/Ubar以及是否阻塞当前前沿分类。
2. 仅在不同request争用同一FU时改变优先级，保留每个RHS内算术顺序。
3. 同时观察totalCycles、前沿完成时刻、最长等待及后台Ubar饥饿，
   不以拒绝次数下降作为唯一成功条件。
4. 只有证明当前仲裁造成可消除的关键等待，才进一步评估统一全局ready队列。

以上是下一轮实验设计，尚未实施；本轮完成观测与验证，不宣称产生新性能收益。

## 验证记录

- `scons build/ARM/gem5.opt -j2`及三个guest目标构建通过。
- 18个矩阵用例和1个observer-off用例全部通过full validation。
- `CONTEXT_ABLATION_CHECK_PASS`与`SCHEDULER_DIAGNOSTICS_CHECK_PASS`均通过；
  同组结果摘要、固定资源参数和操作数一致。
- 本轮observer-off为9476、on为9478，差+2周期，未超出原检查器8周期门限；
  不宣称严格零测量扰动。
- Python语法、suite JSON与格式检查通过。未重跑全部历史回归、
  多line或真实DMA测试；不能外推这些场景的性能或正确性。
