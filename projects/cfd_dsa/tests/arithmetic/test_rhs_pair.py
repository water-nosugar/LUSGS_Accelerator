"""Ingress reuse preserves two independent, credit-reserved MUL operands."""
import _support
import csv
from pathlib import Path
import subprocess
import tempfile
import unittest
from check_register_ports import replay_register_ports, _rows
from check_rhs_pair import replay_rhs_pairs,check_pair_schedule


def paired_fixture():
    cfg={'--coeff-rhs-pair':'1','--coeff-local-products':'1',
         '--coeff-rf-read-words':'16','--coeff-rf-write-words':'8',
         '--coeff-rf-broadcast':'1','--coeff-rf-split-writeback':'1',
         '--coeff-cluster-mul-count':'10','--coeff-cluster-add-count':'10',
         '--coeff-cluster-mul-lat':'3','--coeff-cluster-add-lat':'4',
         '--coeff-cluster-div-lat':'12','--coeff-cluster-div-ii':'4'}
    metadata='id=1 slot=0 token=7 request=9 generation=11 cell=0 k=0 rhs0=10 rhs1=11 width=4 selected=0'
    operands='lu0=0x0p+0 lu1=0x1p+0 lu2=0x1p+1 lu3=0x1.8p+1 lu4=0x1p+2 factor0=0x1p+1 factor1=0x1.8p+1'
    lines=[f'CFD_RHS_PAIR cycle=0 event=select {metadata} loaded=0 read_words=0',
           f'CFD_RHS_PAIR cycle=1 event=load {metadata} loaded=1 read_words=6 {operands}',
           f'CFD_RHS_PAIR cycle=2 event=issue {metadata} loaded=1 read_words=0 {operands}']
    for side in range(2):
        lines.append(f'CFD_RF_ISSUE cycle=2 id={side+1} pool=mul lanes=4 caller=4 read_words=0 ready=5 capacity=30 broadcast=0 bank_access=1 bank_a=-1 bank_b=-1 bank_dst={31+side} bank_words_a=0 bank_words_b=0 bank_owner=7 bank_epoch=11 bank_route=1 pair_id=1 pair_side={side}')
    for side in range(2):
        lines.append(f'CFD_LOCAL_WRITE cycle=5 id={side+1} bank={31+side} words=4 owner=7 epoch=11')
    for side in range(2):
        lines.append(f'CFD_RF_COMPLETE cycle=6 id={side+1} pool=mul lanes=4 nominal=5')
    lines.insert(5,f'CFD_RHS_PAIR cycle=2 event=release {metadata} loaded=1 read_words=0 {operands}')
    cluster=[dict(cycle='2',pool='mul',lanes='4',caller='4',complete_cycle='5')]*2
    trace=[dict(stage='REQUEST_ALLOC',requestId='9',generation='11',cell='0')]+[
        dict(stage='SCHED_SELECT',requestId='9',cycle='2',rhsBatch='2',rhsColumn=str(i),k='0') for i in range(2)]
    return '\n'.join(lines),cfg,cluster,trace


