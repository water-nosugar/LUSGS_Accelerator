"""Ready-R priority must retain physical stores, read ownership and full drain."""
import _support
import copy
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from memory.test_input_batch_ready import fixture
from reporting.test_single_line_config_audit import fixture as audit_fixture
from check_input_batch import check_input_batches, check_batch_rhs
from single_line_config_audit import Effective, capacity_audit
from check_single_line_storage import lifecycle_reserve


def priority_fixture():
    text, cfg, trace, jobs, ends = fixture()
    cfg['--coeff-input-r-before-u'] = '1'
    text = text.replace('cycle=45 request=1', 'cycle=21 request=1')
    text = text.replace('cycle=48 request=1', 'cycle=24 request=1')
    text = text.replace('cycle=24 request=1 generation=10 cell=1 owner=7 event=input_ready slot=0 job=103',
                        'cycle=43 request=1 generation=10 cell=1 owner=7 event=input_ready slot=0 job=102')
    jobs['103']['cycle'] = '21'
    ends['103']['cycle'] = '24'
    for r in trace:
        if r['stage'] == 'SPM_PACKET_RESPONSE' and r['packetId'] == '4':
            r['cycle'] = '18'
        elif r['stage'] == 'INPUT_R_READY':
            r['cycle'] = '24'
        elif r['stage'] in ('INPUT_TOKEN', 'INPUT_SLOT_RELEASE'):
            r['cycle'] = '43'
        elif r['rhsBatch'] == '0' and r['stage'] in ('RHS_INPUT_INIT', 'TRSM_MUL_ISSUE'):
            r['cycle'] = '24' if r['stage'] == 'RHS_INPUT_INIT' else '25'
        elif r['rhsBatch'] == '2' and r['stage'] in ('RHS_INPUT_INIT', 'TRSM_MUL_ISSUE'):
            r['cycle'] = '43' if r['stage'] == 'RHS_INPUT_INIT' else '44'
    trace.sort(key=lambda r:int(r['cycle']))
    return text, cfg, trace, jobs, ends


class ReadyRCheckerTests(unittest.TestCase):
    def test_ready_r_can_load_before_u_with_original_offsets_and_one_handle(self):
        text, cfg, trace, jobs, ends = priority_fixture()
        result = check_input_batches(text, cfg, trace, jobs, ends)
        self.assertEqual(result['requests'], 1)
        self.assertEqual(result['max_rest_reads_per_request'], 1)
        self.assertEqual(check_batch_rhs(text, cfg, trace)['peak_contexts'], 1)

    def test_u_fallback_preserves_original_sequence_when_r_stores_are_late(self):
        text, cfg, trace, jobs, ends = fixture()
        cfg['--coeff-input-r-before-u'] = '1'
        self.assertEqual(check_input_batches(text, cfg, trace, jobs, ends)['requests'], 1)

    def test_default_policy_does_not_accept_reordered_reads(self):
        text, cfg, trace, jobs, ends = priority_fixture()
        cfg['--coeff-input-r-before-u'] = '0'
        with self.assertRaises(AssertionError):
            check_input_batches(text, cfg, trace, jobs, ends)

    def test_reject_u_when_r_was_already_stored_and_unpublished(self):
        text, cfg, trace, jobs, ends = fixture()
        cfg['--coeff-input-r-before-u'] = '1'
        for r in trace:
            if r['stage'] == 'SPM_PACKET_RESPONSE' and r['packetId'] == '4':
                r['cycle'] = '18'
        trace.sort(key=lambda r:int(r['cycle']))
        with self.assertRaisesRegex(AssertionError, 'R priority'):
            check_input_batches(text, cfg, trace, jobs, ends)

    def test_reordered_reads_still_reject_unpaid_and_overlapping_accesses(self):
        for corruption in ('stores', 'overlap', 'physical_offset', 'release'):
            text, cfg, trace, jobs, ends = priority_fixture()
            if corruption == 'stores':
                for r in trace:
                    if r['stage'] == 'SPM_PACKET_RESPONSE' and r['packetId'] == '4':
                        r['cycle'] = '22'
            elif corruption == 'overlap':
                jobs['102']['cycle'] = '22'
                text = text.replace('cycle=36 request=1', 'cycle=22 request=1')
            elif corruption == 'physical_offset':
                text = text.replace('offset=600', 'offset=400')
            else:
                for r in trace:
                    if r['stage'] == 'INPUT_SLOT_RELEASE':r['cycle'] = '42'
            with self.subTest(corruption=corruption), self.assertRaises(AssertionError):
                check_input_batches(text, cfg, trace, jobs, ends)

    def test_abort_can_drain_l_then_r_without_loading_u_or_releasing_the_slot(self):
        for cancelled_at in (22, 30):
            text, cfg, trace, jobs, ends = priority_fixture()
            text = '\n'.join(line for line in text.splitlines()
                             if 'matrix=U' not in line and 'event=input_ready' not in line
                             and not ('matrix=R' in line and 'event=ready' in line and cancelled_at < 24))
            trace = [r for r in trace if int(r['cycle']) <= cancelled_at]
            trace.append(dict(trace[0], stage='CANCEL_PENDING', cycle=str(cancelled_at)))
            with self.subTest(cancelled_at=cancelled_at):
                result = check_input_batches(text, cfg, trace, jobs, ends, allow_abort=True)
                self.assertEqual(result['requests'], 0)
                self.assertEqual(result['aborted_d_reads'], 1)


