# CFD DSA 微体系结构可视化设计 v0.2

日期：2026-10-13。范围：当前 gem5 CFD DSA 的架构与实验查看工具。
交付：[本地交互原型](../visualizer/prototype.html)、[原型说明](../visualizer/README.md)。

## 1. 用户任务与第一版边界

核心操作是选择一次运行、展开一个模块、定位一个请求或 cell、检查某个时刻
的传输/运算/依赖，再沿数据路径查看下一环节。架构、对象与时间共享选择状态。

第一版交互设计覆盖：

- 系统、加速器内部、计算与存储资源三个层级。
- 模块详情、结构参数、源码位置和建模/观测边界。
- 按 cell 跟踪路径与事件；全局事件保留原始归属。
- 时间定位、区间事件选择、播放/单步及下一事件。
- 本地会话导入与包含来源标记的 SVG 导出。

内置事件全部人工编排，省略大量实际运算，用来验证界面交互；其时长、对象关联
和传输片段不能用于性能结论。结构参考 shared 模型的推荐 cold direct 配置。
真实输出解析与状态重建是下一个实现阶段。

## 2. 信息结构与交互布局

桌面布局：顶部为会话与来源，左侧为层级与模块导航，中间为固定布局的架构画布，
右侧为所选模块详情，底部为按资源排列的时间轴。详情独立滚动，避免把时间轴
推到长说明之后。窄屏依次排列画布、详情和时间轴；画布通过平移/缩放查看。

选择逻辑：

| 用户操作 | 架构图 | 模块详情 | 时间轴 |
|---|---|---|---|
| 选择模块 | 标出模块 | 显示资源参数/事件/边界 | 保持时间上下文 |
| 展开模块 | 切换至内部层级 | 选择内部可见模块 | 保持时刻与对象 |
| 选择 cell | 高亮关联路径，其他模块淡化 | 限定活动观测范围 | 对象事件突出，其他事件淡化 |
| 拖动时间 | 更新活动/等待状态 | 显示当前区间事件 | 游标随动 |
| 点击事件 | 定位适合的层级和模块 | 显示区间、对象与证据 | 定位开始时刻 |
| 导出 | 导出当前层级完整结构 | — | 时刻写入导出来源标记 |

颜色语义保持稳定：青色表示有活动事件，琥珀色表示等待事件，蓝色表示所跟踪的
路径或数据传输。控制/依赖用虚线，数据路径用实线；类别有文字标签。
结构位置在回放过程中保持不变。点表示当前事件涉及的路径，不模拟真实传播速度。

## 3. 架构语义

| 层级 | 模块 | 表达边界 |
|---|---|---|
| 系统 | CPU/Guest、内存系统、CFD DSA | 内存层级折叠；不假设具体 cache 配置 |
| 加速器 | 控制器、DMA、输入、LU/RHS 上下文、共享计算簇、SPM、tile、反馈、输出与完成队列 | 区分逻辑任务、物理资源、存储区域和 FIFO |
| 资源 | mul/add/div 资源池、乘积状态、SPM 仲裁与 banks | 资源池级观测；SPM 端口按 bank 区分 |

LU 与 RHS 上下文共享 5 mul、5 add/sub、1 div；算法任务不绘制为额外算术单元。
前后向反馈表示逻辑数据/依赖路径，计算仍经过共享资源。输入、系数与 tile 存储区域
共用 SPM bank/端口。活跃乘积/累加寄存器和完成/输出 FIFO 单独计入容量预算。

192 KiB 是参考配置的统一状态预算，不能用作 SRAM 利用率或综合面积。
当前没有 RTL/综合依据的模块应标明“gem5 时序模型”；折叠或未知内部结构保持该标记。
互斥实现由实际运行配置选择，不叠加为一个包含所有硬件的架构。

## 4. 会话数据契约

原型使用一个 `schemaVersion: 1` JSON 文件；后续可把同一契约拆成 manifest、
architecture、events 与 metrics 独立文件。布局与硬件描述逻辑上分离。

| 字段 | 含义 |
|---|---|
| `meta` | 名称、来源、时间单位/域、时间范围、实际配置引用 |
| `nodes` | 稳定资源 ID、父级、类型、参数、描述、源码、展开视图 |
| `edges` | 稳定路径 ID、端点、数据/控制类型、语义标签 |
| `views` | 可见模块、位置、正交路由、分组；不改硬件语义 |
| `entities` | request、line/cell、generation 等身份与可见标签 |
| `events` | 明确模块、对象、区间、类型、证据及关联路径 |
| `tracks` | 资源轨道与模块 ID 对应关系 |

事件示例：

```json
{
  "id": "cluster:sample:23",
  "moduleId": "div",
  "entityId": null,
  "start": 103,
  "end": 115,
  "kind": "compute",
  "label": "共享除法发射",
  "edgeIds": [],
  "lanes": 1,
  "evidence": "observed"
}
```

