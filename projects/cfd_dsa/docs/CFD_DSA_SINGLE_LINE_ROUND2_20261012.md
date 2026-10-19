# 单线第二轮流水与存储优化（2026-10-12）

本轮按“生命周期解耦 → 逆向留存/预取 → LU 同列并行及仲裁 → 算术敏感性 → SPM 建模与选参”实施。研究范围仍是一条网格线，每轮一次前向、一次后向。新增优化通过 `--coeff-line-opt-level=5..8` 显式启用；保留 0..4 的对照路径。

## 1. 已实现的五项改动

### 完成记录和计算请求分别回收

原先计算请求必须等 320B 完成记录的全部 DMA 响应后才能释放窗口。level5 把身份信息和记录快照移入有界完成队列，立即释放 Request 和计算窗口 credit。记录 DMA 独立推进，响应后才回收完成队列 credit。队列满时背压，最终完成状态仍等待记录、数据和状态的必要响应。

默认完成队列深度 8，每项预留 1024B；增加 `REQUEST_DETACH`、`CFD_COMPLETION_QUEUE` 和最终 `CFD_LINE_LIFETIME phase=complete` 诊断。所有接受的用例都验证了 `commit ≤ detach ≤ retire`、issued/completed 闭合和最终 live=0。这里没有减掉记录流量，也没有删除完成 fence。[实现](../../../src/arch/arm/cfd_coeff_preprocess_lifecycle.inc)、[退休入口](../../../src/arch/arm/cfd_coeff_preprocess_controller.cc)。

warm inverse 的 R/base 输入槽与 y 输出快照队列也拆开：y 进入有限写队列后释放输入槽，写队列继续持有数据和 tile 引用直到响应。输入上限从 `min(4,window)` 改为 `min(8,window)`；输出队列默认深度 8。允许计算进入已分配的下一前向 tile，旧 tile 由响应引用保护。17 点推荐用例最后一个命中轮中，16 对相邻 R 预取和 16 对 y 写回发生重叠，输入峰值 8，输出峰值 5，输出队列等待为 0。[实现](../../../src/arch/arm/cfd_coeff_preprocess_tiles.inc)。

### 留住逆向首个需要计算的 tile

双缓冲前向进入尾 tile 时，若后面已无新 tile，就保留前一个 tile 的 y/Ubar。后向尾点取得 y 后直接消费保留块，释放尾缓冲用于预取更早的块。两块固定容量缓冲始终复用，没有额外整线数组；单缓冲保留原回读路径。

设线长 N、tile 长 T，旧回读前缀为 `floor((N-1)/T)*T`；level6 双缓冲再减去一个 T，最小为 0。cold 每个回读点 240B，命中轮只读 y 的 40B，Ubar 由有限系数缓存提供。N=17、tile4 时，cold 回读从 3840B 降到 2880B；选择 tile8 后降到 1920B。前向必需写回没有省略。验收同时核对回读范围、字节数、响应数、留存事件和最终数值。[实现](../../../src/arch/arm/cfd_coeff_preprocess_tiles.inc)。

### 展开 LU 同列独立目标并调整 LU/RHS 仲裁

level7 同时计算当前列的多个独立下三角或上三角目标，各目标内部仍按原来的 k 顺序执行乘法、减法，保持 FP64 舍入顺序。独立上三角商可按既有 divider II 发射，不再等前一个商完成才提交下一个商。它们共用原来的 5 mul、5 add、1 div，每点 LU 操作数仍为 30 mul、30 sub、10 div。

仲裁顺序为：就绪递推反馈、尚未交付前向结果的最前沿 LU、就绪 RHS、后台 LU。前沿 LU 为当前依赖链提供系数；后台 LU 使用 RHS 之后的剩余槽位。`luVisitedCycle` 保证一个 LU 请求不会在两个仲裁遍历中同周期推进两次。新增 LU 行组 trace，并逐周期检查物理 lane 上限、完成延迟和除法 II。[LU 行组](../../../src/arch/arm/cfd_coeff_preprocess_lu_rows.inc)、[共享簇](../../../src/arch/arm/cfd_coeff_preprocess_cluster.inc)。

### 分开评估低延迟加法和除法吞吐

在新存储模型下单独评估 add latency=2、divider II=2、reciprocal 以及 tile8。推荐配置仍保留 latency=3/4/12、divider II=4；敏感性配置改变了算术硬件能力，需要后续实际实现和综合支持。

以 level8、tile4、DMA16 的固定配置为对照：cold direct 8488 周期改成 II2 后为 8580，未获得收益；cold inverse 9546→7958，减少 16.63%。inverse 七轮复用 20938→19780（II2），改成 add2 为 18948，add2 同时用 tile8 为 18586。direct 七轮复用 23292→22372（add2），add2/tile8 为 21358。降低反馈延迟时同时扩大已测 tile 留存距离，避免算得更快后首块搬运成为新的瓶颈；这些实验没有计入固定延迟推荐结果。[算术 suite](../suites/single-line-round2-arithmetic.json)。

### 加入 SPM bank、端口、响应和仲裁约束

