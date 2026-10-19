# 单线 LU-SGS 流水与瓶颈审计

审计日期：2026-10-12。研究范围：一条网格线，每轮一次前向、一次后向。本文检查当前 `shared + tiled + timing DMA` 实现，不修改模拟器，不引入新的性能仿真。

## 结论

当前瓶颈呈分阶段特征。整轮主要受前沿任务准入、终端记录 DMA、tile 提交屏障和缓存命中路径的串行访存影响；进入连续计算阶段后，除法发射间隔和递推反馈延迟限制吞吐。五路乘法和五路加减法的发射能力明显没有用满。

17 点、tile=4、双缓冲、64 B 回读、DMA outstanding=16 的 cold direct：控制器运行 13,040 周期，其中 **7,393 周期（56.69%）三个算术流水全部为空**；乘法、加减法和除法的发射吞吐利用率分别为 **7.44%、7.68%、32.36%**。这不表示所有空闲都可以消除：一部分是必需的数据/完成等待，另一部分来自目前可以优化的执行组织。

inverse 第一次缓存复用的控制器窗口为 6,708 周期，算术流水全空 **81.29%**。这里不再执行除法；主要等待变成逐点的 R 读取和 y/Ubar 写回。简单继续增加除法单元或 DMA outstanding 不会解决这条串行路径。

![资源发射利用率及后向分解](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-bottleneck-20261011/bottleneck_summary.png)

## 证据与口径

本次重新分析 28 组已验收配置、共 88 轮执行，覆盖 17/65 点、冷启动/复用、不同 tile、DMA 并发度和上下文数。另重新运行 D tuning、optimization、mode-tuned 三个验收器，合计 41 个案例。所有工作量、共享发射预算、资源累计值及以下后向时间分解均由离线检查器核对。

当前 gem5、guest 二进制与 2026-10-09 验收版本的 SHA256 一致；验收文件留存的 8 个关键源文件 hash 均与当前文件一致。其他文件没有历史 hash 的，不扩展声称已逐个验证。

原始案例、配置、输入文件 SHA256 和完整重算结果保存在 [metrics.json](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-bottleneck-20261011/metrics.json)。DMA 和上下文的独立重构保存在 [details.json](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-bottleneck-20261011/details.json)。目录日期是开始审计日期。

采用三个不同概念，避免把它们混用：

1. **控制器周期 T**：从 descriptor 首包 issue 到 `visible_done`，两端包含。逐轮之和精确等于最后一份 `CFD_RESOURCE_STATS.samples`。
2. **发射吞吐利用率**：mul/add 为 `issued/(5*T)`；div 为 `issued*4/T`，因为 divider II=4。向量发射计 5 个 scalar-lane 运算。
3. **流水活跃周期**：至少一条算术操作仍在流水中。三个池都无新发射与三个流水全部为空不同，前者可能还在等待在途算术完成。

资源统计为累计快照，只取每个池最后一条；不能将七轮累计快照再求和。`rejected`、`input_slot_full`、`rhs_candidate_not_selected` 等通常是失败调用或候选观察次数，可以在一个周期发生多次，不能直接相加形成周期分解。

`occupancy_sum` 是在途 lane·cycle，流水多级可使在途运算数超过物理通道数。旧 request 记录中的 legacy busy/idle 字段检查旧 OpKind，不能用于 shared 硬件利用率。相关代码见 [资源统计](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_diagnostics.inc:40)、[旧 busy 统计](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:4708)。

## 1. 算术资源与上下文利用率

以下均为 17 点、tile=4；O 表示 DMA outstanding。除首行 O4 外其余均为 O16；复用行取七轮试验中的第 2 轮，避免把冷启动和复用工作混在一起。

| 配置 | 控制器周期 | mul 发射利用率 | add 发射利用率 | div 发射利用率 | 三个算术流水全空 |
|---|---:|---:|---:|---:|---:|
| cold direct，双缓冲64B，O4 | 29,648 | 3.27% | 3.38% | 14.23% | 70.92% |
| cold direct，O16 | 13,040 | 7.44% | 7.68% | 32.36% | 56.69% |
| cold inverse，O16 | 14,794 | 8.97% | 9.19% | 37.72% | 52.81% |
| warm direct，O16 | 9,985 | 2.28% | 2.60% | 3.41% | 71.26% |
| warm inverse，O16 | 6,708 | 3.65% | 4.13% | 0% | 81.29% |

