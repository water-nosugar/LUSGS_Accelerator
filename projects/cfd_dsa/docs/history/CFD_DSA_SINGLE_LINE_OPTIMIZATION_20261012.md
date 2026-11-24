# 单线 LU-SGS 流水与资源利用率优化（2026-10-12）

研究范围仍为一条网格线，每轮恰好一次前向和一次后向。此轮优化使用 gem5 timing DMA、有限 tile 和共享 5 mul / 5 add / 1 div 计算簇。构建命令为 `scons build/ARM/gem5.opt -j2`。

## 实现与开启方式

新增 `--coeff-line-opt-level=0..4`，各级累积开启；0 保留原行为，便于同一二进制下比较。优化级别要求 `tiled`、共享计算簇和 timing DMA。

| 级别 | 改动 | 消除的等待 | 保留的约束 |
|---|---|---|---|
| 1 | 输入、LU、求解上下文按 cell 前沿准入 | 哈希表遍历导致先占用远端 cell，首个前沿迟迟得不到上下文 | 固定输入槽、上下文和 pending 深度 |
| 2 | 终端记录从 1784B 精简到 320B；数据提交与记录退休分离 | y/Ubar 已完成却要等待大记录回写，阻塞 tile 推进 | 数据提交等待必需数据响应；最终完成仍等待记录、Δq 和状态响应 |
| 3 | warm inverse 的 R 读取、base 计算、y 写回重叠；warm 使用缓存 Ubar | 逐 cell 串行读算写；已缓存 Ubar 仍往返外存 | 最多 `min(4,window)` 个 warm 槽，写回响应前保留缓冲所有权 |
| 4 | cold DInv/Lbar 列乘积提前计算，按原列顺序累加 | 每列乘法结束后才发加法，下一列不能重叠 | 与原列消费者共享全局有界 credit，不增加算术单元、不重排加法 |

`--coeff-rhs-row-parallel=1` 是独立开关：同一前向列下的行更新可以并行；后向先生成独立乘积，再按原 j 顺序减法。60 个固定乘积组预留 4096B，计入统一 SPM 容量账本。没有通过瞬时计算或遗漏操作来提速。

精简记录保留身份、状态、主要生命周期时间和 guest 正确性所需计数。未导出的完整诊断字段由 trace 和全局资源统计提供；不能把展开后为零的可选字段解释成“没有停顿”。descriptor 仍为 160B，最终状态仍为 8B。guest 通过描述符 flag 和对应 stride 选择记录格式，旧格式保持兼容。

关键实现：

- [前沿准入及完成路径](../../../../src/arch/arm/cfd_coeff_preprocess_controller.cc)
- [tile 数据提交、warm 槽、缓存 Ubar](../../../../src/arch/arm/cfd_coeff_preprocess_tiles.inc)
- [cold 列乘积提前计算](../../../../src/arch/arm/cfd_coeff_preprocess_frontier.inc)
- [RHS 独立行并行](../../../../src/arch/arm/cfd_coeff_preprocess_rhs_rows.inc)
- [320B 记录字段](../../../../src/arch/arm/cfd_compact_record_fields.inc)

## 公平比较与验收口径

主对照使用同一最终 gem5 二进制、同一 guest、同一网格与数值检查。固定 mul/add/div 数量为 5/5/1，延迟为 3/4/12，除法 II=4；tile=4、双缓冲、64B tile DMA、DMA outstanding=16、LU/solve 上下文=2/2，SPM 总容量 192KiB。RHS 行并行增加的 4KiB 缓冲单独计账。

“整轮周期”来自 guest 提交到完成通知的 ROI；“计算簇利用率”使用 descriptor 首个 timing 访问至最终状态可见的控制器窗口。两者不混用。发射利用率定义为 `issued/(window_cycles × count / II)`；mul/add 的 II=1。算术全空闲周期是所有 mul/add/div 在途区间并集的补集。DMA 拒绝、bank/队列 stall 事件可一周期多次，不能相加作为总停顿周期。

分阶段结果、最终配置结果和可复现图表由 [分析脚本](../../../../results/cfd_dsa/analysis/single-line-opt-20261012/analyze.py) 从已验收的 JSON 生成，见 [结果表](../../../../results/cfd_dsa/analysis/single-line-opt-20261012/results.md) 和 [机器可读摘要](../../../../results/cfd_dsa/analysis/single-line-opt-20261012/summary.json)。历史审计和旧二进制结果保留，不与这次新对照混合。

## 对气泡和剩余瓶颈的解释

