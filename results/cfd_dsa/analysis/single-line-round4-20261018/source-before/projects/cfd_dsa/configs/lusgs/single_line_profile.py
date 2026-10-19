"""The single-line study: one forward and one reverse recurrence per sweep."""

from step2_profile import _append_default


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
        ("--coeff-line-mvm-model", "serial"),
        ("--coeff3-div-mode", "divide"),
        ("--bench-iters", "1"), ("--coeff-validation", "full"),
    ):
        _append_default(argv, name, value)

