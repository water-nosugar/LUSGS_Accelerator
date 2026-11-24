# 阶段 2 技术报告：真实 2D `8×8` SME 外积阵列基线与两级分段外积（V32）

2026-11-23。本报告归档阶段 2（**Step 2A 真实 2D `8×8` 外积阵列基线 `2d-base`** + **Step 2B 两级递进分段外积 `BD-FMOPA (2d-bd)` → `RS-FMOPA (2d-rs)`**）的完整 gem5 实现、代码审查与缺陷修复、全量回归验证及 4096 点论文级消融实验结果。设计规格见 [CFD_DSA_SME_V32_2D_SEGMENTED_SPEC_20261122.md](CFD_DSA_SME_V32_2D_SEGMENTED_SPEC_20261122.md)。

构建产物：`build/cfd_dsa/sme-za/v32-2d/gem5.opt`；24 组 4096 点全矩阵归档于 `results/cfd_dsa/campaigns/sme-za-v32-2d-20261123/`（汇总 [summary.json](../../../results/cfd_dsa/analysis/sme-za-v32-2d-20261123/summary.json)）；代码审计修复后的 6 组核心 `T1 6R/3W` 4096 点全量复测归档于 `results/cfd_dsa/campaigns/sme-za-v32-2d-fix-20261123/`（汇总 [summary.json](../../../results/cfd_dsa/analysis/sme-za-v32-2d-fix-20261123/summary.json)）。

---

## 1. 实现清单与代码审计修复

### 1.1 核心代码改动边界（向后兼容 1D V30/V31）

| 文件 | 核心改动 |
| :--- | :--- |
| [src/arch/arm/cfd_sme_za.hh](../../../src/arch/arm/cfd_sme_za.hh) | 新增 `ArrayMode { OneD, TwoDBase, TwoDBd, TwoDRs }`、`Op2d`、`Segment2d`、`Return2d`、`Stats2d`（含 `fmopaBundles`/`fmopaPeActive`）、`issue2dBundle(...)`、分布式 PE 本地写回占位 `peWriteUsed[8]` 及 `reservedBytes()` 2D 广播/调度硬件计费（`2d-base` `+128 B`、`2d-bd` `+256 B`、`2d-rs` `+256+(S-1)*128 B`）。1D 兼容周期见第 2 节。 |
| [src/arch/arm/cfd_coeff_preprocess_sme_2d.inc](../../../src/arch/arm/cfd_coeff_preprocess_sme_2d.inc) | 实现静态循环偏移表 `Sme2dOffsetTable = {0,4,3,7,1,5,2,6}`、冷轮 `LU` / `[R\|L]` / `U` / 稀疏 $I_5$ / 延迟 `ReverseRhs` 与暖轮 `direct` / `inv-fma1` / `inv-fma2` / `split-back` 全 2D 微程序，以及每 Tile 独立候选提交 + 互补阶段打包调度器（Offer → Pack → Commit）。 |
| [src/arch/arm/cfd_coeff_preprocess_sme.inc](../../../src/arch/arm/cfd_coeff_preprocess_sme.inc)、[cfd_coeff_preprocess_sme_split_back.inc](../../../src/arch/arm/cfd_coeff_preprocess_sme_split_back.inc)、[cfd_coeff_preprocess_tiles.inc](../../../src/arch/arm/cfd_coeff_preprocess_tiles.inc)、[cfd_coeff_preprocess_controller.cc](../../../src/arch/arm/cfd_coeff_preprocess_controller.cc) | 接入 2D 调度钩子、`GEM5_CFD_SME_ARRAY_MODE` / `GEM5_CFD_SME_2D_SEGMENTS` / `GEM5_CFD_SME_2D_PANEL_PACK` 参数解析、`reservedBytes` 透传以及 `CFD_SME_CONFIG` / `CFD_SME_2D_STATS` 逐轮遥测输出。 |
| [projects/cfd_dsa/configs/single_line_contract.py](../configs/single_line_contract.py) & [run_cfd_dsa.py](../configs/run_cfd_dsa.py) | 同步 CLI 校验（`--coeff-sme-array-mode`、`--coeff-sme-2d-segments`、`--coeff-sme-2d-panel-pack`）、默认 64 PE 网格检查与 `sme_za_reserved_bytes(...)` 合同计费。 |
| [projects/cfd_dsa/tools/check_sme_za.py](../tools/check_sme_za.py) | 新增 `CFD_SME_2D_STATS` 全不变量强校验：算术守恒 `pe_active_sum == sum(fma+mul+add)`、物理上限 `pe_active_sum <= bundles_total*64 <= sweep_cycles*64`、`fmopa` 子项闭合、`2d-base` 零多段发射及双口径利用率报告。 |
| [projects/cfd_dsa/tests/fixtures/sme_za_model.cc](../tests/fixtures/sme_za_model.cc) & [test_sme_za.py](../tests/arithmetic/test_sme_za.py) | 新增原生 C++ 2D 逐拍单测（`reservedBytes` 一致性、`2d-base` 写回冲突与外部 `load` 同 PE 写口阻塞、`2d-bd` `4×4+3×3+1×1` 零交叉污染、`2d-rs` `4×6+4×6` 同列重叠打包）与 Python 合同单测。 |

