# LU-SGS Step2 系数预变换与 MVM 热循环

> 日期：2026-07-14
>
> 状态：已在 ARM O3 SE 模式完成实现与验证
>
> 范围：可选 Step2 路径；全部 TRSV5 和 Step2 基线方案均保持可用

## 1. 目标与兼容性边界

本工作新增两个可选模式：

```text
step2-pretransform
step2-pretransform-context
```

它们不替换 `step2`、`step2-core`、`step2-core-forwarded`、
`step2-core-forwarded-context` 或 `step2-core-forwarded-linebuf`。既有 TRSV5
仍按相同 FP64 顺序执行三角求解，并保留默认 60-cycle 粗粒度延迟模型。新路径将
重复的对角块求解移出 sweep 热循环，把每个更新变为 Path A MVM 加寄存器 Vector5
减法。

## 2. 矩阵语义审查

benchmark 初始化、reference solver 与旧 Step2 路径共同确定如下含义：

| 数组 | 实际含义 | 算法证据 |
|---|---|---|
| `lu_a[i]` | 对角块 `D[i]` 的紧凑 LU 因子 | 每个 forward cell 以 `lu_a` 调用 TRSV5 |
| `c_mat[i]` | 原始下耦合块 `C[i]` | reference 在 TRSV5 前计算 `rhs-C*dq_star_prev` |
| `b_bar[i]` | 已变换的上耦合块 `D^-1 U` | backward 直接计算 `dq=dq_star-b_bar*dq_next` |

新增系数定义为：

```text
D_inv[i] = solve(lu_a[i], I)
L_bar[i] = solve(lu_a[i], c_mat[i]) = D[i]^-1 C[i]
U_bar[i] = b_bar[i]
```

当前 benchmark 不含原始 `u_mat` 或 `sub_matrix_B`。再次对 `b_bar` 左乘 `D^-1`
会改变数学语义，因此 `u_bar_precomputed` 直接别名指向 `b_bar`。不会发出第三条
TRSM，`uBarReusedExisting` 记录这一决定。

紧凑 LU 的约定保持不变：下三角因子包含已存储的对角线和下三角元素，上三角
因子的对角线为单位值并包含已存储的上三角元素。预处理检查重构 `D=L*U`，并验证
`D*D_inv-I` 与 `D*L_bar-C`。

## 3. TRSM5 多 RHS 指令

新增 `trsm5_mrhs_spm` 指令接收三个 X 寄存器中的 SPM 基地址：

```text
xLU  -> 25 个 FP64 紧凑 LU 元素
xRHS -> 25 个 FP64 右端项元素
xOUT -> 25 个 FP64 输出元素
```

编码：

```text
[24:23]=11 [22]=0 [21:20]=00
[19:15]=xLU [14:10]=xRHS [9:5]=xOUT [4:0]=11101
```

实现按列读取 5x5 RHS。对每列 `j`，调用 standalone TRSV5 使用的同一
`cfdTrsv5Solve()` 算法，并将该列解写回输出矩阵。因此一条指令完成五个相互
独立的求解，同时不改变单列内部的除法和乘减顺序。

`CFDDSATrsm5MrhsOp` 配备独立 `CFD_DSA_TRSM5_MRHS_Engine`。默认配置为
`opLat=100`、`count=1`、可流水化。当前粗粒度模型会在 `execute()` 中直接执行
functional SPM 读写，因此该指令不可推测且串行化。这是 O3 可见延迟模型，不是
逐周期内部 TRSM 流水线。

参数：

```text
--trsm5-mrhs-enable=1
--trsm5-mrhs-lat=100
--trsm5-mrhs-count=1
--vec5-lat=2
--vec5-count=1
--pretransform-fallback-trsv5=1
--pretransform-diag-epsilon=1e-12
--pretransform-per-sweep=0
```

通用 Vector5 FU 仍保留原有默认 4 cycles 延迟。Step2 pretransform 显式覆盖为
2 cycles，因此旧模式保留原有默认行为。

## 4. 预处理数据流与存储位置

对每个 `(line, cell)`，软件将一个紧凑 LU 矩阵和一个矩阵 RHS 写入专用的瞬态
SPM 窗口，随后发出两条 TRSM 指令：

```text
I     -> TRSM5_MRHS(lu_a) -> SPM OUT -> d_inv heap 数组
c_mat -> TRSM5_MRHS(lu_a) -> SPM OUT -> l_bar heap 数组
b_bar --------------------------------> u_bar 指针别名
```

专用 SPM 窗口仅保存当前 5x5 批次：

```text
SPM_TRSM5_LU_BASE   当前紧凑 LU，200 B
SPM_TRSM5_RHS_BASE  当前 I 或 C 右端项，200 B
SPM_TRSM5_OUT_BASE  当前 D_inv 或 L_bar 结果，200 B
```

下一 cell 会覆盖这些窗口，它们不是持久系数存储。持久系数保存在对齐的
benchmark heap 数组中：

```text
Problem.d_inv                 [lines][cells][5][5]
Problem.l_bar_precomputed     [lines][cells][5][5]
Problem.u_bar_precomputed     指向 Problem.b_bar 的指针别名
Problem.pretransform_valid    [lines][cells]
```

在稳态 8x256 配置中，每组逻辑矩阵为 409,600 B。新增系数分配为 `d_inv`
409,600 B、`L_bar` 409,600 B，`U_bar` 因复用已有 `b_bar` 而新增 0 B。valid
标志占 2,048 B。两个新模式还保留独立的最终 `dq_star`/`dq` 结果数组用于对照，
四个数组合计 81,920 B。除去很小的指针和结构体字段，benchmark 总新增分配为
1,148,928 B。

## 5. 热循环 forward 映射

变换后的 forward 方程为：

```text
cell 0: dq_star[0] = D_inv[0] * rhs[0]
cell i: dq_star[i] = D_inv[i] * rhs[i]
                     - L_bar[i] * dq_star[i-1]
```

边界 cell 映射：

```text
D_inv, rhs -> Path A MVM -> 最终 dq_star[0]
```

普通 cell 在 AArch64 上的映射：

```text
D_inv, rhs             -> Path A MVM -> z10 (t0)
L_bar, dq_star_prev    -> Path A MVM -> z11 (t1)
z10, z11               -> vec5_sub_z -> z12 (t2)
z12                    -> 最终 dq_star[i] 结果数组
```

`t0`、`t1` 和 `t2` 不是 heap 数组，也不使用旧 tmp slot。固定指令编码在同一段
内联汇编中发出，使两个 MVM 结果保持存活，直至 `vec5_sub_z` 生成 `z12`。仅写回
最终五个 lane，lane 5 到 lane 7 会显式清零。

在 `step2-pretransform` 中，`dq_star_prev` 从结果数组读取；在
`step2-pretransform-context` 中，它来自 `ForwardLineContext.prev_dqstar`。
两种模式均写回最终结果数组，以保留可观察输出和正确性对照。

## 6. 热循环 backward 映射

该 benchmark 已提供变换后的 `U_bar=b_bar`，因此：

```text
last cell: dq[last] = dq_star[last]
cell i:    dq[i] = dq_star[i] - U_bar[i] * dq[i+1]
```

普通 cell 映射：

```text
U_bar, dq_next         -> Path A MVM -> z11 (t1)
dq_star[i]             -> z10
z10, z11               -> vec5_sub_z -> z12 (t2)
z12                    -> 最终 dq[i] 结果数组
```

变换后的热循环不含 TRSV5，也不存在临时 heap/SPM 结果往返。context 模式从
`BackwardLineContext.next_dq` 获得 `dq_next`；普通模式从 `dq` 结果数组读取。
严格的 cell 间依赖关系保持不变。

