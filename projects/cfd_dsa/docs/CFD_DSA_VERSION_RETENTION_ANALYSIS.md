# CFD-DSA 各阶段方案、版本与保留/清理建议

> 审计日期：2026-09-09
>
> 审计对象：`/home/zyy/gem5` 当前工作区中的实现、runner、benchmark、suite、文档与结果目录
>
> 结论性质：代码资产盘点与收敛建议；本文不执行删除
> 重要限制：CFD-DSA 文件和大量结果目前属于未提交工作区内容，Git 分支/标签不能代表它们的版本历史。因此本文所说“版本”主要指代码中可运行的实现变体，而不是 Git release。

## 1. 执行摘要

当前工程不是四个互斥的 Step1/2/3/4 版本，而是三条并存的研究线：

1. **算子/数据通路线**：Path A、Path B、Path C、TRSV5、Vector5；
2. **LU-SGS 控制线**：Step1、Step2 模式族、Step3 coarse macro-controller、Step4 event controller；
3. **系数预处理与 wavefront 线**：prepared、raw、pretransform、coarse/event、Streaming A/B/C、DMA、系数复用和资源上下文实验。

建议把产品/论文主线收敛为：

```text
raw D/L/U/R
  -> event LU5 + 15-RHS TRSM
  -> auto-retire + boundary mask
  -> DInv/Lbar/Ubar direct column consumers
  -> line-autonomous multi-line wavefront
  -> serial FP64 divide 基线
  -> 2 LU contexts / 2 TRSM contexts 实验基线
```

对应现有命名是 **Step2 pretransform raw optprep streaming + B1.5 + B2.5 + B3/B4 direct + C1/C2/B5**。这是当前功能覆盖最完整、仍在继续做性能实验、并拥有最新回归证据的路线。

建议保留三层版本，而不是保留所有排列组合：

- **主实现**：上述 streaming wavefront；
- **正确性基线**：软件 reference、standalone Path A、standalone TRSV5、prepared Step2 最小基线；
- **诊断版本**：off/shadow、queue、internal SPM、1/1 context，以及少量 Step3/4 生命周期回归。

建议删除或迁出主目录的主要对象是：生成的 `__pycache__`、失效/空结果、根目录兼容脚本、重复 benchmark wrapper、不可解码的旧 Path C benchmark，以及大量带 `final2/final3/final4/debug/r2/r3` 的重复 campaign。删除应在建立 Git 提交和结果索引后分批进行。

## 2. 当前版本基线

### 2.1 gem5 基线

- 当前分支：`stable`；远端默认同为 `origin/stable`。
- 当前实际 HEAD：`7a2b0e413d`，`git describe` 为 `v25.1.0.0-0-g7a2b0e413d-dirty`；不是仓库中可见的更新标签 `v25.1.0.1`。
- 仓库可见最新 gem5 标签包含 `v25.1.0.1`、`v25.1.0.0`、`v25.1` 等。
- CFD-DSA 相关源码、项目目录和结果大多尚未进入当前 Git 提交历史；因此不能把 `v25.1.0.1` 当成 CFD-DSA 的发布版本。
- 建议首次收敛后单独建立 `cfd-dsa-v0.1` 风格的里程碑标签，并在 manifest 中同时记录 gem5 commit 与 CFD-DSA 版本。

### 2.2 当前资产规模

| 目录 | 大小（审计时） | 判断 |
| --- | ---: | --- |
| `projects/cfd_dsa/` | 约 1.9 MiB | 源码、runner、suite、文档的规范目录，应保留 |
| `fpga/` | 约 80 KiB | 只有 RTL 设计指南，没有已实现/综合 RTL，应明确标为设计资产 |
| `results/cfd_dsa/` | 约 1.3 GiB | 主要清理对象；正式证据与临时结果混杂 |
| `results/lusgs_raw/` | 约 35 MiB | 旧式结果目录，建议迁档后删除 |
| `results/lusgs_optprep/` | 约 21 MiB | 旧式结果目录，建议迁档后删除 |

## 3. 各阶段方案全景

### 3.1 基础 MVM：Path A / B / C

