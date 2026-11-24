# CFD-DSA LU-SGS Step1

本文档记录“端到端 LU-SGS 加速器三阶段方案”的第一阶段实现。Step1 只在软件 benchmark 层把 LU-SGS 前/后向扫掠组织起来，矩阵-向量乘仍复用既有 Path A MVM 指令序列。

## 范围边界

- 不修改 Path A 底层语义、数据通路或默认配置。
- 复用 `lmat5_spm + CFDDSADotp + CFDDSAPack + pred40 st1d` 的现有 Path A MVM 路径。
- 不新增 TRSV5 硬件、控制器、DMA、融合指令、宏指令或外部硬件调度器。
- TRSV、向量减法、LU-SGS 依赖调度全部在 benchmark 软件侧完成。
- 新增文件独立于原 Path A benchmark：
  - `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step1.c`
  - `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step1.py`
  - `CFD_DSA_LUSGS_STEP1.md`

## 数学流程

Forward sweep:

```text
DQ_STAR[0] = A0^-1 RHS0
for i = 1..N-1:
    TMP = C_i * DQ_STAR[i-1]      # Path A MVM
    RHS'_i = RHS_i - TMP          # CPU/SVE-side vector subtraction
    DQ_STAR[i] = A_i^-1 RHS'_i    # software TRSV5
```

Backward sweep:

```text
DQ[N-1] = DQ_STAR[N-1]
for i = N-2..0:
    TMP = B_BAR_i * DQ[i+1]       # Path A MVM
    DQ[i] = DQ_STAR[i] - TMP      # CPU/SVE-side vector subtraction
```

`trsv5_software()` 保持严格顺序：

```c
for (int k = 0; k < 4; k++) {
    value[k] /= lu[k][k];
    for (int i = k + 1; i < 5; i++)
        value[i] -= lu[i][k] * value[k];
}
value[4] /= lu[4][4];
for (int k = 3; k >= 0; k--) {
    for (int i = k + 1; i < 5; i++)
        value[k] -= lu[k][i] * value[i];
}
```

## 运行方式

编译：

```bash
aarch64-linux-gnu-gcc -static -O2 -march=armv8-a+sve \
  projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step1.c -o build/cfd_dsa/aarch64/test_cfd_lusgs_patha_step1_arm -lm
```

默认 1 line x 17 cells：

```bash
./build/ARM/gem5.opt -d results/cfd_dsa/runs/lusgs_step1_1x17 \
  projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step1.py
```

性能规模 8 lines x 64 cells，按 8 条独立 line 交错：

```bash
./build/ARM/gem5.opt -d results/cfd_dsa/runs/lusgs_step1_8x64 \
  projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step1.py --lusgs-lines=8 --lusgs-cells=64 --lusgs-interleave=8
```

## 输出统计

benchmark 直接打印 LU-SGS 软件层统计，并与 gem5 Path A stats 区分：

- `lusgs.pathaMvmForward`, `lusgs.pathaMvmBackward`, `lusgs.pathaMvmTotal`
- `lusgs.softwareTrsv`
- `lusgs.vectorSubOperations`, `lusgs.vectorSubLanes`
- `lusgs.tmpResultStores`, `lusgs.tmpResultLoads`, `lusgs.tmpResultBytes`
- `lusgs.forwardCycles`, `lusgs.backwardCycles`, `lusgs.totalCycles`
- `lusgs.cyclesPerForwardCell`, `lusgs.cyclesPerBackwardCell`, `lusgs.cyclesPerFullCell`
- `lusgs.cycleSource = m5_rpns_x2_for_2GHz`，即在当前 2GHz 配置下用 `m5_rpns` 返回的 simulated ns 折算为 cycles
- `lusgs.softwareTrsvCycleSource = calibration_before_stats_reset`，TRSV cycle share 使用 stats reset 之前的同规模软件 TRSV 校准值，避免在 Path A 热路径里插入大量 pseudo-op
- `max_abs_error`, `max_rel_error`, `mismatch_count`, first mismatch details
- `LUSGS_STEP1_PASS` / `LUSGS_STEP1_FAIL`

SPM timing counters仍来自 gem5 统计；由于 Step1 中 Path A 直接读 SPM，不能用 benchmark 自行伪造 `system.spm` 读写统计。

## 验证矩阵

本轮验证命令均使用 `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step1.py`，Path A 配置保持：

```text
direct SPM, pred40, stride40, internal-buffer, buffer depth 2,
dotp opLat 7, dotp count 1, pack count 1, unroll 8
```

| Case | Interleave | Path A MVM total | Software TRSV | max_abs_error | total cycles | cycles/MVM | Result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1x1 edge | 1 | 0 | 1 | 0 | 1242 | 0 | PASS |
| 1x2 | 1 | 2 | 2 | 0 | 2746 | 1373.000 | PASS |
| 1x3 | 1 | 4 | 3 | 0 | 3190 | 797.500 | PASS |
| 1x17 | 1 | 32 | 17 | 5.5511151231257827e-17 | 12000 | 375.000 | PASS |
| 2x17 | 1 | 64 | 34 | 5.5511151231257827e-17 | 22012 | 343.938 | PASS |
| 8x17 | 8 | 256 | 136 | 5.5511151231257827e-17 | 83728 | 327.063 | PASS |
| 8x64 | 8 | 1008 | 512 | 1.6653345369377348e-16 | 319718 | 317.181 | PASS |

输出目录：

```text
results/cfd_dsa/runs/lusgs_step1_1x1
results/cfd_dsa/runs/lusgs_step1_1x2
results/cfd_dsa/runs/lusgs_step1_1x3
results/cfd_dsa/runs/lusgs_step1_1x17
results/cfd_dsa/runs/lusgs_step1_2x17
results/cfd_dsa/runs/lusgs_step1_8x17_i8
results/cfd_dsa/runs/lusgs_step1_8x64_i8
```

现有 Path A regression 也已重跑：

```text
build/ARM/gem5.opt -d results/cfd_dsa/runs/patha_regression_after_lusgs \
  projects/cfd_dsa/configs/run_cfd_patha.py --bench-iters=16 --max-ticks=20000000000
```

结果：`pair_final_should_be_pong: PASS`，`benchmark_final_should_be_pong: PASS`，`Overall: PASS`。通用 Path A report 仍会按原 microbenchmark 的 precheck/prologue 模型打印 matld 推断 warning；LU-SGS Step1 的精确 MVM/TRSV/临时结果计数以 benchmark 自身的 `lusgs.*` 输出为准。
