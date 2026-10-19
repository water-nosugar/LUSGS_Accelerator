#!/usr/bin/env python3
"""Check that optimized ownership and ordering violations are rejected."""
import argparse
import csv
import io
from pathlib import Path
import re
import tempfile

from check_single_line_opt import analyze
from check_single_line_storage import check_case


def corrupt_trace(text, stage, field, value):
    src = csv.DictReader(io.StringIO(text))
    rows = list(src)
    row = next(r for r in rows if r['stage'] == stage)
    row[field] = str(value)
    out = io.StringIO()
    writer = csv.DictWriter(out, fieldnames=src.fieldnames)
    writer.writeheader()
    writer.writerows(rows)
    return out.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', type=Path)
    args = parser.parse_args()
    source = args.case.resolve()
    analyze(source, check_case(source))
    trace = (source/'coeff_preprocess_trace.csv').read_text()
    text = (source/'simout.txt').read_text()
    faults = {
        'unordered_column_sum': ('coeff_preprocess_trace.csv',
            corrupt_trace(trace, 'COLUMN_FMA_COMPLETE', 'rhsColumn', 4)),
        'premature_tile_commit': ('coeff_preprocess_trace.csv',
            corrupt_trace(trace, 'TILE_DATA_COMMIT', 'cycle', 1)),
        'frontier_admission_reordered': ('coeff_preprocess_trace.csv',
            corrupt_trace(trace, 'INPUT_START', 'cell', 999)),
        'row_product_leak': ('simout.txt', re.sub(
            r'(CFD_RHS_ROWS[^\n]* live=)0', r'\g<1>1', text)),
    }
    for name, (filename, content) in faults.items():
        assert content != (text if filename == 'simout.txt' else trace)
        with tempfile.TemporaryDirectory(prefix='cfd-opt-fault-') as tmp:
            path = Path(tmp)
            for original in source.iterdir():
                if original.name != filename:
                    (path/original.name).symlink_to(original)
            (path/filename).write_text(content)
            try:
                analyze(path, check_case(path))
            except (ValueError, AssertionError) as exc:
                print(name, 'REJECTED', str(exc))
            else:
                raise ValueError('acceptance checker accepted '+name)
    print('OPTIMIZATION_FAULT_CHECK_PASS (4 injected faults)')


if __name__ == '__main__':
    main()