| 方案 | 核心实现 | 当前定位 | 建议 |
| --- | --- | --- | --- |
| Path A baseline | `lmat5_spm + dotp_row + pack_acc` | LU-SGS 主路径的原始 MVM 基线；ISA/O3/FU/benchmark 完整 | **保留**，作为稳定算子与回归基线 |
| Path A streaming | `patha_stream_ld5 + patha_dotp_stream + patha_store5_result` | Path A-only 的流式实验；默认并未成为 LU-SGS wavefront 的数据面 | **保留为实验**，不要与 coefficient streaming 主线混称 |
| Path A xregs | X16–X20 兼容 accumulator/token | internal-buffer 之前的兼容路径 | **只保留一个回归入口**，不再做性能主点 |
| Path A internal-buffer | 内部 result buffer，默认双 buffer | 当前推荐 Path A 表达 | **保留** |
| Path B | ZA selected partial-sum spatial pipe | standalone 正确性/局部事件实验，未接入 LU-SGS dispatcher | **保留为备选研究分支**；主文档降级 |
| Path C | ZA selected-column outer product | standalone SME/ZA 研究，未接入 LU-SGS 主线 | **保留为备选研究分支**；主文档降级 |
| old Path C FMOPA | 固定 `ZA[:,0]` 的旧编码/类 | decoder 已拒绝，只有遗留 C++ 类和 benchmark | **删除候选** |

判断：Path B/C 的编号不是 Path A 的“新版本”，三者是不同 MVM 映射。当前 LU-SGS 实际主线仍依赖 Path A 语义和 controller 内的 column FMA，因此不能因为 B/C 编号更靠后就删除 Path A。

### 3.2 TRSV5 / Vector5 算子

| 版本 | 说明 | 建议 |
| --- | --- | --- |
| standalone `trsv5_lu_spm` | 从 SPM RHS 求解，默认 coarse 60 cycles | **保留**，是独立数值和 decode 回归 |
| `trsv5_lu_spm_zrhs` | 直接消费 Z RHS，服务 Step2 forwarded | **保留**，减少 RHS staging 的关键版本 |
| Step4 staged TRSV5 engine | divide/multiply-subtract event graph | **保留最小回归**，用于资源图与生命周期研究 |
| Vector5 coarse FU | COPY/SUB/AXPY，Step2 常用 | **保留** |
| Step4 staged Vector5 engine | queue/lane event engine | **保留最小回归**，不作为当前产品主线 |

### 3.3 LU-SGS Step1–4

| 阶段 | 实际含义 | 成熟度 | 建议定位 |
| --- | --- | --- | --- |
| Step1 | Path A MVM + 软件 TRSV/向量运算/CPU 循环 | 稳定历史基线 | **保留一个小规模回归**；停止扩展 |
| Step2 baseline | prepared `LU(D)/L/Ubar` + Path A + standalone TRSV5 | 稳定兼容基线 | **保留**，但只留最小模式和证据 |
| Step2 core | fused MVM-sub | 已验证优化 | **保留代码**；性能主文档并入 Step2 |
| Step2 forwarded | Z RHS 直传 TRSV | 已验证优化 | **保留代码** |
| Step2 context | 软件 line context | 对照版本 | **保留一个消融点** |
| Step2 linebuf | hardware line-buffer forwarding | 已验证、收益有限 | **保留**，作为 heap/SPM staging 对照 |
| Step3 | descriptor + functional coarse macro-controller | 可运行但为解析/宏模型 | **冻结为历史回归**；不再作为主线开发 |
| Step4-A | 异步 token/poll/reap 生命周期 | 已验证生命周期 | **保留生命周期回归** |
| Step4-B | controller 内 Path A resource graph | 已验证事件图 | **保留一组结构回归** |
| Step4-C | staged TRSV5/Vector5 resource graph | 已验证事件图 | **保留一组结构/压力回归** |

关键判断：Step3/4 并没有替代现在的 Step2 streaming。Step3/4 使用另一套 `cfd_lusgs_controller` / `cfd_lusgs_event_controller`，而最新工作集中在 `cfd_coeff_preprocess_controller` 的 line-autonomous wavefront。建议在命名上把 Step3/4 改称“legacy macro-controller experiments”，避免读者误以为 Step4 是当前最高产品版本。

## 4. Step2 模式族与版本建议

### 4.1 输入与预处理版本

