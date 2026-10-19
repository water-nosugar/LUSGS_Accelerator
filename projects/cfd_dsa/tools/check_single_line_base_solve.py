#!/usr/bin/env python3
"""Audit cold single-line direct solve versus inverse with fixed resources."""
import argparse
from collections import Counter
import csv
import json
import math
from pathlib import Path
import re

from check_context_ablation import read_case
from check_single_line_output_dma import options, validate_row


# These are simulator operation widths and callers, not synthesis area.
RESOURCE_ROLES = {
    'lu_div': (1, 'LU scalar divide'),
    'lu_mul': (1, 'LU scalar multiply'),
    'lu_sub': (1, 'LU scalar subtract'),
    'coeff_div': (1, 'RHS scalar divide; separate from LU'),
    'coeff_mul': (1, 'RHS scalar multiply; separate from LU'),
    'coeff_sub': (1, 'RHS scalar subtract; separate from LU'),
    'column_fma': (5, 'cold base/Lbar; serial cached forward/backward'),
    'forward_combine': (5, 'cold forward vector subtract'),
    'line_product': (5, 'split cached forward/backward multiply'),
    'line_accumulate': (5, 'split cached forward/backward ordered add'),
}


def check_case(path, mode, cells, sweeps):
    cycles, hashes, pools, _, binary = read_case(path)
    manifest = json.loads((path / 'manifest.json').read_text())
    config = options(manifest['command'])
    if config.get('--coeff-base-solve') != mode or \
            config.get('--lusgs-lines') != '1' or \
            config.get('--lusgs-interleave') != '1' or \
            config.get('--coefficient-update-interval') != '1' or \
            config.get('--coeff-validation') != 'full':
        raise ValueError(f'{path.name}: wrong solve/scope/validation contract')
    if not any(str(arg).endswith('run_cfd_lusgs_single_line.py')
               for arg in manifest['command']):
        raise ValueError(f'{path.name}: missing single-line runner')
    text = (path / 'simout.txt').read_text()
    if '_FAIL' in text:
        raise ValueError(f'{path.name}: failed validation')
    numeric = [dict(re.findall(r'(\w+)=([^ ]+)', line))
               for line in text.splitlines()
               if line.startswith('CFD_SINGLE_LINE_NUMERIC ')]
    if len(numeric) != sweeps or len(hashes) != sweeps:
        raise ValueError(f'{path.name}: missing complete sweep validation')
    if len(set(hashes)) != 1:
        raise ValueError(f'{path.name}: unchanged inputs gave unstable results')
    for row in numeric:
        if row['base_solve'] != mode or int(row['values']) != 10 * cells or \
                int(row['mismatches']) != 0 or \
                not all(math.isfinite(float(row[k])) and float(row[k]) >= 0
                        for k in ('max_abs', 'max_rel')):
            # The guest counts failures per value using abs OR relative
            # tolerance. Separate global maxima can come from different
            # values, so their conjunction is not a valid failure test.
            raise ValueError(f'{path.name}: numerical error or missing values')
    rows = [dict(re.findall(r'(\w+)=(\w+)', line))
            for line in text.splitlines()
            if line.startswith('CFD_LINE_SCHEDULE ')]
    if len(rows) != sweeps or any(int(r['cache_hit']) for r in rows):
        raise ValueError(f'{path.name}: expected a rebuild every sweep')
    for row in rows:
        validate_row(row, cells, int(config['--coeff-drain-queue-depth']),
                     config['--coeff-spm-model'] == 'dma')
    with (path / 'coeff_preprocess_trace.csv').open(newline='') as source:
        trace = list(csv.DictReader(source))
    expected = Counter({cell: sweeps for cell in range(cells)})
    for stage in ('REQUEST_ALLOC', 'REQUEST_COMPLETE', 'REQUEST_AUTO_RETIRE',
                  'HANDOFF_PRODUCE'):
        observed = Counter(int(r['cell']) for r in trace if r['stage'] == stage)
        if observed != expected:
            raise ValueError(f'{path.name}: {stage} missing or duplicated')
    produced = {}
    consumed = Counter()
    for row in trace:
        cell = int(row['cell'])
        generation = int(row['generation']) - cell
        if row['stage'] == 'HANDOFF_PRODUCE':
            produced[generation, cell] = int(row['cycle'])
        elif row['stage'] == 'HANDOFF_CONSUME':
            ready = produced.get((generation, cell - 1))
            if cell == 0 or ready is None or int(row['cycle']) < ready:
                raise ValueError(f'{path.name}: forward dependency violated')
            consumed[cell] += 1
    if consumed != Counter({cell: sweeps for cell in range(1, cells)}):
        raise ValueError(f'{path.name}: forward handoff missing/duplicated')
    generations = sorted({generation for generation, _ in produced})
    if len(generations) != sweeps:
        raise ValueError(f'{path.name}: invalid sweep generation count')
    for generation, row in zip(generations, sorted(rows, key=lambda r: int(r['token']))):
        if int(row['backward_start']) < produced[generation, cells - 1]:
            raise ValueError(f'{path.name}: backward started before the final y')
    # Check every enabled RHS, including exact first/last boundary masks.
    actual = Counter((int(r['cell']), int(r['rhsBatch']), int(r['rhsColumn']))
                     for r in trace if r['stage'] == 'RHS_COMPLETE')
    wanted = Counter()
    for cell in range(cells):
        for col in range(1 if mode == 'direct' else 5):
            wanted[cell, 0, col] = sweeps
        for batch, needed in ((1, cell > 0), (2, cell + 1 < cells)):
            if needed:
                for col in range(5):
                    wanted[cell, batch, col] = sweeps
    if actual != wanted:
        raise ValueError(f'{path.name}: wrong RHS work or boundary mask')
    direct_ready = Counter(int(r['cell']) for r in trace
                           if r['stage'] == 'DIRECT_BASE_READY')
    if direct_ready != (expected if mode == 'direct' else Counter()):
        raise ValueError(f'{path.name}: direct base result missing/duplicated')
    if mode == 'direct' and any(r['stage'] == 'DINV_READY' or
                               (r['stage'] == 'COLUMN_FMA_ISSUE' and
                                r['rhsBatch'] == '0') for r in trace):
        raise ValueError(f'{path.name}: explicit inverse work in direct mode')
    rhs_count = sum(wanted.values())
    split = config['--coeff-line-mvm-model'] == 'split'
    reciprocal = config['--coeff3-div-mode'] == 'reciprocal'
    expected_issues = dict(lu_div=10*cells*sweeps, lu_mul=30*cells*sweeps,
                           lu_sub=30*cells*sweeps,
                           coeff_div=5*cells*sweeps if reciprocal else 5*rhs_count,
                           coeff_mul=(25 if reciprocal else 20)*rhs_count,
                           coeff_sub=20*rhs_count,
                           forward_combine=(cells-1)*sweeps)
    expected_issues['column_fma'] = sweeps * (
        (5*cells if mode == 'inverse' else 0) +
        5*(cells-1)*(1 if split else 2))
    if split:
        expected_issues.update(line_product=5*(cells-1)*sweeps,
                               line_accumulate=5*(cells-1)*sweeps)
    if pools.keys() != expected_issues.keys():
        raise ValueError(f'{path.name}: missing/unexpected resource pools')
    for pool, count in expected_issues.items():
        if pools[pool]['issued'] != count:
            raise ValueError(f'{path.name}/{pool}: expected {count} issues, '
                             f'got {pools[pool]["issued"]}')
    def stat(name):
        match = re.search(r'\.' + re.escape(name) + r' = (\d+)', text)
        if not match:
            raise ValueError(f'{path.name}: missing {name}')
        return int(match[1])
    if stat('coeff3ColumnsSolved') != rhs_count or \
            stat('streamingStep2.effectiveRhsCount') != rhs_count or \
            stat('dInvGenerated') != (cells*sweeps if mode == 'inverse' else 0) or \
            stat('baseDirectRhsSolved') != (cells*sweeps if mode == 'direct' else 0) or \
            stat('commonPreprocessFallbackRuns'):
        raise ValueError(f'{path.name}: incorrect work stats or hidden fallback')
    if config['--coeff-spm-model'] == 'dma':
        stats = (path / 'stats.txt').read_text()
        dma = {}
        for key in ('coeffDmaIssued', 'coeffDmaCompleted', 'coeffDmaWriteBytes'):
            match = re.search(r'\.' + key + r'\s+(\d+)', stats)
            if not match:
                raise ValueError(f'{path.name}: missing timing DMA counters')
            dma[key] = int(match[1])
        if dma['coeffDmaIssued'] != dma['coeffDmaCompleted'] or \
                dma['coeffDmaWriteBytes'] < 40*cells*sweeps:
            raise ValueError(f'{path.name}: incomplete actual timing DMA')
    audit = {pool: dict(lanes=RESOURCE_ROLES[pool][0],
                        callers=RESOURCE_ROLES[pool][1], **values)
             for pool, values in pools.items()}
    return dict(cycles=cycles, rhs=rhs_count, binary=binary, config=config,
                resources=audit,
                max_abs=max(float(r['max_abs']) for r in numeric),
                max_rel=max(float(r['max_rel']) for r in numeric))


