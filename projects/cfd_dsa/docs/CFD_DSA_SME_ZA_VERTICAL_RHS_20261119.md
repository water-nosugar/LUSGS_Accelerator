# 以 SME 为计算核心的单线 LU-SGS 硬件实验

本轮把 5×5 LU 尾块更新、多个右端项的三角代入和前后扫矩阵向量乘都落实到 SME。标准 Arm 指令程序提供可执行的数值参考，硬件实验在项目已有的控制器驱动 SME/ZA 后端上运行。新增的是 ZA 写入口的行列选路，复用原有的 FP64 外积通道。软件可沿用同一份任务描述符。原单线入口和 SME 的原行布局都保留作对照。[上一轮研究方案](/home/zyy/gem5/projects/cfd_dsa/docs/CFD_DSA_SME_HARDWARE_RESEARCH_20261118.md) · [SME 专用入口](/home/zyy/gem5/projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_single_line_sme.py)

## 标准 Arm SME 上实际执行了哪些计算

普通 Arm SE CPU 使用 512 位流式向量长度执行 AArch64 程序。LU 使用 FP64 `FMOPS` 更新主元后的矩阵，三角代入用同一条指令更新单个向量或五列右端项，前后扫用 `FMOPA` 求各 (5×5) 块的矩阵向量积。各操作读写架构 ZA，掩码限制活动行和列。CPU 负责寻址、主元检查、倒数、缩放和最终向量减法。这个参考实现逐次启动外积，时间用于功能核验。控制器版本独立保有 ZA 状态，其整轮周期在下表单独计量。[外积汇编](/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/sme_rank1.S:15) · [LU、三角代入与矩阵向量乘](/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/sme_reference_kernels.inc) · [Arm SME 的 ZA 与外积定义](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/arm-scalable-matrix-extension-introduction-p2)

| 原始输入 | 一次冷建加一次复用实际提交的 MatrixOP 指令 | 每轮核验的 y 和 Δq 标量 | 两轮误差超限数 |
| --- | ---: | ---: | ---: |
| 65 点 | 3,604 | 650 | 0 |
| 1024 点 | 57,308 | 10,240 | 0 |
| 4096 点验证 | 229,340 | 40,960 | 0 |
| 4096 点留出 | 229,340 | 40,960 | 0 |

对 N 个点，冷轮应执行 4N 次 LU 尾块更新、16(N−1) 次系数面板更新、8N 次基础解更新和 10(N−1) 次前后扫外积。暖轮保留后两项。每组硬件提交计数均与这个独立公式相符。另一个有正负系数的单点程序在 ZA 内完成转置装入、主元缩放及 8 次 `FMOPS`，逐位检查五个结果和掩码外的哨兵。跨实现数值对照使用同一份 CFD5 高精度参考和原有误差门限。控制器开关前后还逐轮比较结果的位哈希。[65 点客体结果](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/arm-sme-warm65.log) · [4096 点指令计数](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/arm-sme-validation4096/stats.txt) · [纵向切片汇编](/home/zyy/gem5/projects/cfd_dsa/tests/fixtures/sme_vertical_rhs_isa.S)

## 硬件如何改动

冷轮沿用多个 RHS 并排的 ZA 布局。暖轮每个点只有一个 RHS，五个解分量存到一条 ZA 行。缓存 LU 仍按行送来，新增入口一次把这一行写入 ZA 的一列。这样，求解第 k 步时，普通 ZA 行读就能取到原 LU 的第 k 列。现有外积通道广播解分量 x[k]，分别对活动方程行执行 x[r] ← fma(−x[k], LU[r,k], x[r])。一个点完成原有 5 次缩放和 20 次有效 FP64 乘加，阵列发射片段由 25 个降为 13 个，其中 8 个是带掩码的外积。前向和后向扫描继续申请同一组物理通道。[三角代入微程序](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_sme.inc:105)

