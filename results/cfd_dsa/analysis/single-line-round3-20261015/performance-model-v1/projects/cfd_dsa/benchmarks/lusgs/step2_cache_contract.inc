// Explicit cache contract checks. Every invocation is one forward/backward
// sweep on one line. R changes on every hit; coefficients change only with
// a new version or an explicit dirty publication.
static int
run_cache_contract_test(const Config *original)
{
    const char *modes[] = {"direct", "direct", "inverse", "inverse", "direct",
                           "direct", "direct", "direct", "direct", "direct"};
    const size_t cells[] = {3, 3, 3, 3, 3, 2, 2, 2, 2, 2};
    const uint64_t versions[] = {1, 1, 1, 1, 1, 1, 1, 2, 2, 2};
    const int dirty[] = {1, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    int valid = 1;
    for (unsigned step = 0; step < 10; ++step) {
        Config cfg = *original;
        cfg.lines = cfg.interleave = 1;
        cfg.cells = cells[step];
        cfg.sweeps = 2; // Enables the version cache; run_once is one sweep.
        cfg.coefficient_update_interval = 2;
        cfg.coeff_base_solve = modes[step];
        Problem p;
        init_problem(&p, 1, cells[step]);
        for (size_t cell = 0; cell < cells[step]; ++cell) {
            for (unsigned lane = 0; lane < N; ++lane) {
                vec_at(p.rhs, &p, 0, cell)[lane] += 0.125 * (step + 1);
                if (step >= 7)
                    mat_at(p.d_mat, &p, 0, cell)[lane * N + lane] += 0.2;
                if (step >= 8)
                    mat_at(p.d_mat, &p, 0, cell)[lane * N + lane] += 0.1;
            }
        }
        Stats reference = {}, stats = {};
        PretransformCheck check = {};
        int ok = raw_reference_solve(&p, &reference);
        if (ok)
            ok = run_pretransform_raw_stream_line_once(&p, &cfg, &stats,
                p.dq_star_pretransform_raw_stream, p.dq_pretransform_raw_stream,
                &check, versions[step], dirty[step]);
        printf("CFD_CACHE_CONTRACT step=%u mode=%s cells=%lu version=%lu "
               "dirty=%d numeric_pass=%d\n", step, modes[step],
               (unsigned long)cells[step], (unsigned long)versions[step],
               dirty[step], ok);
        valid &= ok;
        free_problem(&p);
        if (!ok)
            break;
    }
    printf("%s\n", valid ? "LUSGS_CACHE_CONTRACT_PASS" :
                          "LUSGS_CACHE_CONTRACT_FAIL");
    return valid;
}
