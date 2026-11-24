Completely redraw the supplied architecture diagram as a NEW, simpler, accurate technical infographic. Use the old image ONLY for its dark navy/cyan visual style. DO NOT copy its placement or any old connecting wires. One landscape 4:3 image, clear readable Chinese sans-serif. No decorative texture, black background, navy rounded cards, cyan labels, teal highlight for updates. Fewer arrows, larger labels. The previous dense wiring is NOT to be preserved.

Title: “CFD 单线 LU-SGS · 最新整体架构”
Subtitle: “N=65 · direct · 完整系数缓存 · level 12”

Use FOUR visually distinct areas:

AREA A: full-width top horizontal interface band. Four boxes with bidirectional arrows in this EXACT order:
“ARM CPU / Guest\n准备输入、提交描述符、查询 token”
↔ “系统内存与缓存\nD / L / U / R、backing、Δq、完成记录”
↔ “1 KiB 一致性 cache\n系统资源，不计入设备预算”
↔ “DMA 接口\n分包、在途名额、真实响应”
Below band write “描述符交给控制器；DMA 负责输入读入与输出/记录写回。” Do not draw long wiring from this band.

AREA B: large left 72% accelerator dataflow panel, titled “数据与计算路径（所有片上访问经过 bank / 端口仲裁）”.
Its first row contains FOUR horizontally aligned boxes:
“输入 ready\nDMA 返回 → SPM 写入\n操作数读出后就绪”
→ “LU 上下文 ×2\n冷轮实际分解 D\n产生打包 LU”
→ “RHS 上下文 ×4\n冷轮求 b、Lbar、Ubar\n计算并复用对角倒数”
→ “前向递推\nyᵢ=bᵢ−Lbarᵢ yᵢ₋₁\ny 转发给下一点”.
Only these 3 horizontal arrows. Add small footnote right below this row: “L / U / R 也由输入暂存供给 RHS；LU 与 RHS 可按列就绪重叠执行。”

Its second row contains EXACTLY TWO storage boxes, far apart:
LEFT under RHS area: “系数缓存\nLU* / Lbar / Ubar\n冷轮写入；按版本复用”
RIGHT directly below the forward box: “Tile A / B\n只保存 y，每点 40 B\n区段轮换 / 必要回读”.
Draw one bidirectional vertical connector between RHS box and coefficient cache box. Label it “冷轮写入 / warm 读取 LU* 与 Lbar”. The two boxes should be vertically aligned to make this short.
Draw one downward vertical arrow from FORWARD box to TILE box, label “保存 y”. It must terminate inside TILE, never backward.
There must be ABSOLUTELY NO LINE OR ARROW between coefficient cache and Tile. Leave a very obvious blank gap.

Its third row: ONE large backward box centered below the gap between the two storage boxes:
“后向递推：流式树形 MVM\nΔqᵢ=yᵢ−Ubarᵢ Δqᵢ₊₁\nΔq 转发给前一点”
Draw TWO diagonal arrows DOWN into this backward box: coefficient cache→backward labeled “Ubar 直接读取”; Tile→backward labeled “y”. The arrows form a V shape, do not cross any boxes.
To the RIGHT of backward box, one box “最终 Δq 输出队列\n深度 9；每个向量 40 B\n独立快照 → DMA 写回”.
Connect backward→output by one short horizontal arrow labeled “Δq”.
No other arrows in this panel.

AREA C: right 28% control/lifecycle sidebar, with three vertically stacked cards, visually separate from dataflow:
Top “控制器与依赖调度\n准入、ready、版本、前后向推进\n最终完成条件判定”
Middle teal card “有界请求状态：总上限 12\n计算窗口 ≤8；提交等待 ≤4\n算完 + 片上提交完成 + 输出包发完\n→ 提前归还计算名额\n请求与回调仍保留，等待真实响应”
Bottom “完成记录队列：深度 4\n数据响应完成后提交终端记录\n记录写回响应、数据及资源全部排空\n→ 向 CPU 报告最终完成”
Draw only one downward arrow from middle to bottom labeled “数据响应完成”. No arrow from DMA directly returning compute credit. Do not depict releasing compute credit as releasing all request storage.

AREA D: full-width bottom physical-resource band, NO arrows from the storage boxes. Header “LU / RHS / 前向 / 后向均共用以下物理算术资源”
Large exact text:
“5 MUL + 5 ADD/SUB + 1 DIV  |  延迟 3 / 4 / 12 周期  |  除法 II=4”
“除法器：在途容量 3，结果 FIFO 2，反压、操作数锁存与完成事件”
Under it two small notes:
“LU*：私有缓存原对角字保存已计算倒数，warm 用乘法求 b，不增加系数缓存空间。”
“后向归约：((p0+p1)+(p2+p3))+p4；前两列就绪即启动，仍竞争同一组加法器。”
Footer status strip:
“SPM 192 KiB · direct 4 bank · 总周期 42,608 → 33,905（减少 20.43%）”
“主图为完整缓存模式；缓存不足时 Tile 回退为 y + Ubar。”

Accuracy constraints: ONLY draw the arrows explicitly specified in each area, no automatic extra connectors, no inherited wires from the source image. In Area B, coefficient cache and Tile feed backward separately; Ubar never flows into Tile. The forward result y must flow into Tile first. Reciprocals are solved in RHS using the shared divider, not by the LU context. Logical contexts are not physical arithmetic units. Strong hierarchy, large text, tidy spacing, no collisions or clipped words. Output only this final corrected schematic.
