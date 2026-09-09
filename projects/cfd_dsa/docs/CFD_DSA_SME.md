# CFD-DSA Path C：ZA 阵列外积式空间 Pipeline 技术文档

版本：v2.1 Path C local-event 长跑与 outer FU 统计收敛

本文只记录第二种方法 Path C。第一种方法 Path A 的 dotp-row 细节见 `CFD_DSA_IMPLEMENTATION.md`，Path B 的 ZA partial-sum pipeline 细节见 `CFD_DSA_PATHB_ZA_PIPE.md`。

## 1. 概述

Path C 已从旧的固定 `ZA[:,0]` 累加路径切换为新的 ZA 阵列外积式空间 pipeline：

```text
lmat5_spm -> Z input buffer
ZERO {ZA}
5 x cfdsme_za_outer5_step
MOVA ZA[:,4] -> Z result
st1d result
```

旧 Path C：

```text
cfdsme_fmopa5_step:
    ZA[0:4,0] += A[:,k] * x[k]
```

已从默认 decoder 和推荐 benchmark 中删除。旧编码 guard `[17:15]=101` 默认返回 `Unknown64`，旧测试源文件 `projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_sme_step_perf.c` 只保留为兼容 wrapper，实际包含新的 `projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c`。

新 Path C 的目标是保留 PathB 已验证的外部调度和 6 cycles/matvec 下界，同时用 Path C 自己的 “selected-column outer-product” 指令语义描述 ZA 阵列数据流。

## 2. 文件总览

| 文件 | 作用 |
| --- | --- |
| `projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c` | Path C 推荐 benchmark |
| `build/cfd_dsa/aarch64/test_cfd_pathc_za_outer_arm` | Path C 推荐 binary |
| `projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_sme_step_perf.c` | deprecated compatibility wrapper |
| `build/cfd_dsa/aarch64/test_cfd_pathc_sme_step_arm` | deprecated binary 名，可兼容编译新 Path C |
| `projects/cfd_dsa/configs/run_cfd_pathc.py` | Path C 独立运行入口 |
| `projects/cfd_dsa/configs/run_cfd_dsa.py` | deprecated / compatibility wrapper 和共享 gem5 配置 |
| `src/arch/arm/insts/cfd_dsa.hh/.cc` | `DSACFDSMEZaOuter5Step` execute/disassembly |
| `src/arch/arm/isa/formats/custom_cfd.isa` | Path C outer decoder |
| `src/arch/arm/cfd_local_spm.hh/.cc` | local SPM、ZA rename/MOVA override、Path C outer stats |
| `src/cpu/FuncUnit.py` | `CFDSMEZaOuterStep` OpClass |
| `src/cpu/op_class.hh` | `CFDSMEZaOuterStepOp` alias |
| `src/cpu/o3/FuncUnitConfig.py` | `CFD_SME_ZA_OUTER_Engine` |
| `src/cpu/o3/FUPool.py` | O3 FU pool wiring |
| `src/cpu/o3/commit.cc` | Path C outer commit/squash hook |

## 3. 指令集定义与编码

新增 Path C 自定义计算指令：

```text
cfdsme_za_outer5_step zcol, zvec, #k
```

编码：

```text
[31:27] = 00000
[26:25] = 00
[24:23] = 11
[22]    = 1
[21]    = 0          // Path C outer
[20:18] = k          // 0..4
[17:15] = 110        // Path C guard
[14:10] = zcol
[9:5]   = zvec
[4:0]   = 0
```

非法条件：

```text
k > 4                  -> Unknown64
[17:15] != 110         -> Unknown64
[4:0] != 0             -> Unknown64
旧 cfdsme_fmopa5_step  -> Unknown64 by default
```

OpClass / FU：

```text
OpClass = CFDSMEZaOuterStep
FU      = CFD_SME_ZA_OUTER_Engine
default opLat = 1
default pipelined = True
default count = 1
```

运行参数：

```text
--sme-outer-lat=<n>
--sme-outer-count=<n>
--sme-outer-pipelined=<true|false>
--sme-outer-full-array=<true|false>
```

默认 `--sme-outer-full-array=false`。这只是保留未来完整外积阵列模型的标签，当前默认硬件仍只更新 selected column。

## 4. 执行语义

输入：

```text
zcol[0:4] = A[:,k]
zvec[0:4] = x[0:4]
k in 0..4
```

概念外积：

```text
outer[i][j] = zcol[i] * zvec[j]
```

默认有效更新为 selected-column：

```text
scalar = zvec[k]

if k == 0:
    ZA[0:4,0] = zcol[0:4] * scalar
else:
    ZA[0:4,k] = ZA[0:4,k-1] + zcol[0:4] * scalar
```

最终：

```text
MOVA reads ZA[0:4,4] -> Z result
st1d stores first 5 FP64 lanes
```

