# CFD-DSA Documentation Index

- `CFD_DSA_VERSION_RETENTION_ANALYSIS.md`: 2026-09-09 各阶段方案与版本盘点、主线选择、保留/迁档/删除候选及证据边界。
- `GIT_SCHEME_TAGS.md`: Git 完整快照及各方案 annotated tag 的含义与入口。

- `CFD_DSA_IMPLEMENTATION.md`: canonical Path A implementation document.
- `CFD_DSA_SME.md`: Path C SME/ZA implementation.
- `CFD_DSA_PATHB_ZA_PIPE.md`: Path B ZA pipeline.
- `CFD_DSA_TRSV5.md`: standalone TRSV5 ISA and FU.
- `CFD_DSA_LUSGS_STEP2.md`: 当前 Step2 模式族的权威执行路径说明，覆盖 prepared/raw、TRSV/pretransform、dual/coeff3、coarse/event、context/linebuf、ISA/FU/SPM、统计与回退边界。
- `CFD_DSA_LUSGS_STEP2_STREAMING.md`: Step2 Streaming Stage A+B1.5+B2.5+B3+B4 的 per-cell window、auto-retire、boundary 5/10/15-RHS、共享 DInv/Lbar ColumnFma5、controller-local dq-star handoff、ForwardCombine、timed dq-star publication、off/shadow/direct、性能/回归，以及尚未实现的 tail priority、backward frontier 和 multi-line 边界。
- `CFD_DSA_LUSGS_STEP2_CORE.md`: Step2 fused MVM-sub, forward RHS-forwarded TRSV5, software line-context, and hardware line-buffer forwarding implementation/results.
- `CFD_DSA_LUSGS_PRETRANSFORM.md`: legacy prepared-input 与 raw D/L/U/R 路径、coarse/event 公共 LU/Ubar 预处理、RHS15 四种 scheduler、LU step-ready、共享 CfdLocalSpm request/response、三种 SPM layout、div1/div2、动态 auto、cancel、software-LU/event-TRSM、压力回归和公平回本点。
- `CFD_DSA_LUSGS_STEP1.md` through `STEP4.md`: staged LU-SGS work.
- `CFD_DSA_LUSGS_FULL_TECHNICAL_REPORT.md`: consolidated LU-SGS report.
- `performance_analysis.md` and `test_report.md`: comparison artifacts.
- `RESULT_MANAGEMENT.md`: directory ownership, naming, retention, and commands.
- `CFD_DSA_VERSIONS_AND_RETENTION.md`: 各阶段方案/版本清单与保留删除建议（决策性汇总；必留/回归/研究线/可删/待审查五档）。

Measured evidence referenced by these documents belongs under
`results/cfd_dsa/`, not beside the source files.

Coefficient-event one-factor sensitivity campaign:
`python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/coeff-event-sensitivity.json`.

最终证据目录包括 `coeff-event-packet-formal-s16`、`coeff-event-layout-perf-8x256`、
`coeff-event-auto-correctness`、`coeff-event-cancel-correctness-v3`、
`coeff-event-software-lu-timed` 和 `core-regression-coeff-final`。
