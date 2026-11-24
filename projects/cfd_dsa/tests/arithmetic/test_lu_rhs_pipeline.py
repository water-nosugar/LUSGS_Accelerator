"""Finite LU lookahead and temporal LU factor retention, including negatives."""
import _support
from pathlib import Path
import subprocess,tempfile,unittest
from check_register_ports import replay_register_ports
from check_lu_rhs_pipeline import replay_rhs_factors,replay_lu_products


def factor_fixture():
    cfg={'--coeff-rhs-factor-reuse':'1','--coeff-local-products':'1',
         '--coeff-rhs-product-depth':'32','--coeff-rf-read-words':'16',
         '--coeff-rf-write-words':'8','--coeff-rf-broadcast':'1',
         '--coeff-rf-split-writeback':'1','--coeff-preprocess-broadcast':'1',
         '--coeff-cluster-mul-count':'10','--coeff-cluster-add-count':'10',
         '--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4',
         '--coeff-cluster-div-lat':'12','--coeff-cluster-div-ii':'4'}
    common='cache=1 slot=0 bank=63 token=7 generation=9 request=11 cell=0 backward=0 k=0 width=4'
    values='v0=0x0p+0 v1=0x1p+0 v2=0x1p+1 v3=0x1.8p+1 v4=0x1p+2'
    text=f'''CFD_RF_ISSUE cycle=0 id=1 pool=mul lanes=4 caller=4 read_words=5 ready=3 capacity=30 broadcast=1 bank_access=1 bank_a=-1 bank_b=-1 bank_dst=31 bank_words_a=0 bank_words_b=0 bank_owner=7 bank_epoch=9 bank_route=1 factor_id=0
CFD_RHS_FACTOR cycle=0 event=fill {common} rhs=0 id=1 {values}
CFD_RF_ISSUE cycle=1 id=2 pool=mul lanes=4 caller=4 read_words=1 ready=4 capacity=30 broadcast=1 bank_access=1 bank_a=63 bank_b=-1 bank_dst=32 bank_words_a=4 bank_words_b=0 bank_owner=7 bank_epoch=9 bank_route=1 factor_id=1
CFD_RHS_FACTOR cycle=1 event=use {common} rhs=5 id=2 {values}
CFD_LOCAL_WRITE cycle=3 id=1 bank=31 words=4 owner=7 epoch=9
CFD_RF_COMPLETE cycle=4 id=1 pool=mul lanes=4 nominal=3
CFD_LOCAL_WRITE cycle=4 id=2 bank=32 words=4 owner=7 epoch=9
CFD_RF_COMPLETE cycle=5 id=2 pool=mul lanes=4 nominal=4
CFD_RHS_FACTOR cycle=5 event=clear {common} rhs=0 id=0 {values}'''
    cluster=[dict(cycle=str(c),pool='mul',lanes='4',caller='4',complete_cycle=str(c+3)) for c in (0,1)]
    return text,cfg,cluster


