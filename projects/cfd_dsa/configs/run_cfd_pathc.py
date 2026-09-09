#!/usr/bin/env python3
"""Path C runner for the CFD-DSA SME-ZA selected-column outer pipeline.

This wrapper keeps the new Path C ZA outer-product-style pipeline separate
from Path A and Path B.  The deprecated fixed-ZA[:,0] Path C benchmark is not
the default entry point.
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


os.environ["GEM5_CFD_RUN_WRAPPER"] = "pathc"
os.environ["GEM5_CFD_REPORT_FOCUS"] = "pathc"
os.environ["GEM5_CFD_PATHC_OUTER_ENABLE"] = "1"

_append_default("--binary", binary("test_cfd_pathc_za_outer_arm"))
_append_default("--spm-access-mode", "local-event")
_append_default("--local-spm-lat", "1")
_append_default("--local-spm-read-ports", "2")
_append_default("--local-spm-read-width", "40")
_append_default("--local-spm-outstanding", "8")
_append_default("--local-spm-queue-size", "8")
_append_default("--local-spm-banks", "8")
_append_default("--local-spm-bank-granularity", "40")
_append_default("--local-spm-bank-mapping", "xor")
_append_default("--local-spm-z-wb-ports", "2")
_append_default("--za-state-mode", "rename")
_append_default("--sme-outer-lat", "1")
_append_default("--sme-outer-count", "1")
_append_default("--sme-outer-pipelined", "true")
_append_default("--sme-outer-full-array", "false")

script_dir = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(script_dir, "run_cfd_dsa.py")
sys.argv[0] = target
runpy.run_path(target, run_name="__main__")
