"""Canonical LU-SGS Step2 option schema and profile expansion.

Order is ABI-like for experiment manifests: explicit user arguments remain first;
missing defaults append in this declared order. Keep aliases and dynamic defaults
compatible with the historical wrapper.
"""

STREAM_MODE = "step2-pretransform-raw-optprep-stream"

def _has_option(argv, name):
    prefix = name + "="
    return any(arg == name or arg.startswith(prefix) for arg in argv)


def _option_value(argv, name, default=None):
    prefix = name + "="
    args = argv
    for idx, arg in enumerate(args):
        if arg.startswith(prefix):
            return arg[len(prefix):]
        if arg == name and idx + 1 < len(args):
            return args[idx + 1]
    return default


def _append_default(argv, name, value):
    if not _has_option(argv, name):
        argv.extend([name, str(value)])



def apply_step2_profile(argv, binary, result):
    _append_default(argv, "--binary", binary("test_cfd_lusgs_patha_step2_arm"))
    _append_default(argv, "--bench-iters", "1")
    _append_default(argv, "--cpu", "o3")
    
    _append_default(argv, "--patha-spm-mode", "direct")
    _append_default(argv, "--patha-spm-read-width", "40")
    _append_default(argv, "--patha-store-mode", "pred40")
    _append_default(argv, "--patha-store-stride",
                    "64" if _option_value(argv, "--patha-store-mode") == "full64" else "40")
    _append_default(argv, "--patha-acc-mode", "internal-buffer")
    _append_default(argv, "--patha-result-buffer-depth", "2")
    _append_default(argv, "--patha-dotp-lat", "7")
    _append_default(argv, "--patha-dotp-count", "1")
    _append_default(argv, "--patha-pack-count", "1")
    _append_default(argv, "--patha-unroll", "8")
    _append_default(argv, "--patha-streaming", "0")
    _append_default(argv, "--patha-kernel", "baseline")
    _append_default(argv, "--patha-input-buffer-depth", "2")
    _append_default(argv, "--patha-stream-read-ports", "1")
    _append_default(argv, "--patha-store-queue-depth", "4")
    _append_default(argv, "--patha-async-store", "0")
    
    _append_default(argv, "--trsv5-count", "1")
    _append_default(argv, "--trsv5-lat", "60")
    _append_default(argv, "--trsv5-mode", "zreg-coarse")
    _append_default(argv, "--trsv5-trace-solves", "16")
    _append_default(argv, "--trsv5-trace-file", result("trsv5_trace.csv"))
    _append_default(argv, "--trsm5-mrhs-enable", "1")
    _append_default(argv, "--trsm5-mrhs-lat", "100")
    _append_default(argv, "--trsm5-mrhs-count", "1")
    _append_default(argv, "--trsm5-mrhs-dual-enable", "1")
    _append_default(argv, "--trsm5-mrhs-dual-lat", "200")
    _append_default(argv, "--trsm5-mrhs-dual-count", "1")
    _append_default(argv, "--trsm5-coeff3-enable", "1")
    _append_default(argv, "--trsm5-coeff3-lat", "300")
    _append_default(argv, "--trsm5-coeff3-count", "1")
    _append_default(argv, "--vec5-lat", "2")
    _append_default(argv, "--vec5-count", "1")
    _append_default(argv, "--pretransform-fallback-trsv5", "1")
    _append_default(argv, "--pretransform-diag-epsilon", "1e-12")
    _append_default(argv, "--pretransform-per-sweep", "0")
    _append_default(argv, "--pretransform-output-buffer-enable", "1")
    _append_default(argv, "--pretransform-output-buffer-depth", "4")
    _append_default(argv, "--pretransform-spm-slots", "2")
    _append_default(argv, "--pretransform-overlap-enable", "1")
    _append_default(argv, "--pretransform-use-barrier", "0")
    _append_default(argv, "--pretransform-validate", "full")
    _append_default(argv, "--coeff-preprocess-spm-slots", "2")
    _append_default(argv, "--coeff-output-buffer-depth", "4")
    _append_default(argv, "--coefficient-update-interval", "0")
    _stream_mode = (_option_value(argv, "--lusgs-mode") ==
                    "step2-pretransform-raw-optprep-stream")
    _append_default(argv, "--coeff-preprocess-model", "event" if _stream_mode else "coarse")
    _append_default(argv, "--lu5-model", "event" if _stream_mode else "software")
    _append_default(argv, "--coeff-validation", "full")
    _append_default(argv, "--coeff-validation-sample-rate", "16")
    _append_default(argv, "--coeff-input-slots", "2")
    _append_default(argv, "--lu5-count", "1")
    _append_default(argv, "--lu5-pending-depth", "2")
    _append_default(argv, "--lu5-div-count", "1")
    _append_default(argv, "--lu5-div-lat", "12")
    _append_default(argv, "--lu5-div-ii", "12")
    _append_default(argv, "--lu5-mulsub-count", "1")
    _append_default(argv, "--lu5-mul-lat", "3")
    _append_default(argv, "--lu5-sub-lat", "4")
    _append_default(argv, "--coeff3-count", "1")
    _append_default(argv, "--coeff3-pending-depth", "2")
    _append_default(argv, "--coeff3-rhs-lanes", "15")
    _append_default(argv, 
        "--coeff3-schedule",
        "frontier-aware"
        if _option_value(argv, "--coeff-stream-line-autonomous", "0") == "1"
        else "round-robin-ready")
    _append_default(argv, "--coeff3-div-count", "1")
    _append_default(argv, "--coeff3-div-lat", "12")
    _append_default(argv, "--coeff3-div-ii", "4")
    _append_default(argv, "--coeff3-div-mode", "divide")
    _append_default(argv, "--coeff-mulsub-count", "1")
    _append_default(argv, "--coeff-mul-lat", "3")
    _append_default(argv, "--coeff-sub-lat", "4")
    _append_default(argv, "--coeff3-partial-output", "1")
    _append_default(argv, "--lu-forwarding", "1")
    _append_default(argv, "--lu-solve-early-start", "1")
    _append_default(argv, "--coeff-spm-read-ports", "1")
    _append_default(argv, "--coeff-spm-write-ports", "1")
    _append_default(argv, "--coeff-spm-write-width", "40")
    _append_default(argv, "--coeff-spm-model", "internal")
    _append_default(argv, "--coeff-spm-layout", "legacy")
    _append_default(argv, "--coeff-spm-outstanding", "4")
    _append_default(argv, "--coeff-spm-banks", "4")
    _append_default(argv, "--coeff-spm-bank-granularity", "64")
    _append_default(argv, "--coeff-source-read-lat", "2")
    _append_default(argv, "--coeff-spm-write-lat", "1")
    _append_default(argv, "--coeff-drain-width", "40")
    _append_default(argv, "--coeff-drain-ports", "1")
    _append_default(argv, "--coeff-drain-lat", "1")
    _append_default(argv, "--coeff-drain-outstanding", "4")
    _append_default(argv, "--coeff-drain-queue-depth", "4")
    _append_default(argv, "--coeff-preprocess-trace-enable", "0")
    _append_default(argv, "--coeff-cancel-test", "0")
    _append_default(argv, "--coeff-streaming-enable", "1" if _stream_mode else "0")
    _append_default(argv, "--coeff-stream-line-autonomous", "0")
    _append_default(argv, "--coeff-stream-window", "8")
    _append_default(argv, "--coeff-stream-retire-mode", "auto")
    _append_default(argv, "--coeff-stream-boundary-mask", "1")
    _append_default(argv, "--coeff-stream-dinv-consumer", "off")
    _append_default(argv, "--coeff-stream-lbar-consumer", "off")
    _append_default(argv, "--coeff-stream-ubar-consumer", "off")
    _append_default(argv, "--coeff-column-fma-latency", "4")
    _append_default(argv, "--coeff-column-fma-ii", "1")
    _append_default(argv, "--coeff-column-fma-count", "1")
    _append_default(argv, "--coeff-column-fma-queue-depth", "5")
    _append_default(argv, "--forward-combine-latency", "1")
    _append_default(argv, "--forward-combine-ii", "1")
    _append_default(argv, "--forward-combine-count", "1")
    _append_default(argv, "--forward-combine-queue-depth", "2")
    _append_default(argv, "--coeff-preprocess-trace-cells", "4")
    _append_default(argv, "--coeff-preprocess-trace-file",
                    result("coeff_preprocess_trace.csv"))
    _append_default(argv, "--ubar-inplace", "0")
    if not (_has_option(argv, "--lusgs-preprocess-auto") or
            _has_option(argv, "--lusgs-pretransform-auto")):
        _append_default(argv, "--lusgs-preprocess-auto", "0")
    if not (_has_option(argv, "--lusgs-coeff-expected-sweeps") or
            _has_option(argv, "--lusgs-expected-sweeps")):
        _append_default(argv, "--lusgs-coeff-expected-sweeps", "1")
    
    _append_default(argv, "--lusgs-lines", "1")
    _append_default(argv, "--lusgs-cells", "17")
    _append_default(argv, "--lusgs-interleave", "1")
    _append_default(argv, "--lusgs-check", "1")
    _append_default(argv, "--lusgs-mode", "all")
    _append_default(argv, "--lusgs-linebuf-enable", "1")
    _append_default(argv, "--lusgs-linebuf-entries", "16")
    _append_default(argv, "--lusgs-trace-lines", "1")
    _append_default(argv, "--lusgs-trace-cells", "4")
    
    return argv
