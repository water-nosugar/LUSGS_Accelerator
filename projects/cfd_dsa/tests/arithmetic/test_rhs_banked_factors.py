"""Banked LU operand retention, independent set proof and explicit capacity."""
import _support
from pathlib import Path
import subprocess
import tempfile
import unittest
from arithmetic.test_lu_rhs_pipeline import factor_fixture
from check_register_ports import replay_register_ports
from check_lu_rhs_pipeline import replay_rhs_factors
from check_register_ports import _rows


def fixture(depth,wrong_set=False):
    text,cfg,cluster=factor_fixture();cfg['--coeff-rhs-factor-slots']=str(depth)
    slot=0 if wrong_set else depth-1
    text=text.replace('slot=0 bank=63',f'slot={slot} bank={63+slot}').replace('bank_a=63',f'bank_a={63+slot}')
    return text,cfg,cluster


class BankedFactorTests(unittest.TestCase):
    def test_native_banked_capacity_publication_replacement_and_epochs(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'banked'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra',
                '-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'rhs_banked_factors_model.cc'),'-o',str(binary)],
                check=True,capture_output=True)
            self.assertIn('keys=64 ways=8 ingress=1 bytes=8448',subprocess.check_output([str(binary)],text=True))

    def test_expanded_banks_keep_first_rf_read_and_registered_local_use(self):
        for depth in (16,32,64):
            text,cfg,cluster=fixture(depth)
            with self.subTest(depth=depth):
                result=replay_register_ports(text,cfg,cluster).summary
                self.assertEqual(result['read_words'],6)
                self.assertEqual(result['rhs_factors']['saved_read_words'],4)
                self.assertEqual(result['bank_count'],63+depth)

    def test_reject_global_associative_capture_outside_selected_eight_ways(self):
        for depth in (16,32,64):
            text,cfg,_=fixture(depth,True)
            with self.subTest(depth=depth),self.assertRaisesRegex(AssertionError,'eight ways'):
                replay_rhs_factors(text,cfg,list(_rows(text)))

    def test_reject_free_first_read_or_unregistered_capture(self):
        text,cfg,cluster=fixture(64)
        for before,after in [('read_words=5 ready=3','read_words=1 ready=3'),
                             ('cycle=1','cycle=0')]:
            with self.subTest(before=before),self.assertRaises((AssertionError,KeyError)):
                replay_register_ports(text.replace(before,after),cfg,cluster)

    def test_ledgers_pay_every_active_slot_and_reject_unsupported_depths(self):
        from reporting.test_single_line_config_audit import fixture as audit_fixture
        from single_line_config_audit import Effective,capacity_audit
        from check_single_line_storage import lifecycle_reserve
        resolved,_=audit_fixture(1024)
        resolved['options'].update(coeff_line_cache_allowed=False,coeff_rhs_factor_reuse=True,coeff_rhs_factor_slots=8)
        base=capacity_audit(Effective(resolved))['reservation_bytes']
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0',
             '--coeff-rhs-factor-reuse':'1','--coeff-rhs-factor-slots':'8'}
        baseline=lifecycle_reserve(cfg)
        for depth in (16,32,64):
            resolved['options']['coeff_rhs_factor_slots']=depth
            self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-base,128*(depth-8))
            self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-rhs-factor-slots':str(depth)}))-baseline,128*(depth-8))
        text,cfg,cluster=fixture(16);cfg['--coeff-rhs-factor-slots']='24'
        with self.assertRaisesRegex(AssertionError,'capacity'):replay_register_ports(text,cfg,cluster)