inverse warm 的 divider 为 0 是正常工作量变化，不是除法器故障。更少的利用率也不必然意味着更差的实现：它可能完成了更少的运算；因此还必须同时看完整时间和工作量。

cold direct 共发射 mul/add/div = 4,850/5,010/1,055 个 scalar-lane 运算。乘法发生发射的 3,231 个周期中，分别发射 1/2/3/4/5 lane 的周期数为 2,201/766/99/5/160。即使只看有乘法发射的周期，平均也只有 **1.50 lane**；满五路发射占整个控制器窗口的 **1.23%**。

与此同时，mul/add/div 至少有一条运算在途的时间比例为 35.96%/40.00%/33.40%。这组数不能替代上表的发射利用率。例如乘法流水在途并不代表五个通道都在持续接收工作。

逻辑上下文通过 trace 的进入/退出事件计算占用面积，采用半开区间，分母为 `配置槽数 × 控制器周期`。占用包含等待依赖和等待资源的时间：

| 逻辑资源 | 槽数 | cold direct O16 | cold inverse O16 | warm direct O16 |
|---|---:|---:|---:|---:|
| 输入槽 | 2 | 24.01% | 21.19% | 8.59% |
| LU 上下文 | 2 | 25.77% | 22.82% | 0% |
| 三角求解上下文 | 2 | 33.16% | 37.58% | 17.05% |
| 存活请求窗口 | 8 | 66.89% | 69.20% | 59.79% |

输入槽的占用从 INPUT_START 到 INPUT_SLOT_RELEASE，包含输入完成后等待 LU 准入的时间。请求窗口明显比计算上下文更满，是因为请求完成算术后仍会等待写回及记录退休。inverse warm 使用 parent 路径，不创建这些 child，不能用这些 child 槽位的零占用评价它的计算能力。

cold direct 全局列乘积信用配置深度 8，实际峰值仅 **1**，信用不足停顿为 **0**；parent split 乘积缓冲峰值为 5，缓冲不足停顿为 0。当前不应先扩大这些乘积缓冲。

## 2. 前沿任务在输入与上下文准入阶段没有获得优先级

`requests` 使用 `unordered_map`。输入槽分配、LU 准入和求解上下文准入直接遍历这个容器；现有 `frontier-aware` 主要作用于已激活请求的 RHS 选择，不能纠正更早阶段的准入顺序。

cold direct O16 的前八个请求在同一周期 255,481 创建，但输入顺序为 7、6、5、4、3、2、1、0：

| 事件 | 周期 |
|---|---:|
| cell 7/6 开始输入 | 255,481 |
| cell 0 开始输入 | 256,643 |
| cell 0 开始 LU | 257,044 |
| cell 0 开始三角求解 | 257,540 |
| cell 0 产生首个前向 handoff | 257,858 |

第 0 点比第 7/6 点晚 **1,162 周期**获得输入槽，而第 0 点正是整条前向依赖链的入口。后续点提前准备系数有价值，但准入不应由主机哈希遍历顺序决定。

这是已测到的调度顺序问题；1,162 周期不能直接视为修正后的可节省时间，因为后台工作已经发生重叠，优化收益需要配对仿真。

代码位置：[输入槽分配](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:2526)、[LU 准入](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:2745)、[求解上下文准入](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:3055)。

## 3. 终端记录传输成为 tile 提交的主要屏障

每个 child 的终端记录为 **1,784 B**。17 点 cold 单轮的 DMA 有效载荷分解为：

| 用途 | 字节 |
|---|---:|
| 原始 D/L/U/R 输入 | 10,480 |
| parent descriptor | 160 |
| LU 导出 | 3,400 |
| y/Ubar backing 写回 | 3,880 |
| 后向 tile 回读 | 3,840 |
| 最终 Δq | 680 |
| 17 份终端记录 | **30,328** |
| 完成状态 | 8 |
| 合计 | **52,776** |

记录占 **57.47%**。direct warm 每轮仍写出相同记录，总 DMA 为 39,576 B，记录占 **76.63%**；inverse warm 不创建逐点 child，无这些终端记录，总 DMA 为 9,248 B。因此两种算法的整轮性能差异包含目前诊断 ABI 的差异。

