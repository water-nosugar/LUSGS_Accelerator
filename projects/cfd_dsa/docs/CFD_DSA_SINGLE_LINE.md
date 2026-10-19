# 单线 LU-SGS：研究范围、实现与验收

## 研究范围（2026-09-22）

本轮按用户修正，仅研究**一条网格线，每个 sweep 一次前向、一次后向**：

\[
y_i=b_i-\bar L_i y_{i-1},\qquad
\Delta q_i=y_i-\bar U_i\Delta q_{i+1}.
\]

第一个 cell 不消费 Lbar，最后一个 cell 不消费 Ubar，边界值为零。
保持编号和前后向递推依赖；有序显式逆路径保持原有列消费顺序和 FP64 位模式，
直接求解与树形归约对照使用全量误差容差。原始 D/L/U/R 输入及软件参考继续使用 Step2
benchmark。这里的 y 对应代码中的 dqStar。

多线交错、二维/三维网格、一般 DAG、多色及子域近似均不纳入本研究。
前一轮新建的通用图原型已从项目入口撤出；既有 graph campaign 仅保留为历史记录，
不作为本单线方案的性能或正确性证据。用户原有的多线兼容代码和历史数据未删除。

## 三项研究重点

1. **单线依赖前沿调度**：围绕当前 cell 缺少的 base/Lbar/Ubar 工作组织 LU、RHS
   和列消费；独立系数预处理可以提前执行，递推本身仍逐 cell 推进。无需通用
   cell DAG 或多邻居计数。backward 只有在最后一个 y 就绪后才能启动；
   early-backward 的含义是无需再等无关子请求的退休记录，不能解释为绕过前向依赖。
2. **5×5 运算的流水与反馈延迟**：顺序列消费是基线，乘法提前、有序累加与
   树形归约是对照。多上下文用于独立预处理工作和流水填充，不制造多条独立递推链。
   低 II、FU 使用率、最长等待和完整 sweep 周期分别报告。
3. **跨前后向的数据生命周期与实际写回**：R/base/Lbar 消费后可释放；y/Ubar
   必须保存至对应后向消费；Δq 可内部转发并在有界队列中写回。系数跨 sweep
   复用必须服从显式版本。输入窗口有限不代表整条线的数据驻留容量有限。

旧 `legacy` 模式的 ColumnFma 与 split 的 lineProduct/lineAccumulate 是
不同的模拟资源池；cold 前向仍会使用 ColumnFma。不能仅因都叫“5-lane”就
宣称二者具备相同总面积。C 阶段新增 `shared` 模式统一浮点运算的发射资源，
具体契约及验收见下文。旧模式相同单次名义延迟的
serial7/split7 对照只排除一项延迟配置差异，不能代替综合或完整资源等价分析。

## 本轮代码落地

### 单线实验入口

`configs/lusgs/run_cfd_lusgs_single_line.py` 调用现有 Step2 路径：

- 强制 `--lusgs-lines=1`、`--lusgs-interleave=1`，检查每一个重复参数，越界报错。
- 默认一个 sweep、raw streaming/event、direct 前/后向、原 frontier-aware、
  LU/TRSM 2/2 上下文、serial/divide；允许显式覆盖资源进行消融。
- 旧 Step2 入口和它的默认值不改动。
- `tools/run_experiment.py --target lusgs-single-line` 记录实验到现有不可变 campaign。

### 最终 Δq 的 timing DMA 与完成栅栏

原 `dma` 路径在四个后向发布位置通过 `proxy().writeBlob()` 直接写出最终向量，
没有为这些 40 B 向量发 timing 请求。本轮统一到 `publishLineOutput()`：

- `internal`/`packet` 对照保持原行为；`dma` 使用 `CfdLocalSpm::issueDma()`。
- 每条线有独立的有界最终向量队列，深度复用 `--coeff-drain-queue-depth` 数值；
  它与已有 coefficient drain 队列是两个队列，容量不能重复解释为一个共享队列。
- 队列条目复制 5 个 FP64，持有数据直至所有响应返回。底层 `dataStatic` 要求
  缓冲保持有效；回调同时持有条目，避免取消/回收后悬空。
