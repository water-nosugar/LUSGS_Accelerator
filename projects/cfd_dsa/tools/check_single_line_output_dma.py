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


def resolved_options(path):
    """Include profile defaults, checking explicit arguments against execution."""
    path = Path(path)
    config = options(json.loads((path / 'manifest.json').read_text())['command'])
    resolved_path = path / 'resolved_experiment.json'
    if resolved_path.exists():
        actual = json.loads(resolved_path.read_text()).get('options', {})
        sme_2d_defaults = {'coeff_sme_array_mode': '1d', 'coeff_sme_2d_segments': 1,
                           'coeff_sme_2d_panel_pack': False,
                           'coeff_sme_2d_mixed_kind': False,
                           'coeff_sme_2d_panel_b_overlap': False,
                           'coeff_sme_forward_tile_rotate': False,
                           'coeff_sme_2d_candidate_window': 0,
                           'coeff_sme_feedback_reg': False}
        for key in ('coeff_data_commit_depth', 'coeff_warm_diagonal',
                    'coeff_line_mvm_model', 'coeff_warm_forward_tree', 'coeff_cold_forward_tree',
                    'coeff_extended_input_slots', 'coeff_compute_backend',
                    'coeff_warm_rhs_tree', 'coeff_drain_policy',
                    'coeff_drain_arb_latency', 'coeff_drain_arb_ii',
                    'coeff_rhs_input_coalesce', 'coeff_tile_reload','coeff_rhs_lookahead',
                    'coeff_onchip_compute_reserve','coeff_tile_operand_stream',
                    'coeff_output_coalesce','coeff_dma_cache_size',
                    'coeff_dma_cache_mshrs','coeff_dma_cache_write_buffers',
                    'coeff_cluster_mul_count','coeff_extra_mul_policy',
                    'coeff_backward_feedback_tree','coeff_rhs_supply_priority','coeff_cold_b_tree',
                    'coeff_cold_rhs_tree','coeff_tail_ubar_overlap','coeff_lu_lookahead',
                    'coeff_publish_lu','coeff_lu_output_transition_test',
                    'coeff_rf_read_words','coeff_rf_write_words',
                    'coeff_onchip_arb_latency','coeff_onchip_arb_tasks',
                    'coeff_line_output_depth','coeff_tile_prefetch_buffers',
                    'coeff_rf_broadcast','coeff_rf_split_writeback',
                    'coeff_sme_packed_input', *sme_2d_defaults):
            flag = '--' + key.replace('_', '-')
            if key not in actual and (key not in sme_2d_defaults or flag not in config):
                continue
            effective = actual.get(key, sme_2d_defaults.get(key))
            value = str(int(effective)) if isinstance(effective, bool) else str(effective)
            explicit = config.get(flag)
            if explicit is not None and key in sme_2d_defaults:
                if isinstance(effective, bool):
                    spelling = explicit.lower()
                    if spelling in ('1', 'true', 'yes', 'on'):
                        explicit = '1'
                    elif spelling in ('0', 'false', 'no', 'off'):
                        explicit = '0'
                elif isinstance(effective, int):
                    explicit = str(int(explicit))
                else:
                    explicit = explicit.strip().lower()
            inherited_output = (key == 'coeff_line_output_depth' and
                                explicit == '0')
            if explicit is not None and explicit != value and not inherited_output:
                raise ValueError(f'{flag}: manifest disagrees with resolved configuration')
            config[flag] = value
    return config


def output_depth(config):
    """Independent final FIFO; old manifests inherit preprocessing depth."""
    return int(config.get('--coeff-line-output-depth','0')) or int(config['--coeff-drain-queue-depth'])


def fixed_control_reserve(config):
    """Explicit FIFO allocation was added after the legacy 16 KiB allowance."""
    extra=64*max(0,drain_task_depth(config)-int(config.get('--coeff-output-buffer-depth','4')))
    extra+=128*int(config.get('--coeff-summary-engines','1'))+512*int(config.get('--coeff-summary-depth','0')) if config.get('--coeff-record-policy') in ('line-summary','cold-summary') else 0
    return 16384+(64*output_depth(config) if '--coeff-line-output-depth' in config else 0)+extra


def drain_task_depth(config):
    return int(config.get('--coeff-drain-task-depth','0')) or int(config.get('--coeff-output-buffer-depth','4'))


def validate_row(row, cells, depth, dma, coalesced=False):
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
        if get("output_packets_issued") < ((cells*40+63)//64 if coalesced else cells):
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
        config = resolved_options(path)
        if config.get("--lusgs-lines") != "1" or config.get("--lusgs-interleave") != "1":
            raise ValueError(f"{name}: outside single-line scope")
        if not any(str(arg).endswith("run_cfd_lusgs_single_line.py")
                   for arg in manifest["command"]):
            raise ValueError(f"{name}: not using the scope-checked runner")
        cells = int(config["--lusgs-cells"])
        depth = output_depth(config)
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
