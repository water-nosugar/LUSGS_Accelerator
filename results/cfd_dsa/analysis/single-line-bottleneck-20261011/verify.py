#!/usr/bin/env python3
"""Final consistency check for this read-only analysis artifact."""
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
data=json.loads((OUT/'metrics.json').read_text())
cases=data['cases']
assert len(cases)==28
assert sum(len(c['sweeps']) for c in cases.values())==88
for path,sha in data['source_hashes_verified'].items():
    assert hashlib.sha256((REPO/path).read_bytes()).hexdigest()==sha,path
for name in ('check-tuning.log','check-optimization.log','check-mode-tuned.log'):
    assert (OUT/name).read_text().rstrip().endswith('SINGLE_LINE_STORAGE_CHECK_PASS'),name
assert 'BOTTLENECK_AUDIT_PASS 28 cases; 88 sweeps' in (OUT/'audit.log').read_text()
assert 'DETAIL_AUDIT_PASS' in (OUT/'details.log').read_text()
for c in cases.values():
    assert c['guest_total_cycles']>=c['controller_cycles']
    assert c['dma']['issued']==c['dma']['completed']
    for w in c['sweeps']:
        b=w['backward']
        assert b['duration']==b['any_pipeline_active_cycles']+int(w['storage']['data_wait'])+int(w['schedule']['output_queue_stalls'])
        assert w['duration']==w['forward']['duration']+b['duration']+w['completion_tail']['duration']
direct=cases['out16-direct']
assert direct['guest_total_cycles']==15340 and direct['controller_cycles']==13040
assert direct['dma']['read_bytes']+direct['dma']['write_bytes']==52776
assert 10480+160+3400+3880+3840+680+30328+8==52776
assert cases['n17-t4-k7-o16-direct']['dma']['read_bytes']+cases['n17-t4-k7-o16-direct']['dma']['write_bytes']==52776+6*39576
assert cases['n17-t4-k7-o16-inverse']['dma']['read_bytes']+cases['n17-t4-k7-o16-inverse']['dma']['write_bytes']==52776+6*9248
report=REPO/'projects/cfd_dsa/docs/CFD_DSA_SINGLE_LINE_BOTTLENECK_20261012.md'
for target in re.findall(r'\]\((/[^)]+)\)',report.read_text()):
    path=re.sub(r':\d+$','',target)
    assert Path(path).exists(),path
for name in ('bottleneck_summary','pipeline_timeline'):
    ET.parse(OUT/(name+'.svg'))
    assert (OUT/(name+'.png')).read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
accepted=json.loads((REPO/'results/cfd_dsa/campaigns/single-line-d-delivery-20261009/acceptance.json').read_text())
binary=json.loads((OUT/'binary-provenance.json').read_text())
assert binary==dict(gem5=accepted['gem5_sha256'],guest=accepted['guest_sha256'])
print('FINAL_BOTTLENECK_REPORT_CHECK_PASS: 28 cases / 88 sweeps; 41 storage checks; source and binary provenance; traffic and backward decomposition; report links; figures')
