#!/usr/bin/env python3
"""Compare request-order experiment with same-build fixed-resource controls."""
import argparse
from pathlib import Path
import re
from check_context_ablation import read_case


def check(root):
    for phase in ("cold", "mixed"):
        for trsm in (2, 4):
            name = f"{phase}-l2-t{trsm}"
            a = read_case(root / name)
            b = read_case(root / (name + "-aging"))
            assert a[1] == b[1], "numeric digest changed"
            assert len(a[1]) == (1 if phase == "cold" else 3)
            assert a[4] == b[4], "different simulator binaries"
            assert a[3]["coeff3-schedule"] == "frontier-aware"
            assert b[3]["coeff3-schedule"] == "request-frontier-aging"
            for options in (a[3], b[3]):
                options.pop("coeff3-schedule")
            assert a[3] == b[3], "non-scheduler configuration changed"
            for pool in a[2]:
                for key in ("count", "lat", "ii", "issued"):
                    assert a[2][pool][key] == b[2][pool][key], (pool, key)
            print(f"{name}: baseline={a[0]} aging={b[0]} "
                  f"change={100*(b[0]/a[0]-1):+.2f}%")
            for label, directory in (("baseline", name), ("aging", name+"-aging")):
                text = (root / directory / "simout.txt").read_text()
                for metric in ("eventRhsMaxWaitCycles", "frontierWaitLbarColumnsCycles"):
                    match = re.search(r"\." + metric + r" = (\d+)", text)
                    assert match, metric
                    print(f"  {label} {metric}={match[1]}")
    print("REQUEST_FRONTIER_AGING_CHECK_PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    check(parser.parse_args().campaign)
