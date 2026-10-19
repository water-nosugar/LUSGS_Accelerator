import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
SUITES=ROOT/'projects/cfd_dsa/suites'
r=json.loads((SUITES/'single-line-round3-recommended.json').read_text())
def opts(values):
 return dict(v[2:].split('=',1) for v in values)
profiles={c['name']:opts(r['common_args']+c['args']) for c in r['cases']}
def case(name,profile,changes=None,production=False):
 p=dict(profiles[profile]);p.update({'coeff-line-cache-policy':'adaptive' if production else 'full','coeff-warm-y-source':'fifo' if production else 'spm','coeff-record-policy':'warm-summary' if production else 'per-cell','coeff-dma-issue-packets':'1','coeff-dma-issue-bytes':'64','coeff-poison-outputs':'1','coeff-input-pattern':'baseline','coeff-input-seed':'1'})
 p.update({k:str(v) for k,v in (changes or {}).items()});p['coeff-preprocess-trace-cells']=p['lusgs-cells']
 return {'name':name,'target':'lusgs-single-line','args':['--'+k+'='+v for k,v in p.items()]}
perf=[];pairs=[]
for n in [17,65]:
 for profile in profiles:
  changes={'lusgs-cells':n}
  if n==65 and profile.startswith('cold'):changes['coeff-line-tile-cells']=32
  names=[]
  for prod in [False,True]:
   name=f'n{n}-{profile}-'+('production' if prod else 'baseline');names.append(name)
   perf.append(case(name,profile,changes,prod))
  pairs.append(names)
for n in [115,116,129]:
 names=[]
 for prod in [False,True]:
  name=f'n{n}-reuse-direct-'+('production' if prod else 'baseline');names.append(name)
  perf.append(case(name,'reuse-direct',{'lusgs-cells':n,'bench-iters':2,'coefficient-update-interval':2},prod))
 pairs.append(names)
for n in [17,65]:
 for profile in ['reuse-direct','reuse-inverse']:
  names=[]
  for prod in [False,True]:
   name=f'n{n}-{profile}-unbounded-'+('production' if prod else 'baseline');names.append(name)
   perf.append(case(name,profile,{'lusgs-cells':n,'coeff-dma-issue-packets':0,'coeff-dma-issue-bytes':0},prod))
  pairs.append(names)
# Individual feature ablations against the same-resource normal baseline.
for profile,changes,suffix in [('reuse-direct',{'coeff-record-policy':'warm-summary'},'summary'),('reuse-inverse',{'coeff-warm-y-source':'fifo'},'fifo')]:
 name='n17-'+profile+'-'+suffix;perf.append(case(name,profile,changes));pairs.append(['n17-'+profile+'-baseline',name])
name='n116-reuse-direct-solve-cache'
perf.append(case(name,'reuse-direct',{'lusgs-cells':116,'bench-iters':2,'coefficient-update-interval':2,'coeff-line-cache-policy':'adaptive'}));pairs.append(['n116-reuse-direct-baseline',name])
stress=[]
for mode in ['direct','inverse']:
 for n in [1,2]:
  stress.append(case(f'n{n}-{mode}-credits1','reuse-'+mode,{'lusgs-cells':n,'bench-iters':3,'coefficient-update-interval':3,'coeff-line-tile-cells':1,'coeff-stream-window':1,'coeff-spm-outstanding':1,'coeff-drain-outstanding':1,'coeff-drain-queue-depth':1,'coeff-input-slots':1,'lu5-pending-depth':1,'coeff3-pending-depth':1,'coeff-output-buffer-depth':1,'lu5-count':1,'coeff3-count':1,'coeff-onchip-queue-depth':1,'coeff-completion-depth':1,'coeff-rhs-product-depth':1,'coeff-cluster-product-depth':1,'coeff-interface-width':8,'coeff-spm-write-width':40,'coeff-drain-width':40,'coeff-line-tile-dma-width':40,'coeff-onchip-latency':2},True))
 for pattern,seed in [('random',1),('random',42),('scaled',42)]:
  stress.append(case(f'n17-{mode}-{pattern}-{seed}','reuse-'+mode,{'bench-iters':3,'coefficient-update-interval':3,'coeff-input-pattern':pattern,'coeff-input-seed':seed},True))
 stress.append(case('n257-'+mode,'cold-'+mode,{'lusgs-cells':257,'coeff-line-tile-cells':32,'bench-iters':2,'coefficient-update-interval':2},True))
 stress.append(case('n65-'+mode+'-serial','reuse-'+mode,{'lusgs-cells':65,'bench-iters':3,'coefficient-update-interval':3,'coeff-line-mvm-model':'serial'},True))
 stress.append(case('n65-'+mode+'-tree','reuse-'+mode,{'lusgs-cells':65,'bench-iters':3,'coefficient-update-interval':3,'coeff-line-mvm-model':'tree'},True))