## 7. Path A slot 与中间结果生命周期

两个 forward MVM 使用既有 Path A 的 ping 和 pong 指令 slot。矩阵和输入向量经由
既有 Path A SPM 窗口进行瞬态 staging。产生的五 lane 数值保留在架构 Z 寄存器中，
作为 `vec5_sub_z` 消费的依赖 token。

实现记录 ping/pong issue、无效 slot 和 slot stall。已验证运行均报告零无效 slot
和零 slot stall。这仍是软件发射的 Path A 机制，不是新的硬件 SPM ping-pong
控制器、DMA 引擎或多 line 调度器。

## 8. 有效性检查与整轮回退

出现以下任一情况时，预处理将 cell 标记为无效：

- 任一紧凑 LU 对角元素非有限，或绝对值不大于配置的 epsilon；
- 任一生成系数为 NaN 或 infinity；
- `max(abs(D*D_inv-I))` 或 `max(abs(D*L_bar-C))` 超过 `1e-9`。

默认的一次性预处理模式下，只要存在一个无效 cell，整个 run 都执行保留的
forwarded TRSV5 路径。context 模式回退到对应 context 路径，从而避免在同一依赖
链中混用变换路径与直接 TRSV5 路径。所有有效输入测试均满足
`pretransformFallbackRuns=0` 和 `trsm5MrhsInvalid=0`。

## 9. 统计项

预处理统计项包括：

```text
preprocessCycles, preprocessCells, preprocessMatricesGenerated
trsm5MrhsIssued/Completed/Invalid/BusyCycles/StallCycles
trsm5MrhsInvBatches/LbarBatches/UbarBatches/ColumnsSolved
trsm5MrhsInputLuBytes/InputRhsBytes/OutputBytes
uBarReusedExisting, pretransformValidCells/InvalidCells/NaNInf
pretransformFallbackRuns
```

热循环统计项包括：

```text
pretransformDinvMvm/LbarMvm/UbarMvm
pretransformVectorSub, pretransformHotLoopTrsv5Elided
pretransformCoefficientBytesRead
pretransformTmpStores/Loads
pretransformPathAPingIssued/PongIssued/SlotStalls/InvalidSlots
forwardCycles, backwardCycles, runtimeCycles
totalCyclesIncludingPreprocess, amortizedCyclesPerSweep
cyclesPerFullCell, cyclesPerMvm, hot/total speedup, breakEvenSweeps
```

旧模式使用独立 `Stats` 对象，因此新增统计不会污染它们的 TRSV5 数据。

## 10. 正确性结果

所有模式均在启用检查的条件下一起运行。

| lines x cells | 最大 `D*D_inv-I` | 最大 `D*L_bar-C` | 端到端最大绝对误差 | 端到端最大相对误差 | mismatch | fallback |
|---:|---:|---:|---:|---:|---:|---:|
| 1x1 | 2.22045e-16 | 6.93889e-18 | 2.77556e-17 | 1.58114e-16 | 0 | 0 |
| 1x17 | 4.44089e-16 | 1.38778e-17 | 1.11022e-16 | 5.23460e-16 | 0 | 0 |
| 8x64 | 5.55112e-16 | 2.08167e-17 | 3.33067e-16 | 9.46011e-16 | 0 | 0 |

变换后的公式会改变 FP64 运算顺序，因而不要求 bitwise 完全一致；实测误差远低于
容差。

## 11. Sweep 摊销结果

配置：8 lines、256 cells、interleave=4、TRSM latency=100、Vector5 latency=2。
`runtime` 不含预处理，`total` 包含预处理。

| sweeps | forwarded runtime | forwarded-context runtime | pretransform runtime | pretransform total | pretransform-context runtime | pretransform-context total |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1,564,330 | 1,570,102 | 1,479,372 | 14,637,590 | 1,484,380 | 14,642,770 |
| 2 | 3,164,938 | 3,178,582 | 2,931,456 | 16,089,670 | 2,941,746 | 16,100,070 |
| 4 | 6,366,110 | 6,395,442 | 5,835,458 | 18,993,670 | 5,856,292 | 19,014,596 |
| 8 | 12,768,366 | 12,829,024 | 11,642,986 | 24,801,194 | 11,685,512 | 24,843,812 |
| 16 | 25,572,812 | 25,696,264 | 23,258,026 | 36,416,234 | 23,343,962 | 36,502,266 |
| 50 | 79,991,750 | 80,381,806 | 72,621,946 | 85,780,168 | 72,892,332 | 86,050,636 |

在 50 sweeps 时：

| 模式 | 预处理 | runtime | total | cycles/full cell | cycles/MVM | MVM | Vec5 减法 | 热循环 TRSV5 | TRSM MRHS | 热循环加速比 | 总加速比 | 结果 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| forwarded | 0 | 79,991,750 | 79,991,750 | 781.169 | 392.116 | 204,000 | 0 | 102,400 | 0 | 1.000 | 1.000 | PASS |
| forwarded-context | 0 | 80,381,806 | 80,381,806 | 784.979 | 394.028 | 204,000 | 0 | 102,400 | 0 | 1.000 | 1.000 | PASS |
| pretransform | 13,158,222 | 72,621,946 | 85,780,168 | 709.199 | 237.017 | 306,400 | 204,000 | 0 | 4,096 | 1.10148 | 0.93252 | PASS |
| pretransform-context | 13,158,304 | 72,892,332 | 86,050,636 | 711.839 | 237.899 | 306,400 | 204,000 | 0 | 4,096 | 1.10275 | 0.93412 | PASS |

`D_inv`、`L_bar` 和 `U_bar` 的逻辑系数载荷均为 409,600 B；`U_bar` 新增存储
占用为 0。50 sweeps 时，变换热循环读取 61,280,000 B 系数数据。每个 sweep 执行
6,128 次 MVM 与 4,080 次 Vector5 减法。

实测稳态节省量给出：

```text
forwarded 对比：         ceil(13,158,222 / 147,396.08) = 90 sweeps
forwarded-context 对比： ceil(13,158,304 / 149,789.48) = 88 sweeps
```

当前模型下，系数至少保持约 90 sweeps 不变，新路径才值得使用。如果 `D/C/U_bar`
更新更频繁，保留的直接 TRSV5 路径仍更合适。

## 12. TRSM 延迟敏感性

配置：8x256、一个 sweep、pretransform-context。

| TRSM 延迟 | 预处理 cycles | runtime cycles | total cycles | 结果 |
|---:|---:|---:|---:|---|
| 60 | 13,031,302 | 1,484,380 | 14,515,682 | PASS |
| 100 | 13,158,390 | 1,484,380 | 14,642,770 | PASS |
| 150 | 13,363,190 | 1,484,380 | 14,847,570 | PASS |
| 300 | 13,977,590 | 1,484,380 | 15,461,970 | PASS |

热循环与 TRSM 延迟无关。延迟高于 100 cycles 时，预处理成本严格按
`4096 * delta_latency` 变化，对应每个 cell 两条指令。

## 13. 构建与回归证据

最终修改后，以下检查均已通过：

```text
scons -Q build/ARM/gem5.opt -j1
build_benchmarks.py --opt=-O2 lusgs-step2 trsv5 decode-exclusive
python3 -m py_compile run_cfd_dsa.py run_cfd_lusgs_step2.py
decode-exclusive：旧 pack/TRSV、TRSM5 MRHS、Vector5 均 PASS
standalone TRSV5，17 次 solve：mismatch=0，TRSV5_PASS
standalone Path A，1000 iterations：Overall PASS
所有 Step2 模式，1x1/1x17/8x64：mismatch=0，PASS
```

