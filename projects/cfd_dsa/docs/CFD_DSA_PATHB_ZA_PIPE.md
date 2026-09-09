# CFD-DSA Path B：SME-ZA 空间 partial-sum pipeline 技术文档

版本：v1.1 Path B local-event basic 默认真实性模型收敛

本文档只记录 Path B。第一种方法 Path A 的 dotp-row 技术细节见
`CFD_DSA_IMPLEMENTATION.md`；Path C 的 ZA outer pipeline
技术细节见 `CFD_DSA_SME.md`。

## 1. 目标

Path B 保留 Path C 的外层执行框架：

```text
lmat5_spm -> Z input buffer
ZERO {ZA}
5 x custom ZA step
MOVA ZA -> Z result
st1d result
```

区别在于 ZA 的使用方式。Path C 把完整结果累加在 `ZA[0:4,0]`。Path B
把 ZA 的 5 个 column 作为空间 partial-sum pipeline：

```text
ZA[0:4,0] = A[:,0] * x[0]
ZA[0:4,1] = ZA[0:4,0] + A[:,1] * x[1]
ZA[0:4,2] = ZA[0:4,1] + A[:,2] * x[2]
ZA[0:4,3] = ZA[0:4,2] + A[:,3] * x[3]
ZA[0:4,4] = ZA[0:4,3] + A[:,4] * x[4]
MOVA 读取 ZA[0:4,4]
```

中间 partial sum 始终留在 ZA 内部。Path B 不使用 Z 寄存器保存 partial
sum，不使用 `cfdsme_reduce5`，也不恢复 `smemvm_prefetch5_spm` 或
`smemvm_exec5`。

## 2. 文件总览

```text
projects/cfd_dsa/configs/run_cfd_pathb.py
projects/cfd_dsa/benchmarks/pathb/test_cfd_pathb_za_pipe_perf.c
build/cfd_dsa/aarch64/test_cfd_pathb_za_pipe_arm

src/arch/arm/insts/cfd_dsa.hh
src/arch/arm/insts/cfd_dsa.cc
src/arch/arm/isa/formats/custom_cfd.isa
src/arch/arm/cfd_local_spm.hh
src/arch/arm/cfd_local_spm.cc
src/arch/arm/isa/insts/sme.isa
src/cpu/FuncUnit.py
src/cpu/op_class.hh
src/cpu/o3/FuncUnitConfig.py
src/cpu/o3/FUPool.py
src/cpu/o3/commit.cc
```

Path B 通过独立 runner 选择：

```text
GEM5_CFD_RUN_WRAPPER=pathb
GEM5_CFD_REPORT_FOCUS=pathb
GEM5_CFD_PATHB_ENABLE=1
```

如果没有设置 `GEM5_CFD_PATHB_ENABLE`，MOVA 保持 Path C 行为，读取
`ZA[0:4,0]`。

## 3. 指令定义

Path B 新增一条自定义 step 指令：

```text
cfdsme_mvm5_pipe_step zcol, zvec, #k
```

编码：

```text
[31:27] = 00000
[26:25] = 00
[24:23] = 11
[22]    = 1
[21]    = 1
[20:18] = k       // 0..4
[17:15] = 110     // Path B pipe-step guard
[14:10] = zcol
[9:5]   = zvec
[4:0]   = 0
```

非法编码返回 `Unknown64`。其中 `k > 4` 必须非法。

执行语义：

```text
scalar = zvec[k]

if k == 0:
    ZA[i][0] = zcol[i] * scalar
else:
    ZA[i][k] = ZA[i][k - 1] + zcol[i] * scalar

i = 0..4
```

每条 step 做 5 个有效 FP64 MAC 等价更新。一个完整 MVM 需要 5 条 step，
总计 25 MAC = 50 effective FLOPs。

## 4. Benchmark 数据布局

SPM 布局沿用 SME column-streaming 的 A^T 格式：

