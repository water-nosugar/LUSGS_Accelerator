import _support
import unittest

from analyze_round1_diagnostics import Round1Replay, Distribution


class Round1DiagnosticsTests(unittest.TestCase):
    def replay(self, lines):
        replay = Round1Replay(ticks_per_cycle=500)
        for line in lines:
            replay.feed(line.encode())
        return replay

    def test_histogram_percentile_counts_repeated_samples(self):
        samples = Distribution()
        for value in [0, 0, 0, 0, 5]:
            samples.add(value)
        result = samples.summary()
        self.assertEqual((result['count'], result['mean'], result['p95']), (5, 1, 5))

    def test_pair_snapshot_does_not_imply_critical_pair_can_issue(self):
        replay = self.replay([
            'CFD_RHS_READY cycle=10 token=2 generation=9 cell=0 phase=forward k=0 width=4 '
            'mask=35 base_mask=1 frontier=1 mul_free=10 read_free=16 product_free=2 '
            'return_free=30 local_tags_free=32 preprocess_broadcast=1 local_products=1\n'])
        result = replay.result()['rhs_pairing']
        self.assertEqual(result['ready_pair_cycle_count'], 1)
        self.assertEqual(result['background_pair_cycle_count'], 1)
        self.assertEqual(result['forward_resource_feasible_cycle_count'], 1)
        self.assertEqual(result['paired_forward_read_saving_per_snapshot']['mean'], 4)

    def test_eight_multiply_pair_requires_eight_free_lanes(self):
        replay = self.replay([
            'CFD_RHS_READY cycle=10 token=2 generation=9 cell=0 phase=forward k=0 width=4 '
            'mask=34 base_mask=0 frontier=0 mul_free=5 read_free=16 product_free=2 '
            'return_free=30 local_tags_free=32 preprocess_broadcast=1 local_products=1\n'])
        result = replay.result()['rhs_pairing']
        self.assertEqual(result['ready_pair_cycle_count'], 1)
        self.assertEqual(result['forward_resource_feasible_cycle_count'], 0)

    def test_ready_base_rhs_is_not_held_to_form_a_background_pair(self):
        replay = self.replay([
            'CFD_RHS_READY cycle=10 token=2 generation=9 cell=0 phase=forward k=0 width=4 '
            'mask=3 base_mask=1 frontier=1 mul_free=10 read_free=16 product_free=2 '
            'return_free=30 local_tags_free=32 preprocess_broadcast=1 local_products=1\n'])
        result = replay.result()['rhs_pairing']
        self.assertEqual(result['ready_pair_cycle_count'], 1)
        self.assertEqual(result['background_pair_cycle_count'], 0)

    def test_two_requests_with_different_lu_never_form_a_pair(self):
        prefix = 'CFD_RHS_READY cycle=10 generation=9 cell=0 phase=forward k=0 width=4 '
        suffix = ' mask=2 base_mask=0 frontier=0 mul_free=10 read_free=16 product_free=2 return_free=30 local_tags_free=32 preprocess_broadcast=1 local_products=1\n'
        replay = self.replay([prefix+'token=2'+suffix, prefix+'token=3'+suffix])
        self.assertEqual(replay.result()['rhs_pairing']['ready_pair_cycle_count'], 0)

    def test_result_delay_uses_actual_complete_and_separate_local_route(self):
        replay = self.replay([
            'CFD_RF_ISSUE cycle=10 id=1 pool=mul lanes=4 caller=4 read_words=5 ready=13 bank_dst=31 bank_route=1\n',
            'CFD_RF_OWNER cycle=10 id=1 owner=2 frontier=2\n',
            'CFD_LOCAL_WRITE cycle=14 id=1 bank=31 words=4 owner=2 epoch=1\n',
            'CFD_RF_COMPLETE cycle=15 id=1 pool=mul lanes=4 nominal=13\n'])
        result = replay.result()['rf']['by_caller']['4']
        self.assertEqual(result['actual_return_delay']['mean'], 2)
        self.assertEqual(result['delay_excluding_declared_local_route']['mean'], 1)
        self.assertEqual(result['frontier_at_issue_operations'], 1)
        self.assertEqual(result['frontier_at_issue_return_delay']['mean'], 2)

    def test_tile_memory_response_is_distinct_from_spm_publication(self):
        replay = self.replay([
            'CFD_DMA_PACKET tick=5000 event=queue packet=7 op=R bytes=40 live=1\n',
            'CFD_DMA_PACKET tick=5500 event=send packet=7\n',
            'CFD_ONCHIP_START cycle=11 id=4 owner=1 base=0x0 words=5 stride=8 op=W active=0 priority=1\n',
            'CFD_TILE_DMA cycle=11 token=1 event=issue first=0 region=0 bytes=40 packet=7 offset=0 job=4\n',
            'CFD_DMA_PACKET tick=10500 event=response packet=7\n',
            'CFD_ONCHIP_ACTIVATE cycle=21 id=4\n',
            'CFD_ONCHIP_ACCESS cycle=24 id=4 owner=1 op=W addr=0x0 bank=0 priority=1\n',
            'CFD_ONCHIP_ACCESS cycle=24 id=4 owner=1 op=W addr=0x8 bank=1 priority=1\n',
            'CFD_ONCHIP_ACCESS cycle=24 id=4 owner=1 op=W addr=0x10 bank=2 priority=1\n',
            'CFD_ONCHIP_ACCESS cycle=24 id=4 owner=1 op=W addr=0x18 bank=3 priority=1\n',
            'CFD_ONCHIP_ACCESS cycle=25 id=4 owner=1 op=W addr=0x20 bank=0 priority=1\n',
            'CFD_ONCHIP_COMPLETE cycle=26 id=4 owner=1\n',
            'CFD_TILE_DMA cycle=26 token=1 event=response first=0 region=0 bytes=40 packet=7 offset=0 job=4\n'])
        result = replay.result()['tile']
        self.assertEqual(result['queue_to_send']['mean'], 1)
        self.assertEqual(result['send_to_dma_response']['mean'], 10)
        self.assertEqual(result['dma_response_to_spm_ready']['mean'], 5)
        self.assertEqual(result['spm_activation_to_first_access']['mean'], 3)

    def test_tile_response_before_spm_completion_is_rejected(self):
        lines = [
            'CFD_DMA_PACKET tick=5000 event=queue packet=7 op=R bytes=40 live=1\n',
            'CFD_DMA_PACKET tick=5500 event=send packet=7\n',
            'CFD_ONCHIP_START cycle=11 id=4 owner=1 base=0x0 words=5 stride=8 op=W active=0 priority=1\n',
            'CFD_TILE_DMA cycle=11 token=1 event=issue first=0 region=0 bytes=40 packet=7 offset=0 job=4\n',
            'CFD_DMA_PACKET tick=10500 event=response packet=7\n',
            'CFD_TILE_DMA cycle=22 token=1 event=response first=0 region=0 bytes=40 packet=7 offset=0 job=4\n']
        with self.assertRaisesRegex(ValueError, 'SPM'):
            self.replay(lines)

    def test_duplicate_rf_identity_is_rejected(self):
        row = 'CFD_RF_ISSUE cycle=10 id=1 pool=mul lanes=5 caller=8 read_words=6 ready=13 bank_dst=-1 bank_route=0\n'
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            self.replay([row, row])

    def test_nominal_completion_cannot_substitute_for_actual_return(self):
        with self.assertRaisesRegex(ValueError, 'before nominal'):
            self.replay([
                'CFD_RF_ISSUE cycle=10 id=1 pool=mul lanes=5 caller=8 read_words=6 ready=13 bank_dst=-1 bank_route=0\n',
                'CFD_RF_COMPLETE cycle=12 id=1 pool=mul lanes=5 nominal=13\n'])

    def test_mvm_binding_must_match_the_physical_issue_cycle(self):
        with self.assertRaisesRegex(ValueError, 'binding'):
            self.replay([
                'CFD_RF_ISSUE cycle=10 id=1 pool=mul lanes=5 caller=8 read_words=6 ready=13 bank_dst=-1 bank_route=0\n',
                'CFD_MVM_BIND cycle=11 id=1 token=1 context=1 epoch=0 cell=3 multiply=1 feedback=1 node=0\n'])

    def test_old_logs_report_missing_binding_coverage_as_zero(self):
        self.assertEqual(self.replay([]).result()['event_counts']['backward_cells_physically_bound'], 0)

    def test_unreturned_results_fail_drain_verification(self):
        replay = self.replay([
            'CFD_RF_ISSUE cycle=10 id=1 pool=mul lanes=5 caller=8 read_words=6 ready=13 bank_dst=-1 bank_route=0\n'])
        with self.assertRaisesRegex(ValueError, 'drain'):
            replay.result(require_drained=True)


if __name__ == '__main__':
    unittest.main()
