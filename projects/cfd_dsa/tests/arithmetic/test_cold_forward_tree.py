import _support
import copy
import unittest

from check_single_line_frontier import check_forward_tree
from check_single_line_c import work


class ColdForwardTreeTests(unittest.TestCase):
    config = {'--coeff-warm-forward-tree': '1', '--coeff-cold-forward-tree': '1',
              '--coeff-line-mvm-model': 'tree', '--coeff-lane-mvm': '0'}

    def fixture(self):
        common = dict(requestId='2', generation='101', cell='1')
        trace = [dict(common, stage='HANDOFF_CONSUME', cycle='20'),
                 dict(common, stage='CORRECTION_ACCUMULATOR_COMPLETE', cycle='38')]
        trace += [dict(common, stage='RHS_COMPLETE', rhsBatch='1', rhsColumn=str(col),
                       cycle=str(10 + col)) for col in range(5)]
        text = ['CFD_FORWARD_TREE cycle=20 token=7 request=2 generation=101 cell=1 epoch=0 event=begin mode=cold',
                'CFD_FORWARD_TREE cycle=38 token=7 request=2 generation=101 cell=1 epoch=0 event=complete mode=cold']
        for col in range(5):
            for event, tick in [('issue', 20 + col), ('complete', 23 + col)]:
                text.append(f'CFD_MVM_PRODUCT cycle={tick} token=7 context=0 epoch=0 cell=1 event={event} column={col}')
        for node, issue in enumerate([24, 26, 30, 34]):
            for event, tick in [('issue', issue), ('complete', issue + 4)]:
                text.append(f'CFD_MVM_TREE cycle={tick} token=7 context=0 epoch=0 cell=1 event={event} node={node}')
        return '\n'.join(text), trace

    def test_cold_columns_join_actual_rhs_returns(self):
        text, trace = self.fixture()
        self.assertEqual(check_forward_tree(text, self.config, trace),
                         dict(requests=1, cold_requests=1))

    def test_reject_early_column_and_missing_producer(self):
        text, trace = self.fixture()
        early = copy.deepcopy(trace)
        early[-1]['cycle'] = '25'
        with self.assertRaisesRegex(AssertionError, 'before actual Lbar return'):
            check_forward_tree(text, self.config, early)
        with self.assertRaisesRegex(AssertionError, 'missing cold Lbar producer'):
            check_forward_tree(text, self.config, trace[:-1])

    def test_reject_duplicate_column_and_stale_generation(self):
        text, trace = self.fixture()
        with self.assertRaises(AssertionError):
            check_forward_tree(text.replace('column=1', 'column=0'), self.config, trace)
        with self.assertRaises(AssertionError):
            check_forward_tree(text.replace('generation=101', 'generation=100'), self.config, trace)

    def test_reject_cold_mode_disguised_as_cached(self):
        text, trace = self.fixture()
        with self.assertRaises(AssertionError):
            check_forward_tree(text.replace('mode=cold', 'mode=cached'), self.config, trace)

    def test_requires_selected_mode_and_existing_vector_tree(self):
        text, trace = self.fixture()
        for changes in [{'--coeff-cold-forward-tree': '0'},
                        {'--coeff-warm-forward-tree': '0'},
                        {'--coeff-lane-mvm': '1'}, {'--coeff-line-mvm-model': 'serial'}]:
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                check_forward_tree(text, dict(self.config, **changes), trace)

    def test_legacy_cached_tree_still_uses_cache_register_barrier(self):
        text, trace = self.fixture()
        trace += [dict(requestId='2', generation='101', cell='1', stage='LU_CACHE_REUSE', cycle='0'),
                  dict(requestId='2', generation='101', cell='1', stage='CACHE_REGISTER_READY', cycle='15')]
        cfg = dict(self.config, **{'--coeff-cold-forward-tree': '0'})
        self.assertEqual(check_forward_tree(text.replace('mode=cold', 'mode=cached'), cfg, trace), dict(requests=1))
        trace[-1]['cycle'] = '21'
        with self.assertRaises(AssertionError):
            check_forward_tree(text, cfg, trace)


class ColdTreeWorkTests(unittest.TestCase):
    def test_same_multiplier_divider_work_five_fewer_adds_per_cold_internal_cell(self):
        for cells, sweeps, interval in [(1, 1, 1), (17, 1, 1), (65, 7, 3)]:
            args = (cells, sweeps, interval, 'direct', 'tree', True, True)
            before = work(*args, cached_reciprocal=True, warm_forward_tree=True)
            after = work(*args, cached_reciprocal=True, warm_forward_tree=True,
                         cold_forward_tree=True)
            rebuilds = (sweeps + interval - 1) // interval
            self.assertEqual(before['issues']['shared_mul'], after['issues']['shared_mul'])
            self.assertEqual(before['issues']['shared_div'], after['issues']['shared_div'])
            self.assertEqual(before['issues']['shared_add'] - after['issues']['shared_add'],
                             rebuilds * (cells - 1) * 5)
            self.assertEqual(before['col'] - after['col'], rebuilds * (cells - 1) * 5)


if __name__ == '__main__':
    unittest.main()
