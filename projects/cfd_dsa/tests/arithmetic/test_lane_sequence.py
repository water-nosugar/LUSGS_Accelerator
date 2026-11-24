"""Cached inverse phases retain strict physical epoch/feedback ordering."""
import _support
from pathlib import Path
import unittest
from check_lane_mvm import advance_lane_epoch

class LaneSequenceTests(unittest.TestCase):
    def test_inverse_base_then_correction_can_share_one_cell(self):
        first=advance_lane_epoch(None,4,0,10,20,0,True)
        second=advance_lane_epoch(first,4,1,20,30,0,True)
        self.assertEqual(second,(4,30,1,2))
        self.assertEqual(advance_lane_epoch(second,5,2,30,40,0,True),(5,40,2,1))

    def test_no_third_inverse_phase(self):
        with self.assertRaisesRegex(AssertionError,'repeated cell'):
            advance_lane_epoch((4,30,1,2),4,2,30,40,0,True)

    def test_direct_and_background_contexts_cannot_repeat_cells(self):
        for context,inverse in ((0,False),(1,True),(2,True)):
            with self.subTest(context=context),self.assertRaises(AssertionError):
                advance_lane_epoch((4,20,0,1),4,1,20,30,context,inverse)

    def test_backward_context_moves_in_reverse(self):
        self.assertEqual(advance_lane_epoch((4,20,0,1),3,1,20,30,1,False),(3,30,1,1))
        with self.assertRaises(AssertionError):
            advance_lane_epoch((4,20,0,1),5,1,20,30,1,False)

    def test_epoch_and_completed_predecessor_are_required(self):
        for epoch,begin in ((0,20),(2,20),(1,19)):
            with self.subTest(epoch=epoch,begin=begin),self.assertRaises(AssertionError):
                advance_lane_epoch((4,20,0,1),5,epoch,begin,30,0,False)

    def test_forward_cannot_skip_a_cell(self):
        with self.assertRaises(AssertionError):
            advance_lane_epoch((4,20,0,1),6,1,20,30,0,False)

    def test_invalid_context_epoch_and_time_are_rejected(self):
        for context,epoch,begin,end in ((3,0,0,1),(0,-1,0,1),(0,0,2,1)):
            with self.subTest(context=context),self.assertRaises(AssertionError):
                advance_lane_epoch(None,0,epoch,begin,end,context,False)

if __name__=='__main__':unittest.main()
