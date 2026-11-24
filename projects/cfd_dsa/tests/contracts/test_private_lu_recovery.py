import _support
from pathlib import Path
import unittest

from check_private_lu import validate_private_lu


class PrivateLuRecoveryTests(unittest.TestCase):
    def fixture(self):
        text=[];trace=[]
        for i in range(4):
            request=str(i+1);generation=100+i*10;base=1000+i*100;ok=i%2
            text.append(f'CFD_LU_OUTPUT_RANGE round={i+1} generation={generation} publish=0 base=0x1000 bytes=200 cells=1')
            events=[('REQUEST_ALLOC',0),('LU_COMPLETE',1)]
            if ok:
                events += [('DIRECT_BASE_READY',2),('HANDOFF_PRODUCE',3),('REQUEST_COMPLETE',4)]
                text += [f'CFD_LINE_SCHEDULE token={request} cache_hit=0',
                         'CFD_SINGLE_LINE_NUMERIC values=10 mismatches=0']
            else:
                events += [('CANCEL_PENDING',3),('REQUEST_DETACH',4)]
                text += [f'CFD_LINE_ABORT token={request} event=begin cycle={base+3} status=9',
                         f'CFD_LINE_ABORT token={request} event=visible cycle={base+8} status=9 dma_live=0 records=0 children=0']
            events.append(('REQUEST_AUTO_RETIRE',5))
            text.append(f'CFD_PRIVATE_LU event=local-ready request={request} generation={generation} cell=0 cycle={base+2}')
            text.append(f'CFD_RECOVERY_CONTRACT step={i} expected={ok} actual={ok}')
            for stage,offset in events:
                trace.append(dict(requestId=request,generation=str(generation),cell='0',
                    stage=stage,cycle=str(base+offset),k='-1',requester='',
                    packetId='0',address='0x0',size='0'))
        # The cancelled logical context is gone; the physical write still acks.
        trace.append(dict(requestId='1',generation='100',cell='0',stage='SPM_PACKET_ISSUE',
            cycle='1002',k='-1',requester='CoeffDrain',packetId='77',address='0x2000',size='40'))
        text += ['CFD_DMA_PACKET packet=77 event=queue op=W bytes=40 tick=1002',
                 'CFD_DMA_PACKET packet=77 event=send tick=1002',
                 'CFD_DMA_PACKET packet=77 event=response tick=1006',
                 'LUSGS_RECOVERY_CONTRACT_PASS']
        return '\n'.join(text),{'--lusgs-cells':'1','--coeff-publish-lu':'0',
                                '--coeff-recovery-test':'1'},trace

    def test_cancelled_and_successful_rounds_have_distinct_lifecycles(self):
        report=validate_private_lu(*self.fixture())
        self.assertTrue(report['recovery'])
        self.assertEqual([r['aborted'] for r in report['rounds']],[True,False,True,False])
        self.assertTrue(all(r['physical_lu_bytes']==0 for r in report['rounds']))

    def test_cancelled_write_requires_actual_transport_ack(self):
        text,config,trace=self.fixture()
        with self.assertRaisesRegex(ValueError,'physical acknowledgement'):
            validate_private_lu(text.replace('event=response tick=1006','event=ignored tick=1006'),config,trace)

    def test_cancelled_call_cannot_write_into_the_private_lu_range(self):
        text,config,trace=self.fixture();trace[-1]['address']='0x1000'
        with self.assertRaisesRegex(ValueError,'physical DMA wrote the private LU'):
            validate_private_lu(text,config,trace)

    def test_abort_visibility_waits_for_request_retirement(self):
        text,config,trace=self.fixture()
        with self.assertRaisesRegex(ValueError,'retire before visibility'):
            validate_private_lu(text.replace('cycle=1008 status=9','cycle=1004 status=9'),config,trace)

    def test_cancelled_factor_ready_must_have_real_factor_completion(self):
        text,config,trace=self.fixture()
        trace=[r for r in trace if not(r['requestId']=='1' and r['stage']=='LU_COMPLETE')]
        with self.assertRaisesRegex(ValueError,'ready without completed factor'):
            validate_private_lu(text,config,trace)

    def test_duplicate_generation_cell_is_rejected_even_for_aborted_call(self):
        text,config,trace=self.fixture();copy=dict(trace[0],requestId='999');trace.append(copy)
        with self.assertRaisesRegex(ValueError,'duplicate private LU request identity'):
            validate_private_lu(text,config,trace)

    def test_factor_publication_after_retirement_is_rejected(self):
        text,config,trace=self.fixture()
        with self.assertRaisesRegex(ValueError,'factor/retirement lifetime'):
            validate_private_lu(text.replace('cell=0 cycle=1002','cell=0 cycle=1006'),config,trace)

    def test_cancelled_warm_call_can_consume_cached_factors(self):
        text,config,trace=self.fixture()
        for row in trace:
            if row['requestId']=='1' and row['stage']=='LU_COMPLETE':
                row['stage']='LU_CACHE_REUSE'
        result=validate_private_lu(text,config,trace)
        self.assertEqual(result['rounds'][0]['cache_hit'],1)


if __name__=='__main__':unittest.main()
