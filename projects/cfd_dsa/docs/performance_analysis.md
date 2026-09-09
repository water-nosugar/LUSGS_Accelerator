下面是我从 `1-1-2.txt` 里提取的关键数据。这个测试配置是：`CFDDSADotp opLat=7, count=1`，`CFDDSAMatLd opLat=1, count=2`。也就是说，dotp 是单流水线引擎，MatLd 有 2 个 ReadPort 侧发射资源。

## 1. 配置数据

| 项目                     |        数值 |
| ---------------------- | --------: |
| `CFDDSADotp opLat`     |  7 cycles |
| `CFDDSADotp count`     |         1 |
| `CFDDSADotp pipelined` |      True |
| `CFDDSAMatLd opLat`    |   1 cycle |
| `ReadPort count`       |         2 |
| CPU clock period       | 500 ticks |
| 频率推算                   |     2 GHz |

## 2. 总体性能数据

| 指标                     |             数值 |
| ---------------------- | -------------: |
| `simSeconds`           |     0.000153 s |
| `simTicks`             |    152,614,000 |
| `simInsts`             |        733,284 |
| `simOps`               |        738,147 |
| `system.cpu.numCycles` |        305,230 |
| `system.cpu.cpi`       |       0.416144 |
| `system.cpu.ipc`       |       2.403014 |
| `system.cpu.issueRate` |       2.474000 |
| `hostSeconds`          |         1.71 s |
| `hostInstRate`         | 429,302 inst/s |

这个 IPC 很高，说明 O3 pipeline 整体利用率不错；但 FU busy 数据显示 DSA 两个自定义 FU 资源仍然是主要瓶颈。

## 3. DSA 指令计数

| 指标                                 |      数值 | 含义               |
| ---------------------------------- | ------: | ---------------- |
| `issuedInstType_0::CFDDSADotp`     | 250,010 | 发射的 dotp 指令数     |
| `issuedInstType_0::CFDDSAMatLd`    | 300,030 | 发射的 SPM load 指令数 |
| `committedInstType_0::CFDDSADotp`  | 250,010 | 提交的 dotp 指令数     |
| `committedInstType_0::CFDDSAMatLd` | 300,024 | 提交的 SPM load 指令数 |

这个比例很关键：

```text
CFDDSADotp ≈ 250,010
CFDDSAMatLd ≈ 300,024
```

如果按你现在的新模型：

```text
每个 matvec = 5 条 dotp_row + 6 条 lmat5_spm
```

那么推导：

| 推导项               |          计算 |     结果 |
| ----------------- | ----------: | -----: |
| dotp 推导 matvec 数  | 250,010 / 5 | 50,002 |
| MatLd 推导 matvec 数 | 300,024 / 6 | 50,004 |
| 估计实际 matvec 数     |    约 50,000 |        |

这说明你修改后的“向量也通过 SPM 读取”基本生效了：`CFDDSAMatLd` 数量已经接近 `6 × matvec`，而不是旧模型的 `5 × matvec`。

## 4. Cycles / matvec

如果按 50,000 个 matvec 计算：

```text
305,230 / 50,000 = 6.1046 cycles/matvec
```

如果按 dotp 计数推导的 50,002 个 matvec 计算：

```text
305,230 / 50,002 ≈ 6.1044 cycles/matvec
```

所以这次测试的核心性能可以写成：

| 口径                | cycles/matvec |
| ----------------- | ------------: |
| 按 50,000 matvec   |         6.105 |
| 按 dotp 推导 matvec  |         6.104 |
| 按 MatLd 推导 matvec |         6.103 |

这个结果比你之前理想 Direct-SPM 的 `6.00 cycles/iter` 略高，符合预期：现在每个 matvec 多了一条 vector SPM load，而且 `ReadPort count` 从之前讨论的 8 降到了 2。

## 5. FU busy / 瓶颈数据

| FU 类型         | busy 次数 |     占比 |
| ------------- | ------: | -----: |
| `CFDDSADotp`  | 249,996 | 73.80% |
| `CFDDSAMatLd` |  87,546 | 25.85% |
| `IntAlu`      |     510 |  0.15% |
| `MemWrite`    |     504 |  0.15% |
| `MemRead`     |     113 |  0.03% |

最重要的是这两项：

```text
CFDDSADotp busy = 249,996
CFDDSAMatLd busy = 87,546
```

这说明当前主要瓶颈仍然是 **dotp 单流水线引擎**，但 MatLd 已经不是完全无压力了。`CFDDSAMatLd` 占 busy 的 25.85%，说明 `ReadPort count=2` 下，SPM load 发射资源已经开始参与瓶颈，但还没有超过 dotp。

## 6. 发射宽度分布

| 每周期发射数 |     周期数 |     占比 |
| -----: | ------: | -----: |
|      0 |  16,825 |  5.99% |
|      1 |  39,901 | 14.20% |
|      2 |  77,245 | 27.49% |
|      3 |  39,609 | 14.10% |
|      4 | 101,899 | 36.27% |
|      5 |   1,719 |  0.61% |
|      6 |   1,640 |  0.58% |
|      7 |   1,038 |  0.37% |
|      8 |   1,078 |  0.38% |

`numIssuedDist::mean = 2.687767`，最大达到 8。比较有价值的观察是：**4 发射周期占比最高，达到 36.27%**，说明 O3 前端/调度总体能提供较高并行度，但受自定义 FU 资源限制，平均 issue rate 只有 `2.474`。

## 7. 提交阶段数据

| 指标                            |      数值 |
| ----------------------------- | ------: |
| `commitStats0.numInsts`       | 733,472 |
| `commitStats0.numOps`         | 738,335 |
| `commitStats0.numInstsNotNOP` | 733,284 |
| `commitStats0.numOpsNotNOP`   | 738,147 |
| `commitStats0.numMemRefs`     |  61,977 |
| `commitStats0.numLoadInsts`   |   6,023 |
| `commitStats0.numStoreInsts`  |  55,954 |
| `commitStats0.numVecInsts`    | 651,655 |

这里要注意：`CFDDSAMatLd` 是自定义 OpClass，不一定被统计进普通 `numLoadInsts`。所以判断 SPM 逻辑读量时，应该看 `committedInstType_0::CFDDSAMatLd`，不要只看 `numLoadInsts`。

## 8. SPM 逻辑读量推导

按当前模型：

```text
每条 lmat5_spm = 40B
committed CFDDSAMatLd = 300,024
```

所以：

```text
implied SPM read bytes = 300,024 × 40B = 12,000,960B
≈ 11.45 MiB
```

按约 50,000 个 matvec 算：

```text
12,000,960B / 50,000 ≈ 240.02B / matvec
```