该例只说明字段格式，不是测量结果。没有 request ID 时保持 `entityId: null`。
未确定关联路径时保持 `edgeIds: []`，不补出想象中的流动。

区间统一 `[start,end)`：完成时刻不再算在途。瞬时事件在后续正式接口中单独使用
`timestamp` 和 `instant` 类型；原型只接受非空区间，不能把累计计数伪装成区间。
`lanes` 指此次发射宽度，不指持续占用的具体 lane，也不等于在途数量。

原型接受 `cycle` 或 `tick` 的单一时间域，只支持 JavaScript 安全整数范围。
正式跨时钟域会话以 gem5 tick 为主轴，另存每个 clock domain 的周期/频率；超出安全
整数时使用十进制字符串/BigInt 或带已记录 origin 的相对时间，不能静默损失精度。

证据类型：

- `illustrative`：交互设计示意，只允许 demo 会话。
- `observed`：直接 trace 字段或明确记录的开始/完成对。
- `derived`：根据已声明规则重建的区间或状态，附来源与推导规则。

原型拒绝实测会话中的示意事件、未知模块/路径、循环父级、无效时间和未分配轨道。
界面以文本方式显示导入标签，不把输入内容作为 HTML 或脚本执行。

## 5. gem5 数据接入方案

| 输入 | 可提取内容 | 不能直接推断的内容 |
|---|---|---|
| `config.json / config.ini` | SimObject、端口、时钟、配置 | 控制器内部 C++ 队列及环境变量生效参数 |
| `manifest.json`、suite、实际 runner 参数 | 运行来源、模式和选参 | 不按报告默认值替代实际配置；重复参数按实际解析顺序处理 |
| coeff 主 CSV | request/generation/line/cell、stage、token、packetId、address/size/bank/port 等 | 任意阶段都有配对结束事件、trace 无截断 |
| `.cluster.csv` | `cycle,pool,lanes,caller,complete_cycle` | 具体 lane ID、request/cell 身份、乘积数值 |
| `CFD_ONCHIP_*` | 任务起止、owner、字访问、bank、priority | 所有 owner 都等价于 coeff request ID |
| diagnostics / `stats.txt` | 定义明确的累计计数、容量峰值、汇总指标 | 按周期回放或互斥 stall 时长 |

接入分成三个适配器：结构适配器、事件适配器、统计适配器。结构适配器合并 SimObject
连接与手工声明的内部资源模板，从实际 runner 输出记录完整生效参数。
事件适配器显式配对、检查重复/缺失/取消，不能配对的事件保留为瞬时/不完整证据。
统计适配器保留单位、定义、采样窗与统计口径，不参与虚构逐周期状态。

最先接入 cluster CSV：其发射与完成端点明确，可以可靠构建资源池时间轴。
这部分保持无 request 归属；随后接入主 CSV 的 request 生命周期，再补 SPM owner
命名空间映射及 DMA packet 开始/响应对应关系。跨文件身份未验证时不联结。

每个真实会话的 provenance 应记录：git commit 与 dirty 状态、源文件/二进制哈希、
实际 argv 和生效环境参数、输入路径与哈希、ROI、trace 采集范围、截断/完整性标记、
时钟域。部分 trace 必须显示“未观测”，不能把空白标成空闲。

## 6. 正式实现与原型选择

原型采用无依赖 HTML/CSS/JS + SVG，可本地直接打开，便于评审结构和选择行为。
生产界面建议 React + TypeScript + React Flow 管理节点、端口、层级及状态；ELK 仅用于
初始或局部布局，保留固定模块锚点。对同一会话的图不在播放时重新排布。

数据适配器用 Python 流式处理 CSV/日志，输出版本化会话数据。回放索引使用起止
事件扫描和周期性 checkpoint，按时间窗读取。Web Worker 负责解析与索引；密集时间轴
使用 Canvas，架构图保持 SVG/HTML。先离线查看，后续需求明确时再加实时连接。

原型上限 20 MiB / 10000 事件 / 500 模块，主要验证界面和契约，尚未实现分块、
完整生命周期状态重建、实际队列占用、搜索、运行比较或瓶颈归因。

## 7. 实现阶段与验收

| 阶段 | 交付 | 验收标准 |
|---|---|---|
| 设计原型（本次） | 三层图、选择联动、示意回放、JSON 导入、SVG 导出、接口定义 | 来源始终可见；选择、单步、播放、导出能操作；导入拒绝不一致数据 |
| 真实资源回放 | cluster CSV + 实际配置适配器 | 发射数/lane 数和完成端点逐项对应输入；无猜测的 request 归属 |
| 请求与存储联动 | 主 CSV、SPM/DMA 事件配对与身份映射 | 选定请求能追踪有证据的传输；缺失区间/未知身份明确显示 |
| 研究分析 | 窗口指标、队列快照、运行对比与相关路径检查 | 分母/单位/ROI 一致；拒绝次数与等待周期分别展示；不以相关性直接声称因果 |