1. **准入和终端记录是最先消除的大气泡。** 原始输入顺序从 7、6、5…开始，优化后为 0、1、2…。17 个 child 的记录数据从 30328B 降到 5440B，降幅 82.1%；记录可与下一 tile 的工作重叠，仍保留全局完成 fence。
2. **warm 存储路径已经重叠。** 17 点 inverse 的缓存命中轮，16 对相邻 cell 发生 R 预取，12 对发生 y 写回重叠，warm 槽峰值为 4。缓存 Ubar 后，warm 后向的 y/Ubar 回读从 3840B 减至 640B，前向 backing 写入从 3880B 减至 680B。cold 首轮仍执行完整写入与回读。
3. **局部列反馈缩短，并不保证整轮单调下降。** 连续可用列的前向 handoff 间隔由约 39 周期降至 27 周期；cold 系数就绪、tile 边界和共享资源竞争仍会产生更长间隔。单独启用 level 4 对 direct 整轮可略退化，因此结合 RHS 行并行和全局 RHS 前沿仲裁测量最终配置。
4. **提高上下文数需要看调度方式。** 在原 frontier-aware 策略下，4/4 上下文冷启动退化；启用现有 rhs-frontier 全局仲裁后，inverse 的 2/2 更合适，direct 复用的 2/4 可以小幅受益。不能把上下文数当作物理计算单元数。
5. **除法吞吐与 LU 内部并行单独评估。** direct cold 的 LU 工作为 170 div、510 mul、510 sub；求解及线运算的工作量更大。LU 仍按原列递推和顺序点积状态机执行，当前先并行 RHS 行更新。LU 同列不同目标元素确有并行空间，但它们与 RHS 争用全局算术簇，尚未实现新的 LU 行并行状态机，不能把理论潜力当成实测收益。
6. **warm inverse 的 divider 空闲是算法结果。** 其系数已缓存，命中轮不需要除法。不能为了提高各单元利用率而增加没有必要的工作。此时优先看整轮时间、乘加利用率、数据等待和输出背压。

除法 II=2/1 和 reciprocal 的实验独立于同计算资源主表。降低 II 改变了硬件峰值吞吐假设，未获得综合面积/频率证据；保留 II=4 为默认可比基线。实验收益见结果表。

## 参数选择与复现

推荐配置保存在 `suites/history/single-line-opt-recommended.json`，包含 cold/reuse × direct/inverse 四个实测配置。该推荐针对 17 点和本次存储系统；长线、不同更新周期需要重新扫描。所有路径仍只有一条前后向递推。

```bash
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/history/single-line-opt-recommended.json --campaign single-line-opt-recommended-new
python3 projects/cfd_dsa/tools/check_single_line_opt.py results/cfd_dsa/campaigns/single-line-opt-recommended-new --suite projects/cfd_dsa/suites/history/single-line-opt-recommended.json --report /tmp/recommended-check.json
```

主分阶段、容量/参数扫描、压力与缓存契约、divider 和调度实验分别在 `single-line-opt-final-stages`、`single-line-opt-tuning`、`single-line-opt-stress`、`single-line-opt-divider`、`single-line-opt-scheduler`、`single-line-opt-selection` suites 中。测试覆盖 N=1/2/17/65、window/产品 credit/输出队列/DMA 并发为 1、单/双 tile 缓冲、serial/split/tree、cache 不适配容量、版本/dirty/模式/尺寸/R 更新契约以及旧 C/D 路径。完整验收状态与源码/二进制哈希见 [证据清单](../../../../results/cfd_dsa/analysis/single-line-opt-20261012/evidence.json)。

新增 trace 验收检查：前沿准入顺序、列累加顺序、数据提交与退休关系、warm 槽峰值与所有权、Ubar 不重复 DMA、所有 timing 请求响应闭合、固定物理预算、操作数量、配对 FP64 位模式。故障注入验证乱序列加法、提前 tile 提交、准入顺序错误、行乘积泄漏会被拒绝；另保留容量、记录/数据响应、提前完成等故障检查。

## 模型边界

本次交付是 gem5 事件与 timing DMA 硬件模型优化。数值缓冲、上下文、tile、产品 credit 和存储容量均有界，但 operand 仍采用当前寄存器/直接消费语义。尚未建立完整 SRAM bank/端口逐操作竞争、RTL 综合面积、频率和真实 CFD 数据集验证。因此本报告给出计算簇与 DMA 的模型内利用率和周期，不能据此宣称实际芯片整体利用率、面积或功耗已经验证。