这与新设计目标完全吻合：

```text
5 行矩阵 × 40B + 1 个向量 × 40B = 240B / matvec
```

所以这是这份测试文档里最关键的验证点之一：**MatLd 数量证明“每次向量也从 SPM 读”已经体现在指令流里。**

## 9. 分支预测数据

| 指标                              |       数值 |
| ------------------------------- | -------: |
| `branchPred.lookups total`      |   40,267 |
| `branchPred.committed total`    |   31,658 |
| `branchPred.mispredicted total` |    1,133 |
| `BTBLookups`                    |   40,267 |
| `BTBHits`                       |   31,181 |
| `BTBHitRatio`                   | 0.774356 |
| `branchMisspredict`             |    1,463 |

分支不是主瓶颈，但有一定扰动。对于纯性能测试，如果想进一步稳定 cycles/matvec，可以考虑扩大迭代数、减少外围控制流，或只统计 ROI。

## 10. 最核心结论

这份测试的关键结论可以写成：

```text
在 CFDDSADotp opLat=7、count=1，CFDDSAMatLd opLat=1、ReadPort count=2 的配置下，
50K 级别测试达到约 305,230 cycles，总 IPC=2.403，CPI=0.416，
折合约 6.10 cycles/matvec。

提交指令中 CFDDSADotp=250,010，CFDDSAMatLd=300,024，
比例约为 5:6，说明每个 matvec 已经包含 5 条 dotp_row 和 6 条 SPM load。
这验证了修改后的数据流：5 行矩阵 + 1 个向量均从 SPM 读取。

FU busy 中 CFDDSADotp 占 73.80%，CFDDSAMatLd 占 25.85%，
说明当前主要瓶颈仍是单 dotp pipeline，但 ReadPort count=2 下 MatLd 已经产生可见争用。
```

## 建议放进报告的摘要表

| 类别                |                                             关键值 |
| ----------------- | ----------------------------------------------: |
| 配置                | `Dotp opLat=7,count=1`; `MatLd opLat=1,count=2` |
| 总周期               |                                         305,230 |
| IPC / CPI         |                                   2.403 / 0.416 |
| cycles/matvec     |                                          ≈ 6.10 |
| committed Dotp    |                                         250,010 |
| committed MatLd   |                                         300,024 |
| Dotp:MatLd 比例     |                                             5:6 |
| implied matvecs   |                                        ≈ 50,000 |
| implied SPM read  |                                    12,000,960 B |
| SPM read / matvec |                                         ≈ 240 B |
| 主要瓶颈              |                                    `CFDDSADotp` |
| 次要瓶颈              |                                   `CFDDSAMatLd` |
| Dotp FU busy      |                                249,996 / 73.80% |
| MatLd FU busy     |                                 87,546 / 25.85% |



下面是 `1-1-8.txt` 这组测试的关键数据提取。该组配置是：`CFDDSADotp opLat=7, count=1`，`CFDDSAMatLd opLat=1, ReadPort count=8`。

## 1. 配置数据

| 项目                     |        数值 |
| ---------------------- | --------: |
| `CFDDSADotp opLat`     |  7 cycles |
| `CFDDSADotp count`     |         1 |
| `CFDDSADotp pipelined` |      True |
| `CFDDSAMatLd opLat`    |   1 cycle |
| `ReadPort count`       |         8 |
| CPU clock period       | 500 ticks |
| 频率推算                   |     2 GHz |

## 2. 总体性能数据

| 指标                     |             数值 |
| ---------------------- | -------------: |
| `simSeconds`           |     0.000153 s |
| `simTicks`             |    152,605,000 |
| `simInsts`             |        733,284 |
| `simOps`               |        738,147 |
| `system.cpu.numCycles` |        305,212 |
| `system.cpu.cpi`       |       0.416119 |
| `system.cpu.ipc`       |       2.403156 |
| `system.cpu.issueRate` |       2.474342 |
| `hostSeconds`          |         1.76 s |
| `hostInstRate`         | 415,480 inst/s |

这组和 `count=2` 的结果非常接近，周期数只少了 18 cycles，说明在当前 workload 下，`ReadPort count=2` 已经基本够用，继续加到 8 对总性能提升很小。

## 3. DSA 指令计数

| 指标                                            |      数值 |
| --------------------------------------------- | ------: |
| `issuedInstType_0::CFDDSADotp`                | 250,010 |
| `issuedInstType_0::CFDDSAMatLd`               | 300,030 |
| `commitStats0.committedInstType::CFDDSADotp`  | 250,010 |
| `commitStats0.committedInstType::CFDDSAMatLd` | 300,024 |

按照新数据流：

```text
每个 matvec = 5 条 dotp_row + 6 条 lmat5_spm
```

可以推导：

| 推导项               |          计算 |     结果 |
| ----------------- | ----------: | -----: |
| dotp 推导 matvec 数  | 250,010 / 5 | 50,002 |
| MatLd 推导 matvec 数 | 300,024 / 6 | 50,004 |
| 估计实际 matvec 数     |    约 50,000 |        |

这继续验证了：**每个 matvec 已经包含 6 条 SPM 读取，也就是 5 行矩阵 + 1 个向量。**

## 4. Cycles / matvec

按 50,000 个 matvec 计算：

```text
305,212 / 50,000 = 6.10424 cycles/matvec
```

按 dotp 计数推导的 50,002 个 matvec 计算：

```text
305,212 / 50,002 ≈ 6.1040 cycles/matvec
```

按 MatLd 计数推导的 50,004 个 matvec 计算：

```text
305,212 / 50,004 ≈ 6.1038 cycles/matvec
```

核心性能可以写成：

| 口径                | cycles/matvec |
| ----------------- | ------------: |
| 按 50,000 matvec   |         6.104 |
| 按 dotp 推导 matvec  |         6.104 |
| 按 MatLd 推导 matvec |         6.104 |

## 5. FU busy / 瓶颈数据

| FU 类型         |  busy 次数 |     占比 |
| ------------- | -------: | -----: |
| `CFDDSADotp`  |  249,997 | 99.63% |
| `CFDDSAMatLd` |        0 |  0.00% |
| `MemWrite`    |      376 |  0.15% |
| `IntAlu`      |      500 |  0.20% |
| 总 `fuBusy`    |  250,932 |      — |
| `fuBusyRate`  | 0.332273 |      — |

这组最关键的变化是：

```text
CFDDSAMatLd busy = 0
CFDDSADotp busy ≈ 250K
```

说明 `ReadPort count=8` 已经完全消除了 MatLd 发射端口争用，系统瓶颈几乎全部集中到单条 `CFD_Matrix_Engine` dotp pipeline。

