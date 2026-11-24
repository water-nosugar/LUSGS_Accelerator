**单线 LU-SGS 实现检查：2026-10-18**

检查范围是当前单条网格线、每轮一次前向和一次后向的 shared/tiled/timing-DMA 主路径。检查覆盖控制器、片上访问、DMA、缓存、数值参考、配置和验收脚本。其他历史路径只检查与当前入口有关的交叉影响，没有对整个 gem5 仓库作穷尽审计。

主要判断：正常完成路径已有较完整的数值、容量和响应闭合验证，但发现了失败任务污染系数缓存、复用场景 bank 利用率重复累计、传输宽度配置与底层对齐要求不一致等实际问题。当前乘加发射率低主要需要结合反馈链解释，不能直接推导为应继续增加上下文、bank 或 DMA 并发。

本次新增的是检查报告、独立复现程序和复核数据；模型缺陷修复尚未进行。历史证据未被改写。

**1. 证据与验证范围**

- 重新运行 round3 的 `verify.py`：125 个最终场景、1178 个文件的哈希及清单核验通过。这证明归档未变化，不证明归档分析公式正确。
- 重新运行现有 11 项单元测试：通过。
- 使用现有 gem5 和正式 guest 新跑 5 个正常场景，再运行 `check_case` 和 `analyze`：全部通过。包括 N17 cold direct、N17 七轮 inverse、N65 cold direct/tile32，以及 N115/N116 的容量边界。
- 新跑 3 个非法粒度探测：输入宽度41B、warm interface 宽度7B、tile 回读宽度41B，均进入仿真后以 SIGABRT 结束，未在配置阶段拒绝。
- 新跑一次普通 LU 失败探测：失败标志被 runner 正确识别，没有误标通过。
- 编译独立 guest，验证“失败→同版本重试→强制重建”三次单线调用，确认缓存失效处理缺陷。该 guest 与正式性能 guest 分别保留并记录哈希。
- 独立重算 15 个场景的资源、阶段和 bank 指标；每个 bank 的最终累计字数与逐条 `CFD_ONCHIP_ACCESS` 事件一致。

正常复测的整轮周期为 N17 cold direct 7814、N17 七轮 inverse 18908、N65 cold direct 21818；对应归档为7816、18902、21818。数值摘要一致，前两项存在2～6周期的整轮差异。本次没有定位这种小差异的唯一原因，不能把数周期的变化直接解释为架构收益。

证据入口：

- [本次实验清单](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-review-20261018/probes.json)、[补充实验清单](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-review-20261018/extra-probes.json)
- [正常场景验收](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-review-20261018/fresh-validation.json)
- [独立重算脚本](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-review-20261018/recompute.py)、[重算指标](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-review-20261018/metrics.json)
- [二进制与独立 guest 哈希](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-review-20261018/binary-provenance.json)

**2. 已确认的实现和分析问题**

| 优先级 | 问题 | 已取得的证据 | 影响 |
| --- | --- | --- | --- |
| P1 | 失败的 child 可以把系数缓存标为有效 | 独立 guest 实测失败、同版本重试失败、dirty 重建成功 | 异常恢复复用不完整 LU，正常用例不能覆盖这个缺陷 |
| P1 | bank 利用率重复累加累计快照 | 原始访问事件与最后快照一致，汇总脚本却叠加所有快照 | 高估复用场景端口占用，可能把优化方向引向错误资源 |
| P2 | 传输宽度校验缺少8B对齐约束 | 3个 CLI 可接受配置均触发 `startOnchip` panic | 参数扫描中出现运行时崩溃，参数契约不一致 |
| P2 | 后向 `other` 与输出队列等待并非互斥 | N65/tile32 分类求和2090，实际阶段2003周期 | 消费 summary 的图表可能重复计算87周期 |

P1 表示应先于进一步选参处理；没有把尚未综合标定的硬件假设当作已确认的功能错误。

**2.1 失败任务污染缓存**

`commitTileChild` 先复制 LU/DInv、Lbar、Ubar 并增加 committedChildren；当计数到达线长时设置 `cache.valid=true`，之后才检查 `req.status`。因此“所有 child 都走到提交函数”被错误地当成“所有系数都成功生成”。