写入口每拍最多接收一个纵向切片，每次消耗一个原有 ZA 宽写口，并同时预约所有目标行 bank。任一行冲突、目标元素有在途结果或缺少写额度时，整条切片重试。接收后的下一拍才能读到新值。每个写入的 FP64 元素均计入搬移量，结果仍经过原有的返回队列和 ZA 写回。阵列共有 10 路，原有 ZA tile 容量、返回槽、DMA、SPM 和 guest 二进制在每对实验中一致。五变量 RTL 窗口复用已有 ZA 数据存储，增加选路与按元素写使能。[ZA 纵向入口与互锁](/home/zyy/gem5/src/arch/arm/cfd_sme_za.hh:197) · [阵列工作量验收](/home/zyy/gem5/projects/cfd_dsa/tools/check_sme_za.py:274)

## 性能与适用范围

周期从描述符搬运计到结果写回可见，比较两种布局时只切换 `--coeff-sme-warm-vertical-rhs=0|1`。12 读／6 写采用 V14 的 ZA 端口预算。另一个配对将读写口各减半，用来检验写入口在端口紧张时的作用。整轮有效利用率按“有效算术通道操作数 ÷（整轮周期 × 10）”计算，FMA 记为一个通道操作。

| ZA 读／写口 | 规模 | 暖轮原行布局 | 暖轮纵向写入 | 说明 |
| --- | --- | ---: | ---: | --- |
| 12／6 | 1 点 | 616 | 618 | 第一轮复用多 2 拍 |
| 12／6 | 65 点 | 1,452 | 1,424 | 两轮复用各省 28 拍 |
| 12／6 | 1024 点 | 16,407 | 16,788 | 增加 381 拍 |
| 12／6 | 4096 点验证 | 64,295 | 64,251 | 节省 44 拍 |
| 12／6 | 4096 点留出 | 64,295 | 64,273 | 节省 22 拍 |
| 6／3 | 1024 点 | 26,575 | 19,073 | 整轮时间按固定时钟缩短 28.23% |
| 6／3 | 4096 点验证 | 105,017 | 74,992 | 节省 30,025 拍，整轮缩短 28.59% |
| 6／3 | 4096 点留出 | 105,017 | 74,992 | 独立输入复现 28.59% 的周期收益 |

在 12／6 的 4096 点验证输入上，暖轮 RHS 的 ZA 行读从 266,381 次降至 110,592 次，写从 122,880 次降至 57,344 次。前向稳态有效利用率从 74.71% 变为 74.60%，后向仍为 90.91%。端口充裕时，减少 ZA 访问的收益并未等量反映到整轮时间。6／3 的 1024 点配对中，整轮有效利用率从 48.13% 提高到 67.06%。单点、两条长线、1024 点的版本重建，以及 17 点的三次取消和恢复也分别通过原有的数值及生命周期检查。原专用后端 17 点为 4,514 周期，结果摘要与冻结基线一致。[全端口验收](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/validation4096-vertical/acceptance.json) · [半端口 1024 点验收](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/independent1024-vertical-6r3w/acceptance.json) · [取消与恢复](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/recover17-warm-back-poll124-vertical/acceptance.json)

6 读／3 写的两份 4096 点输入中，预处理及前向均从 73,870 降到 43,845 周期，后向均为 30,715 周期。暖轮 RHS 读 ZA 从 283,668 降至 110,592 次，写从 122,880 降至 57,344 次，整轮有效利用率从 48.74% 升至 68.26%，前向稳态从 41.67% 升至 70.31%。该配置首轮冷建在验证输入上均为 219,817 周期，在留出输入上均为 219,819 周期。按暖轮周期折算，新设计保留基线约 71.41% 的工作频率时两者耗时打平，更高时才有时间收益。若以同频绝对速度为目标，两份输入中 12 读／6 写的原行布局均比 6 读／3 写的纵向布局快 10,697 周期。[验证输入基线](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/validation4096-row-6r3w-frozen/acceptance.json) · [验证输入纵向写](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/validation4096-vertical-6r3w-frozen/acceptance.json) · [留出输入基线](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/holdout4096-row-6r3w-frozen/acceptance.json) · [留出输入纵向写](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/holdout4096-vertical-6r3w-frozen/acceptance.json)