这也证明：`count=8` 是更强的 idealized load-side 配置；它适合展示上界，但硬件解释时要说明这是理想 SPM 发射模型。

## 6. 发射宽度分布

| 每周期发射数 |     周期数 |     占比 |
| -----: | ------: | -----: |
|      0 |  16,821 |  5.99% |
|      1 |  39,904 | 14.20% |
|      2 | 102,232 | 36.39% |
|      3 |  39,580 | 14.09% |
|      4 |  39,461 | 14.05% |
|      5 |  26,697 |  9.50% |
|      6 |  14,095 |  5.02% |
|      7 |   1,008 |  0.36% |
|      8 |   1,142 |  0.41% |

| 汇总项                        |       数值 |
| -------------------------- | -------: |
| `numIssuedDist::mean`      | 2.688115 |
| `numIssuedDist::stdev`     | 1.582651 |
| `numIssuedDist::max_value` |        8 |

和 `count=2` 相比，这组的高发射周期更多：5 发射和 6 发射周期明显增加。这说明 `ReadPort count=8` 让 MatLd 更容易被提前发射出去，但由于最终瓶颈是 dotp 单流水线，总周期几乎没有明显下降。

## 7. 提交阶段数据

| 指标                            |      数值 |
| ----------------------------- | ------: |
| `commitStats0.numInsts`       | 733,472 |
| `commitStats0.numOps`         | 738,335 |
| `commitStats0.numInstsNotNOP` | 733,284 |
| `commitStats0.numOpsNotNOP`   | 738,147 |
| `commitStats0.numMemRefs`     |  61,977 |
| `commitStats0.numLoadInsts`   |   6,023 |
| `commitStats0.numStoreInsts`  |  55,954 |
| `commitStats0.numVecInsts`    | 651,655 |
| `commitStats0.numFpInsts`     | 550,034 |
| `commitStats0.numIntInsts`    | 109,348 |

这里仍然要注意：`CFDDSAMatLd` 是自定义 DSA load，不等价于普通 `numLoadInsts`。判断 SPM 逻辑读量，应使用 `commitStats0.committedInstType::CFDDSAMatLd = 300,024`。

## 8. SPM 逻辑读量推导

```text
每条 lmat5_spm = 40B
committed CFDDSAMatLd = 300,024
```

因此：

```text
implied SPM read bytes = 300,024 × 40B = 12,000,960B
≈ 11.45 MiB
```

按 50,000 个 matvec 计算：

```text
12,000,960B / 50,000 = 240.0192B / matvec
```

这和目标数据流吻合：

```text
5 行矩阵 × 40B + 1 个向量 × 40B = 240B / matvec
```

## 9. 分支预测数据

| 指标                              |       数值 |
| ------------------------------- | -------: |
| `branchPred.lookups total`      |   40,254 |
| `branchPred.committed total`    |   31,658 |
| `branchPred.mispredicted total` |    1,131 |
| `branchMisspredict`             |    1,461 |
| `BTBLookups`                    |   40,254 |
| `BTBHits`                       |   31,173 |
| `BTBHitRatio`                   | 0.774408 |
| `condPredicted`                 |   35,377 |
| `condIncorrect`                 |    1,131 |

分支数据和上一组基本一致，不是主要性能差异来源。

## 10. 与 `ReadPort count=2` 的直接对比

| 指标            |  count=2 |  count=8 |        变化 |
| ------------- | -------: | -------: | --------: |
| cycles        |  305,230 |  305,212 |       -18 |
| IPC           | 2.403014 | 2.403156 | +0.000142 |
| CPI           | 0.416144 | 0.416119 | -0.000025 |
| issueRate     | 2.474000 | 2.474342 | +0.000342 |
| Dotp issued   |  250,010 |  250,010 |         0 |
| MatLd issued  |  300,030 |  300,030 |         0 |
| Dotp FU busy  |  249,996 |  249,997 |        +1 |
| MatLd FU busy |   87,546 |        0 |   -87,546 |
| cycles/matvec |    6.105 |    6.104 |    -0.001 |

结论很明确：

```text
ReadPort count 从 2 增加到 8 后，MatLd FU busy 从 87,546 降到 0，
但总周期只减少 18 cycles，cycles/matvec 几乎不变。
```

这说明当前性能已经不是 MatLd 端口受限，而是 **CFDDSADotp 单流水线受限**。

## 11. 建议放进报告的摘要表

| 类别                |                                             关键值 |
| ----------------- | ----------------------------------------------: |
| 配置                | `Dotp opLat=7,count=1`; `MatLd opLat=1,count=8` |
| 总周期               |                                         305,212 |
| IPC / CPI         |                             2.403156 / 0.416119 |
| issueRate         |                                        2.474342 |
| cycles/matvec     |                                         ≈ 6.104 |
| committed Dotp    |                                         250,010 |
| committed MatLd   |                                         300,024 |
| Dotp:MatLd 比例     |                                             5:6 |
| implied matvecs   |                                        ≈ 50,000 |
| implied SPM read  |                                    12,000,960 B |
| SPM read / matvec |                                         ≈ 240 B |
| 主要瓶颈              |                                    `CFDDSADotp` |
| Dotp FU busy      |                                249,997 / 99.63% |
| MatLd FU busy     |                                       0 / 0.00% |

## 最核心结论

这组 `count=8` 测试说明：**增加 ReadPort 数量可以完全消除 MatLd 发射资源争用，但对总性能几乎没有提升**。当前 steady-state 已经由单 `CFDDSADotp` pipeline 决定，约为 `6.10 cycles/matvec`。如果下一步要继续降低 cycles/matvec，优先方向不是继续增加 ReadPort，而是考虑增加 `CFD_Matrix_Engine count` 或减少 dotp 路径/pack/store/loop 的调度开销。


下面是 `2-1-2.txt` 这组测试的关键数据提取。该组配置是：`CFDDSADotp opLat=7, count=2`，`CFDDSAMatLd opLat=1, ReadPort count=2`。

## 1. 配置数据

| 项目                     |        数值 |
| ---------------------- | --------: |
| `CFDDSADotp opLat`     |  7 cycles |
| `CFDDSADotp count`     |         2 |
| `CFDDSADotp pipelined` |      True |
| `CFDDSAMatLd opLat`    |   1 cycle |
| `ReadPort count`       |         2 |
| CPU clock period       | 500 ticks |
| 频率推算                   |     2 GHz |

## 2. 总体性能数据

