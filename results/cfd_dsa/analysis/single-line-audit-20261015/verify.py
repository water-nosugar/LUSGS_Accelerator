#!/usr/bin/env python3
"""Verify immutable audit inputs, current implementation and computed summaries."""
import hashlib
import json
import math
from pathlib import Path

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]


def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as src:
        for chunk in iter(lambda:src.read(8*1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


def main():
    evidence=json.loads((OUT/'evidence.json').read_text())
    for name,digest in evidence['files'].items():
        assert sha(REPO/name)==digest,('changed audit evidence',name)
    original=REPO/'results/cfd_dsa/analysis/single-line-round2-20261012'
    for name,digest in json.loads((original/'source-final.json').read_text()).items():
        assert sha(REPO/name)==digest,('implementation changed',name)
    assert sha(REPO/'build/ARM/gem5.opt')==evidence['binary']
    baseline=json.loads((OUT/'details.json').read_text())
    sensitivity=json.loads((OUT/'sensitivity.json').read_text())
    rows={**baseline,**sensitivity};count=sweeps=new_count=new_sweeps=0
    for name,r in rows.items():
        path=REPO/'results/cfd_dsa/campaigns'/r['campaign']/name
        manifest=json.loads((path/'manifest.json').read_text())
        assert manifest['status']=='passed' and manifest['return_code']==0
        assert manifest['gem5_sha256']==r['binary']==evidence['binary']
        report=json.loads((path.parent/'optimization.json').read_text())[name]
        assert r['total_roi_cycles']==report['cycles']
        assert math.isfinite(report['max_abs']) and report['max_abs']<=2.220446049250313e-16
        assert r['controller_cycles']==sum(w['cycles'] for w in r['windows'])
        for w in r['windows']:
            assert w['cycles']==sum(w['phase'][key]['cycles'] for key in ('forward','backward','tail'))
            back=w['phase']['backward']
            assert back['empty']==sum(s['bubble'] for s in w['backward_steps'])
            assert back['cycles']==back['active']+back['empty']
            assert w['tile_dma_wait']<=back['empty']
            for pool,v in w['pools'].items():
                assert 0<=v['issue_pct']<=100 and 0<=v['active_pct']<=100
                assert v['issued']==sum(int(k)*v2 for k,v2 in v['width_histogram'].items())
            for v in w['owners'].values():assert v['peak']<=v['capacity']
        if r['campaign'].startswith('single-line-audit-'):
            new_count+=1;new_sweeps+=len(r['windows'])
        count+=1;sweeps+=len(r['windows'])
    for name,r in sensitivity.items():
        b=baseline[name.removesuffix('-ii2').removesuffix('-add2')]
        assert abs(r['reduction_pct']-100*(1-r['total_roi_cycles']/b['total_roi_cycles']))<1e-12
        assert set(r['config_difference'])==({'--coeff-cluster-div-ii'} if name.endswith('-ii2') else {'--coeff-cluster-add-lat'})
    for name,marker in evidence['logs'].items():assert marker in (OUT/'logs'/name).read_text()
    assert (count,sweeps,new_count,new_sweeps)==(10,46,6,30)
    print(f'AUDIT_EVIDENCE_PASS {count} analyzed cases / {sweeps} sweeps; '
          f'{new_count} fresh diagnostic cases / {new_sweeps} sweeps; implementation unchanged')


if __name__=='__main__':main()
