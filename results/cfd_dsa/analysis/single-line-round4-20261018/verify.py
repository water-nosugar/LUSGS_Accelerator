#!/usr/bin/env python3
"""Verify frozen round4 evidence; live source checks are on by default."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'projects/cfd_dsa/tools'))
from run_experiment import source_fingerprint


def sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive-only',action='store_true',
        help='verify archived evidence without asserting it matches the live build')
    args=parser.parse_args()
    evidence=json.loads((HERE/'evidence.json').read_text())
    expected=json.loads((HERE/'sources-final.json').read_text())
    runtime=lambda files:{name:digest for name,digest in files.items()
                         if not name.startswith('projects/cfd_dsa/tools/')}
    for name,digest in evidence['files'].items():
        assert sha256(ROOT/name)==digest, ('evidence changed',name)
    total=0
    for entry in evidence['campaigns']:
        campaign=ROOT/'results/cfd_dsa/campaigns'/entry['name']
        suite=json.loads((ROOT/entry['suite']).read_text())
        assert len(suite['cases'])==entry['cases']
        for case in suite['cases']:
            manifest=json.loads((campaign/case['name']/'manifest.json').read_text())
            assert manifest['status']=='passed', (entry['name'],case['name'])
            assert not manifest['provenance_errors']
            if entry['automatic_acceptance']:
                assert manifest['acceptance_status']=='passed'
            assert manifest['gem5_sha256']==evidence['gem5_sha256']
            assert manifest['guest_sha256']==evidence['guest_sha256']
            assert manifest['sources_sha256']==entry['sources_sha256']
            sources=json.loads((campaign/case['name']/'sources.json').read_text())
            assert runtime(sources)==runtime(expected), 'simulated model/config/guest source changed'
            differences={k for k in sources.keys()|expected.keys() if sources.get(k)!=expected.get(k)}
            assert differences<={'projects/cfd_dsa/tools/check_context_ablation.py',
                                 'projects/cfd_dsa/tools/check_single_line_c.py'}
            total+=1
    if not args.archive_only:
        assert source_fingerprint()==expected, 'live CFD source contents changed'
        assert sha256(ROOT/'build/ARM/gem5.opt')==evidence['gem5_sha256']
        assert sha256(ROOT/'build/cfd_dsa/aarch64/test_cfd_lusgs_patha_step2_arm')==evidence['guest_sha256']
    print('ROUND4_ARCHIVE_PASS' if args.archive_only else 'ROUND4_EVIDENCE_PASS',
          len(evidence['files']),'files',total,'final simulation cases')


if __name__=='__main__':main()
