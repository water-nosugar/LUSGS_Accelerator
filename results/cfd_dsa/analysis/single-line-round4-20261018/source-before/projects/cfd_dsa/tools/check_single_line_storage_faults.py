#!/usr/bin/env python3
"""Exercise acceptance checks with corrupted traces, without running gem5."""
import argparse
from pathlib import Path
import re
import tempfile
from check_single_line_storage import check_case


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('case',type=Path)
    args=parser.parse_args();source=args.case.resolve();check_case(source)
    text=(source/'simout.txt').read_text()
    faults={
        'capacity':re.sub(r'(CFD_LINE_STORAGE[^\n]* capacity=)\d+',r'\g<1>1',text,count=1),
        'missing_reload_response':re.sub(r'^CFD_TILE_DMA[^\n]* event=response[^\n]*\n','',text,count=1,flags=re.M),
        'missing_record_response':re.sub(r'^CFD_INTERFACE_DMA[^\n]* event=response kind=record[^\n]*\n','',text,count=1,flags=re.M),
        'premature_completion':re.sub(r'(CFD_LINE_SCHEDULE[^\n]* visible_done=)\d+',r'\g<1>1',text,count=1),
        'wrong_cache_residency':re.sub(r'(CFD_LINE_STORAGE[^\n]* cache_bytes=)\d+',r'\g<1>999999',text,count=1),
    }
    trace=(source/'coeff_preprocess_trace.csv').read_text()
    terminal=next(line for line in trace.splitlines() if ',REQUEST_COMPLETE,' in line)
    faults['duplicated_terminal_event']=text
    for label,bad in faults.items():
        if bad==text and label!='duplicated_terminal_event':
            raise ValueError('fault fixture requires a tiled timed case with reloads')
        with tempfile.TemporaryDirectory(prefix='cfd-storage-fault-') as tmp:
            path=Path(tmp)
            for f in source.iterdir():
                if f.name!='simout.txt' and not (label=='duplicated_terminal_event' and f.name=='coeff_preprocess_trace.csv'):
                    (path/f.name).symlink_to(f)
            (path/'simout.txt').write_text(bad)
            if label=='duplicated_terminal_event':
                (path/'coeff_preprocess_trace.csv').write_text(trace+terminal+'\n')
            try:check_case(path)
            except ValueError as exc:print(label,'REJECTED',str(exc))
            else:raise ValueError('acceptance checker accepted '+label)
    print('STORAGE_FAULT_CHECK_PASS (6 injected faults)')


if __name__=='__main__':main()
