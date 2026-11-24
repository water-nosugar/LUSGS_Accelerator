"""Capacity contracts for a single Ubar copy shared by cold/warm backward."""
import _support
import copy
from pathlib import Path
import unittest

from single_line_contract import line_storage_bytes
from check_single_line_onchip import check_cached_ubar


class UbarCacheStorageTests(unittest.TestCase):
    def layout(self, cells, *, policy='adaptive', direct=True, maximum=128,
               enabled=True, buffers=2):
        return line_storage_bytes(cells, 16, buffers, maximum,
                                  196608 - 136448, direct, policy, enabled)

    def test_full_cache_releases_both_tile_matrix_regions(self):
        self.assertEqual(self.layout(65), (1280, 39000))

    def test_joint_allocation_uses_released_space_at_capacity_boundary(self):
        resident, cache = self.layout(98)
        self.assertEqual((resident, cache), (1280, 58800))
        self.assertEqual(196608 - 136448 - resident - cache, 80)

    def test_next_cell_falls_back_to_bounded_solve_cache(self):
        self.assertEqual(self.layout(99), (7680, 39600))
        self.assertEqual(self.layout(99, policy='full'), (7680, 0))

    def test_inverse_cannot_use_a_partial_coefficient_cache(self):
        self.assertEqual(self.layout(99, direct=False), (7680, 0))

    def test_no_cache_and_cache_cell_limit_keep_tile_ubar(self):
        self.assertEqual(self.layout(65, enabled=False), (7680, 0))
        self.assertEqual(self.layout(65, maximum=64), (7680, 0))

    def test_tail_capacity_and_single_buffer(self):
        self.assertEqual(self.layout(1), (80, 600))
        self.assertEqual(self.layout(17, buffers=1), (640, 10200))

    def test_tagged_sme_cache_accounts_for_per_cell_format_tag(self):
        self.assertEqual(
            line_storage_bytes(4096, 32, 6, 4096, 3407872 - 1658600, True,
                               'adaptive', True, private_upper=True,
                               cache_tag_bytes=8),
            (7680, 4096 * 408))
        # Tight private-upper budget that fits 400 B/cell must reject 408 B/cell.
        self.assertEqual(
            line_storage_bytes(65, 32, 6, 128, 7680 + 65 * 400, True,
                               'adaptive', True, private_upper=True,
                               cache_tag_bytes=8),
            (7680, 0))
        # External-Ubar full tagged cache uses 608 B/cell and falls back to 408 B/cell.
        self.assertEqual(
            line_storage_bytes(65, 16, 2, 128, 1280 + 65 * 608, True,
                               'adaptive', True, private_upper=False,
                               cache_tag_bytes=8),
            (1280, 65 * 608))
        self.assertEqual(
            line_storage_bytes(65, 16, 2, 128, 1280 + 65 * 600, True,
                               'adaptive', True, private_upper=False,
                               cache_tag_bytes=8),
            (7680, 65 * 408))


class CachedUbarReadinessTests(unittest.TestCase):
    def setUp(self):
        self.storage = [dict(token='7', cells='2', cache_bytes='1200',
                             resident='160', reserved='4096', tile_y_only='1')]
        base = 4096 + 160
        self.jobs = {}
        self.completed = {}
        lines = ['CFD_LINE_SCHEDULE token=7 cache_hit=0']
        for cell, cycle in ((0, 5), (1, 6)):
            job = str(10 + cell)
            self.jobs[job] = dict(owner='7', op='W', words='25', stride='8',
                                  base=hex(base + cell*600 + 400))
            self.completed[job] = dict(cycle=str(cycle))
            lines.append(f'CFD_UBAR_CACHE_READY token=7 cell={cell} version=1 '
                         f'cycle={cycle} job={job}')
            # The last point only needs y; its unused Ubar can finish later.
            lines.append(f'CFD_LOCAL_READY token=7 cell={cell} cycle=5 '
                         f'cache_job={job if cell == 0 else 0}')
        for col in range(5):
            job = str(20 + col)
            self.jobs[job] = dict(owner='7', op='R', words='5', stride='40',
                                  base=hex(base + 400 + col*8))
            lines.append(f'CFD_BACK_OPERAND token=7 cell=0 event=read '
                         f'column={col} job={job} cycle=6')
        self.text = '\n'.join(lines)

    def check(self, text=None, jobs=None):
        check_cached_ubar(text or self.text, self.storage,
                          jobs or self.jobs, self.completed)

    def test_current_sweep_can_read_committed_cache_cells(self):
        self.check()

    def test_reject_read_before_cache_write_completion(self):
        with self.assertRaises(AssertionError):
            self.check(text=self.text.replace('job=20 cycle=6', 'job=20 cycle=4'))

    def test_reject_tile_address_used_as_cached_matrix(self):
        jobs = copy.deepcopy(self.jobs)
        jobs['20']['base'] = hex(4096 + 80)
        with self.assertRaises(AssertionError):
            self.check(jobs=jobs)

    def test_reject_local_ready_without_cache_write_dependency(self):
        with self.assertRaises(AssertionError):
            self.check(text=self.text.replace('cache_job=10', 'cache_job=0'))

    def test_reject_missing_or_duplicate_cache_publication(self):
        event = next(line for line in self.text.splitlines()
                     if line.startswith('CFD_UBAR_CACHE_READY token=7 cell=0'))
        for text in (self.text.replace(event, ''), self.text + '\n' + event):
            with self.subTest(text=text), self.assertRaises(AssertionError):
                self.check(text=text)


if __name__ == '__main__':
    unittest.main()