def lu_fixture():
    _,cfg,_=factor_fixture()
    cfg.update({'--coeff-rhs-factor-reuse':'0','--coeff-lu-product-ahead':'1',
                '--coeff-preprocess-broadcast':'0'})
    events=[];cluster=[]
    for identity,mul,k,start,end,bank in [(1,True,0,0,4,27),(2,True,1,1,5,63),
                                         (3,False,0,4,8,27),(4,False,1,8,12,63)]:
        pool,caller=('mul','1') if mul else ('add','2')
        nominal=start+(3 if mul else 4)
        access=f'bank_a=-1 bank_b=-1 bank_dst={bank} bank_words_a=0 bank_words_b=0' if mul else f'bank_a={bank} bank_b=-1 bank_dst=-1 bank_words_a=3 bank_words_b=0'
        common=f'token=7 generation=9 column=2 upper=0 k={k} slot={k%2}'
        events.append((start, f'CFD_RF_ISSUE cycle={start} id={identity} pool={pool} lanes=3 caller={caller} read_words={6 if mul else 3} ready={nominal} capacity={30 if mul else 40} broadcast=0 bank_access=1 {access} bank_owner=7 bank_epoch=9 bank_route={1 if mul else 0}'))
        events.append((start,f'CFD_LU_PRODUCTS cycle={start} event={"mul" if mul else "sub"}-issue {common} id={identity} bank={bank}'))
        if mul:events.append((nominal,f'CFD_LOCAL_WRITE cycle={nominal} id={identity} bank={bank} words=3 owner=7 epoch=9'))
        else:events.append((nominal,f'CFD_RF_WRITE cycle={nominal} id={identity} pool=add lanes=3 offset=0 nominal={nominal}'))
        events.append((end,f'CFD_RF_COMPLETE cycle={end} id={identity} pool={pool} lanes=3 nominal={nominal}'))
        events.append((end,f'CFD_LU_PRODUCTS cycle={end} event={"mul" if mul else "sub"}-ready {common}'))
        cluster.append(dict(cycle=str(start),pool=pool,lanes='3',caller=caller,complete_cycle=str(nominal)))
    # All physical returns precede new issues, including callbacks whose
    # original generating operation was appended later in the fixture.
    def order(entry):
        cycle,line=entry
        return cycle, int('ISSUE ' in line or '-issue ' in line)
    events.sort(key=order)
    return '\n'.join(line for _,line in events),cfg,cluster


