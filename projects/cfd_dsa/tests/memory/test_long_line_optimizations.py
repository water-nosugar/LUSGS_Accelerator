import _support
from pathlib import Path
import unittest

from check_single_line_onchip import check_onchip_credits
from check_single_line_supply import check_streamed_tiles, check_output_bursts


class ComputeCreditTests(unittest.TestCase):
    def test_compute_uses_reserved_slot_while_dma_waits(self):
        text='''CFD_ONCHIP_START cycle=1 id=1 active=0
CFD_ONCHIP_START cycle=2 id=2 active=1
CFD_ONCHIP_COMPLETE cycle=4 id=2
CFD_ONCHIP_ACTIVATE cycle=5 id=1
CFD_ONCHIP_COMPLETE cycle=7 id=1
'''
        check_onchip_credits(text,2,1)
        with self.assertRaisesRegex(AssertionError,'reserved compute'):
            check_onchip_credits(text.replace('id=2 active=1','id=2 active=0'),2,1)

    def test_cancel_returns_pending_credit_and_missing_activation_fails(self):
        text='''CFD_ONCHIP_START cycle=1 id=1 active=0
CFD_ONCHIP_CANCEL cycle=2 id=1
CFD_ONCHIP_START cycle=3 id=2 active=0
CFD_ONCHIP_ACTIVATE cycle=4 id=2
CFD_ONCHIP_COMPLETE cycle=6 id=2
'''
        check_onchip_credits(text,2,1)
        with self.assertRaises(AssertionError):
            check_onchip_credits(text.replace('CFD_ONCHIP_ACTIVATE cycle=4 id=2\n',''),2,1)


class MatrixTileStreamTests(unittest.TestCase):
    config={'--coeff-tile-reload':'reverse-stream','--coeff-tile-operand-stream':'1'}

    def fixture(self):
        rows=['CFD_LINE_STORAGE token=1 cells=2 tile_y_only=0',
              'CFD_TILE_EVENT cycle=1 token=1 event=reload_start first=0 cells=2 stream=1']
        packet=0
        times={}
        for reg,size in ((0,80),(1,400)):
            remaining=size
            while remaining:
                n=min(64,remaining);offset=remaining-n;remaining-=n;packet+=1
                end=20 if reg==0 and offset else 25 if reg==0 else 10+3*(packet-2)
                rows.extend([
                    f'CFD_TILE_DMA cycle=1 token=1 event=issue first=0 region={reg} bytes={n} packet={packet} offset={offset} job={packet}',
                    f'CFD_ONCHIP_START cycle=1 id={packet} op=W words={n//8}',
                    f'CFD_DMA_PACKET tick={(end-2)*1000} event=response packet={packet}',
                    f'CFD_ONCHIP_ACTIVATE cycle={end-2} id={packet}',
                    f'CFD_ONCHIP_COMPLETE cycle={end} id={packet}',
                    f'CFD_TILE_DMA cycle={end} token=1 event=response first=0 region={reg} bytes={n} packet={packet} offset={offset} job={packet}'])
                for w in range(offset//8,(offset+n)//8):times[reg,w]=end
        col0=max(times[1,25+r*5] for r in range(5))
        rows.extend(['CFD_TILE_CELL_READY cycle=20 token=1 first=0 cell=1',
                     'CFD_TILE_CELL_READY cycle=25 token=1 first=0 cell=0',
                     'CFD_TILE_EVENT cycle=40 token=1 event=reload_ready first=0 cells=2',
                     'CFD_BACK_OPERAND cycle=21 token=1 cell=1 event=allocate',
                     f'CFD_BACK_OPERAND cycle={max(20,col0)} token=1 cell=1 event=read column=0'])
        return '\n'.join(rows)

    def test_column_waits_for_both_regions_without_whole_tile_barrier(self):
        self.assertEqual(check_streamed_tiles(self.fixture(),self.config),
                         dict(tiles=1,cells=2,early_cells=1))

    def test_reject_early_matrix_read_and_omitted_matrix_word(self):
        text=self.fixture();read=text.splitlines()[-1]
        for broken in (text.replace(read,'CFD_BACK_OPERAND cycle=20 token=1 cell=1 event=read column=0'),
                       text.replace('region=1 bytes=16','region=1 bytes=8')):
            with self.assertRaises(AssertionError):check_streamed_tiles(broken,self.config)


class OutputBurstTests(unittest.TestCase):
    config={'--coeff-output-coalesce':'1','--coeff-drain-width':'64'}

    def fixture(self):
        return '''CFD_LINE_STORAGE token=1 cells=2
CFD_LINE_SCHEDULE token=1 visible_done=50 output_packets_issued=2
CFD_OUTPUT_BURST cycle=1 token=1 packet=1 event=issue addr=0x1000 bytes=64 members=2
CFD_OUTPUT_BURST cycle=2 token=1 packet=2 event=issue addr=0x1040 bytes=16 members=1
CFD_OUTPUT_SLICE cycle=1 token=1 packet=1 cell=0 offset=0 payload_offset=0 bytes=40
CFD_OUTPUT_SLICE cycle=1 token=1 packet=1 cell=1 offset=0 payload_offset=40 bytes=24
CFD_OUTPUT_SLICE cycle=2 token=1 packet=2 cell=1 offset=24 payload_offset=0 bytes=16
CFD_DMA_PACKET tick=1000 event=queue packet=1 op=W bytes=64
CFD_DMA_PACKET tick=2000 event=queue packet=2 op=W bytes=16
CFD_DMA_PACKET tick=20000 event=response packet=1
CFD_DMA_PACKET tick=21000 event=response packet=2
CFD_OUTPUT_BURST cycle=20 token=1 packet=1 event=response addr=0x1000 bytes=64 members=2
CFD_OUTPUT_BURST cycle=21 token=1 packet=2 event=response addr=0x1040 bytes=16 members=1
'''

    def test_exact_coverage_and_real_response(self):
        self.assertEqual(check_output_bursts(self.fixture(),self.config),dict(packets=2,merged=1))

    def test_reject_overlap_wrong_physical_bytes_and_early_visibility(self):
        text=self.fixture()
        for broken in (text.replace('cell=1 offset=24','cell=1 offset=16'),
                       text.replace('op=W bytes=64','op=W bytes=56'),
                       text.replace('visible_done=50','visible_done=20')):
            with self.assertRaises(AssertionError):check_output_bursts(broken,self.config)


if __name__=='__main__':unittest.main()
