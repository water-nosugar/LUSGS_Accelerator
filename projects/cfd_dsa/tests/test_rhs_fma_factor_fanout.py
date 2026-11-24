"""A second FMA receiver may share only the physically paid LU factor."""
import copy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'projects/cfd_dsa/tools'))
from test_rhs_factor_fanout import CONFIG, fixture as mul_fixture, replay


CFG = dict(CONFIG, **{'--coeff-rhs-fma-factor-fanout': '1',
                     '--coeff-rhs-fma-count': '10', '--coeff-rhs-upper-inverse': '1'})


def fixture():
    rows = mul_fixture()
    for prefix, row in rows:
        if prefix == 'CFD_RF_ISSUE':
            row.update(pool='fma', ready=int(row['cycle'])+5, capacity=50,
                       bank_dst=-1, read_words=9 if row['id'] == 1 else 5)
            if row['id'] == 1:
                row['bank_access'] = 0
    rows.append(('CFD_RHS_FMA_FACTOR_FANOUT_CAPACITY', dict(
        control_bytes=128, bus_bytes=256, receivers=2, vectors_per_cycle=1,
        extra_fma=0, extra_ports=0)))
    return rows


class RhsFmaFactorFanoutTests(unittest.TestCase):
    def test_actual_cli_registry_accepts_flag_and_default_stays_off(self):
        import ast
        import getopt
        tree = ast.parse((ROOT/'projects/cfd_dsa/configs/run_cfd_dsa.py').read_text())
        call = next(n for n in ast.walk(tree) if isinstance(n, ast.Call)
                    and isinstance(n.func, ast.Attribute) and n.func.attr == 'getopt')
        accepted = ast.literal_eval(call.args[2])
        parsed, remaining = getopt.getopt(['--coeff-rhs-fma-factor-fanout=1'], '', accepted)
        self.assertEqual(parsed, [('--coeff-rhs-fma-factor-fanout', '1')])
        self.assertEqual(remaining, [])
        default = next(n for n in ast.walk(tree) if isinstance(n, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == 'coeff_rhs_fma_factor_fanout'
                               for t in n.targets) and isinstance(n.value, ast.Constant))
        self.assertIs(default.value.value, False)

    def test_two_fmas_keep_both_other_operands_paid(self):
        wire, factors = replay(fixture(), CFG)
        self.assertEqual(wire['summary']['fma_uses'], 1)
        self.assertEqual(wire['summary']['saved_read_words'], 4)
        self.assertEqual(factors['summary']['hits'], 1)
        self.assertEqual(sum(r['read_words'] for p, r in fixture() if p == 'CFD_RF_ISSUE'), 19)

    def test_accumulator_and_scalar_read_cannot_be_removed(self):
        for index in [2, 5]:
            rows = fixture()
            rows[index][1]['read_words'] = 4
            with self.subTest(index=index), self.assertRaises(AssertionError):
                replay(rows, CFG)

    def test_mode_and_mux_budget_are_required(self):
        with self.assertRaises(AssertionError):
            replay(fixture(), dict(CFG, **{'--coeff-rhs-fma-factor-fanout': '0'}))
        for key, value in [('control_bytes', 127), ('receivers', 3), ('extra_fma', 1), ('extra_ports', 1)]:
            rows = fixture()
            rows[-1][1][key] = value
            with self.subTest(key=key), self.assertRaises(AssertionError):
                replay(rows, CFG)
        with self.assertRaises(AssertionError):
            replay(fixture()[:-1], CFG)

    def test_mixed_arithmetic_pool_and_local_result_are_rejected(self):
        for key, value in [('pool', 'mul'), ('bank_dst', 31), ('bank_words_a', 4)]:
            rows = fixture()
            rows[5][1][key] = value
            with self.subTest(key=key), self.assertRaises(AssertionError):
                replay(rows, CFG)

    def test_third_receiver_and_cross_cycle_reuse_rejected(self):
        rows = fixture()
        third = copy.deepcopy(rows[5:7])
        third[0][1]['id'] = 4
        third[1][1].update(id=4, rhs=8)
        with self.assertRaises(AssertionError):
            replay(rows[:-1]+third+[rows[-1]], CFG)
        rows = fixture()
        rows[6][1]['cycle'] = 2
        with self.assertRaises(AssertionError):
            replay(rows, CFG)

    def test_source_values_and_owner_are_required(self):
        for key, value in [('generation', 10), ('source', 99), ('v1', '-0x0p+0')]:
            rows = fixture()
            rows[6][1][key] = value
            with self.subTest(key=key), self.assertRaises((AssertionError, KeyError)):
                replay(rows, CFG)

    def test_control_is_paid_in_storage_and_audit(self):
        from check_single_line_storage import lifecycle_reserve
        before = dict(CFG, **{'--coeff-rhs-fma-factor-fanout': '0'})
        self.assertEqual(lifecycle_reserve(CFG)-lifecycle_reserve(before), 128)
        from reporting.test_single_line_config_audit import fixture as audit_fixture
        from single_line_config_audit import Effective, capacity_audit
        resolved, _ = audit_fixture(1024)
        resolved['options'].update(coeff_rhs_factor_fanout=True, coeff_rhs_fma_count=10,
            coeff_rhs_upper_inverse=True, coeff_rhs_fma_factor_fanout=False,
            coeff_rhs_factor_reuse=True, coeff_rf_read_words=32, coeff_rf_write_words=16)
        resolved['controller_environment'].update(GEM5_CFD_RHS_FACTOR_FANOUT='1',
            GEM5_CFD_RHS_FMA_COUNT='10', GEM5_CFD_RHS_UPPER_INVERSE='1',
            GEM5_CFD_RHS_FACTOR_REUSE='1', GEM5_CFD_RF_READ_WORDS='32', GEM5_CFD_RF_WRITE_WORDS='16',
            GEM5_CFD_RHS_FMA_FACTOR_FANOUT='0')
        before = capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options']['coeff_rhs_fma_factor_fanout'] = True
        resolved['controller_environment']['GEM5_CFD_RHS_FMA_FACTOR_FANOUT'] = '1'
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-before, 128)

    def test_native_shared_factor_has_exact_fma_payload_and_no_third_receiver(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder)/'fma-fanout'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fno-fast-math', '-ffp-contract=off', '-I'+str(ROOT/'src'),
                str(ROOT/'projects/cfd_dsa/tests/fixtures/rhs_fma_factor_fanout_model.cc'),
                '-o', str(binary)], check=True, capture_output=True)
            self.assertIn('FMA_FACTOR_FANOUT_PASS', subprocess.check_output([str(binary)], text=True))


if __name__ == '__main__':
    unittest.main()
