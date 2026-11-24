# 更新图片的生成说明

使用内置 image_gen 工具；两张原图分别作为编辑目标。

## 存储架构图提示词

Edit target: /home/zyy/桌面/2.png. Use case: infographic-diagram, technical architecture diagram update. Update the attached dark Chinese storage architecture to the CURRENT verified design described below. Produce ONE polished high-resolution diagram, landscape about 4:3, at least 2400 pixels wide if available. Preserve the original near-black background, navy rounded boxes, light cyan Chinese text and thin arrows, but reorganize into a compact, very legible grid with no giant empty space. Use Chinese sans-serif typography. Crisp flat 2D diagram, no decorative illustrations. Use teal borders only for changed components. Exact technical content is more important than mimicking old wiring. Do NOT copy obsolete labels saying full-cache Tile contains Ubar.

Title: “CFD 单线加速器 · 存储架构（更新版）”
Subtitle: “推荐配置：N=65 · direct · 完整系数缓存 · level 12”

Layout and exact node text:
TOP EXTERNAL STRIP, outside device boundary: 
A “系统内存与缓存层级\n输入 D / L / U / R\n中间结果 backing、最终 Δq、描述符与完成记录”
B “1 KiB 一致性 DMA cache\n独立系统资源，不计入设备容量”
A↔B.
Large internal boundary titled “加速器内部：192 KiB 统一容量预算”.
Inside top row LEFT C “计算寄存器与上下文\nD、LU、RHS、b、乘积与累加器\n相邻点反馈：前向 y / 后向 Δq 各 40 B”
Inside top row CENTER D, teal highlighted: “有界请求状态：总上限 12\n计算窗口 ≤8；提交等待 ≤4\n复用既有状态槽，不新增 4 份大请求”
Inside top row RIGHT E, teal highlighted: “提交等待状态\n输出包全部发出且片上提交完成\n提前归还计算名额\n保留 token 与回调，等待真实 DMA 响应”
Connect C→D→E with short, clean arrows. D/E are logically the request state partition, NOT two additional allocations.
Next row three queue boxes, connected DOWN from compute/request state as appropriate:
Q1 “前向数据 drain 队列\n必要输出的独立字节快照”
Q2 “最终 Δq 输出队列\n9 个名额；每个向量 40 B\n响应完成后释放”
Q3 “完成记录队列\n深度 4；终端记录快照\n满足完成条件后发布”
E→Q3 label “真实响应完成”.
Central bridge F “DMA 控制与在途快照\n分包、在途名额、响应跟踪” connecting queues to F. F↔external B, route around edge without crossing text.
Below F and compute C connect bidirectionally to G “统一片上访问队列与仲裁\n地址映射到 bank；受读写端口限制\n访问完成后发布 ready”. Make G the ONLY portal for timed accesses to all lower SPM regions.
LOWER GROUP headed “Banked SPM：逻辑区域共享 bank 与端口”
Four boxes in row:
S1 “输入暂存区\nD / L / U / R\n等待计算读取”
S2 teal “系数缓存（完整模式）\nLU* / Lbar / Ubar\n600 B/点 × 65 = 39,000 B”
S3 teal “Tile A：只保存 y\n16 × 40 B = 640 B”
S4 teal “Tile B：只保存 y\n16 × 40 B = 640 B”
G↔each S box. The arrows to S2 are labeled “Ubar 冷轮写入，本轮及后续后向读取”; arrows to S3/S4 labeled “y 保存 / 回读” if space permits without clutter.
Add a neatly boxed concise note under the SPM row, exact text:
“LU*：私有缓存的 LU 对角字保存已计算倒数；非对角仍为 LU。外部 LU 输出保持原格式。”
Bottom budget strip:
“当前预算：136,448 B 状态预约 + 1,280 B 双 Tile + 39,000 B 系数缓存 = 176,728 B ≤ 196,608 B”
Bottom small note:
“主图为完整缓存模式；缓存不足时 Tile 回退为 y + Ubar。bank 数不变：direct 4，inverse 8。”
Legend: “实线：数据搬运；虚线：状态 / 名额控制；青绿边框：本次更新”
Use dashed arrows for D/E compute-credit controls only. No arrows implying that commit detach means external completion. Do not duplicate Ubar in Tiles in the main diagram. Clear arrowheads, no crossings through nodes, no overlapping labels, no clipped text. All requested Chinese text must be readable and spelled accurately. Output ONLY this storage diagram.

## 整体架构图提示词

Edit target: /home/zyy/桌面/mermaid-diagram.png. Use case: infographic-diagram, technical architecture diagram update. Update this Chinese overall architecture diagram to the current verified gem5 accelerator. Produce ONE polished high-resolution 4:3 landscape diagram, at least 2400 pixels wide if available. Preserve near-black background, navy rounded boxes, light cyan Chinese type and thin arrows. Reorganize compactly into clear horizontal layers, avoiding the old huge whitespace and crossing wires. Teal borders denote updated modules. Chinese sans-serif with legible body text. Engineering schematic, no decoration.

