#!/usr/bin/env python3
"""Compare an explicit pre-extraction runner with the standalone reporter.

The baseline is trusted local Python source supplied by the developer.
Only its defaults, report helpers and report block execute; no gem5 setup runs.
"""
import argparse
import ast
import contextlib
import io
import os
from pathlib import Path
import re
import subprocess
import sys

CONFIG = Path(__file__).resolve().parents[1] / "configs"
sys.path.insert(0, str(CONFIG))
from cfd_reporting import bind


def read_baseline(spec):
    path = Path(spec)
    if path.is_file():
        return path.read_text(), str(path)
    if ":" not in spec:
        raise FileNotFoundError(spec)
    source = subprocess.check_output(
        ["git", "show", spec], cwd=CONFIG.parents[2], text=True)
    return source, spec


def check(baseline, files):
    source, baseline_name = read_baseline(baseline)
    start = source.index("THEORY = {")
    end = source.index("\nif stats_file:", start)
    defaults = source[source.index("report_focus ="):source.index("\ntry:\n")]
    tree = ast.parse(source)
    helper = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                  and n.name == "patha_matld_extra_per_run")
    comparisons = 0
    for focus in ("auto", "patha", "pathb", "pathc"):
        for unroll in (1, 8):
            ns = {"os": os, "re": re, "binary": str, "result": str}
            exec(defaults, ns)
            ns.update(report_focus=focus, patha_unroll=unroll)
            ns["patha_store_stride"] = (
                40 if ns["patha_store_mode"] in ("pred40", "store5") else 64)
            exec(compile(ast.Module(body=[helper], type_ignores=[]),
                         baseline_name, "exec"), ns)
            exec(source[start:end], ns)
            report = bind(ns)
            for file in files:
                old = ns["parse_stats_file"](str(file))
                new = report.parse_stats_file(str(file))
                assert old == new, (focus, unroll, file, "parsed statistics")
                outputs = []
                for fn, data in ((ns["print_performance_report"], old),
                                 (report.print_performance_report, new)):
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out):
                        fn(data, 64)
                    outputs.append(out.getvalue())
                assert outputs[0] == outputs[1], (focus, unroll, file, "report")
                comparisons += 1
    print(f"REPORTING_COMPAT_PASS comparisons={comparisons}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "baseline", help="local source or REV:projects/.../run_cfd_dsa.py")
    parser.add_argument("stats", type=Path, nargs="+")
    args = parser.parse_args()
    check(args.baseline, args.stats)
