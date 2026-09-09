# CFD-DSA Results

```text
baselines/  stable results referenced by documentation
campaigns/  complete named experiment batches
runs/       disposable development runs
failed/     incomplete, crashed, or empty-stat runs
archive/    read-only legacy migration
```

Every managed run contains `manifest.json`, simulator output, gem5 config
files, and `stats.txt`. A campaign/case path is immutable; start a new campaign
instead of appending `fix`, `rerun`, or `final` suffixes.

`baseline-index.json` is the stable lookup table for documentation. The full
legacy move map is recorded in `migration-inventory.json`; no legacy result was
deleted during the 2026-07-05 migration.
