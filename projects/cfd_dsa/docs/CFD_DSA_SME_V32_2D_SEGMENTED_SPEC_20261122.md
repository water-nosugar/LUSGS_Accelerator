# 阶段 2 微架构规格：真实 2D `8×8` SME 外积阵列与两级递进分段外积（V32）

2026-11-22。本文档给出面向 **5×5 块三对角 LU-SGS** 的 **Step 2A（真实 2D `8×8` 外积阵列基线，`2d-base`）+ Step 2B（两级递进分段外积：`BD-FMOPA` → `RS-FMOPA`）** 详细微架构方案与 gem5 落地规格。设计严格对齐现有代码结构（[cfd_sme_za.hh](../../../src/arch/arm/cfd_sme_za.hh)、[cfd_coeff_preprocess_sme.inc](../../../src/arch/arm/cfd_coeff_preprocess_sme.inc)、[run_cfd_dsa.py](../configs/run_cfd_dsa.py)、[check_sme_za.py](../tools/check_sme_za.py)），完整保留既有 1D V30/V31 模式与 540 项单元测试。

---

## 1. 总体微架构对比：1D 现有模型 vs Step 2A vs Step 2B 两级方案

| 模式标识 (`--coeff-sme-array-mode`) | 物理执行阵列 | 每拍最大分段数 $S$ | 广播总线 | 打包碰撞约束 | 5×5 单步典型 PE 利用率（分母=64） |
| :--- | :--- | :---: | :--- | :--- | :--- |
| **`1d`**（保留历史基线） | 1D 10-lane 行片段池 | — | 行片段 | 共享 10 lanes + ZA 行端口 | V30/V31 原有口径（完全兼容） |
| **`2d-base`（Step 2A）** | **2D `8×8`（64 PE）标准外积阵** | $S=1$ | 1 条 `a[0..7]` + 1 条 `b[0..7]` | 单上下文独占 | `LU`: **11.7%**；`I5`: **13.7%**；`U`: **19.5%**；`[R\|L]`: **23.4%** |
| **`2d-bd`（Step 2B-Tier1: `BD-FMOPA`）** | 2D `8×8` + 2-bit 段比较门控 | $S \le 3$（默认 3） | **1 条 `a[0..7]` + 1 条 `b[0..7]`**（零新增总线） | **`rowMask` 与 `colMask` 双互斥** | `4×4+3×3+1×1`: **40.6%**；`4×4+4×4`: **50.0%**；`5×5+3×3`: **53.1%** |
| **`2d-rs`（Step 2B-Tier2: `RS-FMOPA`）** | 2D `8×8` + 行级 $S$:1 `b` 选路 MUX | $S \in \{2, 3\}$（默认 2） | **1 条 `a[0..7]` + $S$ 条 `b_s[0..7]`** | **仅 `rowMask` 互斥**（各段独占全部 8 列） | 原 `[R\|L]`/`U` 布局：**65%–75%**；配合可选 8 列面板拼装（`panel-pack=1`）：**`4×8+4×8` / `4×8+3×8+1×8` = 100%** |

> **关键设计直觉**：
> - **`2d-bd`（Tier 1）** 证明“零新增广播线 + 2-bit 比较器”就能把纯 5×5 LU 尾块从 11.7% 提升 3.5×–4.3×，同时也用硬件实验直接暴露纯块对角面积受限于 $\sum n_i^2 \le 50\%$ 的物理上限；
> - **`2d-rs`（Tier 2）** 精准利用 5×5 LU-SGS 面板“行窄（1–5 行）而列宽（5–8 列）”的特征，仅在 **8 个行入口** 加 2:1 / 3:1 `b` 选路 MUX（无需 64 个 PE 级 MUX），彻底消除非对角空洞，将 2D `8×8` 活跃发射拍填充率推向高利用率区间。

---

## 2. Step 2A 详细规格：真实 2D `8×8` 外积阵列基线（`2d-base`）

### 2.1 硬件组织与数据通路
1. **PE 网格与分布式 ZA 累加器文件（Tile-Distributed Accumulator File）**：
   - 物理阵列为 `8×8 = 64` 个流水化 FP64 FMA/MUL/ADD PE：`PE[r][c]`（$r,c \in \{0..7\}$），每个 PE 本地持有全部 $T$ 块 ZA Tile 的对应元素 `tile_rf[0..T-1][r][c]`（$T = 3C + 2 + C_{\text{rev}}$，与现有 Tile 数完全一致）。
   - 执行 2D 外积时，所有使能的 `PE[r][c]` **原位并发读写**本地 `tile_rf[dstTile][r][c]`；单次 2D 外积的外部 ZA 切片端口开销严格对应操作数取数：1 次垂直切片读 `a` + 1 次水平切片读 `b`（计为 2 个 ZA 切片读端口）。
