"""Bounded snapshot ownership, ingress, concurrency and credit-release proofs."""
import _support
from copy import deepcopy
from pathlib import Path
import subprocess
import tempfile
import unittest
from check_summary_hash import check_summary_hash,SEED,PRIME,MASK


class SnapshotTests(unittest.TestCase):
    def test_native_snapshot_storage_and_registered_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            binary=Path(folder)/'snapshots'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-I'+str(_support.REPO_ROOT/'src'),str(_support.FIXTURES/'summary_snapshots.cc'),'-o',str(binary)],check=True,capture_output=True)
            self.assertIn('SUMMARY_SNAPSHOTS_PASS',subprocess.run([str(binary)],check=True,capture_output=True,text=True).stdout)

    def fixture(self):
        rows=[];trace=[]
        def emit(c,p,**kw):rows.append((c,p+' cycle='+str(c)+' '+ ' '.join(k+'='+str(v) for k,v in kw.items())))
        for engine in range(2):
            token=engine+4;request=engine+2;generation=engine+7;owner=1
            data=[i for i in range(40)];data[0]=request;data[1]=generation;data[20]=1
            emit(0,'CFD_SUMMARY_SNAPSHOT',event='allocate',token=token,owner=owner,request=request,slot=engine,depth=2)
            for i in range(5):emit(1+5*engine+i,'CFD_SUMMARY_SNAPSHOT',event='copy',token=token,owner=owner,first=8*i,words=8,**{'w'+str(j):f'{data[8*i+j]:016x}' for j in range(8)})
            at=5+5*engine
            emit(at,'CFD_SUMMARY_SNAPSHOT',event='detach',token=token,owner=owner,request=request)
            emit(at+1,'CFD_SUMMARY_HASH',event='accept',token=token,owner=owner,request=request,words=40,word_cycles=2,bytes=128,engine=engine,snapshot=1)
            digest=SEED
            for i,word in enumerate(data):
                after=((digest^word)*PRIME)&MASK
                emit(at+3+2*i,'CFD_SUMMARY_HASH',event='word',token=token,owner=owner,request=request,index=i,word=f'{word:016x}',before=f'{digest:016x}',after=f'{after:016x}',engine=engine,snapshot=1)
                digest=after
            emit(at+81,'CFD_SUMMARY_HASH',event='complete',token=token,owner=owner,request=request,hash=f'{digest:016x}',engine=engine,snapshot=1)
            emit(at+81,'CFD_RECORD_DIGEST_SOURCE',request=request,generation=generation,hash=f'{digest:016x}',summary_owner=owner)
            for stage,cy in [('REQUEST_COMPLETE',0),('REQUEST_SNAPSHOT_DETACH',at),('REQUEST_AUTO_RETIRE',at),('REQUEST_SUMMARY',at+81)]:trace.append(dict(requestId=str(request),generation=str(generation),cycle=str(cy),stage=stage))
        emit(100,'CFD_SUMMARY_SNAPSHOTS',depth=2,engines=2,copy_words=8,entry_bytes=512,peak=2,live=0,issued=2,completed=2,dropped=0)
        rows.sort(key=lambda r:r[0])
        return '\n'.join(r[1] for r in rows),{'--coeff-record-policy':'cold-summary','--coeff-summary-depth':'2','--coeff-summary-engines':'2'},trace

    def test_two_overlapping_engines_with_five_tick_ingress(self):
        r=check_summary_hash(*self.fixture());self.assertEqual((r['records'],r['engine_busy_cycles'],r['snapshot_copy_cycles']),(2,160,10))

    def test_reject_corrupt_snapshot_and_early_detach(self):
        text,config,trace=self.fixture()
        for before,after in [('w0=0000000000000002','w0=0000000000000099'),('first=8','first=0'),('cycle=5 event=detach','cycle=4 event=detach'),('cycle=6 event=accept','cycle=5 event=accept')]:
            with self.assertRaises(AssertionError):check_summary_hash(text.replace(before,after,1),config,trace)

    def test_reject_engine_alias_and_storage_reuse(self):
        text,config,trace=self.fixture()
        for before,after in [('engine=1','engine=0'),('slot=1','slot=0'),('word_cycles=2','word_cycles=1')]:
            with self.assertRaises(AssertionError):check_summary_hash(text.replace(before,after),config,trace)

    def test_reject_missing_release_or_unbounded_engine(self):
        text,config,trace=self.fixture()
        changed=deepcopy(trace);changed[2]['cycle']='4'
        with self.assertRaises(AssertionError):check_summary_hash(text,config,changed)
        with self.assertRaises(AssertionError):check_summary_hash(text,dict(config,**{'--coeff-summary-engines':'1'}),trace)

    def test_abort_drains_both_active_registered_engines(self):
        text,config,trace=self.fixture()
        rows=[]
        for line in text.splitlines():
            if line.startswith('CFD_RECORD_DIGEST_SOURCE') or line.startswith('CFD_SUMMARY_SNAPSHOTS '):continue
            cycle=int(line.split('cycle=')[1].split()[0])
            if cycle<=20 and 'event=complete' not in line:rows.append((cycle,line))
        rows += [(20,'CFD_LINE_ABORT cycle=20 token=1 event=begin'),
                 (21,'CFD_SUMMARY_HASH cycle=21 event=drop token=5 owner=1 engine=1 snapshot=1'),
                 (22,'CFD_SUMMARY_HASH cycle=22 event=drop token=4 owner=1 engine=0 snapshot=1'),
                 (23,'CFD_SUMMARY_SNAPSHOTS depth=2 engines=2 copy_words=8 entry_bytes=512 peak=2 live=0 issued=2 completed=0 dropped=2')]
        rows.sort(key=lambda x:x[0]);text='\n'.join(x[1] for x in rows)
        trace=[r for r in trace if r['stage']!='REQUEST_SUMMARY']
        self.assertEqual(check_summary_hash(text,config,trace)['dropped'],2)
        with self.assertRaises(AssertionError):check_summary_hash(text.replace('cycle=21 event=drop','cycle=20 event=drop'),config,trace)

    def test_abort_discards_partially_copied_entries(self):
        text,config,trace=self.fixture()
        rows=[line for line in text.splitlines() if line.startswith('CFD_SUMMARY_SNAPSHOT ') and ('event=allocate' in line or ('event=copy' in line and int(line.split('cycle=')[1].split()[0])<3))]
        rows += ['CFD_LINE_ABORT cycle=3 token=1 event=begin',
                 'CFD_SUMMARY_SNAPSHOT cycle=3 event=drop token=4 owner=1 detached=0',
                 'CFD_SUMMARY_SNAPSHOT cycle=3 event=drop token=5 owner=1 detached=0',
                 'CFD_SUMMARY_SNAPSHOTS depth=2 engines=2 copy_words=8 entry_bytes=512 peak=2 live=0 issued=2 completed=0 dropped=2']
        self.assertEqual(check_summary_hash('\n'.join(rows),config,[r for r in trace if r['stage']=='REQUEST_COMPLETE'])['dropped'],2)

    def test_additional_snapshot_and_integer_state_are_paid(self):
        from single_line_config_audit import Effective,reservation_components
        base={'options':{'coeff_input_slots':8,'lu5_pending_depth':1,'coeff3_pending_depth':1,'coeff_output_buffer_depth':5,'coeff_line_opt_level':13,'coeff_record_policy':'cold-summary','coeff_spm_outstanding':128},'controller_environment':{}}
        legacy=reservation_components(Effective(base))
        newer=deepcopy(base);newer['options'].update(coeff_summary_depth=4,coeff_summary_engines=2)
        added=reservation_components(Effective(newer))
        self.assertEqual(sum(x['bytes'] for x in added)-sum(x['bytes'] for x in legacy),4*512+128)

        newer['options'].update(coeff_summary_depth=24, coeff_summary_engines=16)
        event = Effective(newer)
        reservation_components(event)
        self.assertIn('invalid bounded summary engines/snapshot configuration', event.errors)

        newer['options']['coeff_compute_backend'] = 'sme-za'
        newer['controller_environment']['GEM5_CFD_SME_ZA'] = '1'
        sme = Effective(newer)
        wider = reservation_components(sme)
        self.assertEqual(sme.errors, [])
        self.assertEqual(sum(x['bytes'] for x in wider) - sum(x['bytes'] for x in legacy),
                         24 * 512 + 15 * 128)
