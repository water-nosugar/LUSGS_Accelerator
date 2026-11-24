"""Native tests of the real private-LU CLI and post-ROI guest validator."""
import _support
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = _support.REPO_ROOT
BENCHMARK = ROOT/'projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c'


class PrivateLuGuestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='private-lu-guest-')
        directory = Path(cls.temporary.name)
        source = directory/'guest-test.c'
        cls.binary = directory/'guest-test'
        source.write_text(r'''
#define main step2_benchmark_main
#include "'''+str(BENCHMARK)+r'''"
#undef main
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "ranges")) {
        Problem p;
        init_problem(&p, 1, 2);
        Config cfg = {0};
        report_lu_output_range(&p, &cfg, 1000);
        cfg.coeff_publish_lu = 1;
        report_lu_output_range(&p, &cfg, 1003);
        free_problem(&p);
        return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "validate-", 9)) {
        const char *mode = argv[1]+9;
        Problem p;
        init_problem(&p, 1, 2);
        Config cfg = {0};
        cfg.coeff_stream_dinv_consumer = "direct";
        cfg.coeff_stream_lbar_consumer = "direct";
        cfg.coeff_base_solve = "direct";
        cfg.coeff_validation = "full";
        cfg.coeff_stream_boundary_mask = 1;
        cfg.coeff_publish_lu = !strcmp(mode,"public");
        cfg.pretransform_diag_epsilon = 1e-12;
        Stats stats = {0};
        PretransformCheck check = {0};
        for (size_t cell=0; cell<2; ++cell) {
            double *d=mat_at(p.d_mat,&p,0,cell);
            double *l=mat_at(p.l_mat,&p,0,cell);
            double *u=mat_at(p.u_mat,&p,0,cell);
            double temp_lu[N*N];
            for (int i=0;i<N*N;++i) {
                d[i]=(i/N==i%N ? 4.0+i/N*.1 : .01*(i/N-i%N));
                l[i]=(cell && i/N==i%N ? -.2 : 0);
                u[i]=(!cell && i/N==i%N ? .3 : 0);
                p.l_bar_precomputed[cell*N*N+i]=0;
                p.lu_d[cell*N*N+i]=NAN;
            }
            if (!cfd_lu5_factor(d,temp_lu,cfg.pretransform_diag_epsilon)) return 20;
            trsm5_mrhs_software(temp_lu,u,mat_at(p.u_bar,&p,0,cell));
            if (cfg.coeff_publish_lu)
                memcpy(mat_at(p.lu_d,&p,0,cell),temp_lu,sizeof(temp_lu));
        }
        if (!strcmp(mode,"modified-poison")) p.lu_d[17]=0.0;
        if (!strcmp(mode,"different-nan")) {
            uint64_t different=0x7ff8000000000001ULL;
            memcpy(&p.lu_d[17],&different,sizeof(different));
        }
        if (!strcmp(mode,"wrong-ubar")) p.u_bar[3]+=.01;
        if (!strcmp(mode,"singular")) memset(p.d_mat,0,N*N*sizeof(double));
        int poison_ok=cfg.coeff_publish_lu || validate_local_lu_poison(&p);
        int coeff_ok=validate_streaming_coefficients(&p,&cfg,&stats,&check,NULL);
        printf("VALIDATE poison=%d coefficients=%d publication_bytes=%lu factors=%lu\n",
            poison_ok,coeff_ok,(unsigned long)stats.lu5_factor_output_bytes,
            (unsigned long)stats.lu5_factor_cells);
        if (!cfg.coeff_publish_lu && strcmp(mode,"modified-poison") && strcmp(mode,"different-nan") &&
            !validate_local_lu_poison(&p)) return 21;
        free_problem(&p);
        return poison_ok && coeff_ok ? 0 : 1;
    }
    Config cfg;
    parse_args(argc,argv,&cfg);
    printf("CLI_PUBLISH_LU=%d flag=%llu transition=%d\n",cfg.coeff_publish_lu,
        (unsigned long long)COEFF_PRE_LOCAL_LU,cfg.coeff_lu_output_transition_test);
    return 0;
}
''')
        subprocess.run(['cc','-O2',str(source),'-o',str(cls.binary),'-lm'],
                       check=True,capture_output=True,text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self,*arguments):
        return subprocess.run([str(self.binary),*arguments],capture_output=True,text=True)

    def private_arguments(self):
        return ['--lusgs-mode','step2-pretransform-raw-optprep-stream',
            '--coeff-preprocess-model','event','--lu5-model','event',
            '--coeff-streaming-enable','1','--coeff-stream-line-autonomous','1',
            '--coeff-stream-dinv-consumer','direct','--coeff-stream-lbar-consumer','direct',
            '--coeff-stream-ubar-consumer','direct','--coeff-stream-retire-mode','auto',
            '--coeff-stream-boundary-mask','1','--coeff-base-solve','direct',
            '--coeff-publish-lu','0']

    def test_default_publishes_and_private_flag_is_unused_bit25(self):
        result=self.run_case()
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('CLI_PUBLISH_LU=1 flag=33554432',result.stdout)

    def test_output_range_trace_tracks_calls_generations_policy_and_real_span(self):
        result=self.run_case('ranges')
        self.assertEqual(result.returncode,0,result.stderr)
        rows=[dict(field.split('=',1) for field in line.split()[1:])
              for line in result.stdout.splitlines() if line.startswith('CFD_LU_OUTPUT_RANGE ')]
        self.assertEqual(len(rows),2)
        self.assertEqual([row['round'] for row in rows],['1','2'])
        self.assertEqual([row['generation'] for row in rows],['1000','1003'])
        self.assertEqual([row['publish'] for row in rows],['0','1'])
        self.assertEqual(rows[0]['base'],rows[1]['base'])
        self.assertGreater(int(rows[0]['base'],0),0)
        self.assertTrue(all(row['bytes']=='400' and row['cells']=='2' for row in rows))

    def test_legal_direct_cli_accepts_private_policy_without_external_data(self):
        result=self.run_case(*self.private_arguments())
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('CLI_PUBLISH_LU=0',result.stdout)

    def test_transition_cli_requires_legal_direct_cached_scope(self):
        result=self.run_case(*self.private_arguments(),'--coeff-lu-output-transition-test','1')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('transition=1',result.stdout)
        for extra in (['--coeff-line-cache-allowed','0'],['--coeff-recovery-test','1']):
            result=self.run_case(*self.private_arguments(),'--coeff-lu-output-transition-test','1',*extra)
            self.assertEqual(result.returncode,2,result.stderr)

    def test_cli_rejects_unsupported_modes_and_values(self):
        for arguments in (['--coeff-publish-lu','0'],['--coeff-publish-lu','invalid'],
                          self.private_arguments()+['--lu-forwarding','0'],
                          self.private_arguments()+['--check','0'],
                          self.private_arguments()+['--coeff-base-solve','inverse'],
                          self.private_arguments()+['--coeff-stream-line-autonomous','0'],
                          self.private_arguments()+['--lusgs-lines','2']):
            with self.subTest(arguments=arguments):
                result=self.run_case(*arguments)
                self.assertEqual(result.returncode,2,result.stderr)

    def test_private_debug_reconstruction_preserves_poison_and_output_byte_count(self):
        result=self.run_case('validate-private')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('poison=1 coefficients=1 publication_bytes=0 factors=2',result.stdout)

    def test_public_validation_still_requires_and_counts_actual_output(self):
        result=self.run_case('validate-public')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('poison=1 coefficients=1 publication_bytes=400 factors=2',result.stdout)

    def test_modified_private_output_fails_including_changed_nan_payload(self):
        for mode in ('modified-poison','different-nan'):
            result=self.run_case('validate-'+mode)
            self.assertEqual(result.returncode,1,result.stderr)
            self.assertIn('CFD_LOCAL_LU_UNEXPECTED_WRITE index=17',result.stderr)

    def test_private_reconstruction_does_not_mask_bad_ubar_or_singular_D(self):
        for mode in ('wrong-ubar','singular'):
            result=self.run_case('validate-'+mode)
            self.assertEqual(result.returncode,1,result.stderr)
            self.assertIn('coefficients=0',result.stdout)


if __name__=='__main__':
    unittest.main()
