import _support
import copy
from pathlib import Path
import unittest

ROOT = _support.PROJECT_ROOT

from single_line_profile import apply_single_line_profile
from check_single_line_c import work
from check_single_line_output_dma import validate_row
from analyze_single_line_storage import ledger


class SingleLineScopeTests(unittest.TestCase):
    def test_scope_and_idempotence(self):
        argv = ["runner"]
        apply_single_line_profile(argv)
        expected = list(argv)
        apply_single_line_profile(argv)
        self.assertEqual(argv, expected)
        self.assertEqual(argv[argv.index("--lusgs-lines") + 1], "1")
        self.assertEqual(argv[argv.index("--lusgs-interleave") + 1], "1")

    def test_reject_multiple_lines_even_with_duplicate_overrides(self):
        for args in (["--lusgs-lines=2"], ["--lusgs-interleave", "4"],
                     ["--lusgs-lines=1", "--lusgs-lines=2"],
                     ["--lusgs-lines=2", "--lusgs-lines=1"], ["--lusgs-lines"]):
            with self.subTest(args=args), self.assertRaises(ValueError):
                apply_single_line_profile(["runner", *args])

    def test_preserve_explicit_resource_settings(self):
        argv = ["runner", "--coeff3-count=4", "--coeff-spm-model=dma"]
        apply_single_line_profile(argv)
        self.assertNotIn("--coeff3-count", argv)
        self.assertIn("--coeff-spm-model=dma", argv)

    def test_reciprocal_optimized_default_and_explicit_divide(self):
        argv = ["runner"]
        apply_single_line_profile(argv)
        self.assertEqual(argv[argv.index("--coeff3-div-mode") + 1], "divide")
        for level, expected in ((1, "divide"), (2, "reciprocal"),
                                (11, "reciprocal")):
            with self.subTest(level=level):
                argv = ["runner", f"--coeff-line-opt-level={level}"]
                apply_single_line_profile(argv)
                self.assertEqual(argv[argv.index("--coeff3-div-mode") + 1],
                                 expected)
        override = ["runner", "--coeff-line-opt-level=11", "--coeff3-div-mode=divide"]
        apply_single_line_profile(override)
        self.assertEqual(override.count("--coeff3-div-mode=divide"), 1)
        self.assertNotIn("--coeff3-div-mode", override)

    def test_detailed_divider_default_for_one_shared_core(self):
        for extra, expected in (([], 'timing'),
                (['--coeff-compute-model=shared'], 'pipelined'),
                (['--coeff-compute-model=shared', '--coeff-cluster-div-count=2'], 'timing')):
            argv = ['runner', *extra]
            apply_single_line_profile(argv)
            self.assertEqual(argv[argv.index('--coeff-divider-model')+1], expected)
        argv = ['runner', '--coeff-compute-model=shared', '--coeff-divider-model=timing']
        apply_single_line_profile(argv)
        self.assertNotIn('--coeff-divider-model', argv)

    def test_recurrence_profile_and_explicit_ordered_fallback(self):
        argv = ['runner', '--coeff-line-opt-level=12']
        apply_single_line_profile(argv)
        self.assertEqual(argv[argv.index('--coeff-line-mvm-model')+1], 'tree')
        self.assertEqual(argv[argv.index('--coeff-data-commit-depth')+1], '4')
        argv = ['runner', '--coeff-line-opt-level=12',
                '--coeff-line-mvm-model=split', '--coeff-data-commit-depth=0']
        apply_single_line_profile(argv)
        self.assertNotIn('--coeff-line-mvm-model', argv)
        self.assertNotIn('--coeff-data-commit-depth', argv)
        argv = ['runner', '--coeff-line-opt-level=12', '--coeff3-div-mode=divide']
        apply_single_line_profile(argv)
        self.assertEqual(argv[argv.index('--coeff-warm-diagonal')+1], 'divide')

    def test_commit_default_respects_existing_output_state_capacity(self):
        argv = ['runner', '--coeff-line-opt-level=12',
                '--coeff-output-buffer-depth=1']
        apply_single_line_profile(argv)
        self.assertEqual(argv[argv.index('--coeff-data-commit-depth')+1], '1')

    def test_frontier_profile_preserves_explicit_legacy_paths(self):
        argv = ['runner', '--coeff-line-opt-level=13']
        apply_single_line_profile(argv)
        for flag, expected in [('--coeff-warm-forward-tree','1'),
                ('--coeff-warm-rhs-tree','1'),('--coeff-drain-policy','age')]:
            self.assertEqual(argv[argv.index(flag)+1], expected)
        argv = ['runner', '--coeff-line-opt-level=13', '--coeff-warm-forward-tree=0',
                '--coeff-warm-rhs-tree=0', '--coeff-drain-policy=legacy']
        apply_single_line_profile(argv)
        for flag in ('--coeff-warm-forward-tree','--coeff-warm-rhs-tree','--coeff-drain-policy'):
            self.assertNotIn(flag, argv)


