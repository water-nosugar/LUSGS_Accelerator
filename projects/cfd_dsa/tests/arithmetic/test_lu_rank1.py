"""Right-looking LU keeps scalar rounding, finite products and real returns."""
import _support
from pathlib import Path
import subprocess
import tempfile
import unittest
import re
from check_lu_rank1 import replay_lu_rank1, check_rank1_schedule, rows
from check_register_ports import _rows


def fixture(folded=False,broadcast=False):
    lines=[];identity=0;cursor=10
    def emit(t,text):lines.append((t,text))
    def rank(t,event,c,row=-1,slot=-1,i=0,bank=-1):
        emit(t,f'CFD_LU_RANK1 cycle={t} event={event} token=7 generation=9 request=11 cell=0 column={c} row={row} slot={slot} id={i} bank={bank}')
    def operation(t,kind,width,bank=-1):
        nonlocal identity
        identity+=1;i=identity
        pool='div' if kind=='pivot' else 'add' if kind=='sub' else 'mul'
        caller=0 if kind=='pivot' else 2 if kind=='sub' else 1
        end=t+(12 if kind=='pivot' else 3 if kind=='scale' else 4)
        a=bank if kind=='sub' else -1;dst=bank if kind=='mul' else -1
        fanout=broadcast and kind=='mul' and width>1
        reads=width if kind=='sub' else width+1 if fanout else 2*width
        emit(t,f'CFD_RF_ISSUE cycle={t} id={i} pool={pool} caller={caller} lanes={width} read_words={reads} broadcast={int(fanout)} bank_access={int(kind in ("mul","sub"))} bank_a={a} bank_b=-1 bank_dst={dst} bank_words_a={width if kind=="sub" else 0} bank_owner=7 bank_epoch=9')
        emit(t,f'CFD_RF_OWNER cycle={t} id={i} owner=7 frontier=0')
        emit(end,f'CFD_RF_COMPLETE cycle={end} id={i} pool={pool} lanes={width}')
        if fanout:
            emit(t,f'CFD_PRE_BROADCAST cycle={t} id={i} request=11 generation=9 cell=0 family=lu phase=rank1 k={c} column={row} rhs=-1 lanes={width} factor=0x1p+0')
        return i,end
    rank(cursor,'bind',0,slot=0,bank=27)
    for c in range(5):
        cursor+=0 if folded else 1;i,end=operation(cursor,'pivot',1)
        rank(cursor,'pivot',c,i=i);rank(end,'reciprocal-ready',c,i=i)
        if c==4:rank(cursor+1,'done',c);break
        cursor=end;i,end=operation(cursor,'scale',4-c)
        rank(cursor,'scale',c,i=i);rank(end,'scale-ready',c,i=i)
        cursor=end+(0 if folded else 1);rank(cursor,'rows-begin',c)
        for row in range(c+1,5):
            slot=(row-c-1)%2;bank=27 if slot==0 else 63
            i,end=operation(cursor,'mul',4-c,bank)
            rank(cursor,'mul-issue',c,row,slot,i,bank);rank(end,'mul-ready',c,row,slot,i,bank)
            cursor=end;i,end=operation(cursor,'sub',4-c,bank)
            rank(cursor,'sub-issue',c,row,slot,i,bank);rank(end,'sub-ready',c,row,slot,i,bank)
            cursor=end
    lines.sort(key=lambda v:v[0])
    return '\n'.join(v for _,v in lines),{'--coeff-lu-rank1':'1','--coeff-lu-product-ahead':'1','--lu5-count':'4','--coeff-rhs-product-depth':'32',
        '--coeff-lu-control-fold':str(int(folded)),'--coeff-preprocess-broadcast':str(int(broadcast)),
        '--coeff-rf-broadcast':'1','--coeff-rf-read-words':'16','--coeff-rf-write-words':'8'}


def replay(text,cfg):return replay_lu_rank1(text,cfg,list(_rows(text)))


