"""Two independent chains must use the actual normalized operands and RF tags."""
import _support
import copy
from pathlib import Path
import subprocess
import tempfile
import unittest
from check_register_ports import _rows
from check_rhs_fma import rounded_fma,replay_rhs_fma
from check_rhs_upper_stream import replay_rhs_upper_stream,check_rhs_stream_schedule

CFG={'--coeff-rhs-upper-stream':'1','--coeff-rhs-upper-inverse':'1',
     '--coeff-rhs-fma-count':'10','--coeff-cluster-mul-lat':'3','--coeff-lu-reciprocal-share':'1'}
UPPER=dict(owners={('7','9')},physical_ids=set())


def fixture(cancel=None):
    rows=[];identity=0;values=[1.0,2.0,3.0,4.0,5.0];issued={}
    def emit(t,prefix,**r):rows.append((t,prefix,dict(cycle=t,**r)))
    emit(0,'CFD_RHS_UPPER',event='allocate',token=7,generation=9,request=11,cell=0)
    def operation(t,pool,width,reads):
        nonlocal identity
        identity+=1;i=identity;issued[i]=t;lat=3 if pool=='mul' else 5
        emit(t,'CFD_RF_ISSUE',id=i,pool=pool,caller=4,lanes=width,read_words=reads,
             ready=t+lat,capacity=30 if pool=='mul' else 50,bank_dst=-1,bank_access=0,local_result=0)
        emit(t+lat,'CFD_RF_COMPLETE',id=i,pool=pool,lanes=width)
        return i,t+lat
    def norm(t,k):
        i,end=operation(t,'mul',1,2);a=values[k];b=.5
        common=dict(rf=i,token=7,generation=9,request=11,cell=0,rhs=0,k=k,a=a.hex(),b=b.hex())
        emit(t,'CFD_RHS_NORM',event='issue',result=0.0.hex(),**common)
        values[k]=a*b
        emit(end,'CFD_RHS_NORM',event='complete',result=values[k].hex(),**common)
        return end
    def fma(t,backward,k):
        first=0 if backward else k+1;width=4-k
        i,end=operation(t,'fma',width,2*width+1)
        a=[.125*(lane+1) for lane in range(width)]
        b=[values[4-k] if backward else values[k]]*width
        c=[values[first+lane] for lane in range(width)]
        common=dict(rf=i,token=7,generation=9,request=11,cell=0,rhs=0,backward=int(backward),
                    k=k,first=first,width=width,upper_inverse=int(backward),streaming=1)
        for v,array in [('a',a),('b',b),('c',c)]:
            common.update({v+str(lane):x.hex() for lane,x in enumerate(array)})
        emit(t,'CFD_RHS_FMA',event='issue',**common,**{'r'+str(lane):0.0.hex() for lane in range(width)})
        result=[rounded_fma(a[lane],b[lane],c[lane]) for lane in range(width)]
        for lane,value in enumerate(result):values[first+lane]=value
        emit(end,'CFD_RHS_FMA',event='complete',**common,**{'r'+str(lane):x.hex() for lane,x in enumerate(result)})
        return end
    now=norm(0,0)
    for k in range(4):
        end=fma(now,False,k)
        if k:fma(now,True,4-k)
        now=norm(end,k+1)
    end=fma(now,True,0)
    emit(end,'CFD_RHS_UPPER',event='release',token=7,generation=9,request=11,cell=0)
    if cancel is not None:
        valid={i for i,t in issued.items() if t<=cancel};filtered=[]
        for t,p,r in rows:
            if p=='CFD_RHS_UPPER':
                if r['event']=='allocate':filtered.append((t,p,r))
                continue
            i=r.get('id',r.get('rf'))
            if i not in valid:continue
            if t>cancel and p in ('CFD_RHS_NORM','CFD_RHS_FMA'):
                assert r['event']=='complete';r=dict(r,event='drop')
            filtered.append((t,p,r))
        rows=filtered
        emit(cancel,'CFD_RHS_UPPER',event='cancel',token=7,generation=9,request=11,cell=0)
        emit(cancel,'CFD_RHS_UPPER',event='release',token=7,generation=9,request=11,cell=0)
    last=max(t for t,_,_ in rows)
    emit(last,'CFD_RHS_UPPER_STREAM_CAPACITY',bytes=512,rhs_state_bytes=80,
         forward_groups=4,upper_groups=4,independent_chains=2)
    emit(last,'CFD_RHS_FMA_CAPACITY',count=10,lat=5,ii=1,slots=50,bytes=9856,live=0,
         peak=0 if cancel is not None and cancel<3 else 4)
    rows.sort(key=lambda row:row[0])
    return rows


def text(rows):return ''.join(p+' '+' '.join(f'{k}={v}' for k,v in r.items())+'\n' for _,p,r in rows)
def replay(rows):
    value=text(rows);ops=list(_rows(value))
    stream=replay_rhs_upper_stream(value,CFG,ops,UPPER)
    aggregate=replay_rhs_fma(value,CFG,ops,UPPER,stream)
    return stream,aggregate


