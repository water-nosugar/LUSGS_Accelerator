/* Scalar C CPU baselines for the same CFD5IN01 single-line SGS problem.
 * All allocation/input/oracle checking/cache conditioning lies outside ROI.
 * The compiler may vectorize; build flags retain strict FP association.
 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 5
#define EVICTION_BYTES (16U*1024U*1024U)
typedef struct {
    size_t lines, cells;
    double *d_mat, *l_mat, *u_mat, *rhs;
    double *dataset_oracle_y, *dataset_oracle_dq;
} Problem;
typedef struct { const char *coeff_input_file; } Config;

static void *xcalloc(size_t count, size_t size, const char *label)
{
    if (size && count > SIZE_MAX/size) {
        fprintf(stderr, "CPU_STUDY_ERROR allocation overflow: %s\n", label);
        exit(2);
    }
    void *p = calloc(count, size);
    if (!p) { fprintf(stderr, "CPU_STUDY_ERROR allocation: %s\n", label); exit(2); }
    return p;
}
static double *mat_at(double *base, const Problem *p, size_t line, size_t cell)
{ return base+(line*p->cells+cell)*N*N; }
static double *vec_at(double *base, const Problem *p, size_t line, size_t cell)
{ return base+(line*p->cells+cell)*N; }
#include "step2_dataset.inc"

/* Same 2GHz cycle counter convention as the accelerator benchmark. */
static inline uint64_t read_cycles(void)
{
#if defined(__aarch64__)
    register uint64_t x0 __asm__("x0") = 0;
    __asm__ __volatile__("dsb sy\n\tisb\n\t.long 0xff070110\n\tisb"
                         : "+r"(x0) :: "memory");
    return x0*2U;
#else
    __asm__ __volatile__("" ::: "memory");
    return 0; /* Native tests validate arithmetic, never report CPU performance. */
#endif
}

#ifdef CFD_ARCH_SME_REFERENCE
#include "sme_reference_kernels.inc"
#else
static int factor(const double *d, double *lu, double *reciprocal)
{
    memcpy(lu, d, N*N*sizeof(double));
    for (unsigned k = 0; k < N; ++k) {
        if (!isfinite(lu[k*N+k]) || lu[k*N+k] == 0) return 0;
        reciprocal[k] = 1.0/lu[k*N+k];
        for (unsigned row = k+1; row < N; ++row) {
            lu[row*N+k] *= reciprocal[k];
            for (unsigned col = k+1; col < N; ++col)
                lu[row*N+col] -= lu[row*N+k]*lu[k*N+col];
        }
    }
    return 1;
}

static void solve(const double *lu, const double *reciprocal, double *x)
{
    for (unsigned row = 0; row < N; ++row)
        for (unsigned col = 0; col < row; ++col)
            x[row] -= lu[row*N+col]*x[col];
    for (int row = N-1; row >= 0; --row) {
        for (unsigned col = row+1; col < N; ++col)
            x[row] -= lu[row*N+col]*x[col];
        x[row] *= reciprocal[row];
    }
}

static void product(const double *matrix, const double *vector, double *out)
{
    for (unsigned row = 0; row < N; ++row) {
        double value = 0;
        for (unsigned col = 0; col < N; ++col)
            value += matrix[row*N+col]*vector[col];
        out[row] = value;
    }
}

static void transform(const double *lu, const double *reciprocal,
                      const double *matrix, double *out)
{
    for (unsigned col = 0; col < N; ++col) {
        double x[N];
        for (unsigned row = 0; row < N; ++row) x[row] = matrix[row*N+col];
        solve(lu, reciprocal, x);
        for (unsigned row = 0; row < N; ++row) out[row*N+col] = x[row];
    }
}
#endif

#ifdef CFD_OPT_SVE_KERNEL
extern int sve_opt_preprocess_line(
    size_t cells, int transformed,
    const double *d_mat, const double *l_mat, const double *u_mat,
    double *lu_col, double *recip, double *lbar_col, double *ubar_col);
extern void sve_opt_sweep_pretransform_line(
    size_t cells, const double *lu_col, const double *recip,
    const double *lbar_col, const double *ubar_col,
    const double *rhs, double *y, double *dq);
#elif defined(CFD_OPT_SME_KERNEL)
extern int sme_opt_preprocess_line(
    size_t cells, int transformed,
    const double *d_mat, const double *l_mat, const double *u_mat,
    double *lu_col, double *recip, double *lbar_col, double *ubar_col);
extern void sme_opt_sweep_pretransform_line(
    size_t cells, const double *lu_col, const double *recip,
    const double *lbar_col, const double *ubar_col,
    const double *rhs, double *y, double *dq);
#endif

