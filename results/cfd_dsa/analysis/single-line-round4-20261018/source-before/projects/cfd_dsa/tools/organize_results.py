#!/usr/bin/env python3
"""Move legacy top-level m5out directories into the CFD-DSA result tree."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil


REPO = Path(__file__).resolve().parents[3]
ROOT = REPO / "results/cfd_dsa"
CAMPAIGN = ROOT / "campaigns/2026-07-01-step4c"
ARCHIVE = ROOT / "archive/legacy-2026-07-05"

CANONICAL = {
    "m5out_step4a_1x64_post": ROOT / "baselines/lusgs/step4a/1x64",
    "m5out_step4b_1x64": ROOT / "baselines/lusgs/step4b/1x64",
    "m5out_step4c_reg_step4c_1x1": CAMPAIGN / "correctness/1x1",
    "m5out_step4c_reg_step4c_1x2": CAMPAIGN / "correctness/1x2",
    "m5out_step4c_reg_step4c_1x3": CAMPAIGN / "correctness/1x3",
    "m5out_step4c_reg_step4c_1x17": CAMPAIGN / "correctness/1x17",
    "m5out_step4c_reg_step4c_1x64": CAMPAIGN / "correctness/1x64",
    "m5out_step4c_reg_step4c_1x512": CAMPAIGN / "correctness/1x512",
    "m5out_step4c_reg_step4c_1x50000_final":
        CAMPAIGN / "correctness/1x50000",
    "m5out_step4c_backpressure_final2": CAMPAIGN / "faults/backpressure",
    "m5out_step4c_reg_stale_final": CAMPAIGN / "faults/stale-generation",
    "m5out_step4c_reg_badreq_final": CAMPAIGN / "faults/bad-request-id",
    "m5out_step4c_reg_duplicate_final":
        CAMPAIGN / "faults/duplicate-completion",
    "m5out_step4c_reg_errorlegal_final": CAMPAIGN / "faults/error-then-legal",
    "m5out_step4c_reg_watchdog_final": CAMPAIGN / "faults/watchdog",
    "m5out_step4c_reg_wronglaunch_final": CAMPAIGN / "faults/wrong-launch",
    "m5out_step4c_reg_wrongwait_final": CAMPAIGN / "faults/wrong-wait",
    "m5out_step4c_trace_final2": CAMPAIGN / "trace/basic",
    "m5out_step4c_reg_vec_l1_q1_o1": CAMPAIGN / "vector/lane1-q1-omega1",
    "m5out_step4c_reg_vec_l2_q2_o05": CAMPAIGN / "vector/lane2-q2-omega0.5",
    "m5out_step4c_reg_vec_l5_q4_on025":
        CAMPAIGN / "vector/lane5-q4-omega-0.25",
}


def directory_size(path: Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*")
               if item.is_file())


def stats_status(path: Path) -> str:
    stats = path / "stats.txt"
    if not stats.exists():
        return "missing"
    return "empty" if stats.stat().st_size == 0 else "present"


def destination_for(path: Path) -> tuple[Path, str]:
    name = path.name
    if name == "m5out":
        return ARCHIVE / "m5out", "archive"
    if stats_status(path) == "empty":
        return ROOT / "failed/legacy" / name, "failed"
    if name in CANONICAL:
        destination = CANONICAL[name]
        category = "baseline" if "baselines" in destination.parts else "campaign"
        return destination, category
    if name.startswith("m5out_step4c_sens_"):
        case = name.removeprefix("m5out_step4c_sens_")
        return CAMPAIGN / "sensitivity" / case, "campaign"
    if name.startswith("m5out_step4c_finalreg_"):
        case = name.removeprefix("m5out_step4c_finalreg_")
        return CAMPAIGN / "regression" / case, "campaign"
    return ARCHIVE / "top-level" / name, "archive"


def write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true",
                        help="perform moves; default is a dry run")
    args = parser.parse_args()

    sources = sorted(path for path in REPO.glob("m5out*") if path.is_dir())
    if args.apply and not sources:
        print("No top-level m5out directories require migration.")
        return 0
    records = []
    for source in sources:
        destination, category = destination_for(source)
        record = {
            "original_path": str(source.relative_to(REPO)),
            "destination": str(destination.relative_to(REPO)),
            "category": category,
            "stats": stats_status(source),
            "bytes": directory_size(source),
        }
        records.append(record)
        print(f"{record['category']:8} {record['original_path']} -> "
              f"{record['destination']}")
        if not args.apply:
            continue
        if destination.exists():
            raise SystemExit(f"destination already exists: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(source), str(destination))
        write_json(destination / "legacy-manifest.json", {
            "schema_version": 1,
            "migrated_at": datetime.now(timezone.utc).isoformat(),
            **record,
        })

    inventory = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "applied": args.apply,
        "count": len(records),
        "total_bytes": sum(record["bytes"] for record in records),
        "records": records,
    }
    name = "migration-inventory.json" if args.apply else "migration-dry-run.json"
    write_json(ROOT / name, inventory)

    if args.apply:
        links = {
            ROOT / "baselines/lusgs/step4c/1x64":
                CAMPAIGN / "correctness/1x64",
            ROOT / "baselines/lusgs/step4c/1x50000":
                CAMPAIGN / "correctness/1x50000",
        }
        for link, target in links.items():
            link.parent.mkdir(parents=True, exist_ok=True)
            link.symlink_to(os.path.relpath(target, link.parent),
                            target_is_directory=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