for name,profile,changes in [
 ('packet-only','reuse-direct',{'coeff-dma-issue-packets':1,'coeff-dma-issue-bytes':0}),
 ('bytes-only','reuse-inverse',{'coeff-dma-issue-packets':0,'coeff-dma-issue-bytes':64,'coeff-interface-width':40,'coeff-spm-write-width':40,'coeff-spm-banks':2,'coeff-spm-read-ports':2,'coeff-spm-bank-granularity':16}),
 ('two-packets','reuse-direct',{'lusgs-cells':2,'coeff-dma-issue-packets':2,'coeff-dma-issue-bytes':128}),
 ('multi-bank-ports','reuse-inverse',{'lusgs-cells':65,'coeff-dma-issue-packets':0,'coeff-dma-issue-bytes':128,'coeff-spm-banks':2,'coeff-spm-read-ports':2,'coeff-spm-write-ports':2,'coeff-spm-bank-granularity':16})]:
 changes.update({'bench-iters':3,'coefficient-update-interval':3})
 stress.append(case(name,profile,changes,True))
for mode in ['direct','inverse']:
 stress.append(case('n257-'+mode+'-without-poison','cold-'+mode,{'lusgs-cells':257,'coeff-line-tile-cells':32,'coeff-poison-outputs':0},True))
for policy in ['per-cell','warm-summary']:
 stress.append(case('changed-r-contract-'+policy,'reuse-direct',{'lusgs-cells':3,'coeff-cache-contract-test':1,'coeff-record-policy':policy},True))
recovery=[]
for mode in ['direct','inverse']:
 for n in [1,2,17,65]:
  recovery.append(case(f'n{n}-{mode}-recovery','reuse-'+mode,{'lusgs-cells':n,'coeff-recovery-test':1,'coeff-record-policy':'per-cell'},True))
for mode in ['direct','inverse']:
 recovery.append(case('n17-'+mode+'-full-recovery','reuse-'+mode,{'lusgs-cells':17,'coeff-recovery-test':1,'coeff-record-policy':'per-cell','coeff-line-storage':'full','coeff-interface-timing':0,'coeff-line-opt-level':0,'coeff-rhs-row-parallel':0,'coeff-line-cache-policy':'full','coeff-warm-y-source':'spm'},False))
# Smoke cases are copied unchanged from the final suites.
smoke=[next(c for c in perf if c['name']==n) for n in ['n17-reuse-direct-production','n116-reuse-direct-production','n65-cold-direct-baseline']]
smoke += [next(c for c in stress if c['name']=='n17-inverse-scaled-42'),next(c for c in recovery if c['name']=='n17-direct-recovery')]
for name,cases in [('performance',perf),('stress',stress),('recovery',recovery),('smoke',smoke)]:
 data={'campaign':'single-line-round4-'+name+'-20261018','cases':cases}
 if name=='performance':data['comparison_pairs']=pairs
 (SUITES/('single-line-round4-'+name+'.json')).write_text(json.dumps(data,indent=2)+'\n')
 print(name,len(cases))