2. **单拍发射规则（`2d-base`）**：
   - 每拍最多向 2D `8×8` 阵列发射 **1 条算术命令**（$S = 1$）：
     - **2D 外积减法（`Fmopa`）**：给定 `(dstTile, rowMask, colMask, colSrc, colIdx, rowSrc)`，在使能的 `PE[r][c]` 上计算
       $$Z_{\text{dst}}[r,c] \leftarrow \text{fma}(-a[r],\, b[c],\, Z_{\text{dst}}[r,c])$$
       激活 PE 数为 `popcount(rowMask) * popcount(colMask)`，延迟为 `cfg.fmaLatency`（`T1=5`, `T2=8`）。
     - **1D 向量/行操作（`ScaleRow`、`CombineRow`、`IngressRow`、暖轮 `*Bypass` 系列）**：复用阵列中的 1 行 PE（`1 × popcount(colMask)`），在 `2d-base` 中占用该拍 1 个发射槽。
     - **独立流水化 FP64 除法器（`reciprocal`）**：旁挂于 2D 阵列，受 `divLatency`（`T1=12`, `T2=18`）与 `divIi`（`4`）约束，与 2D PE 网格并行执行。
3. **5×5 块 LU-SGS 各核向 2D `8×8` 的精确映射（操作总数严格守恒）**：
   - **冷轮 `LU`**（`k = 0..4`）：每步 1 次 `div` + 1 次 `1×(4-k)` `ScaleRow` + **1 次 `(4-k)×(4-k)` 2D `Fmopa`**（共 30 FMA + 10 MUL + 5 DIV）。
   - **冷轮 `[R|L]`（5×6）、`U`（5×5）、`ReverseRhs`**：前代 `k=0..3` 各发 1 次 `(4-k)×|J|` `Fmopa`；回代 `k=4..1` 各发 1 次 `k×|J|` `Fmopa`（总计 `20*|J|` FMA + `5*|J|` MUL）。
   - **冷轮稀疏 $I_5$（V31）**：前代 `k=0..3` 发 `4×1, 3×2, 2×3, 1×4`（20 FMA），回代 `k=4..1` 发 `4×5, 3×5, 2×5, 1×5`（50 FMA），总计 70 FMA + 10 MUL。
   - **前扫 / 后扫 / 暖轮 `direct` & `inv-fma1/2`**：保留 V30/V31 旁路锁存器与树归约语义，作为 `1`-row 微操作进入阵列统一调度。

### 2.2 元素级记分牌与返回退休模型
- **记分牌**：每块 Tile 保持 `busy[8]`、`valid[8]`、`loadBlock[8]`（各 8×8-bit）。发射前检查目标区与源操作数全部 `valid && !busy && !loadBlock`。
- **2D 流水线返回退休**：每拍发射的 1 个 2D 波前（`Return2d`）在 `cycle + lat` 拍同步写回各使能 `PE[r][c]` 的本地 `tile_rf[dstTile]`。每拍记录每个 PE 的写回占位位图 `peWriteUsed[8]`，若不同延迟波前（如 `MUL` 与 `FMA`）同拍撞写同一 PE 则阻塞并计入 `returnWait`。返回队列总深度受 `cfg.depth` 严格约束。

---

## 3. Step 2B 详细规格：两级递进分段外积（`BD-FMOPA` → `RS-FMOPA`）

### 3.1 静态循环行/列偏移（Cyclic Offset Mapping）
- 为每个活跃上下文槽位 `idx ∈ [0..C-1]` 分配静态循环偏移表：
  ```cpp
  constexpr std::array<uint8_t, 8> Sme2dOffsetTable = {0, 4, 3, 7, 1, 5, 2, 6};
  ```
- 仅在装填/导出端口侧经过轻量 8 路循环桶形移位器：
  - **`2d-bd`（Tier 1）**：行列同步循环偏移 `phys_r = (r + off) & 7`, `phys_c = (c + off) & 7`；
  - **`2d-rs`（Tier 2）**：**仅行循环偏移** `phys_r = (r + off) & 7`, `phys_c = c`（水平 `b` 向量零移位）。
- 即使在 $C=4$ 上下文下，也天然支持 `4+4`、`4+3+1`、`5+3` 零冲突满行配对。

