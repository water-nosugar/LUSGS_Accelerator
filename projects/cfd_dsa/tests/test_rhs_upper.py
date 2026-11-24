"""Shared upper transforms conserve arithmetic, RF publication and owners."""
import copy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'projects/cfd_dsa/tools'))
from check_register_ports import _rows
from check_rhs_upper import COORDS,INDEX,replay_rhs_upper,check_rhs_upper_schedule
from check_rhs_fma import rounded_fma
from check_single_line_c import work,validate_cluster_trace
from check_single_line_storage import lifecycle_reserve

CFG={'--coeff-rhs-upper-inverse':'1','--coeff-rhs-upper-slots':'16','--coeff-rhs-fma-count':'10'}
def fixture(cancel=False,consumer=False):
    rows=[];done=0;pending=0;now=0;values={};lu={(i,j):0.125 for i in range(5) for j in range(i+1,5)}
    def upper(event,rf=0,e=-1,k=-1,a=0.,b=0.,c=0.,result=0.,cancelled=0):
        return ('CFD_RHS_UPPER',dict(cycle=now,event=event,id=1,slot=0,token=7,generation=9,request=2,cell=0,
            rf=rf,entry=e,i=-1 if e<0 else COORDS[e][0],j=-1 if e<0 else COORDS[e][1],k=k,
            done=done,pending=pending,cancelled=cancelled,a=float(a).hex(),b=float(b).hex(),c=float(c).hex(),result=float(result).hex()))
    rows.append(upper('allocate'));rf=0
    for e in (0,3,1,5,4,2):
        i,j=COORDS[e];acc=lu[i,j]
        for k in range(j-1,i,-1):
            rf+=1;pending=1<<e;a=lu[i,k];b=lu[k,j] if k+1==j else values[INDEX[k,j]];c=acc
            rows.append(('CFD_RF_ISSUE',dict(cycle=now,id=rf,pool='fma',caller=14,lanes=1,read_words=3,ready=now+5,capacity=50,
                broadcast=0,bank_access=0,local_result=0,bank_dst=-1,local_reads=0)))
            rows.append(upper('issue',rf,e,k,a,b,c))
            if cancel:
                now+=1;rows.append(upper('cancel',cancelled=1));now+=4
                rows.append(('CFD_RF_COMPLETE',dict(cycle=now,id=rf,pool='fma',lanes=1)))
                pending=0;rows.append(upper('drop',rf,e,k,a,b,c,rounded_fma(a,b,c),1))
                rows.append(upper('release',cancelled=1))
                rows.append(('CFD_RHS_UPPER_CAPACITY',dict(slots=16,bytes=4352,live=0,peak=1,issued=1,completed=0,dropped=1,issue_width=6)))
                return rows
            now+=5;acc=rounded_fma(a,b,c);pending=0
            rows.append(('CFD_RF_COMPLETE',dict(cycle=now,id=rf,pool='fma',lanes=1)))
            if k==i+1:done|=1<<e
            rows.append(upper('complete',rf,e,k,a,b,c,acc));values[e]=acc
    if consumer:
        op=dict(cycle=now,event='issue',rf=99,token=7,generation=9,request=2,cell=0,rhs=0,backward=1,k=2,first=0,width=2,upper_inverse=1,
            a0=values[INDEX[0,2]].hex(),a1=lu[1,2].hex(),b0=float(1).hex(),b1=float(1).hex(),c0=float(2).hex(),c1=float(3).hex())
        rows.append(('CFD_RHS_FMA',op));now+=5
        rows.append(('CFD_RHS_FMA',dict(op,cycle=now,event='complete')))
    rows.append(upper('release'))
    rows.append(('CFD_RHS_UPPER_CAPACITY',dict(slots=16,bytes=4352,live=0,peak=1,issued=10,completed=10,dropped=0,issue_width=6)))
    return rows
def replay(rows,cfg=CFG):
    text=''.join(p+' '+' '.join(f'{k}={v}' for k,v in r.items())+'\n' for p,r in rows)
    return replay_rhs_upper(text,cfg,list(_rows(text)))