| 指标                     |             数值 |
| ---------------------- | -------------: |
| `simSeconds`           |     0.000103 s |
| `simTicks`             |    102,640,000 |
| `simInsts`             |        733,284 |
| `simOps`               |        738,147 |
| `system.cpu.numCycles` |        205,282 |
| `system.cpu.cpi`       |       0.279877 |
| `system.cpu.ipc`       |       3.572997 |
| `system.cpu.issueRate` |       3.678374 |
| `hostSeconds`          |         1.65 s |
| `hostInstRate`         | 443,793 inst/s |

相较 `Dotp count=1` 的约 `305K cycles`，这组降到 `205K cycles`，说明增加 dotp engine 数量带来了明显收益。

## 3. DSA 指令计数

| 指标                              |      数值 |
| ------------------------------- | ------: |
| `issuedInstType_0::CFDDSADotp`  | 250,010 |
| `issuedInstType_0::CFDDSAMatLd` | 300,024 |

按当前数据流：

```text
每个 matvec = 5 条 dotp_row + 6 条 lmat5_spm
```

推导：

| 推导项               |          计算 |     结果 |
| ----------------- | ----------: | -----: |
| dotp 推导 matvec 数  | 250,010 / 5 | 50,002 |
| MatLd 推导 matvec 数 | 300,024 / 6 | 50,004 |
| 估计实际 matvec 数     |    约 50,000 |        |

这说明新模型依然成立：每个 matvec 包含 5 条 dotp 和 6 条 SPM load。

## 4. Cycles / matvec

按 50,000 个 matvec 计算：

```text
205,282 / 50,000 = 4.10564 cycles/matvec
```

按 dotp 计数推导的 50,002 个 matvec 计算：

```text
205,282 / 50,002 ≈ 4.1055 cycles/matvec
```

按 MatLd 计数推导的 50,004 个 matvec 计算：

```text
205,282 / 50,004 ≈ 4.1053 cycles/matvec
```

核心性能：

| 口径                | cycles/matvec |
| ----------------- | ------------: |
| 按 50,000 matvec   |         4.106 |
| 按 dotp 推导 matvec  |         4.105 |
| 按 MatLd 推导 matvec |         4.105 |

## 5. FU busy / 瓶颈数据

| FU 类型         |  busy 次数 |     占比 |
| ------------- | -------: | -----: |
| `CFDDSADotp`  |   99,990 | 39.81% |
| `CFDDSAMatLd` |  149,996 | 59.72% |
| `MemWrite`    |      501 |  0.20% |
| `MemRead`     |      113 |  0.04% |
| `IntAlu`      |      509 |  0.20% |
| 总 `fuBusy`    |  251,168 |      — |
| `fuBusyRate`  | 0.332627 |      — |

这组非常关键：当 `CFDDSADotp count` 从 1 增加到 2 后，瓶颈明显从 dotp 侧转移到了 MatLd 侧。`CFDDSAMatLd` busy 达到 `149,996`，占 `59.72%`；`CFDDSADotp` busy 降为 `39.81%`。

也就是说，当前配置下：

```text
2 个 dotp engine 已经让计算侧变快；
ReadPort count=2 开始成为主要瓶颈。
```

## 6. 发射宽度分布

| 每周期发射数 |    周期数 |     占比 |
| -----: | -----: | -----: |
|      0 | 16,846 |  9.31% |
|      1 |  2,397 |  1.32% |
|      2 |  2,292 |  1.27% |
|      3 | 27,086 | 14.96% |
|      4 | 26,960 | 14.90% |
|      5 | 76,679 | 42.36% |
|      6 | 26,616 | 14.71% |
|      7 |  1,050 |  0.58% |
|      8 |  1,073 |  0.59% |

| 汇总项                        |       数值 |
| -------------------------- | -------: |
| `numIssuedDist::mean`      | 4.171868 |
| `numIssuedDist::stdev`     | 1.714068 |
| `numIssuedDist::max_value` |        8 |

这组的发射并行度比 `Dotp count=1` 明显更高，5 发射周期占比高达 `42.36%`，平均 issued/cycle 也从约 `2.69` 提升到 `4.17`。

## 7. SPM 逻辑读量推导

```text
每条 lmat5_spm = 40B
issued CFDDSAMatLd = 300,024
```

因此：

```text
implied SPM read bytes = 300,024 × 40B = 12,000,960B
≈ 11.45 MiB
```

按 50,000 个 matvec 计算：

```text
12,000,960B / 50,000 = 240.0192B / matvec
```

这继续吻合你的新数据流：

```text
5 行矩阵 × 40B + 1 个向量 × 40B = 240B / matvec
```

## 8. 分支预测数据

| 指标                              |       数值 |
| ------------------------------- | -------: |
| `branchPred.lookups total`      |   40,257 |
| `branchPred.committed total`    |   31,658 |
| `branchPred.mispredicted total` |    1,133 |
| `branchMisspredict`             |    1,463 |
| `BTBLookups`                    |   40,257 |
| `BTBHits`                       |   31,179 |
| `BTBHitRatio`                   | 0.774499 |
| `condPredicted`                 |   35,366 |
| `condIncorrect`                 |    1,133 |

分支数据与前几组基本一致，性能提升主要来自 dotp engine 数量增加，而不是分支行为变化。

## 9. 与前两组的对比

| 配置             |  cycles |   IPC | issueRate | cycles/matvec | Dotp busy | MatLd busy | 主要瓶颈  |
| -------------- | ------: | ----: | --------: | ------------: | --------: | ---------: | ----- |
| Dotp1 + MatLd2 | 305,230 | 2.403 |     2.474 |         6.105 |   249,996 |     87,546 | Dotp  |
| Dotp1 + MatLd8 | 305,212 | 2.403 |     2.474 |         6.104 |   249,997 |          0 | Dotp  |
| Dotp2 + MatLd2 | 205,282 | 3.573 |     3.678 |         4.106 |    99,990 |    149,996 | MatLd |

最关键的变化：

```text
Dotp count 从 1 增加到 2 后：
cycles/matvec 从约 6.10 降到约 4.11；
但瓶颈从 CFDDSADotp 转移到了 CFDDSAMatLd。
```

## 10. 建议放进报告的摘要表

| 类别                |                                             关键值 |
| ----------------- | ----------------------------------------------: |
| 配置                | `Dotp opLat=7,count=2`; `MatLd opLat=1,count=2` |
| 总周期               |                                         205,282 |
| IPC / CPI         |                             3.572997 / 0.279877 |
| issueRate         |                                        3.678374 |
| cycles/matvec     |                                         ≈ 4.106 |
| issued Dotp       |                                         250,010 |
| issued MatLd      |                                         300,024 |
| Dotp:MatLd 比例     |                                             5:6 |
| implied matvecs   |                                        ≈ 50,000 |
| implied SPM read  |                                    12,000,960 B |
| SPM read / matvec |                                         ≈ 240 B |
| 主要瓶颈              |                                   `CFDDSAMatLd` |
| Dotp FU busy      |                                 99,990 / 39.81% |
| MatLd FU busy     |                                149,996 / 59.72% |

