# 后向就绪拆分与按 cell 回读（2026-10-26）

本轮在共享5个MUL、5个ADD/SUB、1个DIV和4个SPM bank上优化控制与数据供给。两个新开关分别控制数据依赖和回读顺序，兼容默认值均为0。没有新增算术单元、Tile缓冲、操作数槽或RF端口。

完整周期、独立数据、资源比较和验证清单集中在[实验报告](/home/zyy/gem5/results/cfd_dsa/analysis/ready-supply-opt-20261025/RESULTS.md)。该实验目录从10月25日开始建立，构建与后续测试在26日完成。

## 独立就绪

`--coeff-back-independent-ready=1` 要求共享计算、片上时序、Tile、已有后向操作数槽和后向有符号归约树。

对后向计算 `dq[row] = y[row] - sum(Ubar[row,col] * dq_next[col])`，改为：

- 绑定一个有效reload范围，不代表其中任何数据已就绪；Resident/Forward仍遵守原发布条件。
- Ubar列读取等待该列全部5个word的真实SPM写入响应；y读取单独等待全部5个y word写入。
- 列乘积只等待Ubar列的SPM→RF读响应和已有邻点向量。
- 有符号树的`y-p0`节点继续等待y的SPM→RF读响应；五行模式还将y到达时间纳入注册选择时序。
- 每个槽必须完成y和全部5列读取、列消费及y发布后才能重用。所有最终写回与可见完成屏障保留。

复用现有`usedColumns`字节的第5位记录y消费；低5位仍记录列消费。不增加持久数据槽。该改动尚未实现跨cell的分量转发，下一cell仍等待完整邻点结果。

`CFD_BACK_OPERAND event=y_use`将y发布与真实读任务关联。独立检查器分别验证矩阵读取、y读取、乘积发射、实际减法发射和槽回收，统计真正早于y读响应的乘积列；允许提前绑定范围，不允许提前读取未返回的数据。

## 按cell覆盖安排回读

`--coeff-tile-cell-order=1` 要求片上时序、level>=10、`--coeff-tile-reload=reverse-stream`和`--coeff-tile-operand-stream=1`。

长线的每点回读包含40 B y和200 B Ubar。原按包交替会使y的发出覆盖范围推进得更快。新策略复用两个已有issued计数：当`5*y_issued > u_issued`时优先Ubar，否则优先y；一个区域发完则处理另一区域。最近待消费Tile的优先级保留。

选择器是两个计数的常数倍比较，不新增计数器、数据缓存或浮点运算。首选区域不能获得传输额度时，另一区域可尝试剩余额度。每周期发包数、字节数、DMA在途数、cache MSHR、bank端口与SPM队列限制全部继续生效。

完整系数缓存持有Ubar时，Tile只保存y，选择器不产生额外Ubar回读。bitmap容量不足而回退到整Tile回读时，也不启用按cell选择。

原生模型的16点、64 B上限示例中，两个顺序都发出60个包；第一个cell的必需数据由第8个包提前至第5个包。这是相同返回延迟的发出顺序示例，实际缓存行切分、DDR排队和SPM服务决定gem5中的到达时刻。

`CFD_TILE_ORDER`把每个实际发出的DMA包与计数快照、首选区域和第二次尝试关联。验收检查覆盖真实数据包、真实word响应与有限缓冲生命周期，不以选择提示代替数据就绪。

## 资源与实验决策

256 KiB主对照为LU4/RHS4、RF16R8W、4-bank各1R1W、Tile16、两个原Tile加四个逆向预取缓冲、两个后向操作数槽、64槽输出FIFO。长线总预约237,696 B；两个开关均不改变该账本。

另验证固定192 KiB配置的33点功能路径：输入4、窗口9、片上队列96，预约196,224 B，余量384 B。这是另一静态资源配置，不能把它与256 KiB性能结果混报。

新增控制比较、选择和扇出的物理面积与时序尚未综合标定；当前假设其处于已有控制选择阶段。预约容量不等于物理SRAM面积，gem5中的周期改善也不证明目标频率已实现。

训练1024点四组结果分别为162,670（原方案）、162,436（仅独立就绪）、162,377（仅按cell回读）、162,633（两项结合）。独立1024点分别为161,008、161,008、160,566、160,687。两项收益不能简单相加，默认开关保持关闭；按cell回读被预先选为4096点主对照候选。最终长线结论及使用选择见实验报告。

4096点实际为750,893（原方案）、751,286（按cell回读）、751,389（独立就绪）。因此长线保留原配置；两个开关在当前模型中不构成通用提速方案。原有预变换计算图及FP64运算顺序保持，本轮没有实现直接块求解的A/B/C/D算法切换。

## 使用与复现

在原有限资源参数后显式添加候选开关：

```text
--coeff-back-independent-ready=0
--coeff-tile-cell-order=1
```

完整四组1024点消融及短线回归：

```bash
python3 -B projects/cfd_dsa/tools/run_suite.py \
  results/cfd_dsa/analysis/ready-supply-opt-20261025/phase2-suite.json \
  --campaign <新的实验名称>
```

4096点复现使用完整CSV无损流式压缩与分进程标准验收，避免保留多份展开日志和叠加验收器内存。每次使用新campaign和freeze名称：

```bash
python3 -B results/cfd_dsa/analysis/ready-supply-opt-20261025/run_frozen.py \
  results/cfd_dsa/analysis/ready-supply-opt-20261025/large-suite.json \
  --isolated --campaign <新的实验名称> --freeze <新的冻结清单.json>
```

该传输方式保存完整未过滤CSV，核对压缩前后内容SHA，并执行存储、递推、私有LU、独立数值、配置审计和结果统计六个阶段。不能将仅仿真成功的中间manifest当作最终验收。
