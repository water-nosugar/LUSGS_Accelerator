# 第二轮优化结果

N=17；cold=一轮；reuse=一次重建和六次命中。下表基线与优化版均包含 SPM bank/端口/仲裁与真实 timing DMA，算术簇固定5/5/1、latency=3/4/12、divider II=4。

| 场景 | banked level5 基线 | 固定bank配置 level8 | 重新选参数后 | 较banked基线减少 | 峰值容量 |
| --- | --- | --- | --- | --- | --- |
| cold-direct | 8788 | 8488 | 8190 | 6.80% | 139264 / 196608 B |
| cold-inverse | 9722 | 9546 | 9230 | 5.06% | 139264 / 196608 B |
| reuse-direct | 24676 | 23292 | 21682 | 12.13% | 149464 / 196608 B |
| reuse-inverse | 21452 | 20938 | 19950 | 7.00% | 149464 / 196608 B |

固定bank配置为4banks、1R/1W、粒度8B、DMA16、tile4；选参允许bank与DMA资源变化，不代表面积相同。

| 场景 | bank数 | tile | DMA并发 | SPM队列 | 实测case |
| --- | --- | --- | --- | --- | --- |
| cold-direct | 4 | 8 | 32 | 64 | cold-direct-b4-t8-q64 |
| cold-inverse | 8 | 8 | 32 | 64 | cold-inverse-b8-t8-q64 |
| reuse-direct | 8 | 8 | 32 | 64 | reuse-direct-b8-t8-q64 |
| reuse-inverse | 8 | 8 | 32 | 64 | reuse-inverse-b8-t8-q64 |

## 消融：原有寄存器操作数假设与新SPM模型分开统计

| 级别 | cold direct | cold inverse | reuse direct | reuse inverse |
| --- | --- | --- | --- | --- |
| 4 | 8686 | 9662 | 23830 | 23152 |
| 5 | 8692 | 9666 | 23704 | 20012 |
| 6 | 8480 | 9526 | 22896 | 19548 |
| 7 | 8446 | 9502 | 22614 | 19528 |
| 8 | 8488 | 9546 | 23292 | 20938 |

4→7 保持原数据操作数假设，展示生命周期、tile留存和LU组执行的收益；8增加banked SPM访问约束，新增延迟与容量不得隐去。

## 改变硬件吞吐/延迟的独立诊断

| case | 整轮周期 | 最大绝对误差 |
| --- | --- | --- |
| cold-direct-ii2 | 8580 | 5.551115123125783e-17 |
| cold-inverse-ii2 | 7958 | 8.326672684688674e-17 |
| reuse-inverse-ii2 | 19780 | 8.326672684688674e-17 |
| reuse-inverse-add2 | 18948 | 8.326672684688674e-17 |
| reuse-inverse-add2-tile8 | 18586 | 8.326672684688674e-17 |
| reuse-direct-ii2 | 23372 | 5.551115123125783e-17 |
| reuse-direct-add2 | 22372 | 5.551115123125783e-17 |
| reuse-direct-add2-tile8 | 21358 | 5.551115123125783e-17 |
| cold-inverse-reciprocal | 8412 | 8.326672684688674e-17 |

改变divider II或加法延迟需要实际实现/综合证据。这些参数不属于固定延迟推荐配置的免费收益。
