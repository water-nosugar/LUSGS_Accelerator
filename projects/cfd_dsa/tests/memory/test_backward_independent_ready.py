"""Adversarial traces for independently arriving backward y and Ubar."""
import _support
import unittest

from check_single_line_onchip import check_backward_operands
from check_single_line_supply import check_streamed_tiles


def fixture(independent=True):
    config = {'--lusgs-cells': '2', '--coeff-back-operand-depth': '2',
              '--coeff-back-independent-ready': str(int(independent))}
    jobs = {'1': dict(owner='1', op='R', words='5', stride='8', cycle='1')}
    completed = {'1': {'cycle': '20'}}
    events = ['CFD_LINE_SCHEDULE token=1',
              'CFD_BACK_OPERAND cycle=0 token=1 cell=0 event=allocate slot=0',
              'CFD_BACK_OPERAND cycle=1 token=1 cell=0 event=read column=-1 job=1']
    for col in range(5):
        ident = str(col+2)
        jobs[ident] = dict(owner='1', op='R', words='5', stride='40', cycle='1')
        completed[ident] = {'cycle': '5'}
        events.append(f'CFD_BACK_OPERAND cycle=1 token=1 cell=0 event=read column={col} job={ident}')
    for col in range(5):
        cycle, yjob = (6, 0) if independent else (21, 1)
        events.append(f'CFD_BACK_OPERAND cycle={cycle} token=1 cell=0 event=use column={col} job={col+2} y_job={yjob}')
    for col in range(5):
        events.append(f'CFD_MVM_PRODUCT cycle={6+col if independent else 21+col} token=1 context=1 cell=0 event=issue column={col}')
    if independent:
        events.append('CFD_BACK_OPERAND cycle=21 token=1 cell=0 event=y_use job=1')
    events.extend(['CFD_MVM_FEEDBACK cycle=22 token=1 context=1 cell=0 event=issue node=0',
                   'CFD_BACK_OPERAND cycle=30 token=1 cell=0 event=release'])
    return '\n'.join(events), config, jobs, completed


class BackwardIndependentReadyTests(unittest.TestCase):
    def test_products_can_precede_y_but_y_consumer_waits(self):
        text, config, jobs, completed = fixture()
        result = check_backward_operands(text, config, jobs, completed)
        self.assertEqual(result['early_product_columns'], 5)
        self.assertEqual(result['y_consumptions'], 1)

    def test_legacy_contract_still_requires_both_operands(self):
        text, config, jobs, completed = fixture(False)
        check_backward_operands(text, config, jobs, completed)
        text, _, jobs, completed = fixture()
        with self.assertRaises(AssertionError):
            check_backward_operands(text, config, jobs, completed)

    def test_reject_product_before_column_response(self):
        text, config, jobs, completed = fixture()
        text = text.replace('cycle=6 token=1 cell=0 event=use column=0',
                            'cycle=4 token=1 cell=0 event=use column=0')
        with self.assertRaises(AssertionError):
            check_backward_operands(text, config, jobs, completed)

    def test_reject_y_consumer_before_y_response_or_publication(self):
        text, config, jobs, completed = fixture()
        for altered in (text.replace('CFD_MVM_FEEDBACK cycle=22', 'CFD_MVM_FEEDBACK cycle=19'),
                        text.replace('cycle=21 token=1 cell=0 event=y_use',
                                     'cycle=19 token=1 cell=0 event=y_use')):
            with self.assertRaises(AssertionError):
                check_backward_operands(altered, config, jobs, completed)

    def test_reject_missing_or_duplicated_y_consumption(self):
        text, config, jobs, completed = fixture()
        event = 'CFD_BACK_OPERAND cycle=21 token=1 cell=0 event=y_use job=1'
        for altered in (text.replace(event, ''), text.replace(event, event+'\n'+event)):
            with self.assertRaises(AssertionError):
                check_backward_operands(altered, config, jobs, completed)

    def test_reject_slot_reuse_before_y_read_completes(self):
        text, config, jobs, completed = fixture()
        completed = {**completed, '1': {'cycle': '31'}}
        with self.assertRaises(AssertionError):
            check_backward_operands(text, config, jobs, completed)


