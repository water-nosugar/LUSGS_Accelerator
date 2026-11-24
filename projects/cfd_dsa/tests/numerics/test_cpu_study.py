"""Independent input-oracle tests for the standalone CPU study kernels."""
import _support
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
import runpy
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = _support.PROJECT_ROOT
from single_line_dataset import write_dataset


def records(cells):
    result = []
    for i in range(cells):
        d = [4.0 + r/5 if r == c else ((r+2*c)%5-2)/20
             for r in range(5) for c in range(5)]
        l = [(-.09 if r == c else .01*(r-c)) if i else 0.0
             for r in range(5) for c in range(5)]
        u = [(.07 if r == c else .004*(r+c)) if i+1 < cells else 0.0
             for r in range(5) for c in range(5)]
        result.append(d+l+u+[.2*(r+1)+.03*i for r in range(5)])
    return result


class CpuStudyKernelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='cpu-study-test-')
        cls.directory = Path(cls.tmp.name)
        cls.binary = cls.directory/'cpu-study'
        subprocess.run(['cc', '-O3', '-ffp-contract=off', '-Wall', '-Wextra',
                        str(ROOT/'benchmarks/lusgs/test_cfd_cpu_study.c'),
                        '-o', str(cls.binary), '-lm'], check=True,
                       capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_kernel(self, name, cells=17, mode='raw', cache='prepared', expected=None):
        dataset = self.directory/(name+'.bin')
        write_dataset(dataset, records(cells), expected=expected)
        return subprocess.run([str(self.binary), '--coeff-input-file', str(dataset),
            '--lusgs-cells', str(cells), '--bench-iters', '3',
            '--cpu-study-mode', mode, '--cpu-study-cache', cache],
            capture_output=True, text=True)

    def test_raw_and_pretransform_cold_and_reuse_match_decimal_oracle(self):
        for cells in (1, 17):
            for mode in ('raw', 'pretransform'):
                with self.subTest(cells=cells, mode=mode):
                    result = self.run_kernel(f'{mode}-{cells}', cells, mode)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                    self.assertEqual(result.stdout.count('mismatches=0'), 3)
                    self.assertEqual(result.stdout.count('numeric_ok=1'), 3)
                    self.assertIn('CPU_STUDY_PASS', result.stdout)
                    self.assertIn('timing=native-validation-only', result.stdout)
                    self.assertEqual(result.stdout.count('coeff_state=cold'), 1)
                    self.assertEqual(result.stdout.count('coeff_state=reused'), 2)

    def test_eviction_policy_preserves_numerics_and_fingerprints_pattern(self):
        result = self.run_kernel('evicted', cache='evicted', mode='pretransform')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        h = 14695981039346656037
        for i in range(0, 16*1024*1024//8, 8):
            h = ((h ^ (i ^ 0x9e3779b97f4a7c15))*1099511628211) & ((1<<64)-1)
        conditions = re.findall(r'CPU_STUDY_CONDITION.*', result.stdout)
        self.assertEqual(len(conditions), 3)
        for line in conditions:
            self.assertIn('span_bytes=16777216', line)
            self.assertIn(f'checksum={h}', line)

    def test_prepared_policy_reads_original_input_arrays(self):
        result = self.run_kernel('prepared-fingerprint')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        raw = records(17)
        arrays = [sum([row[start:end] for row in raw], [])
                  for start, end in ((0,25), (25,50), (50,75), (75,80))]
        h = 14695981039346656037
        for values in arrays:
            indices = list(range(0, len(values), 8))
            if (len(values)-1)%8: indices.append(len(values)-1)
            for i in indices:
                bits = struct.unpack('<Q', struct.pack('<d', values[i]))[0]
                h = ((h ^ bits)*1099511628211) & ((1<<64)-1)
        self.assertIn(f'span_bytes={17*640} checksum={h}', result.stdout)

    def test_numeric_failure_never_emits_pass(self):
        result = self.run_kernel('wrong-oracle', cells=1, expected=[[0.0]*10])
        self.assertEqual(result.returncode, 1)
        self.assertIn('CPU_STUDY_FAIL', result.stdout)
        self.assertNotIn('CPU_STUDY_PASS', result.stdout)

    def test_invalid_and_missing_arguments_are_rejected(self):
        for arguments in ([], ['--lusgs-cells', '-1'], ['--cpu-study-mode'],
                          ['--unknown', '1']):
            result = subprocess.run([str(self.binary), *arguments],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('CPU_STUDY_ERROR', result.stderr)


class CpuStudyWrapperTests(unittest.TestCase):
    def test_gem5_entry_namespace_expands_stream_profile(self):
        original_run_path = runpy.run_path
        captured = {}
        def common_config(path, **kwargs):
            captured.update(path=str(path), argv=list(sys.argv), **kwargs)
        with patch.object(sys, 'argv', ['run_cfd_cpu_study.py',
                '--lusgs-cells=64', '--coeff-input-file=/tmp/fixture.cfd5']), \
                patch('runpy.run_path', side_effect=common_config):
            original_run_path(str(ROOT/'configs/lusgs/run_cfd_cpu_study.py'),
                              run_name='__m5_main__')
        self.assertTrue(captured['path'].endswith('/configs/run_cfd_dsa.py'))
        argv = captured['argv']
        position = argv.index('--coeff-streaming-enable')
        self.assertEqual(argv[position+1], '1')
        self.assertEqual(captured['init_globals']['cpu_study_args'][:2],
                         ['--cpu-study-mode', 'raw'])

    def setUp(self):
        spec = spec_from_file_location('cpu_study_wrapper',
            ROOT/'configs/lusgs/run_cfd_cpu_study.py')
        self.wrapper = module_from_spec(spec)
        spec.loader.exec_module(self.wrapper)

    def test_guest_options_do_not_reach_common_getopt(self):
        remaining, settings = self.wrapper.extract_cpu_study_options([
            '--cpu=o3', '--cpu-study-mode=pretransform',
            '--cpu-study-cache', 'evicted', '--lusgs-cells=17'])
        self.assertEqual(remaining, ['--cpu=o3', '--lusgs-cells=17'])
        self.assertEqual(settings, dict(mode='pretransform', cache='evicted'))

    def test_invalid_guest_modes_fail_early(self):
        for argv in (['--cpu-study-mode=bad'], ['--cpu-study-cache=cold'],
                     ['--cpu-study-cache']):
            with self.assertRaises(ValueError):
                self.wrapper.extract_cpu_study_options(argv)


if __name__ == '__main__':
    unittest.main()
