import _support
from pathlib import Path
import unittest

from onchip_replay import replay_onchip
from trace_text import fields,prefixed_lines


class StreamedOnchipTests(unittest.TestCase):
    config={'--coeff-onchip-queue-depth':'2','--coeff-onchip-compute-reserve':'1',
        '--coeff-spm-banks':'2','--coeff-spm-bank-granularity':'8',
        '--coeff-spm-read-ports':'1','--coeff-spm-write-ports':'1',
        '--coeff-onchip-latency':'2','--coeff-onchip-arb-latency':'1',
        '--coeff-onchip-arb-tasks':'2','--coeff-line-spm-bytes':'1024'}

    def fixture(self,cancelled=1):
        lines=['CFD_ONCHIP_START cycle=1 id=1 owner=7 base=0x0 words=2 stride=8 op=W active=0 priority=2',
            'CFD_ONCHIP_ACTIVATE cycle=3 id=1',
            'CFD_ONCHIP_ACCESS cycle=4 id=1 owner=7 op=W addr=0x0 bank=0 priority=2',
            'CFD_ONCHIP_ACCESS cycle=4 id=1 owner=7 op=W addr=0x8 bank=1 priority=2',
            'CFD_ONCHIP_COMPLETE cycle=6 id=1 owner=7',
            'CFD_ONCHIP_START cycle=7 id=2 owner=7 base=0x10 words=1 stride=8 op=R active=1 priority=0',
            'CFD_ONCHIP_ACCESS cycle=8 id=2 owner=7 op=R addr=0x10 bank=0 priority=0',
            'CFD_ONCHIP_COMPLETE cycle=10 id=2 owner=7']
        for i in range(cancelled):
            lines += [f'CFD_ONCHIP_START cycle={11+i} id={3+i} owner=7 base=0x18 words=1 stride=8 op=W active=0 priority=2',
                      f'CFD_ONCHIP_CANCEL cycle={11+i} id={3+i}']
        lines += [f'CFD_ONCHIP_SUMMARY banks=2 grain=8 read_ports=1 write_ports=1 latency=2 depth=2 created={2+cancelled} completed=2 cancelled={cancelled} live=0 peak=1 compute_reserve=1 inactive=0',
                  'CFD_ONCHIP_BANK bank=0 read_words=1 write_words=1',
                  'CFD_ONCHIP_BANK bank=1 read_words=0 write_words=1']
        return '\n'.join(lines)

    def test_every_word_and_completed_job_survives_streaming(self):
        _,jobs,ends,words,metrics=replay_onchip(self.fixture(),self.config)
        self.assertEqual(set(jobs),{'1','2'});self.assertEqual(set(ends),{'1','2'})
        self.assertEqual(sum(words.values()),3)
        self.assertEqual(metrics['cancelled_job_metadata_retained'],0)

    def test_cancel_probe_count_does_not_grow_retained_job_table(self):
        result=replay_onchip(self.fixture(20000),self.config)
        self.assertEqual(len(result[1]),2)
        self.assertEqual(result[-1]['cancelled'],20000)
        self.assertEqual(result[-1]['peak_live_jobs'],1)

    def test_inactive_dma_reservation_cannot_access_early(self):
        with self.assertRaisesRegex(AssertionError,'activation/arbitration'):
            replay_onchip(self.fixture().replace('ACCESS cycle=4','ACCESS cycle=3'),self.config)

    def test_missing_word_and_duplicate_word_are_rejected(self):
        line='CFD_ONCHIP_ACCESS cycle=4 id=1 owner=7 op=W addr=0x8 bank=1 priority=2'
        for text in [self.fixture().replace(line,''),self.fixture().replace(line,line+'\n'+line)]:
            with self.assertRaises(AssertionError):replay_onchip(text,self.config)

    def test_bank_port_limit_applies_across_words(self):
        text=self.fixture().replace('words=2 stride=8','words=2 stride=16')
        text=text.replace('addr=0x8 bank=1','addr=0x10 bank=0')
        with self.assertRaisesRegex(AssertionError,'bank port oversubscribed'):
            replay_onchip(text,self.config)

    def test_active_job_cannot_be_cancelled(self):
        text=self.fixture().replace('base=0x18 words=1 stride=8 op=W active=0',
                                    'base=0x18 words=1 stride=8 op=W active=1')
        with self.assertRaisesRegex(AssertionError,'active SPM job cancelled'):
            replay_onchip(text,self.config)

    def test_reserved_compute_credit_and_unique_ids_remain_enforced(self):
        line='CFD_ONCHIP_START cycle=1 id=2 owner=7 base=0x18 words=1 stride=8 op=W active=0 priority=2'
        text=self.fixture().replace('CFD_ONCHIP_ACTIVATE',line+'\nCFD_ONCHIP_ACTIVATE',1)
        with self.assertRaisesRegex(AssertionError,'reserved compute credit'):
            replay_onchip(text,self.config)
        text=self.fixture(2).replace('id=4','id=3')
        with self.assertRaisesRegex(AssertionError,'task ID reused'):
            replay_onchip(text,self.config)

    def test_completion_and_capacity_checks_are_not_relaxed(self):
        with self.assertRaisesRegex(AssertionError,'completed before its latency'):
            replay_onchip(self.fixture().replace('COMPLETE cycle=6','COMPLETE cycle=5'),self.config)
        with self.assertRaises(AssertionError):
            replay_onchip(self.fixture().replace('base=0x10','base=0x400').replace('addr=0x10','addr=0x400'),self.config)

    def test_distinct_task_grants_are_capped_separately_from_bank_ports(self):
        lines=self.fixture().splitlines()
        lines=[line for line in lines if not ('id=2 ' in line or line.endswith('id=2'))]
        lines.insert(1,'CFD_ONCHIP_START cycle=1 id=2 owner=7 base=0x10 words=1 stride=8 op=R active=1 priority=0')
        where=lines.index('CFD_ONCHIP_COMPLETE cycle=6 id=1 owner=7')
        lines.insert(where,'CFD_ONCHIP_ACCESS cycle=4 id=2 owner=7 op=R addr=0x10 bank=0 priority=0')
        lines.insert(where+2,'CFD_ONCHIP_COMPLETE cycle=6 id=2 owner=7')
        text='\n'.join(lines).replace('live=0 peak=1','live=0 peak=2')
        self.assertEqual(replay_onchip(text,self.config)[-1]['peak_progress_tasks'],2)
        with self.assertRaisesRegex(AssertionError,'too many distinct tasks'):
            replay_onchip(text,dict(self.config,**{'--coeff-onchip-arb-tasks':'1'}))


class PrefixRecordTests(unittest.TestCase):
    def test_crlf_unicode_unterminated_tail_and_exact_prefix(self):
        text='ignored CFD_A a=0\r\nCFD_A a=1 note=中文\r\nCFD_AB a=2\n\nCFD_A a=3'
        self.assertEqual(fields(text,'CFD_A'),[{'a':'1','note':'中文'},{'a':'3'}])
        self.assertEqual(list(prefixed_lines(text,'CFD_A ')),['CFD_A a=1 note=中文','CFD_A a=3'])

    def test_prefix_is_literal_and_empty_input_is_empty(self):
        self.assertEqual(fields('CFD_[X] x=2\nCFD_X x=3','CFD_[X]'),[{'x':'2'}])
        self.assertEqual(fields('','CFD_A'),[])


if __name__=='__main__':unittest.main()