- 按 `--coeff-drain-width`、cache line 和虚拟页边界拆包，服从共用
  `--coeff-spm-outstanding` 限制及现有 timing port retry 协议。
- 每条线每个 controller tick 最多提交一个最终向量 chunk；队列满时对后向
  生产者施加背压，已产生的 Δq 仍作为下一个 cell 的内部反馈值。
- 成功完成须同时满足：后向数值计算结束、所有必要 child 退休、全部
  `40*N` 字节最终向量收到 DMA 响应且输出队列清空。
- trace 增加最终向量的 issued/completed 字节和包数、队列峰值、队列背压次数、
  outstanding 拒绝次数、最后响应周期。背压样本不能与总周期直接相加。

DMA 接口经 `system.coeff_dma_cache` 接入 L2 总线。原先把非缓存设备的部分
`WriteReq` 直接送入一致性总线，遇到 CPU L1 脏行会触发
`cache.cc:1225` 的 Modified-line 断言；单点、双点和 dirty rebuild 均可复现。
新接法采用 `configs/common/Caches.py` 中 IOCache 的参数：1 KiB、8 路、
tag/data/response 各 50 周期、20 MSHR、每 MSHR 12 个 target。
cache 负责生成正确的一致性请求，不修改或屏蔽 cache 断言。
该 1 KiB cache 是额外的系统资源，不计入下面的 SPM 容量预算；其延迟计入
DMA 结果。完成表示数据已在一致性域中可见，不表示脏行已经落到 DRAM。
改变通路后的 DMA 周期不可直接与原直连通路混合作性能结论。

这只补齐**最终向量写回**，没有把所有功能性访问改为 DMA。inverse 缓存命中路径的
RHS 读取、部分 y 发布和状态记录仍有 proxy 访问；C 阶段 direct 缓存命中的
新 R 沿用 child 输入 timing DMA。parent/cache 中完整数组也
仍为 host 容器。因此本轮不宣称完整双向 SPM/DMA、外存 spill 或物理存储容量
模型已完成。取消后已经被 memory system 接收的请求不能撤回；本轮测试正常
完成路径，不增加“取消即无在途写回”的承诺。

### 存储容量账本

```bash
python3 projects/cfd_dsa/tools/analyze_single_line_storage.py \
  --cells 17 257 4096 --spm-kib 1024 --reserved-kib 64
```

只计算后向待用有效载荷，忽略最后一个不需要的 Ubar：

\[
S_{backward}=40N+200(N-1)=240N-200\text{ bytes}.
\]

4096 cells 需要 982,840 B，接近 960 KiB。1 MiB 减去 64 KiB 预留后仅剩
200 B 余量；4097 cells 已放不下。这是数据容量下界，不包括未预留的元数据、
对齐或端口限制，不能据此宣称全部 SPM 可用。

现有 `LineRequest` 中 dqStar/dq/rhs + (DInv 或 LU)/Lbar/Ubar 的数组有效载荷为 720N B；
版本缓存另外有 600N B。容器元数据、baseRing、产品缓冲、child 状态另计。
这两个数描述当前 host 模型存储，不能直接当作物理 SPM 使用量。

## 验证命令

```bash
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 lusgs-step2
python3 -m unittest discover -s projects/cfd_dsa/tests -p 'test_single_line.py' -v
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/single-line-output-dma.json \
  --campaign single-line-output-dma-coherent-v2-20260922
python3 projects/cfd_dsa/tools/check_single_line_output_dma.py \
  results/cfd_dsa/campaigns/single-line-output-dma-coherent-v2-20260922
```

专项 suite 所有用例都是一条线，覆盖 N=1/2/17/257、window=1、输出队列深度1、
outstanding=1、40 B drain 宽度的边界拆包、serial/split，以及同一条线的 cold/reuse/dirty 三次
sweep。重复 sweep 仅验证版本与写回生命周期，每次仍恰好一次前向和一次后向。
检查器核对全量数值校验、同二进制 FP64 摘要、40N 字节、包数闭合、队列容量、
边界拆包和“最后响应早于或等于成功完成”。不使用多线吞吐作加速指标。

### ARM 编译故障恢复

