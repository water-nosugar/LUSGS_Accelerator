"""Accept actual simulated failures/cancellations only after a clean retry."""
import json
import re
from pathlib import Path
from check_single_line_c import fields, check_numeric
from check_single_line_output_dma import options
from check_single_line_dma_packets import check_dma_packets


def check_recovery(path):
    text=(path/'simout.txt').read_text()
    config=options(json.loads((path/'manifest.json').read_text())['command'])
    rows=fields(text,'CFD_RECOVERY_CONTRACT')
    count=6 if int(config['--lusgs-cells'])>=17 else 4
    if 'LUSGS_RECOVERY_CONTRACT_PASS' not in text or len(rows)!=count:
        raise ValueError('recovery contract did not finish')
    check_numeric(text,[int(config['--lusgs-cells'])]*(count//2))
    for step,row in enumerate(rows):
        wanted=int(step not in (0,2,4))
        if int(row['step'])!=step or int(row['actual'])!=wanted or int(row['expected'])!=wanted:
            raise ValueError('failed/cancelled invocation did not recover')
    aborts=fields(text,'CFD_LINE_ABORT')
    begins={r['token']:r for r in aborts if r['event']=='begin'}
    visible={r['token']:r for r in aborts if r['event']=='visible'}
    if len(begins)!=count//2 or begins.keys()!=visible.keys():
        raise ValueError('missing error/cancellation completion fence')
    for token,row in visible.items():
        if any(int(row[k]) for k in ('dma_live','records','children')):
            raise ValueError('error became visible before pending operations drained')
        if int(row['cycle'])<int(begins[token]['cycle']):
            raise ValueError('reversed abort lifetime')
        if row['status']!=begins[token]['status']:
            raise ValueError('a later cancellation overwrote the first error')
    schedules=fields(text,'CFD_LINE_SCHEDULE')
    if len(schedules)!=count//2 or any(int(r['cache_hit']) for r in schedules):
        raise ValueError('retry reused coefficients from a failed invocation')
    if fields(text,'CFD_DMA_PACKET'):
        check_dma_packets(text,path,config)
    else:
        if int(config.get('--coeff-line-opt-level','0'))>=11:
            raise ValueError('physical DMA trace missing')
        stats=(path/'stats.txt').read_text()
        issued=int(re.search(r'\.coeffDmaIssued\s+(\d+)',stats)[1])
        completed=int(re.search(r'\.coeffDmaCompleted\s+(\d+)',stats)[1])
        if not issued or issued!=completed:
            raise ValueError('full-storage recovery left DMA outstanding')
    for prefix,keys in [('CFD_RHS_ROWS',('live',)),('CFD_COMPLETION_QUEUE',('live',)),
                        ('CFD_ONCHIP_SUMMARY',('live',)),('CFD_CLUSTER_BUFFER',('live',))]:
        rows=fields(text,prefix)
        required={'CFD_RHS_ROWS':config.get('--coeff-rhs-row-parallel','0')=='1',
            'CFD_COMPLETION_QUEUE':int(config.get('--coeff-line-opt-level','0'))>=5,
            'CFD_ONCHIP_SUMMARY':int(config.get('--coeff-line-opt-level','0'))>=8,
            'CFD_CLUSTER_BUFFER':config.get('--coeff-compute-model')=='shared'}
        if not rows:
            if required[prefix]:raise ValueError('missing resource closure: '+prefix)
            continue
        row=rows[-1]
        if any(int(row[k]) for k in keys):
            raise ValueError('recovery leaked resources: '+prefix)
    return dict(cases=rows,aborts=list(visible.values()),result='passed')