## 最核心结论

这组 `Dotp count=2 + MatLd count=2` 表明：**增加 dotp engine 后，性能从约 6.10 cycles/matvec 提升到约 4.11 cycles/matvec，但瓶颈转移到了 SPM load 发射侧。** 下一组最有价值的测试应该是 `Dotp count=2 + MatLd count=4/8`，用来确认增加 ReadPort 后是否还能继续逼近 3 cycles/matvec 左右。

下面是 `2-2-2.txt` 这组测试的关键数据提取。该组配置是：`CFDDSADotp opLat=7, count=2`，`CFDDSAMatLd opLat=2, ReadPort count=2`。

## 1. 配置数据

| 项目                     |        数值 |
| ---------------------- | --------: |
| `CFDDSADotp opLat`     |  7 cycles |
| `CFDDSADotp count`     |         2 |
| `CFDDSADotp pipelined` |      True |
| `CFDDSAMatLd opLat`    |  2 cycles |
| `ReadPort count`       |         2 |
| CPU clock period       | 500 ticks |
| 频率推算                   |     2 GHz |

这组和上一组 `2-1-2` 的区别只有一个：`CFDDSAMatLd opLat` 从 1 增加到 2，其他保持 `Dotp count=2`、`ReadPort count=2`。

## 2. 总体性能数据

| 指标                     |             数值 |
| ---------------------- | -------------: |
| `simSeconds`           |     0.000103 s |
| `simTicks`             |    102,640,500 |
| `simInsts`             |        733,284 |
| `simOps`               |        738,147 |
| `system.cpu.numCycles` |        205,283 |
| `system.cpu.cpi`       |       0.279878 |
| `system.cpu.ipc`       |       3.572980 |
| `system.cpu.issueRate` |       3.678327 |
| `hostSeconds`          |         1.69 s |
| `hostInstRate`         | 433,902 inst/s |

核心性能几乎和 `2-1-2` 一样。`numCycles` 只比 `2-1-2` 多 1 cycle，说明在当前流水线调度下，`MatLd opLat=1 → 2` **没有实质影响总性能**。

## 3. DSA 指令计数

| 指标                              |      数值 |
| ------------------------------- | ------: |
| `issuedInstType_0::CFDDSADotp`  | 250,010 |
| `issuedInstType_0::CFDDSAMatLd` | 300,024 |

按照当前数据流：

```text
每个 matvec = 5 条 dotp_row + 6 条 lmat5_spm
```

可以推导：

| 推导项               |          计算 |     结果 |
| ----------------- | ----------: | -----: |
| dotp 推导 matvec 数  | 250,010 / 5 | 50,002 |
| MatLd 推导 matvec 数 | 300,024 / 6 | 50,004 |
| 估计实际 matvec 数     |    约 50,000 |        |

这说明这组测试中，指令流仍然符合“每个 matvec 读取 5 行矩阵 + 1 个向量”的新模型。

## 4. Cycles / matvec

按 50,000 个 matvec 计算：

```text
205,283 / 50,000 = 4.10566 cycles/matvec
```

按 dotp 计数推导的 50,002 个 matvec 计算：

```text
205,283 / 50,002 ≈ 4.1055 cycles/matvec
```

按 MatLd 计数推导的 50,004 个 matvec 计算：

```text
205,283 / 50,004 ≈ 4.1053 cycles/matvec
```

| 口径                | cycles/matvec |
| ----------------- | ------------: |
| 按 50,000 matvec   |         4.106 |
| 按 dotp 推导 matvec  |         4.105 |
| 按 MatLd 推导 matvec |         4.105 |

## 5. FU busy / 瓶颈数据

| FU 类型         |  busy 次数 |     占比 |
| ------------- | -------: | -----: |
| `CFDDSADotp`  |   99,990 | 39.81% |
| `CFDDSAMatLd` |  149,996 | 59.72% |
| `MemWrite`    |      501 |  0.20% |
| `MemRead`     |      113 |  0.04% |
| `IntAlu`      |      509 |  0.20% |
| 总 `fuBusy`    |  251,168 |      — |
| `fuBusyRate`  | 0.332630 |      — |

这组的瓶颈结构和 `2-1-2` 完全一致：主要瓶颈仍然是 `CFDDSAMatLd` 发射资源，而不是 dotp。也就是说，当前 `Dotp count=2` 后，系统已经进入 **SPM load / MatLd 端口受限** 状态。

## 6. 发射宽度分布

| 每周期发射数 |    周期数 |     占比 |
| -----: | -----: | -----: |
|      0 | 16,855 |  9.31% |
|      1 |  2,390 |  1.32% |
|      2 |  2,295 |  1.27% |
|      3 | 39,575 | 21.86% |
|      4 | 14,461 |  7.99% |
|      5 | 64,203 | 35.47% |
|      6 | 39,108 | 21.61% |
|      7 |  1,034 |  0.57% |
|      8 |  1,081 |  0.60% |

| 汇总项                        |       数值 |
| -------------------------- | -------: |
| `numIssuedDist::mean`      | 4.171766 |
| `numIssuedDist::stdev`     | 1.792924 |
| `numIssuedDist::max_value` |        8 |

平均每周期发射约 `4.17` 条，明显高于 `Dotp count=1` 的约 `2.69`，说明增加 dotp engine 后 O3 能发掘更多并行性。

## 7. SPM 逻辑读量推导

```text
每条 lmat5_spm = 40B
issued CFDDSAMatLd = 300,024
```

所以：

```text
implied SPM read bytes = 300,024 × 40B = 12,000,960B
≈ 11.45 MiB
```

按 50,000 个 matvec 计算：

```text
12,000,960B / 50,000 = 240.0192B / matvec
```

这与当前数据流一致：

```text
5 行矩阵 × 40B + 1 个向量 × 40B = 240B / matvec
```

## 8. 分支预测数据

| 指标                              |       数值 |
| ------------------------------- | -------: |
| `branchPred.lookups total`      |   40,257 |
| `branchPred.committed total`    |   31,658 |
| `branchPred.mispredicted total` |    1,133 |
| `branchMisspredict`             |    1,463 |
| `BTBLookups`                    |   40,257 |
| `BTBHits`                       |   31,179 |
| `BTBHitRatio`                   | 0.774499 |
| `condPredicted`                 |   35,366 |
| `condIncorrect`                 |    1,133 |

分支行为与前一组基本一致，不是这组性能变化的来源。

