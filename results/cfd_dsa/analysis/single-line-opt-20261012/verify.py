#!/usr/bin/env python3
"""Verify the frozen optimization evidence without modifying experiments."""
import hashlib
import json
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]


def sha256(path):
    h=hashlib.sha256()
    with path.open('rb') as src:
        for block in iter(lambda:src.read(8*1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    evidence=json.loads((OUT/'evidence.json').read_text())
    for name,digest in evidence['files'].items():
        assert sha256(REPO/name)==digest, ('changed evidence/source',name)
    cases=sweeps=0
    for campaign,entry in evidence['campaigns'].items():
        root=REPO/'results/cfd_dsa/campaigns'/campaign
        suite=json.loads((REPO/entry['suite']).read_text())
        for case in suite['cases']:
            path=root/case['name']
            manifest=json.loads((path/'manifest.json').read_text())
            assert manifest['status']=='passed' and manifest['return_code']==0
            assert manifest['gem5_sha256']==evidence['gem5_sha256']
            text=(path/'simout.txt').read_text()
            assert '_FAIL' not in text
            assert 'LUSGS_STEP2_STREAMING_PASS' in text or 'LUSGS_CACHE_CONTRACT_PASS' in text
            sweeps+=sum(line.startswith('CFD_LINE_SCHEDULE ') for line in text.splitlines())
            cases+=1
        assert len(suite['cases'])==entry['cases']
    for filename,marker in evidence['required_logs'].items():
        assert marker in (OUT/'logs'/filename).read_text(), ('missing verification',filename)
    assert cases==evidence['cases'] and sweeps==evidence['sweeps']
    selection=json.loads((REPO/'projects/cfd_dsa/suites/single-line-opt-selection.json').read_text())
    recommended=json.loads((REPO/'projects/cfd_dsa/suites/single-line-opt-recommended.json').read_text())
    assert recommended['common_args']==selection['common_args']
    assert len(recommended['cases'])==4 and all(c in selection['cases'] for c in recommended['cases'])
    print(f'OPTIMIZATION_EVIDENCE_PASS {cases} simulations / {sweeps} single-line sweeps')


if __name__=='__main__':
    main()
