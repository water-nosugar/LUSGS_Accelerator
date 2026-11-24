import _support
from pathlib import Path
import unittest

from prepare_large_line_suite import natural_band_records
from single_line_dataset import oracle


class LargeLineProjectionTests(unittest.TestCase):
    def test_original_blocks_rhs_and_missing_edges_are_preserved(self):
        eye = [float(i//5 == i % 5) for i in range(25)]
        blocks = {(i, i): [4*v for v in eye] for i in range(3)}
        blocks[0, 1] = [-v for v in eye]
        blocks[1, 0] = [-v for v in eye]
        # A nonadjacent source coupling must not be redirected into the band.
        blocks[0, 2] = [100*v for v in eye]
        rhs = [float(i+1) for i in range(15)]
        original = {k: list(v) for k, v in blocks.items()}
        raw = natural_band_records(blocks, rhs, 3)
        self.assertEqual(raw[0][50:75], blocks[0, 1])
        self.assertEqual(raw[1][25:50], blocks[1, 0])
        self.assertEqual(raw[1][50:75], [0.0]*25)
        self.assertEqual(raw[2][25:50], [0.0]*25)
        self.assertEqual(raw[2][75:80], rhs[10:15])
        self.assertEqual(blocks, original)
        expected = oracle(raw)
        self.assertEqual(expected[2][:5], [v/4 for v in rhs[10:15]])

    def test_prefix_boundary_excludes_the_next_original_node(self):
        eye = [float(i//5 == i % 5) for i in range(25)]
        blocks = {(0, 0): eye, (1, 1): eye, (0, 1): eye}
        raw = natural_band_records(blocks, list(range(10)), 1)
        self.assertEqual(raw[0][25:75], [0.0]*50)
        for cells in (0, 3):
            with self.assertRaises(ValueError): natural_band_records(blocks, list(range(10)), cells)


if __name__ == '__main__':
    unittest.main()
