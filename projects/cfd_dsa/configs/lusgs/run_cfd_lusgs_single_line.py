"""Scope-checked entry point for the single-line LU-SGS study."""

from pathlib import Path
import runpy
import sys

directory = Path(__file__).resolve().parent
sys.path.insert(0, str(directory))
from single_line_profile import apply_single_line_profile

try:
    apply_single_line_profile(sys.argv)
except ValueError as error:
    print(f"ERROR: {error}", file=sys.stderr)
    raise SystemExit(1) from error
sys.argv[0] = str(directory / "run_cfd_lusgs_step2.py")
runpy.run_path(sys.argv[0], run_name="__main__")
