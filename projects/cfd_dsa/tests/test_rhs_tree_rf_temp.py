"""Paid RF partial publication, ownership and cancellation protocol checks."""
import copy
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'projects/cfd_dsa/tools'))
from check_register_ports import _rows
from check_rhs_tree_rf_temp import replay_rhs_tree_rf_temp
CFG={'--coeff-rhs-tree-rf-temp':'1','--coeff-local-products':'1',
     '--coeff-rf-read-words':'24','--coeff-rf-write-words':'12',
     '--coeff-rhs-product-depth':'16','--lu5-count':'4'}
def fixture():
    def issue(identity,t,reads,bank,words):
        return ('CFD_RF_ISSUE',dict(cycle=t,id=identity,pool='add',lanes=1,caller=5,
            read_words=reads,ready=t+4,bank_access=1,bank_owner=5,bank_epoch=9,
            bank_a=bank,bank_b=-1,bank_words_a=words,bank_words_b=0,bank_dst=-1))
    def tree(t,node,event):return ('CFD_RHS_TREE',dict(cycle=t,request=1,generation=9,
        cell=0,rhs=5,k=0,node=node,event=event))
    def temp(t,event,rf):return ('CFD_RHS_TREE_RF_TEMP',dict(cycle=t,event=event,
        rf=rf,token=5,generation=9,request=1,cell=0,rhs=5,k=0,slot=0,value='0x1.8p+1'))
    return [issue(1,1,0,31,2),tree(1,1,'issue'),
        ('CFD_RF_COMPLETE',dict(cycle=5,id=1,pool='add',lanes=1,nominal=5)),
        temp(5,'write',1),tree(5,1,'complete'),issue(2,5,2,-1,0),
        temp(5,'read',2),tree(5,2,'issue'),
        ('CFD_RF_COMPLETE',dict(cycle=9,id=2,pool='add',lanes=1,nominal=9)),
        tree(9,2,'complete'),('CFD_LOCAL_VECTOR',dict(tree_rf_temp=1,
        tree_rf_temp_live=0,tree_rf_temp_peak=1,tree_rf_temp_bytes=384))]
def replay(rows,cfg=None):
    text=''.join(p+' '+' '.join(f'{k}={v}' for k,v in r.items())+'\n' for p,r in rows)
    return replay_rhs_tree_rf_temp(text,CFG if cfg is None else cfg,list(_rows(text)))
class RhsTreeRfTempTests(unittest.TestCase):
    def test_first_column_policy_covers_only_lbar_zero(self):
        cfg=dict(CFG,**{'--coeff-rhs-tree-rf-first-column':'1'})
        rows=fixture();rows[-1][1]['tree_rf_first_column']=1
        self.assertEqual(replay(rows,cfg)['writes'],1)
        for _,r in rows:
            if 'rhs' in r:r['rhs']=6
        with self.assertRaises(AssertionError):replay(rows,cfg)
        ordinary=[(p,r) for p,r in rows if p!='CFD_RHS_TREE_RF_TEMP']
        ordinary[0][1]['bank_dst']=31
        ordinary[2][1]['cycle']=6
        ordinary[3][1]['cycle']=6
        ordinary[4][1].update(cycle=6,ready=10,read_words=1,bank_a=31,bank_words_a=1)
        ordinary[5][1]['cycle']=6
        ordinary[6][1].update(cycle=10,nominal=10)
        ordinary[7][1]['cycle']=10
        ordinary[-1][1]['tree_rf_temp_peak']=0
        self.assertEqual(replay(ordinary,cfg)['writes'],0)
        with self.assertRaises(AssertionError):
            replay([],dict(cfg,**{'--coeff-rhs-tree-rf-temp':'0'}))

    def test_one_global_write_and_two_paid_consumer_reads(self):
        self.assertEqual(replay(fixture()),dict(writes=1,reads=1,clears=0,peak=1,extra_reservation_bytes=384))
        split=dict(CFG,**{'--coeff-rf-split-writeback':'1'})
        with self.assertRaises(AssertionError):replay(fixture(),split)
        rows=fixture();rows.insert(2,('CFD_RF_WRITE',dict(cycle=5,id=1,pool='add',lanes=1,offset=0,nominal=5)))
        self.assertEqual(replay(rows,split)['writes'],1)
    def test_reject_missing_rf_cost_and_local_destination(self):
        for index,key,value in [(5,'read_words',1),(0,'bank_dst',31),
                                (0,'bank_words_a',1),(5,'bank_a',31),
                                (0,'local_result',1),(5,'local_reads',1)]:
            rows=fixture();rows[index][1][key]=value
            with self.subTest(key=key,index=index):
                with self.assertRaises(AssertionError):replay(rows)
    def test_owner_generation_slot_and_payload_binding(self):
        for key,value in [('token',6),('generation',10),('rhs',6),('slot',1),
                          ('value','-0x0p+0'),('k',1)]:
            rows=fixture();rows[6][1][key]=value
            with self.subTest(key=key):
                with self.assertRaises(AssertionError):replay(rows)
    def test_actual_rf_completion_and_physical_order_required(self):
        rows=fixture();rows[2],rows[3]=rows[3],rows[2]
        with self.assertRaises(AssertionError):replay(rows)
        rows=fixture();rows[5],rows[6]=rows[6],rows[5]
        with self.assertRaises(AssertionError):replay(rows)
        with self.assertRaises(AssertionError):replay(fixture()[:2]+fixture()[3:])
    def test_missing_witness_and_duplicate_consumer_rejected(self):
        with self.assertRaises(AssertionError):replay(fixture()[:6]+fixture()[7:])
        rows=fixture();rows.insert(7,copy.deepcopy(rows[6]))
        with self.assertRaises(AssertionError):replay(rows)
    def test_abort_clears_published_scalar_and_rejects_later_read(self):
        rows=fixture();clear=copy.deepcopy(rows[3]);clear[1].update(event='clear',rf=0)
        aborted=rows[:5]+[clear,rows[-1]]
        self.assertEqual(replay(aborted)['clears'],1)
        with self.assertRaises(AssertionError):replay(rows[:5]+[clear]+rows[5:])
    def test_disabled_unbounded_and_false_peak_rejected(self):
        with self.assertRaises(AssertionError):replay(fixture(),dict(CFG,**{'--coeff-rhs-tree-rf-temp':'0'}))
        with self.assertRaises(AssertionError):replay(fixture(),dict(CFG,**{'--coeff-rf-read-words':'0'}))
        rows=fixture();rows[-1][1]['tree_rf_temp_peak']=2
        with self.assertRaises(AssertionError):replay(rows)
if __name__=='__main__':unittest.main()