class RhsUpperTests(unittest.TestCase):
    def test_ten_paid_fmas_and_compact_owner_lifecycle(self):
        r=replay(fixture())
        self.assertEqual(r['summary']['issue'],10);self.assertEqual(r['summary']['complete'],10)
        self.assertEqual(r['summary']['bytes'],4352);self.assertEqual(r['summary']['peak'],1)
        self.assertEqual(r['owners'],{('7','9')})
    def test_real_rf_return_required(self):
        rows=fixture();rows[3][1]['cycle']=4
        with self.assertRaises(AssertionError):replay(rows)
        rows=fixture();rows.pop(3)
        with self.assertRaises(KeyError):replay(rows)
    def test_three_paid_inputs_capacity_and_destination(self):
        for key,value in [('read_words',2),('caller',4),('capacity',51),('ready',4),('bank_access',1),('bank_dst',31)]:
            rows=fixture();rows[1][1][key]=value
            with self.subTest(key=key):
                with self.assertRaises((AssertionError,KeyError)):replay(rows)
    def test_accumulator_and_prior_inverse_dependency(self):
        rows=fixture();target=next(r for p,r in rows if p=='CFD_RHS_UPPER' and r['event']=='issue' and r['k']!=r['j']-1)
        target['c']=float(123).hex()
        with self.assertRaisesRegex(AssertionError,'accumulator'):replay(rows)
        rows=fixture();target=next(r for p,r in rows if p=='CFD_RHS_UPPER' and r['event']=='issue' and r['k']+1<r['j'])
        target['b']=float(123).hex()
        with self.assertRaisesRegex(AssertionError,'dependency'):replay(rows)
    def test_owner_generation_slot_and_pending_masks(self):
        for key,value in [('generation',10),('slot',1),('id',2),('pending',2),('entry',2)]:
            rows=fixture();rows[4][1][key]=value
            with self.subTest(key=key):
                with self.assertRaises((AssertionError,KeyError)):replay(rows)
    def test_round_once_and_lu_source_consistency(self):
        rows=fixture();rows[4][1]['result']=float(123).hex()
        with self.assertRaisesRegex(AssertionError,'round once'):replay(rows)
        rows=fixture();rows[6][1]['a']=float(123).hex()
        with self.assertRaises(AssertionError):replay(rows)
    def test_cancel_retains_slot_until_actual_rf_drop(self):
        r=replay(fixture(True))['summary']
        self.assertEqual((r['issue'],r['drop'],r['releases']),(1,1,1))
        rows=fixture(True);rows.insert(4,copy.deepcopy(rows[-2]))
        with self.assertRaises(AssertionError):replay(rows)
    def test_derived_operand_is_bound_to_shared_owner(self):
        self.assertEqual(replay(fixture(consumer=True))['summary']['complete'],10)
        rows=fixture(consumer=True);next(r for p,r in rows if p=='CFD_RHS_FMA')['a0']=float(123).hex()
        with self.assertRaisesRegex(AssertionError,'transform'):replay(rows)
    def test_disabled_or_wrong_capacity_rejected(self):
        self.assertEqual(replay([],{'--coeff-rhs-upper-inverse':'0'})['summary'],{})
        with self.assertRaises(AssertionError):replay(fixture(),{'--coeff-rhs-upper-inverse':'0'})
        rows=fixture();rows[-1][1]['bytes']=0
        with self.assertRaises(AssertionError):replay(rows)
    def test_sources_need_original_lu_row_publication(self):
        r=replay(fixture())['summary']
        trace=[dict(stage='LU_COLUMN_DONE',generation='9',requestId='2',cell='0',k=str(i),cycle='0') for i in range(4)]
        self.assertEqual(check_rhs_upper_schedule(CFG,trace,r)['paid_source_rows'],4)
        trace[1]['cycle']='99'
        with self.assertRaises(AssertionError):check_rhs_upper_schedule(CFG,trace,r)
    def test_capacity_and_physical_work_independent_of_reuse_sweeps(self):
        cfg={'--coeff-cluster-mul-count':'10','--coeff-cluster-add-count':'10'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**CFG))-lifecycle_reserve(cfg),4352+9856)
        a=work(17,3,3,'direct','tree',True,True,rhs_fma=True)
        b=work(17,3,3,'direct','tree',True,True,rhs_fma=True,rhs_upper=True)
        self.assertEqual(b['issues']['shared_fma']-a['issues']['shared_fma'],170)
        one=work(1,1,1,'direct','tree',True,True,rhs_fma=True,rhs_upper=True)
        self.assertEqual(one['issues']['shared_fma'],20)
    def test_precompute_caller_uses_real_fma_pool(self):
        cfg=dict(CFG,**{'--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4','--coeff-cluster-div-lat':'12','--coeff-cluster-div-ii':'4'})
        row=dict(cycle=0,pool='fma',caller=14,lanes=1,complete_cycle=5)
        self.assertEqual(validate_cluster_trace([row],cfg)['shared_fma'],1)
        with self.assertRaises(ValueError):validate_cluster_trace([row],dict(cfg,**{'--coeff-rhs-upper-inverse':'0'}))
    def test_actual_native_helper_with_independent_gaussian_oracle(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'upper'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-fno-fast-math','-ffp-contract=off',
                '-I'+str(ROOT/'src'),str(ROOT/'projects/cfd_dsa/tests/fixtures/rhs_upper_model.cc'),'-o',str(binary)],check=True,capture_output=True)
            r=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
            self.assertIn('rhs=110000',r.stdout)

if __name__=='__main__':unittest.main()