class LuRhsPipelineTests(unittest.TestCase):
    def test_native_overlap_bit_order_capacity_routes_and_stale_identities(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'test'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra',
                '-I'+str(_support.REPO_ROOT/'src'),str(_support.FIXTURES/'lu_rhs_pipeline_model.cc'),
                '-o',str(binary)],check=True,capture_output=True)
            self.assertIn('LU_RHS_PIPELINE_MODEL_PASS cycles=20',subprocess.check_output([str(binary)],text=True))

    def test_first_use_pays_rf_and_later_use_pays_registered_local_access(self):
        text,cfg,cluster=factor_fixture();r=replay_register_ports(text,cfg,cluster)
        self.assertEqual(r.summary['read_words'],6)
        self.assertEqual(r.summary['bank_read_words'],4)
        self.assertEqual(r.summary['bank_write_words'],12)
        self.assertEqual(r.summary['rhs_factors']['saved_read_words'],4)
        self.assertEqual(r.summary['rhs_factors']['lbar_hits'],1)

    def test_reject_unpaid_early_stale_missing_or_wrong_factor_sources(self):
        text,cfg,cluster=factor_fixture()
        changes=[('event=fill','event=use'),('event=use','event=fill'),
                 ('read_words=5 ready=3','read_words=1 ready=3'),
                 ('cycle=1','cycle=0'),('factor_id=1','factor_id=2'),
                 ('event=use cache=1','event=use cache=2'),
                 ('bank_a=63','bank_a=64'),('bank_words_a=4','bank_words_a=3'),
                 ('event=use cache=1 slot=0 bank=63 token=7 generation=9',
                  'event=use cache=1 slot=0 bank=63 token=7 generation=10')]
        for before,after in changes:
            with self.subTest(before=before),self.assertRaises((AssertionError,KeyError)):
                replay_register_ports(text.replace(before,after),cfg,cluster)
        with self.assertRaises(AssertionError):
            replay_register_ports(text,dict(cfg,**{'--coeff-rhs-factor-reuse':'0'}),cluster)

    def test_unbound_cached_read_is_rejected(self):
        text,cfg,cluster=factor_fixture()
        text='\n'.join(line for line in text.splitlines() if 'event=use' not in line)
        with self.assertRaisesRegex(AssertionError,'binding'):
            replay_register_ports(text,cfg,cluster)

    def test_expanded_factor_banks_remain_bounded_and_paid(self):
        text,cfg,cluster=factor_fixture()
        for depth in (4,8):
            expanded=text.replace('slot=0 bank=63',f'slot={depth-1} bank={62+depth}').replace(
                'bank_a=63',f'bank_a={62+depth}')
            r=replay_register_ports(expanded,dict(cfg,**{'--coeff-rhs-factor-slots':str(depth)}),cluster)
            self.assertEqual(r.summary['read_words'],6)
            self.assertEqual(r.summary['rhs_factors']['saved_read_words'],4)
            self.assertEqual(r.summary['bank_count'],63+depth)
            with self.assertRaises(AssertionError):
                replay_register_ports(expanded,cfg,cluster)
        with self.assertRaisesRegex(AssertionError,'capacity'):
            replay_register_ports(text,dict(cfg,**{'--coeff-rhs-factor-slots':'3'}),cluster)

    def test_factor_capacity_ledgers_scale_without_extra_rf_ports(self):
        from reporting.test_single_line_config_audit import fixture
        from single_line_config_audit import Effective,capacity_audit
        from check_single_line_storage import lifecycle_reserve
        resolved,_=fixture(1024)
        resolved['options'].update(coeff_line_cache_allowed=False,coeff_rhs_factor_reuse=True)
        base=capacity_audit(Effective(resolved))['reservation_bytes']
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0',
             '--coeff-rhs-factor-reuse':'1'}
        for depth in (2,4,8):
            resolved['options']['coeff_rhs_factor_slots']=depth
            self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-base,128*(depth-2))
            self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-rhs-factor-slots':str(depth)}))-
                             lifecycle_reserve(cfg),128*(depth-2))

    def test_lu_product_groups_overlap_with_ordered_actual_subtraction(self):
        text,cfg,cluster=lu_fixture();r=replay_register_ports(text,cfg,cluster)
        self.assertEqual(r.summary['lu_products'],{'mul-issue':2,'mul-ready':2,'sub-issue':2,'sub-ready':2})
        self.assertEqual(r.summary['read_words'],18)

    def test_lu_replay_rejects_early_consumption_wrong_slot_and_duplicate_group(self):
        text,cfg,_=lu_fixture()
        for before,after in [('event=sub-issue token=7 generation=9 column=2 upper=0 k=0 slot=0',
                              'event=sub-issue token=7 generation=9 column=2 upper=0 k=1 slot=1'),
                             ('k=1 slot=1 id=2 bank=63','k=1 slot=0 id=2 bank=27'),
                             ('cycle=4 event=mul-ready','cycle=3 event=mul-ready'),
                             ('event=mul-ready','event=sub-ready')]:
            with self.subTest(before=before),self.assertRaises((AssertionError,KeyError,TypeError)):
                replay_lu_products(text.replace(before,after),cfg)

    def test_lu_missing_last_callback_needs_explicit_cancellation_and_real_return(self):
        text,cfg,_=lu_fixture()
        text='\n'.join(line for line in text.splitlines() if 'cycle=12 event=sub-ready' not in line)
        with self.assertRaisesRegex(AssertionError,'did not drain'):
            replay_lu_products(text,cfg)
        cancelled=text+'\nCFD_LU_PRODUCTS_CANCEL cycle=11 token=7 generation=9'
        replay_lu_products(cancelled,cfg)
        missing='\n'.join(line for line in cancelled.splitlines() if 'CFD_RF_COMPLETE cycle=12' not in line)
        with self.assertRaisesRegex(AssertionError,'physically drain'):
            replay_lu_products(missing,cfg)

    def test_independent_capacity_ledgers_charge_both_structures(self):
        from reporting.test_single_line_config_audit import fixture
        from single_line_config_audit import Effective,capacity_audit
        from check_single_line_storage import lifecycle_reserve
        resolved,_=fixture(1024);resolved['options']['coeff_line_cache_allowed']=False
        base=capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options'].update(coeff_lu_product_ahead=True,coeff_rhs_factor_reuse=True,lu5_count=4)
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-base,1152)
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0','--lu5-count':'4'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-lu-product-ahead':'1','--coeff-rhs-factor-reuse':'1'}))-lifecycle_reserve(cfg),1152)
