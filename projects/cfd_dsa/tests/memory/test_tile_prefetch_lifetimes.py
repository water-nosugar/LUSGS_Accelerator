import _support
from pathlib import Path
import unittest

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


if __name__=='__main__':unittest.main()