两份 4096 点输入的半端口原布局在 55,296 个前向稳态周期中有 3,072 拍完全空发射。纵向写把这个窗口缩到 32,768 拍，其中 13,312 拍占满 10 路，18,432 拍占用部分通道，另有 1,024 拍因操作数依赖完全空发射。按这个实测分类，下一项实验可以直接检验减少依赖等待的效果。[前向稳态计数及独立配对](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/summary.json)

另一组 17 点任务在暖轮 RHS 完成 160／425 个有效算术操作、阵列有 4 个结果仍在途时取消，排空后恢复通过。十组配对均核对了二进制、输入、共同参数、算术量和结果哈希。四组标准指令运行的输入指纹与对应控制器配方一致。[暖轮前向取消](/home/zyy/gem5/results/cfd_dsa/campaigns/sme-za-vertical-20261119/recover17-warm-forward-poll64-vertical/acceptance.json) · [交叉核验汇总](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/summary.json)

## RTL 代价与下一项实验

5×5 FP64 ZA 窗口包含 1,600 个数据位，支持行读及掩码写入。Icarus Verilog 对原行写和新增纵向写各跑了 2,304 笔交易，逐项检查完整内容、掩码、仲裁冲突和拒绝后的状态。Yosys 按 UltraScale+ 原语分别综合，两版均得到 1,600 个 FDRE，估算逻辑单元从 828 增至 1,722。两者均包含同一个行读选择器。这个对照给出了单个 ZA 窗口增加纵向选路的逻辑代价。频率、布线、功耗以及完整多端口 ZA 的面积还需实现后测量。[RTL](/home/zyy/gem5/fpga/cfd_lusgs/rtl/sme_za_slice_bank.sv) · [RTL 测试](/home/zyy/gem5/fpga/cfd_lusgs/tb/sme_za_slice_bank_tb.sv) · [仿真记录](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/rtl-simulation.log) · [原行写综合](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/rtl-synth-0.log) · [纵向写综合](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/rtl-synth-1.log)

建议把纵向写作为端口受限配置的候选，全端口配置保留原行布局。上一轮在两条 4096 点线上分别观察到 4,095 次交接中有 2,980 次是本点的基础解晚于前驱结果。下一项硬件对照应使用同一 SME 算术流水线，把最终舍入级的已定值分量经寄存旁路送到前向归约，给返回槽、接收槽和背压计费。用原布局、仅纵向写、仅旁路和两者同时开启做对照，记录首次可用分量、相邻点产出间隔及整轮周期，再比较同频收益是否覆盖综合后的频率和面积变化。[前向交接逐点记录](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-20261109/v14-forward-operands-20261118.json)

## 复现

在仓库根目录，下面的命令构建并运行 65 点标准 SME 数值基准。该 Arm SE 配置单独实例化原生 ZA，避开旧 Path B/C 实验中为读取自定义列结果设置的 MOVA 重定向。控制器入口使用 `lusgs-single-line-sme`，参数、二进制指纹和数值验收分别保存在实验目录中。

```bash
python3 -B projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 sme-line-isa
build/cfd_dsa/sme-za/v29-vertical/gem5.opt -d /tmp/cfd-isa-line65 \
  projects/cfd_dsa/tests/fixtures/run_sme_isa_reference.py \
  build/cfd_dsa/aarch64/test_cfd_lusgs_sme_isa_arm \
  --coeff-input-file results/cfd_dsa/datasets/single-line-study-v2-20261024/stationary-boundary-s101-c035-n65.cfd5 \
  --lusgs-cells 65 --bench-iters 2 --cpu-study-mode pretransform --cpu-study-cache prepared
```

首次半端口长线运行期间，新的指令验证文件进入工作树，实验工具据此拒收该批长线记录。表中半端口长线数据采用源文件冻结后的 `-frozen` 配对。全端口长线的两份源码清单只相差独立运行的 Arm SE 验证脚本，控制器程序、原生二进制、整线 guest、输入和共同参数逐项相符。[基本配方](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/cases.json) · [半端口 1024 点配方](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/port-cases.json) · [半端口长线冻结配方](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/port-cases-frozen.json) · [校验脚本](/home/zyy/gem5/results/cfd_dsa/analysis/sme-za-vertical-20261119/verify.py)
