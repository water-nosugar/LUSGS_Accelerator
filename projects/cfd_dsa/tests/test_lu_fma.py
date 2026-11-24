"""Dedicated LU FMA preserves owner barriers, paid operands and numerical proof."""
import _support
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from check_lu_fma import replay_lu_fma
from check_lu_rank1 import replay_lu_rank1
from check_register_ports import _rows, check_preprocess_broadcast
from check_rhs_fma import rounded_fma
from check_single_line_c import work, validate_cluster_trace
from check_single_line_storage import lifecycle_reserve


def fixture(broadcast=True):
    cfg={'--coeff-lu-fma-count':'4','--coeff-lu-rank1':'1',
         '--coeff-lu-product-ahead':'1','--coeff-lu-control-fold':'1',
         '--lu5-count':'6','--coeff-rhs-product-depth':'32',
         '--coeff-preprocess-broadcast':str(int(broadcast)),
         '--coeff-rf-broadcast':'1','--coeff-rf-read-words':'24','--coeff-rf-write-words':'12'}
    lines=[];logical=[];identity=0;now=10
    matrix=[6.0+.2*i if i==j else .015*(1+5*i+j) for i in range(5) for j in range(5)]
    def emit(cycle,text):lines.append((cycle,text))
    def rank(cycle,event,column,row=-1,i=0,slot=-1,bank=-1):
        emit(cycle,f'CFD_LU_RANK1 cycle={cycle} event={event} token=7 generation=9 request=11 cell=0 column={column} row={row} slot={slot} bank={bank} id={i}')
    def dump(cycle,event):
        emit(cycle,f'CFD_LU_FMA_MATRIX cycle={cycle} event={event} token=7 generation=9 request=11 cell=0 '+
             ' '.join(f'v{i}={x.hex()}' for i,x in enumerate(matrix)))
    def operation(cycle,pool,caller,width,reads,shared=False):
        nonlocal identity
        identity+=1;i=identity;lat={'div':12,'mul':3,'lu_fma':5}[pool]
        emit(cycle,f'CFD_RF_ISSUE cycle={cycle} id={i} pool={pool} caller={caller} lanes={width} read_words={reads} ready={cycle+lat} capacity={20 if pool=="lu_fma" else 30} broadcast={int(shared)} bank_access=0 bank_a=-1 bank_b=-1 bank_dst=-1 local_result=0')
        emit(cycle,f'CFD_RF_OWNER cycle={cycle} id={i} owner=7 frontier=0')
        emit(cycle+lat,f'CFD_RF_COMPLETE cycle={cycle+lat} id={i} pool={pool} lanes={width}')
        return i,cycle+lat
    def fma(cycle,event,i,c,row,a,b,acc,result):
        emit(cycle,f'CFD_LU_FMA cycle={cycle} event={event} rf={i} token=7 generation=9 request=11 cell=0 column={c} row={row} width={4-c} '+
             ' '.join(f'{v}{lane}={values[lane].hex()}' for lane in range(4-c)
                      for v,values in [('a',a),('b',b),('c',acc),('r',result)]))
    rank(now,'bind',0,slot=0,bank=27);dump(now,'input')
    logical.extend([dict(stage='REQUEST_ALLOC',requestId='11',generation='9',cell='0'),
                    dict(stage='LU_ISSUE',requestId='11',cycle=str(now)),
                    dict(stage='INPUT_D_READY',requestId='11',cycle=str(now))])
    for c in range(5):
        i,end=operation(now,'div',0,1,2);rank(now,'pivot',c,i=i);rank(end,'reciprocal-ready',c,i=i)
        if c==4:
            rank(now+1,'done',c);dump(now+1,'factor');break
        now=end;reciprocal=1.0/matrix[5*c+c]
        i,end=operation(now,'mul',1,4-c,2*(4-c));rank(now,'scale',c,i=i)
        for j in range(c+1,5):
            numerator=matrix[5*c+j];matrix[5*c+j]=numerator*reciprocal
            emit(end,f'CFD_LU_SCALE cycle={end} request=11 generation=9 cell=0 k={c} column={j} numerator={numerator.hex()} reciprocal={reciprocal.hex()} result={matrix[5*c+j].hex()}')
        rank(end,'scale-ready',c,i=i);now=end;rank(now,'rows-begin',c)
        for row in range(c+1,5):
            width=4-c;shared=broadcast and width>1
            a=[matrix[5*row+c]]*width;b=[matrix[5*c+j] for j in range(c+1,5)]
            acc=[matrix[5*row+j] for j in range(c+1,5)]
            result=[rounded_fma(a[k],b[k],acc[k]) for k in range(width)]
            i,end=operation(now,'lu_fma',15,width,2*width+1 if shared else 3*width,shared)
            fma(now,'issue',i,c,row,a,b,acc,[0.0]*width);rank(now,'fma-issue',c,row,i)
            fma(end,'complete',i,c,row,a,b,acc,result);rank(end,'fma-ready',c,row,i)
            if shared:
                emit(now,f'CFD_PRE_BROADCAST cycle={now} id={i} request=11 generation=9 cell=0 family=lu phase=rank1-fma k={c} column={row} rhs=-1 lanes={width} factor={a[0].hex()}')
            logical.append(dict(stage='LU_ROW_FMA_ISSUE',requestId='11',cycle=str(now),k=str(c),row=str(row)))
            for j,value in enumerate(result,c+1):matrix[5*row+j]=value
            now=end
    logical.append(dict(stage='LU_COMPLETE',requestId='11'))
    emit(now+12,'CFD_LU_FMA_CAPACITY count=4 lat=5 ii=1 slots=20 bytes=4096 live=0 peak=4 issue_width_per_context=1')
    lines.sort(key=lambda item:item[0])
    return '\n'.join(line for _,line in lines),cfg,logical


