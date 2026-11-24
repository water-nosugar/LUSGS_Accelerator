# CFD-DSA 清理执行报告（2026-09-09）

## 1. 执行原则

本轮依据 `CFD_DSA_VERSION_RETENTION_ANALYSIS.md` 清理已确认的生成物、兼容入口和
不可达实现。主线 Step2 raw event streaming、C1/C2/B5 wavefront、软件 reference、
Path A、TRSV5、off/shadow 对照，以及 Step1/3/4、Path B/C 的回归实现均保留。

清理前已经用 Git snapshot 和方案标签保存完整实现，因此被删除的源码仍可从
`snapshot/cfd-dsa-2026-09-09` 或相应 `scheme/*-v1` 标签恢复。

## 2. 已删除源码

- 根目录九个 `run_cfd_*.py` 转发 shim；canonical 配置入口仍在
  `projects/cfd_dsa/configs/`。
- Path C 的 include-only compatibility source 与 `pathc-compat` build target。
- decoder 已拒绝的 fixed-ZA FMOPA guest source 与 `sme-step` build target。
- `DSACFDSMEFmopa5Step` 的声明/实现、专用 OpClass、FUDesc 和默认 FUPool 项。
- 全仓无构造引用的 `DSAGetAcc`、`DSAVsetZero` 声明与实现。
- 未被 ARM ISA 生成器 include 的 `src/arch/arm/isa/insts/cfd_dsa.isa` 说明 stub。
- Path B decoder 分支中位于无条件 `return` 之后的不可达语句。

旧 `CFDSMEFmopaStep` 的统计名称仍在 Python 报告解析器中保留，只用于读取历史
`stats.txt`，不再对应可执行指令、FU 或命令行调参入口。

## 3. 已清理生成物

- `projects/cfd_dsa/**/__pycache__/`
- `projects/cfd_dsa/**/*.pyc`

这些对象由 Python 自动生成且已被 `.gitignore` 排除，可随时重新生成。

## 4. 明确保留

- `results/cfd_dsa/archive/`、`campaigns/`、`baselines/`：历史或正式证据。
- `results/cfd_dsa/runs/`：包含仍被报告直接引用的开发运行，本轮不按目录批量删除。
- `results/cfd_dsa/failed/`：保留 DMA 多轮重建等失败诊断，不按“失败”名称直接删除。
- `results/lusgs_raw/`、`results/lusgs_optprep/`：虽然是旧布局，但当前
  `migration-inventory.json` 未记录这两个来源前缀，且
  `CFD_DSA_LUSGS_PRETRANSFORM.md` 仍直接引用，待补齐一一映射后再迁档。
- 根目录未跟踪文件 `三对角矩阵求解.c` 和用户已有的 `.vscode/settings.json` 修改：
  来源不属于本轮已确认清理闭包，保持原状。

## 5. 验证

| 检查 | 结果 |
| --- | --- |
| Python config/build tool 语法 | PASS；配置被重建后的 gem5 和 suite 正常加载 |
| 全部保留 guest benchmark 构建 | PASS；11/11 targets，`-O3 -static -Wall -Wextra` |
| `build/ARM/gem5.opt` 增量构建 | PASS；包含 ISA 重新生成、OpClass/FU/C++ 重编译和最终链接 |
| decode-exclusive / core regression | PASS；10/10 cases |
| 全仓删除对象运行时引用检查 | PASS；仅保留文档历史描述和旧 stats 字段解析 |
| `git diff --check` | PASS |

回归证据位于
`results/cfd_dsa/campaigns/cleanup-validation-2026-09-10/`，包含每个 case 的
manifest、simout 和 stats。用例覆盖 Path A/B/C、TRSV5、decode-exclusive、Step1、
Step2、Step2 raw streaming、Step3 与 Step4-C。

## 6. 后续可选清理

结果目录只能依据“文档引用 + manifest 配置 + 诊断价值”逐目录审查。不得按
`debug`、`final2`、`failed` 等名称批量删除。完成旧 `lusgs_*` 目录的 hash/manifest
映射后，可先归档并经过一个观察周期，再移入回收站。