本次链接失败的根因是生成文件 `build/ARM/ext/drampower/libdrampower.a`
内部的 `LibDRAMPower.os` 成员长度为 0，而相应独立目标文件是有效 ELF。
保留损坏 archive 为同目录下 `libdrampower.a.corrupt-20260922` 后，按
`ext/drampower/SConscript` 已声明的 13 个对象重建 archive，核对归档成员与
独立对象逐字节一致且符号存在，再运行 `scons build/ARM/gem5.opt -j2`。
不需要修改 DRAMPower 源码或添加链接库。SCons 的 Configure 缓存会导致仅移走
该 archive 后不自动重建，因此本次显式重建了该生成文件。

### 实测验收（2026-09-22）

`scons build/ARM/gem5.opt -j2` 退出码为 0；6 项单元测试通过。
最终 campaign 为 `results/cfd_dsa/campaigns/single-line-output-dma-coherent-v2-20260922/`，
13 项仿真全部通过，专项检查器输出 `SINGLE_LINE_OUTPUT_DMA_CHECK_PASS`。
所有用例的 gem5 SHA256 一致：
`fdae8d2699a2f9513b2dfff1c5b680d7b52d5653b9480317191acb39ebf63b5e`。

| 用例 | streaming 计算窗口周期 | 最终向量 DMA 完成字节 | 输出队列峰值 |
| --- | ---: | ---: | ---: |
| internal-serial-17 | 7,922 | 0 | 0 |
| internal-split-17 | 7,968 | 0 | 0 |
| internal-serial7-17 | 8,310 | 0 | 0 |
| internal-split7-17 | 8,118 | 0 | 0 |
| dma-serial-17 | 16,650 | 680 | 4 |
| dma-split-17 | 16,658 | 680 | 4 |
| dma-one-cell | 2,932 | 40 | 1 |
| dma-two-cell-window1 | 4,274 | 80 | 1 |
| dma-257 | 229,346 | 10,280 | 4 |
| dma-q1-outstanding1 | 52,632 | 680 | 1 |
| internal-reuse | 15,080 | 0 | 0 |
| dma-reuse | 32,566 | 2,040 | 4 |
| dma-reuse-q1 | 106,370 | 2,040 | 1 |

reuse 三项报告三个 sweep 的累计计算窗口，其余为单 sweep；均不是全程序耗时。
全量数值检查通过，同规模对照的 y/Δq FP64 摘要一致。257 点用例实际完成
386 个最终输出包，覆盖 cache-line 拆包；单槽、单 outstanding 用例记录到
2,818 次输出队列背压。每次成功完成都晚于或等于最后 DMA 响应，且全局
DMA 发出数等于响应数。三 sweep 用例的系数缓存命中序列均为 `0,1,0`。
这里 dirty 指版本失效并重建，现有 benchmark 不改变数值系数；不把它解释为
真实 CFD Jacobian 更新或收敛验证。

这组短线实测中默认 split 没有快于 serial；名义单次延迟均为 7 的对照中，
split 为 8,118 周期，serial 为 8,310 周期。资源池并未证明面积等价，不能据此
作等面积加速结论。internal 的 0 DMA 字节表示功能性写回对照，不能解释为零访存。

## 2026-09-23 单线调度验证

原计划的“通用网格基线”因研究范围改为单线而未完成。单线版 A/B 使用原始 D/L/U/R 和 Step2 软件参考；固定资源 trace 核对了 17/64 点 cold sweep 每个 cell 恰好分配一次系数 child，三 sweep 的分配为第 1、3 次各一次、第 2 次缓存命中。每次 sweep 的 y/Δq 全量数值校验通过。这不构成真实 CFD 的收敛基线。

可选 `--coeff3-schedule=rhs-frontier` 每周期汇集已就绪 RHS，先选当前前向 cell 缺少的 Lbar/DInv 列；后台 RHS 等待超过 256 周期后可提升。排序本身的硬件面积和周期尚未建模；原 `frontier-aware` 保持默认。运行 `suites/single-line-rhs-frontier.json` 并用 `tools/check_single_line_rhs_frontier.py` 检查：最终 8 项均通过，检查器输出 `SINGLE_LINE_RHS_FRONTIER_CHECK_PASS`，同组 FP64 摘要、每点 child 次数及各 FU 发射总数相同。证据在 `results/cfd_dsa/campaigns/single-line-rhs-frontier-final-20260923/`。