static int preprocess(const Problem *p, int transformed, double *lu,
                       double *reciprocal, double *lbar, double *ubar)
{
#ifdef CFD_OPT_SVE_KERNEL
    return sve_opt_preprocess_line(p->cells, transformed, p->d_mat, p->l_mat,
                                   p->u_mat, lu, reciprocal, lbar, ubar);
#elif defined(CFD_OPT_SME_KERNEL)
    return sme_opt_preprocess_line(p->cells, transformed, p->d_mat, p->l_mat,
                                   p->u_mat, lu, reciprocal, lbar, ubar);
#else
    for (size_t i = 0; i < p->cells; ++i) {
        if (!factor(p->d_mat+i*25, lu+i*25, reciprocal+i*5)) return 0;
        if (!transformed) continue;
        if (i) transform(lu+i*25, reciprocal+i*5, p->l_mat+i*25, lbar+i*25);
        if (i+1 < p->cells)
            transform(lu+i*25, reciprocal+i*5, p->u_mat+i*25, ubar+i*25);
    }
    return 1;
#endif
}

static void sweep(const Problem *p, int transformed, const double *lu,
                  const double *reciprocal, const double *lbar,
                  const double *ubar, double *y, double *dq)
{
#ifdef CFD_OPT_SVE_KERNEL
    if (transformed) {
        sve_opt_sweep_pretransform_line(p->cells, lu, reciprocal, lbar, ubar,
                                        p->rhs, y, dq);
        return;
    }
#elif defined(CFD_OPT_SME_KERNEL)
    if (transformed) {
        sme_opt_sweep_pretransform_line(p->cells, lu, reciprocal, lbar, ubar,
                                        p->rhs, y, dq);
        return;
    }
#endif
    for (size_t i = 0; i < p->cells; ++i) {
        double x[N], correction[N];
        memcpy(x, p->rhs+i*5, sizeof(x));
        if (transformed) solve(lu+i*25, reciprocal+i*5, x);
        if (i) {
            product((transformed ? lbar : p->l_mat)+i*25, y+(i-1)*5, correction);
            for (unsigned row = 0; row < N; ++row) x[row] -= correction[row];
        }
        if (!transformed) solve(lu+i*25, reciprocal+i*5, x);
        memcpy(y+i*5, x, sizeof(x));
    }
    memcpy(dq+(p->cells-1)*5, y+(p->cells-1)*5, N*sizeof(double));
    for (size_t cursor = p->cells-1; cursor > 0; --cursor) {
        const size_t i = cursor-1;
        double correction[N];
        product((transformed ? ubar : p->u_mat)+i*25, dq+(i+1)*5, correction);
        if (!transformed) solve(lu+i*25, reciprocal+i*5, correction);
        for (unsigned row = 0; row < N; ++row)
            dq[i*5+row] = y[i*5+row]-correction[row];
    }
}

static volatile double checksum_sink;
static volatile uint64_t eviction_sink;
static void condition_cache(const Problem *p, volatile uint64_t *eviction,
                            uint64_t round)
{
    uint64_t checksum = UINT64_C(14695981039346656037);
    if (eviction) {
        for (size_t i = 0; i < EVICTION_BYTES/sizeof(uint64_t); i += 8)
            checksum = (checksum ^ eviction[i])*UINT64_C(1099511628211);
    } else {
        const double *arrays[] = {p->d_mat, p->l_mat, p->u_mat, p->rhs};
        const size_t lengths[] = {p->cells*25, p->cells*25, p->cells*25, p->cells*5};
        for (unsigned a = 0; a < 4; ++a) {
            const volatile double *values = arrays[a];
            for (size_t i = 0; i < lengths[a]; i += 8) {
                const double value = values[i];
                uint64_t bits;
                memcpy(&bits, &value, sizeof(bits));
                checksum = (checksum ^ bits)*UINT64_C(1099511628211);
            }
            if ((lengths[a]-1)%8) {
                const double value = values[lengths[a]-1];
                uint64_t bits;
                memcpy(&bits, &value, sizeof(bits));
                checksum = (checksum ^ bits)*UINT64_C(1099511628211);
            }
        }
    }
    eviction_sink = checksum;
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
#endif
    printf("CPU_STUDY_CONDITION round=%" PRIu64 " cache=%s span_bytes=%zu"
           " checksum=%" PRIu64
           " label=cache-conditioning-not-guaranteed-cold\n", round,
           eviction ? "evicted" : "prepared",
           eviction ? (size_t)EVICTION_BYTES : p->cells*80*sizeof(double),
           checksum);
}

static uint64_t parse_positive(const char *value)
{
    char *end;
    errno = 0;
    const unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || !value[0] || value[0] == '-' || *end || !parsed) {
        fprintf(stderr, "CPU_STUDY_ERROR expected positive integer: %s\n", value);
        exit(2);
    }
    return parsed;
}

