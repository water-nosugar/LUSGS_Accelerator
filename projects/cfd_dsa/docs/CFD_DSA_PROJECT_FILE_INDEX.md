# CFD DSA / LU-SGS 文件索引

本附录是 `CFD_DSA_PROJECT_STRUCTURE.md` 的逐文件分类表。状态按代码调用关系、runner、
suite 和 build map 判定，不按文件名或修改时间判定。`Evidence` 均为仓库相对路径和行号；
对纯数据/生成文件给出其生产者证据。

## 1. 模拟器与 CPU 文件

| Path | Type | Module | Role / Entry points | Used by / Dependencies | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `src/arch/arm/isa/formats/custom_cfd.isa` | ISA | ISA | `decodeCustomCFD()`；全部 custom CFD encoding | `insts/cfd_dsa.hh` | active | `:115-124,131-320`；旧 FMOPA guard 明确非法 `:138-146` |
| `src/arch/arm/isa/insts/cfd_dsa.isa` | ISA | ISA | 兼容 stub，说明实现已转 C++ | ISA build | legacy | `:4-11` |
| `src/arch/arm/isa/formats/formats.isa` | ISA | ISA | 注册 custom format | ARM ISA generator | active | `:95` |
| `src/arch/arm/isa/includes.isa` | ISA | ISA | include CFD instruction declarations | ARM ISA generator | active | `:98` |
| `src/arch/arm/insts/cfd_dsa.hh` | C++ | ISA | StaticInst 类声明、operands | custom decoder / cc | active + legacy classes | classes `:53-621` |
| `src/arch/arm/insts/cfd_dsa.cc` | C++ | ISA/FU | execute、Path A/TRSV/MRHS/dual/coeff3/controllers/PathB/C | CfdLocalSpm/controllers/math helper | active + legacy | constructors/execute `:467-2477` |
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
| `projects/cfd_dsa/benchmarks/pathc/test_cfd_pathc_sme_step_perf.c` | C | Benchmark | include 新 Path C 的兼容源 | `pathc-compat` | legacy compatibility | build map `projects/cfd_dsa/tools/build_benchmarks.py:28-29` |
| `projects/cfd_dsa/benchmarks/pathc/test_cfd_sme_step_perf.c` | C | Benchmark | old fixed-ZA encoding source | `sme-step` compile target | test-only/legacy candidate | decoder拒绝 old guard `src/arch/arm/isa/formats/custom_cfd.isa:138-146` |
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
| `projects/cfd_dsa/configs/run_cfd_dsa.py` | Python | Config | 唯一完整 gem5 config/CLI/FU/SPM/workload/report | all wrappers/root shims | active, overloaded | env `:1381-1552`; FU `:3435-3598`; argv `:3661-3803` |
| `projects/cfd_dsa/configs/run_cfd_patha.py` | Python | Config | Path A profile defaults -> base runner | patha target | active | wrapper env/default/runpy in file |
| `projects/cfd_dsa/configs/run_cfd_pathb.py` | Python | Config | local-event Path B profile | pathb target | experimental | defaults `:29-47` |
| `projects/cfd_dsa/configs/run_cfd_pathc.py` | Python | Config | local-event outer Path C profile | pathc target | experimental | defaults `:30-49` |
| `projects/cfd_dsa/configs/run_cfd_trsv5.py` | Python | Config | TRSV standalone profile | trsv5 target | active regression | defaults `:58-71` |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step1.py` | Python | Config | Step1 profile | Step1 target | legacy regression | binary/default/runpy in file |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step2.py` | Python | Config | Step2 defaults；stream event/event/window8/auto/mask；line模式 frontier-aware；B3/B4/B5 显式开关 | Step2 main benchmark | active | stream/consumer defaults block |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step3.py` | Python | Config | Step3 Stage A coarse profile | Step3 target | experimental | `:36-86` |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step4.py` | Python | Config | Step4 event profile，默认 A；B/C explicit | Step4 target | experimental | `:36-113` |
| `projects/cfd_dsa/configs/README.md` | Markdown | Config | config 入口说明 | developers | docs | 应以 base/wrapper code 为准 |
| `projects/cfd_dsa/configs/__pycache__/run_cfd_dsa.cpython-310.pyc` | binary | Config | Python bytecode | generated by py_compile/import | generated | 不作为源码 |
| `projects/cfd_dsa/configs/lusgs/__pycache__/run_cfd_lusgs_step2.cpython-310.pyc` | binary | Config | Python bytecode | generated | generated | 不作为源码 |
| 根 `run_cfd_dsa.py` 与 `run_cfd_path*.py`、`run_cfd_trsv5.py`、`run_cfd_lusgs_step*.py` | Python | Config | 转发 canonical configs | old commands | legacy shims | `docs/RESULT_MANAGEMENT.md:15-16`；不要加入逻辑 |

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
| `projects/cfd_dsa/suites/step4c-sensitivity.json` | JSON | Test/Perf | Step4-C engine sensitivity | Step4 research | experimental performance | cases in file |
| `projects/cfd_dsa/suites/README.md` | Markdown | Test | suite 使用说明 | developers | docs | runner code 为准 |
| `projects/cfd_dsa/tools/build_benchmarks.py` | Python | Build | target->source/binary map，cross compile | developer/CI | active | `:13-76` |
| `projects/cfd_dsa/tools/run_experiment.py` | Python | Test | immutable result path、manifest、metrics | suite runner | active | targets `:19-42`; lifecycle `:113-178` |
| `projects/cfd_dsa/tools/run_suite.py` | Python | Test | JSON case dispatcher | suites | active | `:17-38` |
| `projects/cfd_dsa/tools/organize_results.py` | Python | Results | migration/organization | one-time/maintenance | experimental/admin | `projects/cfd_dsa/docs/RESULT_MANAGEMENT.md:53-68` |
| `projects/cfd_dsa/tools/run_patha_dotp_sensitivity.py` | Python | Perf | Path A dotp sweep | historical/perf | experimental | file entry/main |
| `projects/cfd_dsa/tools/__pycache__/run_suite.cpython-310.pyc` | binary | Test | bytecode | generated | generated | 不作为源码 |
| `projects/cfd_dsa/README.md` | Markdown | Docs | canonical project ownership/entry | developers | active index | canonical boundaries `:3-17,35-49` |