| 单线负载 | LU/TRSM | 原模式周期 | 逐 RHS 模式周期 | 变化 |
| --- | --- | ---: | ---: | ---: |
| cold 17 点 | 2/2 | 7,918 | 8,248 | +4.17% |
| cold 17 点 | 2/4 | 7,864 | 7,678 | -2.37% |
| 17 点三 sweep | 2/2 | 15,076 | 15,740 | +4.40% |
| cold 64 点 | 2/2 | 24,378 | 26,002 | +6.66% |

这些是 streaming 计算窗口周期。2/2 冷启动最长 RHS 等待 432→276 周期，但完整周期变长，所以新模式仅供实验。后台提升门槛 64 周期的独立消融在 `results/cfd_dsa/campaigns/single-line-rhs-frontier-age64-20260923/`；四组分别变化 +5.30%、-0.66%、+5.56%、+6.69%，未优于 256 周期版本。

该轮二进制重新运行 13 项单线 DMA 回归，`SINGLE_LINE_OUTPUT_DMA_CHECK_PASS`；证据为 `results/cfd_dsa/campaigns/single-line-output-dma-rhs-regression-20260923/`。D1 的最终向量 timing 写回继续成立。该时点尚未实现的 C 内容由下文 2026-10-09 增量补齐；D2 有限容量 spill/reload、D3 全数据 timing 生命周期仍待完成。

## 后续顺序与验收边界

### C：冷启动 base 直接求解（2026-10-09）

新增 `--coeff-base-solve=inverse|direct`，默认 `inverse`。这与原
`--coeff-stream-dinv-consumer=direct` 不同：原参数仅跳过 DInv 写回，仍求
5 个单位向量 RHS；新参数把这 5 个 RHS 换成一个 R 向量，直接求 `D*b=R`。
Lbar/Ubar 的槽位、运算顺序、边界掩码、前后向依赖及写回栅栏保持原契约。
内部点为 11 RHS，首尾点为 6 RHS，单点为 1 RHS；整条线从 `15N-10`
降为 `11N-10` RHS。每个 RHS 在当前 divide 模型中发射 5 次除法、
20 次乘法和 20 次减法，因此省下 `20N` 次除法、`80N` 次乘法、
`80N` 次减法以及 `5N` 次 base ColumnFma 调用。LU 工作量不变。

直接求解沿用 LU/TRSM 的资源池及上下文，完成时把结果送入 base accumulator，
不伪造 DInv 矩阵或列就绪事件。benchmark 的 `baseDirectRhsSolved` 单独计数，
`dInvGenerated` 和 DInv 列消费数为零。公开 descriptor/record 的大小不变，
新增 flag bit 21。每个 sweep 的完整 y/Δq 在计时窗口之后与 raw 软件参考
比较，使用绝对/相对误差 `1e-10`，同时检查有限值；不同算法不要求位模式相同。

此项最初仅支持一条自主调度线、direct Ubar、每 sweep 重建。下文的 C 增量
支持 LU 缓存复用及显式更新间隔；范围仍是一条自主调度线和 direct Ubar。
parent 现在按模式分配 LU 或 DInv 数组，不同时保存两者，仍不能把 host 数组
有效载荷当作已实现的物理 SPM 容量。

```bash
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/single-line-base-solve.json \
  --campaign single-line-base-solve-20261009
python3 projects/cfd_dsa/tools/check_single_line_base_solve.py \
  results/cfd_dsa/campaigns/single-line-base-solve-20261009 \
  --report /tmp/single-line-base-solve-resource-audit.json
```

专项检查逐 cell/RHS 核对边界、完成与退休次数，核对每个资源池的精确发射量，
比较 count/latency/II 和全部非算法配置，并验证每个 sweep 的数值及 DMA
可见性。该轮资源报告记录逻辑运算宽度与调用者，不能当作综合面积：LU 与 TRSM
仍各有独立的 scalar div/mul/sub，cold 前向使用 ColumnFma，split 后向使用
另一个 product/accumulate 池。serial 模式下 cold base/Lbar 与后向已经通过
同一个 ColumnFma 池仲裁。上述描述适用于 `legacy`；下文 `shared` 统一资源，
并为前/后向最终向量减法计入五个加减通道。

