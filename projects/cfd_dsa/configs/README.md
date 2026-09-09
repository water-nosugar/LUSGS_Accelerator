# CFD-DSA gem5 Configurations

`run_cfd_dsa.py` owns the shared gem5 system configuration. The Path A/B/C,
TRSV5, and LU-SGS scripts are small policy wrappers around that common entry.

Use these files directly with `build/ARM/gem5.opt`, or use
`../tools/run_experiment.py` to create a managed result directory and manifest.
