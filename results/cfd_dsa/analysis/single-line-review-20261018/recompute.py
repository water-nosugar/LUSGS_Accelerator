#!/usr/bin/env python3
"""Independent review metrics. Does not rewrite the round3 historical report."""
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[3]
CAMPAIGNS = ROOT / "results/cfd_dsa/campaigns"


def fields(text, prefix):
    return [dict(re.findall(r"(\w+)=([^ ]+)", line))
            for line in text.splitlines() if line.startswith(prefix + " ")]


def union(intervals):
    end = -1
    total = 0
    for a, b in sorted(intervals):
        total += max(0, b - max(a, end))
        end = max(end, b)
    return total


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def measure(path):
    text = (path / "simout.txt").read_text()
    manifest = json.loads((path / "manifest.json").read_text())
    schedules = fields(text, "CFD_LINE_SCHEDULE")
    interface = fields(text, "CFD_INTERFACE_DMA")
    storage = {r["token"]: r for r in fields(text, "CFD_LINE_STORAGE")}
    ops = list(csv.DictReader((path / "coeff_preprocess_trace.csv.cluster.csv").open()))
    bank_snapshots = fields(text, "CFD_ONCHIP_BANK")
    latest = {int(r["bank"]): r for r in bank_snapshots}
    onchip = fields(text, "CFD_ONCHIP_SUMMARY")[-1]
    accesses = fields(text, "CFD_ONCHIP_ACCESS")
    traffic = Counter((r["op"], int(r["bank"])) for r in accesses)
    for bank, r in latest.items():
        assert traffic["R", bank] == int(r["read_words"])
        assert traffic["W", bank] == int(r["write_words"])
    pools = {r["pool"]: r for r in fields(text, "CFD_RESOURCE_STATS")}
    windows = []
    issued_total = Counter()
    for s in schedules:
        token = s["token"]
        start = min(int(r["cycle"]) for r in interface
                    if r["token"] == token and r["kind"] == "descriptor")
        end = int(s["visible_done"]) + 1
        selected = [r for r in ops if start <= int(r["cycle"]) < end]
        count = Counter()
        for r in selected:
            count[r["pool"]] += int(r["lanes"])
        issued_total.update(count)
        intervals = [(int(r["cycle"]), int(r["complete_cycle"])) for r in selected]
        back, done = int(s["backward_start"]), int(s["backward_done"])
        back_intervals = [(max(a, back), min(b, done)) for a, b in intervals
                          if min(b, done) > max(a, back)]
        back_ops = [r for r in selected if back <= int(r["cycle"]) < done]
        back_callers = Counter()
        for r in back_ops:
            back_callers[r["caller"]] += int(r["lanes"])
        banks = Counter(r["op"] for r in accesses if start <= int(r["cycle"]) < end)
        metadata = Counter()
        for r in interface:
            if r["token"] == token and r["event"] == "issue":
                metadata[r["kind"]] += int(r["bytes"])
        windows.append(dict(token=token, cache_hit=int(s["cache_hit"]),
            controller_cycles=end-start, first_math_delay=min(a for a, _ in intervals)-start,
            completion_tail_cycles=end-done, issued=dict(count),
            cluster_empty_cycles=end-start-union(intervals),
            bank_words=dict(banks), metadata_dma_bytes=dict(metadata),
            backward=dict(cycles=done-back, any_math_active_cycles=union(back_intervals),
                math_inactive_cycles=done-back-union(back_intervals),
                tile_wait_counter=int(storage[token]["data_wait"]),
                output_queue_wait_counter=int(s["output_queue_stalls"]),
                callers=dict(back_callers)), storage=storage[token]))
    cycles = sum(r["controller_cycles"] for r in windows)
    roi = int(re.search(r"streamingStep2.totalCycles = (\d+)", text)[1])
    issue = {k: 100*issued_total[k]*int(pools["shared_"+k]["ii"])/(cycles*int(pools["shared_"+k]["count"]))
             for k in ("mul", "add", "div")}
    bank_pct = {}
    for name, op, key in (("read", "R", "read_words"), ("write", "W", "write_words")):
        denom = cycles * int(onchip["banks"]) * int(onchip[name+"_ports"])
        words = sum(int(r[key]) for r in latest.values())
        assert words == sum(r["bank_words"].get(op, 0) for r in windows)
        bank_pct[name] = dict(words=words, corrected_pct=100*words/denom,
            historical_sum_of_snapshots_pct=100*sum(int(r[key]) for r in bank_snapshots)/denom)
    clock = json.loads((path/"config.json").read_text())["system"]["clk_domain"]["clock"][0]
    packets = fields(text, "CFD_DMA_PACKET")
    queued = {r["packet"]: r for r in packets if r["event"] == "queue"}
    per_cycle = Counter()
    per_tick = Counter()
    bytes_cycle = Counter()
    for r in packets:
        if r["event"] == "send":
            per_cycle[int(r["tick"])//clock] += 1
            per_tick[int(r["tick"])] += 1
            bytes_cycle[int(r["tick"])//clock] += int(queued[r["packet"]]["bytes"])
    return dict(case=str(path.relative_to(ROOT)), roi_cycles=roi,
        gem5_sha256=manifest["gem5_sha256"], controller_cycles=cycles,
        issue_slot_pct=issue, cluster_empty_pct=100*sum(w["cluster_empty_cycles"] for w in windows)/cycles,
        bank_port_utilization=bank_pct, snapshots_per_bank=len(bank_snapshots)//len(latest),
        peak_dma_packets_per_cycle=max(per_cycle.values()),
        peak_dma_packets_same_tick=max(per_tick.values()),
        peak_dma_requested_bytes_per_cycle=max(bytes_cycle.values()),
        numeric=fields(text, "CFD_SINGLE_LINE_NUMERIC"), windows=windows)


def main():
    cases = [("single-line-round3-final-20261015", f"n{n}-{phase}-{mode}-l11")
             for n in (17, 65) for phase in ("cold", "reuse") for mode in ("direct", "inverse")]
    cases += [("single-line-round3-final-tile-20261015", f"n65-cold-{mode}-l11-t32")
              for mode in ("direct", "inverse")]
    cases += [("single-line-review-20261018", n) for n in
              ("fresh-n17-cold-direct", "fresh-n17-reuse-inverse", "fresh-n65-cold-direct", "capacity-n115", "capacity-n116")]
    data = {campaign+"/"+name: measure(CAMPAIGNS/campaign/name) for campaign, name in cases}
    (OUT/"metrics.json").write_text(json.dumps(data, indent=2)+"\n")
    paths = [ROOT/"build/ARM/gem5.opt", ROOT/"build/cfd_dsa/aarch64/test_cfd_lusgs_patha_step2_arm",
             OUT/"guest/test_cfd_lusgs_patha_step2_arm", OUT/"guest/cache_failure_probe.c"]
    (OUT/"binary-provenance.json").write_text(json.dumps({str(p.relative_to(ROOT)): digest(p) for p in paths}, indent=2)+"\n")
    print("REVIEW_RECOMPUTE_PASS", len(data), "cases; bank counters match every raw access")


if __name__ == "__main__":
    main()