| 版本 | 输入/行为 | 价值 | 建议 |
| --- | --- | --- | --- |
| software reference | CPU 完成标准 LU-SGS | correctness oracle | **永久保留** |
| legacy prepared | 直接生成 `LU(D)/L/Ubar` | 早期算法与 ISA 基线，但不含完整预处理成本 | **保留最小回归，不用于公平性能结论** |
| raw TRSV5 | `D/L/U/R`，软件 LU + MRHS Ubar + 热循环 TRSV | 完整成本基线 | **保留** |
| prepared pretransform | 预先提供 Dinv/Lbar/Ubar | 验证热循环结构 | **保留为诊断，不作为端到端主结论** |
| raw dual coarse | 双批求 Dinv/Lbar，另求 Ubar | coarse 对照 | **保留一个对照点** |
| raw coeff3 coarse | 一次得到 Dinv/Lbar/Ubar | event 前的粗粒度基线 | **保留一个对照点** |
| raw coeff3 event | LU/TRSM/partial ready/drain 的细粒度事件图 | 当前预处理主实现 | **保留并作为主线** |
| raw optprep streaming | 预处理与 sweep wavefront 重叠 | 当前整体主实现 | **保留并作为默认性能路径** |

建议把 `--lusgs-mode=all` 只用于综合回归，禁止用作性能测量默认值。当前 wrapper 默认 `all` 容易使新用户误读，应改为显式 smoke profile 或要求用户选择模式。

### 4.2 Streaming Stage A/B/C

| 子阶段 | 当前实现 | 版本关系 | 建议 |
| --- | --- | --- | --- |
| Stage A | per-cell state、window 1/2/4/8、预处理/forward overlap | streaming 基础 | **保留为冻结对照** |
| B1 | completion queue + round-robin reaper | 被 B1.5 取代但有压力测试价值 | **仅保留 queue 压力回归** |
| B1.5 | stable record + controller auto-retire，无 guest WAIT/reaper | 当前默认 | **保留并设为主线** |
| B2 | dynamic NEED mask/worklist 兼容语义 | 已被 B2.5 固定表替代 | **兼容代码短期保留，停止新增测试** |
| B2.5 | 固定 bitmask/worklist，边界跳过无效 RHS | 当前默认 | **保留并设为主线** |
| B3 off | DInv 仍 drain，guest Path A 计算 base | B3 控制组 | **保留一个性能/回归点** |
| B3 shadow | controller 与 guest 双算并逐 cell bitwise compare | ownership/数值验证 | **保留在专项 suite，不用于性能** |
| B3 direct | controller column FMA 直接生成 base | B3 性能版 | **保留** |
| B4 off | 保留 B3 行为 | B4 控制组 | **保留一个性能/回归点** |
| B4 shadow | 双算 Lbar correction/dq_star | 数值/所有权验证 | **保留在专项 suite** |
| B4 direct | controller 直接完成 forward wavefront | B4 性能版 | **保留** |
| C1 line autonomous | 每 line parent token，controller 自动 refill | 删除逐 cell guest 调度 | **保留并作为主线** |
| C2/B5 Ubar direct | controller 内 backward reverse wavefront | 当前完整数据流 | **保留并作为主线** |
| multi-line frontier-aware | 多 line + aging 的 ready 调度 | 已实现但 tile/QoS 未完成 | **保留，作为当前调度主版本** |

性能证据也支持这一收敛：文档记录 1×17 下 Stage A 约 17300 cycles，B1+B2 曾退化到 23228，B1.5+B2.5 降到 15648，B3 direct 为 15466，B4 direct 为 16754，C1 为 12602，C2/B5 约 9640。跨构建数字不能直接作为严格 speedup，但足以说明 queue/reaper 路径不应继续作为默认实现，C1/C2/B5 才是当前完整路线。

### 4.3 Stage 6b/7/8 与后续实验

| 版本 | 状态 | 建议 |
| --- | --- | --- |
| internal/packet SPM | controller 内部仲裁模型 | **保留为快速、确定性基线** |
| Stage 6b RequestPort DMA | 已有 timing request/retry/outstanding | **保留实验**；在 multi-sweep dirty/rebuild 问题关闭前不设默认 |
| Stage 7 version/dirty reuse | 已实现 cache hit/rebuild | **保留实验**；先修复/锁定 DMA 多 sweep 一致性 |
| Stage 8 reciprocal | 每 pivot 求倒数后乘法归一化 | **保留为容差等价实验**；默认继续 `divide` |
| serial line MVM | 保持原 FP64 列顺序 | **默认保留** |
| base-ahead + early-backward | 两开关当前默认开启；命中时提前算 base，按数值就绪启动 backward | **保留主线及双关闭对照**；不改变对外完成必须等待退休的条件 |
| split line MVM | 独立乘法/加法流水 | 目前 matched 延迟下没有稳定优势，且改变中间舍入边界 | **保留一个实验分支，不进入默认** |

## 5. 资源配置版本建议

### 5.1 建议保留的正式配置点

