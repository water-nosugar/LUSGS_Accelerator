#!/usr/bin/env python3
"""Check numerical and lifecycle invariants of coeff-line-dependency runs."""

import argparse
import json
from pathlib import Path
import re


def check(root):
    records = {}
    for name in ("baseline", "base-only", "early-only", "both", "one-cell",
                 "two-cell-window1", "lines4", "slow-drain-baseline",
                 "slow-drain", "dma-single-sweep", "reuse8-baseline", "reuse8"):
        path = root / name
        manifest = json.loads((path / "manifest.json").read_text())
        if manifest["status"] != "passed":
            raise ValueError(f"{name}: simulation did not pass")
        simout = (path / "simout.txt").read_text()
        if "LUSGS_STEP2_STREAMING_PASS" not in simout:
            raise ValueError(f"{name}: missing streaming correctness marker")
        text = (path / "driver.log").read_text() + simout
        rows = [dict(re.findall(r"(\w+)=(\w+)", line))
                for line in text.splitlines()
                if line.startswith("CFD_LINE_SCHEDULE ")]
        rows.sort(key=lambda row: int(row["token"]))
        expected = 12 if name == "lines4" else 3
        hits = expected // 3
        if name == "dma-single-sweep":
            expected, hits = 1, 0
        elif name.startswith("reuse8"):
            expected, hits = 8, 7
        if len(rows) != expected:
            raise ValueError(f"{name}: expected {expected} records, got {len(rows)}")
        if sum(int(r["cache_hit"]) for r in rows) != hits:
            raise ValueError(f"{name}: rebuild/hit/dirty-rebuild not covered")
        for r in rows:
            if not (int(r["backward_start"]) <= int(r["backward_done"]) <=
                    int(r["visible_done"])):
                raise ValueError(f"{name}: invalid compute/visibility order")
            if int(r["children_retired"]) > int(r["visible_done"]):
                raise ValueError(f"{name}: completion before child retirement")
        records[name] = rows
    for name in ("base-only", "early-only", "both"):
        if [r["result_hash"] for r in records[name]] != [
                r["result_hash"] for r in records["baseline"]]:
            raise ValueError(f"{name}: FP64 bit-pattern digest differs")
    if [r["result_hash"] for r in records["slow-drain"]] != [
            r["result_hash"] for r in records["slow-drain-baseline"]]:
        raise ValueError("slow drain: FP64 bit-pattern digest differs")
    if [r["result_hash"] for r in records["reuse8"]] != [
            r["result_hash"] for r in records["reuse8-baseline"]]:
        raise ValueError("reuse8: FP64 bit-pattern digest differs")
    if not any(int(r["backward_done"]) < int(r["children_retired"])
               for r in records["one-cell"]):
        raise ValueError("numeric completion before retirement not exercised")
    if not any(int(r["base_ahead_columns"]) > 0 for r in records["both"]):
        raise ValueError("base-ahead overlap not exercised")
    if not any(int(r["backward_start"]) < int(r["children_retired"])
               for r in records["slow-drain"] if r["cache_hit"] == "0"):
        raise ValueError("early backward overlap not exercised")
    print("LINE_DEPENDENCY_CHECK_PASS: correctness, bit-pattern digests, "
          "cache reuse, bounded-window cases, overlap and visibility ordering")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    check(parser.parse_args().campaign)
