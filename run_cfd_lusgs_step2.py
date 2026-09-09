#!/usr/bin/env python3
"""Compatibility entrypoint for the canonical LU-SGS Step2 config."""

from pathlib import Path
import runpy
import sys

target = Path(__file__).resolve().parent / "projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step2.py"
sys.argv[0] = str(target)
runpy.run_path(str(target), run_name="__main__")
