# CFD DSA 渐进精简记录

本轮按阶段推进；不把“文件变短”当作整个重构完成。
已有用户修改（包括RESULT_MANAGEMENT.md删除、实验策略改动）保留。

## 阶段1：文档入口

README区分现行入口、专题实验、冻结历史快照。五份历史汇总/报告停止维护
实时默认值，仅允许勘误与链接修复；原技术内容和历史数据不删除。
新RESULT_LIFECYCLE.md承接原结果管理内容，保留已有删除状态，修复六份文档的旧引用。
特别收紧“开发结果可丢弃”为先审查引用、manifest与诊断价值。

## 阶段2：报告职责抽取

run_cfd_dsa.py的THEORY、统计解析、统计段选择及报告打印抽入cfd_reporting.py。
ReportContext显式捕获所需配置，每次运行独立绑定；不依赖gem5 SimObject导入。
配置入口保留原函数名绑定，保留stats-only和仿真结束两处调用行为。
历史统计名、文本格式、浮点格式、统计段选择规则均不主动更改。
patha_matld_extra_per_run仍来自现有配置逻辑，THEORY保留兼容导入。

检查器：projects/cfd_dsa/tools/check_reporting_compat.py，使用显式指定的可信
旧runner源文件抽取原报告实现，与新模块比较解析字典和完整stdout。
可从本次修改前Git版本提取旧runner；本轮执行时快照在/tmp/cfd-runner-before.py。
该临时路径不是长期依赖，报告比较器参数接受任意明确的旧源码快照。
已核实报告区与Git提交`0f75982f97c8257263b9f57a47858ed3db02d6b7`中的runner一致，
可以用该提交恢复长期对照源，不依赖/tmp文件存续。

## 阶段3：Step2参数profile

Step2 wrapper由约170行缩为22行。全部有序默认值、动态stream/coarse选择、
autonomous scheduler选择和两个别名组集中到
`projects/cfd_dsa/configs/lusgs/step2_profile.py`。
显式用户参数仍在argv前部，缺失默认值按原顺序追加；wrapper仍只设置报告焦点并
转交canonical runner。`check_step2_profile.py`从Git基线提取旧wrapper，比较六组
代表性输入的最终argv，要求逐项完全相同。

这一步统一的是Step2 profile，不是将整个run_cfd_dsa.py的所有参数迁入schema；
通用runner仍保留其他Path/Step参数定义，避免一次修改全部入口。

## 阶段4：Step2 guest拆分

仍使用单一C翻译单元，通过include模块按职责拆出：

- `step2_cli.inc`：CLI解析和mode分发设置；
- `step2_validation.inc`：最终解比较；
- `step2_report.inc`：统计、矩阵比较、回本点和报告；
- `step2_streaming.inc`：stream launch/wait/reaper/line执行路径。

主文件由11057行降到6850行。模块仍在原位置被include，函数顺序、static链接、
宏、结构体可见性和编译参数不变。拆分前后`-O2`静态AArch64二进制SHA-256均为
`571ff4e9de1cbf20df23617ac68134a4240595067717673145d300e1c88b8a87`。
prepared/raw剩余路径尚未强行拆成独立库，避免改变数值helper所有权。

## 阶段5：controller拆分边界

仅将`beginResourceStats/resourcePools/endResourceStats`移到
`src/arch/arm/cfd_coeff_preprocess_diagnostics.inc`并在原位置include。
`processTick()`、各stage调用顺序、pendingOps、事件对象、request生命周期和SimObject
边界均未移动。该模块仍属于同一匿名namespace和同一C++翻译单元。

## 阶段6：死代码审查结论

本轮没有删除新的C/C++/ISA/Python实现。guest以`-Wall -Wextra`构建，没有出现
拆分导致的未使用static函数警告；SConscript、mode、suite和文档闭包仍覆盖
legacy prepared、raw、coarse、event、Step1/3/4及Path B/C。
历史统计字段继续服务旧stats解析，实验scheduler继续由专项suite引用。

可重复删除项仍仅限`__pycache__`/`*.pyc`等生成物；结果目录按
`RESULT_LIFECYCLE.md`审查，不能按名称批量删除。若要删除源码，仍须为每个符号提供
零build引用、零mode引用、零test引用、零文档依赖及替代实现证据，并单独跑核心回归。

## 已执行验证

- runner由4030行缩到2301行；cfd_reporting.py为1742行。代码职责分离，
  不是删除1700行功能；不以总代码行数下降作为正确性标准。
- gem5 ARM构建、lusgs-step2/trsv5/decode-exclusive guest构建通过。
- core-regression 10/10与request-frontier-aging对照8/8通过。
  证据：results/cfd_dsa/campaigns/reporting-extraction-core-20260912/及
  results/cfd_dsa/campaigns/reporting-extraction-stream-20260912/。
- 8个stream用例与抽取前campaign比较，结果hash和全部resource计数相同；
  benchmark总周期差为0、±2或-4，原因尚未确认，不宣称严格周期不变。
  本轮没有更改算术或事件调度，不能将微小差异作为性能收益。
- 文档入口链接、Python语法、suite JSON与git diff --check通过。