证据位于 `m5out/lusgs-pretransform-*` 下，包括 `final-decode`、
`regression-trsv5`、`regression-patha`、`final-1x1`、`final-all-1x17`、
`final-8x64`、`perf-s*` 和 `lat*-s1`。

## 14. 当前限制与下一步

- TRSM5 MRHS 是粗粒度 functional 指令，而非内部流水线。
- Path A 矩阵和向量仍由软件配合 barrier staged 到 SPM。
- context 模式刻意保留软件 line context，作为对照点。
- 尚未引入新的 DMA、多 line 硬件调度器或 SPM ping-pong 引擎。
- 持久变换矩阵会增加存储占用和系数带宽。
- `pretransform-per-sweep=1` 会刻意破坏摊销，仅用于敏感性测试。
- 下一步可在本地矩阵 buffer 中复用变换后 tile，并以显式 producer/consumer
  token 调度两个 forward MVM。后续可在不改变既有算术顺序的前提下，独立实现
  内部 TRSM 流水线。

## 15. 优化预处理模式与兼容边界

本轮在原模式旁路新增：

```text
step2-pretransform-optprep
step2-pretransform-optprep-context
```

旧 `step2`、`step2-core`、三种 forwarded/context/linebuf 路径、
`step2-pretransform[-context]` 以及 `trsm5_mrhs_spm` 均未删除或改写。两个 optprep
模式只替换一次性系数生成器；forward/backward 热循环继续调用与第 5、6 节相同的
Path A MVM 和 `vec5_sub_z` 代码。8x256、一个 sweep 时旧 pretransform 与 optprep
runtime 分别为 1,478,760 和 1,477,348 cycles，差异为 0.096%，可视为同一热路径。

## 16. 单 LU 读取双批指令

新增 cell-local 指令 `trsm5_inv_lbar_spm`，对应
`CFDDSATrsm5InvLbarOp` 和 `CFD_DSA_TRSM5_INV_LBAR_Engine`。默认
`opLat=200`、`count=1`、可流水化；固定寄存器接口为：

```text
x21 = LU(D) 地址       x22 = C 地址
x23 = D_inv 输出地址   x24 = L_bar 输出地址
x26 = input token      x25 = completion token = x26 + 1
```

编码精确匹配 `x21/x22/x23/x24` 字段，放在旧 MRHS 解码之前，不扩大旧指令的
匹配范围。执行时只读取一次 200 B 紧凑 LU 和一次 200 B C。单位矩阵不占 SPM：
硬件对列 `j` 生成只有 `value[j]=1` 的五 lane RHS。随后先按列 0..4 生成
`D_inv`，再按列 0..4 生成 `L_bar`，每列都调用原 `cfdTrsv5Solve()`。
除法、乘法、减法及 volatile multiply-subtract 顺序保持不变，不使用倒数近似或
强制 FMA。`U_bar` 仍直接别名复用 `b_bar`。

参数为：

```text
--trsm5-mrhs-dual-enable=1
--trsm5-mrhs-dual-lat=200
--trsm5-mrhs-dual-count=1
```

## 17. SPM ping-pong、token 与输出缓冲

输入 slot 的地址和内容为：

```text
slot 0 base = 0x70002800    slot 1 base = 0x70003000
+0 B   LU, 200 B
+200 B C,  200 B
+400 B realistic D_inv output, 200 B
+600 B realistic L_bar output, 200 B
```

每个软件 slot 状态包含 `valid/busy/generation/line_id/cell_id`。slot 数由
`--pretransform-spm-slots=1|2` 控制。输入路径每 cell 只 stage 一次 LU 和 C，I
stage 次数和字节数恒为 0。debug 模式 `--pretransform-use-barrier=1` 使用旧
`dsb sy; isb`；默认 token 模式删除该全系统+取指 barrier，但保留 `dsb st` 作为
SPM store 完成边界。这里的 `pretransformBarriersElided` 特指旧 full barrier，不能
解读为完全没有内存排序指令。

完成端使用真正的数据依赖，而非只做软件标志检查：x25 token 写入 entry，drain
地址计算为 `output_addr + completion_token-(generation+1)`。因此 O3 的 SPM load
必须等待新指令产生 x25。decode 回归曾捕获到缺少该地址依赖时的提前 load；加入
依赖后 `trsm5_inv_lbar_spm`、token 和双输出均 PASS。

可配置 coefficient output buffer 位于 uncacheable SPM：

```text
base = 0x70004000
entry stride = 0x200 B
entry = {valid, line/cell/generation, completion_token,
         D_inv SPM address, L_bar SPM address}
```

深度范围为 1..8。指令完成只写 SPM entry，不等待普通 heap store；drain 再把两个
200 B 块写入持久 `Problem.d_inv` 与 `Problem.l_bar_precomputed`。默认深度 4 时
最大占用为 2、full stalls 为 0。深度 1、17 cells 的回压回归得到 alloc/free=17、
full stalls=16、max occupancy=1，且 full residual 校验通过。

当前 coarse 指令仍带 serialize-before/after，软件循环也没有 event completion
时间戳。因此两个输入 slot 和输出 queue 已保证 tag、覆盖保护与 backpressure，
但没有证明 stage/compute/drain 在时间上重叠；三个 overlap 统计均如实为 0。

## 18. full/fast 校验与统计口径

`--pretransform-validate=full` 执行软件参考系数、NaN/Inf、对角 epsilon 和两个
residual；`fast` 只保留对角 finite/epsilon 与所有输出 finite 检查。两者都执行
最终 SPM-to-heap 写回，fast 不是跳过输出的性能捷径。

8x256 默认 optprep 的周期分解为：

| validation | total | stage | dual compute | drain | validation | residual | barrier | idle |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fast | 1,971,204 | 318,898 | 452,840 | 340,772 | 718,172 | 0 | 0 | 140,522 |
| full | 13,103,458 | 318,898 | 452,840 | 340,772 | 11,850,472 | 11,083,674 | 0 | 140,476 |

full 数据说明 residual 软件矩阵运算仍是 correctness 成本，不能归入硬件 dual
compute。正式预处理吞吐比较采用 fast，同时单独给出 full 结果。

新增统计覆盖 total/stage/compute/drain/validation/barrier/idle、LU/C/I 与输出字节、
dual issue/LU reads/columns、buffer alloc/free/full、token stall 和 auto selection。
`preprocessOverlapEfficiency` 当前为 0，未用估算值伪造重叠。

## 19. 理论计数与实测计数

8x256 共 2048 cells。实测严格等于理论值：

| 项目 | 旧 pretransform | optprep |
|---|---:|---:|
| MRHS/dual issue | 4,096 | 2,048 |
| LU stage/read | 4,096 | 2,048 |
| identity stage | 2,048 | 0 |
| C stage | 2,048 | 2,048 |
| solved columns | 20,480 | 20,480 |
| LU stage bytes | 819,200 | 409,600 |
| C stage bytes | 409,600 | 409,600 |
| optprep output SPM/heap bytes | - | 819,200 / 819,200 |

optprep 的 `trsm5InvLbarLuReads=2048`、invalid=0、fallback=0，证明每 cell 只读
一次 LU，且没有省略最终系数写回。

## 20. 分阶段性能贡献

8x256、sweeps=1、fast validation 的实际运行如下。LU 复用、内部 I 和 dual MRHS
由同一新指令共同实现，无法从该实现中无损拆成三个独立周期点，故合并报告，不对
三者虚构分摊。

