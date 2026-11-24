// Exercise the real controller's error and cancellation paths, then reuse
// the same line/version with changed R. Every invocation is one sweep.
static int
run_recovery_contract_test(const Config *original)
{
    Config cfg = *original;
    cfg.lines = cfg.interleave = 1;
    cfg.sweeps = cfg.coefficient_update_interval = 2;
    cfg.coeff_poison_outputs = 1;
    Problem p;
    init_problem(&p,1,cfg.cells);
    int valid = 1;
    const unsigned steps = cfg.cells >= 17 ? 6 : 4;
    const uint64_t late_polls = cfg.coeff_recovery_late_polls ?
        cfg.coeff_recovery_late_polls : cfg.coeff_recovery_warm ? 64 : 128;
    for (unsigned step = 0; step < steps; ++step) {
        // A cancelled rebuild must not revive the preceding coefficient
        // version, even if the guest retries with the same version number.
        if (step == 4 && !cfg.coeff_recovery_warm)
            for (size_t cell = 0; cell < cfg.cells; ++cell)
                for (unsigned lane = 0; lane < N; ++lane)
                    mat_at(p.d_mat,&p,0,cell)[lane*N+lane] += 0.1;
        cfg.pretransform_diag_epsilon = step == 0 ? 10.0 : 1e-12;
        cfg.coeff_cancel_after_polls = step == 2 ? 1 : step == 4 ? late_polls : 0;
        for (size_t cell = 0; cell < cfg.cells; ++cell)
            for (unsigned lane = 0; lane < N; ++lane)
                vec_at(p.rhs,&p,0,cell)[lane] += 0.0625;
        Stats reference = {}, stats = {};
        PretransformCheck check = {};
        int ok = raw_reference_solve(&p,&reference);
        if (ok)
            ok = run_pretransform_raw_stream_line_once(&p,&cfg,&stats,
                p.dq_star_pretransform_raw_stream,p.dq_pretransform_raw_stream,
                &check,1,step == 0 || (step == 4 && !cfg.coeff_recovery_warm));
        const int wanted = step != 0 && step != 2 && step != 4;
        printf("CFD_RECOVERY_CONTRACT step=%u cells=%lu mode=%s expected=%d actual=%d\n",
            step,(unsigned long)cfg.cells,cfg.coeff_base_solve,wanted,ok);
        valid &= ok == wanted;
        fflush(stdout);
        if (!valid) break;
    }
    free_problem(&p);
    puts(valid ? "LUSGS_RECOVERY_CONTRACT_PASS" : "LUSGS_RECOVERY_CONTRACT_FAIL");
    return valid;
}
