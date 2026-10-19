static CompareResult
compare_solution(const Problem *p, const double *ref_base,
                 const double *got_base, double abs_tol, double rel_tol)
{
    CompareResult cmp;
    memset(&cmp, 0, sizeof(cmp));
    cmp.first_line = -1;
    cmp.first_cell = -1;
    cmp.first_lane = -1;

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            const double *ref = vec_const_at(ref_base, p, line, cell);
            const double *got = vec_const_at(got_base, p, line, cell);
            for (int lane = 0; lane < N; lane++) {
                double abs_err = fabs(got[lane] - ref[lane]);
                double denom = fmax(fabs(ref[lane]), 1.0e-14);
                double rel_err = abs_err / denom;
                if (abs_err > cmp.max_abs_error)
                    cmp.max_abs_error = abs_err;
                if (rel_err > cmp.max_rel_error)
                    cmp.max_rel_error = rel_err;
                if (!isfinite(ref[lane]) || !isfinite(got[lane]) ||
                    (abs_err > abs_tol && rel_err > rel_tol)) {
                    if (cmp.mismatch_count == 0) {
                        cmp.first_line = (long)line;
                        cmp.first_cell = (long)cell;
                        cmp.first_lane = (long)lane;
                        cmp.first_ref = ref[lane];
                        cmp.first_got = got[lane];
                    }
                    cmp.mismatch_count++;
                }
            }
        }
    }
    return cmp;
}

