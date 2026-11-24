"""FMA arithmetic witnesses must match finite hardware and ordered RHS data."""
import copy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'projects/cfd_dsa/tools'))
from check_register_ports import _rows
from check_rhs_fma import replay_rhs_fma,rounded_fma,bits
from check_single_line_c import work,validate_cluster_trace
from check_single_line_storage import lifecycle_reserve

CFG={'--coeff-rhs-fma-count':'5'}
def fixture(drop=False):
    rows=[];values=[float(i+1) for i in range(5)];cycle=0
    order=[(0,k,k+1,4-k) for k in range(4)]+[(1,k,j,1) for k in range(3,-1,-1) for j in range(k+1,5)]
    for identity,(back,k,first,width) in enumerate(order,1):
        if not back:values[k]*=0.5
        elif identity==5:values[4]*=0.5
        a=[0.125]*width
        b=[values[first+i if back else k] for i in range(width)]
        c=[values[k if back else first+i] for i in range(width)]
        row=dict(cycle=cycle,event='issue',rf=identity,token=7,generation=9,
                 request=2,cell=0,rhs=0,backward=back,k=k,first=first,width=width)
        for i in range(width):
            for name,vector in [('a',a),('b',b),('c',c)]:row[name+str(i)]=vector[i].hex()
            row['r'+str(i)]=float(0).hex()
        rows.append(('CFD_RF_ISSUE',dict(cycle=cycle,id=identity,pool='fma',caller=4,
            lanes=width,ready=cycle+5,capacity=25,bank_dst=-1,bank_access=0,local_result=0)))
        rows.append(('CFD_RHS_FMA',row));cycle+=5
        rows.append(('CFD_RF_COMPLETE',dict(cycle=cycle,id=identity,pool='fma',lanes=width)))
        result=copy.deepcopy(row);result.update(cycle=cycle,event='drop' if drop else 'complete')
        for i in range(width):
            v=rounded_fma(a[i],b[i],c[i]);result['r'+str(i)]=v.hex()
            values[k if back else first+i]=v
        rows.append(('CFD_RHS_FMA',result))
        if drop:break
    rows.append(('CFD_RHS_FMA_CAPACITY',dict(count=5,lat=5,ii=1,slots=25,bytes=5056,live=0,peak=4)))
    return rows
def replay(rows,cfg=CFG,upper=None):
    text=''.join(p+' '+' '.join(f'{k}={v}' for k,v in r.items())+'\n' for p,r in rows)
    return replay_rhs_fma(text,cfg,list(_rows(text)),upper)

class RhsFmaTests(unittest.TestCase):
    def test_complete_solve_has_twenty_fmas_and_fourteen_groups(self):
        r=replay(fixture())
        self.assertEqual((r['scalar_fma'],r['issue_groups'],r['complete_rhs']),(20,14,1))
        self.assertEqual((r['capacity'],r['peak_scalar_slots'],r['bytes']),(25,4,5056))
    def test_single_rounding_cancellation_and_signed_zero(self):
        a=float.fromhex('0x1.0000002p0');b=float.fromhex('0x1.ffffffcp-1')
        self.assertEqual(rounded_fma(a,b,1.0),2.0**-54)
        self.assertEqual(1.0-a*b,0.0)
        self.assertEqual(bits(rounded_fma(0.0,1.0,-0.0)),bits(-0.0))
    def test_reject_non_fused_arithmetic(self):
        rows=fixture();rows[3][1]['r0']=float(123).hex()
        with self.assertRaisesRegex(AssertionError,'round once'):replay(rows)
    def test_actual_rf_return_required(self):
        for key,value in [('cycle',4),('id',999)]:
            rows=fixture();rows[2][1][key]=value
            with self.subTest(key=key):
                with self.assertRaises((AssertionError,KeyError)):replay(rows)
    def test_wrong_fu_capacity_latency_or_destination_rejected(self):
        for key,value in [('capacity',26),('ready',4),('caller',5),('bank_dst',31),('local_result',1)]:
            rows=fixture();rows[0][1][key]=value
            with self.subTest(key=key):
                with self.assertRaises(AssertionError):replay(rows)
    def test_order_and_generation_binding(self):
        for idx,key,value in [(1,'k',1),(3,'generation',10),(1,'width',3)]:
            rows=fixture();rows[idx][1][key]=value
            with self.subTest(key=key):
                with self.assertRaises((AssertionError,KeyError)):replay(rows)
    def test_no_missing_or_duplicated_witness(self):
        rows=fixture()
        with self.assertRaises(AssertionError):replay(rows[:1]+rows[2:])
        with self.assertRaises(AssertionError):replay(rows[:4]+[copy.deepcopy(rows[3])]+rows[4:])
    def test_actual_accumulator_dependency(self):
        rows=fixture();rows[5][1]['c0']=float(123).hex()
        with self.assertRaisesRegex(AssertionError,'accumulator'):replay(rows)
    def test_broadcast_cannot_change_lane_scalar(self):
        rows=fixture();rows[1][1]['b1']=float(123).hex()
        with self.assertRaisesRegex(AssertionError,'broadcast'):replay(rows)
    def test_cancel_drains_real_fma_and_disabled_has_no_work(self):
        self.assertEqual(replay(fixture(True))['drop_groups'],1)
        self.assertEqual(replay([],{'--coeff-rhs-fma-count':'0'}),{})
        with self.assertRaises(AssertionError):replay(fixture(),{'--coeff-rhs-fma-count':'0'})
    def test_capacity_ledger_and_physical_work_preserve_flops(self):
        cfg={'--coeff-cluster-mul-count':'10','--coeff-cluster-add-count':'10'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**CFG))-lifecycle_reserve(cfg),5056)
        old=work(17,3,3,'direct','tree',True,True,lu_reciprocal_share=True)
        new=work(17,3,3,'direct','tree',True,True,lu_reciprocal_share=True,rhs_fma=True)
        self.assertEqual(old['issues']['shared_mul']-new['issues']['shared_mul'],20*old['rhs'])
        self.assertEqual(old['issues']['shared_add']-new['issues']['shared_add'],20*old['rhs'])
        self.assertEqual(new['issues']['shared_fma'],20*old['rhs'])
    def test_wrong_pool_and_oversubscribed_fma_lanes(self):
        cfg=dict(CFG,**{'--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4',
            '--coeff-cluster-div-lat':'12','--coeff-cluster-div-ii':'4','--coeff-rhs-row-parallel':'1'})
        op=dict(pool='fma',cycle=1,lanes=4,caller=4,complete_cycle=6)
        self.assertEqual(validate_cluster_trace([op],cfg)['shared_fma'],4)
        with self.assertRaises(ValueError):validate_cluster_trace([op,op],cfg)
        with self.assertRaises(ValueError):validate_cluster_trace([dict(op,caller=5)],cfg)
    def test_native_contract_against_independent_gaussian_oracle(self):
        source=ROOT/'projects/cfd_dsa/tests/fixtures/rhs_fma_model.cc'
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'fma'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
                '-fno-fast-math','-ffp-contract=off','-I'+str(ROOT/'src'),str(source),'-o',str(binary)],check=True,capture_output=True)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
            self.assertIn('110000',result.stdout)