class RhsPairTests(unittest.TestCase):
    def test_full_acceptance_retains_required_pair_selection_evidence(self):
        from check_single_line_opt import acceptance_trace
        text,cfg,_,trace=paired_fixture()
        pairing=replay_rhs_pairs(text,cfg,list(_rows(text)))
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'trace.csv'
            keys=sorted(set().union(*(set(r) for r in trace)))
            with path.open('w') as f:
                writer=csv.DictWriter(f,fieldnames=keys);writer.writeheader();writer.writerows(trace)
            # SCHED_SELECT was historically diagnostic-only; pairing now
            # depends on it, so the full acceptance adapter must preserve it.
            with self.assertRaises(AssertionError):check_pair_schedule(pairing,acceptance_trace(path))
            check_pair_schedule(pairing,acceptance_trace(path,rhs_pair=True))

    def test_native_selection_route_ports_ownership_and_cancellation(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'test'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra',
                '-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'rhs_pair_model.cc'),'-o',str(binary)],
                check=True,capture_output=True)
            self.assertIn('RHS_PAIR_MODEL_PASS',subprocess.check_output([str(binary)],text=True))

    def test_two_destinations_share_one_paid_registered_operand_capture(self):
        text,cfg,cluster,trace=paired_fixture()
        r=replay_register_ports(text,cfg,cluster)
        self.assertEqual(r.summary['read_words'],6)
        self.assertEqual(r.summary['bank_write_words'],8)
        self.assertEqual(r.summary['operations'],2)
        self.assertEqual(r.summary['rhs_pair']['saved_global_read_words'],10)
        paired=replay_rhs_pairs(text,cfg,list(_rows(text)))
        check_pair_schedule(paired,trace)
        with self.assertRaises(AssertionError):check_pair_schedule(paired,trace[:-1])

    def test_reject_free_reads_routes_stale_ownership_and_early_recycle(self):
        text,cfg,cluster,_=paired_fixture()
        for before,after in [('cycle=1 event=load','cycle=0 event=load'),
            ('cycle=2 event=issue','cycle=1 event=issue'),
            ('loaded=1 read_words=6','loaded=1 read_words=5'),
            ('pair_side=1','pair_side=0'),('bank_epoch=11','bank_epoch=12'),
            ('bank_dst=32','bank_dst=31'),('cycle=2 event=release','cycle=1 event=release'),
            ('rhs0=10','rhs0=0'),('lu4=0x1p+2 factor0','lu4=nan factor0')]:
            with self.subTest(before=before),self.assertRaises((AssertionError,KeyError)):
                replay_register_ports(text.replace(before,after),cfg,cluster)
        with self.assertRaises(AssertionError):
            replay_register_ports(text,dict(cfg,**{'--coeff-rhs-pair':'0'}),cluster)

    def test_pair_load_competes_for_the_same_global_read_ports(self):
        text,cfg,cluster,_=paired_fixture()
        extra='CFD_RF_ISSUE cycle=1 id=3 pool=mul lanes=5 caller=8 read_words=10 ready=4 capacity=30'
        # A second two-word operation would exceed the remaining 10 reads.
        extra+='\nCFD_RF_ISSUE cycle=1 id=4 pool=add lanes=1 caller=5 read_words=2 ready=5 capacity=40'
        text=text.replace('CFD_RF_ISSUE cycle=2 id=1',extra+'\nCFD_RF_ISSUE cycle=2 id=1')
        with self.assertRaisesRegex(AssertionError,'read bandwidth'):
            replay_register_ports(text,cfg,cluster)

    def test_ingress_reuse_keeps_both_previous_results_live_and_immutable(self):
        text,cfg,cluster,_=paired_fixture()
        original=text.splitlines()
        second=[]
        for row in original:
            if row.startswith('CFD_RHS_PAIR '):
                row=row.replace(' id=1 slot=0',' id=2 slot=0').replace('selected=0','selected=2')
                row=row.replace('loaded=1','loaded=3')
                row=row.replace('lu4=0x1p+2','lu4=0x1p+3')
            else:
                row=row.replace(' id=2 ',' id=4 ').replace(' id=1 ',' id=3 ')
                row=row.replace('bank_dst=31','bank_dst=33').replace('bank_dst=32','bank_dst=34')
                row=row.replace('bank=31','bank=33').replace('bank=32','bank=34')
                row=row.replace('pair_id=1','pair_id=2').replace('ready=5','ready=7').replace('nominal=5','nominal=7')
            import re
            row=re.sub(r'cycle=(\d+)',lambda m:'cycle='+str(int(m[1])+2),row)
            second.append(row)
        # Stable same-cycle ordering keeps the old capture/release before the
        # new selection, while both old RF results are still in flight.
        lines=sorted(original+second,key=lambda r:int(r.split('cycle=')[1].split()[0]))
        cluster=cluster+[dict(r,cycle='4',complete_cycle='7') for r in cluster]
        result=replay_register_ports('\n'.join(lines),cfg,cluster)
        self.assertEqual(result.summary['operations'],4)
        self.assertEqual(result.summary['rhs_pair']['issued_pairs'],2)
        self.assertEqual(result.summary['live_peak']['mul'],16)
        self.assertEqual(result.summary['bank_write_words'],16)
        missing='\n'.join(r for r in lines if not r.startswith('CFD_RF_COMPLETE cycle=6 id=1 '))
        with self.assertRaises(AssertionError):replay_register_ports(missing,cfg,cluster)

    def test_loaded_cancellation_preserves_read_cost_without_fabricating_fp_work(self):
        text,cfg,_,_=paired_fixture()
        lines=text.splitlines()[:2]
        lines.append(lines[1].replace('cycle=1 event=load','cycle=2 event=cancel'))
        r=replay_rhs_pairs('\n'.join(lines),cfg,[])
        self.assertEqual(r['summary']['cancelled_pairs'],1)
        self.assertEqual(r['summary']['issued_pairs'],0)
        self.assertEqual(r['summary']['operand_read_words'],6)

    def test_capacity_contracts_independently_pay_768_bytes(self):
        from reporting.test_single_line_config_audit import fixture
        from single_line_config_audit import Effective,capacity_audit
        from check_single_line_storage import lifecycle_reserve
        resolved,_=fixture(1024)
        resolved['options']['coeff_line_cache_allowed']=False
        base=capacity_audit(Effective(resolved))
        resolved['options']['coeff_rhs_pair']=True
        paired=capacity_audit(Effective(resolved))
        self.assertEqual(paired['reservation_bytes']-base['reservation_bytes'],768)
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-rhs-pair':'1'}))-lifecycle_reserve(cfg),768)
