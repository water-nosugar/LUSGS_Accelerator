# CFD-DSA Suites

Suite files define repeatable correctness, regression, sensitivity, and fault
campaigns. They are consumed by `projects/cfd_dsa/tools/run_suite.py`.

- `single-line-audit-20261015.json`: two N=65 cold-plus-six-reuse checks at
  the current tile8/DMA32 settings.
- `single-line-audit-throughput-20261015.json`: four single-parameter
  divider-II/add-latency diagnostics at the current measured configurations.
  Use `tools/check_single_line_opt.py`; interpretation and source provenance
  are in `docs/CFD_DSA_SINGLE_LINE_AUDIT_20261015.md`.
- `single-line-round2-final-stages.json`: levels 4–8, including independent
  completion/warm queues, retained reverse tile and grouped LU targets.
- `single-line-round2-tuning.json`, `single-line-round2-selection.json`:
  finite queue/context/tile/DMA/bank combinations; selection includes a
  level5 baseline with the same banked SPM timing rules.
- `single-line-round2-stress.json`: boundary lengths, single credits, finite
  cache, dirty/version/R contracts and arithmetic controls.
- `single-line-round2-arithmetic.json`: separately measured add latency,
  divider II and reciprocal experiments under the new storage model.
- `single-line-round2-recommended.json`: four measured fixed 5/5/1 arithmetic
  cold/reuse settings. Validate using `tools/check_single_line_opt.py`; it
  also invokes the per-word bank/port/readiness checker. See
  `docs/CFD_DSA_SINGLE_LINE_ROUND2_20261012.md` for capacity, hardware tradeoffs,
  same-binary results and evidence verification.
- `single-line-d-storage.json`: 24 full/tiled pairs, one/two/partial/all-fit/long
  lines and ordered/tree arithmetic. Validate finite capacity, reload responses
  and FP64 output identity with `tools/check_single_line_storage.py`.
- `single-line-d-optimization.json`: 25 cases for finite coefficient caching,
  version/dirty keys with changing R, double buffers, cache-line reloads and DMA
  pressure. Use the same checker with `--suite` and `--report`.
- `single-line-d-mode-selection.json`: 26 fixed-resource comparisons across
  line length, tile size, coefficient update interval and window. Validate with
  the storage checker, then use `tools/analyze_single_line_d.py` to compare
  complete sweep cycles and select an exact measured configuration. See
  `docs/CFD_DSA_SINGLE_LINE_D.md` for scope and timing boundaries.
- `single-line-d-tuning.json`: 10 finite timed cold cases varying DMA
  outstanding 8/16, logical contexts 1/2 and tile sizes 4/8/16, with fixed
  shared arithmetic. Validate with the storage checker.
- `single-line-d-mode-tuned.json`: 6 seven-sweep mode comparisons after raising
  DMA outstanding to 16; combine its report using `--tuned-mode-report` in
  `tools/analyze_single_line_d.py`.
- `single-line-c-complete.json`: 35 cases covering shared 5/5/1 arithmetic,
  LU reuse, serial/split/tree, full numeric validation, product pressure,
  latency/context sensitivity and DMA. The two cache-contract fixtures change
  R, coefficients, size, mode, version and dirty state. Validate with
  `tools/check_single_line_c.py`; `--report` exports physical issue counts and
  backward feedback intervals. Physical lane traces include every operation.
- `single-line-base-solve.json`: cold single-line direct `D*b=R` versus inverse,
  exact RHS/FU counts, boundary sizes, split, DMA pressure and repeated rebuilds.
  Validate with `tools/check_single_line_base_solve.py`; `--report` exports the
  simulator resource inventory. This suite explicitly rebuilds every sweep.
- `single-line-rhs-frontier.json`: fixed-resource single-line RHS arbitration
  against frontier-aware controls (17/64 cells, cold/reuse); validate with
  `tools/check_single_line_rhs_frontier.py`. Experimental mode is opt-in.