1. **兼容控制组**：LU context 1 / TRSM context 1。
2. **推荐实验基线**：LU context 2 / TRSM context 2。
3. **性能参照**：LU context 2 / TRSM context 4。

2026-09-09 的 1×17 消融中，2/2 相对 1/1 降低约 16%–17%，2/4 仅比 2/2 再降低约 0.7%；4/4 没有更快。没有面积综合数据，因此不能称 2/2 为面积最优，但它是更合理的默认研究点。

### 5.2 建议不再保留为长期版本矩阵的点

- LU/TRSM context 的完整 3×3 笛卡尔积只保留原 campaign，不再每次回归全跑；
- `dotp_count=4`、过低 dotp latency 等只保留历史 sweep 数据；
- queue depth、consumer queue depth 在已证明不构成瓶颈后只留边界测试；
- reciprocal、split、full-array 标签不进入默认配置组合。

## 6. 推荐保留矩阵

### 6.1 必须保留（Tier 1）

- 软件 reference 与数值比较逻辑；
- decode-exclusive；
- Path A 当前 benchmark 和 baseline/internal-buffer 路径；
- standalone TRSV5；
- Step2 prepared 最小回归；
- raw TRSV5 完整成本基线；
- coeff3 event + auto-retire + fixed boundary mask；
- B3/B4/C1/C2/B5 direct 主实现；
- B3/B4 shadow 正确性回归；
- multi-line frontier-aware；
- `run_experiment.py`、`run_suite.py`、核心 suite 与 manifest 机制；
- 最新正式 correctness/performance campaign 及其 manifest。

### 6.2 应冻结保留（Tier 2）

- Step1；
- Step2 core/forwarded/context/linebuf；
- Step3 coarse controller；
- Step4 A/B/C controller；
- Path B/C；
- B1 queue/reaper 压力模型；
- coarse dual/coeff3；
- internal SPM、DMA、reuse、reciprocal、split 的代表性对照。

“冻结”表示只修复阻断构建或回归的问题，不再叠加新功能。每类只维护 1–2 个代表 suite/campaign。

### 6.3 建议迁档后删除（Tier 3）

- 根目录九个 `run_cfd_*.py` 兼容 shim；正式入口已经位于 `projects/cfd_dsa/configs/`；
- `test_cfd_pathc_sme_step_perf.c` 这种只 include 新文件的 wrapper；
- decoder 已拒绝的 `test_cfd_sme_step_perf.c` 与旧 `DSASmeFmopa5Step` 类；
- `projects/cfd_dsa/**/__pycache__` 和 `*.pyc`；
- 空 `stats.txt`、崩溃/超时结果和已经明确放入 `failed/` 的运行；
- `results/lusgs_raw/`、`results/lusgs_optprep/` 等旧顶层结果，在有迁移清单后删除；
- 名称带 `debug`、`final2`、`final3`、`final4`、`r2`、`r3` 且已被正式 campaign 完整取代的目录；
- `results/cfd_dsa/runs/` 中不被文档引用、无独特配置、无失败诊断价值的开发运行；
- 只记录旧结论且内容已并入规范文档的重复报告。

## 7. 不建议立即删除的对象

以下内容看似旧，但目前仍承担回归或因果对照，不能只按文件名删除：

- Step1/Step2 baseline：提供跨阶段数值基线；
- Step3/Step4：虽然不是当前主线，但覆盖 descriptor、token、cancel、wrong-path、queue/backpressure 生命周期；
- B3/B4 off/shadow：direct 出错时用于区分数值错误、数据所有权错误和性能回退；
- Path B/C：如果研究目标仍包含 SME/ZA 映射，它们是独立架构候选；
- internal SPM：用于区分控制器调度问题和真实 timing DMA 问题；
- divide：reciprocal 未证明 bitwise 等价，必须保留为默认语义；
- 1/1 context：最新消融的必要控制组；
- 历史正式 campaign：论文数字可复现前不能删除。

## 8. 结果目录清理策略

`results/cfd_dsa/` 已约 1.3 GiB，且存在 campaign 名称版本化而非 manifest 版本化的问题。建议：