```text
A_T_BASE + 0 * 40B = A[:,0]
A_T_BASE + 1 * 40B = A[:,1]
A_T_BASE + 2 * 40B = A[:,2]
A_T_BASE + 3 * 40B = A[:,3]
A_T_BASE + 4 * 40B = A[:,4]
VEC_BASE            = x[0:4]
```

C reference 仍从 row-major `A` 计算：

```text
y[i] = sum_k A[i][k] * x[k]
```

这样可以检测 row-major / column-major 混淆问题。

## 5. Kernel 调度

benchmark 使用两个 Z column buffer 和一个 Z vector buffer：

```text
lmat5_spm z5, [vec_base, row=0]
lmat5_spm z0, [A_base, row=0]
lmat5_spm z1, [A_base, row=1]

ZERO {ZA}

cfdsme_mvm5_pipe_step z0, z5, #0
lmat5_spm z0, [A_base, row=2]

cfdsme_mvm5_pipe_step z1, z5, #1
lmat5_spm z1, [A_base, row=3]

cfdsme_mvm5_pipe_step z0, z5, #2
lmat5_spm z0, [A_base, row=4]

cfdsme_mvm5_pipe_step z1, z5, #3
cfdsme_mvm5_pipe_step z0, z5, #4

MOVA ZA[0:4,4] -> z11
st1d z11 -> result
```

结果写回仍使用 predicated 40B store。`SMSTART/SMSTOP` 放在 hot matvec
循环外部，不在每个 matvec 内重复进入/退出 SME mode。

## 6. O3、ZA 状态与 local-event basic

Path B 有独立 OpClass：

```text
CFDSMEPipeStep
```

默认 FU 模型：

```text
CFD_SME_PIPE_Engine:
    opLat = 1
    pipelined = True
    count = 1
```

该模型表示一个 ZA 空间 partial-sum pipeline。默认 `count=1`，表示单条
5-lane ZA partial-sum pipeline，每 cycle 最多接收一个 k-step；它不是
25-MAC/cycle 的全展开阵列。

ZA 状态通过现有 local SPM / ZA helper state 记录。Path B 新增统计：

```text
pathBPipeSteps
pathBPipeCommitUpdates
pathBPipeSquashDiscards
pathBPipeCol0Writes ... pathBPipeCol4Writes
pathBPipeFinalReads
pathBPipeForwardHits
pathBPipeForwardStalls
```

O3 commit / squash hook 识别 `CFDSMEPipeStep`。因此 Path B 有独立
committed / squashed 统计，同时不改变 Path A 或 Path C 的执行路径。

Path B 默认使用 `local-event basic`，而不是 `local-ideal`：

```text
spm-access-mode          = local-event
local-spm-lat            = 1
local-spm-read-ports     = 2
local-spm-read-width     = 40B
local-spm-outstanding    = 8
local-spm-queue-size     = 8
local-spm-banks          = 8
local-spm-bank-granularity = 40B
local-spm-bank-mapping   = row
local-spm-z-wb-ports     = 2
za-state-mode            = rename
sme-pipe-lat             = 1
sme-pipe-count           = 1
```

该配置的硬件含义：

```text
2 个 40B local SPM read port:
    给 O3 clustered lmat issue 留出 arbitration slack。

8 banks + row mapping:
    让 x vector 与 A[:,0]..A[:,4] 的 logical rows 分散到不同 bank。

queue/outstanding = 8:
    能容纳一个 matvec 的 6 条 lmat 以及少量跨迭代 overlap。

2 个 Z writeback ports:
    避免 lmat response 与 MOVA / vector writeback 完全串行化。
```

`local-event basic` 会记录 request queue、read port、bank、SRAM latency、
Z writeback 和 event lifecycle 统计。当前 O3-visible timing 仍通过
`lmat5_spm` FU latency 近似表达：basic 配置使用 `lmat5_spm opLat=2`，
stress 配置通过更小 queue/outstanding、单 bank 或更少 ZWB port 引入额外
latency / 非流水行为。也就是说，basic 已经不再是纯 local-ideal bypass，但还
不是逐 request 动态唤醒的完整 SRAM 事件队列模型。