reciprocal 对照单独计算工作量：每个 cell 只计算 5 个倒数，每个 RHS
发射 25 次乘法和 20 次减法。direct 因减少 RHS 而减少乘减工作，但不会
减少已经共享到每个 cell 的倒数除法数。batch 0 的旧
`identityBatchWaitCycles` 字段在 direct 模式表示 R-vector solve 等待，
不表示实际生成了单位向量或 DInv。

#### 实测验收

指定的 `scons build/ARM/gem5.opt -j2` 编译退出码 0，guest `-O2` 编译无告警。
`single-line-base-solve-20261009` 的 20 项均通过，检查器输出
`SINGLE_LINE_BASE_SOLVE_CHECK_PASS`；每个 cold cell 恰好分配、完成、退休一次，
每个前向 handoff 按相邻依赖消费，后向晚于最后一个 y。每池的精确工作量
与公式一致，两种算法的资源 count/latency/II 相同。direct 不产生 DInv ready
或 base ColumnFma 事件。全量数值验证覆盖每次 sweep 的 `10N` 个 y/Δq 标量。

| 单线配置 | inverse 周期 | direct 周期 | 变化 | RHS（inverse/direct） |
| --- | ---: | ---: | ---: | ---: |
| 1 点、window=1 | 2,818 | 2,796 | -0.78% | 5 / 1 |
| 2 点、window=1 | 3,432 | 3,344 | -2.56% | 20 / 12 |
| 17 点、LU/TRSM=2/2 | 8,232 | 7,056 | -14.29% | 245 / 177 |
| 17 点、LU/TRSM=2/4 | 8,180 | 7,076 | -13.50% | 245 / 177 |
| 64 点、LU/TRSM=2/2 | 24,696 | 20,636 | -16.44% | 950 / 694 |
| 17 点、split | 8,278 | 7,102 | -14.21% | 245 / 177 |
| 17 点、rhs-frontier | 8,572 | 7,488 | -12.65% | 245 / 177 |
| 17 点、DMA queue/outstanding=1 | 52,940 | 52,938 | 约 0% | 245 / 177 |
| 17 点、3 次重建累计 | 20,726 | 17,256 | -16.74% | 735 / 531 |
| 17 点、reciprocal | 8,934 | 7,678 | -14.06% | 245 / 177 |

这些是 streaming 计算窗口，包含该窗口的写回与成功完成，排除数值验证与报告。
每组仅使用本轮同一 binary/guest 和匹配配置作比较；benchmark 元数据布局与
旧轮次不同，旧周期不能混作当前 baseline。direct 最大绝对误差为
`1.6653345369377348e-16`。DMA 单槽压力几乎抵消算术收益，不能把减少 RHS
直接当作端到端加速。该负载没有真实 Jacobian 更新或收敛验证。

资源逐项报告保存为
`results/cfd_dsa/campaigns/single-line-base-solve-20261009/resource-audit.json`。
原有 13 项 DMA 回归也通过，检查器输出 `SINGLE_LINE_OUTPUT_DMA_CHECK_PASS`，
证据为 `single-line-output-dma-base-regression-20261009`；6 项单元测试通过。
原有 8 项 RHS 调度回归通过 `SINGLE_LINE_RHS_FRONTIER_CHECK_PASS`，证据为
`single-line-rhs-frontier-base-regression-20261009`。上述三组 campaign 使用
同一 gem5 二进制；逐 RHS 策略仍维持可选实验模式。
该轮 3 项非法配置（当时未支持的复用、未知模式、无 direct 后向）正确拒绝；对检查器注入
数值失配、丢失 RHS、丢失 FU 发射三项错误，也均正确拒绝。
gem5 SHA256 为
`d8550ce7b5c5c2a1937cc0542657cef41f35004341c67406587e2d7dceb55f20`。
复现时须使用新的 campaign 名称，已有 case 结果不可覆盖。

