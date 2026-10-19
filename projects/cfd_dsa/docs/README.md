# CFD-DSA 文档入口

## 当前维护入口

- [单线研究主线](CFD_DSA_SINGLE_LINE.md)：仅一条网格线、一次前向和一次后向；单线入口、最终向量 DMA、容量账本及验收边界。
- [单线 D2/D3 与重叠](CFD_DSA_SINGLE_LINE_D.md)：有限 tile/系数缓存、整轮接口计时、双缓冲及模式选择。
- [单线第四轮修复与优化（2026-10-19）](CFD_DSA_SINGLE_LINE_ROUND4_20261019.md)：失败缓存及异常收尾、统计修正、接口聚合、有限缓存策略、DMA入口限速和实验溯源。
- [项目地图](CFD_DSA_PROJECT_STRUCTURE.md)：目录、ISA/FU、数据与控制边界。
- [文件索引](CFD_DSA_PROJECT_FILE_INDEX.md)：逐文件定位；移动代码后同步更新。
- [Step2 模式说明](CFD_DSA_LUSGS_STEP2.md)：prepared/raw、coarse/event 的语义。
- [Streaming 实现](CFD_DSA_LUSGS_STEP2_STREAMING.md)：line、列消费、复用及执行边界。
- [结果生命周期](RESULT_LIFECYCLE.md)：结果保留、引用和清理纪律。
- [精简进度](CFD_DSA_SIMPLIFICATION.md)：各阶段修改、验证、保留边界和后续入口。

当前实验基线是固定资源 serial/divide 的 2/2 上下文，2/4 为性能参照；
这不是修改各入口默认值的授权。具体配置由 suite 和实际代码确定，不从历史报告推断。

## 专题与实验

- [当前单线流水与瓶颈检查（2026-10-15）](CFD_DSA_SINGLE_LINE_AUDIT_20261015.md)：逐周期气泡、资源/队列利用率、SPM读取串行门控、DMA桥接模型，以及65点和当前推荐配置的吞吐补测。
- [单线第二轮流水与存储优化（2026-10-12）](CFD_DSA_SINGLE_LINE_ROUND2_20261012.md)：完成/输入/输出生命周期解耦、逆向 tile 留存、LU 同列并行、SPM bank/端口模型及最终验收；含固定资源和重新选参的独立对照。
- [单线流水优化（2026-10-12）](CFD_DSA_SINGLE_LINE_OPTIMIZATION_20261012.md)：前沿准入、精简记录、warm DMA 重叠、cold 乘积提前计算和参数扫描；含同二进制对照与推荐 suite。
- [上下文消融](CFD_DSA_CONTEXT_ABLATION.md)
- [调度诊断](CFD_DSA_SCHEDULER_DIAGNOSTICS.md)
- [request aging 实验](CFD_DSA_REQUEST_FRONTIER_AGING.md)：2/2退化，非推荐默认。
- [单线逐 RHS 调度实验](CFD_DSA_SINGLE_LINE.md#2026-09-23-单线调度验证)：固定资源的数值、请求次数及周期对照；2/4 有小幅收益，2/2 退化。
- [Path A](CFD_DSA_IMPLEMENTATION.md)、[Path B](CFD_DSA_PATHB_ZA_PIPE.md)、
  [Path C](CFD_DSA_SME.md)、[TRSV5](CFD_DSA_TRSV5.md)
- [Step1](CFD_DSA_LUSGS_STEP1.md)、[Step2 core](CFD_DSA_LUSGS_STEP2_CORE.md)、
  [pretransform](CFD_DSA_LUSGS_PRETRANSFORM.md)、[Step3](CFD_DSA_LUSGS_STEP3.md)、
  [Step4](CFD_DSA_LUSGS_STEP4.md)：保留兼容与独立研究说明。

## 冻结的历史快照

以下文档不再维护当前默认值和推荐路径，仅保留原版本证据与勘误：

- [版本审查](CFD_DSA_VERSION_RETENTION_ANALYSIS.md)
- [版本保留汇总](CFD_DSA_VERSIONS_AND_RETENTION.md)
- [完整技术报告](CFD_DSA_LUSGS_FULL_TECHNICAL_REPORT.md)
- [历史性能分析](performance_analysis.md)、[历史测试报告](test_report.md)

审计记录：[既有清理报告](CFD_DSA_CLEANUP_REPORT_2026-09-09.md)、
[Git方案标签](GIT_SCHEME_TAGS.md)。未完成引用审查的源码不删除；
历史文档不等于无用文档，不以最新性能数字覆盖旧证据。

单线第三轮优化：[2026-10-15后向预读、数值ready/写回解耦、160B记录与容量收缩](CFD_DSA_SINGLE_LINE_ROUND3_20261015.md)。
