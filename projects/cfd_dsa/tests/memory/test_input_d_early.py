"""Independent ingress fences: D may feed LU while other inputs remain live."""
import _support
import unittest

from check_input_split import check_input_split


def fixture():
    config = {'--coeff-input-d-early': '1'}
    events = []
    trace = []

    def event(stage, cycle, **extra):
        trace.append(dict(requestId='1', generation='10', cell='0',
                          stage=stage, cycle=str(cycle), slot='0',
                          packetId='0', **extra))

    # D occupies the first 200 bytes, even when DMA responses are reordered.
    for packet, size, done in ((1, 64, 5), (2, 64, 7),
                               (3, 64, 6), (4, 8, 8), (5, 40, 30)):
        for stage, cycle in (('SPM_PACKET_ISSUE', 1),
                             ('SPM_PACKET_RESPONSE', done)):
            r = dict(requestId='1', generation='10', cell='0', stage=stage,
                     cycle=str(cycle), slot='0', packetId=str(packet),
                     requester='CoeffInput', size=str(size))
            trace.append(r)
    event('INPUT_START', 0)
    event('INPUT_D_READY', 15)
    trace[-1]['packetId'] = '100'
    event('LU_ISSUE', 16)
    event('INPUT_TOKEN', 35)
    event('INPUT_SLOT_RELEASE', 35)
    event('COEFF3_ISSUE', 36)
    for kind, cycle, job in (('d_read', 8, '100'), ('d_ready', 15, '100'),
                             ('rest_read', 30, '101'), ('input_ready', 35, '101')):
        events.append(f'CFD_INPUT_SPLIT cycle={cycle} request=1 generation=10 '
                      f'cell=0 owner=7 event={kind} slot=0 job={job} bytes=240')
    jobs = {
        '100': dict(cycle='8', owner='7', op='R', active='1', base='0x0',
                    words='25', stride='8'),
        '101': dict(cycle='30', owner='7', op='R', active='1', base='0xc8',
                    words='5', stride='8')}
    ends = {'100': {'cycle': '15'}, '101': {'cycle': '35'}}
    return '\n'.join(events), config, trace, jobs, ends


class InputDEarlyTests(unittest.TestCase):
    def test_lu_may_start_early_but_rhs_and_slot_wait(self):
        result = check_input_split(*fixture())
        self.assertEqual(result['early_lu_requests'], 1)
        self.assertEqual(result['requests'], 1)

    def test_disabled_path_rejects_unexpected_events(self):
        text, _, trace, jobs, ends = fixture()
        with self.assertRaises(AssertionError):
            check_input_split(text, {}, trace, jobs, ends)
        self.assertEqual(check_input_split('', {}, [], {}, {})['requests'], 0)

    def test_reject_d_read_before_last_d_store(self):
        text, config, trace, jobs, ends = fixture()
        jobs['100']['cycle'] = '7'
        with self.assertRaises(AssertionError):
            check_input_split(text, config, trace, jobs, ends)

    def test_reject_ready_or_lu_before_d_load_response(self):
        for stage in ('INPUT_D_READY', 'LU_ISSUE'):
            text, config, trace, jobs, ends = fixture()
            for r in trace:
                if r['stage'] == stage:
                    r['cycle'] = '14'
            with self.assertRaises(AssertionError):
                check_input_split(text, config, trace, jobs, ends)

    def test_reject_rhs_or_slot_release_before_complete_input(self):
        for stage in ('COEFF3_ISSUE', 'INPUT_SLOT_RELEASE'):
            text, config, trace, jobs, ends = fixture()
            for r in trace:
                if r['stage'] == stage:
                    r['cycle'] = '34'
            with self.assertRaises(AssertionError):
                check_input_split(text, config, trace, jobs, ends)

    def test_reject_rest_read_before_all_stores(self):
        text, config, trace, jobs, ends = fixture()
        jobs['101']['cycle'] = '29'
        with self.assertRaises(AssertionError):
            check_input_split(text, config, trace, jobs, ends)

    def test_reject_duplicate_or_missing_operand_words(self):
        for field, value in (('base', '0xc0'), ('words', '6'), ('stride', '16')):
            text, config, trace, jobs, ends = fixture()
            jobs['101'][field] = value
            with self.assertRaises(AssertionError):
                check_input_split(text, config, trace, jobs, ends)

    def test_reject_wrong_owner_or_reused_job(self):
        text, config, trace, jobs, ends = fixture()
        jobs['100']['owner'] = '8'
        with self.assertRaises(AssertionError):
            check_input_split(text, config, trace, jobs, ends)

    def test_reject_ready_token_naming_another_job(self):
        text, config, trace, jobs, ends = fixture()
        for r in trace:
            if r['stage'] == 'INPUT_D_READY':
                r['packetId'] = '101'
        with self.assertRaises(AssertionError):
            check_input_split(text, config, trace, jobs, ends)

    def test_reject_missing_or_duplicate_ready(self):
        text, config, trace, jobs, ends = fixture()
        row = next(r for r in trace if r['stage'] == 'INPUT_D_READY')
        for changed in ([r for r in trace if r is not row], [*trace, row]):
            with self.assertRaises(AssertionError):
                check_input_split(text, config, changed, jobs, ends)

    def test_reject_incomplete_d_or_input_dma(self):
        for packet in ('4', '5'):
            text, config, trace, jobs, ends = fixture()
            trace = [r for r in trace if not (r['stage'] == 'SPM_PACKET_RESPONSE'
                                             and r['packetId'] == packet)]
            with self.assertRaises(AssertionError):
                check_input_split(text, config, trace, jobs, ends)

    def test_cancelled_ingress_still_proves_d_read_fence(self):
        text, config, trace, jobs, ends = fixture()
        text = '\n'.join(line for line in text.splitlines() if
                         'event=rest_read' not in line and 'event=input_ready' not in line)
        trace = [r for r in trace if r['stage'] not in
                 ('INPUT_TOKEN', 'INPUT_SLOT_RELEASE', 'COEFF3_ISSUE')]
        trace.append(dict(requestId='1', stage='CANCEL_PENDING'))
        self.assertEqual(check_input_split(text, config, trace, jobs, ends,
                                          allow_abort=True)['aborted_d_reads'], 1)
        jobs['100']['cycle'] = '7'
        with self.assertRaises(AssertionError):
            check_input_split(text, config, trace, jobs, ends, allow_abort=True)


if __name__ == '__main__':
    unittest.main()
