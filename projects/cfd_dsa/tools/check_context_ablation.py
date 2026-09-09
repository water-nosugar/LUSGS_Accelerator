#!/usr/bin/env python3
"""Validate fixed-resource context ablation and print cycle/issue metrics."""

import argparse
import json
from pathlib import Path
import re


def read_case(path):
    manifest = json.loads((path / "manifest.json").read_text())
    if manifest["status"] != "passed":
        raise ValueError(f"{path.name}: simulation failed")
    text = (path / "simout.txt").read_text() + (path / "driver.log").read_text()
    if "LUSGS_STEP2_STREAMING_PASS" not in text:
        raise ValueError(f"{path.name}: missing full correctness marker")
    hashes = re.findall(r"CFD_LINE_SCHEDULE .*?result_hash=(\w+)", text)
    cycles = re.search(r"streamingStep2.totalCycles = (\d+)", text)
    if not cycles or not hashes:
        raise ValueError(f"{path.name}: missing timing or numeric digest")
    pools = {}
    for line in text.splitlines():
        if line.startswith("CFD_RESOURCE_STATS "):
            fields = dict(re.findall(r"(\w+)=(\w+)", line))
            name = fields.pop("pool")
            pools[name] = {key: int(value) for key, value in fields.items()}
    options = dict(arg[2:].split("=", 1) for arg in manifest["command"]
                   if arg.startswith("--") and "=" in arg)
    for key in ("lu5-count", "coeff3-count", "coeff-resource-stats"):
        options.pop(key, None)
    for name, p in pools.items():
        if p["samples"] != (p["issue_cycles"] + p["blocked_cycles"] +
                            p["no_attempt_cycles"]):
            raise ValueError(f"{path.name}/{name}: nonexclusive cycle accounting")
        if not (p["issued"] <= p["eligible_slots"] <= p["samples"] * p["count"]):
            raise ValueError(f"{path.name}/{name}: invalid issue-slot accounting")
        if p["active_cycles"] > p["samples"]:
            raise ValueError(f"{path.name}/{name}: active fraction exceeds one")
        # These tests are non-cancelled and snapshots are at quiescence.
        if p["occupancy_sum"] != p["issued"] * p["lat"]:
            raise ValueError(f"{path.name}/{name}: lost/duplicated pending samples")
    return int(cycles[1]), hashes, pools, options, manifest["gem5_sha256"]


def check(root):
    results = {}
    for phase in ("cold", "mixed"):
        baseline = read_case(root / f"{phase}-l1-t1")
        expected_hashes = 1 if phase == "cold" else 3
        print(f"\n{phase}: LU TRSM cycles change% lu_mul_active% coeff_div_active% "
              "lu_mul_issue/100ticks coeff_div_issue/100ticks")
        for lu in (1, 2, 4):
            for trsm in (1, 2, 4):
                name = f"{phase}-l{lu}-t{trsm}"
                row = read_case(root / name)
                cycles, hashes, pools, options, binary = row
                if len(hashes) != expected_hashes or hashes != baseline[1]:
                    raise ValueError(f"{name}: numeric digest mismatch")
                if options != baseline[3] or binary != baseline[4]:
                    raise ValueError(f"{name}: changed non-context parameters/binary")
                if len(pools) != 8 or pools.keys() != baseline[2].keys():
                    raise ValueError(f"{name}: missing/unexpected resource snapshots")
                for key, p in pools.items():
                    b = baseline[2][key]
                    if any(p[k] != b[k] for k in ("count", "lat", "ii", "issued")):
                        raise ValueError(f"{name}/{key}: resource/work count changed")
                def pct(pool, field):
                    return 100 * pools[pool][field] / pools[pool]["samples"]
                print(f"{lu} {trsm} {cycles} {100*(cycles/baseline[0]-1):+.2f} "
                      f"{pct('lu_mul', 'active_cycles'):.2f} "
                      f"{pct('coeff_div', 'active_cycles'):.2f} "
                      f"{pct('lu_mul', 'issued'):.3f} "
                      f"{pct('coeff_div', 'issued'):.3f}")
                results[name] = row
        best = min((n for n in results if n.startswith(phase)),
                   key=lambda n: results[n][0])
        print(f"best={best}")
    off = read_case(root / "cold-observer-off")
    base = results["cold-l1-t1"]
    if off[1] != base[1] or off[2] or off[3:] != base[3:]:
        raise ValueError("observer-off control differs in data/configuration")
    print(f"observer_off_cycles={off[0]} observer_on_cycles={base[0]} "
          f"delta={base[0]-off[0]}")
    # Printed, not hidden: guest polling/clock alignment can vary by a few
    # cycles with process placement, so do not conflate it with host overhead.
    if abs(base[0] - off[0]) > 8:
        raise ValueError("observer timing delta requires investigation")
    print("CONTEXT_ABLATION_CHECK_PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    check(parser.parse_args().campaign)