### C：统一计算簇、LU 复用与数值模式

`--coeff-compute-model=shared` 把所有 LU/TRSM 标量运算、列 MVM、parent
MVM 及前/后向最终向量减法映射到三个资源池：

| 物理池 | 通道数 | 默认 latency / II | 使用者 |
| --- | ---: | --- | --- |
| shared_mul | 5 | 3 / 1 | LU、TRSM、5-lane 列乘积 |
| shared_add | 5 | 4 / 1 | LU/TRSM 减法、MVM 累加、前/后向向量减法 |
| shared_div | 1 | 12 / 4 | LU、TRSM 除法或每 cell 的倒数计算 |

标量每次占一个通道，向量原子占五个通道；同周期合计不能超过 5/5/1。
多上下文仍由 `--lu5-count`、`--coeff3-count` 控制，只增加独立状态，
不增加通道。旧 `--lu5-*-count/lat/ii`、`--coeff-mul/sub-*`、ColumnFma
和 forward-combine 参数仅作用于 `legacy`。shared 用
`--coeff-cluster-{mul,add,div}-lat` 和 `--coeff-cluster-div-ii`。
默认计算模型仍为 legacy，复现实验需显式选择 shared。

浮点乘积在乘法完成时取 FP64 结果，加法在单独的加法完成事件中执行。
列 MVM 的乘积信用从五通道乘法发射起，持有到五通道加法完成，包含排队
及在途阶段；`--coeff-cluster-product-depth` 默认 8，禁止超额分配。
就绪的向量反馈工作先获得五通道发射机会，标量预处理使用剩余机会。
仍保留请求窗口、输入槽、LU/TRSM 上下文和退休背压，不把主机计算的瞬时
结果当成同周期可见。该调度策略未包含 RTL 仲裁电路的额外延迟。

三个 MVM 模式使用上述同一资源预算：

- `serial`：一列乘积完成并按原次序相加后，推进下一列。
- `split`：parent MVM 可提前发射独立乘积，五列仍按 0→4 次序相加。
  serial/split 在同算法、同数据下要求 y/Δq 的 FP64 摘要一致。
- `tree`：parent MVM 收齐五列，计算 `((p0+p1)+(p2+p3))+p4`，四次
  五通道加法、三层依赖；第一层的两次加法也竞争同一个资源池。
  每个 MVM 上下文必须容纳五个乘积，配置深度不足直接拒绝。
  该重关联模式采用完整 `1e-10` 绝对/相对误差验证，不要求摘要一致。

parent MVM 包含后向 Ubar、inverse 复用时的 base/Lbar 及提前 base 计算。
cold child 的 Lbar 与 inverse base 保持有序列消费，资源同样进入 shared。
parent 有三个固定 MVM 上下文，乘积分别受 `--coeff-line-product-depth`
限制；它们与全局列 MVM 信用是两个缓冲，不应把容量混为一项。

direct 缓存保存 LU/Lbar/Ubar；inverse 保存 DInv/Lbar/Ubar。缓存键是
`{lineId, cells, coefficientVersion, baseSolveMode}`，dirty 强制重建。
direct 命中仍为每 cell 创建有界 child，读取本次 R、跳过 LU 因式分解与
系数 TRSM，只完成一个 `D*b=R`，再按相邻 handoff 递推。`LU_CACHE_REUSE`
是实际复用事件，不伪造 Lbar/Ubar 的 RHS 完成。inverse 命中保留其 MVM
路径。模式、尺寸或版本不匹配均走重建；缓存不推断系数是否稳定。

单次 cold 的 RHS 分别是 `15N-10` 和 `11N-10`；direct 单次 warm 是 N，
inverse warm 无 TRSM RHS。divide 下每个 RHS 是 5 div、20 mul、20 sub，
每次 cold 的 LU 是每 cell 10 div、30 mul、30 sub。每个有序 MVM 额外
25 mul、25 add；tree MVM 为 25 mul、20 add；每个非边界前/后向 cell
最终向量减法各占 5 add。reciprocal 下每个 child 求 5 个倒数，RHS 改为
25 mul、20 sub。检查器按算法、更新间隔、数值模式精确核对这些工作量。