def upper_fixture():
    rows=[];values=[float(i+1) for i in range(5)];now=0
    u={(i,j):0.125 for i in range(5) for j in range(i+1,5)};v={}
    for j in range(1,5):
        for i in range(j-1,-1,-1):
            acc=u[i,j]
            for k in range(j-1,i,-1):acc=rounded_fma(u[i,k],v[k,j],acc)
            v[i,j]=acc
    order=[(0,k,k+1,4-k) for k in range(4)]+[(1,k,0,4-k) for k in range(3,-1,-1)]
    for rf,(back,k,first,width) in enumerate(order,1):
        if not back:values[k]*=0.5
        elif rf==5:values[4]*=0.5
        j=4-k
        a=[v[i,j] if back else 0.125 for i in range(width)]
        b=[values[j if back else k]]*width
        c=[values[first+i] for i in range(width)]
        op=dict(cycle=now,event='issue',rf=rf,token=7,generation=9,request=2,cell=0,rhs=0,
            backward=back,k=k,first=first,width=width,upper_inverse=back)
        for i in range(width):
            for name,vec in [('a',a),('b',b),('c',c)]:op[name+str(i)]=vec[i].hex()
            op['r'+str(i)]=float(0).hex()
        rows.append(('CFD_RF_ISSUE',dict(cycle=now,id=rf,pool='fma',caller=4,lanes=width,ready=now+5,capacity=25,bank_dst=-1,bank_access=0,local_result=0)))
        rows.append(('CFD_RHS_FMA',op));now+=5
        rows.append(('CFD_RF_COMPLETE',dict(cycle=now,id=rf,pool='fma',lanes=width)))
        result=copy.deepcopy(op);result.update(cycle=now,event='complete')
        for i in range(width):
            value=rounded_fma(a[i],b[i],c[i]);values[first+i]=value;result['r'+str(i)]=value.hex()
        rows.append(('CFD_RHS_FMA',result))
    rows.append(('CFD_RHS_FMA_CAPACITY',dict(count=5,lat=5,ii=1,slots=25,bytes=5056,live=0,peak=4)))
    return rows

class RhsSharedUpperTopologyTests(unittest.TestCase):
    def test_capacity_peak_includes_shared_precompute_fmas(self):
        rows=fixture()
        rows.insert(2,('CFD_RF_ISSUE',dict(cycle=0,id=999,pool='fma',caller=14,lanes=1,ready=5,capacity=25,bank_dst=-1,bank_access=0,local_result=0)))
        rows.insert(4,('CFD_RF_COMPLETE',dict(cycle=5,id=999,pool='fma',lanes=1)))
        rows[-1][1]['peak']=5
        owner=dict(owners=set(),physical_ids={'999'})
        result=replay(rows,upper=owner)
        self.assertEqual((result['peak_scalar_slots'],result['rhs_peak_scalar_slots']),(5,4))
        rows[-1][1]['peak']=4
        with self.assertRaises(AssertionError):replay(rows,upper=owner)
    def test_four_backward_groups_preserve_twenty_scalar_fmas(self):
        owner=dict(owners={('7','9')},physical_ids=set())
        result=replay(upper_fixture(),upper=owner)
        self.assertEqual((result['scalar_fma'],result['issue_groups'],result['complete_rhs']),(20,8,1))
        with self.assertRaises(AssertionError):replay(upper_fixture())
    def test_broadcast_source_and_row_accumulators_stay_live(self):
        owner=dict(owners={('7','9')},physical_ids=set())
        for key in ('b0','c0'):
            rows=upper_fixture();target=next(r for p,r in rows if p=='CFD_RHS_FMA' and r['event']=='issue' and r['backward']==1 and r['width']==2)
            target[key]=float(123).hex()
            with self.subTest(key=key):
                with self.assertRaises(AssertionError):replay(rows,upper=owner)

if __name__=='__main__':unittest.main()
