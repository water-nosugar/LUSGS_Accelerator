#!/usr/bin/env python3
"""LU-SGS Step4 runner: event-driven macro controller skeleton."""

import os
import runpy
import sys

sys.dont_write_bytecode = True
config_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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


os.environ["GEM5_CFD_RUN_WRAPPER"] = "lusgs-step4"
os.environ["GEM5_CFD_REPORT_FOCUS"] = "patha"

_append_default("--binary", binary("test_cfd_lusgs_patha_step4_arm"))
_append_default("--bench-iters", "1")
_append_default("--cpu", "o3")

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

_append_default("--trsv5-count", "1")
_append_default("--trsv5-lat", "60")
_append_default("--trsv5-mode", "zreg-coarse")
_append_default("--trsv5-trace-solves", "16")
_append_default("--trsv5-trace-file", result("trsv5_trace.csv"))

_append_default("--lusgs-lines", "1")
_append_default("--lusgs-cells", "17")
_append_default("--lusgs-interleave", "1")
_append_default("--lusgs-check", "1")
_append_default("--lusgs-step3-stage", "A")
_append_default("--lusgs-contexts", "1")
_append_default("--lusgs-tile-cells", "1")
_append_default("--lusgs-update-q", "0")
_append_default("--lusgs-omega", "1.0")
_append_default("--lusgs-controller-count", "1")
_append_default("--lusgs-vec5-count", "1")
_append_default("--lusgs-vec5-lat", "4")
_append_default("--lusgs-controller-trace-lines", "1")
_append_default("--lusgs-controller-trace-cells", "4")
_append_default("--lusgs-controller-trace-file",
                result("lusgs_controller_trace.csv"))
_append_default("--lusgs-step4-enable", "1")
_append_default("--lusgs-step4-stage",
                _option_value("--lusgs-step3-stage", "A"))
_append_default("--lusgs-step4-trsv-real", "0")
_append_default("--lusgs-step4-vec5-real", "0")
_append_default("--lusgs-step4-watchdog-cycles", "0")
_append_default("--lusgs-event-trace-lines", "1")
_append_default("--lusgs-event-trace-cells", "4")
_append_default("--lusgs-event-trace-file",
                result("lusgs_event_trace.csv"))

_append_default("--trsv5-event-count", "1")
_append_default("--trsv5-div-count", "1")
_append_default("--trsv5-fma-count", "1")
_append_default("--trsv5-div-lat", "4")
_append_default("--trsv5-fma-lat", "3")
_append_default("--trsv5-load-lat", "2")
_append_default("--trsv5-result-lat", "1")
_append_default("--trsv5-forwarding", "1")
_append_default("--trsv5-queue-depth", "2")
_append_default("--vec5-count", "1")
_append_default("--vec5-lanes", "5")
_append_default("--vec5-copy-lat", "1")
_append_default("--vec5-sub-lat", "3")
_append_default("--vec5-axpy-lat", "4")
_append_default("--vec5-initiation-interval", "1")
_append_default("--vec5-queue-depth", "2")

script_dir = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(os.path.dirname(script_dir), "run_cfd_dsa.py")
sys.argv[0] = target
runpy.run_path(target, run_name="__main__")
