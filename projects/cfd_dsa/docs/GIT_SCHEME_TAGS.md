# CFD-DSA Git 方案标签

当前 CFD-DSA 的各方案共存于同一份源代码，通过 runner、mode 和配置开关选择。
因此下面的 annotated tag 都指向同一个完整实现快照；它们是方案入口标记，不表示
仓库中存在可准确恢复的逐阶段历史源码。

| 标签 | 对应方案 | 主要入口 |
| --- | --- | --- |
| `snapshot/cfd-dsa-2026-09-09` | 本次上传的完整工作快照 | `projects/cfd_dsa/` |
| `scheme/path-a-v1` | dotp-row / internal-buffer Path A | `configs/run_cfd_patha.py` |
| `scheme/path-b-v1` | ZA partial-sum pipeline | `configs/run_cfd_pathb.py` |
| `scheme/path-c-v1` | ZA selected-column outer product | `configs/run_cfd_pathc.py` |
| `scheme/trsv5-v1` | standalone TRSV5 | `configs/run_cfd_trsv5.py` |
| `scheme/lusgs-step1-v1` | Path A + software TRSV 基线 | `configs/lusgs/run_cfd_lusgs_step1.py` |
| `scheme/lusgs-step2-v1` | Step2 多模式族 | `configs/lusgs/run_cfd_lusgs_step2.py` |
| `scheme/lusgs-step3-v1` | coarse macro-controller | `configs/lusgs/run_cfd_lusgs_step3.py` |
| `scheme/lusgs-step4-v1` | event controller A/B/C | `configs/lusgs/run_cfd_lusgs_step4.py` |
| `scheme/streaming-main-v1` | raw event streaming，含 B1.5/B2.5/B3/B4/C1/C2-B5 | Step2 runner 的 `step2-pretransform-raw-optprep-stream` mode |

实现选择、保留和清理建议见
`projects/cfd_dsa/docs/CFD_DSA_VERSION_RETENTION_ANALYSIS.md`。
