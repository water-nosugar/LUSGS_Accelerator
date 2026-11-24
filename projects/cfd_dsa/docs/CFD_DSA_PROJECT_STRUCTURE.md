# 当前项目结构

当前研究是单线LU-SGS。日常操作先看[项目README](../README.md)，历史阶段报告集中在[history](history/)。

```text
src/arch/arm/                加速器实现、算术资源、SPM和DMA生命周期
projects/cfd_dsa/
  benchmarks/lusgs/          ARM guest、数值参考、数据文件读取
  configs/lusgs/             单线入口和参数profile
  configs/single_line_contract.py  容量与布局契约
  tools/                     运行、数据生成、共享验收
  tests/                     按7类分组；fixtures为原生C/C++测试源
  suites/                    当前套件；history仅保留4个工具依赖定义
  experiments/               独立算法原型
  docs/                      当前说明；history为40篇旧报告
results/cfd_dsa/
  current/                   6组当前对照结果的链接
  datasets/current/          7个冻结输入的链接
  campaigns/                 当前完整证据及旧实验小型摘要
  analysis/                  结论、分析代码、输入及最新实验
  maintenance/               清理与验证记录
```

测试和分析代码不代表另一套硬件实现。算术模块、上下文、SPM、队列与DMA的边界以当前设计文档和每例配置审计为准。
生成二进制仍放在`build/`，不会混入源码或数据目录。
