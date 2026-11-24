# 文档入口

| 阅读目的 | 文档 |
|---|---|
| 第四轮 LU/RHS 预处理与有限流水 | [双乘积槽、因子保持及最终验证](CFD_DSA_LU_RHS_OPT_20261029.md) |
| 项目使用与目录 | [项目README](../README.md) |
| 当前单线实现 | [单线主线](CFD_DSA_SINGLE_LINE.md) |
| 第一轮配对机会、反馈返回与 Tile 延迟诊断 | [观测结果与优化优先级](CFD_DSA_ROUND1_DIAGNOSTICS_20261028.md) |
| 第二轮公共 LU 操作数及缓存边界实测 | [双 RHS 实现、有限收益与负面边界](CFD_DSA_ROUND2_RHS_PAIR_20261028.md) |
| 第三轮长线 Tile24 及短线缓存保护 | [取数等待、容量代价与三组完整 sweep 对照](CFD_DSA_ROUND3_TILE_PREFETCH_20261028.md) |
| 以整轮周期选择5/5/1与10/10/1的参数版本 | [两套最佳已测组合](CFD_DSA_CYCLE_BEST_20261027.md) |
| 局部乘积/RF消融、同版5/5/1与容量边界 | [局部乘积结果及适用边界](CFD_DSA_PRODUCT_LOCAL_OPT_20261027.md) |
| 单通道下减少控制流量、5/5/1与8/7/1取舍 | [单通道优化](CFD_DSA_SINGLE_CHANNEL_OPT_20261026.md) |
| 算术共享、固定分组与借用 | [四种资源组织](CFD_DSA_RESOURCE_PARTITION_20261026.md) |
| 冷轮 R 供给与 RHS 分批启动 | [冷轮 R 预取](CFD_DSA_COLD_RHS_OPT_20261026.md) |
| D 输入与 LU 提前启动 | [独立 D 就绪](CFD_DSA_FORWARD_INPUT_OPT_20261026.md) |
| 后向依赖与回读顺序 | [独立就绪和按cell回读](CFD_DSA_READY_SUPPLY_OPT_20261026.md) |
| 本轮架构候选与取舍 | [广播、逐lane与局部通路](CFD_DSA_LANE_DESIGN_20261025.md) |
| 当前供给/RF配置 | [单线供给优化](CFD_DSA_STREAM_SUPPLY_OPT_20261025.md) |
| 除法器资源协议 | [除法器模型](CFD_DSA_DIVIDER_MODEL_20261021.md) |
| 数据、CPU基线和评测口径 | [研究评测](CFD_DSA_STUDY_OPT_20261024.md) |
| GT01R来源及投影限制 | [真实系数数据](CFD_DSA_GT01R_20261022.md) |
| 当前清理与结果保留 | [结果管理](RESULT_LIFECYCLE.md) |

[历史报告](history/)保留各阶段方案和负面结果，不代表当前参数或推荐开关。
有效参数以每例`resolved_experiment.json`和`configuration_audit.json`为准。

- [10/10/1双五分量发射与局部数据通路](CFD_DSA_DUAL_GROUP_20261027.md)

供给与完成记录优化见[有界快照与摘要引擎](CFD_DSA_SUPPLY_OPT_20261027.md)，含硬件代价、时序与验收说明。
