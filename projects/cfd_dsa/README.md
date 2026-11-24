# CFD-DSA：单线 LU-SGS 加速器

当前研究只处理**一条网格线的一次前向和一次后向扫描**。gem5实现位于仓库的`src/arch/arm/`，本目录保存运行入口、guest、验收代码和说明。

最新单通道优化、5/5/1与8/7/1的实际取舍及复现入口见[单通道优化说明](docs/CFD_DSA_SINGLE_CHANNEL_OPT_20261026.md)。

## 日常只看这些入口

| 工作 | 位置 |
|---|---|
| 理解当前架构和实验结论 | [docs/README.md](docs/README.md) |
| 选择测试输入 | [results/cfd_dsa/datasets/current](/home/zyy/gem5/results/cfd_dsa/datasets/current) |
| 选择一套实验 | [suites/README.md](suites/README.md) |
| 运行全部单元测试 | [tests/README.md](tests/README.md) |
| 找当前结果与保留范围 | [results/cfd_dsa/README.md](/home/zyy/gem5/results/cfd_dsa/README.md) |

```bash
# 350项回归：数值、算术、SPM/RF、生命周期、数据与归档
python3 -B -m unittest discover -s projects/cfd_dsa/tests

# 编译单线guest（不是重新编译gem5）
python3 projects/cfd_dsa/tools/build_benchmarks.py lusgs-step2

# 对照实验；每次使用新的campaign名
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/single-line-lu-rhs-20261029.json \
  --campaign <新的实验名称>
```

## 目录

```text
benchmarks/    guest源代码
configs/       gem5配置与有限资源契约
tools/         统一运行入口及共享验收模块
suites/        日常套件；history/仅保留工具依赖的4个定义
tests/         按功能分组的回归；fixtures/统一放原生测试源
experiments/   独立算法原型，不是gem5时序实现
docs/          当前文档；history/为历史方案和阶段报告
visualizer/    项目可视化
```

2026-10-30 已删除无引用的历史专用脚本、84 个历史试验套件、旧冷归档和过期大文件。
当前仿真实现与验收模块保留。以前报告中的归档恢复说明已过期；保留规则见[结果管理](docs/RESULT_LIFECYCLE.md)。

最新实现：[10/10/1双五分量发射与局部数据通路](docs/CFD_DSA_DUAL_GROUP_20261027.md)，包含长线配置与65点完整缓存配置的对照。

当前供给与完成记录优化：[有界快照与摘要引擎](docs/CFD_DSA_SUPPLY_OPT_20261027.md)，保持10/10/1、单通道DDR及原RF/SPM端口约束。

最新局部乘积与 RF 消融：[局部乘积结果及适用边界](docs/CFD_DSA_PRODUCT_LOCAL_OPT_20261027.md)。包含固定 guest 布局后的 5/5/1 对照、4096 点验证及 71 点容量边界；实验开关按工作负载选择，日常套件保持原供给配置。

Tile16 下按整轮周期选择的两套 1024 点冷轮参数：[5/5/1 与 10/10/1 最佳已测组合](docs/CFD_DSA_CYCLE_BEST_20261027.md)。分别为 136042、129331 周期；[对照套件](suites/single-line-cycle-best-20261027.json)包含两个命名配置，保持同一冻结实现和输入。

第一轮观测与优化依据：[跨 RHS 配对、反馈返回与 Tile 分段诊断](docs/CFD_DSA_ROUND1_DIAGNOSTICS_20261028.md)。诊断开关经整轮对照验证，下一轮优先考虑有界双 RHS 因子共享及长线数据供给。

第三轮已测长线配置：[Tile24 取数覆盖与短线缓存保护](docs/CFD_DSA_ROUND3_TILE_PREFETCH_20261028.md)。10/10/1训练1024点降至128767周期，另一组1024及4096点也分别减少0.3321%和0.1322%；[新套件](suites/single-line-tile24-20261028.json)显式保留65/71/72点的Tile16完整缓存。新增Tile载荷11520 B，仍在256 KiB预算内；第二轮RHS配对保持关闭。

第四轮 LU/RHS 预处理：[双乘积流水、因子跨周期保持及完整消融](docs/CFD_DSA_LU_RHS_OPT_20261029.md)。[当前实测配置与对照](suites/single-line-lu-rhs-20261029.json)按长线结果和65/71/72点完整缓存预算选择开关；源码全局默认保持关闭。
