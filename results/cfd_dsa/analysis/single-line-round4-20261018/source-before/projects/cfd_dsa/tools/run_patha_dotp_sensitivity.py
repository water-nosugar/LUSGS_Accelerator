#!/usr/bin/env python3
"""Run Path A dotp latency/count sensitivity sweep.

This is a host-side helper.  It invokes gem5 once per configuration, writes
each run to its own m5out directory, parses stats.txt plus stdout, and emits
CSV/Markdown summaries.
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path


SWEEP = [
    (7, 1),
    (6, 1),
    (5, 1),
    (7, 2),
    (6, 2),
    (5, 2),
]


FIELDS = [
    "dotp_lat",
    "dotp_count",
    "correctness",
    "numCycles",
    "simInsts",
    "IPC",
    "issueRate",
    "matvecs",
    "cycles_per_matvec",
    "effective_flops_per_cycle",
    "issued_CFDDSADotp",
    "issued_CFDDSAMatLd",
    "issued_CFDDSAPack",
    "issued_IntAlu",
    "issued_MemWrite",
    "busy_CFDDSADotp",
    "busy_CFDDSAMatLd",
    "busy_CFDDSAPack",
    "busy_IntAlu",
    "busy_MemWrite",
    "issue_0_ratio",
    "issue_1_ratio",
    "issue_2_ratio",
    "issue_3_ratio",
    "issue_4plus_ratio",
    "pathAResultBufferAllocs",
    "pathAResultBufferFrees",
    "pathAResultBufferWrites",
    "pathAResultBufferPacks",
    "pathAResultBufferFullStalls",
    "pathAResultBufferLiveMax",
    "pathAResultBufferOverlapCycles",
    "pathAResultBufferPackWhileDotpCycles",
]


STAT_NAMES = {
    "numCycles": ("system.cpu.numCycles",),
    "simInsts": ("simInsts", "system.cpu.commitStats0.numInsts"),
    "IPC": ("system.cpu.ipc",),
    "issueRate": ("system.cpu.iew.issueRate", "system.cpu.issueRate"),
    "issued_CFDDSADotp": ("system.cpu.issuedInstType_0::CFDDSADotp",),
    "issued_CFDDSAMatLd": ("system.cpu.issuedInstType_0::CFDDSAMatLd",),
    "issued_CFDDSAPack": ("system.cpu.issuedInstType_0::CFDDSAPack",),
    "issued_IntAlu": ("system.cpu.issuedInstType_0::IntAlu",),
    "issued_MemWrite": ("system.cpu.issuedInstType_0::MemWrite",),
    "busy_CFDDSADotp": ("system.cpu.statFuBusy::CFDDSADotp",),
    "busy_CFDDSAMatLd": ("system.cpu.statFuBusy::CFDDSAMatLd",),
    "busy_CFDDSAPack": ("system.cpu.statFuBusy::CFDDSAPack",),
    "busy_IntAlu": ("system.cpu.statFuBusy::IntAlu",),
    "busy_MemWrite": ("system.cpu.statFuBusy::MemWrite",),
    "issue_samples": ("system.cpu.numIssuedDist::samples",),
    "issue_0": ("system.cpu.numIssuedDist::0",),
    "issue_1": ("system.cpu.numIssuedDist::1",),
    "issue_2": ("system.cpu.numIssuedDist::2",),
    "issue_3": ("system.cpu.numIssuedDist::3",),
    "pathAResultBufferAllocs": (
        "system.cpu.local_spm.pathAResultBufferAllocs",
        "patha.bufferAllocs",
    ),
    "pathAResultBufferFrees": (
        "system.cpu.local_spm.pathAResultBufferFrees",
        "patha.bufferFrees",
    ),
    "pathAResultBufferWrites": (
        "system.cpu.local_spm.pathAResultBufferWrites",
        "patha.bufferWrites",
    ),
    "pathAResultBufferPacks": (
        "system.cpu.local_spm.pathAResultBufferPacks",
        "patha.bufferPacks",
    ),
    "pathAResultBufferFullStalls": (
        "system.cpu.local_spm.pathAResultBufferFullStalls",
        "patha.bufferFullStalls",
    ),
    "pathAResultBufferLiveMax": (
        "system.cpu.local_spm.pathAResultBufferLiveMax",
        "patha.bufferLiveMax",
    ),
    "pathAResultBufferOverlapCycles": (
        "system.cpu.local_spm.pathAResultBufferOverlapCycles",
        "patha.bufferOverlapCycles",
    ),
    "pathAResultBufferPackWhileDotpCycles": (
        "system.cpu.local_spm.pathAResultBufferPackWhileDotpCycles",
        "patha.bufferPackWhileDotpCycles",
    ),
}


def parse_value(raw: str):
    raw = raw.strip()
    try:
        if any(ch in raw for ch in ".eE"):
            return float(raw)
        return int(raw)
    except ValueError:
        return raw


def parse_stats(path: Path) -> dict:
    values = {}
    if not path.exists():
        return values
    pattern = re.compile(r"^([A-Za-z0-9_.$:]+)\s+([^\s#]+)")
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.match(line)
        if not match:
            continue
        name, value = match.groups()
        for key, names in STAT_NAMES.items():
            if key not in values and name in names:
                values[key] = parse_value(value)
    return values


def parse_correctness(stdout_path: Path) -> str:
    if not stdout_path.exists():
        return "UNKNOWN"
    text = stdout_path.read_text(errors="replace")
    if "Overall: PASS" in text:
        return "PASS"
    if "Overall: FAIL" in text or "FAILED" in text:
        return "FAIL"
    return "UNKNOWN"


def finalize_row(row: dict) -> dict:
    cycles = float(row.get("numCycles", 0) or 0)
    dotp = int(row.get("issued_CFDDSADotp", 0) or 0)
    matvecs = dotp / 5.0 if dotp else 0.0
    row["matvecs"] = matvecs
    row["cycles_per_matvec"] = cycles / matvecs if cycles and matvecs else 0.0
    effective_flops = matvecs * 50.0
    row["effective_flops_per_cycle"] = (
        effective_flops / cycles if cycles else 0.0
    )

    samples = int(row.get("issue_samples", 0) or 0)
    if samples:
        issue0 = int(row.get("issue_0", 0) or 0)
        issue1 = int(row.get("issue_1", 0) or 0)
        issue2 = int(row.get("issue_2", 0) or 0)
        issue3 = int(row.get("issue_3", 0) or 0)
        row["issue_0_ratio"] = issue0 / samples
        row["issue_1_ratio"] = issue1 / samples
        row["issue_2_ratio"] = issue2 / samples
        row["issue_3_ratio"] = issue3 / samples
        row["issue_4plus_ratio"] = max(
            0, samples - issue0 - issue1 - issue2 - issue3
        ) / samples
    else:
        for key in (
            "issue_0_ratio",
            "issue_1_ratio",
            "issue_2_ratio",
            "issue_3_ratio",
            "issue_4plus_ratio",
        ):
            row[key] = 0.0

    if not row.get("issueRate") and cycles:
        issued_total = sum(
            float(row.get(key, 0) or 0)
            for key in (
                "issued_CFDDSADotp",
                "issued_CFDDSAMatLd",
                "issued_CFDDSAPack",
                "issued_IntAlu",
                "issued_MemWrite",
            )
        )
        row["issueRate"] = issued_total / cycles

    for key in FIELDS:
        row.setdefault(key, 0)
    return row


def fmt(value) -> str:
    if isinstance(value, float):
        return f"{value:.4f}"
    return str(value)


def generate_analysis(rows: list[dict]) -> str:
    lines = ["## Automatic Bottleneck Analysis", ""]
    by_cfg = {(int(r["dotp_lat"]), int(r["dotp_count"])): r for r in rows}

    for row in rows:
        dotp_busy = float(row.get("busy_CFDDSADotp", 0) or 0)
        matld_busy = float(row.get("busy_CFDDSAMatLd", 0) or 0)
        pack_busy = float(row.get("busy_CFDDSAPack", 0) or 0)
        if dotp_busy > max(matld_busy, pack_busy) * 4:
            lines.append(
                f"- lat={row['dotp_lat']}, count={row['dotp_count']}: "
                "Primary bottleneck: CFDDSADotp structural bottleneck."
            )
        if int(row.get("pathAResultBufferFullStalls", 0) or 0) == 0:
            lines.append(
                f"- lat={row['dotp_lat']}, count={row['dotp_count']}: "
                "Internal result buffer is not the bottleneck. "
                "Do not increase buffer depth as a primary optimization."
            )
        if int(row.get("busy_CFDDSAPack", 0) or 0) == 0:
            lines.append(
                f"- lat={row['dotp_lat']}, count={row['dotp_count']}: "
                "pack_acc is not the bottleneck. "
                "Do not increase pack_count as a primary optimization."
            )

    for lat in (7, 6, 5):
        one = by_cfg.get((lat, 1))
        two = by_cfg.get((lat, 2))
        if one and two and one["cycles_per_matvec"]:
            gain = (
                one["cycles_per_matvec"] - two["cycles_per_matvec"]
            ) / one["cycles_per_matvec"]
            if gain > 0.05:
                lines.append(
                    f"- dotp_lat={lat}: dotp_count=2 improves cycles/matvec "
                    f"by {gain * 100:.2f}%. dotp_count=1 is "
                    "under-provisioned for this Path A kernel; dotp_count=2 "
                    "is a valid balanced performance config."
                )

    for count in (1, 2):
        lat7 = by_cfg.get((7, count))
        lat5 = by_cfg.get((5, count))
        if lat7 and lat5 and lat7["cycles_per_matvec"]:
            gain = (
                lat7["cycles_per_matvec"] - lat5["cycles_per_matvec"]
            ) / lat7["cycles_per_matvec"]
            if gain < 0.03:
                lines.append(
                    f"- dotp_count={count}: dotp_lat=5 improves less than "
                    "3% over dotp_lat=7. The workload is throughput/"
                    "structural limited rather than latency limited."
                )
            elif gain > 0.05:
                lines.append(
                    f"- dotp_count={count}: dotp_lat=5 improves cycles/matvec "
                    f"by {gain * 100:.2f}%. Path A is sensitive to "
                    "O3-visible dotp_row latency; a shorter row-dot pipeline "
                    "can improve scheduling and reduce stalls."
                )

    best_balanced = None
    count2_rows = [r for r in rows if int(r["dotp_count"]) == 2]
    if count2_rows:
        # Prefer the fastest reasonable count=2 config.  Keep latency visible in
        # the conclusion so users can decide whether lat=5 is too aggressive.
        best_balanced = min(count2_rows, key=lambda r: r["cycles_per_matvec"])

    lines.extend([
        "",
        "## Recommendation",
        "",
        "Path A default tapeout config:",
        "    dotp_lat = 7",
        "    dotp_count = 1",
        "    reason: conservative single row-dot engine, smallest area, "
        "current baseline.",
        "",
        "Path A balanced performance config:",
    ])
    if best_balanced:
        lines.extend([
            f"    dotp_lat = {int(best_balanced['dotp_lat'])}",
            "    dotp_count = 2",
            "    reason: reduces CFDDSADotp structural bottleneck with "
            "acceptable hardware cost.",
        ])
    else:
        lines.extend([
            "    dotp_lat = 6 or 7",
            "    dotp_count = 2",
            "    reason: rerun sweep to choose the best reasonable point.",
        ])
    lines.extend([
        "",
        "Path A aggressive config:",
        "    dotp_lat = 5",
        "    dotp_count = 2",
        "    reason: evaluates optimized FP64 row-dot pipeline and dual dotp "
        "issue.",
        "",
        "Not recommended:",
        "    dotp_count > 2 as default",
        "    pack_count > 1",
        "    result buffer depth > 2",
        "    full64 store",
        "    pred40_stride64",
        "    padded64 read width",
        "    dotp_lat <= 3 as main result",
    ])
    return "\n".join(lines)


def write_outputs(rows: list[dict], out_root: Path) -> tuple[Path, Path]:
    csv_path = out_root / "patha_dotp_sensitivity_summary.csv"
    md_path = out_root / "patha_dotp_sensitivity_summary.md"

    with csv_path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in FIELDS})

    table_cols = [
        "dotp_lat",
        "dotp_count",
        "correctness",
        "cycles_per_matvec",
        "IPC",
        "effective_flops_per_cycle",
        "busy_CFDDSADotp",
        "busy_CFDDSAMatLd",
        "busy_CFDDSAPack",
        "issue_0_ratio",
        "pathAResultBufferFullStalls",
        "pathAResultBufferLiveMax",
    ]
    lines = [
        "# Path A dotp latency/count sensitivity summary",
        "",
        "| " + " | ".join(table_cols) + " |",
        "| " + " | ".join(["---"] * len(table_cols)) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(fmt(row.get(col, "")) for col in table_cols) + " |")
    lines.extend(["", generate_analysis(rows), ""])
    md_path.write_text("\n".join(lines))
    return csv_path, md_path


def run_one(args, dotp_lat: int, dotp_count: int, out_dir: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = out_dir / "stdout.txt"
    cmd = [
        str(args.gem5),
        "-d",
        str(out_dir),
        str(args.config),
        "--binary",
        str(args.binary),
        "--cpu",
        args.cpu,
        "--bench-iters",
        str(args.bench_iters),
        "--patha-spm-mode",
        "direct",
        "--patha-spm-read-width",
        "40",
        "--patha-store-mode",
        "pred40",
        "--patha-store-stride",
        "40",
        "--patha-acc-mode",
        "internal-buffer",
        "--patha-result-buffer-depth",
        "2",
        "--patha-dotp-lat",
        str(dotp_lat),
        "--patha-dotp-count",
        str(dotp_count),
        "--patha-pack-count",
        "1",
        "--patha-unroll",
        "8",
    ]
    print(f"[PathA sweep] dotp_lat={dotp_lat}, dotp_count={dotp_count}")
    with stdout_path.open("w") as stdout:
        proc = subprocess.run(
            cmd,
            stdout=stdout,
            stderr=subprocess.STDOUT,
            cwd=args.repo,
            text=True,
        )
    if proc.returncode != 0:
        print(f"  run failed with exit code {proc.returncode}; see {stdout_path}")

    row = parse_stats(out_dir / "stats.txt")
    row["dotp_lat"] = dotp_lat
    row["dotp_count"] = dotp_count
    row["correctness"] = parse_correctness(stdout_path)
    row = finalize_row(row)
    return row


def main() -> int:
    repo = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(
        description="Run Path A dotp latency/count sensitivity sweep"
    )
    parser.add_argument("--repo", type=Path, default=repo)
    parser.add_argument("--gem5", type=Path, default=repo / "build/ARM/gem5.opt")
    parser.add_argument(
        "--config", type=Path,
        default=repo / "projects/cfd_dsa/configs/run_cfd_patha.py")
    parser.add_argument(
        "--binary", type=Path,
        default=repo / "build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm")
    parser.add_argument("--cpu", default="o3")
    parser.add_argument("--bench-iters", type=int, default=50000)
    parser.add_argument(
        "--out-root", type=Path,
        default=repo / "results/cfd_dsa/runs/patha-sweep")
    parser.add_argument("--parse-only", action="store_true",
                        help="do not run gem5; parse existing sweep directories")
    args = parser.parse_args()

    args.repo = args.repo.resolve()
    args.out_root.mkdir(parents=True, exist_ok=True)
    rows = []
    for dotp_lat, dotp_count in SWEEP:
        out_dir = args.out_root / f"dotp_lat{dotp_lat}_count{dotp_count}"
        if args.parse_only:
            row = parse_stats(out_dir / "stats.txt")
            row["dotp_lat"] = dotp_lat
            row["dotp_count"] = dotp_count
            row["correctness"] = parse_correctness(out_dir / "stdout.txt")
            row = finalize_row(row)
        else:
            row = run_one(args, dotp_lat, dotp_count, out_dir)
        rows.append(row)

    csv_path, md_path = write_outputs(rows, args.out_root)
    print(f"Wrote {csv_path}")
    print(f"Wrote {md_path}")

    failed = [r for r in rows if r.get("correctness") != "PASS"]
    if failed:
        print("WARNING: some configurations did not report correctness PASS")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
