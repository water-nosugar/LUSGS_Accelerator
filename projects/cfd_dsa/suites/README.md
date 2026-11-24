# 当前测试套件

日常从下表选择；已删除无运行工具引用的历史试验套件；`history/`仅保留4个工具依赖定义。

| 目的 | 套件 |
|---|---|
| 第四轮LU/RHS：按实测选择双槽/因子保持，保护65/71/72完整缓存 | `single-line-lu-rhs-20261029.json` |
| 第三轮10/10/1长线Tile24、Tile16基线及短线缓存保护 | `single-line-tile24-20261028.json` |
| 1024点冷轮：分别调优的5/5/1与10/10/1周期对照 | `single-line-cycle-best-20261027.json` |
| 局部乘积/RF消融、同版5/5/1及71点容量边界 | `single-line-product-local-20261027.json` |
| 单通道控制流量优化，原5/5/1与优化5/5/1、8/7/1 | `single-line-single-channel-optimized-20261027.json` |
| 当前单线资源与供给对照 | `single-line-stream-supply-optimized-20261025.json` |
| 公共因子广播开关对照 | `single-line-preprocess-broadcast-20261025.json` |
| 逐lane/局部RF消融，包含负面结果 | `single-line-lane-ablation-20261025.json` |
| 独立数据与CPU研究 | `single-line-study-v2-20261024.json` |
| GT01R真实系数投影 | `single-line-gt01r-recommended.json` |
| 256～4096点规模压力 | `single-line-scale.json` |
| 单线物理发射、数值、缓存契约 | `single-line-c-complete.json` |
| direct/inverse求解对照 | `single-line-base-solve.json` |
| 输出DMA与完成顺序 | `single-line-output-dma.json` |
| 除法器有限资源协议 | `single-line-divider-model.json` |
| Ubar系数缓存回归 | `single-line-ubar-cache-regression.json` |
| 冷/热取消及恢复 | `single-line-round4-recovery.json` |
| 跨路径基础回归 | `core-regression.json` |

```bash
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/single-line-preprocess-broadcast-20261025.json \
  --campaign <新的实验名称>
```

新实验使用新名称，不覆盖历史目录。每例`resolved_experiment.json`才是有效参数来源。
广播在4096点的测试中有回退；名称含optimized/recommended不表示全部开关都应开启。
单通道新套件保留原数值检查和资源时序；其5/5/1候选只增加576 B状态预约及一个有时序的整数摘要单元。参数、热轮保护和完整规模结果见[优化说明](../docs/CFD_DSA_SINGLE_CHANNEL_OPT_20261026.md)。
旧原始结果和冷归档已清理，无法从本地归档恢复；保留范围见结果目录 README。

- `single-line-dual-group-20261027.json`：10/10/1双五分量发射、局部乘积与同版5/5/1、8/7/1对照；包含65点冷热缓存配置，详见[设计说明](../docs/CFD_DSA_DUAL_GROUP_20261027.md)。

`single-line-supply-20261027.json`：10/10/1完成快照与1/2整数摘要引擎对照、同条件5/5/1、65点缓存边界及独立4096点。解释见[供给与完成记录优化](../docs/CFD_DSA_SUPPLY_OPT_20261027.md)。

`single-line-product-local-20261027.json`：固定未启用跟踪参数后的 15 个正式验收工作点。局部乘积与广播均为消融开关；4096 点广播没有正收益，71 点局部存储预约触发缓存容量回退，不作为通用默认。完整结论见[局部乘积结果及适用边界](../docs/CFD_DSA_PRODUCT_LOCAL_OPT_20261027.md)。

`single-line-cycle-best-20261027.json`：以整轮周期最少为目标，包含 `best551-1024` 和 `best10101-1024` 两个去掉重复参数的配置。5/5/1 采用精简预处理与有界完成快照；10/10/1 采用双组发射、局部 LU/RHS 乘积及广播。该 Tile16 配方在对应输入的已测周期为 136042 和 129331；线长与冷热条件的适用范围见[比较说明](../docs/CFD_DSA_CYCLE_BEST_20261027.md)。

`single-line-tile24-20261028.json`：第三轮已测10/10/1长线配置。训练1024、另一组1024及已有4096点输入分别减少0.4361%、0.3321%、0.1322%的完整控制器周期；增加11520 B Tile载荷，仍在256 KiB预算内。套件同时包含显式Tile16基线、23/24/25与47/48/49及73点边界、49点取消恢复，65/71/72点冷热配置保持Tile16与完整缓存。第二轮RHS配对关闭，不作全局Tile默认切换。详见[第三轮结果与适用范围](../docs/CFD_DSA_ROUND3_TILE_PREFETCH_20261028.md)。

`single-line-lu-rhs-20261029.json`：LU双乘积槽与RHS跨周期因子保持的最终实测配置，保留原浮点顺序及10/10/1、RF16R/8W、单DDR约束。不同线长采用明确静态资源配置，开启冷轮功能仍支付全部硬件预留。结果与验收类型见[第四轮说明](../docs/CFD_DSA_LU_RHS_OPT_20261029.md)。
