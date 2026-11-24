本次改动让完整系数缓存中的 Ubar 同时服务本轮和后续轮次的后向计算。完整缓存启用且容量足够时，Tile A/B 只分配 y 数组，不再分配 Ubar 数组。bank 数、交织粒度、读写端口、算术资源和 192 KiB 总预算沿用每个对照场景的原配置。

分配时先检查“状态预约 + 两个 y Tile + 整线完整系数缓存”能否放下，再决定缓存类型，避免先分配重复的 Tile Ubar 空间而错误拒绝完整缓存。配置脚本、模拟器和验收器使用相同的分配规则。缓存未启用、超过缓存点数上限或完整缓存放不下时，继续使用有界的 y/Ubar Tile；adaptive direct 可以只缓存 LU、Lbar。这里的优化没有强制为所有线长增加缓存容量。

以两个 16 点 Tile 为例，原先预约 `2 × 16 × (40 + 200) = 7,680 B`，现在预约 `2 × 16 × 40 = 1,280 B`，释放 6,400 B，即 6.25 KiB。Tile 预约减少 83.3%；控制、请求和队列预约保持原口径。65 点、4 输入槽 direct 配置的总预约由 183,128 B 降为 176,728 B。两个 32 点 Tile 则释放 12,800 B。

冷轮中，Ubar 的五列 RHS 全部计算完成后，控制器发起 25 个 FP64 数据的缓存写入，不等待 y 的后续计算。写入仍经过原来的 bank/端口仲裁和访问延迟。完成后发布 `CFD_UBAR_CACHE_READY`；相应点的 y 写入也完成后，才发布本地点就绪。后向操作数预取从系数缓存的第三个矩阵区域读取 Ubar，从 Tile 读取 y。末点不需要 Ubar 参与递推，因此末点 y 可以先就绪，其未使用的 Ubar 槽随后完成写入。

每点 Ubar 就绪与整线缓存有效是两层状态：前者允许本轮后向使用已写完的数据；后者在全部前向子请求成功提交后才发布，用于后续轮次的版本复用。现有的整线互斥、版本/模式/长度检查以及异常时缓存失效机制继续约束缓存生命周期。LU/DInv、Lbar 的存储与 Ubar 分开提交，复用原来的三个片上提交上下文，未新增无界数组或队列。

冷轮现有的 Ubar 对外输出接口仍完成写回，以保留 benchmark 可观察结果。此次减少的 DMA 流量来自回读旧 Tile 时省掉 Ubar；片上写入量还会减少 Tile 中的重复写入。`CFD_LINE_STORAGE.write_*` 记录前向对外输出，包含该接口的 Ubar，不等同于 Tile 的片上写入量；后者由 `CFD_ONCHIP_BANK` 的写词数统计。DMA 字节是有效请求字节，不是物理 DRAM 总线流量。

容量边界用例同时暴露了提前后向与较晚完成的前向写回之间的缓冲区归属问题：当尾 Tile 含多个点，当前 Tile 可以切换到更新的区段，而前一个 Tile 仍有子请求等待 DMA 完成。原提交判断只检查 cell 是否超过当前 Tile 上界，遗漏了低于下界的情况。本次补齐下界检查，仍要求目标 Tile 为 Forward 状态并覆盖该 cell。98 点失败原始日志保留在 `results/cfd_dsa/campaigns/single-line-ubar-cache-after-20261020/n98-capacity-direct`，最终版本另用新目录统一重跑；18、34 点场景补充覆盖这种尾部形状。

对照中的冷轮使用一次 sweep、`--coefficient-update-interval=2`，显式启用已有的系数缓存描述符标志。普通只跑一次且没有启用系数缓存的场景继续使用原路径。每个性能配对的命令参数完全相同；direct/inverse 各自沿用自己的 bank 配置，只比较同一行的修改前后。所有对照共用同一 guest 二进制，算术发射数量与逐轮 FP64 结果哈希由比较工具核对。

最终性能配对结果如下。“总周期”使用 benchmark 的软件计数口径；多轮场景为各轮累计，不包含初始化和参考数值检查。下面每行只比较本行修改前后。

| 场景 | 修改前总周期 | 修改后总周期 | 周期减少 |
|---|---:|---:|---:|
| 17 点 direct，冷轮，双缓冲 | 7,894 | 7,894 | 0.00% |
| 17 点 inverse，冷轮，双缓冲 | 8,950 | 8,950 | 0.00% |
| 65 点 direct，冷轮，双缓冲 | 21,836 | 21,800 | 0.16% |
| 65 点 inverse，冷轮，双缓冲 | 26,642 | 26,600 | 0.16% |
| 17 点 direct，单缓冲，2 轮 | 11,794 | 11,542 | 2.14% |
| 65 点 direct，1 次冷轮 + 6 次复用 | 55,494 | 55,384 | 0.20% |
| 65 点 inverse，1 次冷轮 + 6 次复用 | 53,428 | 53,288 | 0.26% |
| 98 点 direct，冷轮 + 复用轮 | 40,410 | 39,482 | 2.30% |
| 116 点 direct，仍为两矩阵缓存，2 轮 | 47,014 | 47,014 | 0.00% |

