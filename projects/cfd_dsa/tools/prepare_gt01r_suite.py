#!/usr/bin/env python3
"""Prepare the measured GT01R paths and same-format synthetic control."""
import argparse
import json
from pathlib import Path

from extract_gt01r import write_json
from single_line_dataset import unpivoted_diagnostics, write_dataset

REPO = Path(__file__).resolve().parents[3]


def baseline_records(cells):
    """Original guest init_problem raw coefficients, with masked endpoints."""
    def signed(line, cell, r, c, base):
        code = (line+1)*17+(cell+3)*13+(r+5)*7+(c+11)*3
        return (-1.0 if code & 1 else 1.0)*base*(1+code % 5)
    raw = []
    for cell in range(cells):
        d = [3.20+0.11*(r+1)+0.009+0.004*(cell+1) if r == c
             else signed(3, cell+2, r, c, 0.003)
             for r in range(5) for c in range(5)]
        lower = [signed(5, cell+1, r, c, 0.010) for r in range(5) for c in range(5)] if cell else [0.0]*25
        upper = [signed(2, cell+7, r, c, 0.008) for r in range(5) for c in range(5)] if cell+1 < cells else [0.0]*25
        rhs = [(-1.0 if (19+(cell+1)*11+(lane+1)*5) & 1 else 1.0)*
               (0.40+0.031+0.019*(cell+1)+0.013*(lane+1)) for lane in range(5)]
        raw.append(d+lower+upper+rhs)
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--campaign', default='single-line-gt01r-final-20261022')
    parser.add_argument('--max-ticks', type=int, default=100000000000)
    args = parser.parse_args()
    if args.max_ticks <= 0:
        parser.error('--max-ticks must be positive')
    directory = args.dataset_dir.resolve()
    data = json.loads((directory/'manifest.json').read_text())
    reference = json.loads((REPO/'projects/cfd_dsa/suites/history/single-line-supply-recommended.json').read_text())['cases'][0]['args']
    common = [a for a in reference if not a.startswith((
        '--lusgs-cells=', '--coeff-preprocess-trace-cells=', '--max-ticks='))]
    common.append(f'--max-ticks={args.max_ticks}')
    cases = []
    for row in data['cases']:
        cases.append(dict(name=row['name'], target='lusgs-single-line', args=[
            f'--lusgs-cells={row["cells"]}', f'--coeff-preprocess-trace-cells={row["cells"]}',
            f'--coeff-input-file={directory/(row["name"]+".cfd5")}']))
    raw = baseline_records(65)
    name = 'synth-s75-n65'
    record = write_dataset(directory/(name+'.cfd5'), raw, precision=200)
    record.update(name=name, source='guest init_problem baseline; synthetic control',
                  derivation='original baseline values; masked endpoints; not GT01R',
                  **unpivoted_diagnostics(raw))
    write_json(directory/(name+'.json'), record)
    assert len(name) == len('gt01r-s75-n65')
    cases.append(dict(name=name, target='lusgs-single-line', args=[
        '--lusgs-cells=65','--coeff-preprocess-trace-cells=65',
        f'--coeff-input-file={directory/(name+".cfd5")}']))
    cases.append(dict(name='legacy-baseline-n65',target='lusgs-single-line',args=[
        '--lusgs-cells=65','--coeff-preprocess-trace-cells=65']))
    for mode in ('inverse','divide'):
        extra = ['--coeff-base-solve=inverse'] if mode == 'inverse' else [
            '--coeff3-div-mode=divide','--coeff-warm-diagonal=divide']
        cases.append(dict(name=f'gt01r-s75-n17-{mode}',target='lusgs-single-line',args=[
            '--lusgs-cells=17','--coeff-preprocess-trace-cells=17',
            f'--coeff-input-file={directory/"gt01r-s75-n17.cfd5"}',*extra]))
    suite = dict(campaign=args.campaign, notes=[
        'GT01R block-path projection, not the full sparse problem.',
        'All coefficients/RHS preserved; no diagonal shifts or scaling.',
        'Seven identical-input calls; actual coefficient reuse depends on finite cache capacity.',
        '200-digit pivoted Decimal oracle; raw recurrence and historical guest tolerances retained.',
        'Original synthetic baseline uses identical input format and filename length.'],
        common_args=common,cases=cases)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    write_json(args.output,suite)
    print(f'Prepared {len(cases)} simulations; {len(data["rejected"])} extraction rejections retained')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
