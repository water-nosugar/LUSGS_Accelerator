#!/usr/bin/env python3
"""Verify the accepted final artifacts have not changed since validation."""
import hashlib
import json
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]


def main():
    evidence=json.loads((OUT/'evidence.json').read_text())
    for name,digest in evidence['files'].items():
        p=REPO/name
        assert p.is_file() and hashlib.sha256(p.read_bytes()).hexdigest()==digest,name
    total=0
    for campaign in evidence['campaigns']:
        root=REPO/'results/cfd_dsa/campaigns'/campaign['campaign']
        report=json.loads((root/campaign['report']).read_text())
        entries=report.get('cases',report)
        assert set(entries)==set(campaign['cases']),campaign['campaign']
        for name in campaign['cases']:
            manifest=json.loads((root/name/'manifest.json').read_text())
            assert manifest['status']=='passed'
            assert manifest['gem5_sha256']==evidence['binaries']['build/ARM/gem5.opt']
        total+=len(campaign['cases'])
    assert total==evidence['final_simulation_cases']
    print('ROUND3_EVIDENCE_VERIFY_PASS',total,'final simulations,',len(evidence['files']),'unchanged files')


if __name__=='__main__':main()
