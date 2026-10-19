# 单线第三轮优化（2026-10-15）

范围保持为一条网格线，每轮一次前向、一次后向。以下实现均是 gem5 周期模型，未进行 RTL 综合或物理时序验证。旧基线和历史审计结果保留在原目录。

## 实现

1. `opt>=9`：两个有界后向操作数槽。每点的 y 和五列 Ubar 同时向已有 SPM 仲裁器提交读请求，当前点执行算术时预读相邻下一点。SPM bank、端口、队列和访问延迟仍生效；数值乘加等待真实邻点反馈，没有执行跨点扫描。两个槽共预留1024B。
2. `opt>=10`：前向结果的本地可读状态和外部写回完成分开。y/Ubar 的 SPM 写完成后设置 ready 位，尾点和后向首点就绪即可启动后向。旧 tile 的物理空间仍要等其前向写回响应后才能复用。新增512B元数据预留。最终完成仍等待所有子任务、记录、warm写回、片上访问、最终解写回和状态响应。
3. `opt>=11`：完成记录从320B压至160B，40个既有字段全部保留。13个身份/时间字段用64位，其余27个单点计数/状态字段用16位；编码前检查范围。host和guest分别计算全部40字段的摘要，验收逐项比较。完成队列每项容量预留从1024B降到512B，并有编译期大小检查。
4. 输入和系数写回可使用已有宽度参数设为64B，合并连续的小传输；真实请求仍按缓存行和页边界切分，并等待 timing DMA 响应。
5. RHS 行乘积容量可通过 `--coeff-rhs-product-depth=1..60` 限制。只有声明容量内的槽可以分配，容量账本计入 `64*depth+256` 字节。默认60保持旧行为。
6. 增加物理 DMA queue/send/retry/response trace，区分排队时间和发送至响应时间。端点 Cache 的 tag/data/response 延迟可单独设定，默认仍为50周期；低延迟端点结果只作为敏感性实验。

相关实现：`cfd_coeff_preprocess_backward.inc`、`cfd_coeff_preprocess_early.inc`、`cfd_coeff_preprocess_lifecycle.inc`、`cfd_coeff_preprocess_rhs_rows.inc`、`cfd_packed_record_fields.inc`、`cfd_local_spm.cc`。

## 编译和验收口径

按指定命令编译，当前主机链接内存不足时使用进程内环境变量：

```bash
export LINKFLAGS_EXTRA='-Wl,--no-keep-memory'
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 lusgs-step2
```

`--no-keep-memory`只影响BFD链接器的内存使用，不改变仿真硬件参数。初次默认链接在C++编译成功后被系统以signal9终止，重试成功。

主性能表使用同一 gem5/guest 二进制，固定5路乘法、5路加减法、1路除法，延迟3/4/12、除法II4、每bank 1R/1W、8B交织，DMA端点延迟50/50/50。N=17；cold是一轮，reuse是一次重建加六次系数命中。整轮ROI包含提交和完成通知，控制器窗口包含descriptor首请求至状态响应。两种周期口径不能混用。

位模式一致性用于保持累加顺序的serial/split对照；tree单独按误差验收。所有性能数字均为周期模型结果。资源收缩只表明有界容量需求降低，不直接等同于芯片面积降低。

## 验证中发现并处理的问题

- 提前后向后，warm inverse 不能再依靠“后向开始前写回已结束”的隐含顺序；增加显式 `completedChildren/warmWrites` 终结栅栏。
- guest 的完成记录报告原先跨轮留在stdout缓冲中，会和下一轮异步DMA trace交错；在计时区间外刷新输出，保留严格摘要检查。
- 初步stage10的cold direct回读等待增加。trace表明前向handoff间隔仍是262周期，额外等待集中在后向tile DMA。后续扫描针对tile与DMA重叠，不将其误归因于输入准入。


- 模式切换会在dirty=0时触发硬件缓存失效。guest现在也根据实际完成记录识别这种重建，避免漏核对inverse的3条记录。此前性能扫描的guest二进制和源码已单独保存；最终对照已用补齐检查的同一guest重跑。


## 长线DMA仲裁修正