def check(root, report=None):
    suite = json.loads((Path(__file__).resolve().parents[1] /
                        'suites/single-line-base-solve.json').read_text())
    results = {}
    for case in suite['cases']:
        name = case['name']
        config = options([*suite['common_args'], *case['args']])
        mode = config['--coeff-base-solve']
        results[name] = check_case(root / name, mode,
                                  int(config['--lusgs-cells']),
                                  int(config['--bench-iters']))
        if mode != 'direct':
            continue
        base = results[name.removesuffix('-direct') + '-inverse']
        direct = results[name]
        if base['binary'] != direct['binary']:
            raise ValueError(f'{name}: different gem5 binaries')
        left, right = dict(base['config']), dict(direct['config'])
        left.pop('--coeff-base-solve'); right.pop('--coeff-base-solve')
        if left != right or base['resources'].keys() != direct['resources'].keys():
            raise ValueError(f'{name}: changed non-algorithm settings')
        for pool in base['resources']:
            if any(base['resources'][pool][k] != direct['resources'][pool][k]
                   for k in ('lanes', 'count', 'lat', 'ii')):
                raise ValueError(f'{name}/{pool}: changed FU budget')
        print(f'{name.removesuffix("-direct")}: '
              f'cycles={base["cycles"]}/{direct["cycles"]} '
              f'change={100*(direct["cycles"]/base["cycles"]-1):+.2f}% '
              f'rhs={base["rhs"]}/{direct["rhs"]} '
              f'direct_max_abs={direct["max_abs"]:.3g}')
    if len({r['binary'] for r in results.values()}) != 1:
        raise ValueError('suite used different binaries')
    if report:
        report.write_text(json.dumps(dict(
            description='Simulator resource inventory; not synthesis area. '
                        'LU and TRSM scalar pools are separate. Split line '
                        'pools are separate from cold ColumnFma. Backward '
                        'final vector subtraction is functional.',
            fully_shared_cluster=False, cases=results), indent=2) + '\n')
    print('SINGLE_LINE_BASE_SOLVE_CHECK_PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('campaign', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    check(args.campaign, args.report)