class IndependentReloadTests(unittest.TestCase):
    config = {'--coeff-back-independent-ready': '1',
              '--coeff-tile-reload': 'reverse-stream',
              '--coeff-tile-operand-stream': '1'}

    def fixture(self):
        lines = ['CFD_LINE_STORAGE token=1 tile_y_only=0',
                 'CFD_TILE_EVENT cycle=0 token=1 event=reload_start first=0 cells=1 stream=1']
        for ident, region, offset, size, end in (
                (1, 1, 136, 64, 10), (2, 1, 72, 64, 11),
                (3, 1, 8, 64, 12), (4, 1, 0, 8, 13), (5, 0, 0, 40, 30)):
            lines.extend([
                f'CFD_ONCHIP_START cycle=0 id={ident} op=W words={size//8} active=0',
                f'CFD_TILE_DMA cycle=0 token=1 event=issue first=0 region={region} offset={offset} bytes={size} packet={ident} job={ident}',
                f'CFD_DMA_PACKET tick={ident*1000} event=response packet={ident}',
                f'CFD_ONCHIP_ACTIVATE cycle={ident} id={ident}',
                f'CFD_ONCHIP_COMPLETE cycle={end} id={ident}',
                f'CFD_TILE_DMA cycle={end} token=1 event=response first=0 region={region} offset={offset} bytes={size} packet={ident} job={ident}'])
        lines.extend(['CFD_TILE_CELL_READY cycle=30 token=1 first=0 cell=0',
                      'CFD_TILE_EVENT cycle=31 token=1 event=reload_ready first=0 cells=1',
                      'CFD_BACK_OPERAND cycle=1 token=1 cell=0 event=allocate',
                      'CFD_BACK_OPERAND cycle=13 token=1 cell=0 event=read column=0 job=10',
                      'CFD_BACK_OPERAND cycle=30 token=1 cell=0 event=read column=-1 job=11'])
        return '\n'.join(lines)

    def test_matrix_column_can_be_read_while_y_is_in_flight(self):
        self.assertEqual(check_streamed_tiles(self.fixture(), self.config)['early_cells'], 1)

    def test_each_source_keeps_its_own_spm_store_fence(self):
        text = self.fixture()
        for altered in (text.replace('cycle=13 token=1 cell=0 event=read column=0',
                                     'cycle=12 token=1 cell=0 event=read column=0'),
                        text.replace('cycle=30 token=1 cell=0 event=read column=-1',
                                     'cycle=29 token=1 cell=0 event=read column=-1')):
            with self.assertRaises(AssertionError):
                check_streamed_tiles(altered, self.config)

    def test_legacy_reload_still_waits_for_y(self):
        with self.assertRaises(AssertionError):
            check_streamed_tiles(self.fixture(), {**self.config, '--coeff-back-independent-ready': '0'})

    def ordered_fixture(self):
        text = self.fixture()+'\nCFD_TILE_READ_POLICY token=1 cell_order=1 vector_bytes=40 matrix_bytes=200'
        for packet,u in ((1,0),(2,64),(3,128),(4,192)):
            text+=f'\nCFD_TILE_ORDER cycle=0 token=1 first=0 packet={packet} y_issued=0 u_issued={u} preferred=0 pass=1 region=1'
        text+='\nCFD_TILE_ORDER cycle=0 token=1 first=0 packet=5 y_issued=0 u_issued=200 preferred=0 pass=0 region=0'
        return text

    def test_ordering_records_match_real_dma_and_bounded_fallbacks(self):
        result=check_streamed_tiles(self.ordered_fixture(), {**self.config, '--coeff-tile-cell-order': '1'})
        self.assertEqual(result['read_order'], dict(packets=5,second_pass_packets=4))

    def test_reject_invented_cell_coverage_or_duplicate_policy(self):
        text=self.ordered_fixture();config={**self.config, '--coeff-tile-cell-order': '1'}
        for altered in (text.replace('u_issued=0 preferred=0', 'u_issued=0 preferred=1'),
                        text.replace('u_issued=64 preferred=0', 'u_issued=128 preferred=0'),
                        text+'\nCFD_TILE_READ_POLICY token=1 cell_order=1 vector_bytes=40 matrix_bytes=200'):
            with self.assertRaises(AssertionError):check_streamed_tiles(altered,config)


if __name__ == '__main__':
    unittest.main()