源码：[cfd_coeff_preprocess_tiles.inc:95](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:95)。缓存命中只要求 valid、版本、线长、模式匹配，见[同文件:54](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:54)。

复现使用 N=1、direct、同一组正常系数、同一版本：第一轮把 pivot 门限设为10来可控地触发 LU 失败；第二轮恢复1e-12且 dirty=0；第三轮恢复1e-12且 dirty=1。预期为失败、成功重建、成功；实际为失败、再次失败、成功。第一轮日志已经出现 `CFD_LINE_CACHE_STORE`，第二轮出现同版本命中。

这个结果没有证明正常输入会静默返回错误解；它明确证明失败后缓存状态错误。修复应在成功判断之前阻止缓存发布，并使失败行对应缓存失效；同时检查错误/取消时的在途 DMA、child 和记录退休，避免只修正一个 valid 位而留下生命周期问题。

复现：[guest 源码](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-review-20261018/guest/cache_failure_probe.c)、[运行日志](/home/zyy/gem5/results/cfd_dsa/campaigns/single-line-review-20261018/probe-failure-cache-recovery/simout.txt:171)。

**2.2 bank 利用率统计错误**

控制器在每次静止时打印累计统计。`analyze.py` 对所有 `CFD_ONCHIP_BANK` 行求和，导致前面的轮次被多次计入。应选每个 bank 的最后一份快照，或逐轮相减；不能对累计快照再求和。

来源：[累计统计输出](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_diagnostics.inc:72)、[错误汇总公式](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-round3-20261015/analyze.py:59)。现有 [SPM 检查器](/home/zyy/gem5/projects/cfd_dsa/tools/check_single_line_onchip.py:44) 已正确选择最后快照，所以容量/端口验收可以通过，而分析结果仍然错误。

以下均以整个控制器窗口为分母，复用场景包含一次重建加六次命中：

| 场景 | 原读端口利用率 | 校正读端口利用率 | 原写端口利用率 | 校正写端口利用率 |
| --- | ---: | ---: | ---: | ---: |
| N17 reuse direct，4 banks | 64.70% | 15.71% | 38.73% | 6.32% |
| N17 reuse inverse，8 banks | 36.52% | 8.93% | 20.97% | 3.42% |
| N65 reuse direct，4 banks | 78.26% | 18.99% | 53.48% | 8.91% |
| N65 reuse inverse，8 banks | 43.26% | 10.57% | 28.38% | 4.73% |

整轮周期和共享 FU 发射率没有使用这条错误求和公式，不因此失效。平均利用率低也不能证明 bank 冲突不重要：短时间内的集中访问仍可能阻塞关键反馈。需要同时看访问时段和关键操作数就绪时间。

**2.3 传输宽度参数不完整**

`startOnchip` 要求基址和步长8B对齐；DMA 拆包后，后续片上任务直接使用字节偏移作为地址。输入宽度41、tile宽度41会产生非对齐偏移；interface宽度7在 warm R 读取时产生同样问题。配置只检查正数或1～1024范围，导致配置阶段通过，运行时崩溃。

来源：[片上对齐检查](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_onchip.inc:9)、[参数校验](/home/zyy/gem5/projects/cfd_dsa/configs/run_cfd_dsa.py:1463)、[输入片上任务](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:3015)。

40B和64B都满足8B对齐，因此这不是“40B不能用”。若模型只支持完整 FP64 字，需要在配置入口明确限制；若要支持任意字节 DMA，则需要建模字节掩码、拼装和跨字写入。

**2.4 阶段分解不能直接相加**

当前 `other = backward_cycles - math - tile_wait`，没有扣除 `output_queue_wait`。N65 cold direct/tile32 中，2003周期被记录为 math=1728、tile_wait=178、output_queue_wait=87、other=97；四项相加为2090。`other` 实际包含输出等待。

来源：[analyze.py:41](/home/zyy/gem5/results/cfd_dsa/analysis/single-line-round3-20261015/analyze.py:41)。本次重算改为单独报告“算术活动区间”和“无算术活动区间”，等待计数保留独立字段。未来若要画互斥堆叠图，应记录每周期的关键路径状态和分类优先级；不能把所有 observe/stall 计数当成可相加的周期。

**3. 流水气泡与资源利用率的实际含义**