禁止路径：

```text
不写 z31..z27 partial sums
不使用 cfdsme_reduce5
不恢复 smemvm_prefetch5_spm / smemvm_exec5
不恢复旧固定 ZA[:,0] 累加 Path C
```

## 5. 数据流

SPM 仍采用 A 转置布局：

```text
A_T_BASE + 0 * 40B: A[:,0]
A_T_BASE + 1 * 40B: A[:,1]
A_T_BASE + 2 * 40B: A[:,2]
A_T_BASE + 3 * 40B: A[:,3]
A_T_BASE + 4 * 40B: A[:,4]
VEC_BASE:            x[0:4]
```

单个 matvec：

```text
6 x lmat5_spm = 240B logical SPM read
5 x cfdsme_za_outer5_step = 25 MAC = 50 FLOPs
1 x ZERO {ZA}
1 x MOVA ZA[:,4] -> Z
1 x predicated st1d = 40B result store
```

推荐调度与 PathB 保持同形：

```text
lmat5_spm z5, [vec]
lmat5_spm z0, [A_T + 0*40]
lmat5_spm z1, [A_T + 1*40]

ZERO {ZA}
outer z0, z5, #0
lmat5_spm z0, [A_T + 2*40]
outer z1, z5, #1
lmat5_spm z1, [A_T + 3*40]
outer z0, z5, #2
lmat5_spm z0, [A_T + 4*40]
outer z1, z5, #3
outer z0, z5, #4
MOVA ZA[:,4] -> z11
st1d z11
```

本轮调度进一步强调 column-ready 启动：`outer0` 只等待 vector、当前
column0 和 ZERO/ZA token；`outer1` 只等待 column1 和 `outer0` token，
不再等待 column2..4 全部完成。Ping/Pong 之间仍保持单 ZA accumulator
边界，不交错两个 MVM 的 ZA accumulate。

## 6. ZA rename 与 MOVA override

Path C 使用 `CfdLocalSpm` 的 ZA rename / pending model：

```text
recordZaOuterExecute(seqNum, k, col, scalar)
recordZaCommit(seqNum)
recordZaSquash(seqNum)
readZaPathCOuterFinalCol(dst, lanes)
```

Path C outer execute 会记录：

```text
pathCOuterSteps
pathCOuterPendingUpdates
pathCOuterCol0Writes ... pathCOuterCol4Writes
pathCOuterForwardHits
pathCEarlyOuterStartEvents
pathCStepIssuedBeforeAllColumnsReady
pathCLoadOuterOverlapCycles
pathCDualLoadIssueCycles
```

MOVA override 按环境变量隔离：

```text
GEM5_CFD_PATHB_ENABLE=1        -> read ZA[:,4] for PathB
GEM5_CFD_PATHC_OUTER_ENABLE=1  -> read ZA[:,4] for PathC outer
otherwise                      -> legacy read ZA[:,0]
```

`projects/cfd_dsa/configs/run_cfd_pathc.py` 默认设置 `GEM5_CFD_PATHC_OUTER_ENABLE=1`。因此 PathC 的 MOVA 读取 final column，不再读取旧 `ZA[:,0]`。

## 7. local-event SPM 模型

Path C 推荐默认 local-event 配置：

```text
--spm-access-mode=local-event
--local-spm-lat=1
--local-spm-read-ports=2
--local-spm-read-width=40
--local-spm-outstanding=8
--local-spm-queue-size=8
--local-spm-banks=8
--local-spm-bank-granularity=40
--local-spm-bank-mapping=xor
--local-spm-z-wb-ports=2
--za-state-mode=rename
```

该配置表示 Path C 的理想 local-event 条件：local SPM 事件、coverage、queue/bank/port stats 可见，同时保持接近 6 cycles/matvec 的性能目标。

## 8. projects/cfd_dsa/configs/run_cfd_pathc.py

推荐运行：

```bash
aarch64-linux-gnu-gcc -static -O2 -march=armv8-a+sve \
    -o build/cfd_dsa/aarch64/test_cfd_pathc_za_outer_arm projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c

./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_pathc.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_pathc_za_outer_arm \
    --cpu=o3 \
    --bench-iters=10000
```

旧入口：

```text
projects/cfd_dsa/configs/run_cfd_dsa.py:
    deprecated / compatibility wrapper

projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_sme_step_perf.c:
    deprecated compatibility source; includes projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c
```

## 9. benchmark correctness

`projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c` 使用：

```text
Ping matrix != Pong matrix
Ping vector != Pong vector
非对称 row-major reference matrix
SPM 内部存 A^T / column-major
reference C 仍计算 y[i] = sum_k A[i][k] * x[k]
```

期望输出：

```text
Ping/Pong PathC ZA-outer MVM: PASS
Benchmark PathC ZA-outer MVM: PASS
Overall: PASS
```