## 5. 文档文件

| Path | Type | Module | Role | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- |
| `projects/cfd_dsa/docs/CFD_DSA_PROJECT_STRUCTURE.md` | Markdown | Docs | 当前项目总地图 | active canonical | 本次新增 |
| `projects/cfd_dsa/docs/CFD_DSA_PROJECT_FILE_INDEX.md` | Markdown | Docs | 逐文件分类 | active appendix | 本次新增 |
| `projects/cfd_dsa/docs/README.md` | Markdown | Docs | 专项文档索引/最终 campaign 列表 | active index | `:3-22` |
| `projects/cfd_dsa/docs/RESULT_MANAGEMENT.md` | Markdown | Docs | source/result ownership、immutable campaign | active | `:3-30`; baseline-index 缺失待确认 |
| `projects/cfd_dsa/docs/CFD_DSA_IMPLEMENTATION.md` | Markdown | Docs | Path A 累积实现/历史结果 | active interfaces + historical perf | `projects/cfd_dsa/docs/README.md:3`; 性能需跟结果路径 |
| `projects/cfd_dsa/docs/CFD_DSA_TRSV5.md` | Markdown | Docs | standalone TRSV、raw event关系 | active specialty | model/default `:16-17,123,164-197` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_PRETRANSFORM.md` | Markdown | Docs | prepared/raw、dual/coeff3/event细节 | active specialty | `projects/cfd_dsa/docs/README.md:8` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP1.md` | Markdown | Docs | Step1 设计/结果 | legacy specialty | staged history |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP2.md` | Markdown | Docs | Step2 当前模式族与执行路径 | active canonical specialty | 与 CORE/PRETRANSFORM/STREAMING 配合读 |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP2_STREAMING.md` | Markdown | Docs | Stage A/B1.5/B2.5/B3 数据流、ABI、生命周期、stats/perf与B4/C边界 | active experimental specialty | B3 实现与 `stageb3-*` 实测证据 |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP2_CORE.md` | Markdown | Docs | fused/forward/context/linebuf | active regression specialty | `projects/cfd_dsa/docs/README.md:7` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP3.md` | Markdown | Docs | coarse macro controller | experimental specialty | controller code为准 |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_STEP4.md` | Markdown | Docs | Step4 A/B/C event controller | active specialty | version/boundary `:3-7` |
| `projects/cfd_dsa/docs/CFD_DSA_LUSGS_FULL_TECHNICAL_REPORT.md` | Markdown | Docs | LU-SGS 综合历史报告 | consolidated, may lag | `projects/cfd_dsa/docs/README.md:10`; 本地图优先 |
| `projects/cfd_dsa/docs/CFD_DSA_PATHB_ZA_PIPE.md` | Markdown | Docs | Path B | experimental specialty | current profile `:196-231` |
| `projects/cfd_dsa/docs/CFD_DSA_SME.md` | Markdown | Docs | Path C/old FMOPA边界 | experimental specialty | old guard `:26,78,395-396` |
| `projects/cfd_dsa/docs/performance_analysis.md` | Markdown | Docs | Path A 多轮性能分析 | historical artifact | `projects/cfd_dsa/docs/README.md:11` |
| `projects/cfd_dsa/docs/test_report.md` | Markdown | Docs | 历史测试报告 | historical artifact | 不作当前默认依据 |

## 6. Results 与生成文件分类

| Path | Type | Module | Role / Producer | Status | Evidence / Notes |
| --- | --- | --- | --- | --- | --- |
| `build/cfd_dsa/aarch64/*` | ELF | Benchmark | `build_benchmarks.py` 输出 | generated | output root `tools/build_benchmarks.py:13-16` |
| `results/cfd_dsa/runs/*` | sim output | Results | development timestamp runs | generated/disposable | `run_experiment.py:113-118` |
| `results/cfd_dsa/campaigns/*` | sim output | Results | immutable named campaigns | generated evidence | `run_experiment.py:118-121` |
| `results/cfd_dsa/baselines/lusgs/step4a,step4b` | sim output | Results | migrated physical baselines | generated evidence | `RESULT_MANAGEMENT.md:53-68` |
| `results/cfd_dsa/baselines/lusgs/step4c/*` | symlink | Results | aliases to 2026-07-01 campaign | generated evidence | `RESULT_MANAGEMENT.md:67-68` |
| `results/cfd_dsa/failed/*` | sim output | Results | failed/empty stats quarantine | generated/non-evidence | policy `RESULT_MANAGEMENT.md:25-27` |
| `results/cfd_dsa/archive/*` | sim output | Results | superseded intact campaigns | generated/historical | policy `:27` |
| `results/lusgs_raw/*`, `results/lusgs_optprep/*` | sim output | Results | pre-organization result trees | generated/legacy | 不作当前默认；迁移关系见 `migration-inventory.json` |
| `results/cfd_dsa/campaigns/core-regression-coeff-final/*` | sim output | Test | 九项核心回归 | current checked evidence | manifests `status=passed` |
| `results/cfd_dsa/campaigns/coeff-event-packet-formal-s16/*` | sim output | Test/Perf | RHS15 div1/div2 raw formal | current checked evidence | manifests passed；simout raw mismatches 0 |
| `results/cfd_dsa/runs/step2-{event-barrier,stream-w4}-1x17-final5/*` | sim output | Test/Perf | 同配置 barrier vs Stage A | current development evidence | 21204 vs 17300 cycles；stream mismatch 0；overlap 10738 |

## 7. 高风险依赖摘要

| 修改点 | 直接影响 | 最小验证 |
| --- | --- | --- |
| `custom_cfd.isa` / operand ABI | 所有 guest inline encoders、decode exclusivity | decode-exclusive + PathA/TRSV/coeff ctrl smoke |
| OpClass/FU files | O3 issue latency/count、Minor legality | build + core regression |
| `cfd_trsv5_math.hh` | TRSV/MRHS/dual/coeff3 numerical semantics | standalone TRSV + raw compare |
| coeff descriptor/record/progress status | guest/C++ ABI、generation/token、stream ready | build benchmark + event correctness/cancel + streaming suite |
| `CfdLocalSpm` request/ZA state | PathA/B/C、coeff packet、Step stats | core + bank/backpressure + ZA squash |
| Step2 benchmark mode dispatcher | legacy/raw全部路径 | `all` 小规模 + raw-compare + context |
| base runner/defaults | 所有 profiles | py_compile + print final config + relevant suite |