### 1.2 实现后代码审查发现并修复的 3 处缺陷

1. **`inv-fma2` + `critical-arb=1` 双链重复提交缺陷**（[cfd_coeff_preprocess_sme_2d.inc](../../../src/arch/arm/cfd_coeff_preprocess_sme_2d.inc#L827-L904)）：`advanceSmeInverseWarmRhs2d` 的 `dual == true` 分支漏检 `if (offered) return;`，导致临界前沿单元在 `processSmeCriticalRhs2d` 与 `processSmeRequests2d` 被重复提交两次，虚增 `offeredNow` 与 `pack_stall_row`。已补齐 `if (offered) return;` 与 `dualOffered` 聚合提交。
2. **暖轮 `processSmeSplitBack2d` 旁路锁存器物理行人为冲突**（[cfd_coeff_preprocess_sme_2d.inc](../../../src/arch/arm/cfd_coeff_preprocess_sme_2d.inc#L1788-L1838)）：原 `(slotIdx*4 + part*2 + (col&1))&7` 将 `col=0`/`col=2` 及 `node=0`/`node=2` 映射至同一物理行，且避让不当会与非旁路 `IngressRow` 行 `{1,3,4}` 冲突。已改为无冲突查表 `SplitBackBypassRow = {0, 2, 5, 6, 7, 2}`，恢复 `2d-rs(S=3)` 暖轮精确贴合 17 拍/点下界（65 点 `2,625` 拍，`inv-fma2` `2,619` 拍）。
3. **2D 分布式 PE RF 单写口严格闭合与冲突归因**（[cfd_sme_za.hh](../../../src/arch/arm/cfd_sme_za.hh#L249-L267)、[L1045-L1095](../../../src/arch/arm/cfd_sme_za.hh#L1045-L1095)）：
   - `tick()` 写回冲突将 `returnWait` 精准归因到实际冲突段 `sub.phase`；
   - `load()` 与 `loadVertical()` 在 2D 模式下同步检查并占位 `peWriteUsed`，杜绝外部装载与同拍 PE 写回或跨 Tile 同行装载共享同一 PE 本地 RF 写口的隐形免费带宽，并在 [sme_za_model.cc](../tests/fixtures/sme_za_model.cc#L346-L386) 新增逐拍断言锁定。

---

## 2. 单元测试、功能安全与边界回归验证

* **全量单元测试**：`python3 -B -m unittest discover -s projects/cfd_dsa/tests -t projects/cfd_dsa/tests` — **541/541 全部通过**（含新增 2D 原生 C++ 与 Python 合同单测）。
* **1D 向后兼容**：`compat-1d-warm65-inv1` 冷/暖周期 **`5,465 / 2,624`**，`max_abs = 1.11e-16`，与 V31-fix 逐拍一致。
* **短线、边界、病态块和取消恢复**（结果向量与独立 Decimal 参考的 `mismatches == 0`）：
  * `warm1-t1-inv1`（4 种 2D 模式）：`751 / 592 / 594`，`max_abs = 2.78e-17`；
  * `equal-pivots-guarded`（等主元上双对角反例，`2d-base` / `2d-bd` / `2d-rs`）：**三轮 `max_abs = 0.00e+00`**，条件数门控精准回退；
  * `illcond65-inv1-guarded`（10 个 $\kappa \approx 10^8$ 病态块，`2d-base` / `2d-bd` / `2d-rs`）：**`inv=55, fallback=10`**, `mismatches = [0, 0, 0]`, `max_abs = 4.896e-11`；
  * `recover17-fwd` & `recover17-back`（`2d-base` / `2d-bd` / `2d-rs`）：3/3 在途取消均干净排空并恢复通过，`max_abs = 1.67e-16`；
  * 扩展模式全通过（修复后复测 `15/15`）：`inv-fma2`（`S=2/3`）、`external` Ubar、`upper-split=3`、稠密 $I_5$（`inv-sparse-i5=0`）、`guard0` 混合回退压力测试，全部 `max_abs = 1.11e-16`。

---

## 3. 4096 点核心消融实验结果（固定 64-PE 2D `8×8` 外积阵）

### 3.1 核心两级递进消融（`T1 6R/3W`，2 GHz ASIC FP64，原生 6 列 `[R|L]` `split-rhs=0`）

下表列出**代码审计修复后 `v32-2d-fix`（`6/6` 通过，`max_abs = 1.67e-16`）**的 4096 点实测数据，并对照 V31-fix 1D 10-lane 基线：

| 模式配置（4096 点 `T1 6R/3W`） | `direct` / `inv1` | 冷轮周期 | 相对 `2d-base` | 活跃拍 PE 填充率 | `FMOPA` PE 填充率 | 整轮 PE 利用率 | 平均段数/拍 | 暖轮周期 | 相对 `2d-base` |
| :--- | :---: | ---: | :---: | :---: | :---: | :---: | :---: | ---: | :---: |
| *参考：V31-fix `1d` (10-lane)* | `direct` | *241,302* | — | — | — | *70.43% (/10)* | — | *139,803* | — |
| *参考：V31-fix `1d` (10-lane)* | `inv-fma1` | *294,796* | — | — | — | *68.77% (/10)* | — | *139,768* | — |
| **`2d-base` (Step 2A 标准 2D)** | `direct` | 248,662 | 1.00× | 12.0% | 19.5% | 10.7% | 1.00 | 184,869 | 1.00× |
| **`2d-rs` (`S=3`, Step 2B-T2)** | `direct` | **200,839** | **−19.2% (1.24×)** | **18.4%** | **26.3%** | **13.2%** | **1.53** | **139,803** | **−24.4% (1.32×)** |
| **`2d-base` (Step 2A 标准 2D)** | `inv-fma1` | 324,449 | 1.00× | 11.7% | 17.9% | 9.8% | 1.00 | 168,655 | 1.00× |
| **`2d-bd` (`S=3`, Step 2B-T1)** | `inv-fma1` | 268,229 | −17.3% (1.21×) | 13.3% | 21.3% | 11.8% | 1.14 | 156,144 | −7.4% (1.08×) |
| **`2d-rs` (`S=2`, Step 2B-T2)** | `inv-fma1` | 222,358 | −31.5% (1.46×) | 17.1% | **28.1%** | 14.3% | 1.46 | 145,908 | −13.5% (1.16×) |
| **`2d-rs` (`S=3`, Step 2B-T2)** | `inv-fma1` | **209,450** | **−35.4% (1.55×)** | **18.6%** | **27.6%** | **15.1%** | **1.59** | **139,767** | **−17.1% (1.21×)** |

> **核心定量发现**：
> 1. **冷轮求逆开销 $\Delta T_{\text{cold}} = T_{\text{cold}}(\text{inv1}) - T_{\text{cold}}(\text{direct})$ 缩减 8.8×**：标准 `2d-base` 下冷求逆需额外付出 **`+75,787` 拍（+30.5%）**；开启 `RS-FMOPA (S=3)` 后仅增 **`+8,611` 拍（+4.3%）**，且冷轮绝对周期 **`209,450`** 反而比 1D 10-lane V31（`294,796`）**快 29.0%**、甚至比 1D 10-lane V30 `direct`（`241,302`）还**快 13.2%**。
> 2. **暖轮全面消除单发射串行化瓶颈**：标准 `2d-base` 暖轮因 1-row 向量操作单发受阻，比 17 拍/点单线递推下界慢 **+28,888 ~ +45,066 拍**；`2d-rs(S=3)` 将 `direct` / `inv-fma1` 暖轮同步压回精确物理下界 **`139,803` / `139,767` 拍**。
> 3. **硬件打包阻塞根因铁证**：在 `2d-bd(S=3)` 中，**列掩码碰撞 `pack_stall_col = 130,370`** 占全部打包受阻的 **81.4%**（行碰撞仅 28,782），直接从硬件遥测层面证实 5×5/5×6 面板无法通过纯块对角突破列宽天花板；切换至 `2d-rs` 后 `pack_stall_col` 归零（**`0`**），释放全部行分段并发。

### 3.2 24 组全矩阵消融汇总（端口敏感性、`T2` FPGA 时序与 `split-rhs` 对照）

以下为 `sme-za-v32-2d-20261123` 完整 24 组 4096 点归档（全部 `max_abs = 1.67e-16`，`24/24` 通过）：

| 实验维度 | 配置（4096 点 `inv-fma1` 除非标明 `dir`） | 冷轮周期 | 活跃拍填充 / `FMOPA` 填充 | 平均段数 | `pack_stall` (行/列/读口) | 暖轮周期 |
| :--- | :--- | ---: | :---: | :---: | :---: | ---: |
| **端口敏感 (`T1`)** | `4R/2W` `2d-bd(S=3)` | 283,875 | 13.1% / 20.4% | 1.12 | 25,514 / 132,159 / 6,285 | 156,144 |
| **端口敏感 (`T1`)** | `4R/2W` `2d-rs(S=2)` | 236,146 | 16.3% / 24.8% | 1.39 | 26,304 / 0 / 18,851 | 145,908 |
| **端口敏感 (`T1`)** | `4R/2W` `2d-rs(S=3)` | 228,375 | 17.8% / 25.0% | 1.52 | 58,228 / 0 / **29,084** | 139,767 |
| **甜蜜点 (`T1`)** | **`6R/3W` `2d-rs(S=3)`** | **208,697** | **18.8% / 27.8%** | **1.60** | 64,217 / 0 / **7,020** | **139,767** |
| **满端口 (`T1`)** | `12R/6W` `2d-base` | 324,324 | 11.7% / 17.9% | 1.00 | 0 / 0 / 0 | 168,428 |
| **满端口 (`T1`)** | `12R/6W` `2d-bd(S=3)` | 261,791 | 13.3% / 21.2% | 1.14 | 29,581 / 129,943 / 49 | 156,144 |
| **满端口 (`T1`)** | `12R/6W` `2d-rs(S=2)` | 214,005 | 17.1% / 27.2% | 1.46 | 30,460 / 0 / 193 | 145,908 |
| **满端口 (`T1`)** | `12R/6W` `2d-rs(S=3)` | **204,899** | **19.2% / 28.2%** | **1.64** | 65,484 / 0 / **264** | **139,767** |
| **满端口 (`T1`)** | `12R/6W` `dir` `2d-base` → `2d-rs(S=3)` | 243,602 → **201,543** | 12.0% → 18.4% | 1.00 → 1.54 | — → 35,734 / 0 / 206 | 184,877 → **139,803** |
| **FPGA `T2` `6R/3W`** | `2d-base` | 332,371 | 11.7% / 17.9% | 1.00 | 0 / 0 / 0 | 232,171 |
| **FPGA `T2` `6R/3W`** | `2d-bd(S=3)` | 309,814 | 12.8% / 20.3% | 1.10 | 43,379 / 107,508 / 773 | 221,975 |
| **FPGA `T2` `6R/3W`** | `2d-rs(S=2)` | 276,276 | 15.6% / 24.4% | 1.33 | 43,899 / 0 / 2,889 | 209,387 |
| **FPGA `T2` `6R/3W`** | **`2d-rs(S=3)`** | **272,546** | **16.8% / 24.3%** | **1.43** | 69,673 / 0 / 3,159 | **205,293** |
| **FPGA `T2` `6R/3W`** | `dir` `2d-base` → `2d-rs(S=3)` | 277,502 → **238,294** | 12.0% → 17.8% | 1.00 → 1.49 | — → 43,059 / 0 / 3,583 | 232,104 → **205,349** |
| **面板完整性 (`T1`)** | `6R/3W` `2d-rs(S=3)` `split-rhs=1` 对照 | 225,694 | 16.9% / 23.0% | 1.73 | 70,973 / 0 / 11,768 | 139,767 |

* **端口甜蜜点结论（已用修订版 `v32-2d-review` 4096 点实测证清）**：旧版 `pack_stall_readport` 在 `ingressPorts < maxSegments` 时会混入 ingress 端口阻塞，修订版 `build/cfd_dsa/sme-za/v32-2d-review/gem5.opt` 已拆分 `pack_stall_ingress`。使用修订版在 4096 点标准配置（`ingressPorts=3 == maxSegments`）重跑 4 组关键用例（归档 `results/cfd_dsa/campaigns/sme-za-v32-2d-review-20261123/`，全部 `max_abs = 1.67e-16`）实测确认 **4 组冷/暖轮 `pack_stall_ingress` 均为 `0`**：
  * `2d-rs(S=3)` `4R/2W`：冷 `229,561`，`pack_stall_readport = 30,295`、`pack_stall_ingress = 0`；
  * `2d-rs(S=3)` **`6R/3W`**：冷 **`209,450`**，`pack_stall_readport = 6,694`（降 **4.53×**）、`pack_stall_ingress = 0`，已拿下 `12R/6W`（冷 `203,024`，`pack_stall_readport = 269`、`pack_stall_ingress = 0`）吞吐的 **96.9%**；
  * `2d-bd(S=3)` `6R/3W`：冷 `268,229`，`pack_stall_col = 129,662`、`pack_stall_readport = 1,016`、`pack_stall_ingress = 0`。
  这组单线仿真中，`6R/3W` 取得了接近 `12R/6W` 的吞吐。
* **2D 面板合并结论**：关闭 1D 遗留的 `--coeff-sme-split-rhs=1`（保持原生 6 列 `[R|L]` 面板）使 `2d-rs(S=3)` 冷轮再降 **16,997 拍（−7.5%）**，`FMOPA` PE 填充率提升 **+4.8 个百分点**（23.0% → 27.8%）。

---

### 3.3 组合方案 A + 方案 B 深度优化与 4096 点 2×2 消融（`v32-2d-ab`）

在 `2d-rs` 基础上进一步实现并调优两项微架构增强，构建产物 `build/cfd_dsa/sme-za/v32-2d-ab/gem5.opt`，12 组 4096 点全矩阵归档于 `results/cfd_dsa/campaigns/sme-za-v32-2d-ab-20261123/`（汇总 [summary.json](../../../results/cfd_dsa/analysis/sme-za-v32-2d-ab-20261123/summary.json)）：

* **方案 A（`--coeff-sme-2d-mixed-kind=1`，模型控制预留 `+128 B`）**：
  1. 允许同一 8 行 `RS-FMOPA` bundle 内跨操作类型混排（`FMA` / `MUL` / `ADD`），使单行 `ScaleRow`/`IngressRow`/`CombineRow` 与多行 `FMOPA` 填补互补行槽；
  2. `CfdSmeZa::tick()` 按段独立按各自管线延迟（`MUL/ADD=4`, `FMA=5`）退休。该归档版本在 2D 返回段之间采用 `Backward > Forward > LU > Rhs/ReverseRhs` 的优先级，除法返回先于这些段处理；
  3. 发射打包前瞻检测未来写回冲突（`willCollideWriteback2d`），在同关键级候选中优先避让冲突段。
* **方案 B（`--coeff-sme-2d-panel-pack=1`，模型控制预留 `+128 B`）**：
  1. 针对冷求逆内部单元（`smeInversePolicy == true` 且非边界点），将原本 3 块窄面板 `[R|L](6)` + `U(5)` + `I5(5)`（34 拍/点）融合为 2 块满 8 列面板 **Panel A `[R(1)|L(5)|U0..1(2)]`**（`tile+1`）与 **Panel B `[U2..4(3)|I5(5)]`**（`tile+2`，优先级 `5`），彻底省去第 3 块面板 Pass 2（34 → 26 拍/点）；
  2. 对无冷求逆的 `direct` 线自动回退原生 `6+5` 面板，避免给 `[R|L]` 引入多余的 `U` 到达依赖。

下表列出 `v32-2d-ab` 12 组 4096 点实测结果（全部 `12/12` 通过 `check_sme_za.py`，`max_abs = 1.67e-16`，`pack_stall_ingress = 0`）：

| 配置（4096 点，`2d-rs`，除非标明均为 `inv-fma1` `S=3`） | 冷轮周期 | `prep+fwd` / `back` | 相对基线变化 | 暖轮周期 | 活跃拍填充 / `FMOPA` 填充 | 平均段数 | 发射 Bundle 数 | ZA 读 / 写次数 | `fwd return_wait`（旧计数） | 模型预留 |
| :--- | ---: | :---: | :---: | ---: | :---: | :---: | ---: | :---: | ---: | ---: |
| `T1 6R/3W` `rs3-base` (`A=0, B=0`) | 209,450 | 137,244 / 71,813 | 基线 | 139,767 | 18.63% / 27.57% | 1.59 | 170,009 | 538.4k / 409.6k | 7,006 | 31,232 B |
| `T1 6R/3W` **`+方案 A`** (`A=1, B=0`) | 200,926 | 130,632 / **69,901** | −4.07% | 139,784 | 20.85% / 21.66% | **1.78** | 151,888 | 548.1k / 409.6k | **404** | 31,360 B |
| `T1 6R/3W` **`+方案 B`** (`A=0, B=1`) | 202,082 | **129,845** / 71,815 | −3.52% | 139,938 | 20.52% / **29.85%** | 1.54 | 154,364 | **474.6k** / **356.3k** | 7,023 | 31,360 B |
| `T1 6R/3W` **`+方案 A+B`** (`A=1, B=1`) | **200,199** | **129,907** / **69,899** | **−4.42% (vs `2d-base` −38.3%)** | **139,767** | **22.46%** / 24.70% | 1.68 | **141,007 (−17.1%)** | **488.3k / 356.3k** | **559 (−92%)** | 31,488 B |
| `T1 6R/3W` `S=2` `+方案 A+B` | 202,534 | 130,326 / 71,815 | −3.30% (超越 `S=3` 基线) | 143,956 | 20.97% / 24.60% | 1.57 | 151,023 | 486.2k / 356.3k | 401 | 31,360 B |
| `T1 6R/3W` `direct` `+方案 A+B` | 201,085 | 130,794 / 69,898 | — | 139,804 | 19.68% / 21.99% | 1.64 | 134,945 | 468.9k / 339.9k | 525 | 31,360 B |
| **`T1 4R/2W`** `+方案 A+B` | **201,163** | 130,996 / 69,774 | **vs `4R/2W` 基线 `229,561` −12.4%** | **139,767** | **21.88%** / 23.66% | 1.64 | 144,779 | 482.5k / 356.3k | 435 | 31,488 B |
| `T1 12R/6W` `+方案 A` / `+方案 B` / `+方案 A+B` | 199,681 / 201,936 / **200,330** | 130,168 / 69,769 | vs `12R/6W` 基线 `203,024` −1.3% | **139,767** | **22.46%** / 24.71% | 1.68 | 141,026 | 489.4k / 356.3k | 548 | 31,488 B |
| **`T2 6R/3W` (FPGA)** `+方案 A+B` | **243,435** | 140,506 / 102,544 | **vs `T2` 基线 `272,546` −10.7%** | **205,293** | **20.41%** / 23.48% | 1.53 | 155,206 | 464.9k / 356.3k | 524 | 31,488 B |
| `T2 6R/3W` (FPGA) `direct` `+方案 A+B` | **232,344** | 129,415 / 102,544 | vs `T2 dir` 基线 `238,294` −2.5% | **205,349** | 18.83% / 21.59% | 1.57 | 141,019 | 438.8k / 339.9k | 552 | 31,360 B |

旧版 `return_wait` 在关闭 A 时统计被阻塞的 bundle，在打开 A 时统计被阻塞的段。两种记录都保留在原表中。修正版的 `return_blocked_cycles` 为每相位每拍最多加 1。T2 的实际模型参数为 FMA 8 拍、MUL/ADD 6 拍、DIV 延迟 18 拍、DIV 启动间隔 4 拍。

在这一组 T1 归档数据中，A+B 求逆冷轮为 200,199 拍，同配置 A+B direct 为 201,085 拍。求逆仍执行了 286,720 次额外 RHS FMA 和 40,960 次额外 RHS MUL。剩余的周期差受到面板装填、发射与写回重叠影响。

## 4. A+B 审计后的实现

- 清单、解析配置和运行日志交叉校验 A/B。融合面板记录实际列掩码，前置装填依赖由验收器逐项核对。Panel A 需要 R、L 和 U，Panel B 还需要 LU 完成以确定求逆分支。
- 除法返回与各 2D 段一起参加混排模式的相位仲裁。命中同一物理 PE 时优先处理 Backward，其次为 Forward、LU 和 RHS。所有发射接口共用返回槽额度。未来写回冲突按标称到期拍查位图，超过标称到期拍的段继续参加实际写回仲裁。发射选择按同样的优先级直接找最优候选，ZA 切片列表每个候选只收集一次。
- 求逆门控在 D 装填与逆矩阵导出时累计 FP64 最大幅值，复用已有数据传输。条件数判据、非有限值拒绝和逐点回退仍按原门槛执行。
- 2D 容量合同给每个共用返回槽预留 512 B FP64 数据和 128 B 元数据。深度为 32 时，此项比历史计费多 16,384 B。A/B 各自的 128 B 是模型控制预留。按位估算的缓冲容量与综合逻辑资源使用不同单位。

### 4.1 RTL 子模块

RS 选择器使用平衡比较树，并检查行掩码、ZA 读切片共享、ingress 额度及旁路目标冲突。8×8 返回槽按段接收和退休结果。写回受阻时按行保留数据，下一次获准后退休。Icarus Verilog 对 S=2 和 S=3 各运行 2,048 组选择用例，也验证了返回槽的提前返回、滞留、同时接收与退休、复用和复位。测试中使用 8 个候选及 16 个窗口内读切片 ID。

| 模块 | Yosys UltraScale+ 逻辑单元估算 | FDRE |
| --- | ---: | ---: |
| RS 选择器 S=2 | 2,238 | 0 |
| RS 选择器 S=3 | 2,798 | 0 |
| 单个 8×8 返回槽 S=3 | 4,154 | 4,186 |

选择器接收已就绪的候选、读切片 ID 和标称写回占位。返回槽接收外部仲裁授权。资源表覆盖这两个独立模块，完整候选前端、FP64 运算、全队列仲裁和物理 ZA 读写网络仍需合成。ZCU104 的布局布线脚本默认以 4 ns 为目标周期并生成时序报告。本机完成了 RTL 仿真和原语映射。目标板 Fmax 需要实际布线结果。ZCU104 使用 XCZU7EV-FFVC1156-2-E。[RTL 测试日志](../../../results/cfd_dsa/analysis/sme-za-v32-2d-ab-corrected-20261123/rtl-simulation.log) · [ZCU104 时序脚本](../../../fpga/cfd_lusgs/synth/sme_za_ab.tcl) · [AMD ZCU104 平台规格](https://docs.amd.com/r/2025.1-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87/ug1702-vitis-accelerated-reference/xilinx_zcu104_base_202510_1-%E7%9A%84%E5%B9%B3%E5%8F%B0%E4%BF%A1%E6%81%AF)

### 4.2 边界回归

544 项单元测试及 12 组 gem5 边界算例通过。覆盖 1D、BD、RS、单点、提前装填、`inv-fma2`、稠密 I₅、外部 Ubar、两组各三次在途取消与恢复，以及返回深度和 ingress 端口数都为 1 的压力用例。最后一组冷、暖轮的最大返回占用均为 1。1D 兼容用例的冷轮为 5,465 拍，暖轮为 2,624 拍。[边界回归汇总](../../../results/cfd_dsa/analysis/sme-za-v32-2d-ab-corrected-20261123/regression-summary.json)

另外复现了病态 65 点数据的既有私有 Ubar 验收失败。结果向量三轮均为零不匹配，55 个点使用逆块，10 个点回退。中间私有 Ubar 的独立 Decimal 对照仍超出既有阈值。修正版与 V31-fix 的 192 条输入及输出矩阵快照逐值一致。这一结果保留在边界回归汇总中。

### 4.3 修正版 4096 点复测

13 组均通过算术、容量、输入追踪及独立数值验收。冷、暖轮最大绝对误差均为 `1.67e-16`。各组使用同一修正版 gem5 构建，原始仿真日志和 CSV 以 gzip 保留。[复测汇总](../../../results/cfd_dsa/analysis/sme-za-v32-2d-ab-corrected-20261123/summary.json)

| 时序与端口 | 模式 | 冷轮拍数 | 暖轮拍数 | 冷轮前扫写回受阻拍数 |
| --- | --- | ---: | ---: | ---: |
| T1 6R/3W | 基线 | 209,450 | 139,767 | 7,541 |
| T1 6R/3W | 仅 A | 199,983 | 139,767 | 0 |
| T1 6R/3W | 仅 B | 202,082 | 139,938 | 7,537 |
| T1 6R/3W | A+B | 200,168 | 139,767 | 0 |
| T1 6R/3W | A+B，S=2 | 202,177 | 143,861 | 0 |
| T1 6R/3W | A+B，direct | 199,094 | 139,804 | 0 |
| T1 4R/2W | A+B | 200,035 | 139,784 | 0 |
| T1 12R/6W | 仅 A | 199,289 | 139,767 | 0 |
| T1 12R/6W | 仅 B | 201,936 | 139,767 | 8,795 |
| T1 12R/6W | A+B | 199,263 | 139,767 | 0 |
| T2 6R/3W | 基线 | 274,733 | 205,293 | 9,405 |
| T2 6R/3W | A+B | 243,024 | 205,293 | 0 |
| T2 6R/3W | A+B，direct | 232,773 | 205,349 | 0 |

- T1 6R/3W 下，A+B 冷轮比基线少 4.43%。仅开 A 再少 185 拍。A 将前扫受阻拍数从 7,541 降到 0，同时 RHS 受阻拍数从 25,481 升至 41,904。
- T2 6R/3W 下，A+B 冷轮比同构建基线少 11.54%。开启求逆比同配置 direct 多 10,251 拍。
- 单线 T1 算例对 4R/2W、6R/3W 和 12R/6W 的 A+B 端口额度变化不敏感。实际端口面积和整机吞吐还需要完整 ZA 读写网络的布局布线数据。
