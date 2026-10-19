#!/usr/bin/env python3
"""Capture final accepted artifacts and their exact source/binary provenance."""
import hashlib
import json
import shutil
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
ROOT=REPO/'results/cfd_dsa/campaigns'
SPECS=[('single-line-round3-final-20261015','single-line-round3-final','optimization.json'),
       ('single-line-round3-final-tile-20261015','single-line-round3-final-tile','optimization.json'),
       ('single-line-round3-final-stress-20261015','single-line-round3-final-stress','optimization.json'),
       ('single-line-round3-final-arithmetic-20261015','single-line-round3-final-arithmetic','optimization.json'),
       ('single-line-round3-final-endpoint-20261015','single-line-round3-final-endpoint','optimization.json'),
       ('single-line-round3-final-regression-c-20261015','single-line-c-complete','acceptance.json'),
       ('single-line-round3-final-regression-d-20261015','single-line-d-optimization','acceptance.json')]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    model=json.loads((OUT/'source-final-model.json').read_text())
    binaries=json.loads((OUT/'binary-final.json').read_text())
    for name,digest in {**model,**binaries}.items():assert sha(REPO/name)==digest,name
    files={};campaigns=[];total=0
    def add(path):
        path=path.resolve();files[str(path.relative_to(REPO))]=sha(path)
    for n in list(model)+list(binaries):add(REPO/n)
    for campaign,suite_name,report in SPECS:
        root=ROOT/campaign;suite=REPO/'projects/cfd_dsa/suites'/f'{suite_name}.json'
        spec=json.loads(suite.read_text());add(suite);add(root/report)
        names=[]
        for case in spec['cases']:
            path=root/case['name'];manifest=json.loads((path/'manifest.json').read_text())
            assert manifest['status']=='passed' and manifest['gem5_sha256']==binaries['build/ARM/gem5.opt']
            for filename in ('manifest.json','simout.txt','driver.log','stats.txt','config.json','config.ini',
                             'coeff_preprocess_trace.csv','coeff_preprocess_trace.csv.cluster.csv'):
                if (path/filename).exists():add(path/filename)
            names.append(case['name'])
        campaigns.append(dict(campaign=campaign,suite=str(suite.relative_to(REPO)),report=report,cases=names))
        total+=len(names)
    for pattern in ('projects/cfd_dsa/tools/check_single_line*.py','projects/cfd_dsa/configs/lusgs/*profile.py',
                    'projects/cfd_dsa/benchmarks/lusgs/step2*.inc','projects/cfd_dsa/suites/single-line-round3*.json'):
        for p in REPO.glob(pattern):add(p)
    for p in OUT.glob('*.py'):add(p)
    for p in OUT.glob('*.json'):
        if p.name!='evidence.json':add(p)
    for folder in ('source-before','performance-model-v1','performance-guest-v1'):
        for p in (OUT/folder).rglob('*'):
            if p.is_file():add(p)
    add(REPO/'projects/cfd_dsa/docs/CFD_DSA_SINGLE_LINE_ROUND3_20261015.md')
    add(ROOT/'single-line-round3-final-20261015/dma-priority.json')
    logs=OUT/'logs';logs.mkdir(exist_ok=True)
    for p in Path('/tmp').glob('cfd-round3-*.log'):
        if p.name in ('cfd-round3-output-priority-build.log','cfd-round3-contract-guest-build.log') or p.name.startswith('cfd-round3-final'):
            target=logs/p.name;shutil.copyfile(p,target);add(target)
    evidence=dict(date='2026-10-15',scope='one line, one forward/backward per sweep',
        build_command='scons build/ARM/gem5.opt -j2',build_environment={'LINKFLAGS_EXTRA':'-Wl,--no-keep-memory'},
        binaries=binaries,campaigns=campaigns,final_simulation_cases=total,
        additional_checks=dict(unit_tests=11,invalid_configurations=15,opt_injected_faults=9,onchip_injected_faults=4),
        preliminary_scans='model/guestV1 preserved separately; excluded from final performance table',files=files)
    (OUT/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print('EVIDENCE_CAPTURE_PASS',total,'final cases',len(files),'hashed files')


if __name__=='__main__':main()