| 阶段 | preprocess cycles | 相对上一阶段变化 | 说明 |
|---|---:|---:|---|
| baseline pretransform | 13,171,910 | - | 两次旧 MRHS、full residual |
| + LU reuse + identity generation + dual MRHS | 2,091,902 | -11,080,008 | 单 slot、full barrier、无 output buffer、fast validation |
| + 双 SPM slot | 2,094,444 | +2,542 | 串行模型中为测量噪声，无真实 overlap |
| + token 化 full-barrier 删除 | 2,042,934 | -51,510 | 仍保留 `dsb st` 输入发布 |
| + depth-4 output buffer | 1,971,204 | -71,730 | max occupancy=2，full stalls=0 |

最终相对旧 13,171,910 减少 11,200,706 cycles，即 85.03%，达到激进目标。最大
贡献明确来自单 LU 读取、硬件 I 和双批合并这一组合，而非 ping-pong 或 opLat。

## 21. 摊销、回本点与自动选路

下表不是十次独立大规模仿真；它使用当前实际测得的一次性 preprocess 和单-sweep
runtime 做线性摊销。单位均为 cycles。

| sweeps | forwarded | forwarded-ctx | old pretransform | old pretransform-ctx | optprep | optprep-ctx |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1,592,502 | 1,571,424 | 14,650,670 | 14,656,550 | 3,448,552 | 3,458,094 |
| 2 | 3,185,004 | 3,142,848 | 16,129,430 | 16,140,504 | 4,925,900 | 4,945,076 |
| 4 | 6,370,008 | 6,285,696 | 19,086,950 | 19,108,412 | 7,880,596 | 7,919,040 |
| 8 | 12,740,016 | 12,571,392 | 25,001,990 | 25,044,228 | 13,789,988 | 13,866,968 |
| 16 | 25,480,032 | 25,142,784 | 36,832,070 | 36,915,860 | 25,608,772 | 25,762,824 |
| 32 | 50,960,064 | 50,285,568 | 60,492,230 | 60,659,124 | 49,246,340 | 49,554,536 |
| 50 | 79,625,100 | 78,571,200 | 87,109,910 | 87,370,296 | 75,838,604 | 76,320,212 |
| 64 | 101,920,128 | 100,571,136 | 107,812,550 | 108,145,652 | 96,521,476 | 97,137,960 |
| 90 | 143,325,180 | 141,428,160 | 146,260,310 | 146,728,456 | 134,932,524 | 135,799,492 |
| 128 | 203,840,256 | 201,142,272 | 202,453,190 | 203,118,708 | 191,071,748 | 192,304,808 |

非 context 热循环加速为 `1,592,502/1,477,348=1.07795x`。保守回本点为
`ceil(1,971,204/(1,592,502-1,477,348))=18 sweeps`；context 对应约 24 sweeps。
原先 50-sweep 稳态数据给出约 14 sweeps，但自动策略采用更保守的 18。

`--lusgs-pretransform-auto=1` 只做整轮选择，不在 cell 间切换。实测
`expected=17` 时 `trsv5AutoSelected=1`，`expected=18` 时
`pretransformAutoSelected=1`。因此系数能保持至少 18 sweeps 时推荐 optprep；频繁
变化时仍应选择原 forwarded TRSV5。

## 22. 本轮回归与剩余问题

已通过：gem5 `-j2` 构建、O2 guest 构建、Python 编译、`git diff --check`、
decode-exclusive、新 dual/token、standalone TRSV5、Path A、1x1、1x17 all、8x64
all、深度 1 backpressure、full/fast validation 和 auto 边界。证据集中在
`results/lusgs_optprep/`。

仍未解决的问题：当前 dual 指令在 `execute()` 中 functional 完成数值与 SPM 写回，
`opLat` 只是 O3 可见延迟；serialize flags 阻止了真实 ping-pong 并发，输出 drain
也只是解耦 queue 而非 event-driven store engine。下一步应给 dual engine 增加
逐周期 completion event、独立 store-drain 资源和时间戳统计，再让 slot 1 staging
与 slot 0 compute 真正并行；不得通过非零估算 overlap 统计代替该实现。

## 23. Raw D/L/U/R coefficient pipeline

前 22 节记录的是 `legacy-prepared-input` 路径；其中 `lu_a=LU(D)`、
`b_bar=D^-1 U` 已经是中间系数，不能代表完整求解成本。新增 raw 路径的唯一外部
输入为每 cell 的 `D[25]`、`L[25]`、`U[25]`、`R[5]`，分别保存在
`Problem.d_mat/l_mat/u_mat/rhs`。generator 直接、确定性地产生对角占优的 D 和独立
的 L/U，不从 `b_bar` 反推 U。初始化不进入求解周期。

公共预处理对每个 D 执行固定 5x5 Crout LU：packed LU 的下三角（含对角）保存
`L_D`，严格上三角保存单位对角 `U_D` 的非对角项。循环顺序与
`cfdTrsv5Solve()` 完全匹配，不使用近似倒数或 FMA 重排：

```text
D -> cfd_lu5_factor -> lu_d
lu_d + U -> trsm5_mrhs_spm -> u_bar
```

`u_bar` 是独立分配的 `[lines][cells][5][5]` 数组，raw 模式的
`uBarReusedExisting` 恒为 0。软件 reference 也从同一 D/L/U/R 开始，先分解 D、
生成 Ubar，再执行原始前向 TRSV 与后向更新。

## 24. Raw TRSV5 与预变换映射

`step2-trsv5-raw[-context]` 在公共预处理后复用原 Step2 Path A 热循环：前向计算
`R-L*dq_star_prev` 并送入 `trsv5_lu_spm_zrhs`，后向计算
`dq_star-U_bar*dq_next`。中间 RHS 保持在 Z/forwarding 路径，不恢复 heap tmp。

`step2-pretransform-raw[-context]` 在公共阶段后额外执行
`trsm5_inv_lbar_spm(lu_d,L)->(D_inv,L_bar)`。热循环为：

```text
forward:  D_inv*R - L_bar*dq_star_prev
backward: dq_star - U_bar*dq_next
```

`step2-pretransform-raw-optprep[-context]` 使用新增
`trsm5_coeff3_spm`。固定接口为 x10/x11/x12 输入 LU/L/U，x13/x14/x15 输出
Dinv/Lbar/Ubar，x26 输入 token，x25 完成 token；编码为 `0x018a56d7`，OpClass 为
`CFDDSATrsm5Coeff3Op`，默认 FU 为 1 个、300-cycle coarse latency。指令只处理一个
cell，内部生成 I，并严格调用 15 次共享 `cfdTrsv5Solve()`。

每个 coeff3 SPM slot 为 1200B：LU/L/U/Dinv/Lbar/Ubar 各 200B，支持 1/2 个输入
slot。三输出 ring 深度支持 1/2/4/8；entry 保存 generation、line/cell、token 和三
个输出地址。输入发布后发射 coeff3，完成屏障保证 functional write 对年轻 load
可见，再 drain 到三个独立 heap 系数数组。8x256 实测 output occupancy=2、full
stall=0。该屏障是当前 coarse functional 模型的可见性边界，不代表逐拍 store
engine。

## 25. 失败与更新语义

LU failure、Ubar 非有限值或公共 residual 超限时，整个 raw run 复制纯软件 raw
reference，增加 `commonPreprocessFallbackRuns`。若公共 LU/Ubar 有效而 Dinv/Lbar
失败，则真正执行 TRSV5-raw fallback，增加
`pretransformFallbackToTrsv5Raw`，绝不回退到 legacy `b_bar`。

`--coefficient-update-interval=N` 将 sweeps 分块，每块开始重建系数；0 表示本次
运行只预处理一次。`--ubar-inplace=1` 当前明确拒绝，默认 0，避免覆盖 raw U。
`raw-compare` 和 `raw-compare-context` 是只包含 reference/TRSV/dual/coeff3 的公平
测试组合；原有单模式与 `all` 均保留。

