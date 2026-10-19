/* Audit-only guest: use the existing solver and ABI; vary only the
 * pivot acceptance threshold and dirty bit between single-line launches. */
#define main original_benchmark_main
#include "/home/zyy/gem5/projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c"
#undef main
int main(int argc, char **argv)
{
    Config cfg;
    parse_args(argc, argv, &cfg);
    cfg.lines = cfg.interleave = cfg.cells = 1;
    cfg.sweeps = cfg.coefficient_update_interval = 2;
    cfg.coeff_base_solve = "direct";
    Problem p;
    init_problem(&p, 1, 1);
    Stats reference = {};
    if (!raw_reference_solve(&p, &reference)) return 2;
    int valid[3] = {};
    for (unsigned step = 0; step < 3; ++step) {
        Stats stats = {};
        PretransformCheck check = {};
        cfg.pretransform_diag_epsilon = step == 0 ? 10.0 : 1.0e-12;
        valid[step] = run_pretransform_raw_stream_line_once(&p, &cfg, &stats,
            p.dq_star_pretransform_raw_stream, p.dq_pretransform_raw_stream,
            &check, 1, step != 1);
        printf("AUDIT_FAILURE_RECOVERY step=%u dirty=%u threshold=%.17g valid=%d\n",
            step, step != 1, cfg.pretransform_diag_epsilon, valid[step]);
        fflush(stdout);
    }
    printf("AUDIT_FAILURE_RECOVERY expected=0,1,1 actual=%d,%d,%d\n", valid[0], valid[1], valid[2]);
    free_problem(&p);
    if (!valid[0] && valid[1] && valid[2]) {
        puts("LUSGS_STEP2_STREAMING_PASS");
        return 0;
    }
    puts("LUSGS_STEP2_STREAMING_FAIL");
    return 1;
}
