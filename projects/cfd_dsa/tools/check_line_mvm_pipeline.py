#!/usr/bin/env python3
"""Check line MVM pipeline counts, bounds, numeric digests and retirement."""

import argparse
import json
from pathlib import Path
import re


def check(root):
    suite = Path(__file__).resolve().parents[1] / "suites/coeff-line-mvm-pipeline.json"
    records = {}
    for case in json.loads(suite.read_text())["cases"]:
        name = case["name"]
        path = root / name
        if json.loads((path / "manifest.json").read_text())["status"] != "passed":
            raise ValueError(f"{name}: simulation failed")
        text = (path / "simout.txt").read_text() + (path / "driver.log").read_text()
        if "LUSGS_STEP2_STREAMING_PASS" not in text:
            raise ValueError(f"{name}: missing full correctness marker")
        rows = [dict(re.findall(r"(\w+)=(\w+)", line))
                for line in text.splitlines()
                if line.startswith("CFD_LINE_SCHEDULE ")]
        rows.sort(key=lambda r: int(r["token"]))
        expected = 12 if name == "split-lines4" else (
            1 if name == "split-dma-one-sweep" else 3)
        if len(rows) != expected:
            raise ValueError(f"{name}: wrong completion count")
        hits = 0 if expected == 1 else expected // 3
        if sum(int(r["cache_hit"]) for r in rows) != hits:
            raise ValueError(f"{name}: wrong cache reuse count")
        for r in rows:
            cells = 1 if name == "split-one-cell" else 17
            # Five column operations per MVM: cold only backward; cache hit
            # also includes N bases and N-1 corrections.
            ops = (5 * (cells - 1) +
                   (5 * (2 * cells - 1) if int(r["cache_hit"]) else 0))
            if name.startswith("serial"):
                ops = 0
            if int(r["line_products"]) != ops or int(r["line_adds"]) != ops:
                raise ValueError(f"{name}: missing/duplicated product or add")
            depth = 1 if name == "split-depth1" else 5
            if int(r["buffered_max"]) > depth:
                raise ValueError(f"{name}: product buffer capacity exceeded")
            if not (int(r["backward_start"]) <= int(r["backward_done"]) <=
                    int(r["visible_done"])):
                raise ValueError(f"{name}: bad numeric/visibility ordering")
            if int(r["children_retired"]) > int(r["visible_done"]):
                raise ValueError(f"{name}: premature software completion")
        records[name] = rows
    reference = [r["result_hash"] for r in records["serial4"]]
    for name, rows in records.items():
        if name not in ("split-one-cell", "split-lines4", "split-dma-one-sweep"):
            if [r["result_hash"] for r in rows] != reference:
                raise ValueError(f"{name}: FP64 digest mismatch")
    if records["split-dma-one-sweep"][0]["result_hash"] != reference[0]:
        raise ValueError("DMA result mismatch")
    if max(int(r["buffered_max"]) for r in records["split3-4"]) <= 1:
        raise ValueError("product overlap was not exercised")
    if not any(int(r["buffer_stalls"]) for r in records["split-depth1"]):
        raise ValueError("depth-one backpressure was not exercised")
    print("LINE_MVM_PIPELINE_CHECK_PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    check(parser.parse_args().campaign)
