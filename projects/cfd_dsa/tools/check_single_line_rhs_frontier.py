#!/usr/bin/env python3
"""Check one-line RHS arbitration against identical-resource controls."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import re

from check_context_ablation import read_case


CASES = ('cold-17-t2', 'cold-17-t4', 'mixed-17-t2', 'cold-64-t2')


def parse_trace(path, cells, sweeps):
    with (path / 'coeff_preprocess_trace.csv').open(newline='') as source:
        allocations = [int(row['cell']) for row in csv.DictReader(source)
                       if row['stage'] == 'REQUEST_ALLOC']
    # The middle sweep is a cache hit, so no child preprocessor is launched.
    expected = 2 if sweeps == 3 else 1
    if Counter(allocations) != Counter({cell: expected
                                         for cell in range(cells)}):
        raise ValueError(f'{path.name}: missing or repeated child cell')


def metrics(path):
    text = (path / 'simout.txt').read_text()
    rows = [dict(re.findall(r'(\w+)=(\w+)', line)) for line in text.splitlines()
            if line.startswith('CFD_LINE_SCHEDULE ')]
    stats = {}
    for key in ('eventRhsMaxWaitCycles', 'frontierWaitLbarColumnsCycles'):
        match = re.search(r'\.' + key + r' = (\d+)', text)
        if not match:
            raise ValueError(f'{path.name}: missing {key}')
        stats[key] = int(match[1])
    diagnostics = {name: int(value) for name, value in
                   re.findall(r'CFD_SCHED_DIAG reason=(\w+) count=(\d+)', text)}
    return rows, stats, diagnostics


def check(root):
    for name in CASES:
        base_path, rhs_path = root / (name + '-base'), root / (name + '-rhs')
        base, rhs = read_case(base_path), read_case(rhs_path)
        expected_cells = 64 if name.startswith('cold-64') else 17
        expected_sweeps = 3 if name.startswith('mixed') else 1
        if base[1] != rhs[1] or len(base[1]) != expected_sweeps:
            raise ValueError(f'{name}: full-sweep FP64 digest changed')
        if base[4] != rhs[4]:
            raise ValueError(f'{name}: different gem5 binary')
        for path, schedule in ((base_path, 'frontier-aware'),
                               (rhs_path, 'rhs-frontier')):
            manifest = json.loads((path / 'manifest.json').read_text())
            command = manifest['command']
            if not any(str(arg).endswith('run_cfd_lusgs_single_line.py')
                       for arg in command):
                raise ValueError(f'{path.name}: scope runner missing')
            parse_trace(path, expected_cells, expected_sweeps)
            options = base[3] if path == base_path else rhs[3]
            if options.get('coeff3-schedule') != schedule:
                raise ValueError(f'{path.name}: wrong schedule')
            if options.get('lusgs-lines') != '1' or options.get('lusgs-interleave') != '1':
                raise ValueError(f'{path.name}: not one line')
        base_options = dict(base[3]); rhs_options = dict(rhs[3])
        base_options.pop('coeff3-schedule'); rhs_options.pop('coeff3-schedule')
        if base_options != rhs_options:
            raise ValueError(f'{name}: non-scheduler settings changed')
        if base[2].keys() != rhs[2].keys():
            raise ValueError(f'{name}: resource pool set changed')
        for pool in base[2]:
            for field in ('count', 'lat', 'ii', 'issued'):
                if base[2][pool][field] != rhs[2][pool][field]:
                    raise ValueError(f'{name}/{pool}: {field} changed')
        base_rows, base_stats, _ = metrics(base_path)
        rhs_rows, rhs_stats, diagnostics = metrics(rhs_path)
        if len(base_rows) != expected_sweeps or len(rhs_rows) != expected_sweeps:
            raise ValueError(f'{name}: missing line completion')
        for rows in (base_rows, rhs_rows):
            if sum(int(r['cache_hit']) for r in rows) != (1 if expected_sweeps == 3 else 0):
                raise ValueError(f'{name}: cache lifecycle changed')
            for row in rows:
                if not (int(row['backward_start']) <= int(row['backward_done'])
                        <= int(row['visible_done'])):
                    raise ValueError(f'{name}: invalid completion order')
        if diagnostics.get('rhs_frontier_exact_issue', 0) == 0 or \
                diagnostics.get('rhs_frontier_background_issue', 0) == 0:
            raise ValueError(f'{name}: frontier/background priority not exercised')
        print(f'{name}: base={base[0]} rhs={rhs[0]} '
              f'change={100*(rhs[0]/base[0]-1):+.2f}% '
              f'exact={diagnostics["rhs_frontier_exact_issue"]} '
              f'background={diagnostics["rhs_frontier_background_issue"]} '
              f'wait={base_stats["eventRhsMaxWaitCycles"]}/'
              f'{rhs_stats["eventRhsMaxWaitCycles"]} '
              f'lbar_wait={base_stats["frontierWaitLbarColumnsCycles"]}/'
              f'{rhs_stats["frontierWaitLbarColumnsCycles"]}')
    print('SINGLE_LINE_RHS_FRONTIER_CHECK_PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('campaign', type=Path)
    check(parser.parse_args().campaign)