1. 更新 `baseline-index.json`，加入当前 streaming 主线、C1、C2/B5、context 1/1、2/2、2/4 的正式证据；现有索引只覆盖 Step4 A/B/C，已经落后于当前主线。
2. 为文档中每个性能数字建立 `document -> campaign/case -> manifest -> binary hash` 关系。
3. 将 campaign 分为 `canonical`、`diagnostic`、`superseded`、`failed` 四类，并生成机器可读 inventory。
4. 同配置、同二进制、同结果时只保留最后一个通过的正式 case；较早目录只在确有调试价值时迁入压缩 archive。
5. 空 stats 或 manifest 显示失败的 case 移到 `failed/`；确认无诊断价值后删除。
6. 删除旧顶层结果前，核对它们是否已经出现在 `migration-inventory.json`。
7. 以后禁止 `final2/fix/rerun` 命名，使用不可变 campaign 和语义化 case 名。

建议结果保留周期：

| 结果类型 | 保留策略 |
| --- | --- |
| 正式 correctness/performance baseline | 长期保留 |
| 论文/文档引用结果 | 长期保留，必须有 manifest/hash |
| 失败但揭示过真实 bug 的最小复现 | 保留一个 |
| 灵敏度 sweep | 保留汇总表、suite 和关键拐点原始结果 |
| 中间 debug/rerun | 30–90 天后删除 |
| 可由当前代码廉价重建的 smoke | 只保留最新一轮 |

## 9. 代码结构收敛建议

### 9.1 立即做，风险低

- 清除并忽略 `__pycache__` / `*.pyc`；
- 给所有 runner profile 建立单一配置表，避免 base runner 已超过数千行且 wrapper 重复默认值；
- 把 Step2 的模式注册、CLI 校验和 guest mode dispatcher 生成自同一份定义；
- 把 `test_cfd_lusgs_patha_step2.c`（约 500 KiB）拆成 reference、descriptor ABI、prepared modes、raw modes、streaming modes、reporting 六个模块；
- 更新文档索引，把本文和 context/scheduler diagnostics 纳入正式阅读顺序；
- 给主线建立独立 suite，例如 `streaming-mainline.json`，不要依赖 `all` 模式。

### 9.2 第二阶段做，需要回归保障

- 将 Step3/Step4 runner、benchmark、controller 放到明确的 `legacy_macro_controller/` 命名空间；
- 删除根目录兼容 shim，同时更新所有文档和自动化命令；
- 移除旧 Path C FMOPA decode 死代码和 benchmark；
- 合并 prepared/core/forwarded/context 中数值等价且只差 staging 的公共 kernel；
- 将 off/shadow/direct 统一为可枚举 profile，避免非法组合通过多个布尔开关表达；
- 修复并锁定 DMA + dirty/rebuild 多 sweep 一致性后，再决定是否把 Stage 6b/7 设为默认。

### 9.3 暂缓

- 在没有 FPGA 综合数据前，不删除 1/1 与 2/2，也不把 2/4 宣称为最终硬件配置；
- 不用 reciprocal 替换 divide；
- 不用 split line MVM 替换 serial；
- 不因 C2/B5 周期更低而删除 B3/B4 shadow 和 software reference；
- 不把 gem5 event timing 直接解释为 ZCU104 RTL 时序。

## 10. 建议的目标版本集合

清理完成后，用户可见版本建议只保留下列七个 profile：

| Profile | 用途 | 组成 |
| --- | --- | --- |
| `reference` | 数值金标准 | software LU-SGS |
| `operator-baseline` | 算子回归 | Path A + TRSV5 standalone |
| `prepared-step2` | 历史兼容 | prepared + Path A + TRSV5 |
| `raw-baseline` | 公平端到端基线 | raw + software LU + event/coarse Ubar + TRSV hot loop |
| `streaming-main` | 当前主版本 | event + auto + mask + B3/B4 + C1/C2/B5 + frontier-aware + divide + serial |
| `streaming-check` | 强正确性 | main + shadow/full validation/1×17 |
| `streaming-experimental` | 架构探索 | DMA/reuse/reciprocal/split/context sweep 显式开启 |

Step1、Step3、Step4、Path B、Path C 不再作为一级用户 profile 出现在默认帮助中，而放入 `legacy-regression` 或 `architecture-experiments` suite。

上述七个 profile 是建议的目标命名，**尚未实现为 CLI 选项**。现有默认仍是 Step2 `all`、LU/TRSM context 1/1、三个 consumer 为 off、line-autonomous 为 0；本文建议不等于已经修改默认参数。2/2 的证据仅覆盖 1×17 serial/divide、固定资源 cold/mixed 测试，长 line、多 line 和 DMA 场景还需单独验证。

## 11. 推荐执行顺序

