"""The single-line study: one forward and one reverse recurrence per sweep."""

from step2_profile import _append_default, _option_value


def apply_single_line_profile(argv):
    # Inspect every occurrence, not just the first or last, so repeated options
    # cannot silently turn a single-line experiment into an interleaved run.
    for name in ("--lusgs-lines", "--lusgs-interleave"):
        for index, arg in enumerate(argv):
            if arg == name:
                if index + 1 >= len(argv) or argv[index + 1] != "1":
                    raise ValueError(f"single-line study requires {name}=1")
            elif arg.startswith(name + "=") and arg.split("=", 1)[1] != "1":
                raise ValueError(f"single-line study requires {name}=1")
        _append_default(argv, name, "1")
    level = int(_option_value(argv, "--coeff-line-opt-level", "0"))
    shared = _option_value(argv, "--coeff-compute-model", "legacy") == "shared"
    dividers = int(_option_value(argv, "--coeff-cluster-div-count",
                   _option_value(argv, "--coeff3-div-count", "1")))
    _append_default(argv, "--coeff-divider-model",
                    "pipelined" if shared and dividers == 1 else "timing")
    for name, value in (
        ("--lusgs-mode", "step2-pretransform-raw-optprep-stream"),
        ("--coeff-preprocess-model", "event"), ("--lu5-model", "event"),
        ("--coeff-stream-line-autonomous", "1"),
        ("--coeff-stream-retire-mode", "auto"),
        ("--coeff-stream-boundary-mask", "1"),
        ("--coeff-stream-dinv-consumer", "direct"),
        ("--coeff-stream-lbar-consumer", "direct"),
        ("--coeff-stream-ubar-consumer", "direct"),
        ("--coeff3-schedule", "frontier-aware"),
        ("--lu5-count", "2"), ("--coeff3-count", "2"),
        ("--coeff-line-mvm-model", "tree" if level >= 12 else "serial"),
        ("--coeff-warm-forward-tree", "1" if level >= 13 else "0"),
        ("--coeff-warm-rhs-tree", "1" if level >= 13 else "0"),
        ("--coeff-drain-policy", "age" if level >= 13 else "legacy"),
        ("--coeff-drain-arb-latency", "1"), ("--coeff-drain-arb-ii", "1"),
        ("--coeff-data-commit-depth", str(min(4, int(_option_value(
            argv, "--coeff-output-buffer-depth", "4")))) if level >= 12 else "0"),
        ("--coeff-warm-diagonal", "reciprocal" if level >= 12 and
            _option_value(argv, "--coeff3-div-mode", "reciprocal") == "reciprocal"
            else "divide"),
        ("--coeff3-div-mode", "reciprocal" if level >= 2 else "divide"),
        ("--bench-iters", "1"), ("--coeff-validation", "full"),
    ):
        _append_default(argv, name, value)