## 26. 正确性与理论计数

1x1、1x17 和 8x64 full-validation 的全部 legacy/raw 模式均 PASS，mismatch、
fallback、NaN/Inf 均为 0。8x64 最大 LU residual 为 `1.776e-15`，Dinv residual
`6.661e-16`，Lbar/Ubar residual 不超过 `2.082e-17`。8x256 fast 性能运行中：

| 路径 | LU cells | Ubar MRHS | coeff3 | solved columns | 结果 |
|---|---:|---:|---:|---:|---|
| TRSV raw | 2048 | 2048 | 0 | 10240 | PASS |
| PRE dual+MRHS | 2048 | 2048 | 0 | 10240+20480 | PASS |
| PRE coeff3 | 2048 | 0 | 2048 | 30720 | PASS |

raw 输入 D/L/U/R 分别为 409600/409600/409600/81920B。TRSV 持久系数 LU+Ubar
为 819200B；PRE 的 LU+Ubar+Dinv+Lbar 为 1638400B，额外 819200B。每 sweep 的
逻辑系数读取量由实际 MVM 数决定：PRE 比 TRSV 多 2048 次 MVM，即额外读取
409600B 系数矩阵和 81920B 向量，共 491520B/sweep 逻辑输入流量；coeff3 没有
省略最终 Ubar 写回。

## 27. 8x256 公平性能

配置：interleave=4、fast validation、固定系数、2-sweep 联测。软件 residual 不在
硬件 compute timer 内。不同统计段的公共周期有轻微抖动，公平表统一采用 TRSV
测得的公共基线。

| 模式 | LU factor | Ubar generation | common | Ubar residual | 结果 |
|---|---:|---:|---:|---:|---|
| TRSV raw | 798230 | 1034164 | 1832394 | 2.082e-17 | PASS |
| PRE dual | 797756 | 1021822 | 1819578 | 2.082e-17 | PASS |

| 模式 | Dinv/Lbar 或融合阶段 | coeff3 compute | 公平 extra | 结果 |
|---|---:|---:|---:|---|
| PRE dual+MRHS | 1454686 | 0 | 1454686 | PASS |
| PRE coeff3 | 2443784 | 688128 | 1409620 | PASS |

coeff3 的 2443784 包括 stage、coarse execute、三输出 drain 与同步，不能无损拆分
为 Ubar/Dinv/Lbar；公平 extra 只做可验证差值 `coeff3_phase-TRSV_Ubar`。以各自
实测 coefficient phase 比较，dual 的 `Ubar+extra=2476508`，coeff3 为 2443784，
减少 32724 cycles（1.32%），并非数量级优化。

| 模式 | forward/sweep | backward/sweep | runtime/sweep | MVM/sweep | Vector5/sweep | TRSV5/sweep | cycles/cell |
|---|---:|---:|---:|---:|---:|---:|---:|
| TRSV raw | 966136 | 622775 | 1589115 | 4080 | 4080 | 2048 | 775.94 |
| PRE dual | 773555 | 686626 | 1460570 | 6128 | 4080 | 0 | 713.17 |
| PRE coeff3 | 768964 | 683173 | 1452443 | 6128 | 4080 | 0 | 709.20 |

热循环加速分别为 1.0880x 和 1.0941x。dual 公平回本点为
`ceil(1454686/(1589115-1460570))=12` sweeps；coeff3 为 11 sweeps。context 联测
TRSV/PRE runtime 为 1609488/1469293 cycles/sweep，dual 回本点同样约 11 sweeps，
没有优于非 context 热循环。

## 28. Sweep 与更新频率表

下表采用上面的实测公共、extra 和 2-sweep runtime 线性组合；已用独立 1-sweep
和 2-sweep 运行核对线性计数，不宣称是十次 128-sweep 大仿真。单位为 cycles。

| sweeps | TRSV common | PRE common | PRE extra | TRSV runtime | PRE runtime | TRSV total | PRE total | hot speedup | total speedup |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1832394 | 1832394 | 1454686 | 1589115 | 1460570 | 3421509 | 4747650 | 1.0880 | 0.7207 |
| 2 | 1832394 | 1832394 | 1454686 | 3178230 | 2921140 | 5010624 | 6208220 | 1.0880 | 0.8071 |
| 4 | 1832394 | 1832394 | 1454686 | 6356460 | 5842280 | 8188854 | 9129360 | 1.0880 | 0.8970 |
| 8 | 1832394 | 1832394 | 1454686 | 12712920 | 11684560 | 14545314 | 14971640 | 1.0880 | 0.9715 |
| 16 | 1832394 | 1832394 | 1454686 | 25425840 | 23369120 | 27258234 | 26656200 | 1.0880 | 1.0226 |
| 32 | 1832394 | 1832394 | 1454686 | 50851680 | 46738240 | 52684074 | 50025320 | 1.0880 | 1.0531 |
| 50 | 1832394 | 1832394 | 1454686 | 79455750 | 73028500 | 81288144 | 76315580 | 1.0880 | 1.0652 |
| 64 | 1832394 | 1832394 | 1454686 | 101703360 | 93476480 | 103535754 | 96763560 | 1.0880 | 1.0700 |
| 90 | 1832394 | 1832394 | 1454686 | 143020350 | 131451300 | 144852744 | 134738380 | 1.0880 | 1.0751 |
| 128 | 1832394 | 1832394 | 1454686 | 203406720 | 186952960 | 205239114 | 190240040 | 1.0880 | 1.0788 |

固定总长度 128 sweeps、每 interval 重建一次系数时：

| update interval | rebuilds | TRSV total | PRE total | speedup | 推荐 |
|---:|---:|---:|---:|---:|---|
| 1 | 128 | 437953152 | 607699200 | 0.7207 | TRSV |
| 2 | 64 | 320679936 | 397326080 | 0.8071 | TRSV |
| 4 | 32 | 262043328 | 292139520 | 0.8970 | TRSV |
| 8 | 16 | 232725024 | 239546240 | 0.9715 | TRSV |
| 16 | 8 | 218065872 | 213249600 | 1.0226 | PRE |
| 32 | 4 | 210736296 | 200101280 | 1.0531 | PRE |
| 64 | 2 | 207071508 | 193527120 | 1.0700 | PRE |
| 128 | 1 | 205239114 | 190240040 | 1.0788 | PRE |

因此系数每 sweep 都变时选择 TRSV raw；更新间隔 8 仍不足，12 是数学交点，给定
离散测试点从 16 开始 PRE 获益。最终策略是在运行开始按预计 coefficient lifetime
选择整轮模式，不在 cell 内混合。

## 29. 当前边界

新增 coeff3 与旧 TRSV/MRHS/dual 一样仍是 functional execute + O3-visible coarse
latency，不是真正逐拍 15-RHS 内部流水线；serialize/屏障限制了 slot 的真实并行。
LU factor 仍由软件执行。下一步应先做 coeff3 event-driven completion/store drain，
再评估 LU factor 硬件化；不能仅调低 opLat 冒充结构优化。

## 30. 细粒度事件驱动系数预处理

第 29 节描述的是保留的 `coarse` 基线。当前新增实现通过
`--coeff-preprocess-model=event --lu5-model=event` 启用，不改变旧指令编码、旧
benchmark 模式或 coarse 统计。event 模式不再调用一次性完成 15 RHS 的
`trsm5_coeff3_spm` 来模拟时间，也不通过降低 `opLat` 获得结果，而是由
`CfdCoeffPreprocessController` 的 gem5 tick event 每周期推进输入、LU、TRSM 和
drain 四类资源。

