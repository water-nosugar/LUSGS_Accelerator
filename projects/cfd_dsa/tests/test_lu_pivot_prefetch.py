"""Prefetch stays owned, paid and bound to its actual diagonal RF producer."""
import _support
from pathlib import Path
import re,subprocess,tempfile,unittest
from test_lu_fma import fixture as fused_fixture
from check_lu_rank1 import replay_lu_rank1
from check_lu_pivot_prefetch import replay_lu_pivot_prefetch,check_pivot_prefetch_schedule
from check_register_ports import _rows

def fields(line):return dict(w.split('=',1) for w in line.split()[1:] if '=' in w)
def fixture():
    text,cfg,_=fused_fixture();cfg=dict(cfg,**{'--coeff-lu-pivot-prefetch':'1','--coeff-lu-reciprocal-share':'1'})
    lines=text.splitlines();rank=[fields(l) for l in lines if l.startswith('CFD_LU_RANK1 ')]
    pivot=next(r for r in rank if r['event']=='pivot' and r['column']=='1');pid=pivot['id'];adopt=int(pivot['cycle'])
    source=next(r for r in rank if r['event']=='fma-ready' and r['column']=='0' and r['row']=='1')
    issue=int(source['cycle']);returned=issue+12;assert returned<adopt
    converted=[];diagonal=None
    for line in lines:
        r=fields(line)
        if line.startswith(('CFD_RF_ISSUE ','CFD_RF_OWNER ')) and r.get('id')==pid:
            line=line.replace('cycle='+r['cycle'],'cycle='+str(issue)).replace('ready='+str(adopt+12),'ready='+str(returned))
        elif line.startswith('CFD_RF_COMPLETE ') and r['id']==pid:
            line=line.replace('cycle='+r['cycle'],'cycle='+str(returned))
        elif line.startswith('CFD_LU_RANK1 ') and r['id']==pid:
            if r['event']=='pivot':line=line.replace('event=pivot','event=pivot-adopt')
            elif r['event']=='reciprocal-ready':line=line.replace('event=reciprocal-ready','event=prefetch-ready').replace('cycle='+r['cycle'],'cycle='+str(returned))
        converted.append(line)
        if line.startswith('CFD_LU_FMA ') and r['event']=='complete' and int(r['row'])==int(r['column'])+1:
            c=int(r['column'])+1
            converted.append(f'CFD_LU_PIVOT_ROW cycle={r["cycle"]} token=7 generation=9 request=11 cell=0 column={c} rf={r["rf"]} fused=1 a={r["a0"]} b={r["b0"]} c={r["c0"]} result={r["r0"]}')
            if c==1:diagonal=r['r0']
    converted.extend([f'CFD_LU_RANK1 cycle={issue} event=pivot-prefetch token=7 generation=9 request=11 cell=0 column=1 row=-1 slot=-1 bank=-1 id={pid}',
        f'CFD_LU_PIVOT_PREFETCH cycle={issue} token=7 generation=9 request=11 cell=0 column=1 rf={pid} source={source["id"]} diagonal={diagonal}',
        'CFD_LU_PIVOT_PREFETCH_CAPACITY contexts=6 per_context=1 state_bytes=400 context_reserved=512 control_bytes=128 selector_width=1 extra_units=0'])
    converted.sort(key=lambda l:int(fields(l).get('cycle',999999)))
    return '\n'.join(converted),cfg

def replay(text,cfg):
    events=list(_rows(text))
    return replay_lu_rank1(text,cfg,events),replay_lu_pivot_prefetch(text,cfg,events)

