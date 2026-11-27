// CFD5IN01: 48-byte little-endian header, then 90 FP64 values per cell:
// D[25], L[25], U[25], R[5], Decimal-oracle y[5], dq[5]. Loading and
// verification execute outside the timed accelerator calls.
static uint64_t
dataset_le(const unsigned char *bytes, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= (uint64_t)bytes[i] << (8*i);
    return value;
}

static void
dataset_error(FILE *file, const char *reason)
{
    if (file) fclose(file);
    fprintf(stderr, "CFD_DATASET_ERROR %s\n", reason);
    exit(2);
}

static void
load_raw_dataset(Problem *p, const Config *cfg, double *input_records)
{
    if (!cfg->coeff_input_file) return;
    FILE *file = fopen(cfg->coeff_input_file, "rb");
    if (!file) dataset_error(NULL, "cannot open input file");
    unsigned char header[48];
    if (fread(header, 1, sizeof(header), file) != sizeof(header))
        dataset_error(file, "truncated header");
    if (memcmp(header, "CFD5IN01", 8) || dataset_le(header+8,4) != 1 ||
        dataset_le(header+12,4) != N || dataset_le(header+16,8) != 1 ||
        p->lines != 1 || dataset_le(header+24,8) != p->cells ||
        p->cells > UINT64_MAX/720 ||
        dataset_le(header+32,8) != p->cells*720)
        dataset_error(file, "invalid format or configured dimensions");
    p->dataset_oracle_y = xcalloc(p->cells*N, sizeof(double), "dataset_oracle_y");
    p->dataset_oracle_dq = xcalloc(p->cells*N, sizeof(double), "dataset_oracle_dq");
    enum { InputWords = 3 * N * N + N };
    uint64_t checksum = 14695981039346656037ULL;
    for (size_t cell = 0; cell < p->cells; ++cell) {
        unsigned char record[720];
        double unpacked[InputWords], oracle[2 * N];
        double *values = input_records ? input_records + cell * InputWords : unpacked;
        if (fread(record, 1, sizeof(record), file) != sizeof(record))
            dataset_error(file, "truncated cell record");
        for (unsigned i = 0; i < sizeof(record); ++i)
            checksum = (checksum ^ record[i])*1099511628211ULL;
        for (unsigned i = 0; i < InputWords + 2 * N; ++i) {
            const uint64_t bits = dataset_le(record+8*i, 8);
            double value;
            memcpy(&value, &bits, sizeof(bits));
            if (!isfinite(value)) dataset_error(file, "nonfinite value");
            if (i < InputWords)
                values[i] = value;
            else
                oracle[i - InputWords] = value;
        }
        if (!cell)
            for (unsigned i = 25; i < 50; ++i)
                if (values[i] != 0) dataset_error(file, "nonzero first L");
        if (cell+1 == p->cells)
            for (unsigned i = 50; i < 75; ++i)
                if (values[i] != 0) dataset_error(file, "nonzero last U");
        memcpy(mat_at(p->d_mat,p,0,cell), values, 25*sizeof(double));
        memcpy(mat_at(p->l_mat,p,0,cell), values+25, 25*sizeof(double));
        memcpy(mat_at(p->u_mat,p,0,cell), values+50, 25*sizeof(double));
        memcpy(vec_at(p->rhs,p,0,cell), values+75, N*sizeof(double));
        memcpy(vec_at(p->dataset_oracle_y,p,0,cell), oracle, N*sizeof(double));
        memcpy(vec_at(p->dataset_oracle_dq,p,0,cell), oracle+N, N*sizeof(double));
    }
    if (checksum != dataset_le(header+40,8)) dataset_error(file, "checksum mismatch");
    if (fgetc(file) != EOF || ferror(file)) dataset_error(file, "trailing data or read error");
    fclose(file);
    printf("CFD_DATASET_INPUT format=CFD5IN01 lines=1 cells=%lu fnv64=%016lx payload_bytes=%lu\n",
        (unsigned long)p->cells, (unsigned long)checksum, (unsigned long)(p->cells*720));
}

