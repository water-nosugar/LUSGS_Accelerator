# 单线 D2/D3：容量、接口计时与重叠

研究范围为一条网格线、每轮一次前向和一次后向。本文描述 gem5 事件周期模型；浮点资源采用同一个 5 mul / 5 add-sub / 1 div 计算簇。所有实验均使用完整 y、Δq 数值验证。D2/D3、重叠与模式选择均已完成本模型范围内的验收；结果与证据见文末。

## 容量与生命周期

`--coeff-line-storage=tiled` 启用有限存储路径，`full` 保留原对照路径。每个 tile 仅保存 y 与 Ubar，预算为每点 240 B，末点未使用的 Ubar 仍保留一个保守槽位。长线不再在 parent 中分配整线 y、Δq、R 或系数副本；后向边界只携带一个 Δq，前向沿用一个 y handoff。

容量检查使用统一账本：

```
request_capacity = input_slots + lu_pending_depth + solve_pending_depth + output_depth
reserved = 8192 * request_capacity + 16384
resident = 240 * min(N, tile_cells) * tile_buffers
cache = cache_enabled ? 600 * N : 0
reserved + resident + cache <= spm_bytes
```

默认 request_capacity=10，固定预留 98,304 B。每个 8 KiB 请求槽包含计算寄存器和终端记录的 DMA 快照，C++ 编译断言检查两者大小；16 KiB 为控制、MVM 上下文、handoff 和输出/接口暂存的保守预留。这是容量模型与保守资源预约，不能作为 RTL 面积估计。1 KiB coherent IOCache 是独立的 DMA 接口缓存，不计入 SPM 账本，实验之间保持相同参数。

前向 child 使用真实 timing DMA 发布 y/Ubar，必须等数据和终端记录响应后才能释放槽位、提交 tile、复用缓冲。当前 guest ABI 要求 y/Ubar 供数值验证读取，因此即使整线可驻留，也保留这些必需写回；“全部可驻留”消除的是后向 reload。被替换的 tile 内容清零，后向必须按逆序从 DRAM backing 回读，不能使用隐藏的整线副本。回读字节数为 `240 * floor((N-1)/T) * T`。

系数缓存默认关闭，`--coeff-line-cache-cells` 指定上限。缓存采用整线可容纳策略，只有 N 不超过上限且统一预算容纳 `600*N` 时启用；其他情况逐轮重建。仅保留一个条目，替换前释放旧条目，按 line/size/version/mode/dirty 校验。direct 保存 LU/Lbar/Ubar，inverse 保存 DInv/Lbar/Ubar。缓存与 tile 共享预算，父任务不再重复复制缓存。缓存命中仍重新读取 R；inverse 使用有界 R DMA 缓冲和上一点 y，direct 使用现有有界 RHS 子任务。

## 整轮计时

`--coeff-interface-timing=1` 将 parent descriptor 读取、需要的终端 child record 写回和 8 B Complete 状态写回接入真实 timing DMA。descriptor 的全部响应到达后才解释任务；child 记录的全部响应到达后才退休；全部结果、记录与状态响应到达后 token 才能返回 Complete。guest 在状态已可见但 token 仍 Busy 时继续等待，避免把可见时刻当成响应完成时刻。

整轮指标为 guest `streamingStep2.totalCycles`，覆盖提交、计算、数据传输和完成等待。数值验证、摘要与诊断聚合在窗口之外；诊断 record 的必需传输在窗口之内。direct warm 仍有逐点 RHS 子任务和完整记录，inverse warm 不创建这些子任务；因此测量结果包含当前 ABI 的接口成本，不能解释为纯算术周期差异。异常/cancel 注入使用原诊断路径，D3 验收针对上述成功完成路径。

驻留计算操作数沿用事件模型的寄存器/直接消费语义；当前容量模型不声称刻画全部片上 SRAM 读端口、布线或 bank 冲突。已有 SPM port/bank 统计适用于其原有 packet 路径，DMA 路径与寄存器读取不能用这些统计推算新的 bank 收益。