N17 现有推荐配置的共享簇指标经原始 cluster trace 重算如下。后三列没有包含错误的 bank 累计公式。

| 场景 | 乘法发射槽 | 加减发射槽 | 除法发射槽（按II归一） | 全簇无在途运算 |
| --- | ---: | ---: | ---: | ---: |
| cold direct | 18.20% | 18.80% | 79.16% | 8.70% |
| cold inverse | 20.22% | 20.70% | 85.01% | 7.07% |
| 一次重建＋六次 direct 命中 | 14.45% | 15.84% | 38.70% | 18.88% |
| 一次重建＋六次 inverse 命中 | 18.73% | 20.22% | 37.36% | 21.64% |

乘法发射率18%并不等于整个加速器82%的时间完全停止。相当多周期仍有除法或依赖加法在途。尤其 warm inverse 本来不执行除法，不能把该阶段除法利用率0解释为调度失败。

当前 split 后向对每点发射5次五路乘法、5次五路有序累加，再执行一次五路向量减法。采用乘法延迟3、加法延迟4时，在操作数预备完成且无争用的条件下，现有执行顺序每点需要 `3 + 5×4 + 4 = 27` 周期。相应乘法、加法发射槽利用率约为 `5/27=18.52%` 和 `6/27=22.22%`。这是当前顺序和数据通路的限制，不是单线 LU-SGS 所有算法的不可突破下界。

N17 direct 后向437周期中，有432周期算术活动；inverse warm 后向435周期中也有432周期算术活动。432正好是16个递推点乘27。后向读操作数留下的额外空闲已较小，再加后向缓冲对这组短线的空间有限。关键代码：[有序累加](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:2086)。

前向 cold 的限制不同：LU 和多 RHS 共用一个除法器，除法发射槽利用率接近80%～85%，同时 LU 列之间、三角求解行之间存在反馈链。现有II2敏感性实验支持“提高除法吞吐值得研究”，但不证明同面积、同频率下能实现II2。

当前 LU/RHS 小批次还采用原子宽度分配：需要4个通道时只有3个空闲就整批等待；向量工作需要5个通道。它可能产生空槽，但其收益上限需要用就绪任务、剩余通道和关键路径关联验证。不能仅凭空槽数量决定引入更复杂的拆分发射器。源码：[issueLanes](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_cluster.inc:4)、[LU行批处理](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_lu_rows.inc:4)。

长线 cold 仍有实际搬运和写回等待：

| N65 / tile32 | 后向周期 | 算术活动 | 无算术活动 | tile等待计数 | 输出队列等待计数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| direct | 2003 | 1728 | 275 | 178 | 87 |
| inverse | 1935 | 1728 | 207 | 116 | 81 |

该两例的等待计数加上剩余10周期可闭合无算术活动区间；这不赋予所有诊断计数普遍的互斥含义。现有输出优先仲裁已经改善旧问题，剩余问题应研究带最低服务保证的回读/输出信用分配、预取时机和分块就绪，而不是只提升某一类请求的固定优先级。

短线 warm 的启动和收尾也不可忽略：N17 inverse 最后一轮控制器窗口1397周期，首次算术前223周期，后向数值结束后197周期才完成。420周期约占30.1%。这部分包含 descriptor、R输入和完成/输出响应，不能全部归为计算反馈或全部归为DRAM。

**4. 数据传输与存储利用**

**4.1 必要数据、接口输出和诊断记录应分别评价。**

冷启动每点仍写出 LU；非尾点写出 Ubar，另外写 y、最终 Δq 和完成记录。对于只需最终解的生产接口，某些输出可能只为当前校验/ABI服务；对于会被逐出并在后向消费的 tile，y/Ubar 又确实需要外存后备。必须区分这些用途后再决定省略哪些写回，不能笼统删除所有中间结果。

warm direct 每点仍产生160B完成记录，而最终向量只有40B；warm inverse 命中走 parent 路径，不产生逐点记录。N17 一个 direct warm sweep 的请求侧数据量按当前契约为：R读取680B、y写回680B、Δq写回680B、记录2720B、descriptor/status共168B，合计4928B。记录约占55.2%。inverse warm 对应没有2720B逐点记录，合计2208B。

