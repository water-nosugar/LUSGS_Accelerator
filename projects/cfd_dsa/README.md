# CFD-DSA Project Layout

This directory is the canonical home for the ARM CFD-DSA research project.
The gem5 source changes remain under `src/`; project runners, guest benchmarks,
documentation, and experiment tooling live here.

```text
configs/       gem5 configuration scripts and compatibility wrappers
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

The historical root-level `run_cfd_*.py` files are thin compatibility
entrypoints. New commands and documentation should use the canonical paths in
`projects/cfd_dsa/configs/`.

## Result Policy

- `baselines/` contains stable evidence referenced by documents.
- `campaigns/` contains named, immutable experiment batches.
- `runs/` contains disposable development runs.
- `failed/` contains incomplete runs, including empty `stats.txt` files.
- `archive/` contains legacy output exactly as migrated.

Do not use suffixes such as `final2`, `fix`, or `rerun` for new work. Use a
campaign name and case name; the managed runner records timestamps, the git
state, command line, and output status in `manifest.json`.
