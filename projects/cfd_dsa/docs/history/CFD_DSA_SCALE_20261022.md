# 扩大单线点数与系统内存测试

65 点用于和旧合成输入做同长度对照，不是模型上限。本阶段将点数扩大到 256、512、1024、1596、2048、4096，使用同一份 gem5 和 guest 二进制，测量一次冷启动前向和后向 SGS。逐例结果、失败、内存流量与等待见 [实测报告](../../../../results/cfd_dsa/analysis/scale-20261022/REPORT.md)。

## 输入与规模

| 输入 | 点数 | 含义 |
|---|---|---|
| GT01R 连通块路径 | 256、512 | 沿原始双向非零块连接，不重复节点，沿用上一阶段规则 |
| GT01R 原始编号的带状投影 | 512、1024、1596 | 保留原 D/R 和编号相邻的 L/U，缺失块为零；其他耦合省略 |
| 合成规模压力输入 | 256、512、1024、2048、4096 | 扩大原合成规则，检验容量、数据搬运和规模增长 |

GT01R 来自 [SuiteSparse 官方数据页](https://sparse.tamu.edu/Fluorem/GT01R)，只有 1596 个五变量节点。1596 点投影使用所有原始节点的 D/R，但仍省略大量原始非相邻耦合，不是完整 GT01R 求解。带状投影也不假定原始编号是某条连续几何网格线。2048/4096 点明确属于合成压力测试，没有复制或拼接 GT01R 节点来冒充真实大网格。

五个真实来源输入逐项核验原始矩阵块和 RHS，没有缩放、对角平移或更换 RHS。每点的独立参考用 200 位 Decimal 生成，并用 240 位交叉复核，保留原来的 `1e-10` 数值容限。数值失败记录保留，不列为通过的性能样本。

## 内存如何承载几千点

每点 D/L/U/R 为 `3×25×8 + 5×8 = 640 B`。4096 点逻辑输入为 **2.5 MiB**。实际 DMA 省去首 L 与尾 U 共 400 B。

片上容量仍为 **192 KiB、4 banks**。本批每例只运行一次，系数复用缓存不分配；状态预约为 136448 B，两个 16 点 Tile 共 7680 B，合计 **144128 B，140.75 KiB**。增加线长不会在设备内分配一个同长度的大数组。

DMA 按窗口将输入搬入有限槽位；前向产生的 y/Ubar 发布到系统内存地址空间。后向保留最近两个 Tile，轮换回读较早区段，最终 dq 通过输出 DMA 写回。结果验收检查实际请求数、全部点的 y/dq、每个 bank 的读写端口、Tile 回读、DMA 响应和完成顺序。

外部系统内存设为 **128 MiB**，容纳扩大后的 guest 数组。它与设备 192 KiB 的 SPM 预算不同。沿用的外存模型为 `SimpleMemory`，延迟 30 ns、带宽 32 GB/s，保留 CPU 缓存和一致性 DMA；尚不是包含行激活、刷新及存储控制器调度的详细 DDR 模型。

DMA 逻辑读写流量与实际 DRAM 流量不同：缓存命中可能减少外存访问，而缓存行填充及写分配可能增加实际外存读。报告按请求者和统计区间说明口径，不把两类字节混为一谈。

本单线模式的 banked SPM 以 `CFD_ONCHIP`、`CFD_ONCHIP_BANK`、`CFD_LINE_STORAGE` 验收。`system.spm` 属于兼容 ISA 的另一映射窗口，不能用其流量为零或通用性能打印的提示来判断本模式没有片上访问。

## 复现

在 `/home/zyy/gem5` 执行：

```bash
python3 projects/cfd_dsa/tools/prepare_large_line_suite.py \
  --archive results/cfd_dsa/datasets/Fluorem/GT01R.tar.gz \
  --dataset-dir results/cfd_dsa/datasets/Fluorem/single-line-scale-v1 \
  --output projects/cfd_dsa/suites/single-line-scale.json

python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/single-line-scale.json \
  --campaign single-line-scale-my-run
```

[套件](../../suites/single-line-scale.json) 计划运行十例，每例执行一次 SGS；没有外层 CFD 收敛循环。它包含数值压力例，整体 runner 返回非零时应检查逐例 manifest，不能将其他通过例一并判为失败。已有运行目录不可覆盖，复测须使用新 campaign 名。

增大 `--lusgs-cells` 时，输入文件也必须具有对应点数。不能将 65 点文件改一个命令行参数就用于 1024 点；配置端和 guest 都会拒绝维度不一致的文件。生成器会实际生成新的全部 D/L/U/R 及参考输出。

实现：[prepare_large_line_suite.py](../../tools/prepare_large_line_suite.py)。原数据格式和初次接入见 [GT01R 文档](../CFD_DSA_GT01R_20261022.md)。
