#!/usr/bin/env python3
"""Validate dataset provenance and independent-oracle checks for every sweep."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path

from check_single_line_c import fields
from single_line_dataset import HEADER, fnv64, oracle, read_dataset, sha256


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
    for row in checks:
        if int(row['values']) != 10*len(records) or int(row['mismatches']) != 0 or any(
            not math.isfinite(float(row[k])) or float(row[k]) < 0 for k in ('max_abs','max_rel')):
            raise ValueError('independent dataset oracle comparison failed')
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
    return dict(input_sha256=digest, cells=len(records), sweeps=sweeps,
                oracle=f'{precision}-digit Decimal, partial pivoting; original binary64 inputs',
                max_abs=max(float(row['max_abs']) for row in checks),
                max_rel=max(float(row['max_rel']) for row in checks),
                fnv64=checksum)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    print(json.dumps(check_dataset(parser.parse_args().directory), indent=2))
