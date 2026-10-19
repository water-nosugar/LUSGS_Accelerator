#!/usr/bin/env python3
"""Capacity ledger for one FP64 5x5 LU-SGS line, not a DMA/spill simulation."""

import argparse
import json


def ledger(cells, budget_bytes, reserved_bytes=0):
    if cells < 1 or budget_bytes < 0 or not 0 <= reserved_bytes <= budget_bytes:
        raise ValueError("invalid cell count, budget or reserved capacity")
    available = budget_bytes - reserved_bytes
    # Ubar at the last cell is not consumed. This is a data-only lower bound;
    # reserved_bytes must include contexts, input staging, output queue, etc.
    backward_bytes = cells * 40 + (cells - 1) * 200
    return {
        "cells": cells, "spm_budget_bytes": budget_bytes,
        "reserved_bytes": reserved_bytes, "available_backward_bytes": available,
        "y_bytes": cells * 40, "required_ubar_bytes": (cells - 1) * 200,
        "backward_live_data_bytes": backward_bytes,
        "all_backward_data_fits": backward_bytes <= available,
        "minimum_overflow_bytes": max(0, backward_bytes - available),
        "max_cells_with_boundary_elision": max(0, (available + 200) // 240),
        "current_parent_numeric_array_payload_bytes": cells * 720,
        "additional_version_cache_array_payload_bytes": cells * 600,
        "scope": "single line; payload accounting only; no spill traffic inferred",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cells", type=int, nargs="+", default=[17, 257, 4096])
    parser.add_argument("--spm-kib", type=int, default=1024)
    parser.add_argument("--reserved-kib", type=int, default=64)
    args = parser.parse_args()
    try:
        result = [ledger(cells, args.spm_kib * 1024, args.reserved_kib * 1024)
                  for cells in args.cells]
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