DMA direct warm 的输入实际为 `40N` B 新 R；cold 边界掩码输入为
`200N+400(N-1)+40N = 640N-400` B。最终输出仍逐 sweep 40N B，并等待
全部响应。缓存数组有效载荷仍是 600N B，parent 是 720N B；两种算法的
host 有效载荷相同，尚无缓存容量/端口/换入换出的物理模型。

`CFD_RESOURCE_STATS` 在 shared 只报告三个物理池，issued 为标量通道运算数，
occupancy_sum 包含 latency 内所有在途通道周期。trace 的
`.cluster.csv` 包含每次发射的 cycle/pool/lanes/caller/complete_cycle，
验收逐周期检查通道和共享 divider II，并核对 `issued*latency` 与占用总和。
旧 request record 的每引擎 busy/idle 字段基于 legacy 池，shared 下不能
用作物理利用率；以三个物理池报告为准。
`CFD_CLUSTER_BUFFER` 报告乘积信用闭合与背压；`CFD_LINE_FEEDBACK` 为每个
非尾端后向 cell 给出完成周期。反馈间隔包含依赖 MVM、最终减法与已发生
的输出背压，区别于 FU 的 II 和整次 sweep 周期。

```bash
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 lusgs-step2
python3 -m unittest discover -s projects/cfd_dsa/tests -v
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/single-line-c-complete.json \
  --campaign single-line-c-complete-20261009
python3 projects/cfd_dsa/tools/check_single_line_c.py \
  results/cfd_dsa/campaigns/single-line-c-complete-20261009 \
  --report results/cfd_dsa/campaigns/single-line-c-complete-20261009/resource-audit.json
```

suite 有 35 项，包括 N=1/2/17/64、window=1、LU/TRSM 2/2 与 2/4、三种
MVM、divide/reciprocal、单次 cold、cold/warm/dirty 三 sweep、七 sweep
复用、乘积信用深度1、DMA 单队列/单 outstanding 和 legacy 数值对照。
另外两项缓存契约 fixture 在每步改变 R，切换 direct/inverse、3→2 cells，
随后改变 D 并公布新版本、同版本 dirty 重建及再次命中。预期命中序列是
`0,1,0,1,0,0,1,0,0,1`，每次均重新计算软件参考。

#### C 完成验收（2026-10-09）

`scons build/ARM/gem5.opt -j2` 退出码 0；guest 使用 `-O2`。
35 项 C 专项全部通过 `SINGLE_LINE_C_COMPLETE_CHECK_PASS`：所有物理发射
和 occupancy 工作量闭合，乘积信用没有泄漏，深度1的 inverse 压力用例
实际触发信用背压；serial/split 和 shared/legacy 的同算法 FP64 摘要一致。
全部数值模式的最大绝对误差为 `2.220446049250313e-16`。
两种缓存契约 fixture 均通过实际 R/系数更新与完整参考校验，命中序列符合
预期。每项 direct warm 恰好 N 个 LU 复用事件和 N 个 R-vector RHS，
没有 DInv ready 或 inverse base 列乘事件。

| shared 单线配置 | inverse 周期 | direct 周期 | direct 变化 | RHS（inverse/direct） |
| --- | ---: | ---: | ---: | ---: |
| cold 1 点 | 2,784 | 2,858 | +2.66% | 5 / 1 |
| cold 2 点、window=1 | 3,380 | 3,456 | +2.25% | 20 / 12 |
| cold 17 点、serial | 8,918 | 7,634 | -14.40% | 245 / 177 |
| cold 17 点、split | 8,726 | 7,442 | -14.71% | 245 / 177 |
| cold 17 点、tree | 8,676 | 7,396 | -14.75% | 245 / 177 |
| cold 64 点、split | 26,438 | 21,320 | -19.36% | 950 / 694 |
| 17 点 cold/warm/dirty、split | 16,768 | 15,862 | -5.40% | 490 / 371 |
| 17 点 cold/warm/dirty、tree | 16,586 | 15,718 | -5.23% | 490 / 371 |
| 17 点一次 cold + 六次 warm、split | 15,694 | 24,314 | +54.93% | 245 / 279 |
| 17 点三 sweep、reciprocal/serial | 13,424 | 15,178 | +13.07% | 490 / 371 |
| 17 点三 sweep、split、DMA queue/outstanding=1 | 107,766 | 112,312 | +4.22% | 490 / 371 |
| 17 点三 sweep、split、LU/TRSM=2/4 | 16,864 | 15,396 | -8.70% | 490 / 371 |

