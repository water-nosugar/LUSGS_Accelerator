# 工具入口

日常只需要以下几个入口，其余`check_*`等文件是共享验收实现，不是重复的独立测试程序。

| 入口 | 用途 |
|---|---|
| `build_benchmarks.py` | 编译guest，支持按目标选择 |
| `run_experiment.py` | 单例仿真、来源快照及完整验收 |
| `run_suite.py` | 批量运行当前或显式历史套件 |
| `prepare_single_line_study.py` | 生成可复现的研究输入 |
| `single_line_config_audit.py` | 有效参数和容量拆账 |
| `analyze_round1_diagnostics.py` | 流式重放 RHS 配对快照、实际 RF 返回、MVM 依赖和 Tile DMA/SPM 分段 |
| `artifact_archive.py` | 显式处理外部存档；本地旧存档已删除 |

无引用的旧轮次分析和迁移脚本已删除；独立前缀扫描原型在`../experiments/affine_prefix.py`。
普通回归统一运行`python3 -B -m unittest discover -s projects/cfd_dsa/tests`。

第一轮分析入口接受案例目录及`--output <JSON路径>`。当前后向有符号树配置启用`--coeff-round1-diagnostics=1`后可获得完整绑定；`--allow-legacy`支持旧日志已有的 RF 和 DMA/SPM 事件。配对快照和各操作延迟相互重叠，不作为整轮损失相加。完整使用方法见[第一轮结果](../docs/CFD_DSA_ROUND1_DIAGNOSTICS_20261028.md)。
