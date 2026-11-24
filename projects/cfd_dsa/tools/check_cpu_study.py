"""Independent identity, oracle and ROI checks for CPU study experiments."""
import json
import math
from pathlib import Path
from check_single_line_c import fields
from single_line_dataset import read_dataset, oracle, sha256


def check_cpu_study(directory):
    directory = Path(directory)
    resolved = json.loads((directory/'resolved_experiment.json').read_text())
    manifest = json.loads((directory/'manifest.json').read_text())
    command = resolved['process_cmd'][1:]
    args = dict(zip(command[::2], command[1::2]))
    path = Path(args['--coeff-input-file'])
    data = read_dataset(path)
    if sha256(path) != manifest['input_files'].get(str(path)):
        raise ValueError('CPU input identity differs')
    sidecar = path.with_suffix('.json')
    precision = 200
    if sidecar.exists():
        if sha256(sidecar) != manifest['input_files'].get(str(sidecar)):
            raise ValueError('CPU metadata identity differs')
        precision = json.loads(sidecar.read_text())['oracle_precision']
    if oracle([row[:80] for row in data], precision) != [row[80:] for row in data]:
        raise ValueError('CPU independent oracle differs')
    text = (directory/'simout.txt').read_text()
    rounds = fields(text, 'CPU_STUDY_ROUND')
    checks = fields(text, 'CFD_DATASET_NUMERIC')
    conditions = fields(text, 'CPU_STUDY_CONDITION')
    count = int(args['--bench-iters'])
    if len(rounds) != count or len(checks) != count or len(conditions) != count:
        raise ValueError('CPU study did not complete all rounds/checks')
    for index, (row, check, condition) in enumerate(zip(rounds, checks, conditions)):
        if (int(row['round']) != index or int(row['numeric_ok']) != 1 or
                row['mode'] != args['--cpu-study-mode'] or
                row['cache'] != args['--cpu-study-cache'] or
                row['timing'] != 'gem5-2GHz-cycles' or
                int(row['cycles']) <= 0 or
                int(row['cycles']) != int(row['preprocess_cycles'])+int(row['runtime_cycles']) or
                int(check['mismatches']) or int(check['values']) != len(data)*10 or
                check['phase'] != f'cpu_{row["mode"]}_{index}' or
                condition['cache'] != row['cache'] or int(condition['round']) != index or
                not math.isfinite(float(row['checksum']))):
            raise ValueError('CPU numeric or timing contract failed')
    return dict(cells=len(data), rounds=rounds, conditions=conditions,
                input_sha256=sha256(path), oracle_precision=precision,
                timing_scope='preallocated compute + cold preprocessing; excludes allocation, conditioning and validation')