## 7. 编译与运行命令

编译：

```bash
scons build/ARM/gem5.opt -j2

aarch64-linux-gnu-gcc -static -O2 -march=armv8-a+sve \
    -o build/cfd_dsa/aarch64/test_cfd_pathb_za_pipe_arm projects/cfd_dsa/benchmarks/pathb/test_cfd_pathb_za_pipe_perf.c
```

Path B 默认 local-event basic 运行命令：

```bash
./build/ARM/gem5.opt -d results/cfd_dsa/runs/pathb_local_event_basic_5k \
    projects/cfd_dsa/configs/run_cfd_pathb.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_pathb_za_pipe_arm \
    --cpu=o3 \
    --bench-iters=5000
```

展开后的关键默认参数：

```bash
--spm-access-mode=local-event
--local-spm-lat=1
--local-spm-read-ports=2
--local-spm-read-width=40
--local-spm-outstanding=8
--local-spm-queue-size=8
--local-spm-banks=8
--local-spm-bank-granularity=40
--local-spm-bank-mapping=row
--local-spm-z-wb-ports=2
--za-state-mode=rename
--sme-pipe-lat=1
--sme-pipe-count=1
```

local-ideal 上限对照命令：

```bash
./build/ARM/gem5.opt -d results/cfd_dsa/runs/pathb_local_ideal_5k_after_basic \
    projects/cfd_dsa/configs/run_cfd_pathb.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_pathb_za_pipe_arm \
    --cpu=o3 \
    --bench-iters=5000 \
    --spm-access-mode=local-ideal \
    --local-spm-lat=1 \
    --local-spm-read-width=40 \
    --za-state-mode=rename \
    --sme-pipe-lat=1 \
    --sme-pipe-count=1
```

stress 配置只用于证明反压模型有效，不作为默认性能结论。

## 8. 正确性

预期 benchmark 输出：

```text
Ping/Pong PathB ZA-pipe MVM: PASS
Benchmark PathB ZA-pipe MVM: PASS
Overall: PASS
```

测试数据要求：

```text
Ping/Pong 矩阵非对称
Ping/Pong vector 不同
reference 使用 row-major C MVM
SPM 使用 A^T column-major 布局
只有 GEM5_CFD_PATHB_ENABLE=1 时，MOVA 才读取 ZA[:,4]
```

## 9. 实测结果与统计口径

benchmark 用 `m5_reset_stats()` 和 `m5_dump_stats()` 包围核心 kernel。下面表格
使用 hot-region stats dump。

| 运行 | Matvecs | Cycles | Cycles/matvec | IPC | Effective FLOPs/cycle |
| --- | ---: | ---: | ---: | ---: | ---: |
| local-event basic 默认 | 5,000 | 30,169 | 6.034 | 2.5701 | 8.287 |
| local-ideal upper-bound | 5,000 | 30,169 | 6.034 | 2.5701 | 8.287 |
| local-ideal upper-bound 50K | 50,000 | 300,174 | 6.003 | 2.5820 | 8.329 |
| Path B small correctness | 100 | 771 | 7.710 | 2.0584 | 6.485 |

local-event basic 5K 关键计数：

```text
committed_CFDDSAMatLd       = 30,003
committed_CFDSMEPipeStep    = 25,000
local_spm.logicalBytesRead  = 1,200,120
local_spm.physicalBytesRead = 1,200,120
local_spm.numReads          = 30,003
local_spm.coverage          = 100.00%
local_spm.avgReadLatency    = 1.00 cycles
local_spm.maxReadLatency    = 1 cycle
portConflictCycles          = 0
bankConflictCycles          = 0
queueFullCycles             = 0
zWritebackConflictCycles    = 0
PathB col0..col4 writes     = 5,000 each
PathB final reads           = 5,000
PathB forward hits          = 25,000
PathB forward stalls        = 0
```