### 30.1 ISA、descriptor 与局部 token

新增 launch/wait 控制指令使用 160B、8B 对齐 descriptor。descriptor 保存
`flags/generation/lineId/cellId`、D/L/U 输入地址、LU/Dinv/Lbar/Ubar 输出地址、
completion record 地址和 pivot epsilon。launch 返回非零 request token；wait 返回
`Complete/Busy/BadDescriptor/BadAlignment/QueueFull/BadToken/LuFailure/
GenerationMismatch/InternalError`。两条指令均为 non-speculative，但 event 路径不使用
全局 serialize-before/after；依赖由 request token、stage 和各矩阵 ready/drain 状态表达。

同一 `lineId/cellId` 的 generation 必须单调增加。控制器在 launch 时检查 live
generation，防止旧请求覆盖新系数。request 只有在所需输出全部 drain 后才 Complete，
wait/reap 后才释放 request entry。输入 slot 在 D/L/U 已复制到 request-local buffer、
LU 真正 issue 时释放，因此 slot 复用不会覆盖正在计算的数据。

### 30.2 请求状态和数据保存位置

request 状态为：

```text
WaitingSlot -> InputFetching -> InputReady -> LuQueued -> LuActive
            -> LuReady -> SolveQueued -> SolveActive -> DrainPending
            -> Complete / Failed
```

每个 request 内部保存 `d/lower/upper/lu/dInv/lBar/uBar` 七个 5x5 FP64 数组、15 个
`RhsState`、当前 LU 列/行/k、临时乘积、分批完成计数和各 drain 位。因而 LU 和
TRSM 的中间结果始终位于控制器 request-local hardware buffer；只在 drain 完成时
通过 `writeBlob` 对最终 heap 地址可见。软件不再同步复制 Dinv/Lbar/Ubar。completion
record 保存全部 stage 时间戳、操作数、stall、active/overlap cycle 和字节计数。

### 30.3 输入 staging 和 SPM 映射

首次处理 request 时从源地址读取 D/U；coeff3 另读 L。随后按
`--coeff-spm-write-width` 分块占用 SPM write port 和 bank，写完后发布 input token。
TRSV 每 cell 输入 400B，PRE 每 cell 输入 600B。默认 slot stride 为 `0x800`，输入
chunk 地址为：

```text
slot_base = slot * 0x800
bank = (slot_base + chunk * write_width) / bank_granularity % bank_count
```

默认 write width=40B、bank granularity=64B、4 banks、1 read/1 write port。输入和
drain 使用同一周期 bank 占用表，所以同时命中同 bank 会产生真实
`stallSpmBank`，端口不足分别累计 read/write stall。源读延迟、SPM 写延迟、端口、
bank 数和粒度均由 `--coeff-*` 参数配置。

### 30.4 逐周期 Crout LU5

LU 状态机严格按原 `cfd_lu5_factor()` 的 Crout 顺序执行：

```text
StartLower -> LowerMul -> LowerSub -> StoreLower -> PivotCheck
           -> StartUpper -> UpperMul -> UpperSub -> DivideUpper
           -> StoreUpper -> NextColumn -> Done
```

每个乘法先产生独立 FP64 product，再经独立 subtract event 更新累加值，不收缩为
FMA；除法也直接做 FP64 divide，不使用 reciprocal。默认 LU 资源为 1 engine、1
divider（lat=12, II=12）和 1 组 mul/sub pipeline（lat=3/4）。5x5 每 cell 精确发射
10 div、30 mul、30 sub。pivot 非 finite 或 `abs(pivot)<=epsilon` 时 request 以
`LuFailure` 结束。

`--lu-forwarding=1` 时 packed LU ready 后直接从 request-local LU buffer 进入统一
TRSM kernel，同时可独立 drain LU；关闭时必须等待 LU drain 并经过 reload 延迟后才能
solve，用于量化 heap/SPM round trip。LU engine count、pending depth、divider 和
mul/sub 参数均可独立配置。

### 30.5 统一 5/15 RHS TRSM pipeline

TRSV raw 与 PRE coeff3 共享同一 `RhsState` 和资源池，唯一区别是 RHS 数量：TRSV
处理 U 的 5 列，PRE 按 I、L、U 三批共 15 列。单 RHS 状态为：

```text
ForwardDiv -> ForwardMul -> ForwardSub -> LastDiv
           -> BackwardMul -> BackwardSub -> Done
```

状态机保持 `cfdTrsv5Solve()` 的 k/i 依赖和 FP64 运算次序。同一 RHS 的下一步只有在
前一 event 完成后才可发射，不同 RHS 可占用 `--coeff3-rhs-lanes` 条 lane。divider 的
latency、II 和 count 分离建模；mul 和 sub 有独立 latency/resource pool。当前默认 15 个
RHS context、1 divider（lat=12, II=4）、1 条 mul/sub lane（lat=3/4）。每 cell TRSV 发射
25 div、100 mul、100 sub；PRE 发射 75 div、300 mul、300 sub。

### 30.6 partial ready 与独立 drain

PRE 中 RHS 0..4、5..9、10..14 全部完成时分别发布 DInvReady、LBarReady 和
UBarReady。`--coeff3-partial-output=1` 允许在计算下一批时把已完成矩阵加入 drain
queue；关闭后等待全部 15 RHS 完成再统一入队。drain engine 独立受 width、port、
latency、outstanding、queue depth 和 output buffer depth 约束，每个 200B 矩阵按块
占用 SPM read port/bank，最后一个块经过 drain latency 后才写最终 heap 并发布 done。

drain 地址映射为
`slot*0x800 + matrix_type*0x100 + bytes_done`。LU/Dinv/Lbar/Ubar 各用独立
matrix type；queue 满、output ring 满、outstanding 满、port 或 bank 冲突都会阻塞，
不会由 benchmark 绕过。request 的完整可见条件为：TRSV 的 LU+Ubar drain 完成，
或 PRE 的 LU+Dinv+Lbar+Ubar drain 完成。

### 30.7 真实 overlap、trace 与统计

每个 tick 根据当周期各 engine 是否 active 累计 `inputLu`、`inputSolve`、
`luSolve`、`computeDrain` 和 `threeWay` overlap；没有根据理论阶段长度补写计数。
stall 分为 input slot、LU/solve pending、output ring、LU/coeff divider、mul/sub、
dependency、SPM read/write port、bank 和 drain queue。另有请求 alloc/complete/fail/free、
first/average/last cell latency、steady-state II、各 batch ready latency、busy lane cycle、
输入/输出字节和最大 output occupancy。

trace 由 `--coeff-preprocess-trace-enable=1` 打开，CSV 字段为：

```text
cycle,requestId,generation,line,cell,engine,stage,rhsBatch,rhsColumn,
k,row,slot,queueOccupancy,readyToken,completionToken,note,
requester,packetId,address,size,bank,port
```

可观察 `INPUT_START/TOKEN`、`LU_ISSUE/DIV_ISSUE/COLUMN_DONE/COMPLETE`、
`COEFF3_ISSUE/DINV_READY/LBAR_READY/UBAR_READY`、各 drain start/done、stall 和
REQUEST_COMPLETE。`--coeff-validation=performance|sampled|full` 将性能计时与软件
residual 分离；performance 只保留 pivot/output finite 安全检查。

### 30.8 最终正确性和性能证据

event full-validation 已通过 1x1、1x17 和 8x64；coarse 1x17 回归也通过。最终
8x64 raw-compare 结果如下：