## 有界重叠与传输合并

`--coeff-line-tile-buffers=1|2` 控制 tile 缓冲数。两缓冲时前向可以提前进入下一 tile 的输入请求，仍受 window、请求容量和 DMA outstanding 限制；只有当前 tile 已提交，缓冲才可轮换。后向在计算当前 tile 时预取前一个待消费 tile，只在当前点完成且下一 tile 全部读响应到达后切换。旧缓冲不会在 DMA 尚未完成时覆盖。

`--coeff-line-tile-dma-width=64` 将连续 y/Ubar 的回读合并至 cache line 大小，仍在 cache line/page 边界截断。默认 40 保留基线。接口 record 已使用独立的 `--coeff-interface-width=64` 合并。前向逐点发布与最终 Δq 写回仍保留原 ABI 粒度；不宣称已合并所有 DMA。

`CFD_LINE_STORAGE` 报告容量、响应闭合、数据等待和 tile 停顿；`CFD_TILE_EVENT`/`CFD_TILE_DMA` 证明预取、回读与使用顺序；`CFD_INTERFACE_DMA`/`CFD_CACHE_DMA` 区分接口和命中时的新 R/数据流。共享簇 trace 记录算术的发射与完成，输出背压由 `CFD_LINE_SCHEDULE` 报告。离线分析检查 DMA 与算术区间确实重叠，而不只检查开关是否打开。

## 可复现入口

```bash
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 lusgs-step2
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/history/single-line-d-storage.json --campaign single-line-d-storage-verified-20261009
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/history/single-line-d-optimization.json --campaign single-line-d-optimization-accepted-20261009
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/history/single-line-d-mode-selection.json --campaign single-line-d-mode-accepted-20261009
```

`check_single_line_storage.py --suite <suite>` 校验资源工作量、容量峰值、tile 顺序、所有 DMA 字节/包响应、整轮完成门槛和完整数值结果。存储对照按 FP64 摘要逐位比较；tree 的重排单独评估误差。`check_single_line_storage_faults.py <case>` 用损坏的容量、响应、完成时刻、缓存账本及重复终端事件验证验收器能拒绝错误。

`analyze_single_line_d.py` 比较相同预算下的 direct/inverse，输出实际测量的选择表。推荐只适用于报告中精确匹配的线长、tile、更新间隔和 window，不外推未知系数稳定性。输入的更新间隔必须由算法明确发布。

长线七轮扫描使用 10,000,000,000 tick 的仿真上限，防止把被截断的执行当作完整结果。执行器现在要求显式 `LUSGS_STEP2_STREAMING_PASS` 或 `LUSGS_CACHE_CONTRACT_PASS` 标记；返回码 0 和非空 stats 不能单独证明成功。已完整结束的案例保留原始 manifest，只重跑被截断/未运行的案例，来源记录在 `provenance.json`；仿真上限不参与计算资源或 SPM 预算比较。

## 2026-10-09 验收结果

按用户指定的 `scons build/ARM/gem5.opt -j2` 编译成功。167 项套件仿真使用同一 gem5 `7fd970351597f5b02280a5fa62dbaf11005b5684778288bd4f9f66f12e5f797c` 和 guest `ddd2e2b66a0e42ab109d2f5b6fa64a59d3fd0f5a58dde0e09d8d99b408d492c4`。完整数值检查的最大绝对误差为 `2.220446049250313e-16`；所有有限存储案例都满足容量限制，已测试最高预约占用 144,984 B，低于相应 196,608 B 预算。容量不足的 65 点案例禁用缓存，实际每轮重建，guest 的次数验收使用有效更新间隔 1。

