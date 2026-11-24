import _support
import csv
from pathlib import Path
import tempfile
import unittest

ROOT = _support.PROJECT_ROOT
from check_single_line_opt import acceptance_trace
from check_single_line_storage import check_record_release
from check_single_line_output_dma import resolved_options, output_depth, fixed_control_reserve


class TraceReplayTests(unittest.TestCase):
    def test_explicit_output_fifo_inheritance_resolves_before_validation(self):
        import json
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)
            (path/'manifest.json').write_text(json.dumps({'command':[
                '--coeff-drain-queue-depth=18','--coeff-line-output-depth=0']}))
            (path/'resolved_experiment.json').write_text(json.dumps({
                'options':{'coeff_line_output_depth':18}}))
            config=resolved_options(path)
            self.assertEqual(output_depth(config),18)
            self.assertEqual(fixed_control_reserve(config),16384+18*64)

    def test_filter_keeps_order_payload_errors_and_unknown_stages(self):
        events = ['REQUEST_ALLOC', 'TRSM_WAIT_LU_STEP', 'SCHED_SELECT',
                  'SPM_PACKET_RESPONSE', 'REQUEST_FAILED', 'FUTURE_STAGE',
                  'REQUEST_COMPLETE', 'REQUEST_AUTO_RETIRE']
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'trace.csv'
            with path.open('w', newline='') as output:
                writer = csv.writer(output)
                writer.writerow(['cycle', 'stage', 'requestId', 'note'])
                writer.writerows((i, stage, 7, f'value,{i}') for i, stage in enumerate(events))
            result = acceptance_trace(path)
        self.assertEqual([r['stage'] for r in result],
                         [s for s in events if s not in ('TRSM_WAIT_LU_STEP', 'SCHED_SELECT')])
        self.assertEqual(result[1]['note'], 'value,3')

    def test_streamed_ownership_still_rejects_missing_or_early_record(self):
        fields = ['cycle', 'requestId', 'cell', 'stage']
        rows = [[11, 1, 0, 'REQUEST_ALLOC'], [20, 1, 0, 'REQUEST_COMPLETE'],
                [25, 1, 0, 'REQUEST_AUTO_RETIRE']]
        text = '\n'.join([
            'CFD_LINE_SCHEDULE token=2 visible_done=30',
            'CFD_INTERFACE_DMA token=2 kind=descriptor cycle=10 event=issue cell=0',
            'CFD_INTERFACE_DMA token=2 kind=record cycle=24 event=response cell=0'])
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            def write(selected):
                with (path/'coeff_preprocess_trace.csv').open('w', newline='') as output:
                    writer = csv.writer(output); writer.writerow(fields)
                    writer.writerows(selected)
            write(rows)
            check_record_release(path, text)
            write(rows[:-1])
            with self.assertRaises(ValueError): check_record_release(path, text)
            write([rows[0], rows[1], [23, 1, 0, 'REQUEST_AUTO_RETIRE']])
            with self.assertRaises(ValueError): check_record_release(path, text)


if __name__ == '__main__':
    unittest.main()
