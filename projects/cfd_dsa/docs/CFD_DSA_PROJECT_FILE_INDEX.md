# CFD DSA / LU-SGS 文件索引

2026-10-09 C阶段增量：`src/arch/arm/cfd_coeff_preprocess_cluster.inc`实现
LU/TRSM/MVM/向量操作的共享5/5/1通道、乘积信用和发射trace；controller
加入LU/Lbar/Ubar缓存复用及serial/split/tree对照。
`benchmarks/lusgs/step2_cache_contract.inc`覆盖R与系数实际更新、模式/尺寸/
版本/dirty失效；`suites/single-line-c-complete.json`与
`tools/check_single_line_c.py`逐周期核对资源预算、全量数值与依赖反馈。
`tests/test_single_line_cluster.py`验证检查器能拒绝超额发射等非法trace。
见[单线研究主线](CFD_DSA_SINGLE_LINE.md)。

2026-10-09此前计算增量：`--coeff-base-solve=direct`增加冷启动 `D*b=R`，
`suites/single-line-base-solve.json`与`tools/check_single_line_base_solve.py`
对照显式逆，检查每个RHS、固定FU配置、数值容差与DMA完成，并可导出
资源使用账本。该轮仅覆盖每sweep重建；缓存复用由上述C阶段增量补齐。见
[单线研究主线](CFD_DSA_SINGLE_LINE.md)。

