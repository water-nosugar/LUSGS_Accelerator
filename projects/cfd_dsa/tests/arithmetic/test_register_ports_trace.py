"""Independent physical RF replay and logical dependency readiness tests."""
import _support
from collections import Counter
import copy
from pathlib import Path
import unittest

from check_register_ports import replay_register_ports
from check_single_line_recurrence import check_tree
from check_single_line_frontier import check_rhs_tree
from check_single_line_onchip import check_onchip_arbitration


CONFIG={'--coeff-rf-read-words':'20','--coeff-rf-write-words':'5',
    '--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4',
    '--coeff-cluster-div-lat':'12','--coeff-cluster-div-ii':'4',
    '--coeff-divider-model':'pipelined','--coeff-divider-inflight':'0',
    '--coeff-divider-result-depth':'2'}


def physical(operations, config=None):
    """operations=(pool, caller, issue, lanes, actual write)."""
    config=config or CONFIG
    latency={'mul':int(config['--coeff-cluster-mul-lat']),
        'add':int(config['--coeff-cluster-add-lat']),
        'div':int(config['--coeff-cluster-div-lat'])}
    capacity={'mul':5*latency['mul'],'add':5*latency['add'],'div':5}
    cluster=[];events=[]
    for identity,(pool,caller,start,lanes,done) in enumerate(sorted(operations,key=lambda op:op[2]),1):
        nominal=start+latency[pool]
        cluster.append(dict(pool=pool,caller=str(caller),cycle=str(start),
            lanes=str(lanes),complete_cycle=str(nominal)))
        events.append((start,2,identity,
            f'CFD_RF_ISSUE cycle={start} id={identity} pool={pool} lanes={lanes} caller={caller} read_words={2*lanes} ready={nominal} capacity={capacity[pool]}'))
        events.append((done,0 if pool=='div' else 1,(nominal,identity),
            f'CFD_RF_COMPLETE cycle={done} id={identity} pool={pool} lanes={lanes} nominal={nominal}'))
    return '\n'.join(row[-1] for row in sorted(events,key=lambda row:row[:3])),cluster


