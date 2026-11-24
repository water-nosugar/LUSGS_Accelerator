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

static int
validate_dataset_oracle(const Problem *p, const double *y, const double *dq,
                        const char *phase)
{
    if (!p->dataset_oracle_y) return 1;
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
                ++mismatches;
        }
    }
    printf("CFD_DATASET_NUMERIC phase=%s values=%lu max_abs=%.17g max_rel=%.17g mismatches=%lu\n",
        phase, (unsigned long)(p->cells*10), max_abs, max_rel, (unsigned long)mismatches);
    return mismatches == 0;
}
