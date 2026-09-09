# CFD-DSA 测试报告

## 1. 报告目的

基于 [performance_analysis.md](/home/zyy/gem5/projects/cfd_dsa/docs/performance_analysis.md) 中已有测试结果，对 CFD-DSA 在不同 FU 参数组合下的性能进行统一口径分析、横向对比与瓶颈定位，输出可直接用于汇报和后续调参的结论。

## 2. 数据来源与口径

- 数据来源：`performance_analysis.md`（覆盖 6 组配置）。
- 统一口径：
  - 每个 matvec 指令流按新模型统计：`5 × dotp_row + 6 × lmat5_spm`。
  - 每条 `lmat5_spm` 逻辑读取 `40B`，所以 `SPM 逻辑读 = 240B/matvec`。
  - `cycles/matvec` 以文档给出的 `numCycles / 50,000` 为主，辅以 dotp/MatLd 反推值校验。

## 3. 测试配置总览

| Case | Dotp (`count/opLat`) | MatLd (`opLat`) | ReadPort | cycles | IPC | cycles/matvec |
|---|---:|---:|---:|---:|---:|---:|
| 1-1-2 | 1 / 7 | 1 | 2 | 305,230 | 2.403 | 6.105 |
| 1-1-8 | 1 / 7 | 1 | 8 | 305,212 | 2.403 | 6.104 |
| 2-1-2 | 2 / 7 | 1 | 2 | 205,282 | 3.573 | 4.106 |
| 2-2-2 | 2 / 7 | 2 | 2 | 205,283 | 3.573 | 4.106 |
| 2-2-4 | 2 / 7 | 2 | 4 | 180,265 | 4.069 | 3.605 |
| 4-2-4 | 4 / 7 | 2 | 4 | 155,275 | 4.724 | 3.106 |

## 4. 关键一致性验证（数据流是否生效）

从各组 `CFDDSADotp` 与 `CFDDSAMatLd` 计数看：

- `dotp` 基本在 `250,010` 左右，`matld` 基本在 `300,024~300,035`。
- 比例稳定接近 `5:6`，与新模型一致。
- 推导 SPM 逻辑读量：
  - `300,024 × 40B = 12,000,960B`
  - 约 `240B/matvec`（50K 级 workload）

结论：**“向量也通过 SPM 读取（row=0）”已经在指令流中被稳定体现。**

## 5. 分组对比分析

### 5.1 `1-1-2` vs `1-1-8`（只增 ReadPort：2 → 8）

- cycles：`305,230 → 305,212`，仅下降 `18` cycles（`0.0059%`）。
- `MatLd FU busy`：`87,546 → 0`，但总性能几乎不变。

结论：在 `Dotp count=1` 下，主瓶颈仍是 dotp 引擎吞吐，继续增加 ReadPort 对总周期收益极小。

### 5.2 `1-1-2` vs `2-1-2`（只增 Dotp count：1 → 2）

- cycles：`305,230 → 205,282`，下降 `32.75%`。
- cycles/matvec：`6.105 → 4.106`，明显改善。
- 瓶颈迁移：`Dotp` 主瓶颈转为 `MatLd` 主瓶颈（MatLd busy 59.72%）。

结论：**增加 Dotp count 是高收益优化项**，但会把瓶颈推向 load 发射侧。

### 5.3 `2-1-2` vs `2-2-2`（只改 MatLd opLat：1 → 2）

- cycles：`205,282 → 205,283`，几乎不变（+1 cycle）。
- IPC/CPI、FU busy 几乎一致。

结论：当前流水调度下，`MatLd opLat`（1→2）被隐藏，性能主要不受单条 MatLd 延迟影响，而受 `ReadPort count` 影响。

### 5.4 `2-2-2` vs `2-2-4`（只增 ReadPort：2 → 4）

- cycles：`205,283 → 180,265`，下降 `12.19%`。
- cycles/matvec：`4.106 → 3.605`。
- `MatLd busy`：`149,996 → 12,507`，端口瓶颈明显缓解。
- 瓶颈重新回到 `Dotp`。

结论：在 `Dotp count=2` 下，`ReadPort count=4` 是有效提升点。

### 5.5 `2-2-4` vs `4-2-4`（只增 Dotp count：2 → 4）

- cycles：`180,265 → 155,275`，下降 `13.86%`。
- cycles/matvec：`3.605 → 3.106`。
- `Dotp busy` 与 `MatLd busy` 几乎相等（约 `49.5%` vs `49.5%`）。

结论：系统进入较平衡状态，计算与加载两侧吞吐接近匹配。

## 6. 总体趋势与瓶颈迁移

按调参路径可归纳为：

1. `Dotp1` 阶段：Dotp 为绝对瓶颈，ReadPort 扩容收益接近 0。
2. `Dotp2` 阶段：性能大幅提升，但瓶颈转移到 MatLd/ReadPort。
3. `Dotp2 + RP4`：ReadPort 补齐后再次提速，瓶颈回到 Dotp。
4. `Dotp4 + RP4`：两侧资源趋于均衡，达到当前最优 `~3.106 cycles/matvec`。

相对起点 `1-1-2`，`4-2-4` 的总周期下降约 `49.13%`。

## 7. 风险与注意项

- `MatLd` 计数存在少量额外指令（如 prologue/边界效应），因此 `dotp` 与 `matld` 反推 matvec 数会有极小偏差（约 +2~+6 量级），属于预期。
- 文档中有的组使用 `issued`，有的组使用 `committed`；报告里已按“趋势分析”处理，不影响结论方向。
- `system.spm.bytesRead` 在 direct-functional 路径下不可靠，SPM 逻辑读应继续使用 `CFDDSAMatLd × 40B` 推导。

## 8. 结论

1. 新数据流（每个 matvec 6 条 SPM load）已被测试数据验证。
2. 最有效的性能提升路径不是单独调 `MatLd opLat`，而是匹配 Dotp 与 ReadPort 吞吐。
3. 在当前测试集中，最佳配置是 `Dotp count=4, MatLd opLat=2, ReadPort count=4`，达到约 `3.106 cycles/matvec`。
4. 后续若继续优化，建议围绕“吞吐平衡”而非“单条延迟”做 sweep：优先测试 `Dotp count=3/4` 与 `ReadPort count=4/8` 的组合。

## 9. 建议的下一轮实验矩阵

| Dotp count | MatLd opLat | ReadPort count | 目的 |
|---:|---:|---:|---|
| 3 | 1 | 4 | 验证 3 路 dotp 是否已足够接近平衡点 |
| 3 | 2 | 4 | 验证 MatLd latency 隐藏能力在 3 路 dotp 下是否仍成立 |
| 4 | 1 | 4 | 检查降低 MatLd latency 后的边际收益 |
| 4 | 2 | 8 | 验证 RP8 对 balanced 态是否还有增益 |
