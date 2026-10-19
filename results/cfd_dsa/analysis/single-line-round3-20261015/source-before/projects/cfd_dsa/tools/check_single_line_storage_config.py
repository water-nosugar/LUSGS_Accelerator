#!/usr/bin/env python3
"""Verify finite-storage configurations fail before simulated execution."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest',type=Path);parser.add_argument('--report',type=Path)
    args=parser.parse_args();command=json.loads(args.manifest.read_text())['command']
    index=next(i for i,s in enumerate(command) if s.endswith('run_cfd_lusgs_single_line.py'))
    tests={
        'insufficient_capacity':(['--coeff-line-spm-bytes=98304'],'finite SPM needs'),
        'invalid_buffer_count':(['--coeff-line-tile-buffers=3'],'tile buffers must'),
        'invalid_tile':(['--coeff-line-tile-cells=0'],'finite tile requires'),
        'invalid_dma_width':(['--coeff-line-tile-dma-width=0'],'tile buffers must'),
        'interface_requires_tiles':(['--coeff-line-storage=full'],'timed interface requires'),
        'fixture_requires_cache':(['--coeff-cache-contract-test=1','--coeff-line-cache-cells=0'],'finite cache contract requires'),
        'single_line_only':(['--lusgs-lines=2'],'single-line study requires'),
    }
    config = dict(arg.split('=', 1) for arg in command if arg.startswith('--') and '=' in arg)
    if int(config.get('--coeff-line-opt-level', '0')):
        tests['interface_requires_tiles'] = (['--coeff-line-storage=full'],
                                             'optimization requires tiled timing DMA')
        tests['optimized_timing_required'] = (['--coeff-interface-timing=0'],
                                               'optimization requires tiled timing DMA')
        tests['invalid_optimization_level'] = (['--coeff-line-opt-level=9'],
                                               '--coeff-line-opt-level must be 0..8')
    if int(config.get('--coeff-line-opt-level','0'))>=8:
        tests.update({'onchip_zero_ports':(['--coeff-spm-read-ports=0'],'onchip banks require'),
                      'onchip_bad_grain':(['--coeff-spm-bank-granularity=7'],'onchip banks require'),
                      'onchip_zero_queue':(['--coeff-onchip-queue-depth=0'],'onchip timing requires')})
    results={}
    for name,(extra,expected) in tests.items():
        with tempfile.TemporaryDirectory(prefix='cfd-invalid-') as tmp:
            run=subprocess.run([command[0],'--outdir='+tmp,*command[index:],*extra],
                text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=False)
            logs=run.stdout+'\n'+''.join(p.read_text() for p in Path(tmp).glob('*.txt'))
            if run.returncode==0 or expected not in logs or 'CFD_LINE_SCHEDULE ' in logs:
                raise ValueError(name+' was not rejected as intended: '+logs[-1500:])
            results[name]={'returncode':run.returncode,'expected_error':expected}
            print(name,'REJECTED')
    if args.report:args.report.write_text(json.dumps(results,indent=2)+'\n')
    print(f'STORAGE_CONFIG_CHECK_PASS ({len(tests)} invalid configurations)')


if __name__=='__main__':main()