def replay(text,config):return replay_lu_fma(text,config,list(_rows(text)))


class LuFmaTests(unittest.TestCase):
    def test_native_rank1_with_bounded_real_returns_and_independent_crout(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'lu_fma'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
                '-ffp-contract=off','-fno-fast-math','-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'lu_fma_model.cc'),'-o',str(binary)],check=True,capture_output=True)
            self.assertIn('LU_FMA_MODEL_PASS',subprocess.check_output([str(binary)],text=True))

    def test_exact_fma_matrix_and_actual_return_barriers(self):
        text,cfg,_=fixture();result=replay(text,cfg)
        self.assertEqual((result['scalar_fma'],result['issue_groups'],result['normalization_muls']),(30,10,10))
        self.assertEqual((result['factor_matrices'],result['capacity'],result['bytes']),(1,20,4096))
        rank=replay_lu_rank1(text,cfg,list(_rows(text)))
        self.assertEqual((rank['summary']['fma-ready'],rank['summary']['pivot']),(10,5))

    def test_broadcast_and_ordinary_three_input_rf_paths(self):
        text,cfg,trace=fixture();result=check_preprocess_broadcast(text,cfg,trace)
        self.assertEqual((result['groups'],result['lu_saved_words']),(9,20))
        text,cfg,_=fixture(False);self.assertEqual(replay(text,cfg)['scalar_fma'],30)

    def test_capacity_snapshot_has_no_event_timestamp(self):
        text,cfg,_=fixture()
        snapshot=next(line for line in text.splitlines() if line.startswith('CFD_LU_FMA_CAPACITY '))
        self.assertNotIn('cycle=',snapshot)
        self.assertEqual(replay(text,cfg)['peak_scalar_slots'],4)

    def test_reject_unpaid_inputs_wrong_capacity_latency_or_caller(self):
        text,cfg,_=fixture()
        for before,after in [('read_words=9','read_words=8'),('capacity=20','capacity=21'),
                ('pool=lu_fma caller=15','pool=lu_fma caller=1'),
                ('bytes=4096','bytes=4095'),('peak=4','peak=3')]:
            with self.subTest(before=before),self.assertRaises(AssertionError):
                replay(text.replace(before,after),cfg)

    def test_reject_changed_accumulator_and_nonfused_result(self):
        text,cfg,_=fixture()
        for event,field in [('issue','c0'),('complete','r0')]:
            line=next(line for line in text.splitlines() if line.startswith('CFD_LU_FMA ') and f'event={event} ' in line)
            changed=re.sub(r' '+field+r'=[^\s]+',' '+field+'=0x1p+10',line)
            with self.subTest(event=event),self.assertRaises(AssertionError):replay(text.replace(line,changed),cfg)

    def test_reject_early_rf_return_or_factor_publication(self):
        text,cfg,_=fixture()
        line=next(line for line in text.splitlines() if line.startswith('CFD_RF_COMPLETE ') and 'pool=lu_fma ' in line)
        cycle=int(re.search(r'cycle=(\d+)',line)[1]);changed=line.replace(f'cycle={cycle}',f'cycle={cycle+1}')
        with self.assertRaises(AssertionError):replay(text.replace(line,changed),cfg)
        line=next(line for line in text.splitlines() if line.startswith('CFD_LU_FMA_MATRIX ') and 'event=factor ' in line)
        changed=re.sub(r' v24=[^\s]+',' v24=0x1p+10',line)
        with self.assertRaisesRegex(AssertionError,'factor differs'):replay(text.replace(line,changed),cfg)

    def test_disabled_pool_rejects_work_and_missing_witness(self):
        text,cfg,_=fixture()
        self.assertEqual(replay('',{}),{})
        with self.assertRaises(AssertionError):replay(text,dict(cfg,**{'--coeff-lu-fma-count':'0'}))
        line=next(line for line in text.splitlines() if line.startswith('CFD_LU_FMA ') and 'event=issue ' in line)
        with self.assertRaises((AssertionError,KeyError)):replay(text.replace(line,''),cfg)

    def test_explicit_pipeline_budget_and_conserved_physical_work(self):
        cfg={'--coeff-lu-rank1':'1','--lu5-count':'6'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-lu-fma-count':'4'}))-lifecycle_reserve(cfg),4096)
        from reporting.test_single_line_config_audit import fixture as audit_fixture
        from single_line_config_audit import Effective,capacity_audit
        resolved,_=audit_fixture(1024);resolved['options']['coeff_lu_rank1']=True
        before=capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options']['coeff_lu_fma_count']=4
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-before,4096)
        args=(17,3,3,'direct','tree',True,True)
        old=work(*args,lu_reciprocal_share=True,rhs_fma=True,rhs_upper=True)
        new=work(*args,lu_reciprocal_share=True,rhs_fma=True,rhs_upper=True,lu_fma=True)
        self.assertEqual(old['issues']['shared_mul']-new['issues']['shared_mul'],510)
        self.assertEqual(old['issues']['shared_add']-new['issues']['shared_add'],510)
        self.assertEqual(new['issues']['shared_lu_fma'],510)

    def test_cluster_cannot_borrow_rhs_or_other_pool_lanes(self):
        cfg={'--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4','--coeff-cluster-div-lat':'12',
             '--coeff-lu-fma-count':'4'}
        row=dict(pool='lu_fma',caller='15',cycle='10',complete_cycle='15',lanes='4')
        self.assertEqual(validate_cluster_trace([row],cfg)['shared_lu_fma'],4)
        for values in [dict(caller='4'),dict(lanes='5'),dict(complete_cycle='14')]:
            with self.subTest(values=values),self.assertRaises(ValueError):validate_cluster_trace([dict(row,**values)],cfg)
