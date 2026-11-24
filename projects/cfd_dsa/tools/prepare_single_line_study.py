#!/usr/bin/env python3
"""Freeze a uniform-config single-line study with stationary synthetic inputs.

Synthetic coefficients are not physical CFD data.  They provide reproducible
length and coupling controls without the old cell-dependent diagonal trend.
Independent GT01R paths, when enabled, remain projections of the official
problem rather than complete CFD solves.  No simulation or result selection is
performed here.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct

from extract_gt01r import (ARCHIVE_SHA256, SOURCE_URL, block_path, graph,
                          read_archive, records_for_path, write_json)
from single_line_dataset import oracle, sha256, unpivoted_diagnostics, write_dataset

REPO = Path(__file__).resolve().parents[3]
DEFAULT_DIRECTORY = REPO / "results/cfd_dsa/datasets/single-line-study-v2-20261024"
DEFAULT_SUITE = REPO / "projects/cfd_dsa/suites/single-line-study-v2-20261024.json"
BOUNDARIES = (64, 65, 67, 68, 85, 86, 128, 129)
HOLDOUT_SEEDS = (211, 307)
COUPLINGS = (0.35, 1.4)
ORACLE_PRECISIONS = (200, 240)
TOLERANCE = 1e-10


def _unit(seed, cell, field, entry):
    """Specified counter-based sample; independent of N and traversal order."""
    key = f"CFD5-STATIONARY-v1:{seed}:{cell}:{field}:{entry}".encode("ascii")
    bits = int.from_bytes(hashlib.sha256(key).digest()[:8], "little") >> 11
    return bits / (1 << 53)


def stationary_records(cells, seed=101, coupling=0.35):
    """Return raw D/L/U/R (80 doubles/cell), masking only line endpoints.

    D off-diagonals are U[-.12,.12), diagonals U[3.5,4.5), and R
    U[-1,1).  There is no dependence on N or absolute cell index except
    using the index to select an independent, fixed-distribution sample.
    Each L/U row has L1 norm coupling/2 times the corresponding strict
    diagonal-dominance margin of D.  Coupling therefore measures the sum
    of both neighbor norms relative to that margin.  Values up to 1.8
    allow non-dominance of the *full* line while bounding each one-way
    recurrence amplification below .9 in the infinity-norm estimate.
    """
    if not isinstance(cells, int) or isinstance(cells, bool) or cells < 1:
        raise ValueError("cells must be a positive integer")
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0:
        raise ValueError("seed must be a nonnegative integer")
    if not math.isfinite(coupling) or not 0 <= coupling <= 1.8:
        raise ValueError("coupling must be finite and in [0,1.8]")
    records = []
    for cell in range(cells):
        d = [(3.5 + _unit(seed, cell, 'D', r*5+c)) if r == c else
             0.12*(2*_unit(seed, cell, 'D', r*5+c)-1)
             for r in range(5) for c in range(5)]
        margins = [d[r*5+r]-math.fsum(abs(d[r*5+c]) for c in range(5) if c != r)
                   for r in range(5)]
        neighbors = []
        for field in ('L', 'U'):
            matrix = []
            for row in range(5):
                values = [2*_unit(seed, cell, field, row*5+c)-1 for c in range(5)]
                scale = coupling*margins[row]/(2*math.fsum(map(abs, values)))
                matrix.extend(v*scale for v in values)
            neighbors.append(matrix)
        lower, upper = neighbors
        if cell == 0:
            lower = [0.0]*25
        if cell + 1 == cells:
            upper = [0.0]*25
        rhs = [2*_unit(seed, cell, 'R', r)-1 for r in range(5)]
        records.append(d+lower+upper+rhs)
    return records


def freeze_args(reference, overrides=None, remove=()):
    """Canonicalize last-wins CLI options, then apply explicit replacements.

    All study options use --name=value, so accidental parser-dependent
    duplicate flags cannot silently change the intended experiment.
    """
    options = {}
    for argument in reference:
        if not argument.startswith('--') or '=' not in argument:
            raise ValueError(f"expected --name=value: {argument}")
        key, value = argument.split('=', 1)
        options[key] = value
    for key in remove:
        options.pop(key, None)
    for key, value in (overrides or {}).items():
        if not key.startswith('--') or '=' in key:
            raise ValueError(f"invalid option key: {key}")
        options[key] = str(value)
    return [f'{key}={value}' for key, value in options.items()]


def _normalized_residuals(raw, expected):
    maxima = [0.0, 0.0]
    for cell, (row, values) in enumerate(zip(raw, expected)):
        y, dq = values[:5], values[5:]
        for r in range(5):
            d_y = [row[r*5+c]*y[c] for c in range(5)]
            l_y = ([row[25+r*5+c]*expected[cell-1][c] for c in range(5)]
                   if cell else [])
            terms = d_y+l_y+[-row[75+r]]
            maxima[0] = max(maxima[0], abs(math.fsum(terms)) /
                            max(1e-300, math.fsum(map(abs, terms))))
            d_correction = [row[r*5+c]*(dq[c]-y[c]) for c in range(5)]
            u_dq = ([row[50+r*5+c]*expected[cell+1][5+c] for c in range(5)]
                    if cell+1 < len(raw) else [])
            terms = d_correction+u_dq
            maxima[1] = max(maxima[1], abs(math.fsum(terms)) /
                            max(1e-300, math.fsum(map(abs, terms))))
    return {'forward_normalized_residual': maxima[0],
            'backward_normalized_residual': maxima[1]}


def diagnostics(raw, expected):
    """Finite/pivot/growth and recurrence diagnostics; no tolerance changes."""
    margins, diagonals, ratios = [], [], []
    for row in raw:
        for r in range(5):
            diag = abs(row[r*5+r])
            margin = diag-math.fsum(abs(row[r*5+c]) for c in range(5) if c != r)
            margins.append(margin)
            diagonals.append(diag)
            if margin > 0:
                ratios.extend(math.fsum(abs(row[offset+r*5+c]) for c in range(5))/margin
                              for offset in (25, 50))
    middle = max(1, len(diagonals)//2)
    return dict(unpivoted_diagnostics(raw), **_normalized_residuals(raw, expected),
                min_diagonal_dominance_margin=min(margins),
                mean_abs_diagonal=statistics.fmean(diagonals),
                first_half_mean_abs_diagonal=statistics.fmean(diagonals[:middle]),
                second_half_mean_abs_diagonal=statistics.fmean(diagonals[middle:] or diagonals),
                max_neighbor_row_norm_over_D_margin=max(ratios, default=0),
                max_abs_y=max(abs(v) for row in expected for v in row[:5]),
                max_abs_dq=max(abs(v) for row in expected for v in row[5:]))


def audited_dataset(path, raw):
    """Require independent 200/240-digit oracles to round identically to FP64."""
    low = oracle(raw, ORACLE_PRECISIONS[0])
    high = oracle(raw, ORACLE_PRECISIONS[1])
    rounded_low = b''.join(struct.pack('<10d', *row) for row in low)
    rounded_high = b''.join(struct.pack('<10d', *row) for row in high)
    if rounded_low != rounded_high:
        raise ValueError('200/240-digit independent oracle FP64 rounding differs')
    info = write_dataset(path, raw, expected=low, precision=ORACLE_PRECISIONS[0])
    info.update(oracle_crosscheck_precision=ORACLE_PRECISIONS[1],
                oracle_fp64_identical=True,
                oracle_rounded_sha256=hashlib.sha256(rounded_low).hexdigest(),
                validation_tolerance=TOLERANCE,
                diagnostics=diagnostics(raw, low))
    return info


def prepare_study(directory, suite_path, campaign='single-line-study-v2-20261024',
                  include_gt=True):
    directory, suite_path = Path(directory).resolve(), Path(suite_path).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    source = REPO/'projects/cfd_dsa/suites/history/single-line-joint-recommended.json'
    reference = json.loads(source.read_text())['cases'][1]['args']
    common = freeze_args(reference, {
        '--coeff-cold-rhs-tree': 'none', '--coeff-cold-b-tree': 0,
        '--coeff-tail-ubar-overlap': 0, '--coeff-lu-lookahead': 1,
        '--coeff-study-cache': 'prepared',
        '--bench-iters': 1, '--coefficient-update-interval': 1,
    }, remove=('--lusgs-cells', '--coeff-preprocess-trace-cells', '--coeff-input-file',
               '--bench-iters', '--coefficient-update-interval'))
    # bench-iters/update interval belong in each case, never both common/case.
    common = freeze_args(common, remove=('--bench-iters', '--coefficient-update-interval'))
    rows, cases, rejected = [], [], []

    def add(name, raw, role, iterations, **metadata):
        path = directory/(name+'.cfd5')
        row = dict(name=name, role=role, **audited_dataset(path, raw), **metadata)
        row['path'] = str(path)
        write_json(directory/(name+'.json'), row)
        rows.append(row)
        cases.append(dict(name=name, target='lusgs-single-line', args=[
            f'--lusgs-cells={len(raw)}', f'--coeff-preprocess-trace-cells={len(raw)}',
            f'--coeff-input-file={path}', f'--bench-iters={iterations}',
            f'--coefficient-update-interval={iterations}']))

    for cells in BOUNDARIES:
        add(f'stationary-boundary-s101-c035-n{cells}', stationary_records(cells),
            'capacity-boundary', 3, seed=101, coupling=0.35,
            source='synthetic stationary-distribution control',
            expected_capacity_regime=('full' if cells <= 67 else 'partial' if cells <= 85 else 'none'))
    for cells in (257, 1024):
        for seed in HOLDOUT_SEEDS:
            for coupling in COUPLINGS:
                tag = f'{round(coupling*100):03d}'
                add(f'stationary-holdout-s{seed}-c{tag}-n{cells}',
                    stationary_records(cells, seed, coupling), 'held-out-input', 1,
                    seed=seed, coupling=coupling,
                    source='synthetic stationary-distribution control')
    if include_gt:
        archive = REPO/'results/cfd_dsa/datasets/Fluorem/GT01R.tar.gz'
        blocks, rhs, members = read_archive(archive)
        neighbors, norms = graph(blocks, len(rhs)//5)
        eligible = [node for node in range(len(neighbors))
                    if neighbors[node] and any(rhs[5*node:5*(node+1)])]
        for anchor in HOLDOUT_SEEDS:
            # Selection depends only on the original RHS/graph and anchor,
            # never on oracle values, numerical pass/fail or measured speed.
            candidates = [node for node in eligible if node >= anchor]
            if not candidates:
                rejected.append(dict(anchor=anchor, phase='dataset-generation',
                                     reason='no nonzero-RHS start at or above anchor'))
                continue
            start = min(candidates)
            name = f'gt01r-holdout-s{start}-n129'
            try:
                path = block_path(start, 129, neighbors, norms)
                add(name, records_for_path(path, blocks, rhs), 'held-out-real-path', 1,
                    source=SOURCE_URL, archive_sha256=ARCHIVE_SHA256,
                    selection_anchor=anchor, selected_start=start,
                    start_selection='smallest original node ID >= anchor with nonzero original RHS and bidirectional neighbors',
                    member_sha256=members, original_node_ids=path,
                    derivation='bidirectional index-nearest block path; path-external couplings omitted; not complete CFD')
            except ValueError as error:
                rejected.append(dict(name=name, phase='dataset-generation', reason=str(error)))
    # Verify the final effective list has no common/case duplicate keys.
    for case in cases:
        arguments = common+case['args']
        keys = [a.split('=', 1)[0] for a in arguments]
        if len(keys) != len(set(keys)):
            raise AssertionError('study arguments contain duplicate keys')
    config_sha = hashlib.sha256(('\n'.join(common)+'\n').encode()).hexdigest()
    manifest = dict(schema_version=2, generator='CFD5-STATIONARY-v1',
                    oracle_precisions=list(ORACLE_PRECISIONS), tolerance=TOLERANCE,
                    tolerance_changed=False, frozen_global_config_sha256=config_sha,
                    reference_suite_sha256=sha256(source), cases=rows, rejected=rejected,
                    source='synthetic stationary controls and optional GT01R block-path projections',
                    limitations=['Synthetic records are not physical CFD fields.',
                                 'Coupling strength controls numerical dependence, not graph shape.',
                                 'Three identical-input calls measure coefficient reuse, not CFD convergence.',
                                 'System-cache state is not flushed by this input generator.',
                                 'Holdout designation means excluded from prior tuning, not a second source matrix.'])
    write_json(directory/'manifest.json', manifest)
    suite = dict(campaign=campaign, description='Frozen global single-line config; capacity boundaries and held-out stationary/GT01R inputs.',
                 common_args=common, cases=cases, manifest=str(directory/'manifest.json'),
                 frozen_global_config_sha256=config_sha,
                 notes=manifest['limitations'])
    suite_path.parent.mkdir(parents=True, exist_ok=True)
    write_json(suite_path, suite)
    return manifest, suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset-dir', type=Path, default=DEFAULT_DIRECTORY)
    parser.add_argument('--output', type=Path, default=DEFAULT_SUITE)
    parser.add_argument('--campaign', default='single-line-study-v2-20261024')
    parser.add_argument('--no-gt', action='store_true')
    args = parser.parse_args()
    manifest, suite = prepare_study(args.dataset_dir, args.output, args.campaign, not args.no_gt)
    print(json.dumps(dict(cases=len(suite['cases']), rejected=manifest['rejected'],
                          manifest=str(args.dataset_dir.resolve()/'manifest.json'),
                          suite=str(args.output.resolve())), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