1. **先提交当前工作区快照**：源码、runner、suite、文档分开提交；结果只提交索引/manifest 或使用外部制品存储。
2. **建立主线回归**：reference、decode、Path A、TRSV5、prepared Step2、streaming-check、multi-line C2/B5。
3. **更新 baseline-index**：加入 2026-09 streaming 与 context 证据。
4. **清理生成物和失败结果**：pycache、空 stats、未引用 runs。
5. **迁档重复 campaign**：先生成清单和 hash，再删除被取代目录。
6. **移除兼容入口/死代码**：root shim、旧 Path C wrapper/FMOPA。
7. **重构大 benchmark 与配置矩阵**：在回归稳定后进行。
8. **最后决定 Step3/4、Path B/C 是否独立分支化**：取决于后续论文是否仍研究 macro-controller 或 SME/ZA。

## 12. 最终建议

如果目标是形成一个可继续推进到 RTL 的清晰版本，建议：

- **主线保留**：Step2 raw event streaming 的 C1/C2/B5 wavefront；
- **默认算术语义**：divide + serial column accumulation；
- **默认研究资源点**：2/2 context，同时保留 1/1 控制和 2/4 性能参照；
- **正确性护栏**：software reference + B3/B4 shadow + decode/TRSV5/Path A；
- **历史冻结**：Step1、Step3、Step4、Path B、Path C；
- **优先删除**：生成物、失败/重复结果、兼容 wrapper、不可解码旧 Path C；
- **暂不删除**：off/shadow、internal SPM、divide、1/1 context 和正式历史 evidence。

这套收敛方式既能显著减少“版本太多”的认知负担，又不会丢掉验证 direct/wavefront 正确性所需的关键对照。

## 13. 证据入口与审计边界

本次进行了静态实现核对和现有实验报告核对，未重新编译 gem5 或重跑历史 campaign。文中性能均为已有记录，不代表本次重新测得。

| 判断 | 可追溯依据 |
| --- | --- |
| 正式构建目标与兼容目标 | [build_benchmarks.py](../tools/build_benchmarks.py)，`BENCHMARKS` |
| 正式 runner 与 manifest | [run_experiment.py](../tools/run_experiment.py)，`TARGETS` / `main` |
| 默认配置、streaming consumer 开关 | [Step2 runner](../configs/lusgs/run_cfd_lusgs_step2.py) |
| 真实 mode 与别名 | [Step2 benchmark](../benchmarks/lusgs/test_cfd_lusgs_patha_step2.c)，`--lusgs-mode` 分派（约 7063 行起） |
| line autonomous/backward 实现 | [coefficient controller](../../../src/arch/arm/cfd_coeff_preprocess_controller.cc)，`launchLine` / `processLineBackward` |
| 旧 FMOPA 编码已拒绝 | [custom decoder](../../../src/arch/arm/isa/formats/custom_cfd.isa)，约 140 行 |
| Streaming A–C、DMA/reuse、依赖优化与 split | [Streaming 专项报告](CFD_DSA_LUSGS_STEP2_STREAMING.md)，第 9、13、16、17 节 |
| context 2/2 与 2/4 的实测范围 | [上下文消融](CFD_DSA_CONTEXT_ABLATION.md) |
| 最新调度诊断与统计解释 | [调度拒绝与背压诊断](CFD_DSA_SCHEDULER_DIAGNOSTICS.md) |
| Step3/4 控制器演进 | [完整历史报告](CFD_DSA_LUSGS_FULL_TECHNICAL_REPORT.md)及[Step4](CFD_DSA_LUSGS_STEP4.md) |
| 结果迁档规范 | [RESULT_MANAGEMENT.md](RESULT_MANAGEMENT.md) |

### 清理建议的严格含义

本次没有完成逐文件 hash 去重和全部引用闭包审查，因此带 `final/debug/r2` 后缀的目录只属于**审查候选**，不能按名称批量删除。空 `stats.txt` 也只说明该运行没有完整统计，不能证明同目录 trace/simout 没有诊断价值。尤其 DMA dirty-rebuild 的失败证据和独立诊断 suite 应保留到问题修复并回归通过。

根目录 shim 和 Path C wrapper 删除前须更新旧命令的引用；旧 FMOPA 类删除前须同时核查头文件、OpClass/FU、ISA include 和 squash 测试依赖。`configs/run_cfd_dsa.py` 是共享完整配置实现，**不是应删除的兼容 shim**；早期 Path A 文档对此有过时表述，以实际 `runpy` 调用链为准。

本文第 6–11 节的删除、迁移、重命名、提交和默认值调整均为后续建议；本次交付只新增说明文档与索引条目。
