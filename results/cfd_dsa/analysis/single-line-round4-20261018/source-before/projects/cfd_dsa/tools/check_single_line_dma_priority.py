#!/usr/bin/env python3
"""Prove that output priority removes backpressure under the same DMA credits."""
import argparse
import json
from pathlib import Path
from check_single_line_c import fields


def check(root):
    data=json.loads((root/'optimization.json').read_text())
    results={}
    for mode in ('direct','inverse'):
        before=data[f'n65-cold-{mode}-priority0']
        after=data[f'n65-cold-{mode}-l11']
        a=dict(before['config']);b=dict(after['config'])
        a.pop('--coeff-dma-output-priority',None);b.pop('--coeff-dma-output-priority',None)
        assert a==b, 'priority comparison changes hardware resources'
        assert before['binary']==after['binary'] and before['hashes']==after['hashes']
        assert before['windows'][0]['issued']==after['windows'][0]['issued']
        old=fields((root/f'n65-cold-{mode}-priority0'/'simout.txt').read_text(),'CFD_LINE_SCHEDULE')[0]
        new=fields((root/f'n65-cold-{mode}-l11'/'simout.txt').read_text(),'CFD_LINE_SCHEDULE')[0]
        assert int(new['output_queue_stalls'])<int(old['output_queue_stalls'])
        assert after['windows'][0]['backward_cycles']<before['windows'][0]['backward_cycles']
        assert after['windows'][0]['cycles']<before['windows'][0]['cycles']
        results[mode]={'before':old,'after':new}
        print(mode,'output_wait',old['output_queue_stalls'],'->',new['output_queue_stalls'])
    (root/'dma-priority.json').write_text(json.dumps(results,indent=2)+'\n')
    print('DMA_PRIORITY_CHECK_PASS (same resources, same FP64 work/results)')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('campaign',type=Path)
    check(parser.parse_args().campaign)