class LuPivotPrefetchTests(unittest.TestCase):
    def test_native_exact_rounding_in_both_lu_modes_and_cancel_before_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'pivot'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math',
                '-I'+str(_support.REPO_ROOT/'src'),str(_support.FIXTURES/'lu_pivot_prefetch_model.cc'),'-o',str(binary)],check=True,capture_output=True)
            self.assertIn('LU_PIVOT_PREFETCH_MODEL_PASS',subprocess.check_output([str(binary)],text=True))
    def test_prefetch_returns_before_adoption_without_second_division(self):
        text,cfg=fixture();rank,p=replay(text,cfg)
        self.assertEqual((rank['summary']['pivot'],rank['summary']['pivot-prefetch'],rank['summary']['pivot-adopt']),(4,1,1))
        self.assertEqual((p['issues'],p['adoptions'],p['control_bytes']),(1,1,128))
    def test_prefetch_return_after_adoption_uses_existing_rf_tag(self):
        text,cfg=fixture();r=next(fields(l) for l in text.splitlines() if l.startswith('CFD_LU_PIVOT_PREFETCH '));rf=r['rf']
        adopt=next(int(fields(l)['cycle']) for l in text.splitlines() if 'event=pivot-adopt ' in l)
        updated=[]
        for line in text.splitlines():
            row=fields(line)
            if (line.startswith('CFD_RF_COMPLETE ') and row['id']==rf) or 'event=prefetch-ready ' in line:
                line=line.replace('cycle='+row['cycle'],'cycle='+str(adopt+1)).replace('event=prefetch-ready','event=reciprocal-ready')
            updated.append(line)
        updated.sort(key=lambda l:int(fields(l).get('cycle',999999)))
        self.assertEqual(replay('\n'.join(updated),cfg)[1]['issues'],1)
    def test_disabled_prefetch_rejects_transfers(self):
        text,cfg=fixture()
        with self.assertRaises(AssertionError):replay(text,dict(cfg,**{'--coeff-lu-pivot-prefetch':'0'}))
    def test_wrong_diagonal_or_source_and_unrounded_row_are_rejected(self):
        text,cfg=fixture()
        for prefix,field,value in [('CFD_LU_PIVOT_PREFETCH ','diagonal','0x1p+10'),('CFD_LU_PIVOT_PREFETCH ','source','9999'),('CFD_LU_PIVOT_ROW ','result','0x1p+10')]:
            line=next(l for l in text.splitlines() if l.startswith(prefix));changed=re.sub(r' '+field+r'=[^\s]+',' '+field+'='+value,line)
            with self.subTest(field=field),self.assertRaises((AssertionError,KeyError)):replay(text.replace(line,changed),cfg)
    def test_missing_numeric_or_physical_prefetch_witness_is_rejected(self):
        text,cfg=fixture()
        for prefix in ['CFD_LU_PIVOT_PREFETCH ','CFD_LU_PIVOT_ROW ']:
            with self.subTest(prefix=prefix),self.assertRaises((AssertionError,KeyError)):
                replay('\n'.join(l for l in text.splitlines() if not l.startswith(prefix)),cfg)
    def test_paid_divider_csv_must_match_returned_diagonal(self):
        from check_lu_reciprocal import bits
        text,cfg=fixture();r=next(fields(l) for l in text.splitlines() if l.startswith('CFD_LU_PIVOT_PREFETCH '));d=float.fromhex(r['diagonal'])
        trace=[dict(stage='REQUEST_ALLOC',requestId='11',generation='9',cell='0'),dict(stage='LU_RECIP_DIV_ISSUE',requestId='11',k='1',cycle=r['cycle'])]
        div=[dict(event='result',accepted=r['cycle'],a=format(bits(1.0),'016x'),b=format(bits(d),'016x'),result=format(bits(1.0/d),'016x'))]
        self.assertEqual(check_pivot_prefetch_schedule(text,cfg,trace,div)['paid_divider_sources'],1)
        div[0]['b']=format(bits(d+1),'016x')
        with self.assertRaisesRegex(AssertionError,'another diagonal'):check_pivot_prefetch_schedule(text,cfg,trace,div)
    def test_all_storage_ledgers_charge_global_control(self):
        from check_single_line_storage import lifecycle_reserve
        self.assertEqual(lifecycle_reserve({'--coeff-lu-pivot-prefetch':'1'})-lifecycle_reserve({}),128)
        from reporting.test_single_line_config_audit import fixture as audit_fixture
        from single_line_config_audit import Effective,capacity_audit
        resolved,_=audit_fixture(1024);resolved['options'].update(coeff_lu_rank1=True,coeff_lu_reciprocal_share=True)
        before=capacity_audit(Effective(resolved))['reservation_bytes'];resolved['options']['coeff_lu_pivot_prefetch']=True
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-before,128)
