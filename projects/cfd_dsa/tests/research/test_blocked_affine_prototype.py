import _support
import random
from pathlib import Path
import unittest

from experiments.affine_prefix import Arithmetic,blocked_scan,sequential_scan


class BlockedAffineTests(unittest.TestCase):
    def test_noncommuting_matrices_and_irregular_block_boundaries(self):
        rng=random.Random(123)
        for n in [1,2,17,31,65]:
            pairs=[([rng.uniform(-.05,.05) for _ in range(25)],
                    [rng.uniform(-1,1) for _ in range(5)]) for _ in range(n)]
            expected=sequential_scan(pairs,Arithmetic())
            for block in [1,3,8,16,32]:
                with self.subTest(n=n,block=block):
                    got=blocked_scan(pairs,block,Arithmetic())
                    self.assertEqual(len(got),n)
                    self.assertLess(max(abs(a-b) for x,y in zip(got,expected)
                                        for a,b in zip(x,y)),1e-12)

    def test_one_cell_block_has_no_hidden_matrix_product(self):
        arithmetic=Arithmetic()
        pair=([0.0]*25,[1.0,2.0,3.0,4.0,5.0])
        self.assertEqual(blocked_scan([pair],1,arithmetic),[pair[1]])
        self.assertEqual(arithmetic.work['mul'],25)
        self.assertEqual(arithmetic.work['affine_compositions'],0)


if __name__=='__main__':unittest.main()
