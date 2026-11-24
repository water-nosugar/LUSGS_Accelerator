"""Adversarial early-b traces with bounded, parked RHS execution credits."""
import _support
import unittest

from check_cold_rhs import check_cold_rhs
from check_input_split import check_input_split
from check_single_line_supply import check_direct_rhs
from memory.test_input_d_early import fixture as input_fixture


def fixture():
    text = ('CFD_DIRECT_RHS cycle=3 token=1 cell=0 event=consume request=10 '
            'generation=20 job=99 dma_done=2 mode=cold')
    config = {'--coeff-cold-rhs-prefetch': '1', '--coeff3-count': '1'}
    rows = []

    def add(stage, cycle, batch=-1, col=-1):
        rows.append(dict(stage=stage, cycle=str(cycle), requestId='10',
                         generation='20', cell='0', rhsBatch=str(batch),
                         rhsColumn=str(col)))

    add('INPUT_R_READY', 3)
    add('LU_ISSUE', 6)
    add('RHS_INPUT_INIT', 10, 0, 0)
    add('COEFF3_ISSUE', 10)
    add('TRSM_DIV_ISSUE', 10, 0, 0)
    add('RHS_COMPLETE', 18, 0, 0)
    add('DIRECT_BASE_READY', 18)
    add('TRSM_INPUT_PARK', 18)
    add('INPUT_TOKEN', 30)
    add('TRSM_INPUT_RESUME', 31)
    for batch in (1, 2):
        for col in range(5):
            add('RHS_INPUT_INIT', 31, batch, col)
            add('TRSM_MUL_ISSUE', 32, batch, col)
            add('RHS_COMPLETE', 35, batch, col)
    add('COEFF3_COMPLETE', 36)
    return text, config, rows


class ColdRhsTests(unittest.TestCase):
    def test_b_can_precede_coefficients_and_park(self):
        result = check_cold_rhs(*fixture())
        self.assertEqual(result['early_b_requests'], 1)
        self.assertEqual(result['early_b_completed'], 1)
        self.assertEqual(result['parked_requests'], 1)
        self.assertEqual(result['peak_contexts'], 1)

    def test_disabled_mode_rejects_cold_events(self):
        text, _, rows = fixture()
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, {}, rows)
        self.assertEqual(check_cold_rhs('', {}, [])['cold_requests'], 0)

    def test_b_initialization_waits_r_register_response(self):
        text, config, rows = fixture()
        for r in rows:
            if r['stage'] == 'INPUT_R_READY':
                r['cycle'] = '11'
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, config, rows)

    def test_coefficients_cannot_initialize_before_full_input(self):
        text, config, rows = fixture()
        next(r for r in rows if r['stage'] == 'RHS_INPUT_INIT'
             and r['rhsBatch'] == '1')['cycle'] = '29'
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, config, rows)

    def test_arithmetic_waits_its_own_initialization(self):
        text, config, rows = fixture()
        next(r for r in rows if r['stage'] == 'TRSM_MUL_ISSUE')['cycle'] = '30'
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, config, rows)

    def test_missing_or_duplicate_initialization_is_rejected(self):
        text, config, rows = fixture()
        row = next(r for r in rows if r['stage'] == 'RHS_INPUT_INIT')
        for changed in ([r for r in rows if r is not row], [*rows, row]):
            with self.assertRaises(AssertionError):
                check_cold_rhs(text, config, changed)

    def test_resume_waits_data_and_a_registered_cycle(self):
        for altered in (17, 29):
            text, config, rows = fixture()
            next(r for r in rows if r['stage'] == 'TRSM_INPUT_RESUME')['cycle'] = str(altered)
            with self.assertRaises(AssertionError):
                check_cold_rhs(text, config, rows)

    def test_park_requires_completed_b(self):
        text, config, rows = fixture()
        next(r for r in rows if r['stage'] == 'TRSM_INPUT_PARK')['cycle'] = '17'
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, config, rows)

    def test_parked_request_cannot_issue_arithmetic(self):
        text, config, rows = fixture()
        rows.append(dict(stage='TRSM_DIV_ISSUE', cycle='20', requestId='10',
                         generation='20', cell='0', rhsBatch='0', rhsColumn='0'))
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, config, rows)

    def test_contexts_remain_bounded(self):
        text, config, rows = fixture()
        text += '\n'+text.replace('request=10', 'request=11').replace('cell=0', 'cell=1').replace('generation=20', 'generation=21')
        rows += [{**r, 'requestId': '11', 'cell': '1', 'generation': '21'} for r in rows.copy()]
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, config, rows)

    def test_cancel_after_park_closes_credit_without_resume(self):
        text, config, rows = fixture()
        rows = [r for r in rows if int(r['cycle']) <= 18]
        rows.append(dict(stage='CANCEL_PENDING', cycle='19', requestId='10',
                         generation='20', cell='0', rhsBatch='-1', rhsColumn='-1'))
        result = check_cold_rhs(text, config, rows, allow_abort=True)
        self.assertEqual(result['parked_requests'], 1)

    def test_success_requires_context_completion(self):
        text, config, rows = fixture()
        rows = [r for r in rows if r['stage'] != 'COEFF3_COMPLETE']
        with self.assertRaises(AssertionError):
            check_cold_rhs(text, config, rows)


