# 当前文件入口索引

| 内容 | 入口 |
|---|---|
| 加速器控制器 | `src/arch/arm/cfd_coeff_preprocess_controller.cc`及`.hh` |
| 共享算术、RF、广播 | `src/arch/arm/cfd_coeff_preprocess_cluster.inc` |
| 逐lane计算 | `src/arch/arm/cfd_coeff_preprocess_lane_mvm.inc` |
| LU/RHS行更新 | `src/arch/arm/cfd_coeff_preprocess_lu_rows.inc`、`cfd_coeff_preprocess_rhs_rows.inc` |
| D 独立就绪与输入验收 | `cfd_coeff_preprocess_input_bursts.inc`、`projects/cfd_dsa/tools/check_input_split.py` |
| Tile/SPM/后向 | `cfd_coeff_preprocess_tiles.inc`、`cfd_coeff_preprocess_onchip.inc`、`cfd_coeff_preprocess_backward.inc` |
| 按cell回读选择器 | `src/arch/arm/cfd_tile_stream.hh`，复用现有Tile传输计数 |
| 除法器/RF独立资源类 | `src/arch/arm/cfd_fp64_divider.hh`、`cfd_register_ports.hh` |
| gem5单线入口 | `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_single_line.py` |
| guest/参考求解 | `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c`及同目录include |
| 运行和验收 | [tools/README](../tools/README.md) |
| 测试与原生fixtures | [tests/README](../tests/README.md) |
| 选实验 | [suites/README](../suites/README.md) |
| 数据/实验结果 | [results/README](/home/zyy/gem5/results/cfd_dsa/README.md) |
| 当前清理与保留规则 | [RESULT_LIFECYCLE](RESULT_LIFECYCLE.md) |

历史阶段方案见`history/`；旧原始结果和大型快照已清理，不能作为当前配置或可恢复证据。
