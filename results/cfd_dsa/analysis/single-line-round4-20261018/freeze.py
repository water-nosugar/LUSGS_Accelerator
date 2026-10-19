"""Freeze the final accepted set without relabeling failed/interrupted attempts."""
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tarfile

ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'projects/cfd_dsa/tools'))
from run_experiment import source_fingerprint


def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


specs=[
 ('single-line-round4-isolated-performance-20261019','single-line-round4-performance.json',True),
 ('single-line-round4-isolated-stress-20261019','single-line-round4-stress-main.json',True),
 ('single-line-round4-isolated-cache-contract-20261019','single-line-round4-cache-contract.json',True),
 ('single-line-round4-isolated-recovery-20261019','single-line-round4-recovery.json',True),
 ('single-line-round4-isolated-c-20261019','single-line-c-complete.json',False),
 ('single-line-round4-isolated-d-20261019','single-line-d-storage.json',False),
 ('single-line-round4-input-tuning-20261019','single-line-round4-input-tuning.json',True),
]
sources=source_fingerprint()
(HERE/'sources-final.json').write_text(json.dumps(sources,indent=2,sort_keys=True)+'\n')
with tarfile.open(HERE/'sources-final.tar.gz','w:gz') as archive:
    for name in sources:archive.add(ROOT/name,arcname=name,recursive=False)
runtime=lambda files:{k:v for k,v in files.items() if not k.startswith('projects/cfd_dsa/tools/')}
files=set();campaigns=[];binaries=set()
for name,suite_name,automatic in specs:
    suite_path=ROOT/'projects/cfd_dsa/suites'/suite_name
    suite=json.loads(suite_path.read_text());files.add(suite_path)
    campaign=ROOT/'results/cfd_dsa/campaigns'/name
    source_ids=set()
    for case in suite['cases']:
        folder=campaign/case['name'];m=json.loads((folder/'manifest.json').read_text())
        assert m['status']=='passed' and not m['provenance_errors'],(name,case['name'])
        if automatic:assert m['acceptance_status']=='passed'
        index=next(i for i,s in enumerate(m['command']) if s.endswith('run_cfd_lusgs_single_line.py'))
        assert m['command'][index+1:]==suite.get('common_args',[])+case['args']
        original=json.loads((folder/'sources.json').read_text())
        assert sha(folder/'sources.json')==m['sources_sha256']
        assert runtime(original)==runtime(sources), 'simulated source contents changed'
        differences={k for k in original.keys()|sources.keys() if original.get(k)!=sources.get(k)}
        assert differences<={'projects/cfd_dsa/tools/check_context_ablation.py',
                            'projects/cfd_dsa/tools/check_single_line_c.py'}, differences
        binaries.add((m['gem5_sha256'],m['guest_sha256']))
        source_ids.add(m['sources_sha256'])
        files.update(p for p in folder.rglob('*') if p.is_file())
    assert len(source_ids)==1,(name,'mixed case tool versions')
    campaigns.append(dict(name=name,suite=str(suite_path.relative_to(ROOT)),cases=len(suite['cases']),
        automatic_acceptance=automatic,sources_sha256=next(iter(source_ids))))
    files.update(p for p in campaign.glob('*.json') if p.is_file())
assert len(binaries)==1
gem5,guest=next(iter(binaries))
assert sha(ROOT/'build/ARM/gem5.opt')==gem5
assert sha(ROOT/'build/cfd_dsa/aarch64/test_cfd_lusgs_patha_step2_arm')==guest

# This patch is against the task-start snapshot, not the user's Git base.
before=json.loads((HERE/'source-before.json').read_text())
new=[
 'projects/cfd_dsa/benchmarks/lusgs/step2_numerical_contract.inc',
 'projects/cfd_dsa/benchmarks/lusgs/step2_recovery_contract.inc',
 'projects/cfd_dsa/configs/single_line_contract.py',
 'projects/cfd_dsa/tools/single_line_metrics.py',
 'projects/cfd_dsa/tools/analyze_single_line_metrics.py',
 'projects/cfd_dsa/tools/check_single_line_recovery.py',
 'projects/cfd_dsa/tools/summarize_single_line_campaign.py',
 'projects/cfd_dsa/tests/test_single_line_metrics.py',
 'projects/cfd_dsa/tests/raw_reference_isolation.c',
 'projects/cfd_dsa/tests/test_reference_isolation.py',
]
patch=[];changed=[]
for name in sorted(set(before)|set(new)):
    old=(HERE/'source-before'/name).read_text() if name in before else ''
    current=(ROOT/name).read_text()
    if old==current:continue
    changed.append(name)
    patch.extend(difflib.unified_diff(old.splitlines(True),current.splitlines(True),
        fromfile='a/'+name if name in before else '/dev/null',tofile='b/'+name))
(HERE/'changes.patch').write_text(''.join(patch))
(HERE/'changed-files.json').write_text(json.dumps(changed,indent=2)+'\n')

# Keep the root-cause reproductions and the latency regression reproducible.
for name in ['single-line-round4-performance-20261019','single-line-round4-stress-20261019',
             'single-line-round4-recovery-20261019','single-line-round4-full-recovery-probe-20261019',
             'single-line-round4-full-recovery-dirty-probe-20261019']:
    files.update(p for p in (ROOT/'results/cfd_dsa/campaigns'/name).rglob('*') if p.is_file())
files.update(p for p in HERE.rglob('*') if p.is_file() and p.name not in
             ['evidence.json','verification.log'] and '__pycache__' not in p.parts)
files.update(p for p in (ROOT/'projects/cfd_dsa/tests').glob('*') if p.is_file())
files.add(ROOT/'projects/cfd_dsa/docs/CFD_DSA_SINGLE_LINE_ROUND4_20261019.md')
files.add(ROOT/'projects/cfd_dsa/docs/README.md')
files.add(ROOT/'projects/cfd_dsa/configs/README.md')
files.add(ROOT/'projects/cfd_dsa/suites/README.md')
files.add(ROOT/'projects/cfd_dsa/suites/single-line-round4-recommended.json')
for name in changed:files.add(ROOT/name)
result=dict(schema_version=1,gem5_sha256=gem5,guest_sha256=guest,
    sources_sha256=sha(HERE/'sources-final.json'),campaigns=campaigns,
    files={str(p.relative_to(ROOT)):sha(p) for p in sorted(files)})
(HERE/'evidence.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
print('ROUND4_FROZEN',len(files),'files',sum(c['cases'] for c in campaigns),'accepted simulation cases',
      len(changed),'changed/new code files')