| 项目 | TRSV event | PRE event |
|---|---:|---:|
| requests complete/fail | 512 / 0 | 512 / 0 |
| LU residual | 1.776e-15 | 1.776e-15 |
| Ubar residual | 2.082e-17 | 2.082e-17 |
| first/avg/last cell latency | 615 / 1079.63 / 877 | 1022 / 1868.34 / 1499 |
| steady-state II | 452.57 | 726.21 |
| Dinv/Lbar/Ubar avg ready | - / - / 1074.63 | 1461.34 / 1662.34 / 1863.34 |
| LU-solve overlap | 88401 | 154153 |
| compute-drain overlap | 4605 | 9525 |
| three-way overlap | 608 | 747 |
| input/output bytes | 204800 / 204800 | 307200 / 409600 |
| result | PASS | PASS |

`8x256, interleave=4, validation=performance, sweeps=16` 的新公平测量为：

| 项目 | TRSV event | PRE event |
|---|---:|---:|
| preprocess cycles | 930172 | 1488166 |
| fair extra preprocess | 0 | 557994 |
| runtime cycles / 16 sweeps | 25583242 | 23118460 |
| runtime cycles/sweep | 1598952.63 | 1444903.75 |
| steady-state II | 452.97 | 725.99 |
| LU-solve overlap | 353839 | 617178 |
| compute-drain overlap | 18430 | 38093 |
| fair break-even | - | 4 sweeps |
| result | PASS | PASS |

因此当前默认资源下 PRE 的新增预处理成本为 557994 cycles，每 sweep 节省约
154049 cycles，事件模型公平回本点为 4，而不是 coarse 模型的 11。该结论只适用于
上述资源与 validation 配置；系数寿命至少 4 sweeps 时选 PRE，否则选 TRSV。

### 30.9 当前瓶颈和未完成项

默认 PRE 的 solve pending stall 为 978227 cycles、dependency stall 为 5298176，
LU-solve overlap 为 617178；divider 是关键串行资源，SPM 仅有 2 bank-conflict cycles，
output occupancy 最大为 1 且 drain queue stall 为 0，因此默认配置下 staging/drain
不是 steady-state II 的主瓶颈。

第 30 节的内部 SPM、无 cancel、仅 event LU 限制已经由第 31 节后续实现替代；
`--coeff-spm-model=internal` 仍保留为对照上界。

完整 one-factor 资源扫描已固化为：

```bash
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/coeff-event-sensitivity.json
```

suite 固定 8x256、interleave=4、performance validation，并覆盖 RHS lanes 1/3/5/15、
divider count/latency/II、mul-sub lanes 1/2/5/10、pending depth 1/2/4、三组 drain 配置
和 input slots 1/2/4。26 个 case 已全部通过并写入
`results/cfd_dsa/campaigns/coeff-event-sensitivity/`。关键 one-factor 结果为：

| 配置变化 | PRE preprocess | steady II | 结论 |
|---|---:|---:|---|
| RHS lanes 1 / 3 / 5 / 15 | 6363940 / 2284326 / 1488042 / 1065374 | 3105.53 / 1114.51 / 725.89 / 519.61 | 最大敏感项 |
| div count 1 / 2 / 5 | 1488042 / 1463732 / 1448740 | 725.89 / 714.02 / 706.71 | 多 divider 边际收益小 |
| div II 1 / 4 / 12 | 1456316 / 1488034 / 2286828 | 710.41 / 725.89 / 1115.73 | II=12 明显受限 |
| mul/sub lanes 1 / 2 / 5 / 10 | 1488046 / 1488046 / 1488046 / 1487992 | 725.89 / 725.89 / 725.89 / 725.89 | 默认 RHS5 下 1 lane 已足够 |
| pending depth 1 / 2 / 4 | 1487992 / 1487992 / 1487992 | 725.89 / 725.89 / 725.89 | depth=1 已隐藏排队 |
| drain 40p1d1 / 64p1d4 / 64p2d8 | 2345442 / 1485634 / 1390200 | 1145.08 / 724.71 / 677.75 | depth=1 形成 drain 瓶颈 |
| input slots 1 / 2 / 4 | 1449288 / 1488044 / 1488044 | 707.30 / 725.89 / 725.89 | 2 到 4 无收益 |

单因素结果显示，仅扩大 mul/sub、pending depth 或 input slot 没有收益；RHS context
数量最敏感，但 RHS15 会把瓶颈转移到 divider。进一步组合测试得到：

| Pareto 候选 | PRE preprocess | steady II | fair extra | 1-sweep break-even |
|---|---:|---:|---:|---:|
| RHS5/div1/mul1，面积最小基线 | 1488046 | 725.89 | 557866 | 6 |
| RHS15/div1/mul1，面积/性能膝点 | 1127000 | 549.68 | 196828 | 3 |
| RHS15/div1/mul5，低延迟 | 1065374 | 519.61 | 135202 | 2 |
| RHS15/div2/mul1，性能优先 | 1021358 | 498.13 | 96054 | 2 |

因此推荐默认硬件探索点为 `RHS15/div1/mul1`：相对 RHS5 默认预处理减少 24.3%，
不增加昂贵 divider；性能优先时使用 `RHS15/div2/mul1`，再减少 9.4%，但 stall 已转移
到 mul/sub。表中 break-even 来自 1-sweep campaign，含单次热循环启动开销；长期模式
选择仍以第 30.8 节 16-sweep 测量的默认资源公平回本点 4 为准。

另有 1x17 full-validation 压力回归同时使用 single slot/RHS/mul-sub/output entry、
single bank，并关闭 partial output 和 LU forwarding。TRSV/PRE average cell latency 为
1411/3428 cycles，II 为 1579.75/3550.63，PRE 产生 306 个真实 output-ring stall，
两条路径 residual 仍通过。该回归曾发现 `DrainPending` 只尝试一次入队的问题；当前
实现会每周期重试尚未入队的 ready matrix，因此 depth=1 可形成背压并最终完成。

## 31. RHS15 scheduling and real CfdLocalSpm integration

本节记录第 30 节之后完成的最终实现。默认 event 探索点为
`RHS15/div1/mul1 + round-robin-ready + LU early-start`。RHS15 只实例化 15 份
`RhsState` 上下文；divider、mul、sub 仍是共享资源池，不是 15 套完整 TRSV datapath。
RHS 0..4/5..9/10..14 分别对应 I/L/U 的五列，并输出 Dinv/Lbar/Ubar。

### 31.1 四种确定性 scheduler

- `column-major`：按 RHS index 连续扫描，作为旧顺序回归。
- `step-major`：先比较 k，再比较 phase，最后比较 RHS index。
- `round-robin-ready`：从上次成功发射位置之后扫描第一个 ready RHS；当前默认。
- `ready-first`：divider-ready 优先于 mul/sub-ready，同类按 readySinceCycle 最老、
  RHS index 最小仲裁。

所有策略只改变跨 RHS 的选择顺序；单 RHS 内的 divide、multiply、subtract 次序和
FP64 操作数不变，非 ready RHS 不占资源。8x256 比较中四种策略的 dependency stall
均为 5298176，说明调度没有消除数学依赖；round-robin-ready 将 max wait 限制为
56 cycles，并将 PRE preprocess 降到 1080426 cycles（该组为 internal-SPM、early-start
前后的阶段性对比）。新增统计覆盖 scans/selections/no-ready、ready RHS/cycle、候选数、
平均/最大等待、starvation、三批 wait，以及 divider/mul/sub busy 和 idle。

### 31.2 LU step-ready 与同 cell overlap

Crout 第 k 列的 lower 部分和 pivot 完成后，`forwardStepReady[k]` 发布；此时 forward
step k 所需的 `LU[k][k]` 与 `LU[i][k], i>k` 已完整。随后生成第 k 行全部 upper 元素，
`NextColumn` 时才发布 `backwardStepReady[k]`，保证 backward step k 所需
`LU[k][i], i>k` 全部可用。TRSM 因而可以在后续 LU 列仍生成时使用已发布的 step，
而不是假设“一列完成等于全部 upper ready”。