class ColdRhsIngressTests(unittest.TestCase):
    def fixture(self):
        text = '''CFD_LINE_SCHEDULE token=1 cache_hit=0
CFD_LINE_STORAGE token=1 cells=1 tile_y_only=0
CFD_DIRECT_RHS cycle=0 token=1 cell=0 event=allocate slot=0 depth=1 mode=cold
CFD_CACHE_DMA cycle=0 token=1 event=issue kind=rhs cell=0 bytes=40 packet=3 job=1
CFD_DMA_PACKET tick=4000 event=response packet=3
CFD_ONCHIP_START cycle=0 id=1 owner=1 base=0x1000 op=W active=0 words=5
CFD_ONCHIP_ACTIVATE cycle=4 id=1
CFD_ONCHIP_COMPLETE cycle=5 id=1
CFD_CACHE_DMA cycle=5 token=1 event=response kind=rhs cell=0 bytes=40 packet=3 job=1
CFD_ONCHIP_START cycle=5 id=2 owner=1 base=0x1000 op=R active=1 words=5 stride=8
CFD_ONCHIP_COMPLETE cycle=7 id=2
CFD_DIRECT_RHS cycle=7 token=1 cell=0 event=consume request=1 generation=9 job=2 dma_done=5 mode=cold'''
        config = {'--coeff-cold-rhs-prefetch': '1', '--coeff-rhs-lookahead': '1',
                  '--coeff-stream-window': '1', '--coeff-base-solve': 'direct'}
        trace = [dict(stage='INPUT_R_READY', cycle='7', requestId='1',
                      generation='9', cell='0', packetId='2')]
        return text, config, trace

    def test_single_cell_r_uses_completed_landing_read(self):
        self.assertEqual(check_direct_rhs(*self.fixture())['consumed'], 1)

    def test_r_read_waits_its_spm_store(self):
        text, config, trace = self.fixture()
        with self.assertRaises(AssertionError):
            check_direct_rhs(text.replace('START cycle=5 id=2',
                                          'START cycle=4 id=2'), config, trace)

    def test_r_load_cannot_use_another_landing_or_owner(self):
        text, config, trace = self.fixture()
        for bad in ('owner=1 base=0x1008', 'owner=2 base=0x1000'):
            with self.assertRaises(AssertionError):
                check_direct_rhs(text.replace('id=2 owner=1 base=0x1000',
                                              'id=2 '+bad), config, trace)

    def test_single_cell_d_has_no_empty_remaining_load(self):
        text, config, trace, jobs, ends = input_fixture()
        config['--coeff-cold-rhs-prefetch'] = '1'
        text = '\n'.join(line for line in text.splitlines()
                         if 'event=rest_read' not in line).replace(
                             'event=input_ready slot=0 job=101',
                             'event=input_ready slot=0 job=100')
        text += ('\nCFD_DIRECT_RHS cycle=14 token=7 cell=0 event=consume '
                 'request=1 generation=10 job=102 dma_done=10 mode=cold')
        trace = [r for r in trace if r['packetId'] != '5']
        trace.append(dict(requestId='1', generation='10', cell='0',
                          stage='INPUT_R_READY', cycle='14', packetId='102'))
        next(r for r in trace if r['stage']=='COEFF3_ISSUE')['cycle']='16'
        del jobs['101'], ends['101']
        result = check_input_split(text, config, trace, jobs, ends)
        self.assertEqual(result['requests'], 1)
        self.assertEqual(result['added_load_jobs'], 1)


if __name__ == '__main__':
    unittest.main()
