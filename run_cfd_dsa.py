#!/usr/bin/env python3
"""Compatibility entrypoint; use projects/cfd_dsa/configs/run_cfd_dsa.py."""

from pathlib import Path
import runpy
import sys

target = Path(__file__).resolve().parent / "projects/cfd_dsa/configs/run_cfd_dsa.py"
sys.argv[0] = str(target)
runpy.run_path(str(target), run_name="__main__")
