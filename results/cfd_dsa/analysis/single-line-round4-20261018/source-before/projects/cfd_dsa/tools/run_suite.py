#!/usr/bin/env python3
"""Run a JSON CFD-DSA suite through the managed experiment runner."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[3]
RUNNER = REPO / "projects/cfd_dsa/tools/run_experiment.py"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("suite", type=Path)
    parser.add_argument("--campaign")
    args = parser.parse_args()

    suite = json.loads(args.suite.read_text())
    campaign = args.campaign or suite["campaign"]
    common_args = suite.get("common_args", [])
    failures = []
    for case in suite["cases"]:
        command = [sys.executable, str(RUNNER), "--target", case["target"],
                   "--campaign", campaign, "--case", case["name"], "--",
                   *common_args, *case.get("args", [])]
        print("+", " ".join(command), flush=True)
        completed = subprocess.run(command, cwd=REPO, check=False)
        if completed.returncode:
            failures.append(case["name"])
    if failures:
        print("failed cases:", ", ".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
