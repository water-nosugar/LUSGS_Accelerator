# CFD DSA TRSV5 Step2 Report

本文档记录 LU-SGS Step2 引入的独立 FP64 5x5 TRSV 加速指令。它只覆盖
TRSV5 本身；LU-SGS 端到端结果见 `CFD_DSA_LUSGS_STEP2.md`。

2026-07-10 新增的 `trsm5_mrhs_spm` 是独立可选的系数预处理单元。它逐列复用
本文档规定的精确 TRSV5 运算顺序，一条指令处理 5 个 RHS；它没有删除、替换或
改写原 `trsv5_lu_spm` / `trsv5_lu_spm_zrhs`。完整设计和测试见
`CFD_DSA_LUSGS_PRETRANSFORM.md`。

## 1. 目标与边界

- 新增一条独立 TRSV5 指令/FU/统计/test，不改变 Path A 底层 MVM 实现。
- 输入为 SPM 中 5x5 LU 矩阵和 5 元 RHS，输出到一个 Z 寄存器的 lane 0..4。
- Step2 不实现宏控制器、DMA、调度器、融合流水线或跨 cell 自动推进。
- `trsv5_lu_spm` 的当前模型是 `zreg-coarse`：数值在 execute 中按固定顺序算出，
  O3 可见延迟由 FU `opLat` 给出，默认 60 cycles。

## 2. 数学语义

TRSV5 严格使用以下 FP64 顺序。没有倒数替代、没有 reciprocal multiply、没有
重排，也不允许编译器把 multiply-subtract 收缩为 FMA。

```c
copy rhs -> value
for k=0..3:
    value[k] /= lu[k][k]
    for i=k+1..4:
        value[i] -= lu[i][k] * value[k]
value[4] /= lu[4][4]
for k=3..0:
    for i=k+1..4:
        value[k] -= lu[k][i] * value[i]
copy value -> result
```

每次 solve 统计为 5 次 divide、10 次 forward multiply-subtract、10 次 backward
multiply-subtract，总计 20 次 multiply-subtract。

## 3. ISA/FU/统计实现

新增的主要代码点：

- `src/cpu/FuncUnit.py`：新增 `CFDDSATrsv5` OpClass。
- `src/cpu/op_class.hh`：新增 `CFDDSATrsv5Op`。
- `src/cpu/o3/FuncUnitConfig.py`：新增 `CFD_DSA_TRSV5_Engine`，默认
  `opLat=60, pipelined=True`。
- `src/cpu/o3/FUPool.py`：默认 O3 FU pool 加入 TRSV5 engine。
- `src/arch/arm/insts/cfd_dsa.hh/.cc`：新增 `DSATrsv5LuSPM`。
- `src/arch/arm/isa/formats/custom_cfd.isa`：新增 TRSV5 decode，并用
  `x21..x24` 基址寄存器 guard 避免和 Path A `pack_acc` 旧编码冲突。
- `src/arch/arm/cfd_local_spm.hh/.cc`：新增 `recordTrsv5Execute()` 和 TRSV5 stats。

当前 encoding 约定：

```text
[24:23]=11 [22]=0 [21:20]=00 [19:15]=xlu [14:10]=xrhs [9:5]=00000 [4:0]=zd
```

其中 `xlu/xrhs` 当前测试使用 `x21/x23` 等高号基址寄存器，避免踩到 Path A
既有编码空间。

## 4. 统计字段

`system.cpu.local_spm` 下新增：

```text
trsv5Issued
trsv5Completed
trsv5Squashed
trsv5BusyCycles
trsv5IdleCycles
trsv5FullStallCycles
trsv5DivOps
trsv5ForwardMulSubOps
trsv5BackwardMulSubOps
trsv5TotalMulSubOps
trsv5InputMatrixBytes
trsv5InputVectorBytes
trsv5OutputBytes
trsv5LatencyConfigured
trsv5ObservedLatencyTotal
trsv5AverageObservedLatency
trsv5MaxInFlight
```

当前实现输出到 Z 寄存器，`OutputBytes=40` 表示每次 solve 产生 5 个 FP64 lane，
不是额外的 SPM result-buffer 写回。

512 solve、lat=60 的关键 stats：

```text
trsv5Issued                512
trsv5Completed             512
trsv5Squashed              0
trsv5BusyCycles            30720
trsv5DivOps                2560
trsv5ForwardMulSubOps      5120
trsv5BackwardMulSubOps     5120
trsv5TotalMulSubOps        10240
trsv5InputMatrixBytes      102400
trsv5InputVectorBytes      20480
trsv5OutputBytes           20480
trsv5LatencyConfigured     60
trsv5AverageObservedLatency 60
trsv5MaxInFlight           1
```

## 5. 构建与运行

```bash
scons -Q build/ARM/gem5.opt -j2

aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -march=armv8-a+sve \
    projects/cfd_dsa/benchmarks/trsv5/test_cfd_trsv5.c -o build/cfd_dsa/aarch64/test_cfd_trsv5_arm -lm

build/ARM/gem5.opt -d results/cfd_dsa/runs/trsv5_512_rerun \
    projects/cfd_dsa/configs/run_cfd_trsv5.py --trsv5-solves=512 --max-ticks=20000000000
```

## 6. 独立 TRSV5 结果