65 点 direct 七轮的容量和访问量配对：

| 指标 | 修改前 | 修改后 |
|---|---:|---:|
| 两个 16 点 Tile 的预约 | 7,680 B | 1,280 B |
| 状态、Tile、系数缓存总预约峰值 | 183,128 B | 176,728 B |
| Tile 回读有效字节 | 23,040 B | 13,440 B |
| 全部 DMA 有效读字节 | 80,960 B | 71,360 B |
| 全部 DMA 有效写字节 | 135,056 B | 135,056 B |
| 片上 8 B 写词数 | 18,730 | 15,930 |
| 片上 8 B 读词数 | 39,925 | 39,925 |

该场景原本在复用轮就已经直接读取缓存 Ubar，新增的流量减少主要发生在冷轮。Tile 回读少了 41.7%，片上写词少了 15.0%，总周期只减少 0.20%。这表明资源节省没有等比例转化为时延收益；算术工作量和单线递推依赖仍在，原有存储访问也可与计算重叠。双缓冲 17 点冷轮没有旧 Tile 回读，本次只减少冗余存储工作，实测总周期不变。单缓冲 17 点需要回读旧 Tile，删除 Ubar 回读后两轮周期减少 2.14%。

更明显的收益在容量边界。固定 136,448 B 状态预约、两个 16 点 Tile、192 KiB 总预算和 128 点缓存上限时，完整缓存可容纳的最大线长由按旧分配公式计算的 87 点增加到 98 点。98 点此前只缓存 LU、Lbar，合计 39,200 B；现在能够保存三矩阵完整缓存 58,800 B。总预约峰值由 183,328 B 增至 196,528 B，仍低于 196,608 B 上限，剩余 80 B。这里利用了原来剩余的 13,280 B 加上释放的 6,400 B，而不是减少了整线系数自身的数据量。

98 点复用轮的控制器窗口由 8,314 缩短为 7,473 周期，减少 10.12%；两轮软件总周期由 40,410 降为 39,482，减少 2.30%。两轮 DMA 有效读字节由 124,360 B 降为 72,960 B。99 点及 116 点在同组资源参数下仍无法容纳完整缓存，继续采用两矩阵缓存与 y/Ubar Tile，控制器窗口没有变化。不能将 98 点的收益推广为任意线长都能获得相同比例的提升。

最终构建成功，30 项单元测试通过，最终二进制上的 21 个配对场景和 19 个扩展回归场景全部通过资源及数值验收。扩展场景包含 opt 0/2/8/9/10、单点/两点小队列、257 点无完整缓存、serial/tree 运算模式、非 tiled 异常恢复，以及 18/34 点较晚提交回归。21 组配对的逐轮 FP64 结果哈希一致；正常完成场景的算术发射数量一致，异常恢复中的取消工作量允许随调度进度变化。40 个最终场景共用同一模拟源码快照、gem5 二进制和 guest 二进制，并已核对当前工作区源码/二进制与最终记录一致。最终集合的最大绝对误差为 4.44×10⁻¹⁶，最大归一化残差为 3.18×10⁻¹⁶。

完整指标与证据摘要分别见 [report.json](/home/zyy/gem5/results/cfd_dsa/analysis/ubar-cache-20261020/report.json) 和 [verification.json](/home/zyy/gem5/results/cfd_dsa/analysis/ubar-cache-20261020/verification.json)。模拟器分配与提交位于 [cfd_coeff_preprocess_tiles.inc](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_tiles.inc)，物理访问就绪控制位于 [cfd_coeff_preprocess_onchip.inc](/home/zyy/gem5/src/arch/arm/cfd_coeff_preprocess_onchip.inc)。

可复现命令（结果目录不可覆盖，重新执行时需选用新 campaign 名称）：

```bash
cd /home/zyy/gem5
LINKFLAGS_EXTRA=-Wl,--no-keep-memory scons build/ARM/gem5.opt -j4
python3 -m unittest discover -s projects/cfd_dsa/tests -p 'test_*.py'
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/history/single-line-ubar-cache.json --campaign NEW_MAIN_CAMPAIGN
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/single-line-ubar-cache-regression.json --campaign NEW_REGRESSION_CAMPAIGN
python3 projects/cfd_dsa/tools/history/analyze_ubar_cache.py results/cfd_dsa/campaigns/single-line-ubar-cache-before-20261020 results/cfd_dsa/campaigns/NEW_MAIN_CAMPAIGN --report results/cfd_dsa/analysis/ubar-cache-20261020/report.json
```
