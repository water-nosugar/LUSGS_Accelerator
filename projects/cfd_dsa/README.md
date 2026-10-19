# CFD-DSA Project Layout

This directory is the canonical home for the ARM CFD-DSA research project.
The gem5 source changes remain under `src/`; project runners, guest benchmarks,
documentation, and experiment tooling live here.

The current LU-SGS study is restricted to **one grid line with one forward and
one backward scan per sweep**. Use the `lusgs-single-line` experiment target;
see [the single-line implementation and validation notes](docs/CFD_DSA_SINGLE_LINE.md).
Stage C's shared five-lane model and LU reuse use
`--coeff-compute-model=shared` and `--coeff-base-solve=direct`; run
`suites/single-line-c-complete.json` and `tools/check_single_line_c.py` for
the physical issue, numeric and cache-contract audit.

```text
configs/       canonical gem5 configuration scripts
benchmarks/    AArch64 guest benchmark sources
docs/          design documents and measured-result reports
suites/        reusable regression and sensitivity suite definitions
tools/         benchmark build, experiment run, and result-management tools
```

Generated guest binaries are written to `build/cfd_dsa/aarch64/`. Simulation
outputs are written to `results/cfd_dsa/`; neither generated location belongs
in the source tree.

## Recommended Commands

Build all guest programs:

```bash
python3 projects/cfd_dsa/tools/build_benchmarks.py
```

Run one managed experiment:

```bash
python3 projects/cfd_dsa/tools/run_experiment.py \
  --target lusgs-step4 --campaign development --case step4c-1x64 -- \
  --lusgs-step4-stage=C --lusgs-lines=1 --lusgs-cells=64
```

The historical root-level `run_cfd_*.py` compatibility entrypoints were removed
on 2026-09-09. Use only the canonical paths in `projects/cfd_dsa/configs/`.

## Result Policy

- `baselines/` contains stable evidence referenced by documents.
- `campaigns/` contains named, immutable experiment batches.
- `runs/` contains disposable development runs.
- `failed/` contains incomplete runs, including empty `stats.txt` files.
- `archive/` contains legacy output exactly as migrated.

Do not use suffixes such as `final2`, `fix`, or `rerun` for new work. Use a
campaign name and case name; the managed runner records timestamps, the git
state, command line, and output status in `manifest.json`.