默认配置为 `trsv5_count=1, trsv5_lat=60, trsv5_mode=zreg-coarse`。

| solves | cycles | cycles/solve | max_abs | max_rel | mismatch | result |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 722 | 722.000000 | 0 | 0 | 0 | TRSV5_PASS |
| 17 | 6300 | 370.588235 | 0 | 0 | 0 | TRSV5_PASS |
| 512 | 185028 | 361.382812 | 0 | 0 | 0 | TRSV5_PASS |
| 50000 | 20374532 | 407.490640 | 0 | 0 | 0 | TRSV5_PASS |

512 solve latency sweep：

| trsv5_lat | cycles | cycles/solve | result |
|---:|---:|---:|---|
| 20 | 164548 | 321.382812 | TRSV5_PASS |
| 40 | 174788 | 341.382812 | TRSV5_PASS |
| 60 | 185028 | 361.382812 | TRSV5_PASS |
| 80 | 195268 | 381.382812 | TRSV5_PASS |

## 7. 已知限制

- 这是粗粒度 O3-visible FU 延迟模型，不是内部 divide/mul-sub pipeline 的逐拍模型。
- 当前 trace 是 benchmark 级 CSV；没有实现硬件内部 per-stage trace。
- Direct functional SPM read 不应从 `system.spm` timing stats 反推，TRSV5 数据量以
  `trsv5InputMatrixBytes/InputVectorBytes/OutputBytes` 为准。

## 8. LU-SGS raw 输入中的完整成本边界

standalone TRSV5 仍只定义单个 packed-LU、5-lane RHS 的求解语义。新增 LU-SGS raw
模式不再把 `LU(D)` 和 `D^-1U` 当外部免费输入：每个 cell 先由软件
`cfd_lu5_factor(D)` 产生与 TRSV5 完全一致的 packed LU，再由
`trsm5_mrhs_spm(LU,U)` 生成独立 `U_bar`。因此 TRSV raw 的总周期为：

```text
P_common = P_LU_factor + P_Ubar_MRHS
TRSV_total(N) = P_common + N*T_TRSV_hot
```

8x256 实测 `P_LU_factor=798230`、`P_Ubar_MRHS=1034164`、
`P_common=1832394 cycles`，MRHS issue=2048、columns=10240、failure=0。
legacy Step2 仍可从 `lu_a/b_bar` 启动，但只用于回归，不用于完整成本比较。

## 9. Raw 预处理中的 event TRSM5

standalone `trsv5_lu_spm[_zrhs]` 及其默认 60-cycle coarse FU 完整保留。新增的
`--coeff-preprocess-model=event --lu5-model=event` 只用于 raw 系数预处理：TRSV raw
先在 `CfdCoeffPreprocessController` 内逐周期完成 Crout LU，再用统一 TRSM5 kernel
处理 `U_bar=solve(D,U)` 的 5 个 RHS；PRE coeff3 使用同一 kernel 处理 I/L/U 的 15
个 RHS。这样公平比较不再让 TRSV 使用 coarse MRHS、PRE 使用 event coeff3。

event kernel 对每个 RHS 使用 `ForwardDiv -> ForwardMul -> ForwardSub -> LastDiv ->
BackwardMul -> BackwardSub -> Done`，保持 standalone helper 的除法、乘法和减法顺序，
但把每项运算放入具有 latency、II、count 的资源池。同一 RHS 维持严格依赖，不同 RHS
由 `--coeff3-rhs-lanes` 并行。TRSV 每 cell 精确发射 25 div、100 mul、100 sub；最终
Ubar 通过独立 drain engine 写回，热循环仍使用原 TRSV5 指令/FU。

8x64 full-validation 中 512 个 event TRSV preprocess request 全部完成，failure=0，
LU/Ubar residual 分别为 `1.776e-15/2.082e-17`，first/average cell latency 为
`615/1079.63 cycles`，steady-state II 为 `452.57 cycles`。8x256 performance 模式
测得 preprocess=930172 cycles、II=452.97；完整状态机、SPM/drain 和公平数据见
`CFD_DSA_LUSGS_PRETRANSFORM.md` 第 30 节。

## 10. RHS15 scheduler、LU step-ready 与共享 SPM

standalone `trsv5_lu_spm[_zrhs]` 的 5x5 数学语义、编码和默认 60-cycle coarse FU
没有改变。系数预处理控制器在旁路复用同一 FP64 求解顺序，并把 5 或 15 个 RHS
展开为独立 context，由共享 divider/mul/sub 通过
column-major、step-major、round-robin-ready 或 ready-first 仲裁；RHS15 不是 15 套
TRSV5 datapath。

event LU 在 lower column+pivot 完成时发布 forward step，在对应 upper row 完成时发布
backward step，使 TRSM 可以和同 cell 后续 LU 列重叠。`--lu5-model=software` 混合模式
则显式传入软件计时区间生成的 packed LU，event LU active 为 0，只保留 event TRSM。

input/drain 可通过 `--coeff-spm-model=packet` 进入共享 `CfdLocalSpm` 异步
request/response；standalone TRSV5 自身仍是原 direct/coarse 访问模型。packet 生命周期、
layout、16-sweep 结果、auto 和 cancel 见 `CFD_DSA_LUSGS_PRETRANSFORM.md` 第 31 节。
