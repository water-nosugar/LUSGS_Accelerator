#!/usr/bin/env python3
"""Path B runner for the CFD-DSA SME-ZA spatial pipeline benchmark.

Path B reuses the Path C SME/ZA framework but changes only the custom k-step:
partial sums flow through ZA columns 0..4 and the final MOVA reads ZA[:,4].
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


def _append_default(name, value):
    if not _has_option(name):
        sys.argv.extend([name, value])


os.environ["GEM5_CFD_RUN_WRAPPER"] = "pathb"
os.environ["GEM5_CFD_REPORT_FOCUS"] = "pathb"
os.environ["GEM5_CFD_PATHB_ENABLE"] = "1"

_append_default("--binary", binary("test_cfd_pathb_za_pipe_arm"))
_append_default("--spm-access-mode", "local-event")
_append_default("--local-spm-lat", "1")
_append_default("--local-spm-read-ports", "2")
_append_default("--local-spm-read-width", "40")
_append_default("--local-spm-outstanding", "8")
_append_default("--local-spm-queue-size", "8")
_append_default("--local-spm-banks", "8")
_append_default("--local-spm-bank-granularity", "40")
_append_default("--local-spm-bank-mapping", "row")
_append_default("--local-spm-z-wb-ports", "2")
_append_default("--za-state-mode", "rename")
_append_default("--sme-pipe-lat", "1")
_append_default("--sme-pipe-count", "1")
_append_default("--sme-pipe-pipelined", "true")

script_dir = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(script_dir, "run_cfd_dsa.py")
sys.argv[0] = target
runpy.run_path(target, run_name="__main__")
