static uint64_t
validation_random(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static void
apply_raw_input_pattern(Problem *p, const Config *cfg)
{
    if (!strcmp(cfg->coeff_input_pattern,"baseline")) return;
    uint64_t state = cfg->coeff_input_seed;
    for (size_t line = 0; line < p->lines; ++line) {
        for (size_t cell = 0; cell < p->cells; ++cell) {
            double *d = mat_at(p->d_mat,p,line,cell);
            double *l = mat_at(p->l_mat,p,line,cell);
            double *u = mat_at(p->u_mat,p,line,cell);
            double *r = vec_at(p->rhs,p,line,cell);
            for (unsigned row = 0; row < N; ++row) {
                double off = 0;
                for (unsigned col = 0; col < N; ++col) {
                    const unsigned k = row*N+col;
                    d[k] = ((double)(validation_random(&state)%2001)-1000)/5000;
                    l[k] = ((double)(validation_random(&state)%2001)-1000)/10000;
                    u[k] = ((double)(validation_random(&state)%2001)-1000)/10000;
                    if (row != col) off += fabs(d[k]);
                }
                d[row*N+row] = 3+off;
                r[row] = ((double)(validation_random(&state)%2001)-1000)/1000;
                if (!strcmp(cfg->coeff_input_pattern,"scaled")) {
                    const double scale = row%2 ? 1e6 : 1e-6;
                    for (unsigned col = 0; col < N; ++col) {
                        d[row*N+col] *= scale;
                        l[row*N+col] *= scale;
                        u[row*N+col] *= scale;
                    }
                    r[row] *= scale;
                }
            }
        }
    }
}

// Check the raw recurrence equations without calling the reference LU/TRSM.
static double
raw_recurrence_residual(const Problem *p, const double *y, const double *dq)
{
    double maximum = 0;
    for (size_t line = 0; line < p->lines; ++line) {
        for (size_t cell = 0; cell < p->cells; ++cell) {
            const double *d = mat_const_at(p->d_mat,p,line,cell);
            const double *l = mat_const_at(p->l_mat,p,line,cell);
            const double *u = mat_const_at(p->u_mat,p,line,cell);
            const double *r = vec_const_at(p->rhs,p,line,cell);
            const double *current_y = vec_const_at(y,p,line,cell);
            const double *current_dq = vec_const_at(dq,p,line,cell);
            for (unsigned row = 0; row < N; ++row) {
                double f = -r[row], b = 0, fs = fabs(r[row]), bs = 0;
                for (unsigned col = 0; col < N; ++col) {
                    double a = d[row*N+col]*current_y[col];
                    f += a; fs += fabs(a);
                    a = d[row*N+col]*(current_dq[col]-current_y[col]);
                    b += a;
                    bs += fabs(d[row*N+col])*(fabs(current_dq[col])+fabs(current_y[col]));
                    if (cell) {
                        a = l[row*N+col]*vec_const_at(y,p,line,cell-1)[col];
                        f += a; fs += fabs(a);
                    }
                    if (cell+1 < p->cells) {
                        a = u[row*N+col]*vec_const_at(dq,p,line,cell+1)[col];
                        b += a; bs += fabs(a);
                    }
                }
                if (!isfinite(f) || !isfinite(b) || !isfinite(fs) || !isfinite(bs)) return INFINITY;
                maximum = fmax(maximum,fabs(f)/fmax(fs,1e-300));
                maximum = fmax(maximum,fabs(b)/fmax(bs,1e-300));
            }
        }
    }
    return maximum;
}
