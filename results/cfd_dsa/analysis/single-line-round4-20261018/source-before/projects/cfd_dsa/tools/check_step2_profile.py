#!/usr/bin/env python3
"""Prove the extracted Step2 profile preserves the historical final argv."""
import os
from pathlib import Path
import subprocess
import sys

CONFIG = Path(__file__).resolve().parents[1] / "configs"
sys.path.insert(0, str(CONFIG))
from _paths import binary, result
from lusgs.step2_profile import apply_step2_profile

BASELINE = (
    "0f75982f97c8257263b9f57a47858ed3db02d6b7:"
    "projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step2.py")


def old_expand(source, args):
    prefix = source[:source.index("\nscript_dir =")]
    saved = sys.argv
    try:
        sys.argv = ["run_cfd_lusgs_step2.py", *args]
        namespace = {
            "__name__": "step2_profile_baseline",
            "__file__": str(CONFIG / "lusgs" / "run_cfd_lusgs_step2.py"),
        }
        exec(compile(prefix, BASELINE, "exec"), namespace)
        return sys.argv[1:]
    finally:
        sys.argv = saved


def check():
    source = subprocess.check_output(
        ["git", "show", BASELINE], cwd=CONFIG.parents[2], text=True)
    cases = [
        [],
        ["--lusgs-mode=step2-pretransform-raw-optprep-stream"],
        ["--lusgs-mode", "step2-pretransform-raw-optprep-stream",
         "--coeff-stream-line-autonomous=1"],
        ["--patha-store-mode=full64", "--lu5-count=2", "--coeff3-count=2"],
        ["--lusgs-pretransform-auto=1", "--lusgs-expected-sweeps=3"],
        ["--coeff3-schedule=request-frontier-aging",
         "--coeff-preprocess-model=event", "--lu5-model=event"],
    ]
    for args in cases:
        expected = old_expand(source, list(args))
        actual = apply_step2_profile(list(args), binary, result)
        assert actual == expected, (args, expected, actual)
    print(f"STEP2_PROFILE_COMPAT_PASS cases={len(cases)}")


if __name__ == "__main__":
    check()
