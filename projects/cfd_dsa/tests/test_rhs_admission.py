"""A pending critical base keeps one existing context without adding capacity."""
import _support
import unittest
from check_rhs_admission import check_rhs_admission
from check_single_line_storage import lifecycle_reserve


def fixture():
    text=('CFD_RHS_ADMISSION cycle=363 event=hold token=8 generation=9 request=7 cell=6 '
          'protected_token=2 protected_generation=9 protected_request=1 protected_cell=0 '
          'active=5 capacity=6 bytes=256')
    cfg={'--coeff-rhs-frontier-reserve':'1','--coeff3-count':'6'}
    trace=[dict(stage='REQUEST_ALLOC',requestId='1',cycle='100',generation='9',cell='0'),
           dict(stage='REQUEST_ALLOC',requestId='7',cycle='200',generation='9',cell='6'),
           dict(stage='COEFF3_ISSUE',requestId='1',cycle='371'),
           dict(stage='DIRECT_BASE_READY',requestId='1',cycle='426')]
    return text,cfg,trace


class RhsAdmissionTests(unittest.TestCase):
    def test_older_parked_tail_can_wait_after_its_base_has_completed(self):
        text,cfg,trace=fixture()
        text=(text.replace('request=7 cell=6','request=1 cell=0')
                  .replace('protected_request=1 protected_cell=0','protected_request=7 protected_cell=6'))
        trace=[dict(r,cycle='350') if r['stage']=='DIRECT_BASE_READY' else r for r in trace]
        result=check_rhs_admission(text,cfg,trace)
        self.assertEqual(result['holds'],1)

    def test_hold_preserves_one_of_the_existing_six_contexts(self):
        result=check_rhs_admission(*fixture())
        self.assertEqual((result['holds'],result['physical_contexts'],result['added_contexts']),(1,6,0))

    def test_completed_or_already_admitted_base_cannot_hold_a_credit(self):
        text,cfg,trace=fixture()
        for event in ['COEFF3_ISSUE','DIRECT_BASE_READY']:
            changed=[dict(r,cycle='350') if r['stage']==event else r for r in trace]
            with self.subTest(event=event),self.assertRaises(AssertionError):check_rhs_admission(text,cfg,changed)
        with self.assertRaises(AssertionError):check_rhs_admission(text,cfg,trace+[dict(stage='REQUEST_FAILED',requestId='1',cycle='350')])

    def test_disabled_wrong_owner_or_overreserved_capacity_rejected(self):
        text,cfg,trace=fixture()
        for before,after in [('active=5','active=4'),('bytes=256','bytes=0'),
                             ('protected_cell=0','protected_cell=3')]:
            with self.subTest(before=before),self.assertRaises(AssertionError):check_rhs_admission(text.replace(before,after),cfg,trace)
        with self.assertRaises(AssertionError):check_rhs_admission(text,{},trace)

    def test_all_capacity_ledgers_charge_the_bounded_control(self):
        before=lifecycle_reserve({})
        self.assertEqual(lifecycle_reserve({'--coeff-rhs-frontier-reserve':'1'})-before,256)
        from reporting.test_single_line_config_audit import fixture as audit_fixture
        from single_line_config_audit import Effective,capacity_audit
        resolved,_=audit_fixture(1024);before=capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options']['coeff_rhs_frontier_reserve']=True
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-before,256)
