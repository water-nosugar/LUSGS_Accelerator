import _support
import copy
import unittest
from pathlib import Path

from check_single_line_supply import check_rhs_bursts, check_streamed_tiles, check_direct_rhs


class InputBurstTests(unittest.TestCase):
    def fixture(self):
        text='''CFD_RHS_BURST cycle=1 packet=7 event=issue addr=0x1000 bytes=64 members=2
CFD_DMA_PACKET tick=1000 event=queue packet=7 op=R bytes=64 live=1
CFD_DMA_PACKET tick=10000 event=response packet=7
CFD_RHS_BURST cycle=10 packet=7 event=response addr=0x1000 bytes=64 members=2
CFD_RHS_SLICE cycle=1 packet=7 request=1 generation=1 cell=1 offset=0 payload_offset=0 bytes=40 job=1 slot=0
CFD_RHS_SLICE cycle=1 packet=7 request=2 generation=2 cell=2 offset=0 payload_offset=40 bytes=24 job=2 slot=1
CFD_ONCHIP_START cycle=1 id=1 base=0x0 words=5 stride=8 op=W active=0
CFD_ONCHIP_START cycle=1 id=2 base=0x2000 words=3 stride=8 op=W active=0
CFD_ONCHIP_ACTIVATE cycle=10 id=1
CFD_ONCHIP_ACTIVATE cycle=10 id=2
CFD_ONCHIP_COMPLETE cycle=13 id=1
CFD_ONCHIP_COMPLETE cycle=14 id=2
'''
        trace=[]
        for req,cycle,size in [('1',13,40),('2',14,24)]:
            trace.extend([dict(requestId=req,generation=req,cell=req,address=hex(0x1000+(40 if req=='2' else 0)),stage='SPM_PACKET_RESPONSE',requester='CoeffInput',packetId='7',cycle=str(cycle),size=str(size)),
                          dict(requestId=req,stage='INPUT_TOKEN',cycle=str(cycle+1))])
        return text,trace

    def test_two_request_fragments_share_one_physical_response(self):
        text,trace=self.fixture()
        self.assertEqual(check_rhs_bursts(text,{'--coeff-rhs-input-coalesce':'1'},trace),dict(packets=1,logical_fragments=2))

    def test_reject_slice_overlap_early_dma_and_early_operand(self):
        text,trace=self.fixture(); config={'--coeff-rhs-input-coalesce':'1'}
        for broken in (text.replace('payload_offset=40','payload_offset=32'),
                       text.replace('tick=10000 event=response','tick=11000 event=response')):
            with self.assertRaises(AssertionError):check_rhs_bursts(broken,config,trace)
        broken=copy.deepcopy(trace);broken[-1]['cycle']='10'
        with self.assertRaises(AssertionError):check_rhs_bursts(text,config,broken)


class ReverseTileTests(unittest.TestCase):
    def fixture(self):
        return '''CFD_TILE_EVENT cycle=1 token=1 event=reload_start first=0 cells=2 stream=1
CFD_TILE_DMA cycle=1 token=1 event=issue first=0 region=0 bytes=64 packet=1 offset=16 job=1
CFD_TILE_DMA cycle=2 token=1 event=issue first=0 region=0 bytes=16 packet=2 offset=0 job=2
CFD_ONCHIP_START cycle=1 id=1 op=W words=8
CFD_ONCHIP_START cycle=2 id=2 op=W words=2
CFD_DMA_PACKET tick=10000 event=response packet=1
CFD_DMA_PACKET tick=15000 event=response packet=2
CFD_ONCHIP_ACTIVATE cycle=10 id=1
CFD_ONCHIP_ACTIVATE cycle=15 id=2
CFD_ONCHIP_COMPLETE cycle=20 id=1
CFD_ONCHIP_COMPLETE cycle=25 id=2
CFD_TILE_DMA cycle=20 token=1 event=response first=0 region=0 bytes=64 packet=1 offset=16 job=1
CFD_TILE_DMA cycle=25 token=1 event=response first=0 region=0 bytes=16 packet=2 offset=0 job=2
CFD_TILE_CELL_READY cycle=20 token=1 first=0 cell=1
CFD_TILE_CELL_READY cycle=25 token=1 first=0 cell=0
CFD_TILE_EVENT cycle=26 token=1 event=reload_ready first=0 cells=2
CFD_BACK_OPERAND cycle=21 token=1 cell=1 event=allocate
'''

    def test_use_high_cell_before_whole_tile_finishes(self):
        self.assertEqual(check_streamed_tiles(self.fixture(),{'--coeff-tile-reload':'reverse-stream'}),dict(tiles=1,cells=2,early_cells=1))

    def test_reject_partial_cell_missing_word_and_early_use(self):
        text=self.fixture();config={'--coeff-tile-reload':'reverse-stream'}
        for broken in (text.replace('cycle=25 token=1 first=0 cell=0','cycle=20 token=1 first=0 cell=0'),
                       text.replace('cycle=21 token=1 cell=1','cycle=19 token=1 cell=1'),
                       text.replace('packet=2 offset=0','packet=2 offset=8'),
                       text.replace('cycle=15 id=2','cycle=14 id=2')):
            with self.assertRaises(AssertionError):check_streamed_tiles(broken,config)


