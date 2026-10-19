#!/usr/bin/env python3
"""Verify one-line numerical identity, bounded output DMA and visibility fence."""

import argparse
import json
from pathlib import Path
import re


def options(command):
    result = {}
    iterator = iter(command)
    for arg in iterator:
        if not arg.startswith("--"):
            continue
        key, equal, value = arg.partition("=")
        if not equal:
            value = next(iterator, "")
        result[key] = value
    return result


def validate_row(row, cells, depth, dma):
    get = lambda key: int(row[key])
    if not (get("backward_start") <= get("backward_done") <= get("visible_done")):
        raise ValueError("invalid numerical/visible completion order")
    if get("children_retired") > get("visible_done"):
        raise ValueError("completion before child retirement")
    expected = cells * 40 if dma else 0
    if get("output_bytes_issued") != expected or get("output_bytes_completed") != expected:
        raise ValueError("missing, duplicated or incomplete final-vector DMA bytes")
    if get("output_packets_issued") != get("output_packets_completed"):
        raise ValueError("outstanding final-vector DMA at completion")
    if dma:
        if get("output_packets_issued") < cells:
            raise ValueError("too few final-vector packets")
        if not 1 <= get("output_queue_max") <= depth:
            raise ValueError("output queue capacity violated")
        if not 0 < get("output_last_response") <= get("visible_done"):
            raise ValueError("software completion precedes the DMA response")
    elif get("output_packets_issued") or get("output_queue_max"):
        raise ValueError("non-DMA control unexpectedly issued final-vector DMA")


def check(root):
    suite_path = Path(__file__).resolve().parents[1] / "suites/single-line-output-dma.json"
    suite = json.loads(suite_path.read_text())
    records, cycles, binary_hashes = {}, {}, set()
    for case in suite["cases"]:
        name = case["name"]
        path = root / name
        manifest = json.loads((path / "manifest.json").read_text())
        if manifest["status"] != "passed":
            raise ValueError(f"{name}: simulation failed")
        config = options(manifest["command"])
        if config.get("--lusgs-lines") != "1" or config.get("--lusgs-interleave") != "1":
            raise ValueError(f"{name}: outside single-line scope")
        if not any(str(arg).endswith("run_cfd_lusgs_single_line.py")
                   for arg in manifest["command"]):
            raise ValueError(f"{name}: not using the scope-checked runner")
        cells = int(config["--lusgs-cells"])
        depth = int(config["--coeff-drain-queue-depth"])
        sweeps = int(config["--bench-iters"])
        dma = config["--coeff-spm-model"] == "dma"
        text = (path / "simout.txt").read_text() + (path / "driver.log").read_text()
        if "LUSGS_STEP2_STREAMING_PASS" not in text or "_FAIL" in text:
            raise ValueError(f"{name}: missing full numerical validation")
        rows = [dict(re.findall(r"(\w+)=(\w+)", line))
                for line in text.splitlines() if line.startswith("CFD_LINE_SCHEDULE ")]
        rows.sort(key=lambda row: int(row["token"]))
        if len(rows) != sweeps or len({row["token"] for row in rows}) != sweeps:
            raise ValueError(f"{name}: incorrect completion count")
        if sum(int(row["cache_hit"]) for row in rows) != (1 if sweeps == 3 else 0):
            raise ValueError(f"{name}: unexpected coefficient reuse pattern")
        for row in rows:
            validate_row(row, cells, depth, dma)
        if dma:
            system = json.loads((path / "config.json").read_text())["system"]
            cache = system.get("coeff_dma_cache", {})
            cache_parameters = dict(size=1024, assoc=8, tag_latency=50,
                                    data_latency=50, response_latency=50,
                                    mshrs=20, tgts_per_mshr=12)
            if (any(cache.get(key) != value
                    for key, value in cache_parameters.items())
                    or system["cpu"]["local_spm"]["dma_port"]["peer"]
                    != "system.coeff_dma_cache.cpu_side"
                    or not cache["mem_side"]["peer"].startswith(
                        "system.toL2Bus.cpu_side_ports[")):
                raise ValueError(f"{name}: missing coherent DMA cache path")
            stats = (path / "stats.txt").read_text()
            dma_counters = {}
            for metric in ("coeffDmaIssued", "coeffDmaCompleted", "coeffDmaWriteBytes"):
                match = re.search(r"\." + metric + r"\s+(\d+)", stats)
                if not match:
                    raise ValueError(f"{name}: missing actual timing DMA statistics")
                dma_counters[metric] = int(match[1])
                if metric == "coeffDmaWriteBytes" and int(match[1]) < cells * 40 * sweeps:
                    raise ValueError(f"{name}: final output missing from memory traffic")
            if dma_counters["coeffDmaIssued"] != dma_counters["coeffDmaCompleted"]:
                raise ValueError(f"{name}: outstanding timing DMA when simulation ends")
            if dma_counters["coeffDmaIssued"] < sum(
                    int(row["output_packets_issued"]) for row in rows):
                raise ValueError(f"{name}: final packets missing from actual DMA counters")
        match = re.search(r"streamingStep2\.totalCycles = (\d+)", text)
        if not match:
            raise ValueError(f"{name}: missing total cycle count")
        cycles[name] = int(match[1])
        records[name] = rows
        binary_hashes.add(manifest["gem5_sha256"])
        print(f"{name}: cycles={cycles[name]} final_dma_bytes="
              f"{sum(int(row['output_bytes_completed']) for row in rows)} "
              f"queue_max={max(int(row['output_queue_max']) for row in rows)}")
    if len(binary_hashes) != 1:
        raise ValueError("suite used different simulator binaries")
    groups = [
        ("internal-serial-17", "internal-split-17", "internal-serial7-17",
         "internal-split7-17", "dma-serial-17", "dma-split-17", "dma-q1-outstanding1"),
        ("internal-reuse", "dma-reuse", "dma-reuse-q1"),
    ]
    for group in groups:
        expected = [row["result_hash"] for row in records[group[0]]]
        for name in group[1:]:
            if [row["result_hash"] for row in records[name]] != expected:
                raise ValueError(f"{name}: FP64 digest differs from its same-build control")
    if not any(int(row["output_queue_stalls"]) > 0
               for row in records["dma-q1-outstanding1"]):
        raise ValueError("output queue backpressure not exercised")
    if int(records["dma-257"][0]["output_packets_issued"]) <= 257:
        raise ValueError("cache-line splitting not exercised")
    print("SINGLE_LINE_OUTPUT_DMA_CHECK_PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    check(parser.parse_args().campaign)
