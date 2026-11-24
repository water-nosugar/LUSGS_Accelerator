import _support
import copy
import unittest
from pathlib import Path

from check_single_line_frontier import check_rhs_tree, check_drain_arb
from check_single_line_c import work


class WarmRhsTreeTests(unittest.TestCase):
    config = {'--coeff-warm-rhs-tree': '1', '--coeff-cluster-mul-lat': '3',
              '--coeff-cluster-add-lat': '4'}

    def fixture(self):
        text, cluster = [], []
        start = 0
        for k in range(3, -1, -1):
            count = 4-k
            times = {1: [3], 2: [3, 7], 3: [3, 3, 7], 4: [3, 3, 7, 11]}[count]
            for event, cycle in [('products_issue', start), ('products_ready', start+3)]:
                text.append(f'CFD_RHS_TREE cycle={cycle} request=1 generation=1 cell=1 k={k} node=-1 event={event}')
            cluster.append(dict(pool='mul', caller='4', cycle=str(start), lanes=str(count)))
            for node, t in enumerate(times):
                for event, cycle in [('issue', start+t), ('complete', start+t+4)]:
                    text.append(f'CFD_RHS_TREE cycle={cycle} request=1 generation=1 cell=1 k={k} node={node} event={event}')
                cluster.append(dict(pool='add', caller='5', cycle=str(start+t), lanes='1'))
            start += times[-1]+4
        trace = [dict(requestId='1', stage='LU_CACHE_REUSE', cycle='0'),
                 dict(requestId='1', stage='DIRECT_BASE_READY', cycle=str(start))]
        return '\n'.join(text), trace, cluster

    def test_parallel_nodes_and_all_rows_use_physical_adders(self):
        text, trace, cluster = self.fixture()
        self.assertEqual(check_rhs_tree(text, self.config, trace, cluster),
                         dict(requests=1, rows=4))

    def test_reject_early_dependency_and_missing_physical_issue(self):
        text, trace, cluster = self.fixture()
        with self.assertRaises(AssertionError):
            check_rhs_tree(text, self.config, trace, cluster[:-1])
        # Keep latency and physical issue consistent; only violate readiness.
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if 'k=0 node=2' in line:
                cycle = int(line.split()[1].split('=')[1])
                lines[i] = line.replace(f'cycle={cycle}', f'cycle={cycle-1}')
        broken = copy.deepcopy(cluster)
        target = next(row for row in reversed(broken) if row['pool']=='add' and row['cycle'] != broken[-1]['cycle'])
        target['cycle'] = str(int(target['cycle'])-1)
        with self.assertRaises(AssertionError):
            check_rhs_tree('\n'.join(lines), self.config, trace, broken)


class DrainAdmissionTests(unittest.TestCase):
    config = {'--coeff-drain-policy': 'age', '--coeff-drain-arb-latency': '1',
              '--coeff-drain-arb-ii': '1', '--coeff-output-buffer-depth': '1'}

    def fixture(self):
        trace = [dict(requestId='1', stage='DRAIN_READY', cycle='0', k='0'),
                 dict(requestId='2', stage='DRAIN_READY', cycle='1', k='0'),
                 dict(requestId='1', stage='DRAIN_GRANT', cycle='1', k='0'),
                 dict(requestId='1', stage='DRAIN_LU_DONE', cycle='10', k='0'),
                 dict(requestId='2', stage='DRAIN_GRANT', cycle='11', k='0'),
                 dict(requestId='2', stage='DRAIN_LU_DONE', cycle='20', k='0')]
        text = 'CFD_DRAIN_ARB latency=1 ii=1 granted=2 wait_sum=11 wait_max=10 full=0\n'
        return text, trace

    def test_bounded_oldest_first_waits_for_real_response(self):
        text, trace = self.fixture()
        self.assertEqual(check_drain_arb(text, self.config, trace),
                         dict(granted=2, wait_max=10, peak=1))

    def test_separate_descriptor_credits_allow_overlap_without_new_requests(self):
        text,trace=self.fixture()
        trace[4]['cycle']='2'
        trace.sort(key=lambda r:int(r['cycle']))
        text='CFD_DRAIN_ARB latency=1 ii=1 granted=2 wait_sum=2 wait_max=1 full=0\n'
        config=dict(self.config,**{'--coeff-drain-task-depth':'2'})
        self.assertEqual(check_drain_arb(text,config,trace)['peak'],2)
        with self.assertRaises(AssertionError):check_drain_arb(text,self.config,trace)

    def test_reject_latency_bypass_overflow_and_age_inversion(self):
        text, trace = self.fixture()
        for index, cycle in [(2, '0'), (4, '2')]:
            broken = copy.deepcopy(trace)
            broken[index]['cycle'] = cycle
            with self.subTest(index=index), self.assertRaises(AssertionError):
                check_drain_arb(text, self.config, broken)
        broken = copy.deepcopy(trace)
        broken[1]['cycle'] = '0'
        broken[2]['requestId'] = '2'
        with self.assertRaisesRegex(AssertionError, 'older eligible'):
            check_drain_arb(text, self.config, broken)


class ForwardTreeWorkTests(unittest.TestCase):
    def test_same_multipliers_one_less_add_per_warm_internal_cell(self):
        a = work(65, 7, 7, 'direct', 'tree', True, True, cached_reciprocal=True)
        b = work(65, 7, 7, 'direct', 'tree', True, True, cached_reciprocal=True,
                 warm_forward_tree=True)
        self.assertEqual(a['issues']['shared_mul'], b['issues']['shared_mul'])
        self.assertEqual(a['issues']['shared_div'], b['issues']['shared_div'])
        self.assertEqual(a['issues']['shared_add']-b['issues']['shared_add'], 6*64*5)
        self.assertEqual(a['col']-b['col'], 6*64*5)


if __name__ == '__main__':
    unittest.main()
