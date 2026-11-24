"""Independent dependency/resource checks for single-line compute experiments."""
import _support
from pathlib import Path
import unittest
from check_single_line_c import validate_cluster_trace
from check_single_line_recurrence import check_tree, check_wait_accounting


class FeedbackTreeTests(unittest.TestCase):
    config = {'--coeff-line-mvm-model': 'tree',
              '--coeff-backward-feedback-tree': '1',
              '--coeff-cluster-mul-lat': '3', '--coeff-cluster-add-lat': '4'}

    def fixture(self):
        lines = []
        cluster = []
        for prefix, label, times, lat in (
                ('CFD_MVM_PRODUCT', 'column', [0, 1, 2, 3, 4], 3),
                ('CFD_MVM_FEEDBACK', 'node', [3, 5, 7, 9, 13], 4)):
            for index, t in enumerate(times):
                caller = '8' if label == 'column' else ('7' if index == 4 else '9')
                pool = 'mul' if label == 'column' else 'add'
                for event, cycle in [('issue', t), ('complete', t + lat)]:
                    lines.append(f'{prefix} cycle={cycle} token=1 context=1 epoch=0 cell=0 event={event} {label}={index}')
                cluster.append(dict(pool=pool, caller=caller, cycle=str(t), lanes='5', complete_cycle=str(t+lat)))
        return lines, cluster

    def test_feedback_finishes_at_17_without_hidden_final_subtract(self):
        lines, cluster = self.fixture()
        result = check_tree('\n'.join(lines), self.config, cluster)
        self.assertEqual(result['feedback_contexts'], 1)

    def test_reject_dependency_violation(self):
        lines, cluster = self.fixture()
        lines = [s.replace('cycle=13 ', 'cycle=12 ') if 'event=issue node=4' in s else s for s in lines]
        next(r for r in cluster if r['caller'] == '7')['cycle'] = '12'
        with self.assertRaises(AssertionError):
            check_tree('\n'.join(lines), self.config, cluster)

    def test_reject_missing_physical_final_subtract(self):
        lines, cluster = self.fixture()
        with self.assertRaises(AssertionError):
            check_tree('\n'.join(lines), self.config, cluster[:-1])


class WaitCycleTests(unittest.TestCase):
    def fixture(self):
        return '\n'.join(['CFD_RESOURCE_STATS pool=shared_mul samples=10',
            'CFD_FRONTIER_STATE state=input_dma_spm cycles=4 samples=10',
            'CFD_FRONTIER_STATE state=rhs_b_compute cycles=6 samples=10',
            'CFD_WAIT_CYCLES reason=divider_ii cycles=3 samples=10'] + [
            f'CFD_FU_WAIT caller={c} resource={r} cycles=2 frontier_cycles=1 samples=10'
            for c in range(10) for r in range(3)])

    def test_cycle_partition_and_overlapping_unions(self):
        self.assertEqual(check_wait_accounting(self.fixture())['samples'],10)

    def test_reject_duplicate_visits_and_missing_partition(self):
        for old,new in [('cycles=3','cycles=11'),('cycles=6','cycles=5'),
                        ('frontier_cycles=1','frontier_cycles=3')]:
            with self.assertRaises(AssertionError):
                check_wait_accounting(self.fixture().replace(old,new))


class ColdBTreeTests(unittest.TestCase):
    def test_cold_b_tree_uses_same_dependency_and_physical_issue_checks(self):
        from arithmetic.test_single_line_frontier import WarmRhsTreeTests
        from check_single_line_frontier import check_rhs_tree
        fixture = WarmRhsTreeTests()
        text,trace,cluster = fixture.fixture()
        trace = [r for r in trace if r['stage'] != 'LU_CACHE_REUSE']
        config = dict(fixture.config, **{'--coeff-cold-b-tree':'1'})
        result = check_rhs_tree(text,config,trace,cluster)
        self.assertEqual(result['cold_requests'],1)
        with self.assertRaises(AssertionError):
            check_rhs_tree(text,config,trace,cluster[:-1])