理论下界：

```text
每 matvec 有 6 条 lmat5_spm issue slot
每 matvec 有 5 条 ZA pipe step
combined lower bound = max(6, 5) = 6 cycles/matvec
```

local-event basic 实测 6.034 cycles/matvec，基本贴近 6-cycle lower bound。

stress 结果：

| 配置 | Matvecs | Cycles/matvec | IPC | 主要事件 |
| --- | ---: | ---: | ---: | --- |
| port/latency check: `read_ports=1, lat=3` | 1,000 | 6.17 | 2.5165 | `avgReadLatency=3`，无结构冲突，说明单流水 read port 仍可维持吞吐 |
| bank stress: `banks=1, lat=2` | 1,000 | 12.16 | 1.2779 | `bankConflictCycles=6,009` |
| queue/outstanding stress: `queue=1, outstanding=1, lat=3` | 1,000 | 36.12 | 0.4301 | `queueFullCycles=12,006`，`portConflictCycles=12,006` |
| ZWB stress: `z_wb_ports=1` | 1,000 | 6.17 | 2.5182 | `zWritebackConflictCycles=41,891`，当前被 O3 overlap 隐藏 |

这些 stress 不是默认性能结论。它们用于证明 local-event 统计能够观察到
read port、bank、queue/outstanding 和 Z writeback 的反压风险。其中 bank 与
queue/outstanding stress 已经显著拉高 cycles/matvec。

## 10. 回归与隔离边界

Path B 与已有路径隔离：

```text
Path A 使用 CFDDSADotp / pack_acc，不使用 CFDSMEPipeStep。
Path C 使用独立的 CFDSMEZaOuterStep，不使用 CFDSMEPipeStep。
只有 GEM5_CFD_PATHB_ENABLE=1 时，MOVA 才读取 ZA[:,4]。
```

新增 Path B 后的回归结果：

| Path | 命令族 | 结果 |
| --- | --- | --- |
| Path A | `projects/cfd_dsa/configs/run_cfd_patha.py --bench-iters=100` | `Overall: PASS` |
| Path C | `projects/cfd_dsa/configs/run_cfd_pathc.py --bench-iters=100` | `Overall: PASS` |
| Path B | `projects/cfd_dsa/configs/run_cfd_pathb.py --bench-iters=5000` | `Overall: PASS`, 6.034 cycles/matvec |

## 11. 解释与定位

Path B 不是 Path A 或 Path C 的替代品。

```text
Path A:
    dotp-row 性能路径。

Path C:
    ZA outer pipeline 语义路径，完整说明见 CFD_DSA_SME.md。

Path B:
    ZA spatial partial-sum pipeline。
    中间 partial sum 仍留在 ZA 内部，但分布在 ZA columns 0..4。
```

Path B 证明：在 tightly-coupled local SPM、row-mapped 8-bank layout、
2x40B read port、2 个 Z writeback port 与 single-cycle ZA column forwarding
pipeline 条件下，5x5 MVM 可以在不使用 Z-register partial sum 的情况下贴近
6-cycle issue lower bound。

## 12. 已知限制

1. 当前默认 local-event basic 的完成实测是 5K hot-region；50K local-event
   在当前工具会话中出现空 stats/session 残留，因此未作为正式表格口径。
2. 当前实测使用 40B physical read，与 compact logical lmat traffic 对齐；
   64B physical SRAM datapath 可作为后续 sweep。
3. local-event basic 的逐请求 event lifecycle 已可统计；O3-visible wakeup 仍
   由 `lmat5_spm` FU latency 近似承载，而不是每条 request 动态回写 ready time。
4. 如果研究 SRAM port、bank、queue 或 Z writeback conflict，应使用 stress
   配置并结合对应 stats。
5. Path B 改变了 ZA 使用方式，因此不同于 Path C 的 strict `ZA[:,0]`
   accumulator 语义。