周期为 streaming 计算窗口，包含该窗口写回与成功完成；每对使用相同的
shared 5/5/1 资源和非算法设置。direct 可减少 cold 计算，连续复用较多时
inverse 的 base MVM 优于重新 TRSM；短线、reciprocal 和 DMA 压力下也不能
从 RHS 减少推出整轮加速。不混用 legacy 轮次或不同 guest 布局的周期。
一般三 sweep 仅做生命周期验证，数值系数更新由缓存契约 fixture 单独覆盖。

| shared 后向计算模式（17 点、internal） | 反馈间隔中位数 |
| --- | ---: |
| serial，mul/add=3/4 | 39 周期 |
| split，mul/add=3/4 | 27 周期 |
| tree，mul/add=3/4 | 24 周期 |
| serial，mul/add=7/8 | 83 周期 |

该反馈结果包括最终向量减法。tree 第一层并未占用额外加法资源；算子
latency 增加后，实测反馈随之增加，不能以低 II 替代依赖路径延迟。

另外 20 项旧 base 对照、13 项 DMA 和 8 项 RHS 调度回归均通过相应检查器，
合计 76 项仿真；11 项单元测试和 5 项非法配置拒绝检查通过。
四个 campaign 分别为 `single-line-c-complete-20261009`、
`single-line-base-solve-c-regression-20261009`、
`single-line-output-dma-c-regression-20261009`、
`single-line-rhs-frontier-c-regression-20261009`。
它们全部使用同一 gem5 SHA256：
`1296dc18cd264289ab49d6921757ea6924e28acd0b95c83bf25ea2bb486aebc1`。
主 campaign 的 `resource-audit.json`、`acceptance.json`、`build.log`、
`check-*.log` 和 `negative-config-audit.json` 保存验收账本与输出。
已有 campaign 不可覆盖，重新运行时使用新的名称。

**C 阶段在 gem5 事件周期模型范围内已完成。** FP64 单元面积、功耗、寄存器/转发
网络和仲裁 RTL 尚未综合；资源发射等价不构成已证明的等面积结论。
仍无真实 CFD 残差收敛验证。D2/D3 的有限 y/Ubar 驻留及全数据 timing
生命周期不属于本次 C 的实现范围。

| 阶段 | 单线范围内的下一项工作 | 验收条件 |
| --- | --- | --- |
| A/B | 当前单线 benchmark 的数值基线与请求调度已验收 | 完整 sweep、全量数值一致、每个 cold cell 恰好创建一次 child |
| C | 共享 5/5/1 运算、多上下文、有界有序乘积、树形对照与 LU 复用已验收 | 逐周期资源预算、数值模式、依赖反馈与工作量闭合；面积需另行建模 |
| D1 | 最终 Δq 有界 timing 写回已验收 | 40N 字节、真实响应、完成栅栏和背压 |
| D2 | 已验收：有限 tile、统一容量账本、有界系数缓存、逆序 timing 回读及 inverse 命中 R 读取 | 容量不超限、流量与响应闭合、存储对照 FP64 摘要一致 |
| D3 | 已验收：成功完成路径的 descriptor、终端记录、数据与状态 timing DMA，报告整轮时间 | 必需响应先于成功通知；片上寄存器消费与异常诊断路径边界见 D 文档 |

长链分段仿射扫描、多色和一般网格方案不在当前实施范围内。

2026-10-09 后续 D2/D3、DMA 重叠和模式选择已完成事件模型验收；实现、167 项仿真证据、容量边界与测量结果见 [单线 D 阶段报告](CFD_DSA_SINGLE_LINE_D.md)。整轮接口计时会改变 C 阶段纯计算对照的收益，选择模式时使用新的 D 数据。