`--lu-solve-early-start=0/1` 保留 full-LU-ready 对照。8x256 internal-SPM 中，PRE
preprocess 从 1086108 降到 991282 cycles，平均 cell latency 从 1967.22 降到
1699.62；2048 个 cell 均发生 early issue，同 cell LU/TRSM overlap=394202 cycles。
trace 提供 `LU_STEP_READY/TRSM_WAIT_LU_STEP/TRSM_EARLY_ISSUE`，统计提供五个 step 的
ready-to-first-use、blocked、early issued/missed 和同/跨 cell overlap。

### 31.3 CfdLocalSpm request/response 生命周期

`--coeff-spm-model=internal|packet` 保留两条路径。packet 模式调用唯一的
`CfdLocalSpm::issuePacket()`，与 Path A event read 共用 live outstanding、读/写端口
时间线和 bank 时间线。生命周期为：

```text
controller issue -> outstanding check -> port/bank arbitration
-> ACCEPT event -> START event -> RESPONSE event -> COMPLETE callback
-> input token 或 drain bytesDone -> 最终 heap commit
```

requester 分类为 PathA、Trsv5、Mrhs、CoeffInput、CoeffCompute、CoeffDrain。input 的
D/U 或 D/L/U 按 40/64B chunk 发出 write request，全部 callback 完成后才发布 input
token；drain 必须先完成真实共享 SPM read request，最后才用 `writeBlob` 提交 LU、
Dinv、Lbar、Ubar。1x1 中 TRSV 为 20/20 packets、400B write+400B read，PRE 为
35/35 packets、600B write+800B read。8x256 PRE 为 71680/71680 packets。

逐 bank 与逐 matrix vector stats 直接在仲裁结果上累计 access、conflict、total wait 和
max wait；另有 PathA/coeff、TRSV/coeff blocker 分类。single-bank、latency=4、并发端口
压力下 PRE 实测 595/595 packets、77 次 bank conflict、105 bank-wait cycles、最大
单次 3 cycles。outstanding=1 压力中 PRE 产生 510 次 full retry，仍最终完成。

这里的“packet”是 `CfdLocalSpm` SimObject 内共享的异步 request/response API 和
completion event，不是已连接 gem5 memory hierarchy RequestPort 的 `mem::Packet`。
第一阶段仍由 proxy capture 源数据到 request-local buffer，LU/TRSM compute read 也
读取该 buffer；最终 heap store 在 SPM response 后仍由 proxy 完成。这比控制器私有
计数器真实地引入了共享端口、bank、outstanding 和 callback，但不是完整 DMA/总线模型。

### 31.4 三种 SPM layout

`legacy` 保留 slot 内线性地址；`matrix-separated` 为 D/L/U/LU/Dinv/Lbar/Ubar 分配
独立 region，并错开 bank 起点；`row-striped` 将每个 40B row 映射到轮转 bank，packet
不会跨矩阵或行边界。8x256 packet 实测：

| layout | PRE preprocess | II | bank/outstanding stall |
|---|---:|---:|---:|
| legacy | 993106 | 484.66 | 3 / 3 |
| matrix-separated | 996322 | 486.24 | 0 / 0 |
| row-striped | 996322 | 486.24 | 1 / 1 |

layout 改善了 bank 均衡但没有改善总周期，证明默认阶段仍由 divider/dependency 主导；
因此 legacy 保持默认，另外两种用于更高 SPM 并发探索。

### 31.5 packet 下 div1/div2 与正式 16-sweep

最终 `8x256, interleave=4, sweeps=16, performance validation, packet legacy`：

| 项目 | TRSV div1 | PRE div1 | TRSV div2 | PRE div2 |
|---|---:|---:|---:|---:|
| preprocess | 888936 | 1000020 | 884838 | 935614 |
| fair extra | 0 | 111084 | 0 | 50776 |
| runtime/sweep | 1607415.50 | 1444217.88 | 1607415.50 | 1444217.88 |
| steady II | 432.80 | 487.98 | 430.80 | 456.51 |
| divider util | 0.539 | 0.874 | 0.329 | 0.662 |
| divider structural stall | 341200 | 3915543 | 117515 | 1239653 |
| mul/sub stall | 0 | 1472658 | 20045 | 2983983 |
| dependency stall | 2124786 | 5300225 | 2125824 | 5300426 |
| 16-sweep total | 26607584 | 24107506 | 26603486 | 24043100 |
| fair break-even | - | 1 | - | 1 |

div1 总加速为 1.1037x；div2 为约 1.1065x。第二 divider 将 PRE preprocess 再降低
64406 cycles，但 dependency 不降、mul/sub stall 明显上升，且 16-sweep 总收益仅再增
约 0.25%。所以 div1 是面积/性能默认点，div2 只推荐给高频系数重建的性能优先配置。

### 31.6 动态 auto、cancel 与 software-LU/event-TRSM

`--lusgs-preprocess-auto=1 --lusgs-coeff-expected-sweeps=N` 使用两个独立校准轮测量
当前配置的 TRSV/PRE preprocess 和 runtime/sweep，动态计算 fairExtra、savingPerSweep
和 ceil break-even，再执行一个选中正式整轮；同一轮内不混用。RHS1 测得 break-even=16、
expected=1 选择 TRSV；默认 RHS15 测得 break-even=1、expected=16 选择 PRE，两者正式
轮均 mismatch=0。旧参数名 `--lusgs-pretransform-auto/--lusgs-expected-sweeps` 保留别名。

独立 `cfd_coeff_cancel token` 指令返回 Cancelled=9、AlreadyComplete=10、
CancelPending=11，并复用 BadToken=5。solve-active 压力 trace 中 cancel 位于
`TRSM_EARLY_ISSUE` 之后，返回 CancelPending；wait 返回 Cancelled，reap 后再次 cancel
返回 BadToken，四个输出矩阵保持未改写。已提交 SPM event 可以自然返回，但 callback
找不到已取消 request，不再更新 architectural output。
同一 line/cell 以相同 generation 再次 launch 返回 GenerationMismatch=7，且同样不写
输出，证明 cancel/reap 没有绕过 generation 单调性。

event 模式现在也允许 `--lu5-model=software`。软件在预处理计时区间内按原
`cfd_lu5_factor()` 生成 packed LU，descriptor 以 `PACKED_LU_INPUT` 标志传入；packet
input 仍执行，控制器跳过 event LU，仅运行统一 event TRSM/drain。1x17 中 software LU
约 4880 cycles，event LU active=0；TRSV/PRE packet 分别 340/340、595/595，mismatch=0。
1x1、1x17、8x64 均通过，因无 event LU，same-cell LU/TRSM overlap 按定义为 0。

### 31.7 最终回归与剩余限制

最终回归通过 Path A、Path B、Path C、standalone TRSV5、decode-exclusive、Step1、
Step2、Step3 和 Step4-C。另通过 full-validation 1x1/1x17/8x64、三 layout、auto 两向、
cancel-during-solve，以及 single slot/bank/RHS/mul-sub/output entry、outstanding=1、
partial-output=0 的组合压力。

仍未实现的物理边界是：真正的 gem5 RequestPort/mem::Packet、逐元素 compute SPM read、
异步 heap store engine、可中断正在执行的单个 FP event，以及 Path A/TRSV/coeff 在同一
端到端宏控制器内的并发 traffic 生成。当前 conflict source stats 已就绪，但只有这些
requester 真正同时发出请求时才会产生非零跨单元冲突。