class DirectRhsPrefetchTests(unittest.TestCase):
    config={'--coeff-rhs-lookahead':'2','--coeff-stream-window':'9','--coeff-base-solve':'direct'}

    def fixture(self):
        text='''CFD_DIRECT_RHS cycle=0 token=1 cell=0 event=allocate slot=0 depth=2
CFD_DIRECT_RHS cycle=0 token=1 cell=1 event=allocate slot=1 depth=2
CFD_CACHE_DMA cycle=0 token=1 event=issue kind=rhs cell=0 bytes=40 packet=3 job=1
CFD_DMA_PACKET tick=4000 event=response packet=3
CFD_ONCHIP_START cycle=0 id=1 op=W active=0 words=5
CFD_ONCHIP_ACTIVATE cycle=4 id=1
CFD_ONCHIP_COMPLETE cycle=5 id=1
CFD_CACHE_DMA cycle=5 token=1 event=response kind=rhs cell=0 bytes=40 packet=3 job=1
CFD_ONCHIP_START cycle=5 id=2 op=R words=5 stride=8
CFD_ONCHIP_COMPLETE cycle=7 id=2
CFD_DIRECT_RHS cycle=7 token=1 cell=0 event=consume request=1 generation=9 job=2 dma_done=5
CFD_DIRECT_RHS cycle=8 token=1 cell=1 event=discard
'''
        return text,[dict(requestId='1',generation='9',cell='0',stage='INPUT_TOKEN',cycle='7')]

    def test_consumption_and_cancel_release_bounded_slots(self):
        text,trace=self.fixture()
        self.assertEqual(check_direct_rhs(text,self.config,trace,allow_abort=True),
                         dict(allocated=2,consumed=1,discarded=1,peak=2))

    def test_reject_early_read_and_ring_overflow(self):
        text,trace=self.fixture()
        for broken in (text.replace('cycle=7 id=2','cycle=9 id=2'),
                       text.replace('cycle=5 id=2','cycle=4 id=2'),
                       'CFD_DIRECT_RHS cycle=0 token=1 cell=2 event=allocate slot=2 depth=2\n'+text):
            with self.assertRaises(AssertionError):check_direct_rhs(broken,self.config,trace,allow_abort=True)

    def test_reject_missing_cancel_release(self):
        text,trace=self.fixture()
        with self.assertRaises(AssertionError):
            check_direct_rhs(text.replace('CFD_DIRECT_RHS cycle=8 token=1 cell=1 event=discard',''),self.config,trace,allow_abort=True)

    def test_line_length_clamps_window_and_prefetch_depth(self):
        text,trace=self.fixture()
        text+='CFD_LINE_STORAGE token=1 cells=2 tile_y_only=1\n'
        config={**self.config,'--coeff-rhs-lookahead':'8'}
        self.assertEqual(check_direct_rhs(text,config,trace,allow_abort=True)['peak'],2)


if __name__=='__main__':unittest.main()