class RegisterTraceTests(unittest.TestCase):
    def test_explicit_scalar_broadcast_reduces_only_eligible_multiply_reads(self):
        text,cluster=physical([('mul',8,0,5,3)])
        text=text.replace('read_words=10','read_words=6 broadcast=1')
        config=dict(CONFIG,**{'--coeff-rf-broadcast':'1'})
        self.assertEqual(replay_register_ports(text,config,cluster).summary['read_words'],6)
        with self.assertRaises(AssertionError):
            replay_register_ports(text,CONFIG,cluster)
        with self.assertRaises(AssertionError):
            replay_register_ports(text.replace('pool=mul','pool=add'),config,cluster)

    def test_split_result_writes_require_every_lane_before_publication(self):
        text,cluster=physical([('mul',8,0,5,4)])
        text=text.replace('CFD_RF_COMPLETE cycle=4',
            'CFD_RF_WRITE cycle=3 id=1 pool=mul lanes=3 offset=0 nominal=3\n'
            'CFD_RF_WRITE cycle=4 id=1 pool=mul lanes=2 offset=3 nominal=3\n'
            'CFD_RF_COMPLETE cycle=4')
        config=dict(CONFIG,**{'--coeff-rf-write-words':'3','--coeff-rf-split-writeback':'1'})
        result=replay_register_ports(text,config,cluster)
        self.assertEqual(result.summary['write_words'],5)
        self.assertEqual(result.summary['max_writeback_delay'],1)
        for broken in [text.replace('offset=3','offset=2'),
                       text.replace('pool=mul lanes=2 offset=3','pool=mul lanes=1 offset=3'),
                       text.replace('WRITE cycle=3','WRITE cycle=2')]:
            with self.subTest(broken=broken),self.assertRaises(AssertionError):
                replay_register_ports(broken,config,cluster)

    def fixture(self):
        return physical([('mul',8,0,5,3),('add',9,0,5,4),('mul',1,1,1,5)])

    def test_complete_join_delays_held_result_and_preserves_nominal_trace(self):
        text,cluster=self.fixture()
        result=replay_register_ports(text,CONFIG,cluster)
        self.assertEqual(result.summary['operations'],3)
        self.assertEqual(result.summary['read_words'],22)
        self.assertEqual(result.summary['write_words'],11)
        self.assertEqual(result.summary['delayed_operations'],1)
        self.assertEqual(result.summary['max_writeback_delay'],1)
        self.assertEqual(result.actual_cluster[-1]['complete_cycle'],'5')
        self.assertEqual(cluster[-1]['complete_cycle'],'4')
        self.assertEqual(result.completions['mul','1',1,5,1],1)

    def test_reject_read_write_oversubscription_and_missing_write(self):
        text,cluster=self.fixture()
        with self.assertRaisesRegex(AssertionError,'read bandwidth'):
            replay_register_ports(text,dict(CONFIG,**{'--coeff-rf-read-words':'10'}),cluster)
        for broken in (text.replace('COMPLETE cycle=5','COMPLETE cycle=4'),
                       '\n'.join(row for row in text.splitlines() if 'COMPLETE cycle=5' not in row)):
            with self.assertRaises(AssertionError):
                replay_register_ports(broken,CONFIG,cluster)

    def test_reject_false_ready_credit_capacity_and_unmatched_physical_issue(self):
        text,cluster=self.fixture()
        for before,after in [('ready=3','ready=2'),('capacity=15','capacity=1000'),
                             ('caller=8','caller=9'),('read_words=10','read_words=5'),
                             ('nominal=4','nominal=5')]:
            with self.subTest(before=before),self.assertRaises(AssertionError):
                replay_register_ports(text.replace(before,after),CONFIG,cluster)

    def test_oldest_result_cannot_be_bypassed(self):
        text,cluster=physical([('mul',8,0,5,3),('add',9,0,5,5),('mul',1,1,1,4)])
        with self.assertRaisesRegex(AssertionError,'older ready'):
            replay_register_ports(text,CONFIG,cluster)

    def test_finite_pipeline_credits_cannot_grow_while_outputs_stall(self):
        text,cluster=physical([('mul',1,t,5,20+t) for t in range(4)])
        with self.assertRaises(AssertionError):
            replay_register_ports(text,dict(CONFIG,**{'--coeff-rf-read-words':'0'}),cluster)

    def test_divider_core_plus_fifo_capacity_and_priority(self):
        text,cluster=physical([('div',0,0,1,12),('mul',8,9,5,13)])
        result=replay_register_ports(text,CONFIG,cluster)
        self.assertEqual(result.summary['capacities']['div'],5)
        self.assertEqual(result.completions['div','0',0,12,1],1)
        self.assertEqual(result.completions['mul','8',9,13,5],1)

    def test_divider_credit_priority_does_not_require_callback_print_order(self):
        text,cluster=physical([('div',0,0,1,12),('add',9,7,5,11),('mul',4,8,4,12)])
        lines=text.splitlines()
        at12=[i for i,row in enumerate(lines) if row.startswith('CFD_RF_COMPLETE cycle=12 ')]
        self.assertEqual(len(at12),2)
        lines[at12[0]],lines[at12[1]]=lines[at12[1]],lines[at12[0]]
        result=replay_register_ports('\n'.join(lines),CONFIG,cluster)
        self.assertEqual(result.summary['delayed_operations'],1)
        self.assertEqual(result.completions['mul','4',8,12,4],1)
        self.assertEqual(result.completions['div','0',0,12,1],1)

    def test_disabled_mode_retains_exact_nominal_contract(self):
        text,cluster=self.fixture()
        config=dict(CONFIG,**{'--coeff-rf-read-words':'0','--coeff-rf-write-words':'0'})
        self.assertIsNone(replay_register_ports('',config,cluster))
        with self.assertRaisesRegex(AssertionError,'disabled'):
            replay_register_ports(text,config,cluster)


