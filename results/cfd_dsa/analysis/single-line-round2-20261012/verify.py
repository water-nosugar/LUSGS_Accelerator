#!/usr/bin/env python3
"""Read-only verification of final source, build and immutable campaign evidence."""
import hashlib
import json
import math
import re
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]


def sha256(path):
    h=hashlib.sha256()
    with path.open('rb') as src:
        for block in iter(lambda:src.read(8*1024*1024),b''):
            h.update(block)
    return h.hexdigest()


def fields(text,prefix):
    return [dict(re.findall(r'(\w+)=([^ ]+)',line))
            for line in text.splitlines() if line.startswith(prefix+' ')]


def main():
    evidence=json.loads((OUT/'evidence.json').read_text())
    for name,digest in evidence['files'].items():
        assert sha256(REPO/name)==digest,('changed source/evidence',name)
    assert sha256(REPO/'build/ARM/gem5.opt')==evidence['gem5_sha256']
    assert sha256(REPO/'build/cfd_dsa/aarch64/test_cfd_lusgs_patha_step2_arm')==evidence['guest_sha256']
    cases=sweeps=final_cases=final_sweeps=0
    max_abs=0.0
    for campaign,entry in evidence['campaigns'].items():
        suite=json.loads((REPO/entry['suite']).read_text())
        root=REPO/'results/cfd_dsa/campaigns'/campaign
        report=json.loads((root/entry['report']).read_text())
        if entry['report_kind']=='c':report=report['cases']
        assert set(report)=={c['name'] for c in suite['cases']}
        campaign_sweeps=0
        for case in suite['cases']:
            path=root/case['name']
            manifest=json.loads((path/'manifest.json').read_text())
            assert manifest['status']=='passed' and manifest['return_code']==0
            assert manifest['gem5_sha256']==entry['gem5_sha256']
            assert report[case['name']]['binary']==entry['gem5_sha256']
            if entry['final_binary']:assert manifest['gem5_sha256']==evidence['gem5_sha256']
            text=(path/'simout.txt').read_text()
            assert '_FAIL' not in text
            assert 'LUSGS_STEP2_STREAMING_PASS' in text or 'LUSGS_CACHE_CONTRACT_PASS' in text
            numeric=fields(text,'CFD_SINGLE_LINE_NUMERIC')
            schedules=fields(text,'CFD_LINE_SCHEDULE')
            assert len(numeric)==len(schedules)>0
            for row in numeric:
                error=float(row['max_abs'])
                assert int(row['mismatches'])==0 and math.isfinite(error) and error>=0
                max_abs=max(max_abs,error)
            for storage in fields(text,'CFD_LINE_STORAGE'):
                assert int(storage['reserved'])+int(storage['resident'])+int(storage['cache_bytes'])<=int(storage['capacity'])
                assert storage['write_issued']==storage['write_completed']
                assert storage['read_issued']==storage['read_completed']
                assert storage['packets_issued']==storage['packets_completed']
                assert storage['host_line_arrays']=='0'
            for life in fields(text,'CFD_LINE_LIFETIME'):
                if life.get('phase')=='complete':
                    assert life['records_pending']=='0'
                    assert int(life['warm_output_peak'])<=int(life['warm_output_depth'])
            campaign_sweeps+=len(schedules)
            cases+=1
            if entry['final_binary']:final_cases+=1;final_sweeps+=len(schedules)
        assert len(suite['cases'])==entry['cases'] and campaign_sweeps==entry['sweeps']
        sweeps+=campaign_sweeps
    assert (cases,sweeps)==(evidence['cases'],evidence['sweeps'])
    assert (final_cases,final_sweeps)==(evidence['final_cases'],evidence['final_sweeps'])
    assert max_abs==evidence['max_abs']
    for name,marker in evidence['required_logs'].items():
        assert marker in (OUT/'logs'/name).read_text(),('missing successful verification',name)
    selection=json.loads((REPO/'projects/cfd_dsa/suites/single-line-round2-selection.json').read_text())
    recommended=json.loads((REPO/'projects/cfd_dsa/suites/single-line-round2-recommended.json').read_text())
    assert recommended['common_args']==selection['common_args']
    assert len(recommended['cases'])==4 and all(c in selection['cases'] for c in recommended['cases'])
    summary=json.loads((OUT/'summary.json').read_text())
    measured=json.loads((REPO/'results/cfd_dsa/campaigns/single-line-round2-selection-20261012/optimization.json').read_text())
    assert summary['binary']==evidence['gem5_sha256']
    for profile,c in summary['comparisons'].items():
        best=measured[c['case']];baseline=measured[profile+'-banked-l5']
        assert best['hashes']==baseline['hashes']
        assert c['optimized']['cycles']==best['cycles']
        assert c['baseline']['cycles']==baseline['cycles']
        assert best['config']['--coeff-cluster-div-ii']=='4'
        assert best['config']['--coeff-cluster-add-lat']=='4'
        assert c['case'] in {case['name'] for case in recommended['cases']}
        phase=c['details']['backward']
        assert phase['total']==phase['math_activity']+phase['tile_dma_wait']+phase['other_idle']
    print(f'ROUND2_EVIDENCE_PASS {cases} simulations / {sweeps} single-line sweeps; '
          f'final binary {final_cases}/{final_sweeps}; max_abs={max_abs:.3g}')


if __name__=='__main__':main()