记录响应影响实际关键路径：`retireAutoRequests()` 等待整份 record DMA 完成后才调用 `retireLineChild()`；后者增加 tile 的 retired 数，所有点 retired 后 tile 才能提交并轮换。cold direct O16 的实测情况如下：

| tile | 本 tile 算术及必需数据 drain 均完成 | tile 提交 | 额外提交延迟 |
|---|---:|---:|---:|
| 0–3 | 258,045 | 260,429 | 2,384 |
| 4–7 | 258,343 | 260,430 | 2,087 |
| 8–11 | 263,079 | 265,697 | 2,618 |
| 12–15 | 263,315 | 265,698 | 2,383 |
| 16 | 266,443 | 267,287 | 844 |

这些延迟互相重叠，不能相加。它们也包含共享 DMA 争用和按序提交影响，不等于独立“删掉记录就能省下”的周期数。时间线中大段算术全空与记录在途区间重合，与代码中的提交屏障相符。

tiled路径的后向启动也在最后一个tile提交之后，`line-early-backward` 开关不能解除这里的record退休屏障。配置中出现启用标志，不代表该选项在所有存储路径都有提前执行效果。

当前 ABI 下等待记录响应是必要行为。优化应明确设计精简完成记录或有界独立诊断通路，保留所需可见性与完成条件；直接关闭 timing 计时会改变测量口径，不能作为同架构加速。

代码位置：[记录响应后退休](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:4596)、[tile 提交条件](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:243)、[必需 LU 导出](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:4543)。

## 4. inverse warm 的 R 读取、计算和 backing 写回串行化

tiled inverse 命中路径当前按下列顺序执行每个点：

```text
R_i 读响应完成
  → base MVM
  → Lbar MVM（非首点）
  → 最终减法得到 y_i
  → y_i 写响应完成
  → Ubar_i 写响应完成（非尾点）
  → 推进 i+1
```

`drainCachedCell()` 先等待 y 完整响应，再推进 Ubar 传输，全部完成后更新 previousY 和 cell。`processLineBaseAhead()` 在 tiled 模式被跳过，`mvm[0]` 顺序执行当前点 base 和 correction，未提前准备下一点 R/base。

17 点第一次 inverse 复用，从 descriptor 最后响应到 backward_start，共 **5,583 周期**，可按不重叠区间分为：

| 区间 | 周期 |
|---|---:|
| 逐点 R 首包发出至末响应 | 1,476 |
| R 末响应至 y 首包发出，含计算 | 840 |
| 逐点 y 首包发出至末响应 | 1,474 |
| 逐点 Ubar 首包发出至末响应 | 1,745 |
| 其余控制间隔 | 48 |
| 合计 | **5,583** |

三类数据传输占 **4,695 周期，即84.09%**。840 是两个可观测接口事件之间的区间，包含算术和边界控制，不额外声称是纯 FU 忙周期。

这一轮前向允许 O16，但实际平均 DMA 在途数只有 **1.79**，峰值 **4**，从未占满16个信用。需要的是有界 R 预取、下一点 base 提前计算，以及计算进度与 backing 写回释放的分离；继续增加 outstanding 不会自动制造独立请求。

此外，有效系数缓存已经保存整线 Ubar，但当前仍将它复制到 tile、每轮写入 backing，并在后向重新 DMA 读入。17点复用轮中，Ubar写出3,200 B、回读3,200 B，共6,400 B，占该轮9,248 B DMA有效载荷的69.20%。这是当前保守存储/发布契约产生的重复搬运。若修改ABI允许同版本输出保留，可以研究后向直接消费有效系数缓存、每轮主要搬运新的R/y/Δq；必须验证版本、可见性以及片上缓存读取端口成本，不能直接假定这6,400 B均能免费删除。

优化时仍须保存每个写回缓冲直至 DMA 响应，不能为了提早推进而覆盖在途数据。相邻 y 可以在数值就绪后内部转发，存储缓冲释放和整线完成仍等待必需响应。

代码位置：[R 准备与串行 drain](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:113)、[完成数值后创建写回](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:1702)、[base-ahead 限制](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:1900)、[始终回读y和Ubar两个区域](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:181)。

## 5. 算术内部的流水气泡与真正的计算约束

LU 的每个请求只有一个 `luWaiting` 和单个乘积暂存，`processLu()` 每周期推进一个状态。一次算术发射后，请求等待完成才推进；初始化、存储、换列等状态也分别推进。相同消元阶段中的独立行更新尚未组织成充分并发的多通道工作。

