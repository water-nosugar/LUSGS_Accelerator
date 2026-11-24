"""Check the actual integer engine and reject timing/ownership corruption."""
import _support
from copy import deepcopy
from pathlib import Path
import subprocess
import tempfile
import unittest
from check_summary_hash import check_summary_hash,SEED,PRIME,MASK


class SummaryHashTests(unittest.TestCase):
    def test_engine_word_count_matches_complete_metadata_abi(self):
        abi=(_support.REPO_ROOT/'src/arch/arm/cfd_compact_record_fields.inc').read_text()
        self.assertEqual(sum(line.startswith('CFD_RECORD_FIELD(') for line in abi.splitlines()),40)

    def test_actual_bounded_cpp_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            binary=Path(folder)/'hash'
            subprocess.run(['g++','-std=c++17','-O2','-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'summary_hash.cc'),'-o',str(binary)],check=True,capture_output=True,text=True)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
            self.assertIn('SUMMARY_HASH_PASS',result.stdout)

    def fixture(self):
        lines=['CFD_SUMMARY_HASH cycle=20 event=accept token=4 owner=1 request=2 words=40 word_cycles=2 bytes=128']
        digest=SEED
        for i in range(40):
            word=2 if i==0 else 3 if i==1 else 1 if i==20 else i
            result=((digest^word)*PRIME)&MASK
            lines.append(f'CFD_SUMMARY_HASH cycle={22+2*i} event=word token=4 owner=1 request=2 index={i} word={word:016x} before={digest:016x} after={result:016x}')
            digest=result
        lines+= [f'CFD_SUMMARY_HASH cycle=100 event=complete token=4 owner=1 request=2 hash={digest:016x}',
                 f'CFD_RECORD_DIGEST_SOURCE request=2 generation=3 hash={digest:016x} summary_owner=1']
        trace=[dict(requestId='2',generation='3',cycle='20',stage='REQUEST_COMPLETE'),
               dict(requestId='2',generation='3',cycle='100',stage='REQUEST_SUMMARY')]
        return '\n'.join(lines),{'--coeff-record-policy':'line-summary'},trace

    def test_registered_record_and_independent_digest(self):
        self.assertEqual(check_summary_hash(*self.fixture())['engine_busy_cycles'],80)

    def test_cold_policy_rejects_warm_engine_use(self):
        text,config,trace=self.fixture()
        config['--coeff-record-policy']='cold-summary'
        self.assertEqual(check_summary_hash(text,config,trace)['records'],1)
        text+='\nCFD_LINE_SCHEDULE token=1 cache_hit=1'
        with self.assertRaisesRegex(AssertionError,'warm record'):check_summary_hash(text,config,trace)

    def test_reject_early_result_missing_word_bad_digest_or_owner(self):
        args=self.fixture()
        for before,after in [('cycle=22 event=word','cycle=21 event=word'),
                             ('index=19','index=18'),('after=','after=1'),
                             ('cycle=40 event=word token=4 owner=1','cycle=40 event=word token=4 owner=9')]:
            with self.assertRaises(AssertionError):check_summary_hash(args[0].replace(before,after,1),*args[1:])
        trace=deepcopy(args[2]);trace[1]['cycle']='99'
        with self.assertRaises(AssertionError):check_summary_hash(args[0],args[1],trace)
