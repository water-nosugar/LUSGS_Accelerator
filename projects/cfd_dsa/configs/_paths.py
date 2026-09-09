"""Stable filesystem locations shared by CFD-DSA gem5 configs."""

from __future__ import annotations

import os
from pathlib import Path


CONFIG_ROOT = Path(__file__).resolve().parent
REPO_ROOT = CONFIG_ROOT.parents[2]
BINARY_ROOT = REPO_ROOT / "build/cfd_dsa/aarch64"
RESULT_ROOT = Path(os.environ.get(
    "GEM5_CFD_RESULTS_DIR",
    REPO_ROOT / "results/cfd_dsa/runs/compat"))
RESULT_ROOT.mkdir(parents=True, exist_ok=True)


def binary(name: str) -> str:
    return str(BINARY_ROOT / name)


def result(name: str) -> str:
    return str(RESULT_ROOT / name)
