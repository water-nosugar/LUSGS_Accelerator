"""Independent upper accumulation storage, order and real completion gates."""
import _support
from pathlib import Path
import subprocess
import tempfile
import unittest
import re
from check_lu_upper import replay_lu_upper
from check_register_ports import _rows


def fixture():
    lines=[];identity=0;cursor=10
    def emit(t,text):lines.append((t,text))
    def upper(t,event,c,k=-1,i=0,bank=-1):
        emit(t,f'CFD_LU_UPPER cycle={t} event={event} token=7 generation=9 request=11 cell=0 column={c} k={k} slot={k%2 if k>=0 else -1} id={i} bank={bank}')
    def operation(t,pool,width,bank,mul=True,scale=False):
        nonlocal identity
        identity+=1;i=identity;end=t+(3 if scale else 4)
        a=-1 if mul else bank;dst=bank if mul and not scale else -1
        emit(t,f'CFD_RF_ISSUE cycle={t} id={i} pool={pool} lanes={width} caller={1 if mul else 2} bank_a={a} bank_b=-1 bank_dst={dst} bank_owner=7 bank_epoch=9')
        emit(t,f'CFD_RF_OWNER cycle={t} id={i} owner=7 frontier=0')
        emit(end,f'CFD_RF_COMPLETE cycle={end} id={i} pool={pool} lanes={width}')
        return i,end
    for c in range(4):
        upper(cursor,'begin',c,bank=63);cursor+=1
        for k in range(c):
            bank=63+k%2
            i,end=operation(cursor,'mul',4-c,bank)
            upper(cursor,'mul-issue',c,k,i,bank);upper(end,'mul-ready',c,k,i,bank)
            i,end2=operation(end,'add',4-c,bank,mul=False)
            upper(end,'sub-issue',c,k,i,bank);upper(end2,'sub-ready',c,k,i,bank)
            cursor=end2
        upper(cursor,'ready',c);upper(cursor+1,'join',c)
        i,end=operation(cursor+2,'mul',4-c,-1,scale=True)
        upper(cursor+2,'scale',c,i=i);cursor=end+1
    lines.sort(key=lambda pair:pair[0])
    return '\n'.join(line for _,line in lines),{'--coeff-lu-upper-overlap':'1','--lu5-count':'4','--coeff-rhs-product-depth':'32'}


def replay(text,cfg):return replay_lu_upper(text,cfg,list(_rows(text)))


class LuUpperTests(unittest.TestCase):
    def test_native_context_respects_order_capacity_returns_and_reservation(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'upper'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra',
                '-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'lu_upper_model.cc'),'-o',str(binary)],
                check=True,capture_output=True)
            self.assertIn('LU_UPPER_MODEL_PASS',subprocess.check_output([str(binary)],text=True))

    def test_owned_ordered_upper_work_and_normalization_join(self):
        text,cfg=fixture();r=replay(text,cfg)
        self.assertEqual(len(r['operation_ids']),12)
        self.assertEqual(r['summary']['begins'],4)
        self.assertEqual(r['summary']['scale'],4)
        self.assertEqual(r['summary']['extra_reservation_bytes'],1152)
        self.assertEqual(r['summary']['added_banks'],8)

    def test_reject_wrong_owner_bank_and_missing_physical_ownership(self):
        text,cfg=fixture()
        for before,after in [('bank_owner=7','bank_owner=8'),('bank=63','bank=71'),
                             ('owner=7 frontier=0','owner=8 frontier=0'),('lanes=3 caller=1','lanes=2 caller=1')]:
            with self.subTest(before=before),self.assertRaises((AssertionError,KeyError)):
                replay(text.replace(before,after),cfg)

    def test_reject_early_actual_return_join_and_previous_column_publication(self):
        text,cfg=fixture()
        lines=text.splitlines()
        targets=['event=mul-ready','event=join','event=begin']
        for event in targets:
            row=next(r for r in lines if event in r and 'column=1 ' in r)
            old=int(row.split('cycle=',1)[1].split()[0]);changed=row.replace(f'cycle={old}',f'cycle={old-5}')
            with self.subTest(event=event),self.assertRaises((AssertionError,KeyError)):
                replay(text.replace(row,changed),cfg)

    def test_reject_disabled_unproven_or_truncated_upper_context(self):
        text,cfg=fixture()
        with self.assertRaisesRegex(AssertionError,'disabled'):replay(text,{})
        missing='\n'.join(r for r in text.splitlines() if 'CFD_LU_UPPER ' not in r)
        with self.assertRaisesRegex(AssertionError,'missing independent'):replay(missing,cfg)
        truncated='\n'.join(r for r in text.splitlines() if not ('CFD_LU_UPPER ' in r and 'event=scale' in r and 'column=3 ' in r))
        with self.assertRaisesRegex(AssertionError,'drain'):replay(truncated,cfg)

    def test_independent_capacity_ledgers_charge_all_upper_storage(self):
        from reporting.test_single_line_config_audit import fixture as config_fixture
        from single_line_config_audit import Effective,capacity_audit
        from check_single_line_storage import lifecycle_reserve
        resolved,_=config_fixture(1024)
        base=capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options'].update(coeff_lu_upper_overlap=True,lu5_count=4)
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-base,1152)
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0','--lu5-count':'4'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-lu-upper-overlap':'1'}))-lifecycle_reserve(cfg),1152)

    def test_cancelled_product_must_return_before_its_bank_is_reused(self):
        text,cfg=fixture()
        prefix=[r for r in text.splitlines() if int(r.split('cycle=',1)[1].split()[0])<=19
                or r.startswith('CFD_RF_COMPLETE cycle=22 id=2 ')]
        prefix.append('CFD_LU_UPPER cycle=19 event=cancel token=7 generation=9 request=11 cell=0 column=1 k=-1 slot=-1 id=0 bank=-1')
        cancelled='\n'.join(prefix)
        self.assertEqual(replay(cancelled,cfg)['summary']['cancelled_contexts'],1)
        other=text.replace('token=7','token=8').replace('bank_owner=7','bank_owner=8').replace('owner=7 ','owner=8 ').replace('request=11','request=12').replace('generation=9','generation=10').replace('bank_epoch=9','bank_epoch=10')
        other=re.sub(r'\bid=(\d+)',lambda m:'id='+str(int(m[1])+100 if int(m[1]) else 0),other)
        other=re.sub(r'cycle=(\d+)',lambda m:'cycle='+str(int(m[1])+20),other)
        def ordered(second):
            rows=(cancelled+'\n'+second).splitlines()
            rows.sort(key=lambda r:int(r.split('cycle=',1)[1].split()[0]))
            return '\n'.join(rows)
        self.assertEqual(replay(ordered(other),cfg)['summary']['contexts'],2)
        early=other.replace('CFD_LU_UPPER cycle=30 event=begin','CFD_LU_UPPER cycle=21 event=begin')
        with self.assertRaisesRegex(AssertionError,'cancelled RF drain'):replay(ordered(early),cfg)
