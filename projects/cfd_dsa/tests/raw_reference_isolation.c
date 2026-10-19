// Exercise the actual software oracle without the simulated custom ISA.
#define main step2_benchmark_main
#include "../benchmarks/lusgs/test_cfd_lusgs_patha_step2.c"
#undef main

int main(void)
{
    Problem p;
    init_problem(&p, 1, 17);
    for (size_t i = 0; i < 17*N*N; ++i) {
        p.lu_d[i] = 123.0;
        p.u_bar[i] = -456.0;
    }
    Stats stats = {};
    if (!raw_reference_solve(&p, &stats)) return 1;
    for (size_t i = 0; i < 17*N*N; ++i)
        if (p.lu_d[i] != 123.0 || p.u_bar[i] != -456.0) return 2;
    const double residual=raw_recurrence_residual(&p,p.dq_star_raw_ref,p.dq_raw_ref);
    if (!isfinite(residual) || residual>1e-10)
        return 3;
    // The early-error path must also leave both hardware output buffers alone.
    for (size_t i = 0; i < N; ++i) p.d_mat[i] = 0;
    if (raw_reference_solve(&p, &stats)) return 4;
    for (size_t i = 0; i < 17*N*N; ++i)
        if (p.lu_d[i] != 123.0 || p.u_bar[i] != -456.0) return 5;
    free_problem(&p);
    puts("RAW_REFERENCE_ISOLATION_PASS");
    return 0;
}