N=65 cold direct的初步组合中，后向2228周期包含1728周期算术、495周期输出队列满和5周期读启动。发生长气泡时，y和五列Ubar已经完成读取。因此，SPM队列达到32项并不能证明SPM就是该气泡的直接原因；补测48/64项队列也未改善周期。

根因是`processLineTiles`先申请DMA信用，随后最终解写回才申请。连续回读会让最终解队列达到8项并阻塞递推。opt11现在先给已产生的解一次发送机会，再处理tile预取；每周期仍只尝试一个输出包，DMA总并发、队列深度和最终完成栅栏不变。`--coeff-dma-output-priority=0`恢复原顺序，供同二进制、同资源A/B验收。

此后最终性能与压力测试使用新的模型和guest；之前资源扫描保存在`performance-model-v1`与`performance-guest-v1`中，仅作为选参过程，不能和最终表混用为同二进制对照。



<!-- measured-final-start -->
## 最终同版本结果

本节只使用最终模型与最终guest；初步扫描周期不混入下表。cold为一轮，reuse为一轮重建加六轮命中。主表保留原算术和DMA端点延迟。

### N=17

| 场景 | level8整轮 | 最终整轮 | 周期减少 | 控制器窗口 |
| --- | --- | --- | --- | --- |
| cold-direct | 7958 | 7816 | 1.78% | 5487 → 5331 |
| cold-inverse | 9158 | 8966 | 2.10% | 6767 → 6564 |
| reuse-direct | 21610 | 20746 | 4.00% | 17079 → 16175 |
| reuse-inverse | 20008 | 18902 | 5.53% | 16009 → 14937 |

### N=65

| 场景 | level8整轮 | 最终整轮 | 周期减少 | 控制器窗口 |
| --- | --- | --- | --- | --- |
| cold-direct | 22058 | 21818 | 1.09% | 19752 → 19497 |
| cold-inverse | 26938 | 26644 | 1.09% | 24709 → 24406 |
| reuse-direct | 62786 | 57012 | 9.20% | 58421 → 52550 |
| reuse-inverse | 55694 | 53294 | 4.31% | 51898 → 49510 |

### 推荐资源与容量

| 场景 | banks | tile | DMA并发 | SPM队列 | 完成队列 | RHS/共享乘积槽 | 峰值容量B |
| --- | --- | --- | --- | --- | --- | --- | --- |
| cold-direct | 4 | 16 | 32 | 32 | 4 | 16 / 4 | 127488 |
| cold-inverse | 4 | 16 | 32 | 32 | 4 | 16 / 4 | 127488 |
| reuse-direct | 4 | 16 | 32 | 32 | 4 | 16 / 4 | 137688 |
| reuse-inverse | 8 | 16 | 32 | 32 | 4 | 16 / 4 | 137688 |

容量预算为196608B；缩减后N=17的峰值比level8少11776B。N=17的两个tile可覆盖整线，N=65 cold改为tile32，仍有1个tile需要逆序回读，共7680B；warm保留tile16，回读3个tile的y共1920B。cold长线峰值135168B，比旧配置少4096B。二者均经过独立容量/流量账本验收。

选择规则是优先保留接近最低周期的较小资源配置；bank4相对bank8超过1%损失时保留8banks。SPM/DMA队列均有真实容量限制。静态宿主数组的未启用槽不参与分配，容量数字代表模型中的硬件预算。

### 气泡与利用率

| N17场景 | 后向：旧→新 | 新后向算术 | 新后向DMA等候 | 新后向输出背压 | 旧→新全簇空闲比例 |
| --- | --- | --- | --- | --- | --- |
| cold-direct | 537 → 437 | 432 | 0 | 0 | 11.34% → 8.70% |
| cold-inverse | 545 → 437 | 432 | 0 | 0 | 9.86% → 7.07% |
| reuse-direct | 496 → 437 | 432 | 0 | 0 | 23.35% → 18.88% |
| reuse-inverse | 496 → 435 | 432 | 0 | 0 | 26.89% → 21.64% |

| N17场景 | 乘法发射槽利用率 | 加减发射槽利用率 | 除法发射槽利用率 |
| --- | --- | --- | --- |
| cold-direct | 18.20% | 18.80% | 79.16% |
| cold-inverse | 20.22% | 20.70% | 85.01% |
| reuse-direct | 14.45% | 15.84% | 38.70% |
| reuse-inverse | 18.73% | 20.22% | 37.36% |

