# 跨request前沿优先与aging实验

## 实现边界

新增`--coeff3-schedule=request-frontier-aging`，默认不变。
Python配置在`projects/cfd_dsa/configs/run_cfd_dsa.py`校验模式，通过既有
`GEM5_CFD_COEFF3_SCHEDULE`传递给
`src/arch/arm/cfd_coeff_preprocess_controller.cc`的`rhsScheduleFromEnv()`。
本轮定位：Python模式校验1361行；C++模式解析92行、request排序3114行、
复用RHS排序3264行。

`processSolve()`在每周期处理RHS之前对request访问顺序作稳定排序：

1. 若有活动、非在途、LU step已ready的RHS，其ready等待超过64周期，则提升该request；
   多个提升request之间先处理最老候选所在request。
2. 其他情况下，优先`!dqStarReadyLocal && (cellId==0 || previousDqReady)`的request。
3. 然后按cellId升序，同分保留原遍历顺序。
4. 每个request内部复用原`frontier-aware` RHS排序和完整候选访问。

这是request级优先级实验，不是全局逐RHS仲裁队列。aging使用既有readySinceCycle，
新变ready的RHS首次访问时才建立时间戳；排序读取的是本次候选扫描之前的状态。
提升request不等于直接提升该request内某一个RHS，因此不能宣称严格无饥饿。
同一RHS中的乘、减、除顺序不变，不启用reciprocal、split或重关联。

原模式仍保留unordered_map遍历顺序，新模式显式排序；没有改变LU/TRSM准入、
FU count/latency/II、SPM或drain资源。当前模型没有额外收取排序硬件的周期/面积，
属于现有周期内仲裁抽象下的策略对照，不是RTL时序签核。

## 对照与检查

`projects/cfd_dsa/suites/history/request-frontier-aging.json`复用前轮固定资源配置，
覆盖1×17的cold与mixed（3 sweep、interval2），每组2/2、2/4的原模式与新模式，
共8个用例。新模式只覆盖schedule参数，不改变其他参数。

```bash
scons build/ARM/gem5.opt -j2
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 \
  lusgs-step2 trsv5 decode-exclusive
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/history/request-frontier-aging.json \
  --campaign request-frontier-aging-20260912
python3 projects/cfd_dsa/tools/check_request_frontier_aging.py \
  results/cfd_dsa/campaigns/request-frontier-aging-20260912
```

检查同一gem5二进制、数值摘要、运算数量及固定资源参数，并分别报告
总周期、最长RHS等待、Lbar列等待。不将拒绝次数或累计等待当独占周期。
本轮不外推多line、真实DMA或其他规模。

## 实测结果（2026-09-12）

证据目录为`results/cfd_dsa/campaigns/request-frontier-aging-20260912/`，
每个case下保存`manifest.json`、`simout.txt`、`stats.txt`和trace。
总周期取`streamingStep2.totalCycles`；变化为相对同二进制原策略的周期变化。

| 负载 | LU/TRSM | 原策略周期 | 实验策略周期 | 变化 |
| --- | --- | ---: | ---: | ---: |
| cold | 2/2 | 7916 | 8378 | +5.84%（退化） |
| cold | 2/4 | 7864 | 7818 | -0.58% |
| mixed | 2/2 | 15080 | 16006 | +6.14%（退化） |
| mixed | 2/4 | 14974 | 14882 | -0.61% |

冷启动最长RHS等待：2/2从432降到142，2/4从1043降到246；
但Lbar列等待：2/2从2724增加到3060，2/4从2759减少到2668。
混合复用的Lbar列等待分别为5448→6120及5518→5336。
以上等待统计不是独占周期，不能与total相加。

结论：请求级aging改善了本轮观测的最长等待，却没有在2/2获得端到端收益。
2/4存在小幅收益，但不足以支持将新模式提升为推荐默认，更不能据此要求增加上下文。
当前2/2继续使用原frontier-aware，新模式标记experimental，作为后续消融对照保留。

## 下一步入口与尚未确认

本轮将aging置于前沿优先之前；request提升后仍访问其所有候选，
不是仅服务阻塞前沿的那个DInv/Lbar RHS。2/2退化说明这一组合没有达到目标，
但不能仅凭汇总指标将根因确定为aging或某个batch。
下一步应先拆开“前沿优先”和“aging”，并按DInv/Lbar/Ubar及前沿距离统计
每次获批/拒绝；评估只给关键RHS预留优先、后台任务使用剩余机会的策略。
不应以降低最长等待为唯一目标，也不应直接启用更大的全局队列。

验证：gem5 ARM及三个guest目标构建通过；8个用例full validation通过，
检查器输出`REQUEST_FRONTIER_AGING_CHECK_PASS`，每对结果摘要、运算数量和
资源参数一致。Python语法、JSON及`git diff --check`通过。
未重跑完整历史回归、多line、真实DMA；未综合排序逻辑。
