# 列流式 RHS 求解：除法瓶颈优化（2026-10-20）

这次修改接在“完整系数缓存保存 Ubar、Tile 仅保存 y”的实现之后。没有改变 SPM 容量、bank 数、交织粒度、端口数和 Ubar 的存放位置。

## 瓶颈与改动

原来每个 RHS 都对 5 个 LU 对角元分别做除法。一个冷请求通常同时求解多个 RHS，因而同一个对角元会被反复送入共享除法器。该模型只有 1 个除法器，延迟 12 周期，发射间隔 4 周期。在 65 点、7 轮 direct 用例中，冷轮总计发射 4,175 个共享除法操作，控制器窗口为 19,335 周期。

现在 `--coeff3-div-mode=reciprocal` 对**多 RHS 请求**先计算 5 个 LU 对角元的倒数，随后用乘法处理各 RHS；对**单 RHS 请求**直接除法。后者包括系数缓存命中的 warm direct 请求和单点 direct 边界。这样 warm 轮不会为没有复用对象的倒数再付出乘法成本。65 点 cold 轮的共享除法发射数降至 975；6 个 warm 轮仍分别为 325 次除法，且窗口周期与原方案逐轮相同。

单线 profile 在优化级别 `>=2` 时默认选择 reciprocal；级别 0 默认保留 divide。命令行显式指定 `--coeff3-div-mode=divide` 或 `reciprocal` 始终优先。旧的 level 0 direct 路径若显式选择 reciprocal，在这组 17 点用例中慢 3.7%，所以不把它设为该级别的默认值。

对照试验还测试了在一个 SPM 访问任务内部跳过忙 bank 的数据字，以及把共享乘积缓冲从 4 加到 5。前者在代表场景的周期差只有约 ±2 周期，后者使两例变慢；两项试验代码均未保留，原有 bank 调度、4/8 bank 配置和乘积缓冲深度保持不变。

## 模拟结果

下表比较相同测试输入、访存配置、SPM 预算和 gem5 资源参数，仅 RHS 除法模式及其单 RHS 回退不同。数值正确性由每个用例的 raw 软件参考和残差验收检查。

| 场景 | 原方案周期 | 优化后周期 | 降幅 |
| --- | ---: | ---: | ---: |
| 17 点，7 轮 inverse 复用 | 18,898 | 15,958 | 15.56% |
| 65 点，7 轮 direct 复用 | 55,384 | 47,222 | 14.74% |
| 98 点容量边界 direct | 39,482 | 27,098 | 31.37% |
| 99 点容量边界 direct | 40,888 | 28,438 | 30.45% |
| 65 点，7 轮不复用 | 42,000 | 25,732 | 38.73% |

主套件 21/21、回归套件 19/19 用例通过自动验收。主套件 17 个带周期统计的用例无回退：单点 direct 持平，其余提升；另外 4 个是契约/恢复验收，不用于周期比较。主套件最大绝对误差为 `3.33e-16`，小于现有的 `1e-10` 阈值；由于 `x/d` 和 `x*(1/d)` 的浮点舍入不同，优化模式不保证与旧模式逐位相同。回归套件显式 reciprocal 的唯一周期回退是 level 0 direct（3.70%），其 profile 默认仍为 divide。

优化前后均为 192 KiB SPM（196,608 B）：direct 场景 4 bank，inverse 场景 8 bank，8 B 交织，每 bank 1 读/1 写端口。对照分析还检查了普通场景的 SPM 字访问量与存储分配一致。使用最终二进制显式选择 divide 的 65 点对照用例通过验收，输出哈希以及 7 个控制器窗口的周期与原方案一致。

这些周期来自 gem5 当前时序模型，不能直接当作真实芯片的面积、功耗或频率收益。

## 复现材料

- `results/cfd_dsa/analysis/column-stream-20261020/single-line-reciprocal-final.json`：主套件，显式选择 reciprocal 以满足现有验收脚本的命令行参数记录方式。
- `results/cfd_dsa/analysis/column-stream-20261020/single-line-reciprocal-regression-final.json`：回归套件。
- `results/cfd_dsa/analysis/column-stream-20261020/final-comparison.json` 和 `regression-comparison.json`：逐用例对照、数值和配置检查。
- `results/cfd_dsa/analysis/column-stream-20261020/final-opt-check.log`：19 个普通场景、70 轮的深度 trace 检查。
- `results/cfd_dsa/campaigns/single-line-adaptive-reciprocal-final-source-20261020`：最终源码状态下的 65 点复测。

运行入口为 `python3 projects/cfd_dsa/tools/run_suite.py <suite.json>`。对照脚本为 `projects/cfd_dsa/tools/history/analyze_reciprocal.py <before-campaign> <after-campaign> --report <report.json>`。
