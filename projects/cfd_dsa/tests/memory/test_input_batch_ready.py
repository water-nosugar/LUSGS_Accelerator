"""Independent negatives for partial L/U input and retained execution credit."""
import _support
import copy
import unittest
from check_input_batch import check_input_batches,check_batch_rhs


def fixture():
    config={'--coeff-input-d-early':'1','--coeff-input-batch-ready':'1',
            '--coeff-input-l-first':'1','--coeff3-count':'1'}
    trace=[];messages=[];jobs={};ends={}
    def event(stage,t,**extra):
        r=dict(requestId='1',generation='10',cell='1',stage=stage,cycle=str(t),
               slot='0',packetId='0',rhsBatch='-1',rhsColumn='-1')
        r.update({k:str(v) for k,v in extra.items()});trace.append(r)
    event('INPUT_START',0)
    # D,L,U,R are separately stored and separately read. Earlier L may
    # execute before U/R, while the input slot and RHS context remain live.
    for i,(matrix,bytes,stored,started,ready) in enumerate([
            ('D',200,5,6,10),('L',200,12,13,20),
            ('U',200,35,36,43),('R',40,44,45,48)]):
        identity=str(i+1);job=str(100+i);offset=i*200
        event('SPM_PACKET_ISSUE',1,packetId=identity,requester='CoeffInput',size=bytes)
        event('SPM_PACKET_RESPONSE',stored,packetId=identity,requester='CoeffInput',size=bytes)
        jobs[job]=dict(cycle=str(started),owner='7',op='R',active='1',
                       base=hex(offset),words=str(bytes//8),stride='8')
        ends[job]=dict(cycle=str(ready))
        if not i:
            for tag,t in [('d_read',started),('d_ready',ready)]:
                messages.append(f'CFD_INPUT_SPLIT cycle={t} request=1 generation=10 cell=1 owner=7 event={tag} slot=0 job={job} bytes=640')
        else:
            for tag,t in [('read',started),('ready',ready)]:
                messages.append(f'CFD_INPUT_BATCH cycle={t} request=1 generation=10 cell=1 owner=7 event={tag} region={i} matrix={matrix} slot=0 job={job} offset={offset} bytes={bytes}')
        event('INPUT_'+matrix+'_READY',ready,packetId=job)
    messages.append('CFD_INPUT_SPLIT cycle=48 request=1 generation=10 cell=1 owner=7 event=input_ready slot=0 job=103 bytes=640')
    event('LU_ISSUE',11);event('COEFF3_ISSUE',20)
    event('INPUT_TOKEN',48);event('INPUT_SLOT_RELEASE',48)
    for batch,t in [(1,20),(2,48),(0,48)]:
        for column in range(1 if batch==0 else 5):
            event('RHS_INPUT_INIT',t,rhsBatch=batch,rhsColumn=column)
            event('TRSM_MUL_ISSUE',t+1,rhsBatch=batch,rhsColumn=column)
            event('RHS_COMPLETE',60,rhsBatch=batch,rhsColumn=column)
    event('COEFF3_COMPLETE',60)
    trace.sort(key=lambda r:int(r['cycle']))
    return '\n'.join(messages),config,trace,jobs,ends


def parked_fixture():
    text,cfg,trace,jobs,ends=fixture()
    cfg['--coeff-cold-rhs-prefetch']='1'
    for r in trace:
        if r['stage']=='INPUT_R_READY':r['cycle']='12'
        elif r['stage']=='COEFF3_ISSUE':r['cycle']='12'
        elif r['rhsBatch']=='0':
            r['cycle']=str({'RHS_INPUT_INIT':12,'TRSM_MUL_ISSUE':13,'RHS_COMPLETE':18}[r['stage']])
    for stage,t in [('TRSM_INPUT_PARK',18),('TRSM_INPUT_RESUME',20)]:
        trace.append(dict(trace[0],stage=stage,cycle=str(t),rhsBatch='-1',rhsColumn='-1'))
    trace.sort(key=lambda r:int(r['cycle']))
    return text,cfg,trace,jobs,ends


class InputBatchTests(unittest.TestCase):
    def test_paid_partial_publication_preserves_input_slot_and_context(self):
        text,cfg,trace,jobs,ends=fixture()
        result=check_input_batches(text,cfg,trace,jobs,ends)
        self.assertEqual(result['early_batches'],2)
        self.assertEqual(result['extra_operand_words'],0)
        rhs=check_batch_rhs(text,cfg,trace)
        self.assertEqual(rhs['early_batch_initializations'],5)
        self.assertEqual(rhs['peak_contexts'],1)

    def test_packed_boundary_beat_must_retire_before_either_batch_read(self):
        text, cfg, trace, jobs, ends = fixture()
        packet_stages = ('SPM_PACKET_ISSUE', 'SPM_PACKET_RESPONSE')
        example = next(row for row in trace if row['stage'] == packet_stages[0])
        trace = [row for row in trace if row['stage'] not in packet_stages]
        for index, response in enumerate([5] * 4 + [12] * 3 + [35] * 3):
            for stage, cycle in zip(packet_stages, (1, response)):
                trace.append(dict(example, stage=stage, cycle=str(cycle),
                                  size='64', packetId=str(index + 1)))
        trace.sort(key=lambda row: int(row['cycle']))
        with self.assertRaises(AssertionError):
            check_input_batches(text, cfg, trace, jobs, ends)
        cfg['--coeff-sme-packed-input'] = '1'
        self.assertEqual(check_input_batches(text, cfg, trace, jobs, ends)['requests'], 1)
        crossing = next(row for row in trace if row['stage'] == packet_stages[1] and
                        row['packetId'] == '4')
        crossing['cycle'] = '7'
        with self.assertRaisesRegex(AssertionError, 'read preceded its own stores'):
            check_input_batches(text, cfg, trace, jobs, ends)

    def test_reject_read_before_own_stores_and_repeated_word_ranges(self):
        for before,after in [('cycle=13','cycle=11'),('offset=200','offset=192'),
                             ('bytes=200','bytes=208'),('region=1','region=2')]:
            text,cfg,trace,jobs,ends=fixture()
            with self.subTest(before=before),self.assertRaises(AssertionError):
                check_input_batches(text.replace(before,after),cfg,trace,jobs,ends)

    def test_reject_early_ready_and_input_slot_release(self):
        for stage,t in [('INPUT_L_READY',19),('INPUT_SLOT_RELEASE',47),('INPUT_D_READY',9)]:
            text,cfg,trace,jobs,ends=fixture()
            for r in trace:
                if r['stage']==stage:r['cycle']=str(t)
            with self.subTest(stage=stage),self.assertRaises(AssertionError):
                check_input_batches(text,cfg,trace,jobs,ends)

    def test_reject_rhs_initialization_before_specific_batch_read(self):
        text,cfg,trace,_,_=fixture()
        for r in trace:
            if r['stage']=='RHS_INPUT_INIT' and r['rhsBatch']=='2':r['cycle']='42'
        with self.assertRaises(AssertionError):check_batch_rhs(text,cfg,trace)

    def test_ubar_waits_for_full_input_even_after_its_own_read(self):
        text,cfg,trace,_,_=fixture()
        for r in trace:
            if r['stage']=='RHS_INPUT_INIT' and r['rhsBatch']=='2':r['cycle']='44'
        with self.assertRaisesRegex(AssertionError,'Ubar occupied'):
            check_batch_rhs(text,cfg,trace)

    def test_disabled_batch_policy_rejects_unexpected_publication(self):
        from check_input_split import check_input_split
        text,cfg,trace,jobs,ends=fixture()
        with self.assertRaisesRegex(AssertionError,'disabled batch'):
            check_input_split(text,dict(cfg,**{'--coeff-input-batch-ready':'0'}),trace,jobs,ends)

    def test_reject_partial_batch_context_release(self):
        text,cfg,trace,_,_=fixture()
        trace.append(dict(trace[0],stage='TRSM_INPUT_PARK',cycle='30'))
        with self.assertRaises(AssertionError):check_batch_rhs(text,cfg,trace)

    def test_completed_b_may_park_without_partial_rhs_and_resume_for_l(self):
        text,cfg,trace,_,_=parked_fixture()
        result=check_batch_rhs(text,cfg,trace)
        self.assertEqual(result['parked_requests'],1)
        self.assertEqual(result['peak_contexts'],1)

    def test_resume_waits_for_paid_l_and_next_selection_cycle(self):
        for t in (18,19):
            text,cfg,trace,_,_=parked_fixture()
            for r in trace:
                if r['stage']=='TRSM_INPUT_RESUME':r['cycle']=str(t)
            trace.sort(key=lambda r:int(r['cycle']))
            with self.subTest(t=t),self.assertRaisesRegex(AssertionError,'resume'):
                check_batch_rhs(text,cfg,trace)

    def test_reject_duplicate_batch_and_wrong_input_order(self):
        text,cfg,trace,jobs,ends=fixture()
        with self.assertRaises(AssertionError):
            check_input_batches(text,dict(cfg,**{'--coeff-input-l-first':'0'}),trace,jobs,ends)
        with self.assertRaises(AssertionError):
            check_input_batches(text+'\n'+next(s for s in text.splitlines() if 'event=ready region=1' in s),cfg,trace,jobs,ends)

    def test_reject_new_request_without_execution_credit(self):
        text,cfg,trace,_,_=fixture()
        trace.append(dict(trace[0],requestId='2',stage='COEFF3_ISSUE',cycle='21'))
        with self.assertRaises(AssertionError):check_batch_rhs(text,cfg,trace)
