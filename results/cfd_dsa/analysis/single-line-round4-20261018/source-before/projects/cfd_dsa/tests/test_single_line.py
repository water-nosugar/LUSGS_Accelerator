import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "configs/lusgs"), str(ROOT / "tools")]

from single_line_profile import apply_single_line_profile
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