class RhsUpperStreamTests(unittest.TestCase):
    def test_native_bounded_component_preserves_bits_for_both_lu_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'stream'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
                '-ffp-contract=off','-fno-fast-math','-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'rhs_upper_stream_model.cc'),'-o',str(binary)],
                check=True,capture_output=True)
            output=subprocess.check_output([str(binary)],text=True)
            self.assertIn('rhs=220000 LU_modes=2 bitwise_same=1',output)
            self.assertIn('stream_component_cycles=40',output)

    def test_overlapping_chains_preserve_exact_physical_work(self):
        stream,total=replay(fixture())
        self.assertEqual((total['scalar_fma'],total['issue_groups'],total['complete_rhs']),(20,8,1))
        self.assertEqual((stream['summary']['norm_issue'],stream['summary']['norm_complete']),(5,5))
        self.assertEqual((total['capacity'],total['rhs_peak_scalar_slots']),(50,4))

    def test_real_norm_and_fma_returns_are_required(self):
        for pool in ['mul','fma']:
            rows=fixture();row=next(r for _,p,r in rows if p=='CFD_RF_COMPLETE' and r['pool']==pool)
            row['cycle']+=1
            with self.subTest(pool=pool),self.assertRaises(AssertionError):replay(rows)

    def test_normalization_must_use_its_actual_accumulator(self):
        rows=fixture();row=next(r for _,p,r in rows if p=='CFD_RHS_NORM' and r['event']=='issue' and r['k']==1)
        row['a']=float(99).hex()
        with self.assertRaisesRegex(AssertionError,'norm lost'):replay(rows)

    def test_bad_upper_source_or_accumulator_is_rejected(self):
        for name in ['b0','c0']:
            rows=fixture();row=next(r for _,p,r in rows if p=='CFD_RHS_FMA' and r['event']=='issue' and r['backward']==1)
            row[name]=float(99).hex()
            with self.subTest(name=name),self.assertRaises(AssertionError):replay(rows)

    def test_cancel_drains_both_fma_chains_and_pending_normalization(self):
        stream,total=replay(fixture(12));self.assertEqual(total['drop_groups'],2)
        self.assertEqual(total['complete_rhs'],0)
        stream,total=replay(fixture(1));self.assertEqual(stream['summary']['norm_drop'],1)
        self.assertEqual(total['complete_rhs'],0)

    def test_missing_or_wrong_owner_witness_is_rejected(self):
        rows=fixture();rows=[row for row in rows if not (row[1]=='CFD_RHS_NORM' and row[2]['event']=='issue' and row[2]['k']==0)]
        with self.assertRaises(AssertionError):replay(rows)
        rows=fixture();next(r for _,p,r in rows if p=='CFD_RHS_FMA')['generation']=10
        with self.assertRaises(AssertionError):replay(rows)

    def test_stream_disabled_rejects_its_transfers(self):
        rows=fixture()
        with self.assertRaises(AssertionError):replay_rhs_upper_stream(text(rows),{},list(_rows(text(rows))),UPPER)

    def test_full_pool_peak_includes_non_rhs_upper_producer(self):
        rows=fixture()
        rows.extend([(11,'CFD_RF_ISSUE',dict(cycle=11,id=999,pool='fma',caller=14,lanes=1,
                     read_words=3,ready=16,capacity=50,bank_dst=-1,bank_access=0,local_result=0)),
                     (16,'CFD_RF_COMPLETE',dict(cycle=16,id=999,pool='fma',lanes=1))])
        rows.sort(key=lambda row:row[0])
        next(r for _,p,r in rows if p=='CFD_RHS_FMA_CAPACITY')['peak']=5
        value=text(rows);ops=list(_rows(value));upper=dict(UPPER,physical_ids={'999'})
        stream=replay_rhs_upper_stream(value,CFG,ops,upper)
        result=replay_rhs_fma(value,CFG,ops,upper,stream)
        self.assertEqual((result['peak_scalar_slots'],result['rhs_peak_scalar_slots']),(5,4))

    def test_shared_reciprocal_and_paid_input_schedule_are_bound(self):
        stream,_=replay(fixture());summary=copy.deepcopy(stream['summary'])
        for name in ['reciprocal_uses','forward_uses','first_issues']:
            for use in summary[name]:use['cycle']+=100
        def row(stage,cycle,k=-1,**extra):
            return dict(stage=stage,cycle=str(cycle),k=str(k),requestId='11',generation='9',cell='0',
                        rhsBatch=extra.get('rhsBatch','-1'),rhsColumn=extra.get('rhsColumn','-1'))
        trace=[row('REQUEST_ALLOC',0),row('COEFF3_ISSUE',100),row('RHS_INPUT_INIT',100,rhsBatch='0',rhsColumn='0')]
        divider=[]
        for k in range(5):
            trace.extend([row('LU_STEP_READY',16*k,k),row('LU_RECIP_DIV_ISSUE',16*k,k),row('LU_RECIP_READY',16*k+12,k)])
            divider.append(dict(event='result',accepted=str(16*k),cycle=str(16*k+12),result='3fe0000000000000'))
        result=check_rhs_stream_schedule(CFG,trace,summary,divider)
        self.assertEqual(result['reciprocal_owners'],5)
        divider[1]['result']='3ff0000000000000'
        with self.assertRaisesRegex(AssertionError,'another LU reciprocal'):
            check_rhs_stream_schedule(CFG,trace,summary,divider)

    def test_all_capacity_ledgers_charge_control_without_adding_fus(self):
        from check_single_line_storage import lifecycle_reserve
        self.assertEqual(lifecycle_reserve({'--coeff-rhs-upper-stream':'1'})-lifecycle_reserve({}),512)
        from reporting.test_single_line_config_audit import fixture as audit_fixture
        from single_line_config_audit import Effective,capacity_audit
        resolved,_=audit_fixture(1024)
        resolved['options'].update(coeff_rhs_upper_inverse=True,coeff_rhs_fma_count=10,coeff_lu_reciprocal_share=True)
        before=capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options']['coeff_rhs_upper_stream']=True
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-before,512)
