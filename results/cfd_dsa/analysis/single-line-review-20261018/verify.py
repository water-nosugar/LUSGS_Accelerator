#!/usr/bin/env python3
"""Verify review evidence, including the intentionally reproduced failures."""
import json
from pathlib import Path

from recompute import digest

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[3]
CAMPAIGN = ROOT / "results/cfd_dsa/campaigns/single-line-review-20261018"


def main():
    evidence = json.loads((OUT / "evidence.json").read_text())
    for name, expected in evidence["files"].items():
        assert digest(ROOT/name) == expected, name
    for name, expected in json.loads((OUT/"binary-provenance.json").read_text()).items():
        assert digest(ROOT/name) == expected, name
    normal = json.loads((OUT / "fresh-validation.json").read_text())
    assert len(normal) == 5
    for name, row in normal.items():
        manifest = json.loads((CAMPAIGN/name/"manifest.json").read_text())
        assert manifest["status"] == "passed" and row["max_abs"] < 1e-10
    for name in ("probe-input-width41", "probe-interface-width7", "probe-tile-width41"):
        manifest = json.loads((CAMPAIGN/name/"manifest.json").read_text())
        text = (CAMPAIGN/name/"simout.txt").read_text()
        assert manifest["status"] == "failed" and manifest["return_code"] == -6
        assert "cfd_coeff_preprocess_onchip.inc:9: panic" in text
    recovery = (CAMPAIGN/"probe-failure-cache-recovery/simout.txt").read_text()
    assert "expected=0,1,1 actual=0,0,1" in recovery
    assert "CFD_LINE_CACHE_STORE line=0 version=1 cells=1" in recovery
    assert "LUSGS_STEP2_STREAMING_FAIL" in recovery
    pivot = (CAMPAIGN/"probe-pivot-failure/simout.txt").read_text()
    assert "LUSGS_STEP2_STREAMING_FAIL" in pivot and "panic:" not in pivot
    assert [r["cache_hit"] for r in normal["capacity-n115"]["windows"]] == [0, 1]
    assert [r["cache_hit"] for r in normal["capacity-n116"]["windows"]] == [0, 0]
    print("REVIEW_EVIDENCE_VERIFIED", len(evidence["files"]), "unchanged files")
    print("10 simulations: 5 normal passes; 3 width aborts; 1 detected LU failure; 1 cache-recovery defect reproduced")


if __name__ == "__main__":
    main()