class LuRank1Tests(unittest.TestCase):
    def test_native_bitwise_crout_and_bounded_cancelled_returns(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'rank1'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-ffp-contract=off',
                '-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'lu_rank1_model.cc'),'-o',str(binary)],
                check=True,capture_output=True)
            self.assertIn('LU_RANK1_MODEL_PASS',subprocess.check_output([str(binary)],text=True))

    def test_both_capacity_ledgers_charge_contexts_and_bounded_selection(self):
        from reporting.test_single_line_config_audit import fixture
        from single_line_config_audit import Effective,capacity_audit
        from check_single_line_storage import lifecycle_reserve
        resolved,_=fixture(1024)
        before=capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options'].update(coeff_lu_rank1=True,lu5_count=4)
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-before,2176)
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0','--lu5-count':'4'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-lu-rank1':'1'}))-lifecycle_reserve(cfg),2176)

    def test_every_owned_operation_and_registered_column_barrier(self):
        text,cfg=fixture();r=replay(text,cfg)
        self.assertEqual(len(r['product_ids']),20)
        self.assertEqual(r['summary']['pivot'],5)
        self.assertEqual(r['summary']['scale'],4)
        self.assertEqual(r['summary']['sub-ready'],10)
        self.assertEqual(r['summary']['extra_reservation_bytes'],2176)

    def test_reject_wrong_owner_width_bank_and_unpaid_operands(self):
        text,cfg=fixture()
        for before,after in [('owner=7 frontier=0','owner=8 frontier=0'),
                ('bank_owner=7','bank_owner=8'),('bank=27','bank=28'),
                ('lanes=4 read_words=8','lanes=3 read_words=8'),
                ('read_words=8','read_words=4')]:
            with self.subTest(before=before),self.assertRaises((AssertionError,KeyError)):
                replay(text.replace(before,after),cfg)

    def test_reject_early_product_normalization_and_column_publication(self):
        text,cfg=fixture()
        for event in ('mul-ready','scale-ready','rows-begin','pivot'):
            line=next(r for r in text.splitlines() if 'CFD_LU_RANK1 ' in r and
                      f'event={event} ' in r and f'column={1 if event=="pivot" else 0} ' in r)
            now=int(line.split('cycle=',1)[1].split()[0])
            changed=line.replace(f'cycle={now}',f'cycle={now-2}')
            with self.subTest(event=event),self.assertRaises((AssertionError,KeyError)):
                replay(text.replace(line,changed),cfg)

    def test_reject_disabled_missing_and_truncated_independent_evidence(self):
        text,cfg=fixture()
        with self.assertRaisesRegex(AssertionError,'disabled'):replay(text,{})
        missing='\n'.join(r for r in text.splitlines() if not r.startswith('CFD_LU_RANK1 '))
        with self.assertRaisesRegex(AssertionError,'missing independent'):replay(missing,cfg)
        with self.assertRaisesRegex(AssertionError,'drain'):
            replay('\n'.join(r for r in text.splitlines() if 'event=done ' not in r),cfg)

    def test_context_reuse_waits_for_cancelled_real_returns(self):
        text,cfg=fixture();cut=28
        issued={r['id'] for prefix,r in _rows(text) if prefix=='CFD_RF_ISSUE' and int(r['cycle'])<=cut}
        prefix=[]
        for line in text.splitlines():
            t=int(line.split('cycle=',1)[1].split()[0])
            if t<=cut or (line.startswith('CFD_RF_COMPLETE ') and line.split('id=',1)[1].split()[0] in issued):prefix.append(line)
        prefix.append('CFD_LU_RANK1 cycle=28 event=cancel token=7 generation=9 request=11 cell=0 column=0 row=-1 slot=-1 id=0 bank=-1')
        cancelled='\n'.join(prefix)
        self.assertEqual(replay(cancelled,cfg)['summary']['cancelled_contexts'],1)
        other=text.replace('token=7','token=8').replace('bank_owner=7','bank_owner=8').replace('owner=7 ','owner=8 ').replace('request=11','request=12').replace('generation=9','generation=10').replace('bank_epoch=9','bank_epoch=10')
        other=re.sub(r'\bid=(\d+)',lambda m:'id='+str(int(m[1])+100 if int(m[1]) else 0),other)
        other=re.sub(r'cycle=(\d+)',lambda m:'cycle='+str(int(m[1])+100),other)
        def ordered(second):
            values=(cancelled+'\n'+second).splitlines()
            return '\n'.join(sorted(values,key=lambda line:int(line.split('cycle=',1)[1].split()[0])))
        self.assertEqual(replay(ordered(other),cfg)['summary']['contexts'],2)
        early=other.replace('CFD_LU_RANK1 cycle=110 event=bind','CFD_LU_RANK1 cycle=29 event=bind')
        with self.assertRaisesRegex(AssertionError,'actual RF drain'):replay(ordered(early),cfg)

    def test_schedule_joins_paid_input_and_all_factor_publications(self):
        text,cfg=fixture()
        def row(stage,t,k=-1):
            return dict(stage=stage,cycle=str(t),k=str(k),requestId='11',generation='9',cell='0')
        trace=[row('REQUEST_ALLOC',8),row('INPUT_D_READY',9),row('LU_ISSUE',10)]
        mapping={'pivot':'LU_STEP_READY','reciprocal-ready':'LU_RECIP_READY',
                 'rows-begin':'LU_COLUMN_DONE','done':'LU_COLUMN_DONE'}
        for r in rows(text):
            if r['event'] in mapping:trace.append(row(mapping[r['event']],r['cycle'],r['column']))
            if r['event']=='done':trace.append(row('LU_COMPLETE',r['cycle']))
        self.assertEqual(check_rank1_schedule(text,cfg,trace)['paid_input_bindings'],1)
        early=[dict(r,cycle='11') if r['stage']=='INPUT_D_READY' else r for r in trace]
        with self.assertRaisesRegex(AssertionError,'paid D input'):check_rank1_schedule(text,cfg,early)

    def test_bounded_ready_control_never_precedes_real_rf_writes(self):
        text,cfg=fixture(folded=True)
        r=replay(text,cfg)
        self.assertEqual(r['summary']['control_latency_after_actual_rf_return'],0)
        self.assertTrue(r['control_summary']['physical_returns_bound'])
        with self.assertRaisesRegex(AssertionError,'registered'):
            replay(text,dict(cfg,**{'--coeff-lu-control-fold':'0'}))
        line=next(r for r in text.splitlines() if 'event=scale-ready ' in r)
        t=int(line.split('cycle=',1)[1].split()[0])
        with self.assertRaises((AssertionError,KeyError)):
            replay(text.replace(line,line.replace(f'cycle={t}',f'cycle={t-1}')),cfg)

    def test_rank1_broadcast_reuses_only_one_paid_common_factor(self):
        from check_register_ports import check_preprocess_broadcast
        text,cfg=fixture(folded=True,broadcast=True)
        self.assertEqual(replay(text,cfg)['summary']['mul-issue'],10)
        trace=[dict(stage='REQUEST_ALLOC',requestId='11',generation='9',cell='0'),
               dict(stage='LU_COMPLETE',requestId='11')]
        for r in rows(text):
            if r['event']=='mul-issue':
                trace.append(dict(stage='LU_ROW_MUL_ISSUE',requestId='11',cycle=r['cycle'],k=r['column'],row=r['row']))
        self.assertEqual(check_preprocess_broadcast(text,cfg,trace)['lu_saved_words'],20)
        with self.assertRaises(AssertionError):
            check_preprocess_broadcast(text.replace('phase=rank1','phase=lower'),cfg,trace)
        with self.assertRaises(AssertionError):
            replay(text.replace('read_words=5 broadcast=1','read_words=1 broadcast=1'),cfg)
