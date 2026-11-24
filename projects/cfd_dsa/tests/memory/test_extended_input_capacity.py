import _support
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from single_line_contract import input_slot_count_valid
from single_line_config_audit import Effective, audit_input_capacity


class ExtendedInputCapacityTests(unittest.TestCase):
    def test_legacy_discrete_slots(self):
        for level in [0, 7, 12]:
            actual = {slots for slots in range(17) if input_slot_count_valid(level, slots)}
            self.assertEqual(actual, {1, 2, 4})

    def test_base_level13_capacity(self):
        for level in [13, 14]:
            actual = {slots for slots in range(17) if input_slot_count_valid(level, slots)}
            self.assertEqual(actual, set(range(1, 9)))

    def test_explicit_extension_is_bounded_at_sixteen(self):
        for level in [13, 14]:
            actual = {slots for slots in range(-1, 18) if input_slot_count_valid(level, slots, True)}
            self.assertEqual(actual, set(range(1, 17)))

    def test_extension_requires_level13(self):
        for level in [0, 7, 12]:
            self.assertFalse(any(input_slot_count_valid(level, slots, True) for slots in range(17)))

    def test_cpp_native_guard_matches_public_capacity_contract(self):
        compiler = shutil.which('g++') or shutil.which('c++')
        self.assertIsNotNone(compiler)
        source = '''
#include "arch/arm/cfd_input_capacity.hh"
#include <iostream>
int main() {
    for (unsigned level : {12u,13u})
        for (unsigned extended : {0u,1u})
            for (unsigned sme : {0u,1u})
                for (unsigned slots=0;slots<35;++slots)
                    std::cout<<level<<","<<extended<<","<<sme<<","<<slots<<":"
                             <<gem5::ArmISA::CfdInputCapacity::allowed(level,slots,extended,sme)<<"\\n";
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / 'capacity'
            subprocess.run([compiler, '-std=c++17', '-I', str(_support.REPO_ROOT / 'src'),
                            '-x', 'c++', '-o', str(executable), '-'],
                           input=source, text=True, capture_output=True, check=True)
            lines = subprocess.check_output([str(executable)], text=True).splitlines()
        for line in lines:
            key, result = line.split(':')
            level, extended, sme, slots = map(int, key.split(','))
            if extended:
                expected = set(range(1, 33 if sme else 17)) if level == 13 else set()
            else:
                expected = set(range(1, 9)) if level == 13 else {1, 2, 4}
            self.assertEqual(int(result), int(slots in expected), key)
            self.assertEqual(bool(int(result)),
                             input_slot_count_valid(level, slots, bool(extended), bool(sme)), key)

    def options(self):
        return dict(coeff_input_slots=16, coeff_extended_input_slots=True,
                    coeff_line_opt_level=13, coeff_onchip_timing=True, coeff_spm_model='dma')

    def test_independent_audit_reports_paid_instance(self):
        effective = Effective(dict(options=self.options()))
        result = audit_input_capacity(effective, 'tiled', 'shared')
        self.assertEqual(effective.errors, [])
        self.assertEqual((result['configured_slots'], result['slot_limit']), (16, 16))
        self.assertEqual((result['request_frame_bytes'], result['onchip_context_bytes_per_frame']),
                         (8192, 128))

    def test_sme_audit_accepts_paid_wide_landing_capacity(self):
        opts = dict(self.options(), coeff_input_slots=24, coeff_compute_backend='sme-za')
        effective = Effective(dict(options=opts, controller_environment={'GEM5_CFD_SME_ZA': '1'}))
        result = audit_input_capacity(effective, 'tiled', 'shared')
        self.assertEqual(effective.errors, [])
        self.assertEqual((result['configured_slots'], result['slot_limit']), (24, 32))

    def test_scalar_backend_name_matches_the_controller_bit(self):
        opts = dict(self.options(), coeff_compute_backend='event-scalar')
        for flag in ('0', '1'):
            with self.subTest(flag=flag):
                effective = Effective(dict(options=opts, controller_environment={'GEM5_CFD_SME_ZA': flag}))
                audit_input_capacity(effective, 'tiled', 'shared')
                self.assertEqual(bool(effective.errors), flag == '1')

    def test_audit_rejects_unsupported_bounds_and_hardware_modes(self):
        for changes, storage, model in [
                ({'coeff_input_slots': 17}, 'tiled', 'shared'),
                ({'coeff_extended_input_slots': False}, 'tiled', 'shared'),
                ({'coeff_onchip_timing': False}, 'tiled', 'shared'),
                ({'coeff_spm_model': 'packet'}, 'tiled', 'shared'),
                ({}, 'full', 'shared'), ({}, 'tiled', 'legacy')]:
            with self.subTest(changes=changes, storage=storage, model=model):
                effective = Effective(dict(options=dict(self.options(), **changes)))
                audit_input_capacity(effective, storage, model)
                self.assertTrue(effective.errors)


if __name__ == '__main__':
    unittest.main()