2026-09-23单线调度增量：`suites/single-line-rhs-frontier.json`与
`tools/check_single_line_rhs_frontier.py`对照逐RHS前沿优先与原模式，核对
17/64点的child次数、FP64摘要及固定计算资源。控制器的`issueSolveRhs()`
复用原发射逻辑，`rhs-frontier`为可选实验模式。实测见
[单线调度验证](CFD_DSA_SINGLE_LINE.md#2026-09-23-单线调度验证)。

2026-09-22单线增量：`configs/lusgs/run_cfd_lusgs_single_line.py`及
`single_line_profile.py`为单线范围校验入口；`suites/single-line-output-dma.json`
与`tools/check_single_line_output_dma.py`验证最终向量timing写回；
`tools/analyze_single_line_storage.py`计算容量账本；`tests/test_single_line.py`
覆盖范围、完成条件和容量边界。实现与验收状态见
[单线研究主线](CFD_DSA_SINGLE_LINE.md)。

2026-09-12增量：`projects/cfd_dsa/configs/cfd_reporting.py`为active Python统计模块，
`ReportContext`位于第44行，生产者为runner的配置绑定，消费者为stats-only及仿真后报告；
只依赖标准库和显式配置，不构建SimObject。
`projects/cfd_dsa/tools/check_reporting_compat.py`为test-only离线兼容检查。

2026-09-21增量：`configs/lusgs/step2_profile.py`集中Step2 profile默认值；
`tools/check_step2_profile.py`逐项比较历史wrapper最终argv。Step2 guest的CLI、validation、
report、streaming分别位于`benchmarks/lusgs/step2_{cli,validation,report,streaming}.inc`，
均由原C文件在原符号位置include，属于同一翻译单元。controller资源观测实现位于
`src/arch/arm/cfd_coeff_preprocess_diagnostics.inc`，不构成独立控制器或SimObject。

更新基线：2026-09-10，清理后源码提交 `529f5a2ec5`，可恢复标签
`snapshot/cfd-dsa-cleanup-2026-09-10`。

本附录是 `CFD_DSA_PROJECT_STRUCTURE.md` 的逐文件分类表。状态按代码调用关系、runner、
suite 和 build map 判定，不按文件名或修改时间判定。`Evidence` 均为仓库相对路径和行号；
对纯数据/生成文件给出其生产者证据。行号仅作快速定位，符号和调用关系才是状态依据。

## 1. 模拟器与 CPU 文件

| Path | Type | Module | Role / Entry points | Used by / Dependencies | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `src/arch/arm/isa/formats/custom_cfd.isa` | ISA | ISA | `decodeCustomCFD()`；全部 custom CFD encoding | `insts/cfd_dsa.hh` | active | `:115-124,131-320`；旧 FMOPA guard 明确非法 `:138-146` |
| `src/arch/arm/isa/formats/formats.isa` | ISA | ISA | 注册 custom format | ARM ISA generator | active | `:95` |
| `src/arch/arm/isa/includes.isa` | ISA | ISA | include CFD instruction declarations | ARM ISA generator | active | `:98` |
| `src/arch/arm/insts/cfd_dsa.hh` | C++ | ISA | StaticInst 类声明、operands | custom decoder / cc | active | 旧 FMOPA/GetAcc/VsetZero 类已清理 |
| `src/arch/arm/insts/cfd_dsa.cc` | C++ | ISA/FU | execute、Path A/TRSV/MRHS/dual/coeff3/controllers/PathB/C | CfdLocalSpm/controllers/math helper | active | 旧不可达实现已清理 |
| `src/arch/arm/cfd_trsv5_math.hh` | C++ | FU helper | `cfdTrsv5Solve` 数值顺序 | TRSV/MRHS/coeff3 | active, sensitive | `:9-29` |
| `src/arch/arm/cfd_lusgs_vec5.hh` | C++ | FU helper | copy/sub/axpy API | LU-SGS controllers | active | `:11-21` |
| `src/arch/arm/cfd_lusgs_vec5.cc` | C++ | FU helper | Vector5 实现 | Step3/4 | active | definitions in file |
| `src/arch/arm/CfdLocalSpm.py` | Python SimObject | SPM | 声明 `CfdLocalSpm` | CPU attachment | active | `:4-7` |
| `src/arch/arm/cfd_local_spm.hh` | C++ | SPM/Stats | request types（含 B3 Rhs/Base 与 B4 Correction/DqStar Vector）、ZA/PathA/linebuf 状态、stats | all CFD execute/controllers | active | matrix kinds `:26-52` |
| `src/arch/arm/cfd_local_spm.cc` | C++ | SPM/Stats | local events、issuePacket、record/read/stats | all CFD paths | active | stats `:51-1115+`; `issuePacket :2290-2390` |
| `src/arch/arm/cfd_coeff_preprocess_controller.hh` | C++ | Controller | coeff descriptor/status/record/API；stream/NEED/B3+B4 flags、column/handoff/combine/publish record | coeff launch/wait/cancel/stream | active, ABI-sensitive | flags `:45-61`; descriptor `:64-82`; record `:86-316` |
| `src/arch/arm/cfd_coeff_preprocess_controller.cc` | C++ | Controller | request/input/LU/fixed-mask TRSM/drain/auto-retire；DInv/Lbar ColumnFma5、dq_star handoff、line parent/child、多 line、Ubar backward direct、frontier-aware RHS | Step2 raw event/stream | active | `launchDescriptor/launchLine/pumpLineRequests`；`processColumnFma`；`processLineBackward` |
| `src/arch/arm/cfd_lusgs_controller.hh` | C++ | Controller | prepared descriptor/record/status | Step3/Step4 ABI | experimental | `:16-295` |
| `src/arch/arm/cfd_lusgs_controller.cc` | C++ | Controller | coarse functional macro model | Step3 and Step4 fallback | experimental/regression | `:168-267,269-440` |
| `src/arch/arm/cfd_lusgs_event_controller.cc` | C++ | Controller | Step4 event lifecycle、PathA/TRSV/Vec graphs | Step4 benchmark | experimental/regression | state `:40-176`; shape `:689-728`; tick `:2985-3688` |
| `src/arch/arm/SConscript` | SCons | Build | 编译 CFD C++ sources | ARM build | active | `:91-96` |
| `src/cpu/FuncUnit.py` | Python | OpClass | OpClass enum与 opLat 语义 | generated enums/FU | active, sensitive | `:133-162` |
| `src/cpu/op_class.hh` | C++ | OpClass | CFD OpClass constants | StaticInst/FU | active, sensitive | `:151-165` |
| `src/cpu/o3/FuncUnitConfig.py` | Python | FU | CFD O3 FUDesc 默认 | FUPool/runner override | active | `:147-255,268-283` |
| `src/cpu/o3/FUPool.py` | Python | FU | 默认 O3 pool 装配 | O3 CPU | active | `:52-79` |
| `src/cpu/o3/commit.cc` | C++ | O3 | CFD SME ZA commit/squash 识别 | PathB/C stress | active, sensitive | `:79-81` |
| `src/cpu/minor/BaseMinorCPU.py` | Python | FU | Minor CFD 子集 | `--cpu=minor` | experimental/incomplete | `:220-283`; 缺多项 OpClass |
| `src/arch/arm/linux/se_workload.cc` | C++ | Workload | SME/ZA SE 初始化相关改动 | guest execution | active, sensitive | CFD/SME references in file；用途需结合 ARM SE |

## 2. Guest benchmark 文件

| Path | Type | Module | Role / Entry points | Used by | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `projects/cfd_dsa/benchmarks/README.md` | Markdown | Benchmark | benchmark 索引 | developers | docs | build map 以 tool 为准 |
| `projects/cfd_dsa/benchmarks/common/test_cfd_dsa_decode_exclusive.c` | C | Test | custom encoding 排他/执行 smoke | core regression | test-only | `projects/cfd_dsa/tools/build_benchmarks.py:17-19`; suite `projects/cfd_dsa/suites/core-regression.json:24-27` |
| `projects/cfd_dsa/benchmarks/patha/test_cfd_patha_extreme_perf.c` | C | Benchmark | Path A 当前 direct/streaming MVM | patha runner/core suite | active | build map `projects/cfd_dsa/tools/build_benchmarks.py:20-21` |
| `projects/cfd_dsa/benchmarks/patha/test_cfd_extreme_perf.c` | C | Benchmark | 旧 Path A benchmark | `patha-legacy` target | legacy | build map `projects/cfd_dsa/tools/build_benchmarks.py:22-23` |
| `projects/cfd_dsa/benchmarks/pathb/test_cfd_pathb_za_pipe_perf.c` | C | Benchmark | ZA spatial pipe standalone | pathb runner/core suite | experimental | build map `projects/cfd_dsa/tools/build_benchmarks.py:24-25`; suite `projects/cfd_dsa/suites/core-regression.json:9-13` |
| `projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_za_outer_perf.c` | C | Benchmark | selected-column outer Path C | pathc runner/core suite | experimental | build map `projects/cfd_dsa/tools/build_benchmarks.py:26-27`; suite `projects/cfd_dsa/suites/core-regression.json:14-18` |
| `projects/cfd_dsa/benchmarks/pathc/test_cfd_sme_za_squash.c` | C | Test | ZA wrong-path/squash stress | `sme-squash` | test-only | build map `projects/cfd_dsa/tools/build_benchmarks.py:32-33` |
| `projects/cfd_dsa/benchmarks/trsv5/test_cfd_trsv5.c` | C | Benchmark/Test | standalone TRSV5 numeric/FU | trsv5 runner/core suite | active regression | build map `projects/cfd_dsa/tools/build_benchmarks.py:34`; suite `projects/cfd_dsa/suites/core-regression.json:19-22` |
| `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step1.c` | C | Benchmark | Path A + software TRSV | Step1 runner/core | legacy regression | build map `projects/cfd_dsa/tools/build_benchmarks.py:35-36` |
| `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c` | C | Benchmark | 主数据/模式/reference/raw/pretransform/event/stream；B3/B4 off-shadow-direct；C1 line descriptor；C2/B5 Ubar direct；validation/stats | Step2 runner/suites | active + legacy + experimental stream | `run_pretransform_raw_stream_line`；`run_pretransform_raw_stream_auto`；mode dispatcher |
| `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step3.c` | C | Benchmark | prepared descriptor launch/wait | Step3 controller/core | experimental regression | build map `projects/cfd_dsa/tools/build_benchmarks.py:39-40` |
| `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step4.c` | C | Benchmark | lifecycle/snapshot/wrong-path/Step4 stages | Step4 controller/core | experimental regression | build map `projects/cfd_dsa/tools/build_benchmarks.py:41-42`; suite `projects/cfd_dsa/suites/core-regression.json:44-48` |

## 3. 配置文件

| Path | Type | Module | Role / Entry points | Used by / Dependencies | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `projects/cfd_dsa/configs/_paths.py` | Python | Config | binary/result canonical paths | all wrappers | active | `:9-15`; import 时建 results dir |
| `projects/cfd_dsa/configs/run_cfd_dsa.py` | Python | Config | 唯一完整 gem5 config/CLI/FU/SPM/workload/report | canonical profile wrappers | active, overloaded | env/FU/argv 以当前代码为准 |
| `projects/cfd_dsa/configs/run_cfd_patha.py` | Python | Config | Path A profile defaults -> base runner | patha target | active | wrapper env/default/runpy in file |
| `projects/cfd_dsa/configs/run_cfd_pathb.py` | Python | Config | local-event Path B profile | pathb target | experimental | defaults `:29-47` |
| `projects/cfd_dsa/configs/run_cfd_pathc.py` | Python | Config | local-event outer Path C profile | pathc target | experimental | defaults `:30-49` |
| `projects/cfd_dsa/configs/run_cfd_trsv5.py` | Python | Config | TRSV standalone profile | trsv5 target | active regression | defaults `:58-71` |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step1.py` | Python | Config | Step1 profile | Step1 target | legacy regression | binary/default/runpy in file |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step2.py` | Python | Config | Step2 defaults；stream event/event/window8/auto/mask；line模式 frontier-aware；B3/B4/B5 显式开关 | Step2 main benchmark | active | stream/consumer defaults block |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step3.py` | Python | Config | Step3 Stage A coarse profile | Step3 target | experimental | `:36-86` |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step4.py` | Python | Config | Step4 event profile，默认 A；B/C explicit | Step4 target | experimental | `:36-113` |
| `projects/cfd_dsa/configs/README.md` | Markdown | Config | config 入口说明 | developers | docs | 应以 base/wrapper code 为准 |
| 根 `run_cfd_*.py` compatibility shims | Python | Config | 已删除；统一使用 canonical configs | old commands | removed 2026-09-09 | 见 cleanup report |

## 4. Suite 与工具

| Path | Type | Module | Role / Entry points | Used by | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `projects/cfd_dsa/suites/core-regression.json` | JSON | Test | PathA/B/C、TRSV、decode、Step1-4C | `run_suite.py` | active main regression | `:1-50` |
| `projects/cfd_dsa/suites/coeff-event-sensitivity.json` | JSON | Test/Perf | RHS/div/mul/pending/drain/input one-factor | performance campaigns | active performance | `:1-47` |
| `projects/cfd_dsa/suites/coeff-streaming-correctness.json` | JSON | Test | stream 边界规模、window 1/2/4/8、packet backpressure | stream changes | active experimental regression | `:1-64` |
| `projects/cfd_dsa/suites/coeff-streaming-boundary-mask.json` | JSON | Test | N=1/head/interior/tail dynamic RHS 与 drain skip | mask/worklist changes | active experimental regression | all cases in file |
| `projects/cfd_dsa/suites/coeff-streaming-reaper.json` | JSON | Test | detached lifecycle、乱序 reap、slow drain、packet/output backpressure | wait/reap/record changes | active experimental regression | all cases in file |
| `projects/cfd_dsa/suites/coeff-streaming-auto-retire.json` | JSON | Test | stable record/auto-retire 的边界、window、slow drain、ring/outstanding/bank/cancel | auto-retire/mask changes | active experimental regression | 12 cases in file |
| `projects/cfd_dsa/suites/coeff-streaming-dinv-consumer.json` | JSON | Test | B3 shadow/direct、window、ColumnFma sensitivity、R/base pressure、partial cancel/generation、乱序列 | B3 changes | active experimental regression | 23 cases in file |
| `projects/cfd_dsa/suites/coeff-streaming-lbar-consumer.json` | JSON | Test | B4 shadow/direct、window、共享ColumnFma/combine、dq output、packet、四阶段cancel、乱序及handoff fault injection | B4 changes | active experimental regression | 33 cases in file |
| `projects/cfd_dsa/suites/coeff-streaming-line-autonomous.json` | JSON | Test | C1 parent/child、C2/B5 Ubar direct、1/2/4/8-line、frontier-aware、packet双缓冲 | C1/C2 changes | active experimental regression | 6 cases |
| `projects/cfd_dsa/suites/coeff-streaming-boundaries.json` | JSON | Test | timing DMA、version/dirty reuse、reciprocal 边界 | Stage6b/7/8 | active boundary regression | DMA 单 sweep与复用/数值边界 |
| `projects/cfd_dsa/suites/coeff-line-dependency.json` | JSON | Test/Perf | base-ahead、early-backward、cache hit/rebuild、多 line/慢 drain | line dependency changes | active regression | 12 cases；含 DMA 单 sweep |
| `projects/cfd_dsa/suites/coeff-line-dma-rebuild-diagnostic.json` | JSON | Diagnostic | DMA 3-sweep dirty rebuild | known issue reproduction | expected-fail diagnostic | cache Modified-line/WriteReq 一致性断言；修复前保留 |
| `projects/cfd_dsa/suites/coeff-line-mvm-pipeline.json` | JSON | Test/Perf | serial/split product-add pipeline、depth、packet/DMA | line MVM experiments | experimental regression | 12 cases；split 非默认 |
| `projects/cfd_dsa/suites/coeff-context-ablation.json` | JSON | Test/Perf | LU/TRSM active-context 1/2/4 固定资源消融 | context/resource analysis | active diagnostic | cold/mixed 3×3 + observer-off |
| `projects/cfd_dsa/suites/step4c-sensitivity.json` | JSON | Test/Perf | Step4-C engine sensitivity | Step4 research | experimental performance | cases in file |
| `projects/cfd_dsa/suites/README.md` | Markdown | Test | suite 使用说明 | developers | docs | runner code 为准 |
| `projects/cfd_dsa/tools/build_benchmarks.py` | Python | Build | target->source/binary map，cross compile | developer/CI | active | `:13-76` |
| `projects/cfd_dsa/tools/run_experiment.py` | Python | Test | immutable result path、manifest、metrics | suite runner | active | targets `:19-42`; lifecycle `:113-178` |
| `projects/cfd_dsa/tools/run_suite.py` | Python | Test | JSON case dispatcher | suites | active | `:17-38` |
| `projects/cfd_dsa/tools/organize_results.py` | Python | Results | migration/organization | one-time/maintenance | experimental/admin | 仅做可追溯迁移；不按目录名直接删除 |
| `projects/cfd_dsa/tools/run_patha_dotp_sensitivity.py` | Python | Perf | Path A dotp sweep | historical/perf | experimental | file entry/main |
| `projects/cfd_dsa/tools/check_line_dependency.py` | Python | Validation | line schedule、cache/rebuild、retirement invariant | line dependency campaigns | active checker | 校验 full PASS、hash、时序与操作数 |
| `projects/cfd_dsa/tools/check_line_mvm_pipeline.py` | Python | Validation | split pipeline count/depth/numeric checks | line MVM campaign | active checker | 校验 product/add 与 buffered depth |
| `projects/cfd_dsa/tools/check_context_ablation.py` | Python | Validation | context matrix 与资源采样守恒 | context campaigns | active checker | 校验 issued/eligible/occupancy invariant |
| `projects/cfd_dsa/tools/check_scheduler_diagnostics.py` | Python | Validation | issue reject 与 scheduler reason 守恒 | diagnostics campaign | active checker | 复用 context reader；不将事件数当周期 |
| `projects/cfd_dsa/README.md` | Markdown | Docs | canonical project ownership/entry | developers | active index | canonical boundaries `:3-17,35-49` |

## 5. 文档文件

| Path | Type | Module | Role | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- |
| `projects/cfd_dsa/docs/CFD_DSA_PROJECT_STRUCTURE.md` | Markdown | Docs | 当前项目总地图 | active canonical | 2026-09-10 已对齐清理后主线 |
| `projects/cfd_dsa/docs/CFD_DSA_PROJECT_FILE_INDEX.md` | Markdown | Docs | 逐文件分类 | active appendix | 2026-09-10 已更新 |
| `projects/cfd_dsa/docs/README.md` | Markdown | Docs | 专项文档索引/最终 campaign 列表 | active index | `:3-22` |
| `projects/cfd_dsa/docs/RESULT_LIFECYCLE.md` | Markdown | Docs | source/result ownership、immutable campaign | absent in current working tree | 本轮不恢复该文件；结果保留规则以 cleanup report 与现有 manifest 为准 |
| `projects/cfd_dsa/docs/CFD_DSA_IMPLEMENTATION.md` | Markdown | Docs | Path A 累积实现/历史结果 | active interfaces + historical perf | `projects/cfd_dsa/docs/README.md:3`; 性能需跟结果路径 |
| `projects/cfd_dsa/docs/CFD_DSA_TRSV5.md` | Markdown | Docs | standalone TRSV、raw event关系 | active specialty | model/default `:16-17,123,164-197` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_PRETRANSFORM.md` | Markdown | Docs | prepared/raw、dual/coeff3/event细节 | active specialty | `projects/cfd_dsa/docs/README.md:8` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP1.md` | Markdown | Docs | Step1 设计/结果 | legacy specialty | staged history |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP2.md` | Markdown | Docs | Step2 当前模式族与执行路径 | active canonical specialty | 与 CORE/PRETRANSFORM/STREAMING 配合读 |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP2_STREAMING.md` | Markdown | Docs | Stage A/B1.5/B2.5/B3/B4/C1/C2/B5 数据流、ABI、生命周期、stats/perf | active specialty | 当前 streaming 主线的详细证据 |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP2_CORE.md` | Markdown | Docs | fused/forward/context/linebuf | active regression specialty | `projects/cfd_dsa/docs/README.md:7` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP3.md` | Markdown | Docs | coarse macro controller | frozen regression specialty | controller code为准 |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP4.md` | Markdown | Docs | Step4 A/B/C event controller | frozen regression specialty | version/boundary `:3-7` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_FULL_TECHNICAL_REPORT.md` | Markdown | Docs | LU-SGS 综合报告；历史 Step1–4 + 当前 Step2 streaming | active consolidated | 2026-09-10 新增当前实现补遗 |
| `projects/cfd_dsa/docs/CFD_DSA_VERSION_RETENTION_ANALYSIS.md` | Markdown | Docs | 各阶段版本保留/删除决策 | active maintenance | 清理依据；区分 active/regression/frozen/generated |
| `projects/cfd_dsa/docs/CFD_DSA_CLEANUP_REPORT_2026-09-09.md` | Markdown | Docs | 清理删除清单、保留理由、验证与恢复 | active maintenance | cleanup commit/tag 与 10/10 campaign 回归证据 |
| `projects/cfd_dsa/docs/CFD_DSA_CONTEXT_ABLATION.md` | Markdown | Docs | LU/TRSM context 固定资源消融与统计口径 | active analysis | 2/2 基线、2/4 性能参照；不可外推 DMA/多 line |
| `projects/cfd_dsa/docs/CFD_DSA_SCHEDULER_DIAGNOSTICS.md` | Markdown | Docs | FU 拒绝与调度背压事件计数 | active diagnostic | 不把累计拒绝次数换算为独占周期 |
| `projects/cfd_dsa/docs/CFD_DSA_VERSIONS_AND_RETENTION.md` | Markdown | Docs | 面向维护者的版本/标签摘要 | active maintenance | 与详细 retention analysis 配合使用 |
| `projects/cfd_dsa/docs/GIT_SCHEME_TAGS.md` | Markdown | Docs | Git scheme/snapshot 标签映射 | active maintenance | 标签到方案、入口和验证状态的索引 |
| `projects/cfd_dsa/docs/CFD_DSA_PATHB_ZA_PIPE.md` | Markdown | Docs | Path B | experimental specialty | current profile `:196-231` |
| `projects/cfd_dsa/docs/CFD_DSA_SME.md` | Markdown | Docs | Path C 与已删除 fixed-ZA/FMOPA 历史边界 | experimental specialty | 当前只保留 selected-column outer；删除项见 cleanup report |
| `projects/cfd_dsa/docs/performance_analysis.md` | Markdown | Docs | Path A 多轮性能分析 | historical artifact | `projects/cfd_dsa/docs/README.md:11` |
| `projects/cfd_dsa/docs/test_report.md` | Markdown | Docs | 历史测试报告 | historical artifact | 不作当前默认依据 |

## 6. Results 与生成文件分类

| Path | Type | Module | Role / Producer | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- |
| `build/cfd_dsa/aarch64/*` | ELF | Benchmark | `build_benchmarks.py` 输出 | generated | output root `tools/build_benchmarks.py:13-16` |
| `results/cfd_dsa/runs/*` | sim output | Results | development timestamp runs | generated/disposable | `run_experiment.py:113-118` |
| `results/cfd_dsa/campaigns/*` | sim output | Results | immutable named campaigns | generated evidence | `run_experiment.py:118-121` |
| `results/cfd_dsa/baselines/lusgs/step4a,step4b` | sim output | Results | migrated physical baselines | generated evidence | 保留历史 Step4 对照；由 stats/simout 自证 |
| `results/cfd_dsa/baselines/lusgs/step4c/*` | symlink | Results | aliases to 2026-07-01 campaign | generated evidence | 指向 `campaigns/2026-07-01-step4c/correctness/{1x64,1x50000}` |
| `results/cfd_dsa/failed/*` | sim output | Results | failed/empty stats quarantine | generated diagnostic evidence | 需逐项审查，不按目录名删除；其他 expected-fail 也可能保存在 campaign 中 |
| `results/cfd_dsa/archive/*` | sim output | Results | superseded intact campaigns | generated/historical | 逐项保留；先补引用/hash 映射再考虑迁档 |
| `results/lusgs_raw/*`, `results/lusgs_optprep/*` | sim output | Results | pre-organization result trees | generated/legacy evidence | 尚未在 `migration-inventory.json` 完成一一映射，且 PRETRANSFORM 文档仍引用；继续保留 |
| `results/cfd_dsa/campaigns/core-regression-coeff-final/*` | sim output | Test | 九项核心回归 | current checked evidence | manifests `status=passed` |
| `results/cfd_dsa/campaigns/coeff-event-packet-formal-s16/*` | sim output | Test/Perf | RHS15 div1/div2 raw formal | current checked evidence | manifests passed；simout raw mismatches 0 |
| `results/cfd_dsa/campaigns/coeff-streaming-line-autonomous-final-20260728/*` | sim output | Test/Perf | C1/C2 1/2/4/8-line 与 packet 双缓冲 | current checked evidence | 6/6 manifests passed |
| `results/cfd_dsa/campaigns/stage5-frontier-schedule-20260728/*` | sim output | Test/Perf | B5 frontier-aware wavefront | current checked evidence | 4-line 29232 cycles，mismatch 0 |
| `results/cfd_dsa/campaigns/cleanup-validation-2026-09-10/*` | sim output | Test | 清理后全核心回归 | current checked evidence | 10/10 passed；Path A/B/C、TRSV5、decode、Step1/2/3/4-C |
| `results/cfd_dsa/runs/step2-{event-barrier,stream-w4}-1x17-final5/*` | sim output | Test/Perf | 同配置 barrier vs Stage A | current development evidence | 21204 vs 17300 cycles；stream mismatch 0；overlap 10738 |

## 7. 高风险依赖摘要

| 修改点 | 直接影响 | 最小验证 |
| --- | --- | --- |
| `custom_cfd.isa` / operand ABI | 所有 guest inline encoders、decode exclusivity | decode-exclusive + PathA/TRSV/coeff ctrl smoke |
| OpClass/FU files | O3 issue latency/count、Minor legality | build + core regression |
| `cfd_trsv5_math.hh` | TRSV/MRHS/dual/coeff3 numerical semantics | standalone TRSV + raw compare |
| coeff descriptor/record/progress status | guest/C++ ABI、generation/token、stream ready | build benchmark + event correctness/cancel + streaming suite |
| `CfdLocalSpm` request/ZA state | PathA/B/C、coeff packet、Step stats | core + bank/backpressure + ZA squash |
| `CfdLocalSpm.dma_port` / coeff version-dirty | timing 请求、数据复用、跨 sweep 可见性 | DMA request/response + dirty rebuild + software reference |
| C1/C2/B5 line parent/frontier | 跨 cell 依赖、parent/child 生命周期、反向 wavefront | line-autonomous + multi-line frontier + cancel/fault injection |
| Step2 benchmark mode dispatcher | legacy/raw全部路径 | `all` 小规模 + raw-compare + context |
| base runner/defaults | 所有 profiles | py_compile + print final config + relevant suite |
