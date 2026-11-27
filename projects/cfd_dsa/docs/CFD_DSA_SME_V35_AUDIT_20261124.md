# V35 实现、测试与实验数据复核

2026-11-24 · gem5 `2d51bdb70f` · 单线 5×5 LU-SGS / SME-ZA

**原报告的“12/12 专项通过”有明确反证。** 两组的 guest 都打印了 `LUSGS_STEP2_FAIL`，两份 manifest 却保存为 `passed`。按完整验收标准计，9 个正向用例通过，1 个非法配置如期被拒，2 个正向用例失败。4096 点主实验的输入、周期、流量和按轮输出哈希有原始记录可核对。本次检查没有发现主实验测量值被编造的证据，现有记录也无法判定两处误报的主观原因。[原报告](CFD_DSA_SME_V35_HARDENED_20261124.md) · [原汇总](../../../results/cfd_dsa/analysis/sme-za-v35-hardened-20261123/summary.json)

## 1. 两组失败发生在哪里

| 专项 | 加速器最终输出 | 失败证据 |
| --- | --- | --- |
| GT01R，65 点 | 三轮各比较 650 个值，均为 0 个超差，最大相对误差约 `9.11e-11` | CPU 参考解 1/650 个值超差，最大相对误差 `1.38e-10`。日志随后打印 `valid=0` 和 `LUSGS_STEP2_FAIL`，完整验收器在独立参考值比较处退出。 |
| 病态输入，65 点 | 三轮最终输出均为 0 个超差，55 块用逆、10 块回退 | CPU 参考解 137/650 个值超差。完整验收器首先在中间矩阵 Ū 上失败。独立重算 64 个矩阵的 1,600 个元素，6 个块共 7 个元素超过现行门限。 |

