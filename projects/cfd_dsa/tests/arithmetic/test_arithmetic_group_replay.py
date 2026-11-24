from collections import Counter
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from check_arithmetic_groups import replay_arithmetic_groups
from check_register_ports import RegisterReplay


class ArithmeticGroupReplayTests(unittest.TestCase):
    def fixture(self):
        config={'--coeff-arithmetic-mode':'fixed','--coeff-cluster-mul-count':'8',
                '--coeff-cluster-add-count':'7','--coeff-rf-read-words':'16'}
        lines=['CFD_ARITH_GROUP cycle=10 event=accept id=1 slot=0 pool=add caller=7 width=5 scan=1 ready=11 broadcast=0']
        for identity,cycle,width,offset,mask in [(1,11,2,0,96),(2,12,2,2,96),(3,13,1,4,32)]:
            lines.extend([f'CFD_RF_ISSUE cycle={cycle} id={identity} pool=add lanes={width} caller=7 read_words={2*width}',
                f'CFD_ARITH_GROUP cycle={cycle} event=issue id=1 rf={identity} offset={offset} width={width} mask={mask} crossed=0',
                f'CFD_RF_COMPLETE cycle={cycle+4} id={identity}'])
        lines.append('CFD_ARITH_GROUP cycle=18 event=complete id=1 slot=0 crossed=0')
        rf=RegisterReplay({'capacities':{'mul':24,'add':28}},Counter(),[])
        lines.sort(key=lambda line:int(line.split('cycle=')[1].split()[0]))
        return '\n'.join(lines),config,rf

    def run_fixture(self,text=None,config=None):
        original,options,rf=self.fixture()
        return replay_arithmetic_groups(original if text is None else text,config or options,[],rf)

    def test_five_results_reassembled_after_three_native_fragments(self):
        result,logical,rf=self.run_fixture()
        self.assertEqual(result['fragments'],3)
        self.assertEqual(logical,[dict(cycle='10',pool='add',lanes='5',caller='7',complete_cycle='18')])
        self.assertEqual(rf.completions['add','7',10,18,5],1)

    def test_native_partition_violation(self):
        text,_,_=self.fixture()
        with self.assertRaisesRegex(AssertionError,'ownership mismatch'):
            self.run_fixture(text.replace('mask=96','mask=3',1))

    def test_early_group_publication(self):
        text,_,_=self.fixture()
        with self.assertRaisesRegex(AssertionError,'premature'):
            self.run_fixture(text.replace('cycle=18 event=complete','cycle=17 event=complete'))

    def test_duplicate_fragment(self):
        text,_,_=self.fixture()
        with self.assertRaisesRegex(AssertionError,'duplicate fragment'):
            self.run_fixture(text.replace('rf=2','rf=1'))

    def test_missing_fragment(self):
        text,_,_=self.fixture()
        with self.assertRaises(AssertionError):
            self.run_fixture('\n'.join(l for l in text.splitlines() if 'rf=3 ' not in l))

    def test_unregistered_selection(self):
        text,_,_=self.fixture()
        with self.assertRaisesRegex(AssertionError,'unregistered'):
            self.run_fixture(text.replace('ready=11','ready=10'))

    def test_live_slot_cannot_be_reused(self):
        text,_,_=self.fixture()
        text=text.replace('\nCFD_RF_ISSUE cycle=11',
            '\nCFD_ARITH_GROUP cycle=10 event=accept id=2 slot=0 pool=add caller=7 width=1 scan=1 ready=11 broadcast=0\nCFD_RF_ISSUE cycle=11',1)
        with self.assertRaisesRegex(AssertionError,'overwritten'):
            self.run_fixture(text)

    def test_disabled_model_rejects_group_work(self):
        text,config,_=self.fixture();config['--coeff-arithmetic-mode']='legacy'
        with self.assertRaisesRegex(AssertionError,'disabled'):
            self.run_fixture(text,config)

    def borrow_fixture(self):
        config={'--coeff-arithmetic-mode':'borrow','--coeff-cluster-mul-count':'8',
                '--coeff-cluster-add-count':'7','--coeff-rf-read-words':'16'}
        text='\n'.join([
            'CFD_ARITH_GROUP cycle=10 event=accept id=1 slot=0 pool=mul caller=8 width=5 scan=1 ready=11 broadcast=0',
            'CFD_RF_ISSUE cycle=11 id=1 pool=mul lanes=5 caller=8 read_words=10',
            'CFD_ARITH_GROUP cycle=11 event=issue id=1 rf=1 offset=0 width=5 mask=227 crossed=1',
            'CFD_RF_COMPLETE cycle=14 id=1',
            'CFD_ARITH_GROUP cycle=16 event=complete id=1 slot=0 crossed=1'])
        return text,config

    def test_borrow_uses_native_lanes_first_and_pays_route(self):
        text,config=self.borrow_fixture()
        summary,_,_=self.run_fixture(text,config)
        self.assertEqual(summary['crossed_fragments'],1)

    def test_borrow_cannot_skip_cross_group_delay(self):
        text,config=self.borrow_fixture()
        with self.assertRaisesRegex(AssertionError,'premature'):
            self.run_fixture(text.replace('cycle=16 event=complete','cycle=15 event=complete'),config)

    def test_borrow_cannot_steal_native_preference(self):
        text,config=self.borrow_fixture()
        with self.assertRaisesRegex(AssertionError,'ownership mismatch'):
            self.run_fixture(text.replace('mask=227','mask=31'),config)

    def test_finite_group_capacity(self):
        text,config,_=self.fixture();config['--coeff-arithmetic-depth']='4'
        prefix='\n'.join(f'CFD_ARITH_GROUP cycle=10 event=accept id={i} slot={i-1} pool=add caller=7 width=1 scan=1 ready=11 broadcast=0' for i in range(1,6))
        with self.assertRaisesRegex(AssertionError,'capacity overflow'):
            self.run_fixture(prefix+'\n'+text,config)
