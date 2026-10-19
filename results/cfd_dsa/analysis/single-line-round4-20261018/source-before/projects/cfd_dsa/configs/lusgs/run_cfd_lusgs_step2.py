#!/usr/bin/env python3
"""LU-SGS Step2 runner; defaults live in step2_profile.py."""

import os
import runpy
import sys

sys.dont_write_bytecode = True
config_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if config_dir not in sys.path:
    sys.path.insert(0, config_dir)
from _paths import binary, result
from lusgs.step2_profile import apply_step2_profile

os.environ["GEM5_CFD_RUN_WRAPPER"] = "lusgs-step2"
os.environ["GEM5_CFD_REPORT_FOCUS"] = "patha"
sys.argv[1:] = apply_step2_profile(sys.argv[1:], binary, result)

script_dir = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(os.path.dirname(script_dir), "run_cfd_dsa.py")
sys.argv[0] = target
runpy.run_path(target, run_name="__main__")