发射槽利用率按整个控制器窗口计算，除法按II归一化；全簇空闲指没有任何乘、加、除操作在执行。它们不同于资源被上下文持有的比例。reuse行按七轮加权，最后一轮warm inverse的除法利用率为0，因为该路径本来就不执行除法。

### DMA信用仲裁A/B

| N65cold | 原顺序输出背压 | 优先输出背压 | 原顺序后向 | 优先输出后向 |
| --- | --- | --- | --- | --- |
| direct | 495 | 14 | 2228 | 2074 |
| inverse | 401 | 104 | 2169 | 1968 |

这组A/B只改变发送顺序，计算资源、DMA并发、tile、SPM和输出队列容量相同；结果位模式及算术发射次数相同。`check_single_line_dma_priority.py`会拒绝任何额外资源变化。

### 算术时序敏感性

| N17场景 | 固定延迟最终版 | divII2 | addlat2 | 两者同时 |
| --- | --- | --- | --- | --- |
| cold-direct | 7816 | 6164 | 7506 | 5840 |
| cold-inverse | 8966 | 6588 | 8780 | 6330 |
| reuse-direct | 20746 | 19100 | 19196 | 17512 |
| reuse-inverse | 18902 | 16520 | 16684 | 14230 |

这是改变算术资源时序的能力评估，不计入固定延迟优化幅度；目前没有RTL时序/面积证据。II2仍保留12周期除法结果延迟，加法2周期会缩短反馈链。N65同时加速后的回读与输出等候见summary.json的phases；不能只用短线判断DMA是否足够。

### 加速后的长线回读

| N65cold | tile16后向 | tile32后向 | tile16回读等待 | tile32回读等待 |
| --- | --- | --- | --- | --- |
| cold-direct | 1865 | 1594 | 478 | 357 |
| cold-inverse | 1865 | 1594 | 478 | 357 |

### DMA端点敏感性

| 场景 | 50/50/50 | 10/10/10 | 1/1/1 |
| --- | --- | --- | --- |
| cold-direct | 7816 | 7504 | 7442 |
| reuse-inverse | 18902 | 16638 | 16200 |

物理trace区分queue→send与send→response。默认端点没有被调低；无实测目标硬件时，这里的低延迟配置不能称为完成了硬件标定。响应时间还包括相干缓存路径及内存系统，不能全部归为DRAM延迟。

N17 cold direct最终传输21328B、459包，平均46.47B/包；DMA排队均值1.135周期，发送至响应均值99.527周期。记录本身每点320→160B；仍保留descriptor、status和最终输出响应。

最终同一gem5/guest版本共125个独立仿真场景验收通过：主对照18、长线tile补测2、压力25、算术16、端点4、C回归35、D回归25。另有11项单元测试、15种非法配置、9项调度/记录故障和4项SPM故障注入检查通过。新优化场景最大绝对误差为2.22e-16；保持累加顺序的配置跨资源/时序变体结果位模式相同。

## 验收与重现

最终场景清单、二进制/源码哈希、验收日志及原始数据哈希见`results/cfd_dsa/analysis/single-line-round3-20261015/evidence.json`，可运行该目录的`verify.py`检查。没有覆盖旧审计和基线文件。

```bash
python3 projects/cfd_dsa/tools/run_suite.py projects/cfd_dsa/suites/single-line-round3-recommended.json
python3 projects/cfd_dsa/tools/check_single_line_opt.py results/cfd_dsa/campaigns/single-line-round3-recommended-20261015 --suite projects/cfd_dsa/suites/single-line-round3-recommended.json
```

长线配置见`suites/single-line-round3-long-recommended.json`；可选算术能力配置见`suites/single-line-round3-fast-recommended.json`。

对实际冷启动/复用次数选择direct或inverse；本轮没有改变这一边界。仍有的主要限制是单线前向LU/求解反馈、除法吞吐及启动/收尾DMA延迟。继续增加乘法器或无差别扩大队列缺乏本轮证据支持。
