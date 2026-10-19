import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from single_line_metrics import bank_utilization, phase_breakdown
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'configs'))
from single_line_contract import coefficient_cache_bytes


class AccountingTests(unittest.TestCase):
    def test_cumulative_snapshots_are_not_summed(self):
        rows = [dict(bank='0', read_words='20', write_words='10'),
                dict(bank='1', read_words='10', write_words='5'),
                dict(bank='0', read_words='40', write_words='20'),
                dict(bank='1', read_words='30', write_words='15')]
        result = bank_utilization(rows, 100, 2, 1, 1)
        self.assertEqual(result['read_words'], 70)
        self.assertEqual(result['read_pct'], 35)
        self.assertEqual(result['write_pct'], 17.5)

    def test_wait_observations_do_not_double_count_the_phase(self):
        result = phase_breakdown(0, 2003, [(0, 1728)], 178, 87)
        self.assertEqual(result['math_inactive_cycles'], 275)
        self.assertEqual(result['cycles'], result['math_active_cycles']+result['math_inactive_cycles'])
        self.assertFalse(result['wait_counters_additive'])
        self.assertNotIn('other', result)

    def test_overlapping_operations_and_window_clipping(self):
        result = phase_breakdown(10, 30, [(0, 15), (12, 20), (25, 50)])
        self.assertEqual(result['math_active_cycles'], 15)

    def test_capacity_boundary_keeps_a_bounded_solve_cache(self):
        available=196608-127488
        self.assertEqual(coefficient_cache_bytes(115,128,available,True),69000)
        self.assertEqual(coefficient_cache_bytes(116,128,available,True),0)
        self.assertEqual(coefficient_cache_bytes(116,128,available,True,'adaptive'),46400)
        self.assertEqual(coefficient_cache_bytes(116,128,available,False,'adaptive'),0)
        self.assertEqual(coefficient_cache_bytes(129,128,available,True,'adaptive'),0)

    def test_source_fingerprint_detects_uncommitted_content_changes(self):
        import tempfile
        from unittest.mock import patch
        import run_experiment
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            source=root/'src/arch/arm/cfd_example.cc'
            source.parent.mkdir(parents=True)
            source.write_text('int value = 1;\n')
            with patch.object(run_experiment,'REPO',root):
                old=run_experiment.source_fingerprint()
                source.write_text('int value = 2;\n')
                self.assertNotEqual(old,run_experiment.source_fingerprint())

    def test_completed_simulation_can_enter_resource_validation(self):
        import json
        import tempfile
        from check_context_ablation import read_case
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            manifest=dict(status='validating',acceptance_policy='auto',return_code=0,
                provenance_errors=[],command=[],gem5_sha256='example')
            (root/'manifest.json').write_text(json.dumps(manifest))
            (root/'driver.log').write_text('')
            (root/'simout.txt').write_text('LUSGS_STEP2_STREAMING_PASS\n'
                'CFD_LINE_SCHEDULE result_hash=abc\n'
                'lusgs.streamingStep2.totalCycles = 12\n')
            self.assertEqual(read_case(root)[0],12)
            manifest['status']='running'
            (root/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):read_case(root)

    def test_provenance_failure_cannot_enter_resource_validation(self):
        import json
        import tempfile
        from check_context_ablation import read_case
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            manifest=dict(status='validating',acceptance_policy='auto',return_code=0,
                provenance_errors=['source changed'])
            (root/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):read_case(root)


if __name__ == '__main__':
    unittest.main()
