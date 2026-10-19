#!/usr/bin/env python3
"""Reject missing bank accesses, early readiness and leaked completion credit."""
import argparse
import csv
import io
from pathlib import Path
import re
import tempfile
from check_single_line_opt import analyze
from check_single_line_storage import check_case


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case',type=Path)
    args=parser.parse_args();source=args.case.resolve()
    analyze(source,check_case(source))
    text=(source/'simout.txt').read_text()
    faults={
        'missing_spm_access':('simout.txt',re.sub(r'^CFD_ONCHIP_ACCESS[^\n]*\n','',text,count=1,flags=re.M)),
        'premature_spm_ready':('simout.txt',re.sub(r'(CFD_ONCHIP_COMPLETE cycle=)\d+',r'\g<1>1',text,count=1)),
        'completion_credit_leak':('simout.txt',re.sub(r'(CFD_COMPLETION_QUEUE[^\n]* live=)0',r'\g<1>1',text)),
    }
    trace=(source/'coeff_preprocess_trace.csv').read_text()
    reader=csv.DictReader(io.StringIO(trace));rows=list(reader)
    one=next(r for r in rows if r['stage']=='CACHE_REGISTER_READY');one['cycle']='1'
    out=io.StringIO();writer=csv.DictWriter(out,fieldnames=reader.fieldnames);writer.writeheader();writer.writerows(rows)
    faults['cached_registers_before_load']=('coeff_preprocess_trace.csv',out.getvalue())
    for name,(filename,content) in faults.items():
        assert content!=(text if filename=='simout.txt' else trace)
        with tempfile.TemporaryDirectory(prefix='cfd-onchip-fault-') as tmp:
            path=Path(tmp)
            for p in source.iterdir():
                if p.name!=filename:(path/p.name).symlink_to(p)
            (path/filename).write_text(content)
            try:analyze(path,check_case(path))
            except (ValueError,AssertionError) as exc:print(name,'REJECTED',str(exc))
            else:raise ValueError('checker accepted '+name)
    print('ONCHIP_FAULT_CHECK_PASS (4 injected faults)')


if __name__=='__main__':main()
