# CFD-DSA 文件与结果管理

源码在 `src/` 和 `projects/cfd_dsa/`，可再生成二进制在 `build/`，输入与结果在 `results/cfd_dsa/`。

## 保留规则

1. 日常保留当前采用方案、必要基线、独立输入和边界/恢复验证。原始输入及其来源信息必须保留。
2. 失败、退化和已替代方案只保留简短结论、参数、指标与验收状态；结束诊断后删除大轨迹和旧可执行程序。
3. 不再为每轮保留一份 gem5 二进制，不再把所有旧结果搬进 archive 长期堆积。Git 不收录实验二进制、轨迹和大型结果包。
4. 正在运行或被其他聊天使用的结果不能清理。清理前检查当前入口、输入链接和活跃实验。
5. 保留的完整证据可无损压缩；已经删除原始轨迹的结果必须标明仅剩摘要，不能宣称可重新重放验收。
6. 标准运行入口在每例启动前检查结果盘，余量低于 10 GiB 时拒绝运行。直接调用 gem5 的脚本需要自行检查磁盘余量。每轮收敛后清理淘汰候选。

## 运行入口

```bash
python3 projects/cfd_dsa/tools/run_experiment.py \
  --target lusgs-single-line --campaign <新名称> --case <算例> -- <配置参数>
python3 projects/cfd_dsa/tools/run_suite.py <套件.json> --campaign <新名称>
```

结果目录拒绝覆盖；`development` 使用时间戳。每例的 `resolved_experiment.json` 记录有效参数，`manifest.json` 与验收记录说明当时执行状态。

2026-10-30 清理删除了旧冷归档与过期大文件。以前文档中的无损归档/可恢复承诺不再适用于已删除数据，当前保留范围以 `results/cfd_dsa/README.md` 为准。