因此 direct/inverse 对比测得的是“算法＋当前接口和控制实现”的综合结果。不能把全部差距都归结为显式逆减少除法。建议增加生产接口与诊断接口的消融：可选聚合完成记录、LU导出、外部y发布；保留实际数据响应和可见性保证。

依据：[冷系数写回](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:4809)、[完成记录生成](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:5020)、[流量验收公式](/home/zyy/gem5/projects/cfd_dsa/tools/check_single_line_storage.py:128)。这里的字节都是 DMA 请求有效载荷，不是 DRAM 总线字节。

**4.2 warm y 的数据通路存在可研究的重复访问。**

`finishCachedForwardCell` 已把40B y复制进有界写队列；`stepWarmTransfers` 又等待该y写SPM、读SPM完成后才发DMA。若目标架构允许结果同时进入SPM和写回FIFO，可以研究从现有FIFO直接写回的路径，减少一次SPM读。需要明确广播/复制带宽及输出队列生命周期，不应通过删除计时凭空获得收益。来源：[结果队列快照](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:1973)、[SPM写后再读](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:166)。

**4.3 系数缓存的“全放得下才复用”有明显容量突变。**

当前缓存按整线分配 `600×N` 字节；不足时清空缓存并每轮重建，没有部分驻留或tile系数复用。此次保持192KiB预算、tile16双缓冲等相同设置，补测如下：

| 场景，连续两轮 | 固定预留＋tile | 系数缓存需求 | 命中序列 | 两轮整轮周期 |
| --- | ---: | ---: | --- | ---: |
| N115 direct | 127488B | 69000B | 0,1 | 46124 |
| N116 direct | 127488B | 69600B | 0,0 | 71722 |

N115合计196488B，距196608B上限还剩120B；N116超出480B，于是禁用整线缓存。网格点只增加1个，整轮时间增加约55.5%，主要行为变化是第二轮从复用转为重建。该退化符合当前代码，属于架构策略的局限，不是流量账本错误。应在容量阈值附近评估缓存分段、选择性保留、外存系数缓存，以及元数据/缓冲占比；不能由17/65点推广为所有线长的复用收益。

来源：[缓存容量判断](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:45)、[N115日志](/home/zyy/gem5/results/cfd_dsa/campaigns/single-line-review-20261018/capacity-n115/simout.txt)、[N116日志](/home/zyy/gem5/results/cfd_dsa/campaigns/single-line-review-20261018/capacity-n116/simout.txt)。

**4.4 40B、64B、缓存行和内存流量不能混用。**

矩阵行40B、矩阵200B；配置的64B是一次DMA请求的上限，实际还按缓存行和页边界裁剪，并没有把每行强制填成64B。40B可以设置，但跨64B行时仍会拆包。缓存未命中与写分配可能引入额外行流量，命中也可能避免访问内存。

依据：[dmaChunkSize](/home/zyy/gem5/src/arch/arm/cfd_local_spm.cc:1326)。应分别收集有效输入/输出字节、请求数、DMA端点命中/MSHR合并、L2及内存端字节；用请求字节除周期不能直接得到真实DRAM利用率。

**5. 硬件参数和建模边界**

| 项目 | 当前主路径 | 判断及缺失成本 |
| --- | --- | --- |
| 时钟 | 系统与控制器按2GHz、500 ticks/cycle | 是固定假设；没有目标工艺、器件、综合频率证据 |
| FP64共享簇 | 5乘、5加减、1除；延迟3/4/12，除法II4 | 发射和结果时序受到限制；RF读写端口、选路、广播、返回网络没有等价的端口/带宽模型 |
| SPM | 4或8 banks、每bank 1R1W、8B粒度、末字后1周期完成 | bank/端口限额真实起作用；支持同周期1R和1W隐含存储宏能力，访问时序需要结合容量和宏实现标定 |
| 容量 | Request预留8192B/项＋固定16384B，再加队列、tile、缓存等 | 能限制算法驻留，但大量依据宿主结构大小/保守常量，不是物理SRAM面积、RF面积或能耗估算 |
| DMA端点 | 1KiB、8路Cache，tag/data/response各50周期，20 MSHR | 这些通用IOCache参数对短任务影响显著；还存在write-buffer约束，不能只盯DMA outstanding |
| DMA发射 | 全局在途数有限，各生产者分别尝试发包 | 没有一个显式的全局“每周期至多一个包/多少字节”端点发射约束 |
| 内存 | `SimpleMemory`，30ns、32GB/s | 没有DRAM bank、行命中、读写转换、刷新等控制器时序；不能称为DDR带宽饱和实验 |
| 地址翻译 | SE页表同步translate | 没有IOMMU/TLB miss、页表walk和真实驱动映射成本 |
| 调度 | 每周期扫描、排序多个请求和RHS，再分配共享资源 | 仲裁延迟和实现面积尚未计入，不能假设更复杂策略无代价 |

