"""Start-bank rotation preserves every byte, lease and physical access."""
import _support
from pathlib import Path
import subprocess,tempfile,unittest
from check_spm_slot_rotation import check_spm_slot_rotation,input_base,ring_base

def fixture(mode=3):
    cfg={'--coeff-spm-slot-rotation':str(mode),'--coeff-spm-banks':'4','--coeff-spm-bank-granularity':'8','--coeff-input-slots':'8','--lu5-pending-depth':'1','--coeff3-pending-depth':'1','--coeff-output-buffer-depth':'5','--coeff-line-opt-level':'13','--coeff-rhs-row-parallel':'1','--coeff-rhs-product-depth':'32','--coeff-completion-depth':'4','--coeff-stream-window':'13'}
    trace=[dict(stage='REQUEST_ALLOC',requestId='11',generation='9',cell='0')]
    jobs={};ends={};lines=[]
    def job(i,base,words,op,cycle,priority,active):
        jobs[str(i)]=dict(base=hex(base),words=str(words),stride='8',op=op,owner='55',cycle=str(cycle),active=str(active),priority=str(priority));ends[str(i)]=dict(cycle=str(cycle+3))
    for n,(packet,offset) in enumerate([(10,0),(11,16)],1):
        cycle=n;job(n,input_base(cfg,1)+offset,2,'W',cycle,2,0)
        trace.append(dict(stage='SPM_PACKET_ISSUE',requestId='11',generation='9',cell='0',requester='CoeffInput',packetId=str(packet),slot='1',size='16',cycle=str(cycle)))
        lines.append(f'CFD_SPM_SLOT_STORE cycle={cycle} request=11 generation=9 cell=0 token=77 owner=55 slot=1 job={n} offset={offset} packet={packet}')
    base=ring_base(cfg)+2*128+(16 if mode&1 else 0)
    job(3,base,2,'W',3,0,0);job(4,base+16,3,'W',5,0,0);job(5,base,5,'R',20,0,1)
    lines+=['CFD_LINE_STORAGE token=55 cells=9','CFD_DIRECT_RHS cycle=1 token=55 cell=2 event=allocate slot=2 depth=8',
       'CFD_DIRECT_RHS cycle=24 token=55 cell=2 event=consume job=5',
       'CFD_CACHE_DMA cycle=3 token=55 cell=2 event=issue kind=rhs bytes=16 packet=44 job=3',
       'CFD_CACHE_DMA cycle=5 token=55 cell=2 event=issue kind=rhs bytes=24 packet=45 job=4',
       f'CFD_SPM_SLOT_ROTATION_CAPACITY mode={mode} bytes=128 banks=4 grain=8 input_stride=8192 rhs_stride=128 max_shift=24 extra_ports=0 extra_units=0']
    return '\n'.join(lines),cfg,trace,jobs,ends

def replay(data):return check_spm_slot_rotation(*data)

class SpmSlotRotationTests(unittest.TestCase):
    def test_native_same_payloads_and_paid_bank_service(self):
        with tempfile.TemporaryDirectory() as d:
            binary=Path(d)/'rotation'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(_support.REPO_ROOT/'src'),str(_support.FIXTURES/'spm_slot_rotation_model.cc'),'-o',str(binary)],check=True,capture_output=True)
            self.assertIn('SPM_SLOT_ROTATION_PASS',subprocess.check_output([str(binary)],text=True))
    def test_all_enabled_modes_bind_both_sides_of_the_slot(self):
        for mode in [1,2,3]:
            r=replay(fixture(mode));self.assertEqual((r['ingress_stores'],r['rhs_loads'],r['rhs_stores']),(2,1,2))
            self.assertEqual(r['rotated_ingress_stores'],2 if mode&2 else 0)
            self.assertEqual(r['rotated_rhs_loads'],1 if mode&1 else 0)
    def test_wrong_writer_or_reader_bank_is_rejected(self):
        for job in ['1','3','5']:
            data=fixture();data[3][job]['base']=hex(int(data[3][job]['base'],16)-8)
            with self.subTest(job=job),self.assertRaisesRegex(AssertionError,'mapping'):replay(data)
    def test_missing_or_duplicate_ingress_and_missing_payload_rejected(self):
        data=fixture();line=next(l for l in data[0].splitlines() if l.startswith('CFD_SPM_SLOT_STORE '))
        for text in [data[0].replace(line,''),data[0]+'\n'+line,data[0].replace('bytes=24 packet=45','bytes=16 packet=45')]:
            with self.subTest(text=text[:20]),self.assertRaises(AssertionError):replay((text,*data[1:]))
    def test_load_cannot_bypass_real_store_return(self):
        data=fixture();data[4]['4']['cycle']='21'
        with self.assertRaisesRegex(AssertionError,'producer store'):replay(data)
    def test_disabled_mode_and_capability_budget_are_checked(self):
        data=fixture();data[1]['--coeff-spm-slot-rotation']='0'
        with self.assertRaisesRegex(AssertionError,'disabled'):replay(data)
        data=fixture();text=data[0].replace('bytes=128 banks=4','bytes=127 banks=4')
        with self.assertRaises(AssertionError):replay((text,*data[1:]))
    def test_all_capacity_ledgers_price_control_only(self):
        from check_single_line_storage import lifecycle_reserve
        self.assertEqual(lifecycle_reserve({'--coeff-spm-slot-rotation':'3'})-lifecycle_reserve({}),128)
        from reporting.test_single_line_config_audit import fixture as audit_fixture
        from single_line_config_audit import Effective,capacity_audit
        r,_=audit_fixture(1024);r['options'].update(coeff_spm_banks=4,coeff_spm_bank_granularity=8)
        before=capacity_audit(Effective(r))['reservation_bytes'];r['options']['coeff_spm_slot_rotation']=3
        self.assertEqual(capacity_audit(Effective(r))['reservation_bytes']-before,128)
