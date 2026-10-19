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
    if 'CFD_BACK_OPERAND ' in text:
        faults.update({
            'premature_backward_use': ('simout.txt', re.sub(
                r'(CFD_BACK_OPERAND cycle=)\d+( [^\n]*event=use )',r'\g<1>1\2',text,count=1)),
            'backward_operand_leak': ('simout.txt', re.sub(
                r'^CFD_BACK_OPERAND [^\n]*event=release[^\n]*\n','',text,count=1,flags=re.M)),
        })
    if 'CFD_LOCAL_READY ' in text:
        faults['premature_local_ready'] = ('simout.txt', re.sub(
            r'(CFD_LOCAL_READY cycle=)\d+',r'\g<1>1',text,count=1))
    if 'CFD_RECORD_DIGEST_GUEST ' in text:
        faults['packed_record_field_changed'] = ('simout.txt', re.sub(
            r'(CFD_RECORD_DIGEST_GUEST [^\n]*hash=)[0-9a-f]+',r'\g<1>bad',text,count=1))
    if 'CFD_DMA_PACKET ' in text:
        faults['response_before_send'] = ('simout.txt', re.sub(
            r'(CFD_DMA_PACKET tick=)\d+( event=response )',r'\g<1>1\2',text,count=1))
    if 'CFD_RECORD_SUMMARY_GUEST ' in text:
        faults['summary_field_changed'] = ('simout.txt', re.sub(
            r'(CFD_RECORD_SUMMARY_GUEST [^\n]*hash=)[0-9a-f]+',r'\g<1>bad',text,count=1))
        faults['missing_summary_child'] = ('simout.txt', re.sub(
            r'^CFD_RECORD_DIGEST_SOURCE [^\n]*summary_owner=[^\n]*\n','',text,count=1,flags=re.M))
        faults['summary_before_child_retirement'] = ('simout.txt', re.sub(
            r'(CFD_INTERFACE_DMA cycle=)\d+( [^\n]*event=issue kind=line-summary )',
            r'\g<1>1\2',text,count=1))
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
    print(f'OPTIMIZATION_FAULT_CHECK_PASS ({len(faults)} injected faults)')


if __name__ == '__main__':
    main()
