# 架构图更新（2026-10-21）

最终交付：

- `storage-updated.png`：存储架构图，对应桌面 `2-更新版.png`。
- `architecture-updated.png`：整体架构图，对应桌面 `mermaid-diagram-更新版.png`。

两图使用内置 image_gen 编辑/重绘，并保留原图深色风格。原始桌面图片未覆盖。

主图适用配置：N=65，direct，完整系数缓存，level 12；192 KiB 统一容量预算。完整缓存模式 Tile 仅保存 y，Ubar 冷轮写入系数缓存，本轮及后续后向计算均由缓存读取。缓存不足时 Tile 回退为 y + Ubar。图中已区分计算名额提前归还与真实 DMA、完成记录响应后的最终完成。

提示词记录：

- 存储图最终提示词：[imagegen-prompts.md](imagegen-prompts.md) 的“存储架构图提示词”。
- 整体图最终提示词：[architecture-final-prompt.md](architecture-final-prompt.md)。最终采用分区布局，使缓存、Tile 分别向后向计算提供 Ubar、y。
- `imagegen-prompts.md` 中的整体图初稿提示词与 `architecture-correction-prompt.md` 为过程记录；对应草稿未作为最终文件交付。

关键核对：LU 实际分解、RHS 倒数复用、流式树形后向 MVM、共享 5/5/1 算术资源、请求状态上限 12（计算 ≤8 / 提交等待 ≤4）、最终输出队列 9、完成记录队列 4、1 KiB 一致性 cache 独立系统预算，以及 176,728 B 容量账本。

模型依据：[实现说明](../../history/CFD_DSA_RECURRENCE_OPT_20261021.md)。PNG 为生成的栅格图，当前尺寸均为 1448×1086；最终交付文件经过文字、关键箭头及数值目视核对。