浏览器 QA 覆盖桌面与窄屏、层级选择、对象高亮、事件定位、时间拖动/键盘单步、
播放停止、导入拒绝/恢复和 SVG 来源标记。模型 QA 检查 `[start,end)`、共享父级聚合、
身份未知状态与记录/示意隔离。

## 8. 结构参考

- [当前单线研究](CFD_DSA_SINGLE_LINE.md)
- [第二轮流水与存储建模](CFD_DSA_SINGLE_LINE_ROUND2_20261012.md)
- [调度诊断统计语义](CFD_DSA_SCHEDULER_DIAGNOSTICS.md)
- [推荐 suite](../suites/single-line-round2-recommended.json)
- [共享计算簇](../../../src/arch/arm/cfd_coeff_preprocess_cluster.inc)
- [SPM bank 与访问仲裁](../../../src/arch/arm/cfd_coeff_preprocess_onchip.inc)

## 9. v0.2：模块与内部单元展开

概览保持三个入口，每个主要模块增加所属内部视图。左侧显示完整模块树，
详情面板列出直接子单元；资源池与上下文边框也可选择。面包屑支持返回上级。

| 模块 | 细分内容 | 性质 |
|---|---|---|
| 共享计算簇 | MUL0–4、ADD/SUB0–4、DIV0、发射与 lane 预留、在途完成表、四类乘积/累加状态 | 算术单元 + 逻辑调度 + 有界状态 |
| 控制器 | descriptor/token、窗口 credit、准入、前沿依赖、优先级仲裁、generation/取消、完成 fence | 逻辑控制 |
| DMA | 传输快照、分包/地址转换、packet 队列、retry 保持、在途 credit/sender state、响应回调 | 接口与传输状态 |
| 输入暂存 | 输入槽0/1、D/L/U 区域、R 区域、ready 条件 | 有限槽 + 共享 SPM 区域 |
| LU | 两个上下文，各自的 D/LU、目标暂存/乘积、列行 k 状态、pivot/ready、除法掩码 | 寄存器/逻辑状态；共享算术硬件 |
| RHS | 两个上下文，每个15个 RHS 状态槽、依赖/完成掩码、60项行乘积缓冲引用 | 状态槽；由 mask 决定实际启用 |
| Banked SPM | 输入/缓存/tile 区域、访问队列、地址映射、仲裁、Bank0–3、各自 R0/W0、ready 发布 | 共享 bank/端口模型 |
| 有限系数缓存 | direct 的 LU、Lbar、Ubar 区域，valid/version/mode 契约 | SPM 区域与控制；inverse 替代 LU 为 DInv |
| Tile | A/B 两个缓冲，各自 y/Ubar 区域、phase/cell 范围、DMA 计数与响应引用 | SPM 区域与生命周期 |
| 反馈 | forward handoff、前向/后向/base 状态、组合与发布，共享簇引用 | 算法状态；不增加 MVM 单元 |
| 输出 / 完成队列 | 每个8个结构槽、入队 credit、响应/回收 | 有界条目 |
| O3 CPU | Fetch/分支预测、Decode、Rename、IQ、CPU FU/Writeback、LSQ、ROB/Commit、CFD 接口 | CPU 功能/资源边界 |
| 内存层级 | L1I/L1D、DMA cache、L2XBar、L2、SystemXBar、DRAM SimpleMemory、映射 SimpleMemory SPM | 当前 runner 的实际连接 |
| Cache | tag/valid/dirty、data array、MSHR/miss 合并、查询/替换控制 | cache 结构概览 |

槽、端口和 lane 的数量来自参考配置和源码声明；未采集实际分配或身份时，
显示“仅结构 · 状态未采集”。父模块的活动只能用于父级汇总，不自动传播到
每个子单元。原型保留133个原有示意事件，没有为新增单元编造周期或占用。

`nodes.observability` 可为 `structure` 或 `event`。前者禁止直接绑定活动事件；
接入实际细粒度 trace 后，只有具备明确身份和来源的单元才改为 `event`。
`views.ownerId / parentView` 和 `groups.ownerId` 声明视图/分组的所属模块，
校验器检查引用和返回路径是否成环。内部视图中的共享资源引用仍使用同一个模块 ID。

CPU 参数使用本 runner 的 O3 默认值（IQ64、ROB160、各阶段宽度8等）；
正式导入应使用实际生效参数。DRAM 是 SimpleMemory，未画出不存在的真实
DRAM controller 子结构。CPU 地址空间中的256 KiB SimpleMemory SPM 与
设备内部192 KiB统一状态预算分别表达，不能混为同一存储容量。