病态样本中，按从零开始的下标，cell 5 的 Ū[3,0] 为 `0.03643506451646374`，220 位十进制参考值舍入后为 `0.03643506451813432`，绝对误差 `1.671e-12`。该项同时超过绝对和相对门限。原方程 `D·Ū−U` 在全部矩阵上的最大绝对残差只有 `3.032e-13`。对有限的最终 y/dq，逐元素绝对误差和相对误差均超过 `1e-10` 才计一次超差，相对误差的分母为 `max(|参考值|, 1e-14)`。guest 另查非有限值和逐行归一化递推残差。[GT01R 原始日志](../../../results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/verify/real-gt01r-s835-n65/simout.txt#L62709) · [病态原始日志](../../../results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/verify/illcond65-guarded-hw/simout.txt#L62731) · [中间矩阵门限](../tools/check_sme_upper.py#L180) · [最终输出门限](../benchmarks/lusgs/step2_dataset.inc#L79)

两组的 gem5 宿主进程均返回 0。归档的两个 case 均缺少 `acceptance.json`，专项汇总的 `checked` 也缺失。项目通用运行器已包含 guest 失败标记、最终 PASS 标记以及完整验收器三道检查。这批归档的简化 manifest 与通用运行器的产物字段不同。专项状态应由每项适用的验收结果导出，保留失败原因。冷轮取消用例则确有通过的重跑，旧的 campaign 汇总保留了第一次失败。[GT01R manifest](../../../results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/verify/real-gt01r-s835-n65/manifest.json) · [专项 campaign 汇总](../../../results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/verify/campaign-summary.json) · [通用运行器](../tools/run_experiment.py#L217)

## 2. 数据和计算的独立证据

- 从三个 `.cfd5` 文件直接读取 D、L、U、R。另写 220 位十进制、带选主元的 Gauss–Jordan 消元，按 `Dᵢ yᵢ = Rᵢ − Lᵢ yᵢ₋₁` 前扫，按 `dqᵢ = yᵢ − Dᵢ⁻¹ Uᵢ dqᵢ₊₁` 后扫。主输入 40,960 个参考值、GT01R 和病态输入各 650 个参考值，舍入为 binary64 后全部逐位吻合。这里比对的是输入文件内嵌的高精度参考值。实际加速器输出与参考值按上述容差验收。[独立算法](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/check_oracles.py) · [逐数据集结果](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/oracles.json)
- 用冻结的 guest 和 gem5 做了 65 点反例。只把第一个 y 参考值加 0.5，实算冷轮哈希维持 `49931eda7a92f34a`，guest 检出 1 个错误。只把第一个 RHS 分量加 0.5 并保留旧参考，哈希变为 `4480628c04cebd54`，检出 88 个错误。两份派生输入都重算了文件校验和。这项对照说明所测正常路径确实依赖输入计算。源码在失败恢复分支里有复制参考数组的操作，成功轮还要求独立的数值记录和完整 sweep 数。[反例结果](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/probes.json) · [guest 数据路径](../benchmarks/lusgs/step2_streaming.inc#L1043)
- GT01R 原矩阵可从 SuiteSparse 获取。公开压缩包和本地压缩包 SHA-256 相同，用起点 835 重新提取的 65 点文件也与实验输入逐字节一致。它取原矩阵上的一条路径，所选行中有 647 个路径外耦合块被省略。4096 点主实验另用同一条合成线，其所有对角块的无穷范数条件数约为 1.15–1.50。GT01R 的 65 个局部对角块条件数均低于 9，整线参考解的绝对值仍可达 `3.25e105`。小残差衡量代入方程后的误差，不能代替整线解对参考值的逐元素比较。[SuiteSparse GT01R 原始说明](https://sparse.tamu.edu/Fluorem/GT01R) · [公开数据重提取校验](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/gt01r-provenance.json) · [路径元数据](../../../results/cfd_dsa/datasets/Fluorem/gt01r-final-v1/gt01r-s835-n65.json) · [合成输入元数据](../../../results/cfd_dsa/analysis/memory-rf-opt-20261024/stationary-holdout-s307-c035-n4096.json)

## 3. 单测通过后仍能漏掉的错误

**RTL 随机测试没有覆盖生产仲裁器的逐拍等价。** 256 组使用固定种子的独立候选向量，RTL 执行组合选择，期望授权由测试文件内另写的 `anchorBefore`、`fillerKey` 和资源判断计算。定向用例实际调用了 gem5 的写回冲突预测函数。随机部分未调用生产版 `commitSme2dBundle()`，也不跨拍演进返回队列。生产仲裁的候选顺序、可用 lane 或提交回调变动，可能绕过这项测试。优先让两端消费同一串生产候选和实际资源状态，逐拍比较授权与提交结果。[随机测试](../tests/arithmetic/test_sme_za.py#L379) · [生产仲裁](../../../src/arch/arm/cfd_coeff_preprocess_sme_2d.inc#L2363) · [RTL 接口](../../../fpga/cfd_lusgs/rtl/sme_za_rs_issue.sv#L10)

**完整验收器接受了四份刻意损坏的日志副本。** 每次只在副本中改动一种证据，用项目的正常调用方式运行 `check_sme_za.py`，四次退出码均为 0。[反例结果](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/probes.json)

| 对日志副本的改动 | 暴露的缺口 |
| --- | --- |
| 将实际重读中 120 条非零 DMA 偏移改成 0，保留请求 ID 和包长 | 检查配对和总字节数，漏查同一 tile 内的重复或缺失地址区间。 |
| 移除 195 条写完成事件和 9 个 commit 的 `last_write` 字段 | 当独立写完成证据完全消失，仅凭 commit 仍会通过。这个 65 点轮转用例只有三块，不会占满六槽。 |
| 将已取消 token 的一次新分配移至 abort begin 后 1 拍 | 当前截止线只约束到 abort visible。停止接收新工作与允许在途响应排空需要不同的检查。 |
| 将三轮输出哈希改为全零，保留其他数字 | 验收器会转存该哈希。逐轮跨配置比较或从留存的输出重新计算，可补上档案完整性检查。 |

对未经修改的 16 组主实验另查了每个重读包的偏移、包长和响应标识。83,328 对请求与响应全部匹配，每个重读块都连续覆盖 1,280 B。对启用 P1 的 8 组、两轮共 65,536 个点，逐点写完成事件均早于或等于所属 tile 的提交拍。本次已核对的原始主实验通过这两项额外检查。[逐组地址与写完成核验](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/tile-checks.json) · [从原始轨迹复算](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/check_tile_traces.py)

另一个覆盖缺口在暖轮取消测试。日志中最后一次右端项（RHS）发射在 586,165 拍，取消在 586,422 拍，间隔 257 拍，RHS 的 `return_wait=0`。报告引用的 425 和 795 是该轮累计发射的 lane 操作数，快照实际只记录全局 `live=1`。此次数据支持取消排空与恢复。验证“取消当拍 RHS 和 Forward 均在途”需要按相位记录当拍未退休的操作。[取消原始日志](../../../results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/verify/recover17-warm-rot1/simout.txt#L22388) · [累计量计算](../tools/check_sme_za.py#L380) · [快照定义](../../../src/arch/arm/cfd_coeff_preprocess_lifecycle.inc#L59) · [Tile 验收逻辑](../tools/check_single_line_storage.py#L121)

## 4. 方案参数和报告算术

4096 点主表的控制器周期按“最后可见完成拍 − 第一个描述符请求拍 + 1”重算。Guest ROI 用 guest 计数器的 finish − start。其中包含提交和等待开销。两种公式的起止事件不同。冷轮指重建系数，两轮都使用 `prepared` 输入预读。对归档的 16 组、共 32 轮分别重算控制器周期、Guest ROI、阶段和、PE 发射占比、每轮重读字节和包数。[按轮重算](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/archive-metrics.json) · [原始日志读取程序](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/read_cycles.py)

| 原报告项目 | 核实结果 |
| --- | --- |
| P1 的流量和 片上存储（SPM）账本 | 每轮重读从 126 块降到 122 块。每块 32 点 × 5 个 FP64 × 8 B = 1,280 B，节省 5,120 B。逐包数由 2,646 降到 2,562。容量 3,407,872 − 1,675,752 − 7,680 − 1,671,168 = 53,272 B。这是模型预留，实际 BRAM/SRAM 端口、布局和面积还需综合。 |
| 暖轮每点间隔 | 对每组暖轮的 4,094 对相邻后扫反馈事件直接作差。T1 从 17 变为 18 拍，T2 从 25 变为 26 拍。报告写的 19/27 均多 1 拍。暖轮前扫的主要间隔也为 18/26。少数点另有等待。 |
| 全部 4096 点运行的最大残差 | 16 组均为冷轮 `3.5122291803909614e-16`、暖轮 `3.1524129296771246e-16`。报告把暖轮值列成整体值。各配置对应冷轮哈希相同，对应暖轮哈希相同。冷、暖两轮哈希彼此不同。 |
| T1 单开 P0 的行冲突 | 在 `hw8+fbr1` 下固定 P1=0 时，计数从 38,330 增至 41,334，差 3,004。报告的 5,482 来自 P0、P1 同开与基线的比较。T2 的 P1 确有流量收益。Panel B 提前装填点数记录的是首行装填事件，单凭该计数无法量化每个关键路径停顿的贡献。 |
| PE 利用率 | 整轮口径为 `(MUL + ADD + FMA 的发射 lane 次数) / (64 × 控制器周期)`，每个 FMA 计一次，DIV 单列。P0 让冷轮计数从 2,027,170 增至 2,047,640，多做 20,470 次 lane 操作。该占比可以在单开 P0 变慢时上升。`hw8+fbr1` 的暖轮实际范围四舍五入为 3.74%–5.41%，物理利用率上限仍待验证。 |

T1 和 T2 的 FMA/MUL/ADD/DIV 参数分别是 5/4/4/12 和 8/6/6/18 拍，DIV 启动间隔均为 4 拍。两套 gem5 系统都使用 2 GHz、相同的四通道 DDR4-2400 模型。现有 FPGA 脚本用 4 ns 目标综合独立选择器、返回槽。候选生成也做了简化。生产代码先从上下文中筛出 ready 项，再取前 8 项。因此 `hw8` 限制了最终仲裁的输入宽度，生成 8 个 ready 候选的硬件和时序成本仍待验证。T1/T2 适合作为延迟敏感性试验。换算板级吞吐要有实际时钟和内存时钟域关系。[时序参数](../configs/run_cfd_dsa.py#L343) · [系统时钟](../configs/run_cfd_dsa.py#L3546) · [候选形成](../../../src/arch/arm/cfd_coeff_preprocess_sme_2d.inc#L61) · [子模块综合脚本](../../../fpga/cfd_lusgs/synth/sme_za_ab.tcl#L18)

3.25 MiB 取自控制器内部容量参数 `GEM5_CFD_LINE_SPM_BYTES`。系统另给 guest 映射了大小为 256 KiB 的 SPM 地址区间。两者在配置中独立设置，当前容量核对只能证明内部账本的分配与预算相符。[控制器容量计算](../configs/run_cfd_dsa.py#L2753) · [guest SPM 地址区间](../configs/run_cfd_dsa.py#L3554)

还有一些计数由 guest 按公式累加。例如 `patha_dinv_custom_instructions_eliminated` 每次直接增加 `12 × total_cells`。这类字段度量按算法估算的省略工作量。要报告真实少退休了多少条指令，还需配对运行的指令统计。上表的控制器周期和 DMA 包数可以单独从事件核查。[guest 计数代码](../benchmarks/lusgs/step2_streaming.inc#L1039)

求逆门控还要和 LU 可解性区分。2D 前端按当前对角主元执行消元。主元非有限或绝对值不超过门限时直接报告 LU 失败。主元指数跨度门限 20 和随后基于 D、D⁻¹ 幅值的条件性估计用于决定能否缓存逆。启用后级条件检查时，代码采用 `e(D)+e(D⁻¹)+7 ≤ 16` 的保守指数判据。病态用例展示了这一路径的 10 次回退。即使局部 D 可逆，只要固定消元顺序需要换行，仍可能被前一级拒绝。GT01R 的 65 块全部通过门控，整线 CPU 参考解仍有一个分量超差。[2D 主元检查](../../../src/arch/arm/cfd_coeff_preprocess_sme_2d.inc#L170) · [求逆门控](../../../src/arch/arm/cfd_coeff_preprocess_sme.inc#L167)

## 5. 固定运行环境后，哪些收益还能复现

原表只测了“窗口不限、反馈同拍”和“最多 8 个候选、反馈加 1 拍”两个组合。对两项约束的合计代价可以作差，分别归因还缺两个对照。本次在同一工作目录、同一冻结二进制和输入上补齐 T2 的四种组合，固定 P0=P1=0。下表列出控制器周期，括号内是相对首行的变化量。[本次仿真原始记录](../../../results/cfd_dsa/campaigns/sme-za-v35-audit-20261124/fresh) · [新仿真的逐轮重算](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/fresh-metrics.json)

| 候选宽度 | 点间反馈延迟 | 冷轮控制器周期 | 暖轮控制器周期 |
| --- | --- | ---: | ---: |
| 不限 | 同拍 | 243,971 | 205,293 |
| 8 | 同拍 | 243,966（−5） | 205,293（0） |
| 不限 | 加 1 拍 | 248,447（+4,476） | 213,727（+8,434） |
| 8 | 加 1 拍 | 248,500（+4,529） | 213,647（+8,354） |

在这条单线、这组 T2 参数下，单加反馈寄存使暖轮增加 8,434 拍。保留反馈寄存再把候选数限为 8，冷轮只增 53 拍，暖轮减 80 拍。单看窗口的代价远小于反馈延迟。用 guest ROI 得到的变化量不同，例如单开窗口的暖轮控制器周期持平，guest ROI 却少 752 拍。归因时需标明所用的计时区间。[分别只开反馈与只限窗口的完整计时](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/fresh-metrics.json)

原 16 组中，14 组从旧路径下各自的实验目录启动，另两组 T2、窗口不限、P0=1 的补跑从当前项目根目录启动。guest 可执行文件名和输入文件名的绝对路径各增长了 11 个字符。同为 T2、P0=P1=0 时，冷轮第一份描述符的三个包从 48/64/48 B 变成 32/64/64 B，总量仍是 160 B。这里能观察到启动环境和分包均有差异，每项因素各自贡献多少还未辨明。[旧基线描述符](../../../results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/factorial4096/v35-t2-unlim-fbr0-p0_0-p1_0/simout.txt#L130) · [新基线描述符](../../../results/cfd_dsa/campaigns/sme-za-v35-audit-20261124/fresh/t2-w0-fb0-p00/simout.txt#L132)

在同一当前路径下重跑 T2、窗口不限、反馈同拍的四个 P0/P1 组合，得到了以下冷轮结果。P0 表示 Panel B 提前装填，P1 表示 6 槽前向 tile 轮转。[四组冷暖轮的周期与数值](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/fresh-metrics.json)

| P0 | P1 | 归档控制器周期 | 统一路径控制器周期 | 归档 guest ROI | 统一路径 guest ROI |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 243,256 | 243,971 | 247,464 | 247,558 |
| 0 | 1 | 242,223 | 243,150 | 245,722 | 246,738 |
| 1 | 0 | 234,791 | 234,791 | 238,378 | 238,378 |
| 1 | 1 | 229,588 | 229,588 | 233,176 | 233,176 |

两组补跑的冷轮控制器和 guest ROI 都能原样复现。统一基线后，两项同开的控制器降幅由 5.62% 修正为 **5.90%**（14,383 拍），guest ROI 降幅由 5.77% 修正为 **5.81%**（14,382 拍）。原表另有一处小的舍入误差，旧组单开 P1 的 1,033 / 243,256 = 0.42466%，保留两位应为 0.42%。另一个原有 T2 组 `hw8+fbr1` 的两项同开结果也有原始记录支持，冷轮控制器从 247,072 降到 233,882 拍（−5.34%）。这些百分比对应各自的输入、时序和基线。[原表逐轮数据](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/archive-metrics.json) · [统一路径逐轮数据](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/fresh-metrics.json)

## 6. 复核范围与修正顺序

- 重新执行全套 Python 单测，548 项全部通过，含 Verilator 用例。对全部 28 个归档用例重做状态判断。25 个成功的正向用例均有旧 `acceptance.json`，新的完整验收输出与旧文件解析后的内容逐项一致。非法配置按预期报错单独计入专项通过数。另从冻结二进制启动了 7 组 4096 点仿真、两组有争议的 65 点仿真及 1 组非法配置。用于探测校验缺口的派生输入与日志另存于审计目录。[单测输出](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/unit-tests.log) · [逐用例验收索引](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/review-summary.json) · [重放入口](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/replay_case.py)
- 现有冻结 gem5 与当前 ARM 构建 SHA-256 相同，均为 `e14b0d0b362f1e112462e4b0b94e7b6e82f7cfbf1211c42c501f844a45bece2d`。归档主实验的简化 manifest 记录输入哈希和命令，缺少逐次运行的宿主二进制哈希及源码快照。28 份仿真日志的编译时间戳一致，当前也能核实冻结文件、guest、输入和可复现实验结果。追溯当时每一次启动所用宿主文件的确切字节，还需要运行时指纹。[主实验 manifest 样例](../../../results/cfd_dsa/campaigns/sme-za-v35-hardened-20261123/factorial4096/v35-t2-unlim-fbr0-p0_0-p1_0/manifest.json) · [校验索引与指纹](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/review-summary.json) · [核对脚本](../../../results/cfd_dsa/analysis/sme-za-v35-audit-20261124/verify_review.py)

优先修正专项汇总的通过判定，让缺失验收、guest 失败和 CPU 参考值不合格都能阻止发布“通过”。随后分别定位两组 CPU 数值超差和病态 Ū 的误差来源，用条件数、逐元素解误差和原方程残差共同决定各门限的适用范围。下一轮测试最值得补的是生产仲裁的逐拍对照、DMA 地址区间与写完成事件的强制核验，以及取消开始当拍按相位的在途计数。评价硬件吞吐时，再补长真实线、未预读输入和包含候选生成与存储端口的整体时序结果。