## 9. 与 `2-1-2` 的直接对比

| 配置             |  `2-1-2` |  `2-2-2` |        变化 |
| -------------- | -------: | -------: | --------: |
| Dotp count     |        2 |        2 |         0 |
| MatLd opLat    |        1 |        2 |        +1 |
| ReadPort count |        2 |        2 |         0 |
| cycles         |  205,282 |  205,283 |        +1 |
| IPC            | 3.572997 | 3.572980 | -0.000017 |
| CPI            | 0.279877 | 0.279878 | +0.000001 |
| issueRate      | 3.678374 | 3.678327 | -0.000047 |
| Dotp issued    |  250,010 |  250,010 |         0 |
| MatLd issued   |  300,024 |  300,024 |         0 |
| Dotp FU busy   |   99,990 |   99,990 |         0 |
| MatLd FU busy  |  149,996 |  149,996 |         0 |
| cycles/matvec  |   4.1056 |   4.1057 |      基本不变 |

这个对比非常关键：

```text
MatLd opLat 从 1 增加到 2，几乎没有改变总周期。
```

说明当前性能主要受 **MatLd 发射带宽 / ReadPort count** 限制，而不是单条 MatLd 的结果延迟限制。换句话说，在当前软件流水线中，MatLd 的延迟被 Ping-Pong 预取和 O3 乱序执行隐藏掉了；真正卡住的是 `count=2` 造成的 MatLd 发射资源争用。

## 10. 建议放进报告的摘要表

| 类别                |                                             关键值 |
| ----------------- | ----------------------------------------------: |
| 配置                | `Dotp opLat=7,count=2`; `MatLd opLat=2,count=2` |
| 总周期               |                                         205,283 |
| IPC / CPI         |                             3.572980 / 0.279878 |
| issueRate         |                                        3.678327 |
| cycles/matvec     |                                         ≈ 4.106 |
| issued Dotp       |                                         250,010 |
| issued MatLd      |                                         300,024 |
| Dotp:MatLd 比例     |                                             5:6 |
| implied matvecs   |                                        ≈ 50,000 |
| implied SPM read  |                                    12,000,960 B |
| SPM read / matvec |                                         ≈ 240 B |
| 主要瓶颈              |                                   `CFDDSAMatLd` |
| Dotp FU busy      |                                 99,990 / 39.81% |
| MatLd FU busy     |                                149,996 / 59.72% |

## 最核心结论

这组 `Dotp count=2 + MatLd opLat=2 + ReadPort count=2` 的结果与 `MatLd opLat=1` 几乎完全相同：约 `4.106 cycles/matvec`。这说明在当前 Ping-Pong + O3 调度下，**MatLd 的单条延迟不是主要问题，ReadPort 发射带宽才是问题**。下一步更有价值的参数是增加 `ReadPort count`，例如测试 `2-1-4`、`2-2-4`、`2-1-8`，而不是继续只调高或调低 `MatLd opLat`。


下面是 `2-2-4.txt` 这组测试的关键数据提取。根据文件名和统计内容，这组配置应为：`CFDDSADotp count=2`，`CFDDSAMatLd opLat=2`，`ReadPort count=4`。文件正文里可见完整 gem5 统计数据。

## 1. 配置数据

| 项目                     |        数值 |
| ---------------------- | --------: |
| `CFDDSADotp opLat`     |  7 cycles |
| `CFDDSADotp count`     |         2 |
| `CFDDSADotp pipelined` |      True |
| `CFDDSAMatLd opLat`    |  2 cycles |
| `ReadPort count`       |         4 |
| CPU clock period       | 500 ticks |
| 频率推算                   |     2 GHz |

## 2. 总体性能数据

| 指标                     |             数值 |
| ---------------------- | -------------: |
| `simSeconds`           |     0.000090 s |
| `simTicks`             |     90,131,500 |
| `simInsts`             |        733,284 |
| `simOps`               |        738,147 |
| `system.cpu.numCycles` |        180,265 |
| `system.cpu.cpi`       |       0.245769 |
| `system.cpu.ipc`       |       4.068854 |
| `system.cpu.issueRate` |       4.188794 |
| `hostSeconds`          |         1.66 s |
| `hostInstRate`         | 440,395 inst/s |

这组相比 `2-2-2` 的 `205,283 cycles` 明显下降到 `180,265 cycles`，说明 **ReadPort count 从 2 提升到 4 后有效缓解了 MatLd 端口瓶颈**。

## 3. DSA 指令计数

| 指标                              |      数值 |
| ------------------------------- | ------: |
| `issuedInstType_0::CFDDSADotp`  | 250,010 |
| `issuedInstType_0::CFDDSAMatLd` | 300,032 |

按照当前数据流：

```text
每个 matvec = 5 条 dotp_row + 6 条 lmat5_spm
```

推导：

| 推导项               |          计算 |       结果 |
| ----------------- | ----------: | -------: |
| dotp 推导 matvec 数  | 250,010 / 5 |   50,002 |
| MatLd 推导 matvec 数 | 300,032 / 6 | 50,005.3 |
| 估计实际 matvec 数     |    约 50,000 |          |

这说明这组测试仍然符合新模型：**每个 matvec 包含 5 条 dotp 和 6 条 SPM load**。

## 4. Cycles / matvec

按 50,000 个 matvec 计算：

```text
180,265 / 50,000 = 3.6053 cycles/matvec
```

按 dotp 计数推导的 50,002 个 matvec 计算：

```text
180,265 / 50,002 ≈ 3.6052 cycles/matvec
```

按 MatLd 计数推导的 50,005.3 个 matvec 计算：

```text
180,265 / 50,005.3 ≈ 3.6050 cycles/matvec
```

| 口径                | cycles/matvec |
| ----------------- | ------------: |
| 按 50,000 matvec   |         3.605 |
| 按 dotp 推导 matvec  |         3.605 |
| 按 MatLd 推导 matvec |         3.605 |

## 5. FU busy / 瓶颈数据

| FU 类型         |  busy 次数 |     占比 |
| ------------- | -------: | -----: |
| `CFDDSADotp`  |  124,981 | 90.25% |
| `CFDDSAMatLd` |   12,507 |  9.03% |
| `MemWrite`    |      433 |  0.31% |
| `IntAlu`      |      509 |  0.37% |
| 总 `fuBusy`    |  138,489 |      — |
| `fuBusyRate`  | 0.183407 |      — |

这组的瓶颈重新转回了 **CFDDSADotp**。和 `2-2-2` 相比，`CFDDSAMatLd busy` 从 `149,996` 降到 `12,507`，说明 `ReadPort count=4` 已经基本解决 MatLd 发射端口压力。

