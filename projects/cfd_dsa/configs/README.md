# CFD-DSA gem5 Configurations

`run_cfd_dsa.py` owns the shared gem5 system configuration. The Path A/B/C,
TRSV5, and LU-SGS scripts are small policy wrappers around that common entry.

Use these files directly with `build/ARM/gem5.opt`, or use
`../tools/run_experiment.py` to create a managed result directory and manifest.

For the current single-line LU-SGS study, use `lusgs/run_cfd_lusgs_single_line.py`
or managed target `lusgs-single-line`. This entry rejects multiple lines and
multi-line interleaving. Each sweep executes one forward and one backward scan.

`--coeff-base-solve=direct` replaces the five inverse RHS with a single `D*b=R`
solve. Default is `inverse`. Repeated sweeps with an explicit update interval
can reuse LU/Lbar/Ubar; cache keys include the base mode, size and version,
and dirty always rebuilds. See `single-line-base-solve.json` for the cold
comparison and `single-line-c-complete.json` for reuse and shared computation.

`--coeff-compute-model=shared` routes LU/TRSM/MVM/vector arithmetic to five
multiplier lanes, five add/sub lanes and one divider. Cluster latencies use
`--coeff-cluster-{mul,add,div}-lat`; divider II uses `--coeff-cluster-div-ii`.
The old per-engine FU settings apply only to the default `legacy` model.
`--coeff-cluster-product-depth` bounds pending column products.
`--coeff-line-mvm-model=serial|split|tree` selects ordered baseline, products
ahead with ordered addition, or tree reduction for the parent line engine;
tree requires `--coeff-line-product-depth=5` and separate tolerance validation.
The controller's cold child Lbar/base path retains ordered column consumption.

`--coeff3-schedule=rhs-frontier` enables the experimental global RHS
arbitration for this study. The fixed-resource baseline remains
`frontier-aware`; see `../docs/CFD_DSA_SINGLE_LINE.md`.

Round4 retains the existing defaults. The first three data-path alternatives
below require opt level11; global DMA quotas also apply to compatibility modes:

- `--coeff-line-cache-policy=full|adaptive`: adaptive direct mode keeps LU/Lbar when the full cache cannot fit; Ubar reads remain timed and budgeted.
- `--coeff-warm-y-source=spm|fifo`: use the existing bounded result snapshot for inverse warm DMA, while retaining the SPM store and completion fence.
- `--coeff-record-policy=per-cell|warm-summary`: direct warm summary uses a 64B line record; cold diagnostics remain per-cell. It reduces traffic but is not a general latency improvement.
- `--coeff-dma-issue-packets=0..64` and `--coeff-dma-issue-bytes=0|64..4096`: global accepted-request quotas per controller cycle; nonzero byte limits must be divisible by 8. Zero preserves the old unbounded ingress assumption.
- With onchip timing enabled, input, tile-read and interface widths must be divisible by 8; 40B is supported and cache-line/page boundaries can still split requests.

The recommended suite explicitly uses one packet/64B per cycle. Its four-input-slot direct candidates charge an additional 16640B to the same 196608B budget; do not apply that setting to arbitrary line lengths without rechecking cache residency. `--coeff-input-pattern=baseline|random|scaled` and `--coeff-input-seed` select validation data, and `--coeff-poison-outputs=1` detects stale/missing output stores. Reference LU/Ubar are kept in a separate software workspace. Details: [round4 report](../docs/CFD_DSA_SINGLE_LINE_ROUND4_20261019.md).
