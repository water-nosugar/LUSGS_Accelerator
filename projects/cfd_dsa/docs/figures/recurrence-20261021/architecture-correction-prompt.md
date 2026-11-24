Edit the attached generated architecture diagram. Preserve its excellent dark blue/cyan style, exact module text unless specified below, titles, 4:3 landscape aspect, crisp Chinese fonts, and all numerical parameters. This is a technical correction pass. Redraw the arrows CLEANLY; technical accuracy takes priority. DO NOT add random connections. The previous image has several wrong arrows and they MUST be fixed:
1. REMOVE the Ubar arrow from coefficient cache ⑦ into Tile ⑧. Ubar NEVER enters Tile in this full-cache configuration. Draw a clear solid arrow directly from coefficient cache ⑦ to backward ⑨, routed ABOVE Tile ⑧ without touching or branching into Tile, labeled “Ubar：系数缓存直接读取”.
2. Keep the solid y arrow Tile ⑧→backward⑨.
3. CHANGE the line from forward⑥ downward so it terminates in Tile⑧, labeled “保存 y”. It must not terminate directly in backward⑨.
4. REMOVE the dashed warm line currently drawn between backward⑨ and coefficient cache⑦. Draw instead a solid data arrow from coefficient cache⑦ UP to RHS⑤, label “warm：LU* / Lbar”. It MUST terminate at RHS⑤, not backward⑨. Route separately from the cold coefficient path.
5. Keep RHS⑤→coefficient cache⑦ labeled “冷轮系数写入”, distinct from the warm reverse path.
6. REMOVE ALL downward “算术请求 / 结果” arrows originating at coefficient cache⑦, Tile⑧ or output queue⑩. These are memory/queue modules, not arithmetic contexts. Instead use a short separate wide annotation strip immediately above the bottom shared-resource row: “LU、RHS、前向、后向上下文 → 共享发射与仲裁 → 同一组 FP64 算术资源”. This explanatory strip needs no arrows through the storage row. Keep the shared physical compute box but no arrow from it into commit-wait box: they are different facilities, not a serial data pipeline.
7. REMOVE the misleading “归还计算名额” arrow that originates from DMA②. Only COMMIT WAIT box returns the compute credit. Draw a dashed arrow from “有界提交等待 ≤4” box to controller① routed around the LEFT outside margin of inner layers. Label “归还计算名额”. Keep DMA②→controller① dashed “响应与完成通知” as a separate control edge.
8. Draw completion record queue→DMA② (“记录写回”), with an actual joined route terminating at DMA, not a dangling arrow.
9. Add a clear solid DMA②→input③ route labeled “输入数据”，routed in the whitespace at layer boundary. Use outside routing if needed, not over boxes or words.

Correct the LU and RHS text:
LU④ exact: “④ LU 上下文 ×2\n冷轮实际分解 D\n产生打包 LU”
RHS⑤ exact: “⑤ RHS 上下文 ×4\n冷轮求 b、Lbar、Ubar 及倒数\nwarm 复用 LU*：倒数乘法求 b”
RHS diagonal reciprocals are produced by actual division operations sharing the physical divider, they are NOT an output of the LU factorization context.
Change DMA② text to “② DMA 与一致性内存接口\n分包、在途名额、真实响应”. Add a small exterior interface capsule between DMA and system memory: “1 KiB 一致性 cache（系统资源）” so it is visibly outside accelerator capacity.
Preserve controller8/12, commit wait4, recordqueue4, outputqueue9, multiplier5/adder5/divider1 latencies3/4/12 II4 inflight3/resultFIFO2, and final performance/footer text. The last footer may use small text, but every module body must be readable. All formulas must stay intact: yᵢ=bᵢ−Lbarᵢ yᵢ₋₁ and Δqᵢ=yᵢ−Ubarᵢ Δqᵢ₊₁.
If routing is too dense, enlarge the spaces between layers and use a slightly taller 4:3 frame; do not omit the required edges. Do not introduce extra lines beyond those specified. Make final update accurate, unambiguous and presentation-ready.
