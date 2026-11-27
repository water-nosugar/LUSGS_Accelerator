# V36 验证报告：V35 独立审计问题全量闭环修复与 32 组统一路径复测（2026-11-24）

本报告完整归档针对独立审计报告 [CFD_DSA_SME_V35_AUDIT_20261124.md](CFD_DSA_SME_V35_AUDIT_20261124.md) 所揭示全部问题的根因修复、验收器加固、生产级 C++↔Verilator 状态锁步测试、**12 组专项验证（`verify/`）** 以及 **20 组统一路径 4096 点全因子与单约束拆解实验（`factorial4096/`）** 结果。历史 V35 归档目录（`results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/`）与审计目录（`results/cfd_dsa/analysis/sme-za-v35-audit-20261124/`）保持原样未改动，以确保全部历史审计证据链可复查。

* **统一运行工作目录（`cwd`）**：`/home/zyy/UbuntuData/gem5`（全部 31 组正向实验 `config.json` 严格校验一致）
* **仿真器二进制**：`build/cfd_dsa/sme-za/v36-verified/gem5.opt`（与 `build/ARM/gem5.opt` 逐字节一致，SHA-256：`2e0cb2a3a98ed24982ca216c22bf368c336e5ce75c826e2bb9e6b36b5dbafeb4`）
* **客户机基准二进制**：`build/cfd_dsa/sme-za/v36-verified/test_cfd_lusgs_patha_step2_arm`（编译参数含 `-O3 -ffp-contract=off -fno-tree-vectorize`，SHA-256：`ae5e4c8eef4e8c70910cfb68c708e54a1dfdb1a03a1c9628614a7bef80d43218`）
* **实验归档目录**：`results/cfd_dsa/campaigns/sme-za-v36-verified-20261124/`（`verify/` 12 组 + `factorial4096/` 20 组）
* **严格运行器与独立审计脚本**：[run_v36_campaign.py](../../../results/cfd_dsa/analysis/sme-za-v36-verified-20261124/run_v36_campaign.py)、[audit_v36_results.py](../../../results/cfd_dsa/analysis/sme-za-v36-verified-20261124/audit_v36_results.py)
* **结构化验证汇总**：[summary.json](../../../results/cfd_dsa/analysis/sme-za-v36-verified-20261124/summary.json)

---

## 1. 审计问题逐项根因修复清单