## 6. 发射宽度分布

| 每周期发射数 |    周期数 |     占比 |
| -----: | -----: | -----: |
|      0 | 16,821 | 10.78% |
|      1 |  2,388 |  1.53% |
|      2 |  2,312 |  1.48% |
|      3 | 14,605 |  9.36% |
|      4 | 26,957 | 17.28% |
|      5 | 26,695 | 17.11% |
|      6 | 14,104 |  9.04% |
|      7 | 38,493 | 24.68% |
|      8 | 13,611 |  8.73% |

| 汇总项                        |       数值 |
| -------------------------- | -------: |
| `numIssuedDist::mean`      | 4.840774 |
| `numIssuedDist::stdev`     | 2.336473 |
| `numIssuedDist::max_value` |        8 |

这组的发射并行度进一步提升：平均每周期发射 `4.84` 条，7 发射和 8 发射周期占比显著增加，说明 ReadPort 放宽后 O3 可以更充分地把 load、dotp、pack/store 等指令并行排布。

## 7. SPM 逻辑读量推导

```text
每条 lmat5_spm = 40B
issued CFDDSAMatLd = 300,032
```

因此：

```text
implied SPM read bytes = 300,032 × 40B = 12,001,280B
≈ 11.45 MiB
```

按 50,000 个 matvec 计算：

```text
12,001,280B / 50,000 = 240.0256B / matvec
```

这与当前数据流吻合：

```text
5 行矩阵 × 40B + 1 个向量 × 40B = 240B / matvec
```

## 8. 分支预测数据

| 指标                              |       数值 |
| ------------------------------- | -------: |
| `branchPred.lookups total`      |   40,251 |
| `branchPred.committed total`    |   31,658 |
| `branchPred.mispredicted total` |    1,131 |
| `branchMisspredict`             |    1,461 |
| `BTBLookups`                    |   40,251 |
| `BTBHits`                       |   31,157 |
| `BTBHitRatio`                   | 0.774068 |
| `condPredicted`                 |   35,371 |
| `condIncorrect`                 |    1,131 |

分支行为与前几组基本一致，不是这组性能提升的主要原因。性能提升主要来自 `ReadPort count=4` 缓解了 MatLd 端口争用。

## 9. 与前几组的对比

| 配置                   |  cycles |   IPC | issueRate | cycles/matvec | Dotp busy | MatLd busy | 主要瓶颈  |
| -------------------- | ------: | ----: | --------: | ------------: | --------: | ---------: | ----- |
| Dotp1 + MatLd1 + RP2 | 305,230 | 2.403 |     2.474 |         6.105 |   249,996 |     87,546 | Dotp  |
| Dotp1 + MatLd1 + RP8 | 305,212 | 2.403 |     2.474 |         6.104 |   249,997 |          0 | Dotp  |
| Dotp2 + MatLd1 + RP2 | 205,282 | 3.573 |     3.678 |         4.106 |    99,990 |    149,996 | MatLd |
| Dotp2 + MatLd2 + RP2 | 205,283 | 3.573 |     3.678 |         4.106 |    99,990 |    149,996 | MatLd |
| Dotp2 + MatLd2 + RP4 | 180,265 | 4.069 |     4.189 |         3.605 |   124,981 |     12,507 | Dotp  |

关键变化：

```text
ReadPort count 从 2 增加到 4 后：
cycles/matvec 从约 4.106 降到约 3.605；
MatLd busy 从约 150K 降到约 12.5K；
瓶颈从 MatLd 侧重新转向 dotp 侧。
```

## 10. 建议放进报告的摘要表

| 类别                |                                             关键值 |
| ----------------- | ----------------------------------------------: |
| 配置                | `Dotp opLat=7,count=2`; `MatLd opLat=2,count=4` |
| 总周期               |                                         180,265 |
| IPC / CPI         |                             4.068854 / 0.245769 |
| issueRate         |                                        4.188794 |
| cycles/matvec     |                                         ≈ 3.605 |
| issued Dotp       |                                         250,010 |
| issued MatLd      |                                         300,032 |
| Dotp:MatLd 比例     |                                             5:6 |
| implied matvecs   |                                        ≈ 50,000 |
| implied SPM read  |                                    12,001,280 B |
| SPM read / matvec |                                         ≈ 240 B |
| 主要瓶颈              |                                    `CFDDSADotp` |
| Dotp FU busy      |                                124,981 / 90.25% |
| MatLd FU busy     |                                  12,507 / 9.03% |

## 最核心结论

这组 `Dotp count=2 + MatLd opLat=2 + ReadPort count=4` 的结果说明：**当 ReadPort 从 2 增加到 4 后，SPM load 端口瓶颈基本被解除，性能从约 4.106 cycles/matvec 提升到约 3.605 cycles/matvec。**

但此时主要瓶颈又回到 `CFDDSADotp`，说明下一步如果继续追求更低 cycles/matvec，应该测试：

```text
Dotp count=3 或 4
ReadPort count=4 或 8
MatLd opLat=1/2
```

尤其是 `Dotp count=3 + ReadPort count=4`，可能是下一组最有价值的实验。



下面是 `4-2-4.txt` 这组测试的关键数据提取。该组配置是：`CFDDSADotp opLat=7, count=4`，`CFDDSAMatLd opLat=2, ReadPort count=4`。

## 1. 配置数据

| 项目                     |        数值 |
| ---------------------- | --------: |
| `CFDDSADotp opLat`     |  7 cycles |
| `CFDDSADotp count`     |         4 |
| `CFDDSADotp pipelined` |      True |
| `CFDDSAMatLd opLat`    |  2 cycles |
| `ReadPort count`       |         4 |
| CPU clock period       | 500 ticks |
| 频率推算                   |     2 GHz |

## 2. 总体性能数据

| 指标                     |             数值 |
| ---------------------- | -------------: |
| `simSeconds`           |     0.000078 s |
| `simTicks`             |     77,636,500 |
| `simInsts`             |        733,284 |
| `simOps`               |        738,147 |
| `system.cpu.numCycles` |        155,275 |
| `system.cpu.cpi`       |       0.211699 |
| `system.cpu.ipc`       |       4.723697 |
| `system.cpu.issueRate` |       4.862830 |
| `hostSeconds`          |         1.55 s |
| `hostInstRate`         | 474,545 inst/s |

相比 `2-2-4` 的 `180,265 cycles`，这组降低到 `155,275 cycles`，说明把 `CFD_Matrix_Engine count` 从 2 提升到 4 后仍然有明显收益。

## 3. DSA 指令计数

