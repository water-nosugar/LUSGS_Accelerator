#!/usr/bin/env python3
"""Standalone TRSV5 runner for LU-SGS Step2-A validation."""

import os
import runpy
import sys

sys.dont_write_bytecode = True
config_dir = os.path.dirname(os.path.abspath(__file__))
if config_dir not in sys.path:
    sys.path.insert(0, config_dir)
from _paths import binary, result


def _has_option(name):
    prefix = name + "="
    return any(arg == name or arg.startswith(prefix) for arg in sys.argv[1:])


def _option_value(name, default=None):
    prefix = name + "="
    args = sys.argv[1:]
    for idx, arg in enumerate(args):
        if arg.startswith(prefix):
            return arg[len(prefix):]
        if arg == name and idx + 1 < len(args):
            return args[idx + 1]
    return default


def _append_default(name, value):
    if not _has_option(name):
        sys.argv.extend([name, value])


def _translate_solves_option():
    solves = _option_value("--trsv5-solves")
    if solves is None:
        return
    filtered = [sys.argv[0]]
    args = sys.argv[1:]
    idx = 0
    while idx < len(args):
        arg = args[idx]
        if arg == "--trsv5-solves":
            idx += 2
            continue
        if arg.startswith("--trsv5-solves="):
            idx += 1
            continue
        filtered.append(arg)
        idx += 1
    sys.argv[:] = filtered
    if not _has_option("--bench-iters"):
        sys.argv.extend(["--bench-iters", solves])


os.environ["GEM5_CFD_RUN_WRAPPER"] = "trsv5"
os.environ["GEM5_CFD_REPORT_FOCUS"] = "patha"

_translate_solves_option()

_append_default("--binary", binary("test_cfd_trsv5_arm"))
_append_default("--bench-iters", "1")
_append_default("--cpu", "o3")

_append_default("--trsv5-count", "1")
_append_default("--trsv5-lat", "60")
_append_default("--trsv5-mode", "zreg-coarse")
_append_default("--trsv5-trace-solves", "16")
_append_default("--trsv5-trace-file", result("trsv5_trace.csv"))

script_dir = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(script_dir, "run_cfd_dsa.py")
sys.argv[0] = target
runpy.run_path(target, run_name="__main__")
