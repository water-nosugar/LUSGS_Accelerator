/* Architectural SME reference for the pretransform and raw CFD5 guest. */
extern size_t sme_stream_bytes(void);
extern void sme_rank1(double *dst, size_t stride_bytes, const double *left,
                      const double *right, size_t rows, size_t cols, int subtract);

static void gather_column(const double *a, unsigned col, unsigned first,
                          unsigned count, double *v)
{
    for (unsigned i = 0; i < count; ++i)
        v[i] = a[(first + i) * N + col];
}

static int factor(const double *d, double *lu, double *reciprocal)
{
    memcpy(lu, d, N * N * sizeof(double));
    for (unsigned k = 0; k < N; ++k) {
        double *pivot = lu + k * N;
        if (!isfinite(pivot[k]) || pivot[k] == 0)
            return 0;
        reciprocal[k] = 1.0 / pivot[k];
        const unsigned tail = N - 1 - k;
        if (tail) {
            double column[N];
            for (unsigned j = k + 1; j < N; ++j)
                pivot[j] *= reciprocal[k];
            gather_column(lu, k, k + 1, tail, column);
            sme_rank1(pivot + N + k + 1, N * sizeof(double), column,
                      pivot + k + 1, tail, tail, 1);
        }
    }
    return 1;
}

static void solve_panel(const double *lu, const double *reciprocal,
                        double *rhs, unsigned columns)
{
    double factors[N];
    size_t stride = columns * sizeof(double);
    for (unsigned k = 0; k < N; ++k) {
        double *pivot = rhs + k * columns;
        for (unsigned j = 0; j < columns; ++j)
            pivot[j] *= reciprocal[k];
        const unsigned tail = N - 1 - k;
        if (tail) {
            gather_column(lu, k, k + 1, tail, factors);
            sme_rank1(pivot + columns, stride, factors, pivot, tail, columns, 1);
        }
    }
    for (unsigned k = N - 1; k > 0; --k) {
        gather_column(lu, k, 0, k, factors);
        sme_rank1(rhs, stride, factors, rhs + k * columns, k, columns, 1);
    }
}

static void solve(const double *lu, const double *reciprocal, double *rhs)
{
    solve_panel(lu, reciprocal, rhs, 1);
}

static void transform(const double *lu, const double *reciprocal,
                      const double *matrix, double *out)
{
    memcpy(out, matrix, N * N * sizeof(double));
    solve_panel(lu, reciprocal, out, N);
}

static void product(const double *matrix, const double *vector, double *out)
{
    double column[N];
    memset(out, 0, N * sizeof(double));
    for (unsigned j = 0; j < N; ++j) {
        gather_column(matrix, j, 0, N, column);
        sme_rank1(out, sizeof(double), column, vector + j, N, 1, 0);
    }
}