class LogicalWritebackTests(unittest.TestCase):
    def feedback(self):
        config=dict(CONFIG,**{'--coeff-rf-read-words':'0',
            '--coeff-line-mvm-model':'tree','--coeff-backward-feedback-tree':'1'})
        products=[(0,3),(1,4),(2,5),(3,6),(4,8)]
        nodes=[(3,7),(5,9),(8,12),(9,13),(13,17)]
        operations=[('mul',8,begin,5,end) for begin,end in products]+[
            ('add',7 if i==4 else 9,begin,5,end) for i,(begin,end) in enumerate(nodes)]
        rf,cluster=physical(operations,config)
        logical=[]
        for prefix, label, entries in [('CFD_MVM_PRODUCT','column',products),
                                       ('CFD_MVM_FEEDBACK','node',nodes)]:
            for index,(begin,end) in enumerate(entries):
                for event,cycle in [('issue',begin),('complete',end)]:
                    logical.append(f'{prefix} cycle={cycle} token=1 context=1 epoch=0 cell=0 event={event} {label}={index}')
        return '\n'.join(logical)+'\n'+rf,config,cluster

    def test_tree_uses_actual_physical_write_not_nominal_latency(self):
        text,config,cluster=self.feedback()
        self.assertEqual(check_tree(text,config,cluster)['feedback_contexts'],1)
        broken=text.replace('CFD_MVM_PRODUCT cycle=8 ', 'CFD_MVM_PRODUCT cycle=7 ')
        with self.assertRaisesRegex(AssertionError,'actual RF write'):
            check_tree(broken,config,cluster)
        legacy=dict(config,**{'--coeff-rf-write-words':'0'})
        with self.assertRaises(AssertionError):
            check_tree(text,legacy,cluster)

    def test_rhs_tree_completions_join_real_writes(self):
        from arithmetic.test_single_line_frontier import WarmRhsTreeTests
        fixture=WarmRhsTreeTests()
        logical,trace,old_cluster=fixture.fixture()
        config=dict(CONFIG,**fixture.config)
        operations=[(row['pool'],int(row['caller']),int(row['cycle']),int(row['lanes']),
            int(row['cycle'])+(3 if row['pool']=='mul' else 4)) for row in old_cluster]
        rf,cluster=physical(operations,config)
        self.assertEqual(check_rhs_tree(logical+'\n'+rf,config,trace,cluster)['rows'],4)
        broken=logical.replace('CFD_RHS_TREE cycle=3 ', 'CFD_RHS_TREE cycle=4 ',1)
        with self.assertRaises(AssertionError):
            check_rhs_tree(broken+'\n'+rf,config,trace,cluster)


class OnchipArbitrationTests(unittest.TestCase):
    starts=[dict(id='1',cycle='0',active='1'),dict(id='2',cycle='0',active='0')]
    activates=[dict(id='2',cycle='4')]

    def test_task_width_counts_word_progress_not_number_of_bank_words(self):
        accesses=[dict(id='1',cycle='3'),dict(id='1',cycle='3'),dict(id='2',cycle='7')]
        result=check_onchip_arbitration(self.starts,self.activates,accesses,3,1)
        self.assertEqual(result['peak_progress_tasks'],1)
        accesses.append(dict(id='1',cycle='7'))
        with self.assertRaisesRegex(AssertionError,'distinct tasks'):
            check_onchip_arbitration(self.starts,self.activates,accesses,3,1)
        self.assertEqual(check_onchip_arbitration(self.starts,self.activates,accesses,3,2)['peak_progress_tasks'],2)

    def test_reject_latency_and_inactive_activation_bypass(self):
        for accesses in ([dict(id='1',cycle='2')],[dict(id='2',cycle='6')]):
            with self.assertRaises(AssertionError):
                check_onchip_arbitration(self.starts,self.activates,accesses,3,0)


if __name__=='__main__':
    unittest.main()
