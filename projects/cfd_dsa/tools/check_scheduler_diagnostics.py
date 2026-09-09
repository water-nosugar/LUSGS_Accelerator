#!/usr/bin/env python3
"""Check cumulative scheduler event accounting (not wall-cycle counters)."""
import argparse
from pathlib import Path
import re

from check_context_ablation import read_case


def check(root):
    for phase in ("cold", "mixed"):
        for trsm in (2, 4):
            path = root / f"{phase}-l2-t{trsm}"
            cycles, hashes, pools, _, _ = read_case(path)
            text = (path / "simout.txt").read_text()
            reasons = {}
            issues = {}
            for line in text.splitlines():
                fields = dict(re.findall(r"(\w+)=(\w+)", line))
                if line.startswith("CFD_SCHED_DIAG "):
                    reasons[fields["reason"]] = int(fields["count"])
                if line.startswith("CFD_ISSUE_DIAG "):
                    name = fields.pop("pool")
                    issues[name] = {
                        k: int(v) for k, v in fields.items()}
            assert reasons and issues.keys() == pools.keys(), path
            for name, p in issues.items():
                assert p["attempts"] == p["rejected"] + pools[name]["issued"], path
                assert p["rejected_after_issue"] <= p["rejected"], path
            assert reasons["rhs_candidate"] == (
                reasons["rhs_selected"] +
                reasons.get("rhs_candidate_not_selected", 0) +
                reasons.get("rhs_aborted_unvisited", 0)), path
            # Divide mode has no reciprocal-pending pseudo-ready candidates.
            rejected = sum(issues[n]["rejected"] for n in
                           ("coeff_div", "coeff_mul", "coeff_sub"))
            assert rejected == reasons.get("rhs_candidate_not_selected", 0), path
            assert not reasons.get("rhs_aborted_unvisited", 0), path
            for reason, suffix in (
                ("input_slot_full", "InputSlot"),
                ("lu_context_full", "LuPending"),
                ("trsm_context_full", "SolvePending"),
                ("output_ring_full", "OutputRing"),
                ("drain_queue_full", "DrainQueue"),
            ):
                legacy = re.search(r"\.eventStall" + suffix + r" = (\d+)", text)
                assert legacy and int(legacy[1]) == reasons.get(reason, 0), path
            print(f"{path.name}: cycles={cycles}")
            print("  reasons:", dict(sorted(reasons.items())))
            print("  resources:", issues)
    print("SCHEDULER_DIAGNOSTICS_CHECK_PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    check(parser.parse_args().campaign)