static inline uint64_t
cfd_output_word_digest(uint32_t phase, uint64_t cell, uint32_t lane,
                       uint64_t bits)
{
    uint64_t x = bits ^ (((uint64_t)phase + 1ULL) * 0x9e3779b97f4a7c15ULL) ^
                 ((cell + 1ULL) * 0xbf58476d1ce4e5b9ULL) ^
                 (((uint64_t)lane + 1ULL) * 0x94d049bb133111ebULL);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static double
dataset_block_cond_inf(const double d[N * N])
{
    double a[N][2 * N];
    double d_norm = 0.0;
    for (unsigned r = 0; r < N; ++r) {
        double row_sum = 0.0;
        for (unsigned c = 0; c < N; ++c) {
            const double val = d[r * N + c];
            a[r][c] = val;
            a[r][N + c] = (r == c) ? 1.0 : 0.0;
            row_sum += fabs(val);
        }
        if (row_sum > d_norm) d_norm = row_sum;
    }
    for (unsigned k = 0; k < N; ++k) {
        unsigned piv = k;
        double max_piv = fabs(a[k][k]);
        for (unsigned r = k + 1; r < N; ++r) {
            if (fabs(a[r][k]) > max_piv) {
                max_piv = fabs(a[r][k]);
                piv = r;
            }
        }
        if (!(max_piv > 0.0) || !isfinite(max_piv)) return INFINITY;
        if (piv != k) {
            for (unsigned c = 0; c < 2 * N; ++c) {
                double tmp = a[k][c];
                a[k][c] = a[piv][c];
                a[piv][c] = tmp;
            }
        }
        const double diag = a[k][k];
        for (unsigned c = 0; c < 2 * N; ++c)
            a[k][c] /= diag;
        for (unsigned r = 0; r < N; ++r) {
            if (r == k) continue;
            const double factor = a[r][k];
            for (unsigned c = 0; c < 2 * N; ++c)
                a[r][c] -= factor * a[k][c];
        }
    }
    double inv_norm = 0.0;
    for (unsigned r = 0; r < N; ++r) {
        double row_sum = 0.0;
        for (unsigned c = 0; c < N; ++c)
            row_sum += fabs(a[r][N + c]);
        if (row_sum > inv_norm) inv_norm = row_sum;
    }
    return d_norm * inv_norm;
}

static double
dataset_max_block_cond(const Problem *p)
{
    double kappa_max = 1.0;
    for (size_t cell = 0; cell < p->cells; ++cell) {
        const double k = dataset_block_cond_inf(p->d_mat + cell * N * N);
        if (!isfinite(k)) return INFINITY;
        if (k > kappa_max) kappa_max = k;
    }
    return kappa_max;
}

static double
dataset_forward_tolerance(const Problem *p)
{
    if (!p->dataset_oracle_y) return 1.0e-10;
    const double kappa_max = dataset_max_block_cond(p);
    if (!isfinite(kappa_max)) return 1.0e-10;
    const double u_mach = 1.1102230246251565e-16; // 2^-53
    return fmax(1.0e-10, 64.0 * u_mach * kappa_max);
}

static double
dataset_recurrence_residual(const Problem *p, const double *y, const double *dq)
{
    double maximum = 0.0;
    for (size_t line = 0; line < p->lines; ++line) {
        for (size_t cell = 0; cell < p->cells; ++cell) {
            const size_t base = (line * p->cells + cell);
            const double *d = p->d_mat + base * N * N;
            const double *l = p->l_mat + base * N * N;
            const double *u = p->u_mat + base * N * N;
            const double *r = p->rhs + base * N;
            const double *cy = y + base * N;
            const double *cdq = dq + base * N;
            for (unsigned row = 0; row < N; ++row) {
                double f = -r[row], b = 0.0, fs = fabs(r[row]), bs = 0.0;
                for (unsigned col = 0; col < N; ++col) {
                    double a = d[row * N + col] * cy[col];
                    f += a; fs += fabs(a);
                    a = d[row * N + col] * (cdq[col] - cy[col]);
                    b += a;
                    bs += fabs(d[row * N + col]) * (fabs(cdq[col]) + fabs(cy[col]));
                    if (cell) {
                        a = l[row * N + col] * (y + (base - 1) * N)[col];
                        f += a; fs += fabs(a);
                    }
                    if (cell + 1 < p->cells) {
                        a = u[row * N + col] * (dq + (base + 1) * N)[col];
                        b += a; bs += fabs(a);
                    }
                }
                if (!isfinite(f) || !isfinite(b) || !isfinite(fs) || !isfinite(bs))
                    return INFINITY;
                maximum = fmax(maximum, fabs(f) / fmax(fs, 1.0e-300));
                maximum = fmax(maximum, fabs(b) / fmax(bs, 1.0e-300));
            }
        }
    }
    return maximum;
}

static int
validate_dataset_oracle(const Problem *p, const double *y, const double *dq,
                        const char *phase)
{
    if (!p->dataset_oracle_y) return 1;
    const double kappa_max = dataset_max_block_cond(p);
    const double tol = dataset_forward_tolerance(p);
    const double residual = dataset_recurrence_residual(p, y, dq);
    const double residual_limit = (tol > 1.0e-10) ? 1.0e-12 : 1.0e-10;
    uint64_t strict_mismatches = 0;
    uint64_t mismatches = 0;
    double max_abs = 0, max_rel = 0;
    const double *got[] = {y, dq};
    const double *expected[] = {p->dataset_oracle_y, p->dataset_oracle_dq};
    for (unsigned part = 0; part < 2; ++part) {
        for (size_t i = 0; i < p->cells*N; ++i) {
            const double error = fabs(got[part][i]-expected[part][i]);
            const double relative = error/fmax(fabs(expected[part][i]),1e-14);
            max_abs = fmax(max_abs,error);
            max_rel = fmax(max_rel,relative);
            if (!isfinite(got[part][i]) || (error > 1e-10 && relative > 1e-10))
                ++strict_mismatches;
            if (!isfinite(got[part][i]) || (error > tol && relative > tol))
                ++mismatches;
        }
    }
    if (!isfinite(residual) || residual > residual_limit)
        ++mismatches;
    printf("CFD_DATASET_NUMERIC phase=%s values=%lu max_abs=%.17g max_rel=%.17g "
           "residual_max=%.17g kappa_max=%.17g tol=%.17g strict_1e10_exceeded=%lu "
           "mismatches=%lu\n",
        phase, (unsigned long)(p->cells*10), max_abs, max_rel,
        residual, kappa_max, tol,
        (unsigned long)strict_mismatches, (unsigned long)mismatches);
    return mismatches == 0;
}