## 10. 性能结果与统计口径

当前 local-event 复测结果：

```text
bench-iters                = 10,000
cycles/matvec              = 6.02
IPC                        = 2.5766
effective FLOPs/cycle      = 8.310
committed_CFDDSAMatLd      = 60,003
committed_CFDSMEZaOuterStep= 50,000
ZERO ZA                    = 10,000
MOVA ZA->Z                 = 10,000
Path C final reads         = 10,000
local_spm coverage         = 100.00%
local_spm logical bytes    = 2,400,120
local_spm physical bytes   = 2,400,120
local_spm avg latency      = 1.12 cycles
local_spm port/bank/queue  = 383 / 6,276 / 354 cycles
early outer starts         = 30,028
before all columns         = 30,028
dual-load cycles           = 23,759
Path C outer FU issue/busy = 50,000 / 50,000
Path C outer FU util       = 47.78%
nonSpecInstsAdded          = 3
Overall                    = PASS
```

本轮同时尝试 50K local-event 长跑；由于 request/event 统计 wall-time 开销
过大，交互验证中断，没有作为正式结果写入表格。上表采用 10K 可复现实测点，
用于确认新的 column-ready 调度、local-event 统计和 outer FU 口径。

推导：

```text
matvecs = committed_CFDSMEZaOuterStep / 5
effective_MACs = committed_CFDSMEZaOuterStep * 5
effective_FLOPs = effective_MACs * 2
logical_SPM_read_bytes = committed_CFDDSAMatLd * 40
result_store_bytes = matvecs * 40
```

理论下界：

```text
5 outer steps at token latency 1   = 5 cycles
local SPM bandwidth, 2 read ports  = ceil(6 lmat / 2) = 3 cycles
explicit lmat instructions         = 6 per matvec
steady schedule / forwarding bound = 6 cycles/matvec
```

这里的 6 cycles/matvec 不是单纯的 SPM bandwidth 下界，而是当前显式
`lmat5_spm + outer step + MOVA/store` 调度在单 outer engine、单 ZA forwarding
链下的 steady schedule bound。当前 Path C outer 10K local-event 复测达到
6.02 cycles/matvec，接近该下界。

短 trace 可显示 column-ready 后外积 step 直接进入计算：

```text
44471 lmat5_spm field0 column_ready
44471 lmat5_spm field5 vector_ready
44486 za_outer_step k0 execute_before_all_columns
44549 za_outer_step k1 execute_before_all_columns
44551 lmat5_spm field2/3 column_ready
44552 za_outer_step k2 execute_before_all_columns
44553 za_outer_step k3 execute_before_all_columns
44554 za_outer_step k4 execute_before_all_columns
```

trace 命令：

```bash
./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_pathc.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_pathc_za_outer_arm \
    --cpu=o3 \
    --bench-iters=1000 \
    --cfd-trace-enable \
    --cfd-trace-matvecs=4 \
    --cfd-trace-file=results/cfd_dsa/runs/pathc_micro_trace.csv
```

## 11. 与 PathB 的边界

PathB 仍保留原有外部语义、benchmark 和 run 脚本：

```text
PathB instruction: cfdsme_mvm5_pipe_step
PathB runner:      projects/cfd_dsa/configs/run_cfd_pathb.py
PathB benchmark:   projects/cfd_dsa/benchmarks/pathb/test_cfd_pathb_za_pipe_perf.c
```

PathC 使用独立指令：

```text
PathC instruction: cfdsme_za_outer5_step
PathC runner:      projects/cfd_dsa/configs/run_cfd_pathc.py
PathC benchmark:   projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c
```

二者都可读 `ZA[:,4]`，但由不同环境变量和 stats 路径隔离。

## 12. 已知问题与后续

1. O3 通用 FU busy breakdown 中 `outer FU busy` 仍可能为 0；Path C 现在使用 `Path C local outer FU issue/busy/util` 作为专用统计口径。
2. `--sme-outer-full-array=true` 目前只是 future/full-array label，不是默认硬件路径。
3. 旧 `cfdsme_fmopa5_step` 类保留在 C++ 兼容区，但 decoder 默认不再接受旧 PathC 编码。
4. 不再追求低于 6 cycles/matvec；后续只做统计清理、trace 和文档收敛。

## 13. 验证命令摘要

```bash
scons build/ARM/gem5.opt -j2

aarch64-linux-gnu-gcc -static -O2 -march=armv8-a+sve \
    -o build/cfd_dsa/aarch64/test_cfd_pathc_za_outer_arm projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c

./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_pathc.py \
    --cpu=o3 \
    --bench-iters=10000
```

已回归：

```text
PathC outer 10K: PASS, 6.02 cycles/matvec
PathA stream 50K: PASS, 5.52 cycles/matvec
```