class JointRhsTreeTests(unittest.TestCase):
    def fixture(self, scope='forward'):
        from arithmetic.test_single_line_frontier import WarmRhsTreeTests
        fixture = WarmRhsTreeTests()
        text, old_trace, cluster = fixture.fixture()
        indices = [5] if scope == 'lbar' else [0, 5] + ([10] if scope == 'all' else [])
        trace = []
        for index in indices:
            trace.append(dict(requestId='1', stage='RHS_COMPLETE',
                rhsBatch=str(index//5), rhsColumn=str(index%5),
                cycle=old_trace[-1]['cycle']))
        trace.append(old_trace[-1])
        config = dict(fixture.config, **{'--coeff-cold-rhs-tree': scope})
        return '\n'.join(text.replace(' k=', f' rhs={i} k=') for i in indices), trace, cluster*len(indices), config

    def test_concurrent_b_and_lbar_keep_independent_dependencies(self):
        from check_single_line_frontier import check_rhs_tree
        text, trace, cluster, config = self.fixture()
        result = check_rhs_tree(text, config, trace, cluster)
        self.assertEqual(result['rhs'], 2)
        self.assertEqual(result['rows'], 8)

    def test_lbar_only_scope_excludes_b_and_ubar(self):
        from check_single_line_frontier import check_rhs_tree
        text,trace,cluster,config=self.fixture('lbar')
        result=check_rhs_tree(text,config,trace,cluster)
        self.assertEqual(result['rhs'],1)
        self.assertEqual(result['rows'],4)
        for other in [0,10]:
            with self.assertRaises(AssertionError):
                check_rhs_tree(text.replace('rhs=5 ',f'rhs={other} '),config,trace,cluster)

    def test_lbar_scope_preserves_independent_cold_b_switch(self):
        from check_single_line_frontier import check_rhs_tree
        text,trace,cluster,config=self.fixture('forward')
        config=dict(config,**{'--coeff-cold-rhs-tree':'lbar','--coeff-cold-b-tree':'1'})
        self.assertEqual(check_rhs_tree(text,config,trace,cluster)['rhs'],2)
        with self.assertRaises(AssertionError):
            check_rhs_tree('\n'.join(s for s in text.splitlines() if 'rhs=0 ' not in s),config,trace,cluster)

    def test_all_scope_also_checks_ubar(self):
        from check_single_line_frontier import check_rhs_tree
        text, trace, cluster, config = self.fixture('all')
        self.assertEqual(check_rhs_tree(text,config,trace,cluster)['rhs'],3)

    def test_reject_missing_column_and_unselected_ubar(self):
        from check_single_line_frontier import check_rhs_tree
        text, trace, cluster, config = self.fixture()
        with self.assertRaises(AssertionError):
            check_rhs_tree('\n'.join(s for s in text.splitlines() if 'rhs=5 ' not in s),config,trace,cluster)
        with self.assertRaises(AssertionError):
            check_rhs_tree(text.replace('rhs=5 ', 'rhs=10 '),config,trace,cluster)

    def test_reject_missing_aggregate_physical_issue(self):
        from check_single_line_frontier import check_rhs_tree
        text, trace, cluster, config = self.fixture()
        with self.assertRaises(AssertionError):
            check_rhs_tree(text,config,trace,cluster[:len(cluster)//2])


class TailUbarOverlapTests(unittest.TestCase):
    def fixture(self):
        text = ['CFD_LINE_SCHEDULE token=1 cache_hit=0 backward_start=100',
                'CFD_EARLY_BACKWARD cycle=100 token=1']
        trace = []
        for cell in range(2):
            request = str(cell+1)
            for stage, cycle in [('TAIL_UBAR_HOLD',10),('TAIL_UBAR_RELEASE',100),
                                 ('DIRECT_BASE_READY',30),('UBAR_READY',150)]:
                trace.append(dict(requestId=request,stage=stage,cycle=str(cycle),
                                  cell=str(cell),generation='7'))
            for col in range(5):
                trace.append(dict(requestId=request,stage='TAIL_UBAR_ACTIVATE',
                    cycle='101',rhsBatch='2',rhsColumn=str(col)))
                if cell:
                    trace.append(dict(requestId=request,stage='RHS_COMPLETE',
                        cycle='40',rhsBatch='1',rhsColumn=str(col)))
            text += [f'CFD_LOCAL_Y_READY cycle=60 token=1 request={request} generation=7 cell={cell} first=0 storage=0 y_job={request}',
                     f'CFD_ONCHIP_START cycle=50 id={request} owner=1 base={hex(1000+cell*40)} words=5 stride=8 op=W',
                     f'CFD_ONCHIP_COMPLETE cycle=59 id={request}']
        config = {'--coeff-tail-ubar-overlap':'2','--lusgs-cells':'3',
                  '--coeff-line-tile-buffers':'2'}
        storage = [dict(token='1',reserved='1000',resident='1440')]
        return '\n'.join(text),config,trace,storage

    def test_bounded_tail_retains_factors_until_backward(self):
        from check_single_line_frontier import check_tail_overlap
        self.assertEqual(check_tail_overlap(*self.fixture())['deferred_cells'],2)

    def test_reject_early_activation_or_uncommitted_y(self):
        from check_single_line_frontier import check_tail_overlap
        text,config,trace,storage = self.fixture()
        with self.assertRaises(AssertionError):
            check_tail_overlap(text.replace('cycle=59','cycle=61'),config,trace,storage)
        import copy
        broken=copy.deepcopy(trace)
        next(r for r in broken if r['stage']=='TAIL_UBAR_ACTIVATE')['cycle']='99'
        with self.assertRaises(AssertionError):
            check_tail_overlap(text,config,broken,storage)

    def test_reject_spm_address_alias_and_incomplete_lbar(self):
        from check_single_line_frontier import check_tail_overlap
        text,config,trace,storage = self.fixture()
        with self.assertRaises(AssertionError):
            check_tail_overlap(text.replace('base=0x410','base=0x3e8'),config,trace,storage)
        trace=[r for r in trace if r.get('rhsBatch')!='1']
        with self.assertRaises(AssertionError):
            check_tail_overlap(text,config,trace,storage)


class LuLookaheadTests(unittest.TestCase):
    def fixture(self):
        return ('CFD_LU_LOOKAHEAD cycle=10 request=2 cell=2 frontier=1 mul=3 add=0 div=0',
                {'--coeff-lu-lookahead':'1'},
                [dict(pool='mul',caller='1',cycle='10',lanes='3')])

    def test_eligible_future_cell_uses_real_lu_lanes(self):
        from check_single_line_frontier import check_lu_lookahead
        self.assertEqual(check_lu_lookahead(*self.fixture())['mul'],3)

    def test_reject_outside_radius_and_fake_issue(self):
        from check_single_line_frontier import check_lu_lookahead
        text,config,cluster=self.fixture()
        for broken in [text.replace('cell=2','cell=3'),text.replace('mul=3','mul=4')]:
            with self.assertRaises(AssertionError):
                check_lu_lookahead(broken,config,cluster)

    def test_disabled_mode_cannot_claim_lookahead_issue(self):
        from check_single_line_frontier import check_lu_lookahead
        text,config,cluster=self.fixture()
        with self.assertRaises(AssertionError):
            check_lu_lookahead(text,{},cluster)


class ExtraMultiplierTests(unittest.TestCase):
    config = {'--coeff-cluster-mul-lat': '3', '--coeff-cluster-add-lat': '4',
              '--coeff-cluster-div-lat': '12', '--coeff-cluster-div-ii': '4',
              '--coeff-cluster-mul-count': '6', '--coeff-line-opt-level': '13',
              '--coeff-rhs-row-parallel': '1'}

    def rows(self, lu=True):
        return [dict(cycle='0', pool='mul', lanes='4', caller='4', complete_cycle='3'),
                dict(cycle='0', pool='mul', lanes='2', caller='1' if lu else '4', complete_cycle='3')]

    def test_shared_sixth_lane_accepts_six_issues(self):
        validate_cluster_trace(self.rows(), self.config)

    def test_lu_only_extra_lane_cannot_be_used_by_rhs(self):
        config = dict(self.config, **{'--coeff-extra-mul-policy': 'lu-only'})
        validate_cluster_trace(self.rows(), config)
        with self.assertRaises(ValueError):
            validate_cluster_trace(self.rows(lu=False), config)

    def test_baseline_still_rejects_six_issues(self):
        config = dict(self.config, **{'--coeff-cluster-mul-count': '5'})
        with self.assertRaises(ValueError):
            validate_cluster_trace(self.rows(), config)

    def test_added_pipeline_reserve_survives_lifecycle_reservation(self):
        from check_single_line_storage import lifecycle_reserve
        config = {'--coeff-line-opt-level':'13', '--coeff-spm-outstanding':'64',
                  '--coeff-onchip-timing':'0'}
        base = lifecycle_reserve(config)
        config['--coeff-cluster-mul-count'] = '6'
        self.assertEqual(lifecycle_reserve(config)-base,128)


if __name__ == '__main__':
    unittest.main()