level8 默认开启新的片上访问模型；level5..7 可用 `--coeff-onchip-timing=1` 开启相同模型作公平对照。

- 每次访问以 8B 字为单位，bank 为 `(address / grain) % banks`。每 bank 的读、写端口分别限额，每周期重置；推荐粒度为 8B。
- 工作进入仲裁至少等待 1 周期，最后一个字获准后再等待 `--coeff-onchip-latency`，默认 1 周期，才能发布 ready。
- 前沿读取优先级最高，后台 base 次之，批量输入最低；同优先级按任务编号稳定排序。有界任务队列满时背压。
- DMA 输入/回读先预留未激活的存储任务，DMA 响应后激活写入；计算必须等待存储完成和操作数读出。cached LU/Lbar 先装入计算寄存器，不能用缓存命中绕过 SPM ready。
- 覆盖输入暂存、系数缓存和驻留 tile。活跃 LU/RHS 数据、乘积和累加器作为寄存器；控制、完成和输出为有界 FIFO。所有这类状态计入容量预算，但没有把它们伪装成每次算术都访问 SPM。

增加每字访问、启动、激活、完成、取消、队列容量和 bank 冲突 trace。验收器独立推导全轮字流量，检查 bank 映射、逐周期端口上限、访问地址、ready 延迟、缓存装载与消费顺序、最终无未完成任务。[SPM 实现](../../../src/arch/arm/cfd_coeff_preprocess_onchip.inc)、[验收器](../tools/check_single_line_onchip.py)。

## 2. 容量和时序边界

全轮 ROI 保留 guest 提交、完成轮询、设备计算和必需传输；控制器窗口从 descriptor 首次 timing 访问至完成状态可见。发射利用率用窗口中的实际操作数除以物理发射槽位，算术全空闲用各算术操作在途区间的并集计算；它们不是同一指标。

统一预算包括：

```text
基础请求/输出状态 = 8192 × (inputSlots + LU contexts + solve contexts + outputDepth) + 16384
RHS 行并行状态    = 4096（启用时）
level5 生命周期   = 1024 × completionDepth + 2048 + 256 × warmWriteDepth + 64 × DMA outstanding
SPM 访问状态      = 256 × onchipDepth + 128 × 基础状态槽数 + 1024
tile              = 240 × min(N,T) × buffers
系数缓存          = 600 × N（仅容量足够且符合缓存契约时）
```

这些是保守硬件状态预算，包含寄存器/FIFO，不能直接当作 SRAM 字节使用率或综合面积。N=17 推荐 cold 峰值 139264B，七轮复用峰值 149464B，均低于 196608B；每包独立 DMA 数据快照也计入预算。没有计时路径以无界 host 整线数组代替容量约束。

## 3. 同一二进制的结果

下表基线和优化版都启用了相同的 SPM bank/端口/仲裁规则及 timing DMA，算术资源与延迟相同。banked level5 基线已包含生命周期解耦；因此此表的提升不重复归入 level5 的收益。固定配置列保留 4 banks、每 bank 1R/1W、tile4、DMA16；重新选参允许存储和 DMA 资源变化。

| N=17 场景 | banked level5 基线 | 固定资源 level8 | 选参后的周期 | 较基线减少 |
|---|---:|---:|---:|---:|
| direct cold，一轮 | 8788 | 8488 | 8190 | 6.80% |
| inverse cold，一轮 | 9722 | 9546 | 9230 | 5.06% |
| direct，一次重建六次命中 | 24676 | 23292 | 21682 | 12.13% |
| inverse，一次重建六次命中 | 21452 | 20938 | 19950 | 7.00% |

固定资源 level5→8 的减少分别为 3.41%、1.81%、5.61%、2.40%；其余改善来自参数选择。选参共同采用 tile8 双缓冲、64B DMA 包、DMA outstanding32、SPM 任务队列64、最终输出队列8、2个 LU 上下文。solve 上下文为2，direct复用为4；bank 数依次为4、8、8、8，每 bank 1R/1W。

8 bank 并非所有场景都必要：cold inverse 使用4 bank为9234，只比8 bank多4周期（0.043%），选择硬件实现时宜保留这个更少端口的备选。direct复用4 bank为22192，inverse复用4 bank为20628；8 bank 分别减少2.30%、3.29%。这些是相同总容量下增加 bank/端口的取舍，没有面积相同的证据。

旧操作数假设下的消融如下；level8 加入真实片上访问等待，不能把 level7→8 的增加隐藏掉。

| level，固定 tile4/DMA16 | direct cold | inverse cold | direct 七轮 | inverse 七轮 |
|---|---:|---:|---:|---:|
| 4，上一轮优化 | 8686 | 9662 | 23830 | 23152 |
| 5，生命周期解耦 | 8692 | 9666 | 23704 | 20012 |
| 6，逆向首块留存 | 8480 | 9526 | 22896 | 19548 |
| 7，LU 行组和仲裁 | 8446 | 9502 | 22614 | 19528 |
| 8，增加 SPM 时序模型 | 8488 | 9546 | 23292 | 20938 |