| 套件 | 案例数 | 主要证据 |
| --- | ---: | --- |
| D 存储 | 24 | full/tiled FP64 摘要一致、容量及逆序回读 |
| D 缓存与重叠 | 25 | 版本/dirty/模式/尺寸与新 R、响应闭合、双缓冲、DMA 压力 |
| D 模式选择 | 26 | N=17/65，T=4/16，K=1/2/7，window=8/1 |
| D 参数调整 | 10 | outstanding=8/16、上下文=1/2、T=4/8/16 |
| 调整后模式选择 | 6 | outstanding=16，七轮中的 K=1/2/7 |
| 原 C/base/DMA/RHS 回归 | 76 | 共享资源工作量、LU 复用、最终输出与调度 |

另有 11 项单元测试、6 项损坏 trace 拒收测试、7 项非法配置拒收测试，以及 1 项仿真中途截断拒收测试。终端记录 DMA 延迟期间不得将 Complete 回退为 DrainPending；验收器现在检查每个 child 恰好一次完成/退休，并验证终端记录先响应再释放。

17 点、4 点 tile、冷启动，计算资源保持共享 5/5/1，SPM 预算为 131,072 B：

| 配置 | inverse 周期 | direct 周期 | 后向回读包数 |
| --- | ---: | ---: | ---: |
| 单缓冲，40 B，outstanding=4 | 33,092 | 33,400 | 124 |
| 双缓冲，64 B，outstanding=4 | 31,822 | 31,936 | 66 |
| 双缓冲，64 B，outstanding=8 | 18,016 | 18,076 | 66 |
| 双缓冲，64 B，outstanding=16 | 16,894 | 15,340 | 66 |

相对首行，末行整轮时间减少 48.9%/54.1%。增加 DMA outstanding 改变的是接口并发预算；计算簇相同、容量账本仍闭合，不构成等面积结论。双缓冲确实产生前向输入预取及后向 DMA/计算重叠，但 outstanding=4 时会带来输出背压，inverse 的单缓冲 64 B 配置反而略快。上下文 1/1 在本次 cold 对照中较 2/2 慢；T=8/16 的收益依赖 DMA 与反馈链，不能统一增大 tile。

调整后（17 点、T=4、双缓冲、64 B、outstanding=16、2/2 上下文），七轮完整计时的模式选择为：

| 系数更新间隔 K | inverse 累计周期 | direct 累计周期 | 实测选择 |
| --- | ---: | ---: | --- |
| 1：每轮重建 | 105,976 | 94,064 | direct，减少 11.2% |
| 2：每两轮重建 | 82,558 | 84,668 | inverse |
| 7：一次重建后复用六次 | 58,550 | 76,358 | inverse；direct 增加 30.4% |

该表使用相同 196,608 B SPM 预算，实际预约 110,424 B。模式差异包含当前 guest ABI 的记录传输成本；不应拿它替代纯计算对照，也不外推到未测试的硬件/CFD 系数更新规律。

最终证据位于 `results/cfd_dsa/campaigns/single-line-d-delivery-20261009/`：`acceptance.json` 保存整体验收和源文件摘要，`analysis.json` 保存 16 组模式对比和参数分析，`build.log`、`check-*.log` 保存编译及验收输出。完整案例及来源保留在各 campaign；复现请使用新的 campaign 名称，工具不覆盖已有结果。

推荐工具只匹配已测配置。例如选择七轮中复用六次的 17 点模式：

```bash
python3 projects/cfd_dsa/tools/analyze_single_line_d.py \
  --mode-report results/cfd_dsa/campaigns/single-line-d-mode-accepted-20261009/acceptance.json \
  --tuned-mode-report results/cfd_dsa/campaigns/single-line-d-mode-tuned-accepted-20261009/acceptance.json \
  --cells 17 --tile 4 --update-interval 7 --outstanding 16
```

单线成功完成路径的容量、外部 DMA 与接口生命周期已闭合。后续论文仍需独立补充真实 CFD 收敛实验、片上 bank/端口与缓存写入带宽模型，以及 RTL 面积/功耗证据；本次不声称这些内容已完成。
