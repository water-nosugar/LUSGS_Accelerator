"""Common-factor RF accounting cannot broadcast an arbitrary RHS product."""
import _support
from pathlib import Path
import unittest
from check_register_ports import replay_register_ports,check_preprocess_broadcast
from arithmetic.test_register_ports_trace import physical,CONFIG


def fixture():
    cfg=dict(CONFIG,**{'--coeff-rf-broadcast':'1','--coeff-preprocess-broadcast':'1',
                       '--coeff-rf-write-words':'8'})
    text,cluster=physical([('mul',4,0,4,3),('mul',4,4,3,7),('mul',4,8,2,11)],cfg)
    for width in (4,3,2):
        text=text.replace(f'lanes={width} caller=4 read_words={2*width}',
            f'lanes={width} caller=4 read_words={width+1} broadcast=1')
    for identity,k in enumerate(range(3),1):
        text+=f'\nCFD_PRE_BROADCAST cycle={4*k} id={identity} request=7 generation=1 cell=0 family=rhs phase=forward k={k} column=-1 rhs=0 lanes={4-k} factor=0x1p+0'
    trace=[dict(requestId='7',generation='1',cell='0',stage='REQUEST_ALLOC'),
           dict(requestId='7',generation='1',cell='0',stage='RHS_COMPLETE')]
    return text,cfg,cluster,trace


class PreprocessingBroadcastTests(unittest.TestCase):
    def test_one_forward_solve_saves_six_reads_with_same_physical_operations(self):
        text,cfg,cluster,trace=fixture()
        rf=replay_register_ports(text,cfg,cluster)
        self.assertEqual(rf.summary['read_words'],12)
        self.assertEqual(rf.summary['write_words'],9)
        checked=check_preprocess_broadcast(text,cfg,trace)
        self.assertEqual(checked['groups'],3)
        self.assertEqual(checked['rhs_saved_words'],6)
        self.assertEqual(checked['additional_arithmetic_operations'],0)

    def test_requires_explicit_preprocessing_enable(self):
        text,cfg,cluster,trace=fixture()
        cfg=dict(cfg,**{'--coeff-preprocess-broadcast':'0'})
        with self.assertRaisesRegex(AssertionError,'broadcast source'):
            replay_register_ports(text,cfg,cluster)
        with self.assertRaises(AssertionError):check_preprocess_broadcast(text,cfg,trace)

    def test_rejects_backward_scope_stale_owner_and_invalid_factor(self):
        text,cfg,cluster,trace=fixture()
        for changed in (text.replace('phase=forward','phase=backward',1),
                        text.replace('request=7 generation=1','request=7 generation=2',1),
                        text.replace('factor=0x1p+0','factor=nan',1),
                        text.replace('k=1 column=-1','k=2 column=-1',1)):
            with self.subTest(changed=changed),self.assertRaises(AssertionError):
                check_preprocess_broadcast(changed,cfg,trace)

    def test_missing_or_duplicate_fanout_does_not_create_free_reads(self):
        text,cfg,cluster,trace=fixture()
        last=text.splitlines()[-1]
        for changed in (text+'\n'+last,'\n'.join(text.splitlines()[:-1]),
                        text.replace('id=3 request=7','id=99 request=7')):
            with self.assertRaises(AssertionError):check_preprocess_broadcast(changed,cfg,trace)
        with self.assertRaises(AssertionError):
            replay_register_ports(text.replace('read_words=5','read_words=4'),cfg,cluster)

    def test_disabled_mode_has_no_common_factor_transfers(self):
        self.assertEqual(check_preprocess_broadcast('',{},[]),{})

if __name__=='__main__':unittest.main()
