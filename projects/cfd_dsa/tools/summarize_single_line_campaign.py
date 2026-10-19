#!/usr/bin/env python3
"""Summarize accepted cases and explicitly verify same-resource A/B pairs."""
import argparse
import hashlib
import json
from pathlib import Path

from analyze_single_line_metrics import metrics


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(campaign, suite, report):
    specification=json.loads(suite.read_text())
    rows={};recovery={};provenance=set()
    for case in specification['cases']:
        name=case['name'];path=campaign/name
        manifest=json.loads((path/'manifest.json').read_text())
        assert manifest['status']=='passed', (name,'simulation or acceptance failed')
        assert manifest.get('acceptance_status')=='passed', (name,'resource acceptance missing')
        assert not manifest['provenance_errors'], (name,'provenance changed')
        assert sha256(path/'sources.json')==manifest['sources_sha256']
        resolved=json.loads((path/'resolved_experiment.json').read_text())
        assert resolved['guest_sha256']==manifest['guest_sha256']
        assert resolved['guest_binary']==manifest['guest_binary']
        provenance.add((manifest['gem5_sha256'],manifest['guest_sha256'],manifest['sources_sha256']))
        row=json.loads((path/'acceptance.json').read_text())
        if row.get('result')=='passed':
            recovery[name]=row
        else:
            rows[name]=row
    assert len(provenance)==1, 'campaign mixes source contents or binaries'
    detailed={n:metrics(campaign.name,n,r) for n,r in rows.items()}
    pairs=[]
    allowed={'--coeff-line-cache-policy','--coeff-record-policy','--coeff-warm-y-source'}
    for before,after in specification.get('comparison_pairs',[]):
        a,b=rows[before],rows[after]
        config_a={k:v for k,v in a['config'].items() if k not in allowed}
        config_b={k:v for k,v in b['config'].items() if k not in allowed}
        assert config_a==config_b, ('uncontrolled A/B parameters',before,after)
        assert a['hashes']==b['hashes'], ('FP64 output changed',before,after)
        for pool in a['pools']:
            assert all(a['pools'][pool][k]==b['pools'][pool][k] for k in ('count','lat','ii'))
        ma,mb=detailed[before],detailed[after]
        pairs.append(dict(baseline=before,optimized=after,
            before_cycles=a['cycles'],after_cycles=b['cycles'],
            cycle_reduction_pct=100*(1-b['cycles']/a['cycles']),
            before_dma_bytes=ma['dma']['bytes'],after_dma_bytes=mb['dma']['bytes'],
            before_cache_hits=[w['cache_hit'] for w in a['windows']],
            after_cache_hits=[w['cache_hit'] for w in b['windows']]))
    # Preserve the existing analysis schema so raw-trace analysis can be rerun.
    (campaign/'optimization.json').write_text(json.dumps(rows,indent=2)+'\n')
    result=dict(schema_version=1,campaign=campaign.name,provenance=list(next(iter(provenance))),
        cases=detailed,recovery=recovery,comparison_pairs=pairs)
    report.write_text(json.dumps(result,indent=2)+'\n')
    print('SINGLE_LINE_CAMPAIGN_PASS',len(rows),'normal',len(recovery),'recovery',len(pairs),'pairs')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('campaign',type=Path)
    parser.add_argument('--suite',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    summarize(args.campaign,args.suite,args.report)
