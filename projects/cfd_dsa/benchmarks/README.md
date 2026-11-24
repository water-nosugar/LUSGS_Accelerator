# CFD-DSA Guest Benchmarks

Benchmark sources are grouped by architectural path. Generated AArch64 static
binaries are placed in `build/cfd_dsa/aarch64/` by
`projects/cfd_dsa/tools/build_benchmarks.py`.

Compatibility benchmark sources are retained and labeled; they are not used as
the canonical source for new experiments.

The raw single-line streaming benchmark also accepts
`--coeff-input-file=/absolute/path/input.cfd5`. The file supplies D/L/U/R and
an independent numerical oracle. Format, GT01R derivation and reproducible
commands are documented in
[`CFD_DSA_GT01R_20261022.md`](../docs/CFD_DSA_GT01R_20261022.md).
Larger real-coefficient projections and synthetic tests up to 4096 cells are
described in [`CFD_DSA_SCALE_20261022.md`](../docs/history/CFD_DSA_SCALE_20261022.md).
