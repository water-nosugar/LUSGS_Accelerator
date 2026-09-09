#!/usr/bin/env python3
"""LU-SGS Step2 runner with baseline, fused, RHS, line-context, and line-buffer modes."""

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


os.environ["GEM5_CFD_RUN_WRAPPER"] = "lusgs-step2"
os.environ["GEM5_CFD_REPORT_FOCUS"] = "patha"

_append_default("--binary", binary("test_cfd_lusgs_patha_step2_arm"))
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
_append_default("--trsm5-mrhs-enable", "1")
_append_default("--trsm5-mrhs-lat", "100")
_append_default("--trsm5-mrhs-count", "1")
_append_default("--trsm5-mrhs-dual-enable", "1")
_append_default("--trsm5-mrhs-dual-lat", "200")
_append_default("--trsm5-mrhs-dual-count", "1")
_append_default("--trsm5-coeff3-enable", "1")
_append_default("--trsm5-coeff3-lat", "300")
_append_default("--trsm5-coeff3-count", "1")
_append_default("--vec5-lat", "2")
_append_default("--vec5-count", "1")
_append_default("--pretransform-fallback-trsv5", "1")
_append_default("--pretransform-diag-epsilon", "1e-12")
_append_default("--pretransform-per-sweep", "0")
_append_default("--pretransform-output-buffer-enable", "1")
_append_default("--pretransform-output-buffer-depth", "4")
_append_default("--pretransform-spm-slots", "2")
_append_default("--pretransform-overlap-enable", "1")
_append_default("--pretransform-use-barrier", "0")
_append_default("--pretransform-validate", "full")
_append_default("--coeff-preprocess-spm-slots", "2")
_append_default("--coeff-output-buffer-depth", "4")
_append_default("--coefficient-update-interval", "0")
_stream_mode = (_option_value("--lusgs-mode") ==
                "step2-pretransform-raw-optprep-stream")
_append_default("--coeff-preprocess-model", "event" if _stream_mode else "coarse")
_append_default("--lu5-model", "event" if _stream_mode else "software")
_append_default("--coeff-validation", "full")
_append_default("--coeff-validation-sample-rate", "16")
_append_default("--coeff-input-slots", "2")
_append_default("--lu5-count", "1")
_append_default("--lu5-pending-depth", "2")
_append_default("--lu5-div-count", "1")
_append_default("--lu5-div-lat", "12")
_append_default("--lu5-div-ii", "12")
_append_default("--lu5-mulsub-count", "1")
_append_default("--lu5-mul-lat", "3")
_append_default("--lu5-sub-lat", "4")
_append_default("--coeff3-count", "1")
_append_default("--coeff3-pending-depth", "2")
_append_default("--coeff3-rhs-lanes", "15")
_append_default(
    "--coeff3-schedule",
    "frontier-aware"
    if _option_value("--coeff-stream-line-autonomous", "0") == "1"
    else "round-robin-ready")
_append_default("--coeff3-div-count", "1")
_append_default("--coeff3-div-lat", "12")
_append_default("--coeff3-div-ii", "4")
_append_default("--coeff3-div-mode", "divide")
_append_default("--coeff-mulsub-count", "1")
_append_default("--coeff-mul-lat", "3")
_append_default("--coeff-sub-lat", "4")
_append_default("--coeff3-partial-output", "1")
_append_default("--lu-forwarding", "1")
_append_default("--lu-solve-early-start", "1")
_append_default("--coeff-spm-read-ports", "1")
_append_default("--coeff-spm-write-ports", "1")
_append_default("--coeff-spm-write-width", "40")
_append_default("--coeff-spm-model", "internal")
_append_default("--coeff-spm-layout", "legacy")
_append_default("--coeff-spm-outstanding", "4")
_append_default("--coeff-spm-banks", "4")
_append_default("--coeff-spm-bank-granularity", "64")
_append_default("--coeff-source-read-lat", "2")
_append_default("--coeff-spm-write-lat", "1")
_append_default("--coeff-drain-width", "40")
_append_default("--coeff-drain-ports", "1")
_append_default("--coeff-drain-lat", "1")
_append_default("--coeff-drain-outstanding", "4")
_append_default("--coeff-drain-queue-depth", "4")
_append_default("--coeff-preprocess-trace-enable", "0")
_append_default("--coeff-cancel-test", "0")
_append_default("--coeff-streaming-enable", "1" if _stream_mode else "0")
_append_default("--coeff-stream-line-autonomous", "0")
_append_default("--coeff-stream-window", "8")
_append_default("--coeff-stream-retire-mode", "auto")
_append_default("--coeff-stream-boundary-mask", "1")
_append_default("--coeff-stream-dinv-consumer", "off")
_append_default("--coeff-stream-lbar-consumer", "off")
_append_default("--coeff-stream-ubar-consumer", "off")
_append_default("--coeff-column-fma-latency", "4")
_append_default("--coeff-column-fma-ii", "1")
_append_default("--coeff-column-fma-count", "1")
_append_default("--coeff-column-fma-queue-depth", "5")
_append_default("--forward-combine-latency", "1")
_append_default("--forward-combine-ii", "1")
_append_default("--forward-combine-count", "1")
_append_default("--forward-combine-queue-depth", "2")
_append_default("--coeff-preprocess-trace-cells", "4")
_append_default("--coeff-preprocess-trace-file",
                result("coeff_preprocess_trace.csv"))
_append_default("--ubar-inplace", "0")
if not (_has_option("--lusgs-preprocess-auto") or
        _has_option("--lusgs-pretransform-auto")):
    _append_default("--lusgs-preprocess-auto", "0")
if not (_has_option("--lusgs-coeff-expected-sweeps") or
        _has_option("--lusgs-expected-sweeps")):
    _append_default("--lusgs-coeff-expected-sweeps", "1")

_append_default("--lusgs-lines", "1")
_append_default("--lusgs-cells", "17")
_append_default("--lusgs-interleave", "1")
_append_default("--lusgs-check", "1")
_append_default("--lusgs-mode", "all")
_append_default("--lusgs-linebuf-enable", "1")
_append_default("--lusgs-linebuf-entries", "16")
_append_default("--lusgs-trace-lines", "1")
_append_default("--lusgs-trace-cells", "4")

script_dir = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(os.path.dirname(script_dir), "run_cfd_dsa.py")
sys.argv[0] = target
runpy.run_path(target, run_name="__main__")
