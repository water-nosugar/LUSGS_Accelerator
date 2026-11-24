"""Native checks of the real accelerator-study CLI and cache-touch contract."""
import _support
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = _support.REPO_ROOT
BENCHMARK = ROOT/'projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c'
MASK = (1 << 64)-1


def fnv_words(words):
    result = 14695981039346656037
    for value in words:
        result = ((result ^ value)*1099511628211) & MASK
    return result


class StudyConditioningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='cfd-study-conditioning-')
        cls.binary = Path(cls.temporary.name)/'conditioning'
        source = Path(cls.temporary.name)/'conditioning.c'
        source.write_text('''
#define main step2_benchmark_main
#include "'''+str(BENCHMARK)+'''"
#undef main
int main(int argc, char **argv)
{
    if (argc == 2 && !strncmp(argv[1], "condition-", 10)) {
        Problem p;
        init_problem(&p, 1, 2);
        double *arrays[] = {p.d_mat, p.l_mat, p.u_mat, p.rhs};
        const size_t sizes[] = {50, 50, 50, 10};
        for (unsigned a = 0; a < 4; ++a)
            for (size_t i = 0; i < sizes[a]; ++i)
                arrays[a][i] = a*100.0+i+0.25;
        Config cfg = {0};
        cfg.coeff_study_cache = argv[1]+10;
        const uint64_t first = study_condition_cache(&p, &cfg);
        const uint64_t second = study_condition_cache(&p, &cfg);
        if (!strcmp(cfg.coeff_study_cache, "legacy")) {
            if (first || second || study_eviction || study_condition_round) return 3;
        } else if (first != 1 || second != 2) return 4;
        for (unsigned a = 0; a < 4; ++a)
            for (size_t i = 0; i < sizes[a]; ++i)
                if (arrays[a][i] != a*100.0+i+0.25) return 5;
        free(study_eviction);
        free_problem(&p);
        puts("STUDY_CONDITIONING_PASS");
        return 0;
    }
    Config cfg;
    parse_args(argc, argv, &cfg);
    printf("CLI_STUDY_CACHE=%s\\n", cfg.coeff_study_cache);
    return 0;
}
''')
        subprocess.run(['cc', '-O2', str(source), '-o', str(cls.binary), '-lm'],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, *args):
        return subprocess.run([str(self.binary), *args], capture_output=True, text=True)

    def test_legacy_does_not_condition_or_allocate(self):
        result = self.run_case('condition-legacy')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'STUDY_CONDITIONING_PASS\n')

    def test_prepared_reads_only_original_inputs_with_expected_fingerprint(self):
        result = self.run_case('condition-prepared')
        self.assertEqual(result.returncode, 0, result.stderr)
        words = []
        for a, count in enumerate((50, 50, 50, 10)):
            indices = list(range(0, count, 8))
            if (count-1) % 8:
                indices.append(count-1)
            words.extend(struct.unpack('<Q', struct.pack('<d', a*100+i+0.25))[0]
                         for i in indices)
        expected = str(fnv_words(words))
        checksums = re.findall(r'checksum=(\d+)', result.stdout)
        self.assertEqual(checksums, [expected, expected])
        self.assertEqual(result.stdout.count('span_bytes=1280 '), 2)
        self.assertIn('reference_before_roi=0', result.stdout)
        self.assertIn('eviction_bytes=0 ', result.stdout)

    def test_evicted_uses_identical_finite_buffer_on_each_round(self):
        result = self.run_case('condition-evicted')
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = str(fnv_words(i ^ 0x9e3779b97f4a7c15
            for i in range(0, 16*1024*1024//8, 8)))
        self.assertEqual(re.findall(r'checksum=(\d+)', result.stdout), [expected, expected])
        self.assertEqual(result.stdout.count('span_bytes=16777216 '), 2)
        self.assertIn('cache-conditioning-not-guaranteed-cold', result.stdout)

    def test_cli_defaults_legacy_and_rejects_unsupported_modes(self):
        result = self.run_case()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('CLI_STUDY_CACHE=legacy', result.stdout)
        for value in ('cold', 'warm', '', 'prepared'):
            result = self.run_case('--coeff-study-cache', value)
            self.assertEqual(result.returncode, 2, result.stderr)

    def test_cli_requires_external_checked_nonfault_direct_stream(self):
        base = ['--lusgs-mode', 'step2-pretransform-raw-optprep-stream',
            '--coeff-preprocess-model', 'event', '--lu5-model', 'event',
            '--coeff-streaming-enable', '1', '--coeff-stream-line-autonomous', '1',
            '--coeff-stream-dinv-consumer', 'direct', '--coeff-stream-lbar-consumer', 'direct',
            '--coeff-stream-ubar-consumer', 'direct', '--coeff-base-solve', 'direct',
            '--coeff-input-file', 'fixture.cfd5', '--coeff-study-cache', 'prepared']
        result = self.run_case(*base)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('CLI_STUDY_CACHE=prepared', result.stdout)
        for override in (['--coeff-base-solve', 'inverse'],
                         ['--coeff-recovery-test', '1'],
                         ['--coeff-cache-contract-test', '1'],
                         ['--lusgs-check', '0']):
            result = self.run_case(*(base+override))
            self.assertEqual(result.returncode, 2, result.stderr)


if __name__ == '__main__':
    unittest.main()