参数来源：[时钟与内存配置](/home/zyy/gem5/projects/cfd_dsa/configs/run_cfd_dsa.py:2098)、[DMA端点](/home/zyy/gem5/projects/cfd_dsa/configs/run_cfd_dsa.py:2313)、[SPM预留](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc:4)、[DMA发射](/home/zyy/gem5/src/arch/arm/cfd_local_spm.cc:1216)、[RHS仲裁](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:4356)。

还需区分片上访问时序与数据存储本身：`OnchipAccess`记录地址、字数、仲裁和完成时间，实际数值仍保存在Request、LineTile、LineCoeffCache等宿主数组中。因此，即使物理地址布局错误地重叠，独立宿主数组也不一定产生真实SRAM那样的覆盖错误。现有检查证明端口限额、访问顺序和总容量满足模型契约，尚不能代替区域不重叠、逐字数据版本及读写别名测试。可建立按建模地址索引的影子存储，用poison/版本标记核对读取的数据来源。

在N65 reuse direct的实际trace中，一个控制器周期最多接收3个DMA包、请求有效载荷最多192B。该数字不表示DRAM在同周期完成192B，而说明端点入口没有被统一限制为64B/cycle。后级crossbar仍有自身仲裁。是否允许这种入口吞吐，应由目标架构决定；若目标只有单端口、单包发射，需要显式计时，而不能用 outstanding 数替代。

当前 DMA 分支还存在容易误用的参数：`--coeff-drain-ports`/`--coeff-drain-lat` 传给 packet 模式的 `issuePacket`，没有传给 DMA 分支的 `issueDma`；`--coeff-drain-outstanding` 则确实限制 active drain任务数。`--coeff-spm-layout` 在当前DMA/片上模型中并未作为统一的片上地址布局函数：row-striped会影响DMA分包，但片上输入地址仍为 `slot*8192+offset`。扫描这些选项时，应先检查它实际改变了哪个资源，不能只根据名字解释结果。

依据：[processDrains分支](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:4863)、[输入片上地址](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:3015)。上述为源码确认的语义边界，本次没有为这些选项另作全组合性能扫描。

降低加法延迟或除法II、增加bank/端口，均可能改变面积、时钟和功耗。没有目标硬件与综合数据时，应报告为敏感性分析，不能称为已实现的同成本硬件优化。反之，也没有依据直接断言当前3/4/12周期在所有工艺上“不合理”。

**6. 测试脚本与实验设计**

做得较完整的部分包括：逐轮完整y/Δq校验、有限值检查、FP64摘要、精确运算次数、共享通道发射限制、DMA响应闭合、SPM bank/端口检查、容量和版本约束。cache-contract还会改变R，并覆盖版本、模式和线长变化，不能说项目完全没有动态输入验证。

但仍有以下缺口：

