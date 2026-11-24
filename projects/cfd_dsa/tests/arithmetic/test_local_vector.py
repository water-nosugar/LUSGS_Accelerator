"""Bounded local storage, routing and exact dual-vector physical contracts."""
import _support
from pathlib import Path
import subprocess,tempfile,unittest
from check_register_ports import replay_register_ports
from check_single_line_c import validate_cluster_trace

class LocalVectorTests(unittest.TestCase):
    def test_native_bank_ports_and_tags(self):
        with tempfile.TemporaryDirectory() as d:
            binary=Path(d)/'test'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-I'+str(_support.REPO_ROOT/'src'),str(_support.FIXTURES/'local_vector_model.cc'),'-o',str(binary)],check=True,capture_output=True)
            self.assertIn('LOCAL_VECTOR_MODEL_PASS',subprocess.check_output([str(binary)],text=True))
    def test_ten_units_allow_two_vectors_but_reject_an_eleventh(self):
        cfg={'--coeff-cluster-mul-count':'10','--coeff-cluster-add-count':'10','--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4','--coeff-cluster-div-lat':'12','--coeff-cluster-div-ii':'4'}
        rows=[dict(cycle='0',pool=p,lanes='5',caller=c,complete_cycle=t) for p,c,t in [('mul','8','3'),('mul','8','3'),('add','9','4'),('add','9','4')]]
        self.assertEqual(validate_cluster_trace(rows,cfg),{'shared_mul':10,'shared_add':10})
        with self.assertRaisesRegex(ValueError,'oversubscribed'):
            validate_cluster_trace(rows+[dict(rows[0])],cfg)
    def fixture(self):
        cfg={'--coeff-vector-local':'1','--coeff-rf-read-words':'16','--coeff-rf-write-words':'8','--coeff-rf-broadcast':'1','--coeff-rf-split-writeback':'1','--coeff-cluster-mul-count':'10','--coeff-cluster-add-count':'10','--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4','--coeff-cluster-div-lat':'12','--coeff-cluster-div-ii':'4','--coeff-local-route-latency':'1'}
        text='''CFD_RF_ISSUE cycle=0 id=1 pool=mul lanes=5 caller=8 read_words=6 ready=3 capacity=30 broadcast=1 bank_access=1 bank_a=-1 bank_b=-1 bank_dst=0 bank_words_a=0 bank_words_b=0 bank_owner=1 bank_epoch=0 bank_route=1
CFD_LOCAL_WRITE cycle=3 id=1 bank=0 words=5 owner=1 epoch=0
CFD_RF_COMPLETE cycle=4 id=1 pool=mul lanes=5 nominal=3
CFD_RF_ISSUE cycle=4 id=2 pool=add lanes=5 caller=9 read_words=5 ready=8 capacity=40 broadcast=0 bank_access=1 bank_a=-1 bank_b=0 bank_dst=5 bank_words_a=0 bank_words_b=5 bank_owner=1 bank_epoch=0 bank_route=1
CFD_LOCAL_WRITE cycle=8 id=2 bank=5 words=5 owner=1 epoch=0
CFD_RF_COMPLETE cycle=9 id=2 pool=add lanes=5 nominal=8'''
        rows=[dict(cycle='0',pool='mul',lanes='5',caller='8',complete_cycle='3'),dict(cycle='4',pool='add',lanes='5',caller='9',complete_cycle='8')]
        return text,cfg,rows
    def test_local_results_pay_storage_and_registered_route(self):
        text,cfg,rows=self.fixture();r=replay_register_ports(text,cfg,rows)
        self.assertEqual(r.summary['write_words'],0)
        self.assertEqual(r.summary['bank_write_words'],10)
        self.assertEqual(r.summary['bank_read_words'],5)
        self.assertEqual(r.summary['read_words'],11)

    def hybrid_fixture(self):
        text,cfg,rows=self.fixture()
        cfg['--coeff-vector-product-only']='1'
        text=text.replace('bank_dst=5','bank_dst=-1').replace(
            'bank_epoch=0 bank_route=1\nCFD_LOCAL_WRITE cycle=8',
            'bank_epoch=0 bank_route=0\nCFD_LOCAL_WRITE cycle=8')
        text=text.replace('CFD_LOCAL_WRITE cycle=8 id=2 bank=5 words=5 owner=1 epoch=0\n',
            'CFD_RF_WRITE cycle=8 id=2 pool=add lanes=5 offset=0 nominal=8\n')
        text=text.replace('CFD_RF_COMPLETE cycle=9 id=2','CFD_RF_COMPLETE cycle=8 id=2')
        return text,cfg,rows

    def test_product_only_partials_pay_global_write_and_no_local_route(self):
        text,cfg,rows=self.hybrid_fixture()
        result=replay_register_ports(text,cfg,rows)
        self.assertEqual(result.summary['write_words'],5)
        self.assertEqual(result.summary['bank_write_words'],5)
        self.assertEqual(result.summary['bank_read_words'],5)
        self.assertEqual(result.summary['read_words'],11)

    def test_reject_product_only_destination_and_free_partial_shortcuts(self):
        text,cfg,rows=self.hybrid_fixture()
        for changed in [text.replace('bank_dst=-1 bank_words_a=0 bank_words_b=5',
                                   'bank_dst=5 bank_words_a=0 bank_words_b=5'),
                        text.replace('CFD_RF_WRITE cycle=8 id=2 pool=add lanes=5 offset=0 nominal=8\n',''),
                        text.replace('bank_b=0','bank_b=5'),
                        text.replace('read_words=5 ready=8','read_words=0 ready=8')]:
            with self.assertRaises(AssertionError):replay_register_ports(changed,cfg,rows)

    def test_product_only_requires_local_vector_storage(self):
        text,cfg,rows=self.hybrid_fixture();cfg['--coeff-vector-local']='0'
        with self.assertRaisesRegex(AssertionError,'lacks local storage'):
            replay_register_ports(text,cfg,rows)

    def test_product_only_final_subtraction_cannot_read_local_partial(self):
        text,cfg,rows=self.hybrid_fixture()
        text+='''
CFD_RF_ISSUE cycle=8 id=3 pool=add lanes=5 caller=7 read_words=10 ready=12 capacity=40 broadcast=0 bank_access=1 bank_a=-1 bank_b=-1 bank_dst=-1 bank_words_a=0 bank_words_b=0 bank_owner=1 bank_epoch=0 bank_route=0
CFD_RF_WRITE cycle=12 id=3 pool=add lanes=5 offset=0 nominal=12
CFD_RF_COMPLETE cycle=12 id=3 pool=add lanes=5 nominal=12'''
        rows.append(dict(cycle='8',pool='add',lanes='5',caller='7',complete_cycle='12'))
        result=replay_register_ports(text,cfg,rows)
        self.assertEqual(result.summary['read_words'],21)
        changed=text.replace('caller=7 read_words=10','caller=7 read_words=5').replace(
            'bank_a=-1 bank_b=-1 bank_dst=-1 bank_words_a=0 bank_words_b=0',
            'bank_a=-1 bank_b=0 bank_dst=-1 bank_words_a=0 bank_words_b=5')
        with self.assertRaisesRegex(AssertionError,'final subtraction'):
            replay_register_ports(changed,cfg,rows)

    def test_local_fu_credit_releases_after_storage_before_publication(self):
        text,cfg,rows=self.fixture();cfg['--coeff-local-credit-release']='1'
        text=text.replace('CFD_RF_COMPLETE cycle=4',
            'CFD_RF_RELEASE cycle=3 id=1 pool=mul lanes=5 nominal=3\nCFD_RF_COMPLETE cycle=4')
        text=text.replace('CFD_RF_COMPLETE cycle=9',
            'CFD_RF_RELEASE cycle=8 id=2 pool=add lanes=5 nominal=8\nCFD_RF_COMPLETE cycle=9')
        replay_register_ports(text,cfg,rows)
        for changed in [text.replace('CFD_RF_RELEASE cycle=3','CFD_RF_RELEASE cycle=2'),
                        text.replace('CFD_RF_RELEASE cycle=3 id=1 pool=mul lanes=5 nominal=3\n',''),
                        text.replace('CFD_RF_COMPLETE cycle=4','CFD_RF_COMPLETE cycle=3')]:
            with self.assertRaises(AssertionError):replay_register_ports(changed,cfg,rows)
    def test_reject_free_local_data_and_owner_route_shortcuts(self):
        text,cfg,rows=self.fixture()
        for changed in [text.replace('bank_b=0','bank_b=1'),text.replace('cycle=9 id=2','cycle=8 id=2'),text.replace('bank=5 words=5 owner=1','bank=5 words=5 owner=2'),text.replace('read_words=5 ready=8','read_words=4 ready=8')]:
            with self.assertRaises(AssertionError):replay_register_ports(changed,cfg,rows)

    def test_local_capacity_and_expanded_pipelines_are_paid(self):
        from reporting.test_single_line_config_audit import fixture
        from single_line_config_audit import Effective,capacity_audit
        resolved,_=fixture(1024)
        resolved['options']['coeff_line_cache_allowed']=False
        base=capacity_audit(Effective(resolved))
        resolved['options'].update(coeff_vector_local=True,coeff_local_products=True,lu5_count=4)
        new=capacity_audit(Effective(resolved))
        self.assertEqual(new['reservation_bytes']-base['reservation_bytes'],6912)
        resolved['options']['coeff_vector_product_only']=True
        hybrid=capacity_audit(Effective(resolved))
        self.assertEqual(hybrid['reservation_bytes'],new['reservation_bytes'])
        resolved['options'].update(coeff_cluster_mul_count=10,coeff_cluster_add_count=10)
        expanded=capacity_audit(Effective(resolved))
        self.assertEqual(expanded['reservation_bytes']-new['reservation_bytes'],1280)

    def test_storage_ledger_independently_pays_local_state(self):
        from check_single_line_storage import lifecycle_reserve
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0'}
        base=lifecycle_reserve(cfg)
        cfg.update({'--coeff-vector-local':'1','--coeff-local-products':'1','--lu5-count':'4'})
        self.assertEqual(lifecycle_reserve(cfg)-base,6912)
