#!/usr/bin/env python3
"""Path A runner for the CFD-DSA dotp-row benchmark.

This wrapper keeps Path A command lines and reports separate from the SME-ZA
Path C flow while reusing the shared gem5 system configuration in
run_cfd_dsa.py.
"""

import os
import runpy
import sys

sys.dont_write_bytecode = True
config_dir = os.path.dirname(os.path.abspath(__file__))
if config_dir not in sys.path:
    sys.path.insert(0, config_dir)
from _paths import binary


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


os.environ["GEM5_CFD_RUN_WRAPPER"] = "patha"
os.environ["GEM5_CFD_REPORT_FOCUS"] = "patha"

_append_default("--binary", binary("test_cfd_patha_extreme_arm"))
_append_default("--patha-spm-mode", "direct")
_append_default("--patha-spm-read-width", "40")
_append_default("--patha-store-mode", "pred40")
_append_default("--patha-store-stride",
                "64" if _option_value("--patha-store-mode") == "full64" else "40")
_append_default("--patha-acc-mode", "internal-buffer")
_append_default("--patha-result-buffer-depth", "2")
_append_default("--patha-dotp-lat", "7")
_append_default("--patha-dotp-count", "1")
_append_default("--patha-pack-count", "1")
_append_default("--patha-unroll", "8")
_append_default("--patha-streaming", "0")
_append_default("--patha-kernel", "baseline")
_append_default("--patha-input-buffer-depth", "2")
_append_default("--patha-stream-read-ports", "1")
_append_default("--patha-store-queue-depth", "4")
_append_default("--patha-async-store", "0")

script_dir = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(script_dir, "run_cfd_dsa.py")
sys.argv[0] = target
runpy.run_path(target, run_name="__main__")
