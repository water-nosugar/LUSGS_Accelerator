"""Fault injection into the independent private/public LU DMA contract."""
import _support
from copy import deepcopy
from pathlib import Path
import unittest
from check_private_lu import validate_private_lu


class PrivateLuAcceptanceTests(unittest.TestCase):
    def fixture(self, transition=False):
        modes = [0,0,1,1,0] if transition else [0]
        hits = [0,1,0,1,0] if transition else [0]
        text=[];trace=[]
        for i,(publish,hit) in enumerate(zip(modes,hits)):
            request=str(i+1);generation=100+i*2;base=1000+i*100
            text += [f'CFD_LU_OUTPUT_RANGE round={i+1} generation={generation} publish={publish} base=0x1000 bytes=200 cells=1',
                f'CFD_LINE_SCHEDULE token={i+1} cache_hit={hit}',
                f'CFD_SINGLE_LINE_NUMERIC values=10 mismatches=0']
            for offset,stage in enumerate(('REQUEST_ALLOC',
                    'LU_CACHE_REUSE' if hit else 'LU_COMPLETE', 'DIRECT_BASE_READY',
                    'HANDOFF_PRODUCE','REQUEST_COMPLETE','REQUEST_AUTO_RETIRE')):
                trace.append(dict(requestId=request,generation=str(generation),cell='0',
                    stage=stage,cycle=str(base+offset),k='-1',requester='',packetId='0',address='0x0',size='0'))
            if not publish:
                text.append(f'CFD_PRIVATE_LU cycle={base+1} token={i+1} request={request} generation={generation} cell=0 event=local-ready')
            if publish and not hit:
                self.packet(trace,request,4096,200,100+i)
            self.packet(trace,request,8192,40,200+i)
            if transition:
                text.append(f'CFD_LU_OUTPUT_TRANSITION step={i} cells=1 version=1 publish={publish} expected_cache_hit={hit} numeric_pass=1 output_contract_pass=1 cycles=100')
        if transition:text.append('LUSGS_LU_OUTPUT_TRANSITION_PASS')
        return '\n'.join(text),{'--lusgs-cells':'1','--coeff-publish-lu':'0',
            '--coeff-lu-output-transition-test':str(int(transition))},trace

    @staticmethod
    def packet(trace,request,address,size,packet):
        for stage,cycle in [('SPM_PACKET_ISSUE','1003'),('SPM_PACKET_RESPONSE','1004')]:
            trace.append(dict(requestId=request,generation='100',cell='0',stage=stage,
                cycle=cycle,k='-1',requester='CoeffDrain',packetId=str(packet),
                address=hex(address),size=str(size)))

    def test_private_factor_and_lifecycle_without_external_lu(self):
        report=validate_private_lu(*self.fixture())
        self.assertEqual(report['rounds'][0]['physical_lu_bytes'],0)

    def test_rejects_hidden_physical_write_even_without_drain_label(self):
        text,config,trace=self.fixture()
        self.packet(trace,'1',4096,8,999)
        with self.assertRaisesRegex(ValueError,'physical DMA wrote'):
            validate_private_lu(text,config,trace)

    def test_rejects_fake_cached_lu_drain_completion(self):
        text,config,trace=self.fixture()
        row=deepcopy(trace[0]);row['stage']='DRAIN_LU_DONE';trace.append(row)
        with self.assertRaisesRegex(ValueError,'faked'):
            validate_private_lu(text,config,trace)

    def test_transition_requires_misses_on_publication_changes(self):
        text,config,trace=self.fixture(True)
        result=validate_private_lu(text,config,trace)
        self.assertEqual([r['physical_lu_bytes']for r in result['rounds']],[0,0,200,0,0])
        bad=text.replace('CFD_LINE_SCHEDULE token=3 cache_hit=0',
                         'CFD_LINE_SCHEDULE token=3 cache_hit=1')
        with self.assertRaisesRegex(ValueError,'incompatible'):
            validate_private_lu(bad,config,trace)

    def test_transition_rejects_skipped_public_output_and_numeric_failure(self):
        text,config,trace=self.fixture(True)
        missing=[row for row in trace if row['packetId']!='102']
        with self.assertRaisesRegex(ValueError,'public LU output missing'):
            validate_private_lu(text,config,missing)
        with self.assertRaisesRegex(ValueError,'numeric'):
            validate_private_lu(text.replace('mismatches=0','mismatches=1',1),config,trace)

    def test_rejects_missing_factor_or_local_readiness(self):
        text,config,trace=self.fixture()
        with self.assertRaisesRegex(ValueError,'factor calculation'):
            validate_private_lu(text,config,[r for r in trace if r['stage']!='LU_COMPLETE'])
        bad=text+'\n'+next(s for s in text.splitlines()if s.startswith('CFD_PRIVATE_LU '))
        with self.assertRaisesRegex(ValueError,'missing or duplicated'):
            validate_private_lu(bad,config,trace)


if __name__=='__main__':unittest.main()