| 审计问题编号 | 审计揭示的根因与尾巴 | V36 根因修复方案与代码位置 | 闭环验证结果 |
| :--- | :--- | :--- | :--- |
| **问题 1A**：`real-gt01r-s835-n65` 失败（CPU `mismatches=3`） | 旧 `v9-guest` 编译未禁浮点收缩与自动向量化（而 `cpu-study` 早已注释需禁 FMA 收缩），导致客户机 CPU `raw_reference_solve` 出现 `1.04e-10 > 1e-10` 舍入偏差，且被宽松汇总脚本误标为 `passed`。 | 在 [build_benchmarks.py](../../../projects/cfd_dsa/tools/build_benchmarks.py#L84-L86) 对非 `cpu-study` C 基准统一启用 `-ffp-contract=off -fno-tree-vectorize` 重新编译客户机二进制，**不放宽 `1e-10` 门限**。 | 加速器（`max_rel=9.11e-11`）与 CPU（`max_rel=8.58e-11`）在严格 `tol=1e-10` 下均达成 **`strict_1e10_exceeded=0, mismatches=0`**，`LUSGS_STEP2_STREAMING_PASS` 与 `check_sme_za.py` 双通过。 |
| **问题 1B**：`illcond65-guarded-hw` 客户机与 `check_sme_za.py` 双重失败 | 10 个病态块 $\kappa_\infty(D_c)\approx 6.06\times 10^8$，$\kappa_\infty\varepsilon_{\text{mach}}\approx 6.73\times 10^{-8}\gg 10^{-10}$，固定绝对/相对前向阈值 `1e-10`（解）与 `1e-12`（$\bar{U}$）违背数值线性代数前向误差界。 | 在客户机（[step2_dataset.inc](../../../projects/cfd_dsa/benchmarks/lusgs/step2_dataset.inc#L155-L238)、[step2_streaming.inc](../../../projects/cfd_dsa/benchmarks/lusgs/step2_streaming.inc#L1077-L1113)、[test_cfd_lusgs_patha_step2.c](../../../projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c#L6780-L6832)）与验收器（[check_sme_upper.py](../../../projects/cfd_dsa/tools/check_sme_upper.py#L181-L216)、[check_single_line_dataset.py](../../../projects/cfd_dsa/tools/check_single_line_dataset.py#L14-L103)）同步实施**条件数缩放前向门限 + 机器精度归一化后向残差联合门控**：前向容差 $\max(\text{base\_tol}, 64\cdot 2^{-53}\kappa_\infty)$，同时强制归一化后向残差 $\le 10^{-12}$（解递推）与 $\le 64\cdot 2^{-53}$（$\bar{U}$），并透明输出 `strict_1e10_exceeded` / `strict_1e12_exceeded` 绝不掩盖。 | 良态算例（4096 点与 `gt01r`）门限保持严格 `1e-10` / `1e-12` 不变；`illcond65-guarded-hw` 加速器对真解 `max_abs=4.90e-11`（`strict_1e10_exceeded=0`），CPU `strict_1e10_exceeded=139`、递推残差 `3.39e-16`、$\bar{U}$ 归一化残差 `7.19e-17`（`strict_1e12_exceeded=7`），客户机与验收器全量通过。 |
| **问题 2**：实验汇总脚本仅凭 `returncode == 0` 误判 `passed`，且 2 组实验 `cwd` 路径漂移 | 此前活动脚本未检查客户机 `_PASS`/`_FAIL`、`CFD_STUDY_ACCEL_REFERENCE valid=1`、`stats.txt` 非空及 `check_sme_za.py == 0`，且补跑时混用了 `/home/zyy/UbuntuData/gem5` 与 `/home/zyy/gem5`。 | 新建 [run_v36_campaign.py](../../../results/cfd_dsa/analysis/sme-za-v36-verified-20261124/run_v36_campaign.py) 强制要求 `returncode==0`、`stats.txt > 0`、显式 `LUSGS_*_PASS`、零 `*_FAIL`、`valid=1`、`check_sme_za.py == 0` 落盘 `acceptance.json`，统一从 `/home/zyy/UbuntuData/gem5` 启动并固化 `gem5_sha256` 与 `guest_sha256`；负例单独标记为 `expected_rejection`。 | 32 组实验（11 正向专项 + 1 预期拒绝 + 20 组 4096 点）全量零失败通过，31 组正向实验 `cwd` 与二进制哈希 100% 统一。 |
| **问题 3**：4 类日志篡改探针绕过 `check_sme_za.py` | （1）Tile 重读仅累加总字节未验连续偏移；（2）轮转写栅栏未强制要求每点外部写完成证据；（3）取消轮未校验 `begin` 后禁止新发工作及非取消轮时间戳单调性；（4）`result_hash` 仅信客户机自报字符串。 | （1）[check_single_line_storage.py](../../../projects/cfd_dsa/tools/check_single_line_storage.py#L119-L136) `verify_spans` 强制校验 `[0, expected)` 零空洞连续覆盖；（2）同文件结合 `CFD_CACHE_DMA` 与 `coeff_preprocess_trace.csv` 的 `DRAIN_DQSTAR_VECTOR_DONE` 逐点校验写完成早于 `forward_commit`；（3）校验 `CFD_LINE_ABORT begin` 后零新发 Tile 工作且 `forward_allocate <= CFD_LOCAL_READY <= forward_commit`；（4）控制器（[cfd_coeff_preprocess_controller.cc](../../../src/arch/arm/cfd_coeff_preprocess_controller.cc#L4025)、[cfd_coeff_preprocess_tiles.inc](../../../src/arch/arm/cfd_coeff_preprocess_tiles.inc#L305-L345)）与客户机（[step2_streaming.inc](../../../projects/cfd_dsa/benchmarks/lusgs/step2_streaming.inc#L1088)）独立计算顺序无关 64-bit SplitMix64 `output_digest`，由 [check_single_line_c.py](../../../projects/cfd_dsa/tools/check_single_line_c.py#L156-L176) 强制交叉比对并拦截退化零哈希。 | [test_tile_prefetch_lifetimes.py](../tests/memory/test_tile_prefetch_lifetimes.py#L96-L140) 永久固化单测；在 4096 点与恢复实测产物上执行全部 **5 类对抗篡改探针**（含非取消时间戳倒置），**5 / 5 均以精确语义异常被拦截（`rc=1`）**。 |
| **问题 4**：Verilator 锁步测试复制测试专用仲裁逻辑，未跑多拍状态与返回槽 RTL | 旧 `test_sme_za.py` 在测试文件内重写了一份简化选择器，且仅做单拍静态比对。 | 将生产 2D 捆绑仲裁逻辑提取至唯一公共头文件 [cfd_sme_2d_arbiter.hh](../../../src/arch/arm/cfd_sme_2d_arbiter.hh#L30)，由控制器 [cfd_coeff_preprocess_sme_2d.inc](../../../src/arch/arm/cfd_coeff_preprocess_sme_2d.inc#L2379) 与 [test_sme_za.py](../tests/arithmetic/test_sme_za.py#L302-L535) 共同调用；新增 `4 × 256 = 1,024` 拍真实 `CfdSmeZa` 多周期发射/端口共享/满队列背压锁步比对及 `sme_za_return_slot.sv` Verilator 协同仿真。 | 全量单元测试增至 **551 / 551 全部通过**，消除测试与生产仲裁双份代码漂移风险。 |
| **问题 5**：`recover17-warm-rot1` 末次取消时 RHS 已排空（`live=1`），无分阶段在途计数 | `CFD_SME_ABORT` 仅打印总 `live` 槽数，无法区分取消瞬间 `rhs` 与 `forward` 是否同时在途；旧 `late-polls=124` 落在暖轮 RHS 结束后 257 拍。 | 在 [cfd_sme_za.hh](../../../src/arch/arm/cfd_sme_za.hh#L585-L606) 与 [cfd_coeff_preprocess_lifecycle.inc](../../../src/arch/arm/cfd_coeff_preprocess_lifecycle.inc#L60-L72) 为 `CFD_SME_ABORT` 增加分阶段在途段数/通道数（`live_lu`, `live_rhs`, `live_forward`, `live_backward`, `live_reverse_rhs`）；在 [check_sme_za.py](../../../projects/cfd_dsa/tools/check_sme_za.py#L380-L385) 与 `#L577-L580` 强制要求末次取消同时满足 `live_rhs >= 1` 且 `live_forward >= 1`。 | `recover17-fwd-rot1` 命中 `live=7 (live_lu=2, live_rhs=5, live_forward=1)`；`recover17-warm-rot1`（`late-polls=48`）精准命中 `live=5 (live_rhs=5, live_forward=5)`，冷暖均真实覆盖并发在途冲刷。 |

---

## 2. 单元测试、对抗探针与 12 组专项验证（`verify/`）

### 2.1 全量单元测试与 5 类对抗性篡改探针
* **单元测试**：执行 `python3 -B -m unittest discover -s projects/cfd_dsa/tests -t projects/cfd_dsa/tests`，**551 / 551 全部通过**（`memory` 子集含新增时间戳倒置单测 137 / 137 通过，`numerics` 子集 32 / 32 通过）。
* **实测产物对抗篡改探针**（由 [audit_v36_results.py](../../../results/cfd_dsa/analysis/sme-za-v36-verified-20261124/audit_v36_results.py#L272-L355) 自动在 `v36-t1-hw8-fbr1-p0_1-p1_1` 与 `recover17-warm-rot1` 副本上执行并校验语义报错）：
  1. `zero-reload-offsets`：`rc=1`，`AssertionError: non-contiguous or duplicate reload DMA offset 0 (expected 16)`；
  2. `missing-write-evidence`：`rc=1`，`AssertionError: forward_commit at first=0 lacks independent write completion evidence for cells [0, 1, 2, 3]`；
  3. `late-allocation`（取消轮 `begin` 后分配）：`rc=1`，`AssertionError: new tile work forward_allocate initiated at 297954 after abort begin 297953`；
  4. `late-allocation-timestamp-inversion`（非取消轮分配时间戳晚于就绪/提交）：`rc=1`，`AssertionError: CFD_LOCAL_READY timestamp preceded forward_allocate`；
  5. `zero-output-hash`：`rc=1`，`ValueError: invalid or degenerate result_hash: '0000000000000000'`（单独篡改客户机哈希或摘要不一致则触发 `output_digest disagrees`）。

### 2.2 12 组专项验证实测表

全部 **12 / 12 严格通过**（11 组正向 `passed` 均生成 `acceptance.json` 且客户机显式打印 `_PASS`，1 组负例 `expected_rejection`）：

| 验证用例 | 验证目标 | 控制器周期（各轮） | Guest ROI 周期 | 数值误差 / 归一化残差 | 关键验证证据 |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `early1-rot0-n65` | `early_backward=1`, `P1=0` | `[3870, 2750, 2750]` | `[6942, 3412, 3206]` | `max_abs=1.11e-16`, `res=2.50e-16` | `CFD_EARLY_BACKWARD` 正常触发，`strict_1e10=0` |
| `early0-rot0-n65` | `early_backward=0`, `P1=0` | `[3978, 2858, 2858]` | `[7050, 3520, 3314]` | `max_abs=1.11e-16`, `res=2.50e-16` | 无提前后扫事件，冷/暖均精确慢 +108 拍 |
| `early1-rot1-n65` | `early_backward=1`, `P1=1` | `[3872, 2750, 2750]` | `[6944, 3412, 3206]` | `max_abs=1.11e-16`, `res=2.50e-16` | 65 点（3 个 Tile）轮转生命周期与写栅栏全闭环 |
| `early0-rot1-n65` | `early_backward=0`, `P1=1` | `[3980, 2858, 2858]` | `[7052, 3520, 3314]` | `max_abs=1.11e-16`, `res=2.50e-16` | 轮转与关闭提前后扫正交兼容 |
| `early0-rot1-n64-exact-mult` | 整倍数尾块（64 点 = 2×32） | `[4024, 2803, 2803]` | `legacy` 模式 | `max_abs=2.78e-16`, `res=2.74e-16` | 无残余尾块边界闭合，`output_digest` 一致 |
| `reject-chain-steps-2d` | 2D 模式拒绝 `chain_steps` | —（负例） | — | — | `rc=1`，显式抛 `ValueError` 拦截（`expected_rejection`） |
| `allow-chain-steps-1d` | 1D 模式保留 `chain_steps` | `[4784, 2749, 2749]` | `[7856, 3410, 3206]` | `max_abs=1.11e-16`, `res=2.50e-16` | 1D 向后兼容无回归 |
| `recover17-fwd-rot1` | 冷轮并发在途取消恢复 | `[1427, 1422, 1422]` | 恢复合同模式 | `max_abs=1.67e-16`, `res=3.03e-16` | 末次取消 `cycle=587755` 命中 `live=7 (lu=2, rhs=5, fwd=1)` |
| `recover17-warm-rot1` | 暖轮并发在途取消恢复 | `[1427, 1422, 1422]` | 恢复合同模式 | `max_abs=1.67e-16`, `res=3.03e-16` | 末次取消 `cycle=586540` 命中 `live=5 (rhs=5, fwd=5)` |
| `real-gt01r-s835-n65` | 真实求解器导出线（量级 ~$10^{104}$） | `[3871, 2750, 2750]` | `[7026, 3520, 3354]` | `max_rel=9.11e-11`, `res=3.19e-16` | $\kappa_{\max}=8.93$，严格 `tol=1e-10` 下加速器与 CPU 均 **`strict_1e10=0, mismatches=0`** |
| `illcond65-guarded-hw` | 病态 65 点（10 个 $\kappa_\infty\approx 6.06\times 10^8$） | `[3843, 2750, 2750]` | `[7624, 3434, 3246]` | 加速器 `max_abs=4.90e-11`, `res=3.39e-16` | `inv=55, fallback=10`；加速器 `strict_1e10=0`，CPU `strict_1e10=139`；$\bar{U}$ `norm_res=7.19e-17`（`strict_1e12=7`），全通过 |
| `cache-boundary-spill-n65` | 紧 SPM 容量（`565,248 B`）关缓存 | `[3838, 3808, 3806]` | `[6880, 4320, 4254]` | `max_abs=1.11e-16`, `res=2.50e-16` | `cache_bytes=0, inv=0`，安全回退直接求解 |

---

## 3. 20 组统一路径 4096 点实验（16 组全因子 + 4 组单约束拆解）

### 3.1 数值、摘要与存储不变量核查
全部 **20 / 20 组（40 个完整单线 sweep）** 均由 [audit_v36_results.py](../../../results/cfd_dsa/analysis/sme-za-v36-verified-20261124/audit_v36_results.py) 独立从原始事件流重建并与 `acceptance.json` 100% 对齐：
* **冷/暖独立数值与双端摘要一致性**（分列报告，不混同）：
  * **冷轮（全部 20 组一致）**：`result_hash = 5753cf067391c114`，控制器与客户机 `output_digest = cc9cccf0aa5164c5`，`max_abs = 1.6653345369377348e-16`，**`residual_max = 3.5122291803909614e-16`**，`mismatches = 0`。
  * **暖轮（全部 20 组一致）**：`result_hash = 1d2cf81ee1549650`，控制器与客户机 `output_digest = 65d25373ef8e888e`，`max_abs = 1.6653345369377348e-16`，**`residual_max = 3.1524129296771246e-16`**，`mismatches = 0`。
* **Tile DMA 连续性与写完成栅栏**：全部 20 组每个重读 Tile 均精确覆盖 `[0, 1280)` B 无重叠无空洞；全部 8 组 `P1=1`（`p1_1`）配置每组均完成 **`8,192 / 8,192` 点** 外部写完成栅栏先于轮转 `forward_commit` 的逐点核查。
* **片上存储双域边界说明**：控制器内部流式 SRAM 预算 `coeff_line_spm_bytes = 3,407,872 B`（3.25 MiB），扣除预留 `1,675,752 B`（含 SME `47,872 B`）、6 槽常驻 Tile `7,680 B`、系数缓存 `1,671,168 B`（`4096 × 408 B`）后剩余 **`53,272 B`（52.02 KiB）**。客户机代码中的 `SPM_SIZE = 262,144 B`（`0x70000000` 处 256 KiB）是早期标量/PathA 自定义指令的独立 MMIO 暂存区，不参与本流式控制器内部 3.25 MiB SRAM 容量核算。

### 3.2 单约束独立拆解表（`P0=0, P1=0`：分离 `w8` 与 `fbr1` 各自代价）

| 时序与约束配置 | 冷轮控制器周期 | 冷轮相对理想基线 | 暖轮控制器周期 | 暖轮相对理想基线 | 暖轮前向点间间隔分布 | 暖轮后向点间间隔分布 |
| :--- | ---: | :---: | ---: | :---: | :---: | :---: |
| `T1` `unlim + fbr0`（理想对照） | 199,762 | 基线 | 139,767 | 基线 | `17`: 4094, `18`: 1 | **`17`: 4094** |
| `T1` `w8 + fbr0`（仅 8 候选窗口） | 200,342 | +580（+0.29%） | 140,032 | +265（+0.19%） | `17`: 4094, `18`: 1 | **`17`: 4094** |
| `T1` `unlim + fbr1`（仅反馈寄存 1 拍） | 204,417 | +4,655（+2.33%） | 147,954 | **+8,187（+5.86%）** | **`18`: 4095** | **`18`: 4094** |
| `T1` `hw8 + fbr1`（双约束同时启用） | 203,528 | +3,766（+1.89%） | 147,954 | **+8,187（+5.86%）** | **`18`: 4095** | **`18`: 4094** |
| `T2` `unlim + fbr0`（理想对照） | 243,500 | 基线 | 205,293 | 基线 | `25`: 4094, `26`: 1 | **`25`: 4094** |
| `T2` `w8 + fbr0`（仅 8 候选窗口） | 243,207 | −293（−0.12%） | 205,293 | +0（0.00%） | `25`: 4094, `26`: 1 | **`25`: 4094** |
| `T2` `unlim + fbr1`（仅反馈寄存 1 拍） | 247,579 | +4,079（+1.68%） | 213,852 | **+8,559（+4.17%）** | `26`: 4093, 边界×2 | **`26`: 4094** |
| `T2` `hw8 + fbr1`（双约束同时启用） | 248,195 | +4,695（+1.93%） | 213,735 | **+8,442（+4.11%）** | `26`: 4093, 边界×2 | **`26`: 4094** |

> [!IMPORTANT]
> **单约束归因修正说明**：
> 1. 暖轮相邻点后向递推间隔在 `fbr0 → fbr1` 下精确为 **`T1`：`17 → 18` 拍/点**、**`T2`：`25 → 26` 拍/点**（修正原 V35 报告误写的 19 / 27 拍/点），两遍扫描合计 $2 \times 4095 \approx 8,190$ 拍理论增量与实测 `+8,187` 拍（`T1` 及开启 `P1=1` 消除 2 次前向边界停顿后的 `T2`：`205,293 → 213,480`）严格吻合。
> 2. 硬件现实化开销几乎全部来自 **点间反馈寄存一拍（`fbr1`）**（暖轮占 100%，`T2` 冷轮占 86.9%，`T1` 冷轮单独 `fbr1` 为 `+4,655` 拍而单独 `w8` 仅 `+580` 拍 / `+0.29%`）。

### 3.3 16 组 `2×2×2×2` P0/P1 全因子对照表

| 实验分组 | P0 / P1 | 冷轮控制器周期 | 冷轮 Guest ROI | 相对组内基线（控制器 / ROI） | 暖轮控制器周期 | 暖轮 Guest ROI | 冷轮活跃拍 / `FMOPA` 填充 | 冷轮整轮 PE 利用率 | 平均段数 | 行冲突停顿 `pack_stall_row` | 每轮重读块数 / 字节 | Panel B 提前点数 |
| :--- | :---: | ---: | ---: | :---: | ---: | ---: | :---: | :---: | :---: | ---: | :---: | ---: |
| **`T1` `hw8+fbr1`** | `0 / 0` | 203,528 | 206,856 | 基线 | 147,954 | 151,828 | 22.40% / 24.64% | 15.56% | 1.68 | 38,426 | 126 / 161,280 B | 0 |
| **`T1` `hw8+fbr1`** | `0 / 1` | 200,479 | 203,808 | **−1.50% / −1.47%** | 147,954 | 151,130 | 22.12% / 24.39% | 15.80% | 1.66 | 38,786 | **122 / 156,160 B** | 0 |
| **`T1` `hw8+fbr1`** | `1 / 0` | 203,024 | 206,352 | −0.25% / −0.24% | 147,954 | 151,802 | 23.00% / 24.66% | 15.76% | 1.71 | 41,452（+3,026） | 126 / 161,280 B | 3,518 |
| **`T1` `hw8+fbr1`** | `1 / 1` | **200,386** | **203,714** | **−1.54% / −1.52%** | 147,954 | 151,166 | 22.43% / 24.23% | **15.97%** | 1.67 | 44,074（+5,648） | **122 / 156,160 B** | 3,994 |
| `T1` `unlim+fbr0` | `0 / 0` | 199,762 | 203,090 | 基线 | 139,767 | 142,942 | 22.56% / 24.65% | 15.86% | 1.69 | 37,074 | 126 / 161,280 B | 0 |
| `T1` `unlim+fbr0` | `0 / 1` | 196,492 | 199,820 | **−1.64% / −1.61%** | 139,767 | 142,942 | 22.26% / 24.40% | 16.12% | 1.67 | 38,204 | **122 / 156,160 B** | 0 |
| `T1` `unlim+fbr0` | `1 / 0` | 199,508 | 202,836 | −0.13% / −0.13% | 139,767 | 142,978 | 23.21% / 24.74% | 16.04% | 1.72 | 40,484（+3,410） | 126 / 161,280 B | 3,559 |
| `T1` `unlim+fbr0` | `1 / 1` | **196,358** | **199,686** | **−1.70% / −1.68%** | 139,767 | 142,900 | 22.54% / 24.20% | **16.29%** | 1.67 | 43,083（+6,009） | **122 / 156,160 B** | 3,983 |
| **`T2` `hw8+fbr1`** | `0 / 0` | 248,195 | 251,524 | 基线 | 213,735 | 216,910 | 20.74% / 23.55% | 12.76% | 1.56 | 42,043 | 126 / 161,280 B | 0 |
| **`T2` `hw8+fbr1`** | `0 / 1` | 246,827 | 250,156 | −0.55% / −0.54% | **213,480** | 217,320 | 20.75% / 23.53% | 12.83% | 1.56 | 42,369 | **122 / 156,160 B** | 0 |
| **`T2` `hw8+fbr1`** | `1 / 0` | 238,193 | 241,522 | **−4.03% / −3.98%** | 213,742 | **216,836** | 22.27% / 24.22% | 13.43% | 1.65 | 50,796（+8,753） | 126 / 161,280 B | 3,788 |
| **`T2` `hw8+fbr1`** | `1 / 1` | **233,967** | **237,296** | **−5.73% / −5.66%** | **213,480** | 217,424 | 22.21% / 24.09% | **13.67%** | 1.65 | 53,343（+11,300） | **122 / 156,160 B** | 4,043 |
| `T2` `unlim+fbr0` | `0 / 0` | 243,500 | 246,828 | 基线 | 205,293 | 208,466 | 20.44% / 23.49% | 13.01% | 1.53 | 42,183 | 126 / 161,280 B | 0 |
| `T2` `unlim+fbr0` | `0 / 1` | 242,570 | 245,898 | −0.38% / −0.38% | 205,293 | 209,254 | 20.53% / 23.53% | 13.06% | 1.54 | 42,163 | **122 / 156,160 B** | 0 |
| `T2` `unlim+fbr0` | `1 / 0` | 232,906 | 236,234 | **−4.35% / −4.29%** | 205,293 | 208,466 | 21.98% / 24.28% | 13.74% | 1.63 | 50,649（+8,466） | 126 / 161,280 B | 3,795 |
| `T2` `unlim+fbr0` | `1 / 1` | **229,958** | **233,286** | **−5.56% / −5.49%** | 205,293 | 208,468 | 21.89% / 24.09% | **13.91%** | 1.63 | 53,089（+10,906） | **122 / 156,160 B** | 4,049 |

---

## 4. 关键定量结论与明确证据边界

1. **`T1` 与 `T2` 的优化归因清晰稳健**：
   * **`T1`（短延迟 2 GHz ASIC，`hw8+fbr1`）**：单独启用 **P1（6 槽轮转）** 冷轮降低 **`−3,049` 拍（−1.50%）**，且每轮稳定减少 **4 块 Tile（`5,120 B`、84 个 DMA 包）** 重读；单独启用 **P0** 仅降 `−504` 拍（−0.25%），同时使 `pack_stall_row` 增加 **`+3,026`**（`P0+P1` 时较 `0/0` 增加 **`+5,648`**），在 P1 基础上叠加 P0 仅额外减少 `93` 拍（0.05%）。因此 `T1` 仅推荐保留零面积/省流量的 **P1**。
   * **`T2`（长延迟 FPGA，`hw8+fbr1`）**：单独启用 **P0** 冷轮下降 **`−10,002` 拍（−4.03%）**，单独 **P1** 降 `−1,368` 拍（−0.55%），二者叠加 **P0+P1** 消除前向 Tile 边界准入停顿（Panel B 提前装填点数由 `3,788` 增至 `4,043`，且暖轮消除 2 次边界长间隔节省 `255` 拍），冷轮总降幅扩大至 **`−14,228` 拍（−5.73%）**，呈现明确的超加性正协同（额外协同增益 `2,858` 拍）。
   * **单线暖轮天花板**：在 `hw8+fbr1` 下，64-PE 2D 阵列的暖轮整轮有效 PE 利用率严格锁定在 **`5.41%`（`T1`）** 与 **`3.74%–3.75%`（`T2`）**，证明单线串行递推已触及物理反馈延迟墙，后续吞吐提升必须依靠多线波前交错。

2. **明确研究与建模边界（不夸大结论范围）**：
   * **预热缓存口径（`--coeff-study-cache=prepared`）**：4096 点主矩阵在进入计时 ROI 前执行了受控预读以隔离冷启动缺页/DRAM 抖动，反映片上/多级缓存稳态流式吞吐，不等同于冷 DRAM 首轮端到端延迟。
   * **数据集条件数覆盖范围**：4096 点主消融使用的 `stationary-holdout-s307-c035-n4096.cfd5` 为强对角占优合成算例（$\kappa_\infty(D_c)\in[1.15, 1.50]$，全线零求逆回退）；真实求解器导出线（`gt01r-s835-n65`，$\kappa_\infty\le 8.93$，元素量级 ~$10^{104}$）与病态回退线（`illcond-v3-n65`，$\kappa_\infty\approx 6.06\times 10^8$，10 块回退）已在 65 点规模严格闭环验证，更大规模真实网格流式导出仍待后续扩展。
   * **前端候选生成与后端物理实现范围**：`hw8` 精确建模了对齐 `sme_za_rs_issue.sv` 8 槽输入宽度的就绪候选截断与多周期状态仲裁，但候选请求生成前端尚未编写独立 RTL；当前 RTL 综合与 Verilator 协同验证覆盖 `sme_za_rs_issue.sv` 与 `sme_za_return_slot.sv` 两个核心子模块，完整 `6R/3W` ZA 交叉开关与旁路网络的顶层布局布线（P&R）时序仍待硬件阶段实测。
   * **客户机公式计数与事件计数的区分**：客户机 `step2_streaming.inc` 中诸如 `event_requests_allocated`、`terminal_wait_calls_avoided` 等汇总项为 ROI 外部按 `total_cells` 累加的合同统计量；本报告所有周期、PE 利用率、段数、停顿与字节流量指标均 100% 取自控制器硬件事件日志与独立追踪审计，不混用客户机公式计数。
