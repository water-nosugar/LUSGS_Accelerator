import copy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'projects/cfd_dsa/tools'))
from check_lu_rhs_pipeline import replay_rhs_factors
from check_register_ports import _rows
from check_rhs_factor_fanout import replay_rhs_factor_fanout


CONFIG = {'--coeff-rhs-factor-reuse':'1', '--coeff-rhs-factor-fanout':'1',
          '--coeff-local-products':'1', '--coeff-rhs-pair':'0',
          '--coeff-rf-read-words':'16', '--coeff-rf-write-words':'12',
          '--coeff-rhs-factor-slots':'8', '--lu5-count':'4',
          '--coeff-rhs-product-depth':'16', '--coeff-lu-product-ahead':'1'}
PAYLOAD = {f'v{i}':v for i,v in enumerate(['0x0p+0','0x1p+0','0x1p+1','0x1.8p+1','0x1p+2'])}


def fixture():
    def rf(identity, cycle, reads, bank=-1, factor=0, source=0, words=0):
        return ('CFD_RF_ISSUE', dict(cycle=cycle,id=identity,pool='mul',lanes=4,caller=4,
            read_words=reads,ready=cycle+3,capacity=30,broadcast=1,local_reads=0,
            local_result=0,row=0,bank_access=1,bank_a=bank,bank_b=-1,
            bank_dst=30+identity,bank_words_a=4 if bank>=0 else 0,bank_words_b=0,
            bank_owner=5,bank_epoch=9,bank_route=1,pair_id=0,pair_side=0,
            factor_id=factor,factor_source=source,factor_words=words))
    def factor(event, identity, cycle, rhs):
        return ('CFD_RHS_FACTOR', dict(cycle=cycle,event=event,cache=1,slot=0,bank=51,
            token=5,generation=9,request=1,cell=0,backward=0,k=0,width=4,
            rhs=rhs,id=identity,**PAYLOAD))
    def wire(event, identity, rhs):
        return ('CFD_RHS_FACTOR_FANOUT', dict(cycle=1,event=event,source=2,id=identity,
            cache=1,slot=0,bank=51,token=5,generation=9,request=1,cell=0,
            backward=0,k=0,width=4,rhs=rhs,**PAYLOAD))
    return [rf(1,0,5),factor('fill',1,0,5),rf(2,1,1,51,1),
            factor('use',2,1,6),wire('offer',2,6),rf(3,1,1,-1,1,2,4),wire('use',3,7)]


def render(records):
    return ''.join(prefix+' '+' '.join(f'{k}={v}' for k,v in row.items())+'\n'
                   for prefix,row in records)


def replay(records, config=None):
    config = CONFIG if config is None else config
    text = render(records)
    operations = list(_rows(text))
    factors = replay_rhs_factors(text,config,operations)
    return replay_rhs_factor_fanout(text,config,operations,factors), factors


class FactorFanoutTests(unittest.TestCase):
    def test_native_protocol_and_all_depth_direction_payloads(self):
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/'factor-model'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
                '-fno-fast-math','-ffp-contract=off','-I',str(ROOT/'src'),
                str(ROOT/'projects/cfd_dsa/tests/fixtures/factor_fanout_model.cc'),
                '-o',str(output)],check=True,capture_output=True,text=True)
            result=subprocess.run([str(output)],check=True,capture_output=True,text=True)
            self.assertIn('10000 exact payloads',result.stdout)
            self.assertIn('saved_words=25000',result.stdout)

    def test_one_paid_read_two_receivers(self):
        wire,factors=replay(fixture())
        self.assertEqual(wire['summary'],{'offers':1,'uses':1,'saved_read_words':4})
        self.assertEqual(factors['summary']['hits'],1)
        self.assertEqual(set(factors['uses']),{'2'})
        self.assertEqual(set(wire['uses']),{'3'})
        self.assertEqual(sum(int(row['read_words']) for prefix,row in fixture()
                             if prefix=='CFD_RF_ISSUE'),7)

    def test_disabled_bus_rejects_free_operand_metadata(self):
        with self.assertRaises(AssertionError):replay(fixture(),dict(CONFIG,**{'--coeff-rhs-factor-fanout':'0'}))
        old=fixture()[:4]
        wire,_=replay(old,dict(CONFIG,**{'--coeff-rhs-factor-fanout':'0'}))
        self.assertEqual(wire['summary'],{})

    def test_feedback_only_bus_rejects_ubar_consumers_and_disabled_bus(self):
        limited = dict(CONFIG,**{'--coeff-rhs-factor-fanout-feedback':'1'})
        self.assertEqual(replay(fixture(),limited)[0]['summary']['uses'],1)
        rows=fixture();rows[-1][1]['rhs']=12
        self.assertEqual(replay(rows)[0]['summary']['uses'],1)
        with self.assertRaises(AssertionError):replay(rows,limited)
        disabled=dict(limited,**{'--coeff-rhs-factor-fanout':'0'})
        with self.assertRaises(AssertionError):replay(fixture()[:4],disabled)

    def test_binding_payload_and_remaining_operand_reads(self):
        changes=[(6,'generation',10),(6,'source',99),(6,'cache',2),
                 (6,'rhs',6),(6,'v0','-0x0p+0'),(4,'v1','0x1.8p+1'),
                 (5,'read_words',0),(5,'factor_words',3),(5,'bank_a',51),
                 (5,'bank_words_a',4),(5,'bank_owner',6)]
        for index,key,value in changes:
            with self.subTest(index=index,key=key):
                rows=fixture();rows[index][1][key]=value
                with self.assertRaises(AssertionError):replay(rows)

    def test_third_receiver_and_second_bus_vector_rejected(self):
        rows=fixture();third=copy.deepcopy(rows[-2:])
        third[0][1]['id']=4;third[1][1].update(id=4,rhs=8)
        with self.assertRaises(AssertionError):replay(rows+third)
        rows=fixture();rows.insert(5,copy.deepcopy(rows[4]))
        with self.assertRaises(AssertionError):replay(rows)

    def test_cache_clear_invalidates_without_refunding_cycle(self):
        clear=copy.deepcopy(fixture()[3]);clear[1].update(event='clear',id=0,rhs=0)
        rows=fixture();rows.insert(5,clear)
        with self.assertRaises(AssertionError):replay(rows)
        rows=fixture()[:5]+[clear,copy.deepcopy(fixture()[4])]
        with self.assertRaises(AssertionError):replay(rows)

    def test_source_capture_and_physical_order_are_required(self):
        rows=fixture();rows[3],rows[4]=rows[4],rows[3]
        with self.assertRaises(AssertionError):replay(rows)
        rows=fixture();rows[4],rows[5]=rows[5],rows[4]
        with self.assertRaises(AssertionError):replay(rows)
        rows=fixture();rows[3][1]['cycle']=0
        with self.assertRaises(AssertionError):replay(rows)

    def test_every_saved_rf_operand_needs_a_bus_witness(self):
        rows=fixture()[:-1]
        with self.assertRaises(AssertionError):replay(rows)
        rows=fixture();rows[-1][1]['cycle']=2
        with self.assertRaises(AssertionError):replay(rows)


if __name__=='__main__':unittest.main()
