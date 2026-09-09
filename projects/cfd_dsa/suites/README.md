# CFD-DSA Suites

Suite files define repeatable correctness, regression, sensitivity, and fault
campaigns. They are consumed by `projects/cfd_dsa/tools/run_suite.py`.

- `core-regression.json`: cross-path/Step smoke and correctness regression.
- `coeff-event-sensitivity.json`: coefficient-controller resource sensitivity.
- `coeff-context-ablation.json`: fixed-FU LU/TRSM context 1/2/4 matrix,
  cold/mixed sweeps and observer-off control; validate with
  `tools/check_context_ablation.py`. See `docs/CFD_DSA_CONTEXT_ABLATION.md`.
- `coeff-line-mvm-pipeline.json`: experimental split line-MVM arithmetic,
  latency/depth sensitivity, numeric digests, exact operation counts and
  lifecycle checks via `tools/check_line_mvm_pipeline.py`.
- `coeff-line-dependency.json`: autonomous-line base-ahead/early-backward
  ablation, cache hit/dirty rebuild, short lines, window one, multi-line,
  slow drain and DMA. Run `tools/check_line_dependency.py` on the resulting
  campaign to check result digests and compute/retirement ordering.
- `coeff-line-dma-rebuild-diagnostic.json`: known-failing DMA multi-sweep
  rebuild reproducer (both scheduling switches off versus on). This is not
  a passing core regression; see streaming documentation section 16.
- `coeff-streaming-correctness.json`: Step2 Streaming Stage A boundary sizes,
  window depths, and internal packet/backpressure correctness.
- `coeff-streaming-boundary-mask.json`: Stage B2 N=1/head/interior/tail RHS
  worklist and drain-skip correctness regression.
- `coeff-streaming-reaper.json`: Stage B1 detached completion lifetime plus
  single-entry output/packet/SPM backpressure regression. It pins
  `--coeff-stream-retire-mode=queue` so the legacy pressure path remains live.
- `coeff-streaming-auto-retire.json`: Stage B1.5/B2.5 stable completion-record
  auto-retire correctness across boundary/window/pressure configurations.
- `coeff-streaming-dinv-consumer.json`: Stage B3 DInv ColumnFma5 consumer
  off/shadow/direct coverage, latency/II/queue sensitivity, real R/base SPM
  pressure, partial-column cancel/generation, and out-of-order-ready checks.
- `coeff-streaming-lbar-consumer.json`: Stage B4 Lbar ColumnFma5/forward
  accumulator off/shadow/direct coverage, controller-local adjacent-cell
  dq-star handoff, ForwardCombine and timed dq-star output, plus window,
  shared-resource, packet/backpressure, cancel/generation, and ready-order
  pressure and explicit wrong-cell/wrong-generation handoff injection. The
  four deterministic cancel phases. The 33-case suite is the required B4
  regression.
- `step4c-sensitivity.json`: Step4-C event-controller exploration.
