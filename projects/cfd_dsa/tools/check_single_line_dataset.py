#!/usr/bin/env python3
"""Validate dataset provenance and independent-oracle checks for every sweep."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path

from decimal import Decimal, localcontext
from check_single_line_c import fields
from single_line_dataset import HEADER, fnv64, oracle, read_dataset, sha256, solve_decimal


def max_block_cond_inf(records, precision=80):
    with localcontext() as ctx:
        ctx.prec = max(80, precision)
        kappa_max = Decimal(1)
        for rec in records:
            d = [Decimal.from_float(x) for x in rec[:25]]
            d_norm = max(sum(abs(d[r*5+c]) for c in range(5)) for r in range(5))
            inv_cols = [solve_decimal(d, [Decimal(int(r == col)) for r in range(5)])
                        for col in range(5)]
            inv_norm = max(sum(abs(inv_cols[col][r]) for col in range(5))
                           for r in range(5))
            kappa_max = max(kappa_max, d_norm * inv_norm)
        return float(kappa_max)


def check_dataset(directory):
    directory = Path(directory)
    manifest = json.loads((directory/'manifest.json').read_text())
    resolved = json.loads((directory/'resolved_experiment.json').read_text())
    options = resolved['options']
    path = Path(options['coeff_input_file'])
    records = read_dataset(path)
    digest = sha256(path)
    if manifest['input_files'].get(str(path)) != digest or resolved['input_files'].get(str(path)) != digest:
        raise ValueError('dataset provenance mismatch')
    sidecar = path.with_suffix('.json')
    precision = 80
    if sidecar.exists():
        if manifest['input_files'].get(str(sidecar)) != sha256(sidecar):
            raise ValueError('dataset metadata provenance mismatch')
        metadata = json.loads(sidecar.read_text())
        precision = int(metadata['oracle_precision'])
        if not 80 <= precision <= 1000:
            raise ValueError('invalid independent oracle precision')
        if metadata['sha256'] != digest or metadata['cells'] != len(records):
            raise ValueError('dataset metadata does not describe the loaded bytes')
    expected = oracle([row[:80] for row in records], precision)
    if expected != [row[80:] for row in records]:
        raise ValueError('embedded oracle differs from fresh Decimal calculation')
    text = (directory/'simout.txt').read_text()
    inputs = fields(text, 'CFD_DATASET_INPUT')
    checksum = f"{fnv64(path.read_bytes()[HEADER.size:]):016x}"
    if len(inputs) != 1 or inputs[0]['fnv64'] != checksum or int(inputs[0]['cells']) != len(records):
        raise ValueError('guest did not load the expected dataset')
    checks = fields(text, 'CFD_DATASET_NUMERIC')
    # bench_iters is not part of the historical resolved-options namespace.
    schedules = fields(text, 'CFD_LINE_SCHEDULE')
    sweeps = len(schedules)
    if not sweeps or Counter(row['phase'] for row in checks) != {'cpu':1,'accelerator':sweeps}:
        raise ValueError('missing CPU or per-sweep independent oracle comparison')
    kappa_max = max_block_cond_inf(records, precision)
    expected_tol = max(1e-10, 64.0 * (2.0 ** -53) * kappa_max)
    res_limit = 1e-12 if expected_tol > 1e-10 else 1e-10
    for row in checks:
        if int(row['values']) != 10*len(records) or int(row['mismatches']) != 0 or any(
            not math.isfinite(float(row[k])) or float(row[k]) < 0 for k in ('max_abs','max_rel')):
            raise ValueError('independent dataset oracle comparison failed')
        if 'tol' in row:
            reported_tol = float(row['tol'])
            if not math.isfinite(reported_tol) or reported_tol > expected_tol * 1.001:
                raise ValueError('guest dataset forward tolerance exceeds condition bound')
        if 'residual_max' in row:
            reported_res = float(row['residual_max'])
            if not math.isfinite(reported_res) or not 0.0 <= reported_res <= res_limit:
                raise ValueError('dataset normalized recurrence residual failed')
        elif expected_tol > 1e-10:
            raise ValueError('condition-scaled dataset check requires residual_max evidence')
    conditioning = options.get('coeff_study_cache', 'legacy')
    if conditioning != 'legacy':
        rois = fields(text, 'CFD_STUDY_ACCEL_ROI')
        conditions = fields(text, 'CFD_STUDY_ACCEL_CONDITION')
        references = fields(text, 'CFD_STUDY_ACCEL_REFERENCE')
        if len(rois) != sweeps or len(conditions) != sweeps or len(references) != 1:
            raise ValueError('missing study conditioning/ROI/deferred reference')
        if references[0].get('valid') != '1' or text.rfind('CFD_STUDY_ACCEL_REFERENCE ') < text.rfind('CFD_STUDY_ACCEL_ROI '):
            raise ValueError('CPU reference did not run after all performance ROIs')
        for index, (roi, condition) in enumerate(zip(rois, conditions), 1):
            if (int(roi['round']) != index or int(condition['round']) != index or
                    condition['cache'] != conditioning or int(roi['cycles']) <= 0 or
                    int(roi['finish'])-int(roi['start']) != int(roi['cycles'])):
                raise ValueError('invalid accelerator study timing/conditioning')
    strict_mis = {row['phase']: int(row['strict_1e10_exceeded'])
                  for row in checks if 'strict_1e10_exceeded' in row}
    return dict(input_sha256=digest, cells=len(records), sweeps=sweeps,
                oracle=f'{precision}-digit Decimal, partial pivoting; original binary64 inputs',
                kappa_max=kappa_max, forward_tol=expected_tol,
                strict_1e10_mismatches=strict_mis,
                max_abs=max(float(row['max_abs']) for row in checks),
                max_rel=max(float(row['max_rel']) for row in checks),
                fnv64=checksum)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    print(json.dumps(check_dataset(parser.parse_args().directory), indent=2))