- `single-line-output-dma.json`: single-line-only final-vector timing DMA,
  bounded output queues, completion ordering and numerical controls. Validate
  with `tools/check_single_line_output_dma.py`; implementation boundaries and
  measured status are documented in `docs/CFD_DSA_SINGLE_LINE.md`.
- `core-regression.json`: cross-path/Step smoke and correctness regression.
- `coeff-event-sensitivity.json`: coefficient-controller resource sensitivity.
- `request-frontier-aging.json`: fixed-resource request-priority experiment,
  2/2 baseline and 2/4 reference; validate with
  `tools/check_request_frontier_aging.py`. Default scheduler is unchanged.
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
- `coeff-line-dma-rebuild-diagnostic.json`: historical direct-DMA multi-sweep
  rebuild reproducer (both scheduling switches off versus on). The single-line
  coherent-DMA regression now passes; see streaming documentation section 16.
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
- `single-line-opt-final-stages.json`: cumulative optimization levels 0–4 and
  optional RHS row parallelism under the same 5/5/1 shared arithmetic budget.
- `single-line-opt-tuning.json`, `single-line-opt-selection.json`: bounded
  context/tile/output/DMA scans and final parameter combinations.
- `single-line-opt-stress.json`: boundary sizes, single-credit pressure,
  serial/split/tree, cache capacity and version/dirty/R contracts.
- `single-line-opt-divider.json`, `single-line-opt-scheduler.json`: separate
  divider throughput and global RHS frontier arbitration evaluations.
- `single-line-opt-recommended.json`: four tested N=17 cold/reuse × direct/inverse
  settings. Run `tools/check_single_line_opt.py` with `--suite` and `--report`;
  details and model limits are in `docs/CFD_DSA_SINGLE_LINE_OPTIMIZATION_20261012.md`.

- `single-line-round3-stages.json`: same-binary level8/9/10/11,40B/64B transfer controls.
- `single-line-round3-tuning.json`, `single-line-round3-selection.json`: separate resource reductions and combined configurations.
- `single-line-round3-recommended.json`: tested level11 fixed-latency N=17 configurations; see `docs/CFD_DSA_SINGLE_LINE_ROUND3_20261015.md`.
- `single-line-round3-long.json`, `single-line-round3-stress.json`: N=65 finite spills, boundary sizes, credit1 and cache/version contracts.
- `single-line-round3-arithmetic.json`, `single-line-round3-endpoint.json`: timing capability sensitivities; these do not change the fixed-latency baseline.

- `single-line-round3-final.json`, `single-line-round3-final-tile.json`: final same-binary old/new pairs and output-credit A/B; `--coeff-dma-output-priority=0` restores the competing prefetch order.
- `single-line-round3-final-stress.json`: final packed-record mode/size/version contracts and finite-credit stress.
- `single-line-round3-long-recommended.json`: N=65 cold tile32 / warm tile16 configurations under the same 196608B budget.
- `single-line-round3-fast-recommended.json`: optional addlat2/divII2 capability profile; keep separate from fixed-latency comparisons.

- `single-line-round4-performance.json`: 33 fixed-timing cache/record/FIFO comparisons, with explicit A/B pairs.
- `single-line-round4-stress.json`: 24 numeric, bank/port, DMA quota and finite-credit cases. The frozen accepted set uses `single-line-round4-stress-main.json` plus the separately rerun `single-line-round4-cache-contract.json`.
- `single-line-round4-recovery.json`: 10 real LU-failure/cancellation/retry scenarios, including full-storage compatibility.
- `single-line-round4-input-tuning.json`: 12 cases comparing two/four input slots within the same 192KiB capacity and fixed arithmetic banks.
- `single-line-round4-recommended.json`: nine tested workload candidates. Defaults are preserved; summaries favor traffic reduction and are not automatically faster. See [the round4 results](../docs/CFD_DSA_SINGLE_LINE_ROUND4_20261019.md) for the capacity and latency tradeoffs.
