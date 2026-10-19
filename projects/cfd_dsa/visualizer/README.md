# CFD DSA 微体系结构查看器 · 设计原型

本目录交付本地网页交互原型、会话接口和设计说明。结构参数参考当前
`cold-direct-b4-t8-q64` 推荐配置，**所有内置回放事件都是设计示意**，
不是 gem5 实测 trace，也不代表完整 LU-SGS 算例。

## 打开

直接用浏览器打开 `prototype.html`，无需安装依赖、启动后端或连接网络。
`index.html` 是拆分的开发入口，也可以直接打开；服务预览时运行：

```bash
python3 -m http.server 8765 --bind 127.0.0.1 \
  --directory projects/cfd_dsa/visualizer
```

浏览器访问 `http://127.0.0.1:8765/`。停止服务按 Ctrl+C。

## 交互

1. 左侧切换系统、加速器、资源三个概览，下面的模块树可逐级展开。
2. 选择模块，查看参数、活动事件、证据边界与源码位置。
3. 双击带 ↗ 的模块，或点击详情中的展开按钮，进入内部视图；点击面包屑返回上一级。
4. 选择一个 cell，高亮其路径；时间轴保留其他事件作为淡色上下文。
5. 拖动时间滑块或时间轴空白处；点击事件定位模块、对象和开始时刻。
6. 播放、暂停、单步或跳至下一事件。播放到末尾自动停止。
7. 拖动图中空白处平移，用缩放按钮或 Ctrl/Meta+滚轮缩放。
8. 导出当前层级的 SVG，保留所选对象、时刻和来源标记。
9. 顶部“架构总览”打开独立的芯片框图：CFD DSA 总图、CPU 流水、存储系统。
   点击分区标题或模块继续展开内部单元，点击叶子单元查看参数与来源；
   面包屑返回上一级，Esc 关闭。“适应宽度 / 100%”调整图的显示尺寸，
   “导出此图 SVG”保存这张独立结构图。原有视图、数据与回放交互不受影响。

键盘可选择模块/事件（Enter/Space）、展开模块（→）、调整时间滑块。
窄屏将详情移到图下方；架构画布保留可读尺寸，使用平移/缩放查看。

v0.2 增加各模块的内部单元视图：共享簇的 5 MUL / 5 ADD-SUB / 1 DIV、
两套 LU/RHS 上下文状态、SPM 每 bank 的 R0/W0、DMA 排队/重试/完成路径、
Tile A/B、8 个输出槽与 8 个完成记录槽，以及 O3 流水和实际 cache/互连层级。
详情中的“内部单元”也可直接选择子单元。

新增细分单元默认只有结构信息，标注“状态未采集”。父资源池的 trace
不会自动变成各 lane、端口或槽的活动；图中的连线表达模型数据/控制关系，
不是已验证的 RTL 连线。实际时序事件仍保留原有粒度。

## 会话接口

“导入会话”接受本机 JSON 文件。使用 `demo-session.json` 检查导入流程，
或参考其中的 `schemaVersion / meta / nodes / edges / views / entities / events / tracks`。
导入在浏览器本机读取文件，不上传到外部服务。

- 所有事件为 `[start, end)` 区间；一个文件使用一个明确的时钟域和时间单位。
- 事件声明 `illustrative / observed / derived` 证据类型。
- `recorded` 会话拒绝包含 `illustrative` 事件。
- 未知 request/cell 归属用 `entityId: null`，不自动推断。
- `observability: structure` 表示仅有结构；导入不会允许直接给它附加活动事件。
- `views.ownerId / parentView` 绑定内部视图与返回路径，`groups.ownerId` 绑定资源池/上下文分组。
- 只展示事件提供的活动状态；未记录当前事件不等同于硬件空闲。
- 原型文件上限 20 MiB、10000 个事件、500 个模块；大 trace 的分块与索引属于后续实现。

原型尚未直接解析 gem5 输出，也不提供性能归因、完整逐周期状态恢复、
实际队列占用或具体 lane ID。真实接入边界详见
[设计说明](../docs/CFD_DSA_VISUALIZER_DESIGN.md)。

## 开发与验证

```bash
node projects/cfd_dsa/visualizer/verify_model.cjs
python3 projects/cfd_dsa/visualizer/build_standalone.py
```

修改源 HTML、CSS 或 JS 后重新打包，包括独立架构图的
`poster-layout.js / architecture-poster.js / architecture-poster.css`。
`prototype.html` 和 `demo-session.json` 是可复现生成的交付文件。
独立验证脚本检查来源隔离、时间区间、层级聚合和无效引用。