class VisibilityCheckerTests(unittest.TestCase):
    def setUp(self):
        self.row = dict(backward_start="100", backward_done="200", visible_done="240",
                        children_retired="180", output_bytes_issued="80",
                        output_bytes_completed="80", output_packets_issued="3",
                        output_packets_completed="3", output_queue_max="1",
                        output_last_response="239")

    def test_numeric_completion_can_precede_visibility(self):
        validate_row(self.row, cells=2, depth=1, dma=True)

    def test_reject_premature_visibility_lost_bytes_and_overflow(self):
        for key, value in (("visible_done", "220"), ("output_bytes_completed", "40"),
                           ("output_bytes_issued", "120"), ("output_packets_completed", "2"),
                           ("output_queue_max", "2"), ("children_retired", "250")):
            row = copy.deepcopy(self.row)
            row[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_row(row, cells=2, depth=1, dma=True)


class ReciprocalWorkTests(unittest.TestCase):
    def test_cached_diagonal_reuses_existing_reciprocal_without_new_divide(self):
        old = work(65, 7, 7, 'direct', 'tree', True, True)
        new = work(65, 7, 7, 'direct', 'tree', True, True,
                   cached_reciprocal=True)
        self.assertEqual(old['issues']['shared_div']-new['issues']['shared_div'],
                         6*65*5)
        self.assertEqual(new['issues']['shared_mul']-old['issues']['shared_mul'],
                         6*65*5)
    def test_multi_rhs_cold_and_one_rhs_warm(self):
        old = work(65, 7, 7, 'direct', 'split', False, True)
        new = work(65, 7, 7, 'direct', 'split', True, True)
        self.assertEqual(old['issues']['shared_div'] - new['issues']['shared_div'],
                         5 * (705 - 65))
        self.assertEqual(new['issues']['shared_mul'] - old['issues']['shared_mul'],
                         5 * 705)

    def test_single_cell_direct_still_divides(self):
        old = work(1, 3, 3, 'direct', 'split', False, True)
        new = work(1, 3, 3, 'direct', 'split', True, True)
        self.assertEqual(new['issues'], old['issues'])
        cached = work(1, 3, 3, 'direct', 'split', True, True,
                      cached_reciprocal=True)
        self.assertEqual(cached['issues'], old['issues'])


class DedicatedSubtractWorkTests(unittest.TestCase):
    def test_subtract_work_moves_off_shared_add(self):
        shared = work(17, 1, 1, 'direct', 'split', False, True, False)
        dedicated = work(17, 1, 1, 'direct', 'split', False, True, True)
        moved = 10 * (17 - 1)
        self.assertEqual(shared['issues']['shared_add'] -
                         dedicated['issues']['shared_add'], moved)
        self.assertEqual(dedicated['issues']['shared_sub'], moved)
        self.assertNotIn('shared_sub', shared['issues'])


class StorageLedgerTests(unittest.TestCase):
    def test_boundaries(self):
        self.assertEqual(ledger(1, 40)["backward_live_data_bytes"], 40)
        self.assertTrue(ledger(1, 40)["all_backward_data_fits"])
        self.assertFalse(ledger(1, 39)["all_backward_data_fits"])
        self.assertEqual(ledger(1, 39)["max_cells_with_boundary_elision"], 0)
        row = ledger(4096, 1024 * 1024, 64 * 1024)
        self.assertEqual(row["backward_live_data_bytes"], 982840)
        self.assertTrue(row["all_backward_data_fits"])
        self.assertFalse(ledger(4097, 1024 * 1024, 64 * 1024)["all_backward_data_fits"])


if __name__ == "__main__":
    unittest.main()