三角求解每个 RHS 也只有一个 `product` 和 `waiting`。在同一 RHS 内，乘法完成后发射减法，减法完成后才推进下一次更新；默认 mul3/add4 使同一依赖链的下一乘法约隔7周期。不同 RHS 可以交错，但受就绪性、除法器和上下文准入限制。

冷前向 child 的列消费没有使用 parent 的 split 逻辑。在 cold direct O16，80 次 Lbar 列发射中的64个同矩阵相邻列间隔均为 **7 周期**。系数全部准备好时，连续点的前向反馈间隔为 **39 周期**：

```text
5 列 ×（乘法3 + 累加4）+ 最终向量减法4 = 39
```

parent 后向 split 能提前发射列乘积，数据就绪时的反馈间隔为 **27 周期**：

```text
首个乘积延迟3 + 5次有序累加×4 + 最终减法4 = 27
```

所以 `--coeff-line-mvm-model=split` 不等于所有前后向 MVM 都已完成乘加分离。将有界乘积暂存扩展到 cold 列消费者是一个具体候选方向，仍需维持逐列累加顺序、代际检查和信用闭合。

除法器在计算连续执行时已经紧张：cold direct 的1,054个相邻除法发射间隔中 **1,030个为4周期（97.72%）**；inverse 为1,382/1,394（99.14%）。全窗口32%–38%的除法吞吐利用率与计算阶段接近II上限可以同时成立，空闲主要集中在计算阶段之间。

因此乘法、加法低利用率同时来自“没有算术任务的时间段”和“每个有发射周期仅有少量标量lane”。除法 II/倒数复用值得在数据供给改进后做配对消融；不应仅凭整轮平均利用率断言除法器永远不构成瓶颈。

代码位置：[LU 状态机](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:2819)、[RHS 状态机](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:3719)、[cold 列消费](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:3322)、[parent MVM](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:1756)。

## 6. 后向停顿可以精确分解

以下为同一17点 split工作量，后向16次更新的算术活跃时间始终为 `16×27=432` 周期：

| 配置 | 算术活跃 | tile 数据等待 | 输出队列背压 | 后向合计 |
|---|---:|---:|---:|---:|
| 单缓冲40B，O4 | 432 | 3,511 | 0 | 3,943 |
| 双缓冲64B，O4 | 432 | 864 | 1,061 | 2,357 |
| 双缓冲64B，O8 | 432 | 702 | 160 | 1,294 |
| 双缓冲64B，O16 | 432 | 411 | 97 | 940 |

本次检查在全部88轮中验证了：

```text
backward_duration = arithmetic_active + data_wait + output_queue_stalls
```

这个等式是对这些成功完成路径、当前计数定义和 trace 的验证结果，不能推广为任意调度器停顿计数都可以相加。

O16 冷启动后向仍有 **508/940=54.04%** 的时间算术流水为空，具体是411周期tile等待和97周期输出队列背压。双缓冲O4确实减少了数据等待，但反过来让深度4的输出队列形成明显背压，说明预取和输出共享 DMA 信用后的压力转移。

有些跨 tile 的反馈间隔为151/59/147周期，而同 tile、数据就绪时通常为27周期，不能把所有相邻点间隔都解释为算子本身的反馈延迟。

## 7. DMA 信用、接口缓存与片上容量

下表仅统计完整可重构的前向区间，即 descriptor首包至backward_start。占用包含本地队列等待和内存系统在途，不是总线带宽利用率。旧 trace 缺少最终 Δq 的逐包时间戳，因此不声称重构了完整后向 DMA 占用曲线。

| 配置 | 平均在途请求 / 容量 | 峰值 | 信用全满时间比例 |
|---|---:|---:|---:|
| cold direct O4 | 3.87 / 4 | 4 | 93.61% |
| cold inverse O4 | 3.86 / 4 | 4 | 93.44% |
| cold direct O16 | 12.03 / 16 | 16 | 58.06% |
| cold inverse O16 | 10.81 / 16 | 16 | 51.07% |
| warm direct O16，第2轮 | 11.53 / 16 | 16 | 54.72% |
| warm inverse O16，第2轮 | 1.79 / 16 | 4 | 0% |