全部阶段/选参对照使用同一最终二进制，同场景的 FP64 输出位模式一致；不得用旧报告另一二进制的几个周期差异代替本表。

## 4. 剩余停顿和资源利用率

推荐配置的设备窗口如下。warm 为最后一个命中轮，不含冷启动的除法工作量。

| 路径 | 控制器周期 | mul 发射 | add 发射 | div 发射 | 算术全空闲 |
|---|---:|---:|---:|---:|---:|
| direct cold | 5487 | 17.68% | 18.26% | 76.91% | 11.34% |
| inverse cold | 6767 | 19.61% | 20.08% | 82.46% | 9.86% |
| direct warm | 1946 | 11.72% | 13.36% | 17.47% | 27.49% |
| inverse warm | 1538 | 15.93% | 18.01% | 0% | 39.27% |

后向阶段的非重叠核算：

| 路径 | 后向总周期 | 算术活动并集 | tile DMA 数据等待 | 其余空闲 |
|---|---:|---:|---:|---:|
| direct cold | 537 | 432 | 9 | 96 |
| inverse cold | 545 | 432 | 49 | 64 |
| direct warm | 496 | 432 | 0 | 64 |
| inverse warm | 496 | 432 | 0 | 64 |

其余空闲包括片上操作数 ready 和控制推进；不能把 data_wait=0 解释为没有存储延迟。SPM 模型启用后，即使数据已驻留，也要经过有限端口读出。banked level5 的 cold data_wait 为354/374周期，warm 为163/57；本轮降低了回读等待，顺序反馈的432周期没有被更改。

现阶段优先级已经变化：

1. **反馈链是 warm 的主要约束。** 16次后向递推仍各需27周期；5列顺序累加及最终减法受 add latency=4 限制。额外上下文不能制造单线跨点独立性。inverse warm 的除法器空闲符合算法工作量，不应为提高它的利用率增加无用计算。
2. **inverse cold 的除法吞吐仍敏感。** II2 有明显收益；direct 的高 divider 发射利用率并没有转化为 II2 加速，说明其整体依赖和调度限制仍存在。下一步需用实际可实现的 divider/加法反馈结构比较面积、时钟和周期。
3. **SPM 平均端口利用率低但局部突发仍可延迟前沿。** 更多 bank 的收益只有0.04%～3.3%，输入槽增加也未测到收益。保留按银行、优先级和关键路径分类的 trace，避免只看平均百分比继续扩资源。
4. **短线的提交与完成固定成本仍明显。** 整轮与控制器窗口不能混为一谈。完成记录队列在推荐用例中未造成 detach 背压，warm 输出峰值5/8且等待0；继续扩大这些队列缺少收益证据。

## 5. 验证和复现

按用户指定命令完成编译：`scons build/ARM/gem5.opt -j2`，最后构建日志包含 `scons: done building targets.`。ARM guest 也已重建。

| 验证组 | 仿真数 | 单线 sweep 数 | 二进制范围 |
|---|---:|---:|---|
| 最终阶段、存储选参、算术敏感性 | 65 | 269 | 最终版本 |
| 原 C 完整回归、D 优化回归 | 60 | 159 | 最终版本 |
| 参数扫描、边界与信用压力 | 73 | 320 | 仅最终生命周期日志位置不同的前一构建 |
| 合计 | 198 | 748 | 分组记录哈希，不混称全部同一版本 |

最新版本为125次仿真/428轮；另73次扫描和压力仿真保留独立来源。总体最大绝对误差 `2.22e-16`，包括 N=1/2/65、单 credit、缓存不适配、dirty/version/R 契约、serial/split/tree 和高 DMA 压力。另通过11项单元测试、12个非法配置拒绝，以及4个片上访问故障、6个存储故障、4个生命周期故障注入。故障注入验证的是验收器能拒绝损坏的证据，不是运行时任意故障恢复。

```bash
# 推荐四个已经实测的场景；campaign 应使用未存在的新名称。
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/single-line-round2-recommended.json --campaign single-line-round2-recommended
python3 projects/cfd_dsa/tools/check_single_line_opt.py results/cfd_dsa/campaigns/single-line-round2-recommended --suite projects/cfd_dsa/suites/single-line-round2-recommended.json --report results/cfd_dsa/campaigns/single-line-round2-recommended/optimization.json

# 只读复核已保存的源码、构建、验收与仿真证据。
python3 results/cfd_dsa/analysis/single-line-round2-20261012/verify.py
python3 results/cfd_dsa/analysis/single-line-round2-20261012/analyze.py
```

[推荐配置](../suites/single-line-round2-recommended.json)、[选参 suite](../suites/single-line-round2-selection.json)、[分析与来源](../../../results/cfd_dsa/analysis/single-line-round2-20261012/)。

本轮交付是 gem5 的有限资源时序模型和单线合成算例验证。尚无 RTL/综合面积、详细计算寄存器端口和真实 CFD 数据验证；因此性能数字只支持这里的资源和访存假设，不能直接当作芯片面积效率或实际 CFD 加速比。
