"""Hand-authored clock-enable/FIFO timeline, including corrupt trace rejection."""
import _support
import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest

from check_divider_model import validate_divider_trace
from check_context_ablation import read_case


class DividerTraceTests(unittest.TestCase):
    def fixture(self):
        one = '3ff0000000000000'
        rows = []
        for t in range(15):
            if t in (2, 6, 10, 14):
                tag = {2: 0, 6: 1, 10: 2, 14: 3}[t]
                rows.append(dict(cycle=str(t), event='result', tag=str(tag), owner='100',
                    a=one, b=one, result=one, flags='0', accepted=str(tag),
                    produced=str([2, 3, 6, 10][tag]), core=str([1, 1, 0, 0][tag]),
                    outputs=str([0, 1, 1, 0][tag]), dropped='0'))
            if t < 4:
                rows.append(dict(cycle=str(t), event='accept', tag=str(t), owner='100',
                    a=one, b=one, result='0000000000000000', flags='0', accepted=str(t),
                    produced='0', core=str(1 if t == 0 else 2), outputs=str(int(t == 3)), dropped='0'))
        summary = dict(model='pipelined', lat=2, ii=1, capacity=2, result_depth=1,
            writeback_ii=4, accepted=4, produced=4, returned=4, core_live=0, result_live=0,
            core_peak=2, result_peak=1, pipeline_stalls=5, output_stalls=8,
            core_occupancy=15, result_occupancy=11, compute_cycles=15,
            result_wait_cycles=11, residence_cycles=26, max_residence=11, dropped=0, flags=0)
        return rows, summary

    def test_finite_fifo_and_global_stall(self):
        rows, summary = self.fixture()
        self.assertEqual(validate_divider_trace(rows, summary)['residence_cycles'], 26)

    def test_corruption_is_rejected(self):
        rows, summary = self.fixture()
        for key, value in (('a', '4000000000000000'), ('tag', '99'),
                ('produced', '5'), ('core', '3'), ('result', '0000000000000000')):
            altered = copy.deepcopy(rows)
            response = next(r for r in altered if r['event'] == 'result')
            response[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_divider_trace(altered, summary)
        with self.assertRaises(ValueError):
            validate_divider_trace(rows[:-1], summary)
        with self.assertRaises(ValueError):
            validate_divider_trace(rows, dict(summary, residence_cycles=8))

    def test_shared_resource_uses_actual_residence(self):
        rows, summary = self.fixture()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path/'manifest.json').write_text(json.dumps(dict(status='passed', command=[], gem5_sha256='test')))
            (path/'driver.log').write_text('')
            text = ('LUSGS_STEP2_STREAMING_PASS\nstreamingStep2.totalCycles = 14\n'
                    'CFD_LINE_SCHEDULE result_hash=abc\n'
                    'CFD_RESOURCE_STATS pool=shared_div count=1 lat=2 ii=1 samples=15 '
                    'eligible_slots=4 issued=4 issue_cycles=4 blocked_cycles=0 no_attempt_cycles=11 '
                    'active_cycles=14 occupancy_sum=26 occupancy_max=3\n')
            text += 'CFD_DIVIDER_SUMMARY '+' '.join(f'{k}={v}' for k,v in summary.items())+'\n'
            (path/'simout.txt').write_text(text)
            with (path/'coeff_preprocess_trace.csv.divider.csv').open('w') as dst:
                writer = csv.DictWriter(dst, fieldnames=rows[0].keys())
                writer.writeheader(); writer.writerows(rows)
            with (path/'coeff_preprocess_trace.csv.cluster.csv').open('w') as dst:
                dst.write('cycle,pool\n0,div\n1,div\n2,div\n3,div\n')
            self.assertEqual(read_case(path)[2]['shared_div']['occupancy_sum'], 26)


if __name__ == '__main__':
    unittest.main()