O4冷启动被在途信用限制的证据很强；O16缓解了这一层，但出现新的接口缓存压力。cold direct O16 的1 KiB IOCache记录到 `no_wbuffers=2493` blocked cycles、75次事件，`no_mshrs=0`，DMA port retries为46次。缓存 miss rate为94.19%。这些是缓存/接口局部统计，不能把2493再加到整轮时间里，也不能根据miss rate断言每次都访问DRAM；请求还可能由一致性缓存层次满足。

片上容量账本：cold17/tile4/双缓冲为98,304 B预留加1,920 B tile，共100,224 B；缓存17点时再加10,200 B。预留包含请求状态、计算缓冲和记录快照，是保守预约，不能当成实际综合后的SRAM面积。当前并未对每次驻留操作数访问建立完整bank/读写端口时序，因此不能给出可信的全计算路径SPM带宽利用率或bank冲突占比。

65点、tile8、双缓冲需要39,000 B系数缓存，连同98,304 B预留和3,840 B tile共141,144 B，超过128 KiB预算，缓存会关闭。这是容量导致算法退化为逐轮重建的情况，与FU发射气泡应分开报告。

长线并未自动解决利用率问题：65点、tile4、O4、七轮全部重建的direct，mul/add/div利用率为3.37%/3.49%/14.64%，全流水空72.03%。但当前没有同一长线O16的这组优化数据，不能外推17点的加速比例。

## 8. 整轮计时中的软件部分

cold direct guest整轮15,340周期，控制器13,040周期，差2,300周期；cold inverse分别为16,894和14,794，差2,100周期。差值包含控制器窗口外的描述符构造、提交和完成处理等，不能标记成DMA等待。

guest计时从描述符构造和部分统计更新之前开始，到等待完成并更新部分计数之后结束；申请/预触摸在计时前，全量数值验证和后续记录聚合在计时后。因此应同时保留“应用提交到完成”和“加速器活动窗口”两种指标。若单独研究硬件，宜补充更细的接口时间戳，不应悄悄更换已有整轮指标。

## 优化顺序建议

1. **明确前沿准入策略**：在输入槽、LU和求解上下文准入处加入可实现的前沿优先与有界老化，消除依赖主机哈希遍历的调度行为。首先验证cell0输入时间、前向首结果时间和整轮周期；后台工作不得饥饿。
2. **精简生产接口记录并审视LU导出**：把必需完成信息和大体积诊断快照明确分开；若诊断仍必需，则保留有界缓冲和响应生命周期。比较流量、tile提交延迟、空流水时间和整轮时间。不要以关闭timing作为收益。
3. **重叠inverse warm的读取、计算和写回**：增加有界R预取/下一base状态，允许前一结果的写回与后一点计算重叠；数值反馈就绪与写回缓冲释放分别管理。同时审视有效Ubar缓存与backing之间的重复搬运。验证所有数据缓冲在最后响应之前不被覆盖。
4. **改善cold列消费和内部计算组织**：保持原累加顺序，尝试提前生成列乘积；再评估LU/RHS独立行更新并行、divider II和倒数复用。检查逐周期资源预算及完整FP64误差。
5. **最后调整规模参数**：结合新的trace选择outstanding、输出队列深度、上下文数及tile。现有O8中上下文1/1→2/2的direct仅19,064→18,076（约5.2%），T4→T8甚至18,076→18,338，因此不能统一扩大所有结构。

上述建议是基于定位结果的下一步方案，未声称已经实现或测得这些改动的收益。本次保持现有实现与基线不变。

![算术流水与DMA时间线](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-bottleneck-20261011/pipeline_timeline.png)

## 复查入口

```bash
python3 results/cfd_dsa/analysis/single-line-bottleneck-20261011/analyze.py
python3 results/cfd_dsa/analysis/single-line-bottleneck-20261011/details.py
python3 results/cfd_dsa/analysis/single-line-bottleneck-20261011/figures.py
```

`analyze.py` 检查历史源文件hash、manifest通过标记、共享5/5/1逐周期发射和divider II、理论工作量、累计统计守恒、逐轮窗口与samples一致。`details.py` 核对前向全部已知DMA包的响应闭合、信用峰值、上下文容量、inverse warm的串行顺序以及88轮后向互斥分解。`figures.py` 直接从这些数据和trace生成SVG，并检查图中区间与统计一致。

图形使用无依赖SVG生成并通过Sharp导出PNG；当前环境无可用Matplotlib。图中无新仿真点，也没有将离散trace插值成性能数据。
