# CFD-DSA File and Result Management

## Ownership Boundaries

| Content | Canonical location |
| --- | --- |
| gem5 C++/ISA implementation | `src/` |
| gem5 experiment configs | `projects/cfd_dsa/configs/` |
| AArch64 benchmark sources | `projects/cfd_dsa/benchmarks/` |
| generated AArch64 binaries | `build/cfd_dsa/aarch64/` |
| design and result documents | `projects/cfd_dsa/docs/` |
| reusable experiment suites | `projects/cfd_dsa/suites/` |
| generated simulation results | `results/cfd_dsa/` |

The root-level `run_cfd_*.py` files are compatibility shims only. Do not add
logic to them.

## Result Lifecycle

1. Development runs go to `results/cfd_dsa/runs/` and may be discarded.
2. Complete experiment batches go to `campaigns/<campaign>/<case>/` and are
   immutable.
3. Results cited by a document are listed in `baseline-index.json` and exposed
   under `baselines/`.
4. Failed and incomplete runs go to `failed/`; a zero-byte `stats.txt` is never
   treated as a baseline.
5. Superseded campaigns are moved intact to a dated `archive/` directory.

No result is named `final`, `final2`, `fix`, or `rerun`. Start a new campaign
or case with a descriptive name.

## Managed Run

```bash
python3 projects/cfd_dsa/tools/run_experiment.py \
  --target lusgs-step4 \
  --campaign step4c-validation \
  --case correctness-1x64 -- \
  --lusgs-step4-stage=C --lusgs-lines=1 --lusgs-cells=64
```

Named campaign paths are immutable. The special `development` campaign adds a
UTC timestamp and is suitable for disposable work.

## Reusable Suites

```bash
python3 projects/cfd_dsa/tools/run_suite.py \
  projects/cfd_dsa/suites/core-regression.json \
  --campaign core-regression-2026-07-05
```

## Legacy Migration

The 2026-07-05 migration moved 150 top-level output directories without
deleting data. The exact source-to-destination mapping, original size, category,
and stats status are in `results/cfd_dsa/migration-inventory.json`.

```text
archive:  89
campaign: 55
baseline:  2
failed:    4
total:   150 directories, 237706260 bytes before migration manifests
```

Step4-C baselines are symbolic links to immutable campaign cases, so there is
only one physical copy of each raw result.