| 指标                              |      数值 |
| ------------------------------- | ------: |
| `issuedInstType_0::CFDDSADotp`  | 250,015 |
| `issuedInstType_0::CFDDSAMatLd` | 300,035 |

按照当前数据流：

```text
每个 matvec = 5 条 dotp_row + 6 条 lmat5_spm
```

推导：

| 推导项               |          计算 |       结果 |
| ----------------- | ----------: | -------: |
| dotp 推导 matvec 数  | 250,015 / 5 |   50,003 |
| MatLd 推导 matvec 数 | 300,035 / 6 | 50,005.8 |
| 估计实际 matvec 数     |    约 50,000 |          |

这说明测试指令流仍然符合新模型：每个 matvec 包含 5 条 dotp 和 6 条 SPM load。

## 4. Cycles / matvec

按 50,000 个 matvec 计算：

```text
155,275 / 50,000 = 3.1055 cycles/matvec
```

按 dotp 计数推导的 50,003 个 matvec 计算：

```text
155,275 / 50,003 ≈ 3.1053 cycles/matvec
```

按 MatLd 计数推导的 50,005.8 个 matvec 计算：

```text
155,275 / 50,005.8 ≈ 3.1051 cycles/matvec
```

| 口径                | cycles/matvec |
| ----------------- | ------------: |
| 按 50,000 matvec   |         3.106 |
| 按 dotp 推导 matvec  |         3.105 |
| 按 MatLd 推导 matvec |         3.105 |

## 5. FU busy / 瓶颈数据

| FU 类型         |  busy 次数 |     占比 |
| ------------- | -------: | -----: |
| `CFDDSADotp`  |   49,987 | 49.51% |
| `CFDDSAMatLd` |   49,991 | 49.51% |
| `MemWrite`    |      428 |  0.42% |
| `IntAlu`      |      508 |  0.50% |
| 总 `fuBusy`    |  100,973 |      — |
| `fuBusyRate`  | 0.133726 |      — |

这组最关键的现象是：`CFDDSADotp` 和 `CFDDSAMatLd` 的 busy 几乎完全相等，都是约 `50K`。说明在 `Dotp count=4 + ReadPort count=4` 下，计算侧和 SPM load 侧达到了比较平衡的状态，不再是单边明显瓶颈。

## 6. 发射宽度分布

| 每周期发射数 |    周期数 |     占比 |
| -----: | -----: | -----: |
|      0 | 16,809 | 12.83% |
|      1 |  2,394 |  1.83% |
|      2 |  2,310 |  1.76% |
|      3 |  2,108 |  1.61% |
|      4 |  1,988 |  1.52% |
|      5 |  1,707 |  1.30% |
|      6 | 26,597 | 20.30% |
|      7 | 50,979 | 38.92% |
|      8 | 26,102 | 19.93% |

| 汇总项                        |       数值 |
| -------------------------- | -------: |
| `numIssuedDist::mean`      | 5.764203 |
| `numIssuedDist::stdev`     | 2.589933 |
| `numIssuedDist::max_value` |        8 |

这组的高发射周期占比很高：`7 issue/cycle` 占 `38.92%`，`8 issue/cycle` 占 `19.93%`，说明 O3 后端利用率已经非常高。

## 7. SPM 逻辑读量推导

```text
每条 lmat5_spm = 40B
issued CFDDSAMatLd = 300,035
```

因此：

```text
implied SPM read bytes = 300,035 × 40B = 12,001,400B
≈ 11.45 MiB
```

按 50,000 个 matvec 计算：

```text
12,001,400B / 50,000 = 240.028B / matvec
```

这继续吻合当前数据流：

```text
5 行矩阵 × 40B + 1 个向量 × 40B = 240B / matvec
```

## 8. 分支预测数据

| 指标                              |       数值 |
| ------------------------------- | -------: |
| `branchPred.lookups total`      |   40,243 |
| `branchPred.committed total`    |   31,658 |
| `branchPred.mispredicted total` |    1,131 |
| `branchMisspredict`             |    1,461 |
| `BTBLookups`                    |   40,243 |
| `BTBHits`                       |   31,155 |
| `BTBHitRatio`                   | 0.774172 |
| `condPredicted`                 |   35,364 |
| `condIncorrect`                 |    1,131 |

分支行为与前几组基本一致，不是这组性能变化的主要原因。

## 9. 与前几组的对比

| 配置                   |  cycles |   IPC | issueRate | cycles/matvec | Dotp busy | MatLd busy | 主要瓶颈                  |
| -------------------- | ------: | ----: | --------: | ------------: | --------: | ---------: | --------------------- |
| Dotp2 + MatLd2 + RP2 | 205,283 | 3.573 |     3.678 |         4.106 |    99,990 |    149,996 | MatLd                 |
| Dotp2 + MatLd2 + RP4 | 180,265 | 4.069 |     4.189 |         3.605 |   124,981 |     12,507 | Dotp                  |
| Dotp4 + MatLd2 + RP4 | 155,275 | 4.724 |     4.863 |         3.106 |    49,987 |     49,991 | Dotp / MatLd balanced |

关键变化：

```text
Dotp count 从 2 增加到 4 后：
cycles/matvec 从约 3.605 降到约 3.106；
Dotp busy 从 124,981 降到 49,987；
MatLd busy 从 12,507 增加到 49,991；
两侧资源压力趋于均衡。
```

## 10. 建议放进报告的摘要表

| 类别                |                                             关键值 |
| ----------------- | ----------------------------------------------: |
| 配置                | `Dotp opLat=7,count=4`; `MatLd opLat=2,count=4` |
| 总周期               |                                         155,275 |
| IPC / CPI         |                             4.723697 / 0.211699 |
| issueRate         |                                        4.862830 |
| cycles/matvec     |                                         ≈ 3.106 |
| issued Dotp       |                                         250,015 |
| issued MatLd      |                                         300,035 |
| Dotp:MatLd 比例     |                                             5:6 |
| implied matvecs   |                                        ≈ 50,000 |
| implied SPM read  |                                    12,001,400 B |
| SPM read / matvec |                                         ≈ 240 B |
| 主要瓶颈              |                   Dotp / MatLd roughly balanced |
| Dotp FU busy      |                                 49,987 / 49.51% |
| MatLd FU busy     |                                 49,991 / 49.51% |

## 最核心结论

这组 `Dotp count=4 + MatLd opLat=2 + ReadPort count=4` 达到约 **3.106 cycles/matvec**，比 `Dotp count=2 + ReadPort count=4` 的约 **3.605 cycles/matvec** 进一步提升。此时 `CFDDSADotp` 与 `CFDDSAMatLd` 的 FU busy 几乎相等，说明计算吞吐和 SPM load 吞吐已经比较均衡。
