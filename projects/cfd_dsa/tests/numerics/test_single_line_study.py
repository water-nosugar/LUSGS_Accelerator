"""Numerical and reproducibility contracts for the frozen single-line study."""
import _support
import hashlib
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = _support.PROJECT_ROOT

import prepare_single_line_study as study
from single_line_dataset import oracle, read_dataset


class StationaryInputTests(unittest.TestCase):
    def test_nested_lengths_preserve_raw_interiors_and_mask_endpoints(self):
        short = study.stationary_records(17, 211, 1.4)
        long = study.stationary_records(1024, 211, 1.4)
        self.assertEqual(short[:-1], long[:16])
        self.assertEqual(short[-1][:50], long[16][:50])
        self.assertEqual(short[-1][75:], long[16][75:])
        self.assertTrue(any(long[16][50:75]))
        self.assertEqual(short[-1][50:75], [0.0]*25)
        self.assertEqual(long[-1][50:75], [0.0]*25)
        self.assertEqual(long[0][25:50], [0.0]*25)

    def test_D_distribution_cannot_increase_with_cell_number_or_strength(self):
        # Fixed bounded distribution at every cell, including the long tail;
        # coupling cannot silently strengthen D to make harder runs pass.
        weak = study.stationary_records(4096, 307, 0.35)
        strong = study.stationary_records(4096, 307, 1.4)
        for a, b in zip(weak, strong):
            self.assertEqual(a[:25], b[:25])
            self.assertEqual(a[75:], b[75:])
            for row in range(5):
                self.assertTrue(3.5 <= a[row*5+row] < 4.5)
                self.assertTrue(all(abs(a[row*5+c]) <= .12 for c in range(5) if row != c))

    def test_coupling_control_is_exact_row_norm_contract(self):
        records = study.stationary_records(8, 211, 1.4)
        for cell, a in enumerate(records):
            for row in range(5):
                margin = a[row*5+row]-math.fsum(abs(a[row*5+c]) for c in range(5) if c != row)
                for offset, masked in ((25, cell == 0), (50, cell == 7)):
                    norm = math.fsum(abs(a[offset+row*5+c]) for c in range(5))
                    self.assertAlmostEqual(norm, 0 if masked else .7*margin, places=14)

    def test_seed_and_counter_are_reproducible_not_python_hash_dependent(self):
        self.assertEqual(study._unit(101, 0, 'D', 0), 0.4429060230291014)
        self.assertEqual(study.stationary_records(3), study.stationary_records(3))
        self.assertNotEqual(study.stationary_records(3,101), study.stationary_records(3,211))

    def test_reject_invalid_dimensions_strength_seed(self):
        for cells in (0, -1, True, 1.5):
            with self.assertRaises(ValueError): study.stationary_records(cells)
        for coupling in (-1, 1.81, math.nan, math.inf):
            with self.assertRaises(ValueError): study.stationary_records(2, coupling=coupling)
        with self.assertRaises(ValueError): study.stationary_records(2, seed=-1)

    def test_two_precision_oracle_rounding_and_recurrence_residuals(self):
        raw = study.stationary_records(17, 211, 1.4)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'input.cfd5'
            info = study.audited_dataset(path, raw)
            loaded = read_dataset(path)
            self.assertTrue(info['oracle_fp64_identical'])
            self.assertEqual(info['oracle_crosscheck_precision'],240)
            self.assertEqual([row[:80] for row in loaded],raw)
            for key in ('forward_normalized_residual','backward_normalized_residual'):
                self.assertLess(info['diagnostics'][key], 1e-10)
            self.assertGreater(info['diagnostics']['min_abs_pivot'], 1.)

    def test_cross_precision_mismatch_is_rejected_before_file_write(self):
        raw = study.stationary_records(1)
        expected = oracle(raw,200)
        different = [list(row) for row in expected]
        different[0][0] += .01
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'input.cfd5'
            with patch.object(study,'oracle',side_effect=[expected,different]):
                with self.assertRaisesRegex(ValueError,'rounding differs'):
                    study.audited_dataset(path,raw)
            self.assertFalse(path.exists())


class FrozenStudyTests(unittest.TestCase):
    def test_canonicalization_preserves_last_wins_without_duplicates(self):
        args = ['--a=1','--b=2','--a=3','--path=/a=b']
        self.assertEqual(study.freeze_args(args, {'--b':4},remove=('--path',)),
                         ['--a=3','--b=4'])
        with self.assertRaises(ValueError): study.freeze_args(['--a','1'])

    def test_generated_case_hardware_is_uniform_and_capacity_regimes_are_explicit(self):
        # Small fixtures retain the real suite plumbing and 200/240 oracle;
        # full-sized generation is a separate reproducible CLI operation.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(study,'BOUNDARIES',(2,3)), patch.object(study,'HOLDOUT_SEEDS',()):
                manifest, suite = study.prepare_study(root/'data', root/'suite.json',include_gt=False)
            self.assertEqual(len(suite['cases']),2)
            self.assertEqual(suite['common_args'],study.freeze_args(suite['common_args']))
            common = dict(a.split('=',1) for a in suite['common_args'])
            self.assertEqual(common['--coeff-cold-rhs-tree'],'none')
            self.assertEqual(common['--coeff-cold-b-tree'],'0')
            self.assertEqual(common['--coeff-lu-lookahead'],'1')
            self.assertEqual(common['--coeff-study-cache'],'prepared')
            self.assertEqual(common['--coeff-spm-banks'],'4')
            self.assertEqual(common['--coeff-line-spm-bytes'],'196608')
            self.assertNotIn('--coeff-input-file',common)
            for case in suite['cases']:
                keys=[a.split('=',1)[0] for a in suite['common_args']+case['args']]
                self.assertEqual(len(keys),len(set(keys)))
                options=dict(a.split('=',1) for a in case['args'])
                self.assertEqual(options['--bench-iters'],'3')
                self.assertEqual(options['--coefficient-update-interval'],'3')
            self.assertFalse(manifest['tolerance_changed'])
            self.assertEqual(manifest['oracle_precisions'],[200,240])
            on_disk=json.loads((root/'data/manifest.json').read_text())
            self.assertEqual(on_disk,manifest)


if __name__ == '__main__':
    unittest.main()
