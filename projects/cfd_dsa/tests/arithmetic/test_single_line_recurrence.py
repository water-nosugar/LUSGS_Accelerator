import _support
import copy
import json
import tempfile
from pathlib import Path
import unittest
from check_single_line_recurrence import check_tree, check_data_commit


class StreamingTreeTests(unittest.TestCase):
    config = {'--coeff-line-mvm-model': 'tree', '--coeff-cluster-mul-lat': '3',
              '--coeff-cluster-add-lat': '4'}

    def fixture(self):
        lines = []; cluster = []
        for prefix, kind, count, times, lat, pool, caller in (
                ('CFD_MVM_PRODUCT', 'column', 5, [0, 1, 2, 3, 4], 3, 'mul', '8'),
                ('CFD_MVM_TREE', 'node', 4, [4, 6, 10, 14], 4, 'add', '9')):
            for i in range(count):
                for event, cycle in [('issue', times[i]), ('complete', times[i]+lat)]:
                    lines.append(f'{prefix} cycle={cycle} token=1 context=1 epoch=0 cell=0 event={event} {kind}={i}')
                cluster.append(dict(pool=pool, caller=caller, cycle=str(times[i])))
        return lines, cluster

    def test_pair_can_run_before_fifth_product(self):
        lines, cluster = self.fixture()
        self.assertEqual(check_tree('\n'.join(lines), self.config, cluster),
                         dict(contexts=1, early_first_pairs=1))

    def test_reject_unready_partial_and_hidden_issue(self):
        lines, cluster = self.fixture()
        broken = [s.replace('cycle=10 ', 'cycle=9 ') if 'event=issue node=2' in s else s
                  for s in lines]
        bad_cluster = copy.deepcopy(cluster)
        next(r for r in bad_cluster if r['caller']=='9' and r['cycle']=='10')['cycle']='9'
        with self.assertRaises(AssertionError):
            check_tree('\n'.join(broken), self.config, bad_cluster)
        with self.assertRaises(AssertionError):
            check_tree('\n'.join(lines), self.config, cluster[:-1])


class DataCommitTests(unittest.TestCase):
    config = {'--coeff-data-commit-depth':'1', '--coeff-output-buffer-depth':'1',
              '--coeff-input-slots':'1', '--lu5-pending-depth':'1',
              '--coeff3-pending-depth':'1', '--coeff-stream-window':'1'}

    def fixture(self):
        trace=[]
        for req, alloc, detach, complete in [('1',0,3,10),('2',4,11,14)]:
            for stage, cycle in [('REQUEST_ALLOC',alloc),('COEFF3_COMPLETE',detach-1),
                    ('HANDOFF_PRODUCE',detach-1),('SPM_PACKET_ISSUE',detach),
                    ('DATA_COMMIT_DETACH',detach),('SPM_PACKET_RESPONSE',complete),
                    ('REQUEST_COMPLETE',complete),('REQUEST_DETACH',complete),
                    ('DATA_COMMIT_RELEASE',complete),('REQUEST_AUTO_RETIRE',complete+2)]:
                trace.append(dict(requestId=req,stage=stage,cycle=str(cycle),requester='CoeffDrain'))
        text='CFD_DATA_COMMIT depth=1 capacity=4 issued=2 released=2 live=0 peak=1 full=0\n'
        return text,trace

    def test_window_reused_while_prior_dma_pending(self):
        text,trace=self.fixture()
        r=check_data_commit(text,self.config,trace)
        self.assertEqual((r['detached'],r['total_peak'],r['compute_peak']),(2,2,1))

    def test_reject_premature_completion_and_commit_overflow(self):
        text,trace=self.fixture()
        broken=copy.deepcopy(trace)
        next(r for r in broken if r['stage']=='SPM_PACKET_RESPONSE')['cycle']='12'
        with self.assertRaises(AssertionError):check_data_commit(text,self.config,broken)
        broken=copy.deepcopy(trace)
        for r in broken:
            if r['requestId']=='2' and r['stage']=='DATA_COMMIT_DETACH':r['cycle']='9'
            if r['requestId']=='2' and r['stage'] in ('HANDOFF_PRODUCE','COEFF3_COMPLETE'):r['cycle']='8'
            if r['requestId']=='2' and r['stage']=='SPM_PACKET_ISSUE':r['cycle']='9'
        with self.assertRaises(AssertionError):check_data_commit(text,self.config,broken)


class ResolvedDefaultTests(unittest.TestCase):
    def fixture(self, command):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name)
        (path/'manifest.json').write_text(json.dumps({'command':command}))
        (path/'resolved_experiment.json').write_text(json.dumps({'options':{
            'coeff_data_commit_depth':4,'coeff_warm_diagonal':'reciprocal',
            'coeff_line_mvm_model':'tree'}}))
        return path

    def test_acceptance_uses_actual_profile_defaults(self):
        from check_single_line_output_dma import resolved_options
        config = resolved_options(self.fixture(['runner','--coeff-line-opt-level=12']))
        self.assertEqual(config['--coeff-data-commit-depth'],'4')
        self.assertEqual(config['--coeff-warm-diagonal'],'reciprocal')
        self.assertEqual(config['--coeff-line-mvm-model'],'tree')

    def test_reject_manifest_that_disagrees_with_actual_configuration(self):
        from check_single_line_output_dma import resolved_options
        with self.assertRaises(ValueError):
            resolved_options(self.fixture(['runner','--coeff-data-commit-depth=0']))


if __name__=='__main__':unittest.main()
