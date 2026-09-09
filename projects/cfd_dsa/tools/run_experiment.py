#!/usr/bin/env python3
"""Run one CFD-DSA experiment in a managed output directory."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys


REPO = Path(__file__).resolve().parents[3]
CONFIG_ROOT = REPO / "projects/cfd_dsa/configs"
RESULT_ROOT = REPO / "results/cfd_dsa"
TARGETS = {
    "common": CONFIG_ROOT / "run_cfd_dsa.py",
    "patha": CONFIG_ROOT / "run_cfd_patha.py",
    "pathb": CONFIG_ROOT / "run_cfd_pathb.py",
    "pathc": CONFIG_ROOT / "run_cfd_pathc.py",
    "trsv5": CONFIG_ROOT / "run_cfd_trsv5.py",
    "lusgs-step1": CONFIG_ROOT / "lusgs/run_cfd_lusgs_step1.py",
    "lusgs-step2": CONFIG_ROOT / "lusgs/run_cfd_lusgs_step2.py",
    "lusgs-step3": CONFIG_ROOT / "lusgs/run_cfd_lusgs_step3.py",
    "lusgs-step4": CONFIG_ROOT / "lusgs/run_cfd_lusgs_step4.py",
}
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
STAT_SUFFIXES = {
    "sim_ticks": "simTicks",
    "cpu_cycles": "system.cpu.numCycles",
    "ipc": "system.cpu.ipc",
    "cpi": "system.cpu.cpi",
    "lusgs_event_cycles": "lusgsEventActualCycles",
    "trsv_average_latency": "lusgsEventTrsvAverageLatency",
    "vec5_average_latency": "lusgsEventVec5AverageLatency",
}


def git_output(*args: str) -> str:
    completed = subprocess.run(
        ["git", *args], cwd=REPO, text=True, capture_output=True,
        check=False)
    return completed.stdout.strip()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_manifest(path: Path, data: dict) -> None:
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def numeric_value(text: str) -> int | float | str:
    try:
        return int(text, 0)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def extract_metrics(stats: Path, simout_text: str) -> dict:
    metrics = {}
    if stats.is_file():
        for line in stats.read_text(errors="replace").splitlines():
            fields = line.split()
            if len(fields) < 2:
                continue
            for label, suffix in STAT_SUFFIXES.items():
                if label not in metrics and fields[0].endswith(suffix):
                    metrics[label] = numeric_value(fields[1])
    metrics["pass_labels"] = sorted(set(re.findall(
        r"\b(?:[A-Z][A-Z0-9_]*PASS|Overall:\s+PASS)\b", simout_text)))
    mismatch_values = [int(value) for value in re.findall(
        r"mismatch_count\s*=\s*(\d+)", simout_text)]
    if mismatch_values:
        metrics["mismatch_count_max"] = max(mismatch_values)
    return metrics


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True, choices=sorted(TARGETS))
    parser.add_argument("--campaign", default="development")
    parser.add_argument("--case", required=True)
    parser.add_argument("--gem5", type=Path,
                        default=REPO / "build/ARM/gem5.opt")
    parser.add_argument("config_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    for label, value in (("campaign", args.campaign), ("case", args.case)):
        if not SAFE_NAME.fullmatch(value):
            parser.error(f"{label} must match {SAFE_NAME.pattern}")

    config_args = args.config_args
    if config_args and config_args[0] == "--":
        config_args = config_args[1:]

    now = datetime.now(timezone.utc)
    if args.campaign == "development":
        run_id = now.strftime("%Y%m%dT%H%M%SZ") + "_" + args.case
        output = RESULT_ROOT / "runs" / run_id
    else:
        output = RESULT_ROOT / "campaigns" / args.campaign / args.case
    if output.exists():
        parser.error(f"immutable result path already exists: {output}")
    output.mkdir(parents=True)

    gem5 = args.gem5.resolve()
    config = TARGETS[args.target].resolve()
    if not gem5.is_file() or not config.is_file():
        parser.error(f"missing gem5 or config: {gem5}, {config}")

    command = [str(gem5), "--redirect-stdout", "-d", str(output),
               str(config), *config_args]
    status_text = git_output("status", "--porcelain=v1")
    manifest = {
        "schema_version": 1,
        "status": "running",
        "target": args.target,
        "campaign": args.campaign,
        "case": args.case,
        "started_at": now.isoformat(),
        "repo": str(REPO),
        "git_revision": git_output("rev-parse", "HEAD"),
        "git_dirty": bool(status_text),
        "git_status_sha256": hashlib.sha256(
            status_text.encode()).hexdigest(),
        "host": platform.node(),
        "command": command,
        "command_shell": shlex.join(command),
        "gem5_sha256": file_sha256(gem5),
        "output_dir": str(output.relative_to(REPO)),
    }
    manifest_path = output / "manifest.json"
    write_manifest(manifest_path, manifest)

    environment = os.environ.copy()
    environment["GEM5_CFD_RESULTS_DIR"] = str(output)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    driver_log = output / "driver.log"
    with driver_log.open("w") as log:
        completed = subprocess.run(
            command, cwd=REPO, env=environment, stdout=log,
            stderr=subprocess.STDOUT, check=False)

    stats = output / "stats.txt"
    simout = output / "simout.txt"
    simout_text = simout.read_text(errors="replace") if simout.exists() else ""
    passed = (completed.returncode == 0 and stats.is_file() and
              stats.stat().st_size > 0 and "_FAIL" not in simout_text)
    metrics = extract_metrics(stats, simout_text)
    write_manifest(output / "metrics.json", metrics)
    manifest.update({
        "status": "passed" if passed else "failed",
        "return_code": completed.returncode,
        "finished_at": datetime.now(timezone.utc).isoformat(),
        "stats_bytes": stats.stat().st_size if stats.exists() else 0,
        "simout_bytes": simout.stat().st_size if simout.exists() else 0,
        "metrics_file": "metrics.json",
    })
    write_manifest(manifest_path, manifest)
    print(f"{manifest['status']}: {output.relative_to(REPO)}")
    return 0 if passed else (completed.returncode or 1)


if __name__ == "__main__":
    raise SystemExit(main())