1. **数值覆盖集中。** 主生成器构造确定性的强对角矩阵；正式125场景主要是配置变化，普通线长为1/2/3/17/64/65。没有由这些场景建立随机矩阵、近奇异、需选主元、不同尺度、NaN/Inf的完整覆盖。当前Crout不做主元交换，真实CFD系数是否满足可用条件尚需证据。输入生成见[benchmark:3130](/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c:3130)，主元检查见[LU行模型:65](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_lu_rows.inc:65)。
2. **异常完成不等同正常完成。** 现有严格DMA完成栅栏集中在成功路径；`markFailed`、`cancel`仍通过功能性proxy写状态/记录。此次缓存恢复缺陷说明需要真实的失败→重试、取消→响应收尾→重用测试，不能只靠正常生命周期断言。[失败处理](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_controller.cc:5094)。
3. **部分“故障注入”是在修改日志。** 当前opt/onchip故障测试修改trace或simout后运行检查器，能证明检查器拒绝这些错误，不能证明模拟硬件遇到错误会正确恢复。[故障检查脚本](/home/zyy/gem5/projects/cfd_dsa/tools/check_single_line_opt_faults.py:36)。
4. **参考解会预热缓存，也预写部分输出。** `raw_reference_solve`在加速器计时前读取输入，并写到硬件使用的`p->lu_d`、`p->u_bar`。这使cold主要表示加速器系数缓存未命中，不能理解为CPU/L2/DRAM全冷。预先存在的正确输出也削弱“缺失/错地址写回”的数值检测，应在参考结果独立保存后poison输出并保留canary；性能测量另外规定缓存初态。[参考解](/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c:3271)、[调用位置](/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c:5525)。
5. **标准性能复用轮通常不改变数据。** 普通sweep循环只更新版本/dirty，专门的cache-contract才改变R和部分D。还应在跨tile、跨页及容量不足场景改变R/L/U并独立求残差；不把同一组输入重复通过当成更广泛的数值证据。
6. **可复现记录没有完全自动化。** runner记录gem5哈希和git状态文本哈希，但没有逐case记录guest二进制、配置脚本和源内容哈希；未提交文件内容变化可能保持相同git状态。round3通过额外冻结/清单补充了这些信息，今后应纳入runner，并记录解析后的最终配置。[manifest生成](/home/zyy/gem5/projects/cfd_dsa/tools/run_experiment.py:132)。
7. **runner通过与结构验收通过是两件事。** runner会正确拒绝无完成标志的tick超时，但`run_suite.py`不会自动执行资源、容量和DMA专项检查。应在正式campaign流程中串起仿真、专项验收和报告生成，避免只看manifest的passed。
8. **参数边界覆盖不足。** 已归档125场景的onchip latency均为默认值、SPM读写端口均为1、interface width均为默认值；这些可配置维度尚未得到同样覆盖。此次3个粒度崩溃就是边界遗漏。增加输入对齐、页边界、partial tile、容量阈值和时延组合的定向测试，比重复同一短线更有价值。

旧通用`compare_solution`辅助函数也缺少非有限数检查，NaN比较可绕过阈值判断；当前自主单线每轮校验另外显式检查got有限性，因此不能把这个辅助函数缺口直接说成当前主路径已接受NaN。若扩展旧路径或更复杂参考输入，应统一数值检查并同时检查reference有限性。源码：[step2_validation.inc:15](/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/step2_validation.inc:15)、[单线每轮检查](/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/step2_streaming.inc:957)。

**7. 建议执行顺序与验收标准**

1. **先修正确性与统计。** 失败缓存不可命中；错误/取消有明确收尾契约；宽度不支持时在启动前拒绝；bank按最后快照/增量统计；阶段图使用互斥状态。把本次失败→重试和3个宽度探测纳入回归。
2. **明确硬件目标与接口。** 选定时钟、FP64时序依据、SPM宏端口、RF/返回网络、DMA入口宽度、相干路径及内存模型；明确生产接口是否要求逐点记录、LU和y外部可见。每项未标定参数保留敏感性结果。
3. **优先研究容量与实际数据需求。** 在115/116这类阈值附近比较部分系数缓存、分段复用与整线重建；对诊断记录和LU导出作消融；对驻留tile的后备写回区分必需与可省略，保持逐出前完成写回的约束。
4. **再优化关键反馈和重叠。** cold重点是LU/RHS除法和就绪链；warm重点是向量反馈以及启动/完成开销；长线cold重点是回读与输出竞争。比较时报告整轮时间、关键路径等待、流量和实际资源成本，保持数值语义一致。
5. **最后调整资源配比。** 分别报告cold、warm和混合轮；同时扫描线长、更新间隔、容量阈值、对齐及内存初态。利用率作为解释指标，不设“越高越好”的独立优化目标。固定5/5/1中的上下文数不等于FU数量，缩减通道需要相应的数据通路与调度模型。

当前证据支持继续完善单线研究主线；尚不支持“真实CFD已验证”“DRAM已饱和”“资源收缩等于等比例面积降低”或“降低算术延迟没有硬件代价”等结论。