Title: “CFD 单线 LU-SGS 加速器 · 整体架构（更新版）”
Subtitle: “level 12 推荐路径：N=65 · direct · 完整系数缓存”

Top external strip, outside accelerator boundary:
CPU “ARM CPU / Guest\n准备输入、提交描述符\n查询 token、读取结果”
MEM “系统内存与缓存层级\nD / L / U / R、backing\n最终 Δq、完成记录与状态”
CPU↔MEM label “数据准备 / 结果读取”.
Large accelerator boundary, with these layout layers:
TOP INNER ROW controller at left and DMA at right:
CTRL “① 控制器与依赖调度\n计算窗口 ≤8；总请求状态 ≤12\n前后向推进、版本管理、最终完成判定”
DMA “② DMA 与一致性内存接口\n1 KiB 一致性 cache\n分包、在途名额、真实响应”
CPU dashed→CTRL label “描述符 / token”; CTRL dashed→CPU label “全部完成条件满足”.
DMA↔MEM solid, route vertically.
CTRL dashed→INPUT label “准入”; INPUT connected to DMA for data.
MIDDLE CORE ROW left to right, each a clearly readable box:
INPUT “③ 输入暂存与 ready 管理\nDMA 到达 → SPM 写入 → 操作数读出”
LU “④ LU 上下文 ×2\n冷轮实际分解 D\n产生 LU 与所需对角倒数”
RHS teal “⑤ RHS 上下文 ×4\n冷轮求 b、Lbar、Ubar\nwarm 复用 LU*：倒数乘法求 b”
FWD “⑥ 前向递推\nyᵢ = bᵢ − Lbarᵢ yᵢ₋₁\n列就绪即消费；y 转发给下一点”
INPUT→LU label D; LU→RHS label “LU 就绪”; INPUT→RHS route below label “L / U / R”; RHS→FWD label “b / Lbar 列”. Keep readable, no arrow crossing text.
NEXT CORE ROW left to right:
CACHE teal “⑦ 系数缓存\nLU* / Lbar / Ubar\nUbar 冷轮写入后，本轮及后续后向直接读取”
TILE teal “⑧ Tile A / B\n只保存 y；每点 40 B\n区段轮换与必要回读”
BACK teal “⑨ 后向递推：流式树形 MVM\nΔqᵢ = yᵢ − Ubarᵢ Δqᵢ₊₁\nΔq 转发给前一点”
OUT teal “⑩ 最终 Δq 输出队列\n深度 9；独立结果快照\n等待 DMA 写回响应”
RHS→CACHE label “冷轮系数”; CACHE→RHS dashed/solid clearly data label “warm LU* / Lbar”; FWD→TILE label y; CACHE→BACK label Ubar; TILE→BACK label y; BACK→OUT label Δq; OUT→DMA route outer edge label “最终写回”. CACHE/BACK arrows must not misleadingly say memory bypasses bank timing; add a layer label “操作数与结果访问均受片上 bank / 端口仲裁约束”.
BOTTOM resource row left wide box:
SHARED “共享发射、仲裁与完成接口\n唯一 FP64 计算簇：5 MUL + 5 ADD/SUB + 1 DIV\n乘 / 加 / 除延迟：3 / 4 / 12 周期；除法 II=4\n除法器：在途容量 3，结果 FIFO 2，反压与完成事件”
All LU/RHS/FWD/BACK have a concise connector or common resource bus to SHARED labeled “算术请求 / 结果”; contexts are state, NOT separate arithmetic units.
Next to SHARED:
COMMIT teal “有界提交等待 ≤4\n算完、片上提交完成且输出包全部发出\n先归还计算名额；保留请求与回调\n真实响应后进入完成记录流程”
RECORD “完成记录队列：深度 4\n记录快照写回并等待响应\n所有数据、记录和资源排空后最终完成”
COMMIT dashed→CTRL label “归还计算名额”; COMMIT→RECORD label “数据响应完成”; RECORD→DMA label “记录写回”; DMA dashed→CTRL label “响应与完成通知”. Clearly distinguish early compute credit release from final completion.
Concise footer notes in two lines:
“LU* 的原对角字保存冷轮计算的倒数，复用不增加系数缓存空间。主图完整缓存模式下 Tile 不重复保存 Ubar。”
“后向 MVM 使用 ((p0+p1)+(p2+p3))+p4；前两列就绪即归约，仍共享同一组加法器。”
Optional small status strip if space:
“65 点实测：总周期 42,608 → 33,905（−20.43%）；SPM 192 KiB，direct 4 bank 保持不变。”
Legend “实线：数据；虚线：控制 / 名额；青绿边框：更新项”.
Constraints: no separate physical divider per context, no claiming precomputed D at cold start, no direct forward tree claim (direct FWD keeps column consume, tree explicitly on BACK), no free arithmetic or writes. FWD and BACK are logical contexts, physically shared compute. Clean precise arrowheads and no overlap/clipping, preserve all essential content in clearly readable Chinese. Output ONLY this overall architecture diagram.

