import _support
from pathlib import Path
import tempfile
import unittest

from check_single_line_c import check_numeric
from check_single_line_storage import check_prefetch_lifetimes


class PrefetchOwnershipTests(unittest.TestCase):
    config={'--coeff-tile-prefetch-buffers':'1','--coeff-line-tile-buffers':'2'}

    def fixture(self):
        lines=['CFD_LINE_STORAGE token=1 tile_y_only=0',
            'CFD_LOCAL_READY token=1 first=48 storage=0',
            'CFD_TILE_EVENT token=1 event=forward_commit first=48']
        for block,first in enumerate([16,0]):
            lines.append(f'CFD_TILE_EVENT token=1 event=reload_start first={first} cells=16 storage=0')
            for packet in range(60):
                identity=block*60+packet
                for event in ['issue','response']:
                    lines.append(f'CFD_TILE_DMA token=1 event={event} first={first} packet={identity} bytes=64')
        return '\n'.join(lines)

    def test_complete_range_can_reuse_a_fixed_buffer(self):
        check_prefetch_lifetimes(self.fixture(),self.config)

    def test_forward_write_fence_is_required(self):
        with self.assertRaisesRegex(AssertionError,'forward tile overwritten'):
            check_prefetch_lifetimes(self.fixture().replace(
                'event=forward_commit first=48','event=ignored first=48'),self.config)

    def test_pending_landing_prevents_reuse(self):
        text=self.fixture().replace('CFD_TILE_DMA token=1 event=response first=16 packet=59 bytes=64','')
        with self.assertRaisesRegex(AssertionError,'tile overwritten before all DMA/SPM landings'):
            check_prefetch_lifetimes(text,self.config)

    def test_invalid_physical_buffer_identity_is_rejected(self):
        with self.assertRaises(AssertionError):
            check_prefetch_lifetimes(self.fixture().replace('storage=0','storage=3'),self.config)

    def test_forward_rotation_requires_the_previous_writeback(self):
        config = dict(self.config, **{'--coeff-line-tile-cells': '1'})
        lines = ['CFD_LINE_STORAGE token=1 tile_y_only=1 cells=4']
        for cell in range(4):
            storage = cell % 3
            identity = f'first={cell} cells=1 storage={storage}'
            lines.append(f'CFD_TILE_EVENT token=1 event=forward_allocate {identity}')
            lines.append(f'CFD_LOCAL_READY token=1 cell={cell} first={cell} storage={storage}')
            lines.append(f'CFD_TILE_EVENT token=1 event=forward_commit {identity}')
        lines.append('CFD_TILE_EVENT token=1 event=reload_start first=0 cells=1 storage=0')
        for event in ('issue', 'response'):
            lines.append(f'CFD_TILE_DMA token=1 event={event} first=0 packet=1 bytes=40')
        text = '\n'.join(lines)
        check_prefetch_lifetimes(text, config)
        with self.assertRaisesRegex(AssertionError, 'forward tile overwritten'):
            check_prefetch_lifetimes(text.replace('event=forward_commit first=0',
                                                  'event=ignored first=0'), config)

    def test_zeroed_or_noncontiguous_reload_offsets_are_rejected(self):
        config = {'--coeff-tile-prefetch-buffers': '1', '--coeff-line-tile-buffers': '2',
                  '--coeff-line-tile-cells': '2', '--lusgs-cells': '8'}
        lines = [
            'CFD_SME_CONFIG token=1 start=10 done=200',
            'CFD_LINE_STORAGE token=1 tile_y_only=1 cells=8',
            'CFD_TILE_EVENT cycle=20 token=1 event=forward_allocate first=0 cells=2 storage=0',
            'CFD_LOCAL_READY cycle=25 token=1 cell=0 first=0 storage=0',
            'CFD_LOCAL_READY cycle=26 token=1 cell=1 first=0 storage=0',
            'CFD_TILE_EVENT cycle=30 token=1 event=forward_commit first=0 cells=2 storage=0 last_write=28',
            'CFD_TILE_EVENT cycle=31 token=1 event=forward_allocate first=2 cells=2 storage=1',
            'CFD_LOCAL_READY cycle=35 token=1 cell=2 first=2 storage=1',
            'CFD_LOCAL_READY cycle=36 token=1 cell=3 first=2 storage=1',
            'CFD_TILE_EVENT cycle=40 token=1 event=forward_commit first=2 cells=2 storage=1 last_write=38',
            'CFD_TILE_EVENT cycle=41 token=1 event=forward_allocate first=4 cells=2 storage=2',
            'CFD_LOCAL_READY cycle=45 token=1 cell=4 first=4 storage=2',
            'CFD_LOCAL_READY cycle=46 token=1 cell=5 first=4 storage=2',
            'CFD_TILE_EVENT cycle=50 token=1 event=forward_commit first=4 cells=2 storage=2 last_write=48',
            'CFD_TILE_EVENT cycle=51 token=1 event=forward_allocate first=6 cells=2 storage=0',
            'CFD_LOCAL_READY cycle=55 token=1 cell=6 first=6 storage=0',
            'CFD_LOCAL_READY cycle=56 token=1 cell=7 first=6 storage=0',
            'CFD_TILE_EVENT cycle=59 token=1 event=forward_commit first=6 cells=2 storage=0 last_write=58',
            'CFD_TILE_EVENT cycle=60 token=1 event=reload_start first=0 cells=2 storage=1',
            'CFD_TILE_DMA cycle=61 token=1 event=issue first=0 packet=1 offset=0 bytes=64 region=0 job=10',
            'CFD_TILE_DMA cycle=62 token=1 event=issue first=0 packet=2 offset=64 bytes=16 region=0 job=10',
            'CFD_TILE_DMA cycle=70 token=1 event=response first=0 packet=1 offset=0 bytes=64 region=0 job=10',
            'CFD_TILE_DMA cycle=71 token=1 event=response first=0 packet=2 offset=64 bytes=16 region=0 job=10',
        ]
        csv_text = 'cycle,stage,cell\n' + '\n'.join(
            f'{27 + (c // 2) * 10 + (c % 2)},DRAIN_DQSTAR_VECTOR_DONE,{c}' for c in range(8)
        ) + '\n'
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'coeff_preprocess_trace.csv').write_text(csv_text)
            valid = '\n'.join(lines)
            check_prefetch_lifetimes(valid, config, directory=d)
            # Probe 1: zero out the nonzero offset while keeping total bytes=80
            zeroed = valid.replace('offset=64 bytes=16', 'offset=0 bytes=16')
            with self.assertRaisesRegex(AssertionError, 'non-contiguous or duplicate reload DMA offset'):
                check_prefetch_lifetimes(zeroed, config, directory=d)
            # Probe 1b: invert forward_allocate timestamp past CFD_LOCAL_READY / forward_commit
            inverted = valid.replace('cycle=51 token=1 event=forward_allocate',
                                     'cycle=80 token=1 event=forward_allocate')
            with self.assertRaisesRegex(AssertionError, 'timestamp preceded forward_allocate'):
                check_prefetch_lifetimes(inverted, config, directory=d)
            # Probe 2: remove independent write evidence + last_write
            stripped_writes = valid.replace(' last_write=28', '').replace(
                ' last_write=38', '').replace(' last_write=48', '').replace(' last_write=58', '')
            (d / 'coeff_preprocess_trace.csv').write_text('cycle,stage,cell\n')
            with self.assertRaises(AssertionError):
                check_prefetch_lifetimes(stripped_writes, config, directory=d)
            # Also reject missing trace write evidence even if last_write stays
            with self.assertRaisesRegex(AssertionError, 'lacks independent write completion evidence'):
                check_prefetch_lifetimes(valid, config, directory=d)

    def test_late_allocation_after_abort_begin_is_rejected(self):
        config = {'--coeff-tile-prefetch-buffers': '1', '--coeff-line-tile-buffers': '2',
                  '--coeff-line-tile-cells': '2', '--lusgs-cells': '4'}
        lines = '\n'.join([
            'CFD_LINE_STORAGE token=1 tile_y_only=1 cells=4',
            'CFD_TILE_EVENT cycle=100 token=1 event=forward_allocate first=0 cells=2 storage=0',
            'CFD_TILE_EVENT cycle=151 token=1 event=forward_allocate first=2 cells=2 storage=1',
        ])
        aborted = {'1': {'token': '1', 'cycle': '200'}}
        abort_start = {'1': {'token': '1', 'cycle': '150'}}
        with self.assertRaisesRegex(AssertionError, 'after abort begin'):
            check_prefetch_lifetimes(lines, config, aborted=aborted, abort_start=abort_start)

    def test_zero_or_mismatched_output_hash_is_rejected(self):
        valid = '\n'.join([
            'CFD_LINE_SCHEDULE token=1 result_hash_source=guest output_digest=123456789abcdef0',
            'CFD_SINGLE_LINE_NUMERIC version=1 base_solve=direct values=10 max_abs=0 max_rel=0 '
            'mismatches=0 result_hash=49931eda7a92f34a output_digest=123456789abcdef0 residual_max=0',
        ])
        self.assertEqual(check_numeric(valid, [1]), 0.0)
        with self.assertRaisesRegex(ValueError, 'invalid or degenerate result_hash'):
            check_numeric(valid.replace('49931eda7a92f34a', '0000000000000000'), [1])
        with self.assertRaisesRegex(ValueError, 'output_digest disagrees'):
            check_numeric(valid.replace('output_digest=123456789abcdef0 residual_max=0',
                                        'output_digest=fedcba9876543210 residual_max=0'), [1])


if __name__=='__main__':unittest.main()
