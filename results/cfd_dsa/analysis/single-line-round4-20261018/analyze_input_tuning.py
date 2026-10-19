"""Check input admission tuning against a fixed FU/bank/SPM capacity budget."""
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'projects/cfd_dsa/tools'))
from analyze_single_line_metrics import metrics

campaign=ROOT/'results/cfd_dsa/campaigns/single-line-round4-input-tuning-20261019'
suite=json.loads((ROOT/'projects/cfd_dsa/suites/single-line-round4-input-tuning.json').read_text())
rows={};provenance=set()
for case in suite['cases']:
    p=campaign/case['name'];m=json.loads((p/'manifest.json').read_text())
    assert m['status']=='passed' and m['acceptance_status']=='passed' and not m['provenance_errors']
    provenance.add((m['gem5_sha256'],m['guest_sha256'],m['sources_sha256']))
    rows[case['name']]=json.loads((p/'acceptance.json').read_text())
assert len(provenance)==1
detailed={name:metrics(campaign.name,name,row) for name,row in rows.items()}
pairs=[]
for n in [17,65,116]:
    for policy in ['per-cell','summary']:
        before,after=[f'n{n}-direct-input{s}-{policy}' for s in [2,4]]
        a,b=rows[before],rows[after]
        assert {k:v for k,v in a['config'].items() if k!='--coeff-input-slots'}=={
            k:v for k,v in b['config'].items() if k!='--coeff-input-slots'}
        assert a['hashes']==b['hashes']
        for pool in a['pools']:
            assert all(a['pools'][pool][k]==b['pools'][pool][k] for k in ['count','lat','ii','issued'])
        ma,mb=detailed[before],detailed[after]
        assert mb['peak_bytes']-ma['peak_bytes']==16640
        assert mb['peak_bytes']<=196608 and ma['dma']['bytes']==mb['dma']['bytes']
        pairs.append(dict(cells=n,policy=policy,before=before,after=after,
            before_cycles=a['cycles'],after_cycles=b['cycles'],
            cycle_reduction_pct=100*(1-b['cycles']/a['cycles']),
            before_peak_bytes=ma['peak_bytes'],after_peak_bytes=mb['peak_bytes'],
            before_controller_cycles=ma['controller_cycles'],after_controller_cycles=mb['controller_cycles'],
            before_warm_cycles=[w['cycles'] for w in a['windows'][1:]],
            after_warm_cycles=[w['cycles'] for w in b['windows'][1:]],
            dma_bytes=ma['dma']['bytes']))
(campaign/'optimization.json').write_text(json.dumps(rows,indent=2)+'\n')
(HERE/'input-tuning-final.json').write_text(json.dumps(dict(provenance=list(next(iter(provenance))),
    cases=detailed,pairs=pairs),indent=2)+'\n')
for p in pairs:print(p['cells'],p['policy'],p['before_cycles'],p['after_cycles'],round(p['cycle_reduction_pct'],3))
print('INPUT_TUNING_PASS',len(rows),'cases',len(pairs),'pairs')