class ReadyRControlTests(unittest.TestCase):
    def test_native_selector_only_uses_published_flags_and_actual_stored_bytes(self):
        compiler = shutil.which('g++') or shutil.which('c++')
        self.assertIsNotNone(compiler)
        code = r'''
#include "arch/arm/cfd_input_batch_order.hh"
#include <cassert>
using gem5::ArmISA::CfdInputBatchOrder;
int main() {
    using P=CfdInputBatchOrder;
    static_assert(P::ControlBytes==128);
    assert(P::select(false,false,false,199,200,40)==-1);
    assert(P::select(false,false,false,200,200,40)==1);
    assert(P::select(true,false,false,200,200,40)==3);
    assert(P::select(true,false,false,200,200,39)==2);
    assert(P::select(true,false,false,200,199,39)==-1);
    assert(P::select(true,false,true,200,200,40)==2);
    assert(P::select(true,true,false,200,200,40)==3);
    assert(P::select(true,true,true,200,200,40)==-1);
    assert(P::allowed(13,true,true,false));
    assert(!P::allowed(12,true,true,false));
    assert(!P::allowed(13,false,true,false));
    assert(!P::allowed(13,true,false,false));
    assert(!P::allowed(13,true,true,true));
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / 'ready-r'
            subprocess.run([compiler, '-std=c++17', '-I', str(_support.REPO_ROOT/'src'),
                            '-x', 'c++', '-o', str(executable), '-'],
                           input=code, text=True, capture_output=True, check=True)
            subprocess.run([str(executable)], check=True)

    def test_public_config_guard_rejects_unsupported_modes(self):
        from single_line_contract import input_r_priority_allowed
        self.assertTrue(input_r_priority_allowed(13, True, True, False))
        for args in [(12,True,True,False), (13,False,True,False),
                     (13,True,False,False), (13,True,True,True)]:
            with self.subTest(args=args):self.assertFalse(input_r_priority_allowed(*args))

    def test_control_cost_is_paid_in_independent_audit_and_storage_checker(self):
        resolved, _ = audit_fixture()
        resolved['options'].update(coeff_input_batch_ready=True, coeff_input_l_first=True,
                                   coeff_cold_rhs_prefetch=False)
        before = capacity_audit(Effective(copy.deepcopy(resolved)))
        resolved['options']['coeff_input_r_before_u'] = True
        after = capacity_audit(Effective(resolved))
        self.assertEqual(after['reservation_bytes']-before['reservation_bytes'], 128)
        config = {'--'+key.replace('_','-'):str(int(value)) if isinstance(value,bool) else str(value)
                  for key,value in resolved['options'].items()}
        with_priority = lifecycle_reserve(config)
        config['--coeff-input-r-before-u']='0'
        self.assertEqual(with_priority-lifecycle_reserve(config), 128)

    def test_audit_rejects_incompatible_cold_r_dma_prefetch(self):
        resolved, _ = audit_fixture()
        resolved['options'].update(coeff_input_r_before_u=True, coeff_input_batch_ready=True,
                                   coeff_input_l_first=True, coeff_cold_rhs_prefetch=True)
        with self.assertRaises(AssertionError):capacity_audit(Effective(resolved))
