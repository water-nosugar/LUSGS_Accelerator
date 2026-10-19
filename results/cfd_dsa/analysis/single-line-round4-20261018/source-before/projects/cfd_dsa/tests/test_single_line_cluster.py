"""Audit physical issue contracts independently of the simulated scheduler."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_single_line_c import validate_cluster_trace


class ClusterTraceTests(unittest.TestCase):
    config = {'--coeff-cluster-mul-lat': '3',
              '--coeff-cluster-add-lat': '4',
              '--coeff-cluster-div-lat': '12',
              '--coeff-cluster-div-ii': '4'}

    def row(self, cycle, pool, lanes, caller, complete=None):
        lat = {'mul': 3, 'add': 4, 'div': 12}[pool]
        return dict(cycle=str(cycle), pool=pool, lanes=str(lanes),
                    caller=str(caller),
                    complete_cycle=str(cycle+lat if complete is None else complete))

    def check(self, rows):
        return validate_cluster_trace(rows, self.config)

    def test_scalar_and_vector_share_five_lanes(self):
        rows = [self.row(0, 'mul', 1, 1), self.row(0, 'mul', 1, 4),
                self.row(1, 'mul', 5, 8), self.row(1, 'add', 5, 9),
                self.row(2, 'add', 5, 7)]
        self.assertEqual(self.check(rows), {'shared_mul': 7, 'shared_add': 10})

    def test_lu_and_mvm_cannot_oversubscribe_multiplier(self):
        with self.assertRaisesRegex(ValueError, 'oversubscribed'):
            self.check([self.row(0, 'mul', 5, 8), self.row(0, 'mul', 1, 1)])

    def test_lu_and_trsm_use_same_divider_ii(self):
        with self.assertRaisesRegex(ValueError, 'II violated'):
            self.check([self.row(0, 'div', 1, 0), self.row(3, 'div', 1, 3)])
        self.assertEqual(self.check([self.row(0, 'div', 1, 0),
                                     self.row(4, 'div', 1, 3)]),
                         {'shared_div': 2})

    def test_wrong_bank_and_hidden_bank_rejected(self):
        for pool in ('mul', 'private_divider'):
            row = self.row(0, 'mul', 1, 0)
            row['pool'] = pool
            with self.assertRaisesRegex(ValueError, 'wrong shared bank'):
                self.check([row])

    def test_vector_width_and_latency_are_physical(self):
        for row in (self.row(0, 'add', 1, 9),
                    self.row(0, 'mul', 5, 8, complete=2)):
            with self.assertRaisesRegex(ValueError, 'width or completion latency'):
                self.check([row])


if __name__ == '__main__':
    unittest.main()
