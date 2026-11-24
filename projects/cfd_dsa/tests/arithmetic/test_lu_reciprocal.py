"""Reject unowned/early reciprocals and changed upper normalization values."""
import _support
from collections import Counter
from types import SimpleNamespace
import unittest
from check_lu_reciprocal import bits, check_lu_reciprocal
from check_single_line_c import work


class LuReciprocalTests(unittest.TestCase):
    def fixture(self):
        trace=[dict(requestId='1',k='-1',cycle='100',stage='LU_COMPLETE')]
        divider=[]; text=[]; completions=Counter()
        for k in range(5):
            t=20*k
            for stage,cycle in [('LU_RECIP_DIV_ISSUE',t),('LU_RECIP_READY',t+12)]:
                trace.append(dict(requestId='1',k=str(k),cycle=str(cycle),stage=stage))
            divider.append(dict(event='result',accepted=str(t),cycle=str(t+12),
                a=f'{bits(1.0):016x}',b=f'{bits(2.0):016x}',result=f'{bits(0.5):016x}'))
            completions['div','0',t,t+12,1]=1
            if k==4:continue
            trace.append(dict(requestId='1',k=str(k),cycle=str(t+13),stage='LU_UPPER_SCALE_ISSUE'))
            completions['mul','1',t+13,t+16,4-k]=1
            for j in range(k+1,5):
                text.append(f'CFD_LU_SCALE cycle={t+16} request=1 generation=1 cell=0 k={k} column={j} numerator={float(j).hex()} reciprocal={float(0.5).hex()} result={float(j*0.5).hex()}')
        return '\n'.join(text),{'--coeff-lu-reciprocal-share':'1'},trace,SimpleNamespace(completions=completions),divider

    def test_five_actual_divides_feed_ten_upper_multiplications(self):
        result=check_lu_reciprocal(*self.fixture())
        self.assertEqual(result['reciprocal_divides'],5)
        self.assertEqual(result['upper_quotients'],10)

    def test_reject_premature_reciprocal(self):
        args=list(self.fixture());args[2][2]['cycle']='11'
        with self.assertRaisesRegex(AssertionError,'actual RF return'):
            check_lu_reciprocal(*args)

    def test_reject_corrupt_or_missing_upper_scale(self):
        original=self.fixture()
        for text in [original[0].replace('reciprocal=0x1.0000000000000p-1','reciprocal=0x1.0000000000000p+0',1),
                     original[0].replace('result=0x1.0000000000000p-1','result=0x1.0000000000000p+0',1),
                     '\n'.join(original[0].splitlines()[1:])]:
            with self.assertRaises(AssertionError):
                check_lu_reciprocal(text,*original[1:])

    def test_reject_missing_physical_multiplier_return(self):
        args=list(self.fixture());args[3].completions.clear()
        with self.assertRaises(AssertionError):check_lu_reciprocal(*args)

    def test_independent_work_budget_including_single_point_and_warm(self):
        for n in (1,2,65,1024):
            old=work(n,1,1,'direct','tree',True,True)
            new=work(n,1,1,'direct','tree',True,True,lu_reciprocal_share=True)
            self.assertEqual(new['issues']['shared_div'],5*n)
            self.assertEqual(new['issues']['shared_mul']-old['issues']['shared_mul'],10*n+(5 if n==1 else 0))
            self.assertEqual(new['issues']['shared_add'],old['issues']['shared_add'])
        warm=work(65,3,3,'direct','tree',True,True,cached_reciprocal=True,lu_reciprocal_share=True)
        self.assertEqual(warm['issues']['shared_div'],325)