### 3.2 Tier 1：`BD-FMOPA`（`--coeff-sme-array-mode=2d-bd`）
- 保持单条 `a[0..7]` 与单条 `b[0..7]` 广播总线（零新增广播线），每拍广播 2-bit 行/列段标签 `row_seg[0..7]` / `col_seg[0..7]`。
- PE 门控：$\text{en}(r,c) = (\text{row\_seg}[r] \neq 3) \land (\text{row\_seg}[r] == \text{col\_seg}[c])$。
- 合法打包条件：任意两段满足 `(rowMask_i & rowMask_j) == 0 && (colMask_i & colMask_j) == 0`，同 `kind`，且剩余 ZA 切片读端口足够。

### 3.3 Tier 2：`RS-FMOPA`（`--coeff-sme-array-mode=2d-rs`）
- 保持 1 条垂直 `a[0..7]` 总线，水平广播总线扩为 $S$ 条 `b[0..S-1][0..7]`（$S \in \{2, 3\}$），仅在 8 个行入口处放置 $S$:1 `b` 选路 MUX。
- PE 门控：$\text{en}(r,c) = (\text{row\_seg}[r] \neq 3) \land \text{seg\_colMask}[\text{row\_seg}[r]][c]$。
- 合法打包条件：任意两段**仅需满足行互斥** `(rowMask_i & rowMask_j) == 0`，同 `kind`，且剩余 ZA 切片读端口足够。
- 子选项 `--coeff-sme-2d-panel-pack=<0|1>`：首版落地 `panel-pack=0`（零改动现有 3-Tile 布局，全部既有验收契约一次打通）；`panel-pack=1` 保留为后续可选上限消融扩展位。

### 3.4 互补阶段打包调度器（Offer → Pack → Commit）
1. **Phase 1 — 非 2D 事务先行**：`load` / `loadVertical` / `read`（导出）与 `reciprocal`（独立除法器）照常推进。
2. **Phase 2 — Candidate Offer**：各活跃单元将就绪算术候选提交至 `sme2dOffers`（每活跃单元槽位提交当前最高优先级就绪候选）。优先级顺序：`Backward/Forward Scan > Warm Critical RHS > Cold LU > Cold Panel 0 [R|L] > Cold ReverseRhs > Cold Panel 1 U > Background Sparse I5`。
3. **Phase 3 — Complementary-Phase Pack & Commit**：选最高优先级可发射候选作锚点 `c0`，在同 `kind` 候选中按关键组内 Best-Fit（优先最大 `activePes`）最多再选 $S-1$ 个互补段，一次性调用 `smeZa.issue2dBundle(...)` 并提交各段状态推进；精确统计 `pack_stall_row` / `pack_stall_col` / `pack_stall_readport`。

---

## 4. 片上容量计费、配置接口与验收不变量

### 4.1 `reservedBytes()` 新增计费
在 `CfdSmeZa::reservedBytes(...)` 与 [single_line_contract.py](../configs/single_line_contract.py) 中同步计入：
- `2d-base`：`+128 B`（`a/b` 广播锁存器）；
- `2d-bd`：`+256 B`（`a/b` 广播锁存器 + 分段打包调度与偏移表）；
- `2d-rs`：`+256 B + (S - 1) * 128 B`（额外 $S-1$ 条 512-bit 水平 `b` 广播锁存器与行选路控制）。

### 4.2 新增 CLI 与环境变量
- `--coeff-sme-array-mode=1d|2d-base|2d-bd|2d-rs`（默认 `1d`）→ `GEM5_CFD_SME_ARRAY_MODE`（`0..3`）
- `--coeff-sme-2d-segments=<1..4>`（按模式默认 `1` / `1` / `3` / `2`）→ `GEM5_CFD_SME_2D_SEGMENTS`
- `--coeff-sme-2d-panel-pack=<0|1>`（默认 `0`）→ `GEM5_CFD_SME_2D_PANEL_PACK`

### 4.3 遥测与 `check_sme_za.py` 强制不变量
- `CFD_SME_CONFIG` 新增：`array_mode`, `segments_max`, `panel_pack`。
- `CFD_SME_2D_STATS`（仅 `2d-*` 输出）：`bundles_total`, `bundles_seg1..4`, `pe_active_sum`, `pe_capacity`, `pe_issue_capacity`, `row_fill_hist`, `pack_stall_row`, `pack_stall_col`, `pack_stall_readport`。
- 强制校验：算术守恒 `pe_active_sum == sum(fma + mul + add)`、物理上限 `pe_active_sum <= bundles_total * 64 <= sweep_cycles * 64`、段计数与行填充直方图之和等于 `bundles_total`、`2d-base` 无多段发射、200 位外部参考 `mismatches == 0`、病态门控与在途取消恢复全通过。