int main(int argc, char **argv)
{
    size_t cells = 0;
    uint64_t rounds = 1;
    const char *mode = "raw", *cache = "prepared";
    Config cfg = {0};
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        if (i+1 >= argc) { fprintf(stderr, "CPU_STUDY_ERROR missing value\n"); return 2; }
        const char *value = argv[++i];
        if (!strcmp(option, "--coeff-input-file")) cfg.coeff_input_file = value;
        else if (!strcmp(option, "--lusgs-cells")) cells = parse_positive(value);
        else if (!strcmp(option, "--bench-iters")) rounds = parse_positive(value);
        else if (!strcmp(option, "--cpu-study-mode")) mode = value;
        else if (!strcmp(option, "--cpu-study-cache")) cache = value;
        else { fprintf(stderr, "CPU_STUDY_ERROR unknown option: %s\n", option); return 2; }
    }
    if (!cfg.coeff_input_file || !cells || cells > SIZE_MAX/(25*sizeof(double)) ||
        (strcmp(mode,"raw") && strcmp(mode,"pretransform")) ||
        (strcmp(cache,"prepared") && strcmp(cache,"evicted"))) {
        fprintf(stderr, "CPU_STUDY_ERROR need input, cells, valid mode and cache policy\n");
        return 2;
    }
    Problem p = {.lines=1, .cells=cells};
#ifdef CFD_ARCH_SME_REFERENCE
    if (sme_stream_bytes() < N * sizeof(double)) {
        fprintf(stderr, "SME reference needs at least five streaming FP64 lanes\n");
        return 2;
    }
    puts("SME_LINE_ISA_REFERENCE fp64_outer_products=architectural_ZA");
#endif
    p.d_mat = xcalloc(cells, 25*sizeof(double), "D");
    p.l_mat = xcalloc(cells, 25*sizeof(double), "L");
    p.u_mat = xcalloc(cells, 25*sizeof(double), "U");
    p.rhs = xcalloc(cells, 5*sizeof(double), "R");
    double *lu = xcalloc(cells, 25*sizeof(double), "LU");
    double *reciprocal = xcalloc(cells, 5*sizeof(double), "reciprocal");
    double *lbar = xcalloc(cells, 25*sizeof(double), "Lbar");
    double *ubar = xcalloc(cells, 25*sizeof(double), "Ubar");
    double *y = xcalloc(cells, 5*sizeof(double), "y");
    double *dq = xcalloc(cells, 5*sizeof(double), "dq");
    volatile uint64_t *eviction = !strcmp(cache,"evicted") ?
        xcalloc(EVICTION_BYTES, 1, "eviction") : NULL;
    load_raw_dataset(&p, &cfg, NULL); /* Reads data/oracle without solving on CPU first. */
    if (eviction)
        for (size_t i = 0; i < EVICTION_BYTES/sizeof(uint64_t); ++i)
            eviction[i] = i ^ UINT64_C(0x9e3779b97f4a7c15);
#if defined(__aarch64__)
    const char *timing = "gem5-2GHz-cycles";
#else
    const char *timing = "native-validation-only";
#endif
    printf("CPU_STUDY_CONFIG mode=%s cache=%s cells=%zu rounds=%" PRIu64
           " factor=unpivoted-lu-reciprocal fp_contract=off timing=%s eviction_bytes=%u\n",
           mode, cache, cells, rounds, timing, eviction ? EVICTION_BYTES : 0);
    int passed = 1;
    for (uint64_t round = 0; round < rounds; ++round) {
        condition_cache(&p, eviction, round);
        const uint64_t start = read_cycles();
        if (!round && !preprocess(&p, !strcmp(mode,"pretransform"),
                                 lu, reciprocal, lbar, ubar)) {
            fprintf(stderr, "CPU_STUDY_ERROR LU failed\n"); return 2;
        }
        const uint64_t prepared = !round ? read_cycles() : start;
        sweep(&p, !strcmp(mode,"pretransform"), lu, reciprocal, lbar, ubar, y, dq);
        const uint64_t end = read_cycles();
        double checksum = 0;
        for (size_t i = 0; i < cells*N; ++i) checksum += y[i]+dq[i];
        checksum_sink = checksum;
        char phase[64];
        snprintf(phase, sizeof(phase), "cpu_%s_%" PRIu64, mode, round);
        const int valid = validate_dataset_oracle(&p, y, dq, phase);
        passed &= valid;
        printf("CPU_STUDY_ROUND round=%" PRIu64 " mode=%s cache=%s coeff_state=%s"
               " cycles=%" PRIu64 " preprocess_cycles=%" PRIu64
               " runtime_cycles=%" PRIu64 " checksum=%.17g numeric_ok=%d timing=%s\n",
               round, mode, cache, round ? "reused" : "cold", end-start,
               prepared-start, end-prepared, checksum, valid, timing);
    }
    printf("CPU_STUDY_%s mode=%s cache=%s cells=%zu rounds=%" PRIu64 "\n",
           passed ? "PASS" : "FAIL", mode, cache, cells, rounds);
    free(p.d_mat); free(p.l_mat); free(p.u_mat); free(p.rhs);
    free(p.dataset_oracle_y); free(p.dataset_oracle_dq);
    free(lu); free(reciprocal); free(lbar); free(ubar); free(y); free(dq);
    free((void *)eviction);
    return passed ? 0 : 1;
}
