#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 5
#define SPM_BASE 0x70000000ULL
#define SPM_STRIDE_BYTES 40ULL
#define SPM_STRIDE_ELEMS (SPM_STRIDE_BYTES / sizeof(double))
#define SPM_MATRIX_ROWS 5
#define SPM_PING_MAT_BASE SPM_BASE
#define SPM_PING_VEC_BASE (SPM_BASE + (SPM_MATRIX_ROWS * SPM_STRIDE_BYTES))

typedef struct {
    size_t lines;
    size_t cells;
    double *lu_a;
    double *c_mat;
    double *b_bar;
    double *rhs;
    double *dq_star_ref;
    double *dq_ref;
    double *dq_star_patha;
    double *dq_patha;
    double *tmp_slots;
} Problem;

typedef struct {
    uint64_t sweeps;
    size_t lines;
    size_t cells;
    size_t interleave;
    int run_reference;
    int run_patha;
    int check;
    int trace_enable;
    size_t trace_lines;
    size_t trace_cells;
    const char *patha_store_mode;
    uint64_t patha_store_stride;
    uint64_t patha_unroll;
    const char *patha_acc_mode;
    uint64_t patha_buffer_depth;
    int patha_streaming;
    const char *patha_kernel;
} Config;

typedef struct {
    uint64_t reference_mvm_forward;
    uint64_t reference_mvm_backward;
    uint64_t reference_software_trsv;
    uint64_t reference_vector_sub_ops;
    uint64_t forward_cells;
    uint64_t backward_cells;
    uint64_t patha_mvm_forward;
    uint64_t patha_mvm_backward;
    uint64_t software_trsv;
    uint64_t vector_sub_ops;
    uint64_t tmp_result_stores;
    uint64_t tmp_result_loads;
    uint64_t tmp_result_store_bytes;
    uint64_t tmp_result_load_bytes;
    uint64_t forward_cycles;
    uint64_t backward_cycles;
    uint64_t total_cycles;
    uint64_t software_trsv_cycles;
} Stats;

static volatile double trsv_calibration_sink;

typedef struct {
    double max_abs_error;
    double max_rel_error;
    uint64_t mismatch_count;
    long first_line;
    long first_cell;
    long first_lane;
    double first_ref;
    double first_patha;
} CompareResult;

static inline uint32_t
enc_lmat5_spm(int row, int rs1, int zd)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b0U << 24) |
           ((row & 7) << 21) | (0U << 20) |
           ((rs1 & 0x1f) << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_dotp_row(int lane_id, int zra, int zrb)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b1U << 24) | (0b0U << 23) |
           ((zra & 0x1f) << 10) | ((zrb & 0x1f) << 5) | (lane_id & 0x1f);
}

static inline uint32_t
enc_pack_acc(int zd, int xn)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b1U << 24) | (0b1U << 23) |
           ((xn & 0x1f) << 5) | (zd & 0x1f);
}

static inline uint64_t
read_cycle_counter(void)
{
#if defined(__aarch64__)
    register uint64_t x0 asm("x0") = 0;
    __asm__ __volatile__(".long 0xff070110\n\t" : "+r"(x0) :: "memory");
    return x0 * 2U;
#else
    return 0;
#endif
}

static inline void
full_barrier(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy\n\tisb\n\t" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void
gem5_m5_reset_stats(void)
{
#if defined(__aarch64__)
    register uint64_t x0 asm("x0") = 0;
    register uint64_t x1 asm("x1") = 0;
    __asm__ __volatile__(".long 0xff400110\n\t"
                         : "+r"(x0), "+r"(x1) :: "memory");
#endif
}

static inline void
gem5_m5_dump_stats(void)
{
#if defined(__aarch64__)
    register uint64_t x0 asm("x0") = 0;
    register uint64_t x1 asm("x1") = 0;
    __asm__ __volatile__(".long 0xff410110\n\t"
                         : "+r"(x0), "+r"(x1) :: "memory");
#endif
}

static double *
xcalloc(size_t count, size_t size, const char *name)
{
    if (size != 0 && count > ((size_t)-1) / size) {
        fprintf(stderr, "allocation overflow for %s\n", name);
        exit(2);
    }
    double *ptr = calloc(count, size);
    if (!ptr) {
        fprintf(stderr, "calloc failed for %s: %s\n", name, strerror(errno));
        exit(2);
    }
    return ptr;
}

static double *
vec_at(double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N];
}

static const double *
vec_const_at(const double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N];
}

static double *
mat_at(double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N * N];
}

static const double *
mat_const_at(const double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N * N];
}

static double *
tmp_slot_at(Problem *p, size_t line, size_t slot)
{
    return &p->tmp_slots[((line * 2U) + (slot & 1U)) * N];
}

static void
trsv5_software(const double lu[N * N], double value[N])
{
    for (int k = 0; k < 4; k++) {
        value[k] /= lu[k * N + k];
        for (int i = k + 1; i < 5; i++)
            value[i] -= lu[i * N + k] * value[k];
    }
    value[4] /= lu[4 * N + 4];
    for (int k = 3; k >= 0; k--) {
        for (int i = k + 1; i < 5; i++)
            value[k] -= lu[k * N + i] * value[i];
    }
}

static void
trsv5_counted(const double lu[N * N], double value[N], Stats *stats)
{
    trsv5_software(lu, value);
    stats->software_trsv++;
}

static void
mvm5_software(const double mat[N * N], const double vec[N], double out[N])
{
    for (int r = 0; r < N; r++) {
        double acc = 0.0;
        for (int c = 0; c < N; c++)
            acc += mat[r * N + c] * vec[c];
        out[r] = acc;
    }
}

static void
stage_patha_inputs_to_spm(const double matrix[N * N], const double vector[N])
{
    volatile double *mat_dst = (volatile double *)(uintptr_t)SPM_PING_MAT_BASE;
    volatile double *vec_dst = (volatile double *)(uintptr_t)SPM_PING_VEC_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            mat_dst[r * SPM_STRIDE_ELEMS + c] = matrix[r * N + c];
    }
    for (int c = 0; c < N; c++)
        vec_dst[c] = vector[c];
    full_barrier();
}

__attribute__((noinline)) static void
patha_mvm5_existing(const double matrix[N * N], const double vector[N],
                    double result[N])
{
    stage_patha_inputs_to_spm(matrix, vector);

#if defined(__aarch64__)
    const uint32_t lm[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1),
        enc_lmat5_spm(2, 21, 2), enc_lmat5_spm(3, 21, 3),
        enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv = enc_lmat5_spm(0, 23, 5);
    const uint32_t dp[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5),
        enc_dotp_row(2, 2, 5), enc_dotp_row(3, 3, 5),
        enc_dotp_row(4, 4, 5)
    };
    const uint32_t pack = enc_pack_acc(11, 16);

    __asm__ __volatile__(
        "mov x21, %[mat]\n\t"
        "mov x23, %[vec]\n\t"
        "mov x11, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
        ".long %[lm3]\n\t" ".long %[lm4]\n\t"
        ".long %[lv]\n\t"
        ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
        ".long %[dp3]\n\t" ".long %[dp4]\n\t"
        ".long %[pack]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
          [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
          [out] "r"((uint64_t)(uintptr_t)result),
          [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
          [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
          [lv] "n"(lv),
          [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
          [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
          [pack] "n"(pack)
        : "memory", "cc",
          "x11", "x16", "x17", "x18", "x19", "x20", "x21", "x23",
          "p0", "z0", "z1", "z2", "z3", "z4", "z5", "z11");
#else
    mvm5_software(matrix, vector, result);
#endif
}

static void
read_tmp_result(const double src[N], double dst[N], Stats *stats)
{
    const volatile double *tmp = (const volatile double *)src;
    for (int lane = 0; lane < N; lane++)
        dst[lane] = tmp[lane];
    stats->tmp_result_loads++;
    stats->tmp_result_load_bytes += SPM_STRIDE_BYTES;
}

static double
signed_scale(size_t line, size_t cell, int r, int c, double base)
{
    unsigned code = (unsigned)((line + 1) * 17 + (cell + 3) * 13 +
                               (size_t)(r + 5) * 7 + (size_t)(c + 11) * 3);
    double sign = (code & 1U) ? -1.0 : 1.0;
    return sign * base * (double)(1U + (code % 5U));
}

static void
init_problem(Problem *p, size_t lines, size_t cells)
{
    memset(p, 0, sizeof(*p));
    p->lines = lines;
    p->cells = cells;

    size_t vec_count = lines * cells * N;
    size_t mat_count = lines * cells * N * N;
    p->lu_a = xcalloc(mat_count, sizeof(double), "lu_a");
    p->c_mat = xcalloc(mat_count, sizeof(double), "c_mat");
    p->b_bar = xcalloc(mat_count, sizeof(double), "b_bar");
    p->rhs = xcalloc(vec_count, sizeof(double), "rhs");
    p->dq_star_ref = xcalloc(vec_count, sizeof(double), "dq_star_ref");
    p->dq_ref = xcalloc(vec_count, sizeof(double), "dq_ref");
    p->dq_star_patha = xcalloc(vec_count, sizeof(double), "dq_star_patha");
    p->dq_patha = xcalloc(vec_count, sizeof(double), "dq_patha");
    p->tmp_slots = xcalloc(lines * 2U * N, sizeof(double), "tmp_slots");

    for (size_t line = 0; line < lines; line++) {
        for (size_t cell = 0; cell < cells; cell++) {
            double *lu = mat_at(p->lu_a, p, line, cell);
            double *cm = mat_at(p->c_mat, p, line, cell);
            double *bb = mat_at(p->b_bar, p, line, cell);
            double *rhs = vec_at(p->rhs, p, line, cell);
            for (int r = 0; r < N; r++) {
                for (int c = 0; c < N; c++) {
                    if (r == c) {
                        lu[r * N + c] = 2.40 + 0.09 * (double)(r + 1) +
                                        0.015 * (double)(line + 1) +
                                        0.007 * (double)(cell + 1);
                    } else if (r > c) {
                        lu[r * N + c] = signed_scale(line, cell, r, c, 0.006);
                    } else {
                        lu[r * N + c] = signed_scale(line, cell, r, c, 0.004);
                    }
                    cm[r * N + c] = signed_scale(line + 1, cell, r, c, 0.010);
                    bb[r * N + c] = signed_scale(line, cell + 1, r, c, 0.008);
                }
            }
            for (int lane = 0; lane < N; lane++) {
                unsigned code = (unsigned)((line + 1) * 19 + (cell + 1) * 11 +
                                           (size_t)(lane + 1) * 5);
                double sign = (code & 1U) ? -1.0 : 1.0;
                rhs[lane] = sign * (0.40 + 0.031 * (double)(line + 1) +
                                    0.019 * (double)(cell + 1) +
                                    0.013 * (double)(lane + 1));
            }
        }
    }
}

static void
free_problem(Problem *p)
{
    free(p->lu_a);
    free(p->c_mat);
    free(p->b_bar);
    free(p->rhs);
    free(p->dq_star_ref);
    free(p->dq_ref);
    free(p->dq_star_patha);
    free(p->dq_patha);
    free(p->tmp_slots);
}

static void
copy_vec(double dst[N], const double src[N])
{
    for (int i = 0; i < N; i++)
        dst[i] = src[i];
}

static void
reference_solve(const Problem *p, Stats *stats)
{
    for (size_t line = 0; line < p->lines; line++) {
        double value[N];
        copy_vec(value, vec_const_at(p->rhs, p, line, 0));
        trsv5_software(mat_const_at(p->lu_a, p, line, 0), value);
        stats->reference_software_trsv++;
        copy_vec(vec_at(p->dq_star_ref, p, line, 0), value);

        for (size_t cell = 1; cell < p->cells; cell++) {
            double tmp[N];
            double rhs_prime[N];
            mvm5_software(mat_const_at(p->c_mat, p, line, cell),
                          vec_const_at(p->dq_star_ref, p, line, cell - 1),
                          tmp);
            stats->reference_mvm_forward++;
            for (int lane = 0; lane < N; lane++)
                rhs_prime[lane] =
                    vec_const_at(p->rhs, p, line, cell)[lane] - tmp[lane];
            stats->reference_vector_sub_ops++;
            trsv5_software(mat_const_at(p->lu_a, p, line, cell), rhs_prime);
            stats->reference_software_trsv++;
            copy_vec(vec_at(p->dq_star_ref, p, line, cell), rhs_prime);
        }

        copy_vec(vec_at(p->dq_ref, p, line, p->cells - 1),
                 vec_const_at(p->dq_star_ref, p, line, p->cells - 1));
        for (size_t cc = p->cells - 1; cc > 0; cc--) {
            size_t cell = cc - 1;
            double tmp[N];
            mvm5_software(mat_const_at(p->b_bar, p, line, cell),
                          vec_const_at(p->dq_ref, p, line, cell + 1),
                          tmp);
            stats->reference_mvm_backward++;
            for (int lane = 0; lane < N; lane++)
                vec_at(p->dq_ref, p, line, cell)[lane] =
                    vec_const_at(p->dq_star_ref, p, line, cell)[lane] -
                    tmp[lane];
            stats->reference_vector_sub_ops++;
        }
    }
}

static uint64_t
measure_software_trsv_cycles(const Problem *p, uint64_t sweeps)
{
    double sink = 0.0;
    uint64_t start = read_cycle_counter();
    for (uint64_t sweep = 0; sweep < sweeps; sweep++) {
        for (size_t line = 0; line < p->lines; line++) {
            for (size_t cell = 0; cell < p->cells; cell++) {
                double value[N];
                copy_vec(value, vec_const_at(p->rhs, p, line, cell));
                trsv5_software(mat_const_at(p->lu_a, p, line, cell), value);
                sink += value[0];
            }
        }
    }
    uint64_t end = read_cycle_counter();
    trsv_calibration_sink += sink * 1.0e-300;
    return (end >= start) ? (end - start) : 0;
}

static void
patha_forward_sweep(Problem *p, const Config *cfg, Stats *stats)
{
    uint64_t start = read_cycle_counter();
    for (size_t base = 0; base < p->lines; base += cfg->interleave) {
        size_t end = base + cfg->interleave;
        if (end > p->lines)
            end = p->lines;
        for (size_t line = base; line < end; line++) {
            double value[N];
            copy_vec(value, vec_const_at(p->rhs, p, line, 0));
            trsv5_counted(mat_const_at(p->lu_a, p, line, 0), value, stats);
            copy_vec(vec_at(p->dq_star_patha, p, line, 0), value);
            stats->forward_cells++;
        }
    }

    for (size_t cell = 1; cell < p->cells; cell++) {
        for (size_t base = 0; base < p->lines; base += cfg->interleave) {
            size_t end = base + cfg->interleave;
            if (end > p->lines)
                end = p->lines;
            for (size_t line = base; line < end; line++) {
                double *slot = tmp_slot_at(p, line, cell);
                double tmp[N];
                double rhs_prime[N];
                patha_mvm5_existing(mat_const_at(p->c_mat, p, line, cell),
                                    vec_const_at(p->dq_star_patha, p, line,
                                                 cell - 1),
                                    slot);
                stats->patha_mvm_forward++;
                stats->tmp_result_stores++;
                stats->tmp_result_store_bytes += SPM_STRIDE_BYTES;
                read_tmp_result(slot, tmp, stats);
                for (int lane = 0; lane < N; lane++)
                    rhs_prime[lane] =
                        vec_const_at(p->rhs, p, line, cell)[lane] -
                        tmp[lane];
                stats->vector_sub_ops++;
                trsv5_counted(mat_const_at(p->lu_a, p, line, cell),
                              rhs_prime, stats);
                copy_vec(vec_at(p->dq_star_patha, p, line, cell), rhs_prime);
                stats->forward_cells++;
            }
        }
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->forward_cycles += end - start;
}

static void
patha_backward_sweep(Problem *p, const Config *cfg, Stats *stats)
{
    uint64_t start = read_cycle_counter();
    for (size_t base = 0; base < p->lines; base += cfg->interleave) {
        size_t end = base + cfg->interleave;
        if (end > p->lines)
            end = p->lines;
        for (size_t line = base; line < end; line++) {
            copy_vec(vec_at(p->dq_patha, p, line, p->cells - 1),
                     vec_const_at(p->dq_star_patha, p, line, p->cells - 1));
            stats->backward_cells++;
        }
    }

    for (size_t cc = p->cells - 1; cc > 0; cc--) {
        size_t cell = cc - 1;
        for (size_t base = 0; base < p->lines; base += cfg->interleave) {
            size_t end = base + cfg->interleave;
            if (end > p->lines)
                end = p->lines;
            for (size_t line = base; line < end; line++) {
                double *slot = tmp_slot_at(p, line, cell);
                double tmp[N];
                patha_mvm5_existing(mat_const_at(p->b_bar, p, line, cell),
                                    vec_const_at(p->dq_patha, p, line,
                                                 cell + 1),
                                    slot);
                stats->patha_mvm_backward++;
                stats->tmp_result_stores++;
                stats->tmp_result_store_bytes += SPM_STRIDE_BYTES;
                read_tmp_result(slot, tmp, stats);
                for (int lane = 0; lane < N; lane++)
                    vec_at(p->dq_patha, p, line, cell)[lane] =
                        vec_const_at(p->dq_star_patha, p, line, cell)[lane] -
                        tmp[lane];
                stats->vector_sub_ops++;
                stats->backward_cells++;
            }
        }
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->backward_cycles += end - start;
}

static void
patha_step1_solve(Problem *p, const Config *cfg, Stats *stats)
{
    uint64_t start = read_cycle_counter();
    for (uint64_t sweep = 0; sweep < cfg->sweeps; sweep++) {
        patha_forward_sweep(p, cfg, stats);
        patha_backward_sweep(p, cfg, stats);
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->total_cycles += end - start;
}

static CompareResult
compare_solution(const Problem *p, double abs_tol, double rel_tol)
{
    CompareResult cmp;
    memset(&cmp, 0, sizeof(cmp));
    cmp.first_line = -1;
    cmp.first_cell = -1;
    cmp.first_lane = -1;

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            const double *ref = vec_const_at(p->dq_ref, p, line, cell);
            const double *got = vec_const_at(p->dq_patha, p, line, cell);
            for (int lane = 0; lane < N; lane++) {
                double abs_err = fabs(got[lane] - ref[lane]);
                double denom = fmax(fabs(ref[lane]), 1.0e-14);
                double rel_err = abs_err / denom;
                if (abs_err > cmp.max_abs_error)
                    cmp.max_abs_error = abs_err;
                if (rel_err > cmp.max_rel_error)
                    cmp.max_rel_error = rel_err;
                if (abs_err > abs_tol && rel_err > rel_tol) {
                    if (cmp.mismatch_count == 0) {
                        cmp.first_line = (long)line;
                        cmp.first_cell = (long)cell;
                        cmp.first_lane = (long)lane;
                        cmp.first_ref = ref[lane];
                        cmp.first_patha = got[lane];
                    }
                    cmp.mismatch_count++;
                }
            }
        }
    }
    return cmp;
}

static int
parse_uint64(const char *text, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !end || *end != '\0')
        return 0;
    *out = (uint64_t)value;
    return 1;
}

static int
parse_bool_arg(const char *text)
{
    if (!strcmp(text, "1") || !strcmp(text, "true") || !strcmp(text, "yes") ||
        !strcmp(text, "on"))
        return 1;
    if (!strcmp(text, "0") || !strcmp(text, "false") || !strcmp(text, "no") ||
        !strcmp(text, "off"))
        return 0;
    fprintf(stderr, "invalid boolean value: %s\n", text);
    exit(2);
}

static const char *
option_value(int argc, char **argv, int *idx, const char *arg,
             const char *name)
{
    size_t len = strlen(name);
    if (!strncmp(arg, name, len) && arg[len] == '=')
        return arg + len + 1;
    if (!strcmp(arg, name)) {
        if (*idx + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", name);
            exit(2);
        }
        (*idx)++;
        return argv[*idx];
    }
    return NULL;
}

static void
parse_args(int argc, char **argv, Config *cfg)
{
    cfg->sweeps = 1;
    cfg->lines = 1;
    cfg->cells = 17;
    cfg->interleave = 1;
    cfg->run_reference = 1;
    cfg->run_patha = 1;
    cfg->check = 1;
    cfg->trace_enable = 0;
    cfg->trace_lines = 1;
    cfg->trace_cells = 4;
    cfg->patha_store_mode = "pred40";
    cfg->patha_store_stride = 40;
    cfg->patha_unroll = 8;
    cfg->patha_acc_mode = "internal-buffer";
    cfg->patha_buffer_depth = 2;
    cfg->patha_streaming = 0;
    cfg->patha_kernel = "baseline";

    int idx = 1;
    if (idx < argc && strncmp(argv[idx], "--", 2) != 0) {
        uint64_t sweeps = 0;
        if (!parse_uint64(argv[idx], &sweeps) || sweeps == 0) {
            fprintf(stderr, "invalid sweep/bench iteration count: %s\n",
                    argv[idx]);
            exit(2);
        }
        cfg->sweeps = sweeps;
        idx++;
    }

    if (idx + 6 < argc && strncmp(argv[idx], "--", 2) != 0) {
        cfg->patha_store_mode = argv[idx++];
        if (!parse_uint64(argv[idx++], &cfg->patha_store_stride) ||
            cfg->patha_store_stride == 0) {
            fprintf(stderr, "invalid Path A store stride\n");
            exit(2);
        }
        if (!parse_uint64(argv[idx++], &cfg->patha_unroll) ||
            cfg->patha_unroll == 0) {
            fprintf(stderr, "invalid Path A unroll\n");
            exit(2);
        }
        cfg->patha_acc_mode = argv[idx++];
        if (!parse_uint64(argv[idx++], &cfg->patha_buffer_depth) ||
            cfg->patha_buffer_depth == 0) {
            fprintf(stderr, "invalid Path A buffer depth\n");
            exit(2);
        }
        cfg->patha_streaming = parse_bool_arg(argv[idx++]);
        cfg->patha_kernel = argv[idx++];
    }

    for (; idx < argc; idx++) {
        const char *arg = argv[idx];
        const char *value;
        uint64_t parsed = 0;
        if ((value = option_value(argc, argv, &idx, arg,
                                  "--lusgs-lines")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-lines: %s\n", value);
                exit(2);
            }
            cfg->lines = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-cells")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-cells: %s\n", value);
                exit(2);
            }
            cfg->cells = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-interleave")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-interleave: %s\n", value);
                exit(2);
            }
            cfg->interleave = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-check")) != NULL) {
            cfg->check = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-trace-lines")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-trace-lines: %s\n", value);
                exit(2);
            }
            cfg->trace_lines = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-trace-cells")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-trace-cells: %s\n", value);
                exit(2);
            }
            cfg->trace_cells = (size_t)parsed;
        } else if (!strcmp(arg, "--lusgs-reference-only")) {
            cfg->run_reference = 1;
            cfg->run_patha = 0;
        } else if (!strcmp(arg, "--lusgs-patha-only")) {
            cfg->run_reference = 0;
            cfg->run_patha = 1;
            cfg->check = 0;
        } else if (!strcmp(arg, "--lusgs-trace-enable")) {
            cfg->trace_enable = 1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg);
            exit(2);
        }
    }

    if (!cfg->run_reference && !cfg->run_patha) {
        fprintf(stderr, "at least one of reference or Path A mode must run\n");
        exit(2);
    }
    if (cfg->interleave > cfg->lines)
        cfg->interleave = cfg->lines;
}

static int
patha_default_config_match(const Config *cfg)
{
    return !strcmp(cfg->patha_store_mode, "pred40") &&
           cfg->patha_store_stride == 40 &&
           cfg->patha_unroll == 8 &&
           !strcmp(cfg->patha_acc_mode, "internal-buffer") &&
           cfg->patha_buffer_depth == 2 &&
           cfg->patha_streaming == 0 &&
           !strcmp(cfg->patha_kernel, "baseline");
}

static double
ratio_or_zero(uint64_t num, uint64_t den)
{
    if (den == 0)
        return 0.0;
    return (double)num / (double)den;
}

static void
print_trace(const Problem *p, const Config *cfg)
{
    if (!cfg->trace_enable)
        return;
    size_t lines = cfg->trace_lines < p->lines ? cfg->trace_lines : p->lines;
    size_t cells = cfg->trace_cells < p->cells ? cfg->trace_cells : p->cells;
    printf("lusgs.trace.lines = %zu\n", lines);
    printf("lusgs.trace.cells = %zu\n", cells);
    for (size_t line = 0; line < lines; line++) {
        for (size_t cell = 0; cell < cells; cell++) {
            const double *ref = vec_const_at(p->dq_ref, p, line, cell);
            const double *got = vec_const_at(p->dq_patha, p, line, cell);
            printf("lusgs.trace[%zu][%zu].ref =", line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", ref[lane]);
            printf("\n");
            printf("lusgs.trace[%zu][%zu].patha =", line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", got[lane]);
            printf("\n");
        }
    }
}

static void
print_report(const Config *cfg, const Stats *stats, const CompareResult *cmp,
             int counts_ok, int config_match)
{
    uint64_t mvm_per_dir_per_sweep =
        (uint64_t)cfg->lines * (uint64_t)(cfg->cells > 0 ? cfg->cells - 1 : 0);
    uint64_t expected_forward_mvm = mvm_per_dir_per_sweep * cfg->sweeps;
    uint64_t expected_backward_mvm = mvm_per_dir_per_sweep * cfg->sweeps;
    uint64_t expected_total_mvm = expected_forward_mvm + expected_backward_mvm;
    uint64_t expected_trsv =
        (uint64_t)cfg->lines * (uint64_t)cfg->cells * cfg->sweeps;
    uint64_t expected_vector_sub = expected_total_mvm;
    uint64_t actual_total_mvm =
        stats->patha_mvm_forward + stats->patha_mvm_backward;
    uint64_t total_cells =
        (uint64_t)cfg->lines * (uint64_t)cfg->cells * cfg->sweeps;
    uint64_t tmp_bytes =
        stats->tmp_result_store_bytes + stats->tmp_result_load_bytes;

    printf("\n=== LU-SGS Path A Step1 Report ===\n");
    printf("lusgs.lines = %zu\n", cfg->lines);
    printf("lusgs.cells = %zu\n", cfg->cells);
    printf("lusgs.interleave = %zu\n", cfg->interleave);
    printf("lusgs.sweeps = %lu\n", (unsigned long)cfg->sweeps);
    printf("lusgs.forwardCells = %lu\n", (unsigned long)stats->forward_cells);
    printf("lusgs.backwardCells = %lu\n", (unsigned long)stats->backward_cells);
    printf("lusgs.referenceMvmForward = %lu\n",
           (unsigned long)stats->reference_mvm_forward);
    printf("lusgs.referenceMvmBackward = %lu\n",
           (unsigned long)stats->reference_mvm_backward);
    printf("lusgs.referenceMvmTotal = %lu\n",
           (unsigned long)(stats->reference_mvm_forward +
                           stats->reference_mvm_backward));
    printf("lusgs.referenceSoftwareTrsv = %lu\n",
           (unsigned long)stats->reference_software_trsv);
    printf("lusgs.expected.pathaMvmForward = %lu\n",
           (unsigned long)expected_forward_mvm);
    printf("lusgs.expected.pathaMvmBackward = %lu\n",
           (unsigned long)expected_backward_mvm);
    printf("lusgs.expected.pathaMvmTotal = %lu\n",
           (unsigned long)expected_total_mvm);
    printf("lusgs.expected.softwareTrsv = %lu\n",
           (unsigned long)expected_trsv);
    printf("lusgs.expected.vectorSubOperations = %lu\n",
           (unsigned long)expected_vector_sub);
    printf("lusgs.pathaMvmForward = %lu\n",
           (unsigned long)stats->patha_mvm_forward);
    printf("lusgs.pathaMvmBackward = %lu\n",
           (unsigned long)stats->patha_mvm_backward);
    printf("lusgs.pathaMvmTotal = %lu\n", (unsigned long)actual_total_mvm);
    printf("lusgs.softwareTrsv = %lu\n", (unsigned long)stats->software_trsv);
    printf("lusgs.vectorSubOperations = %lu\n",
           (unsigned long)stats->vector_sub_ops);
    printf("lusgs.vectorSubLanes = %lu\n",
           (unsigned long)(stats->vector_sub_ops * N));
    printf("lusgs.tmpResultStores = %lu\n",
           (unsigned long)stats->tmp_result_stores);
    printf("lusgs.tmpResultLoads = %lu\n",
           (unsigned long)stats->tmp_result_loads);
    printf("lusgs.tmpResultStoreBytes = %lu\n",
           (unsigned long)stats->tmp_result_store_bytes);
    printf("lusgs.tmpResultLoadBytes = %lu\n",
           (unsigned long)stats->tmp_result_load_bytes);
    printf("lusgs.tmpResultBytes = %lu\n", (unsigned long)tmp_bytes);
    printf("lusgs.forwardCycles = %lu\n",
           (unsigned long)stats->forward_cycles);
    printf("lusgs.backwardCycles = %lu\n",
           (unsigned long)stats->backward_cycles);
    printf("lusgs.totalCycles = %lu\n", (unsigned long)stats->total_cycles);
    printf("lusgs.cycleSource = m5_rpns_x2_for_2GHz\n");
    printf("lusgs.softwareTrsvCycles = %lu\n",
           (unsigned long)stats->software_trsv_cycles);
    printf("lusgs.softwareTrsvCycleSource = calibration_before_stats_reset\n");
    printf("lusgs.cyclesPerForwardCell = %.6f\n",
           ratio_or_zero(stats->forward_cycles, total_cells));
    printf("lusgs.cyclesPerBackwardCell = %.6f\n",
           ratio_or_zero(stats->backward_cycles, total_cells));
    printf("lusgs.cyclesPerFullCell = %.6f\n",
           ratio_or_zero(stats->total_cycles, total_cells));
    printf("lusgs.cyclesPerMvm = %.6f\n",
           ratio_or_zero(stats->total_cycles, actual_total_mvm));
    printf("lusgs.cyclesPerSweep = %.6f\n",
           ratio_or_zero(stats->total_cycles, cfg->sweeps));
    printf("lusgs.softwareTrsvCycleShare = %.6f\n",
           ratio_or_zero(stats->software_trsv_cycles, stats->total_cycles));
    printf("lusgs.pathaStoreMode = %s\n", cfg->patha_store_mode);
    printf("lusgs.pathaStoreStride = %lu\n",
           (unsigned long)cfg->patha_store_stride);
    printf("lusgs.pathaUnroll = %lu\n", (unsigned long)cfg->patha_unroll);
    printf("lusgs.pathaAccMode = %s\n", cfg->patha_acc_mode);
    printf("lusgs.pathaResultBufferDepth = %lu\n",
           (unsigned long)cfg->patha_buffer_depth);
    printf("lusgs.pathaStreaming = %d\n", cfg->patha_streaming);
    printf("lusgs.pathaKernel = %s\n", cfg->patha_kernel);
    printf("lusgs.pathaDefaultConfigMatch = %d\n", config_match);
    printf("lusgs.countsMatch = %d\n", counts_ok);
    printf("max_abs_error = %.17g\n", cmp->max_abs_error);
    printf("max_rel_error = %.17g\n", cmp->max_rel_error);
    printf("mismatch_count = %lu\n", (unsigned long)cmp->mismatch_count);
    printf("first_mismatch_line = %ld\n", cmp->first_line);
    printf("first_mismatch_cell = %ld\n", cmp->first_cell);
    printf("first_mismatch_lane = %ld\n", cmp->first_lane);
    printf("first_mismatch_ref = %.17g\n", cmp->first_ref);
    printf("first_mismatch_patha = %.17g\n", cmp->first_patha);
}

int
main(int argc, char **argv)
{
    Config cfg;
    Problem problem;
    Stats stats;
    CompareResult cmp;
    memset(&stats, 0, sizeof(stats));
    memset(&cmp, 0, sizeof(cmp));
    cmp.first_line = -1;
    cmp.first_cell = -1;
    cmp.first_lane = -1;

    parse_args(argc, argv, &cfg);
    init_problem(&problem, cfg.lines, cfg.cells);

    printf("CFD LU-SGS Path A Step1 benchmark\n");
    printf("mode.reference = %d\n", cfg.run_reference);
    printf("mode.patha = %d\n", cfg.run_patha);
    printf("mode.check = %d\n", cfg.check);

    if (cfg.run_reference)
        reference_solve(&problem, &stats);

    stats.software_trsv_cycles =
        measure_software_trsv_cycles(&problem, cfg.sweeps);

    if (cfg.run_patha) {
        gem5_m5_reset_stats();
        patha_step1_solve(&problem, &cfg, &stats);
        gem5_m5_dump_stats();
    }

    int counts_ok = 1;
    uint64_t mvm_per_dir_per_sweep =
        (uint64_t)cfg.lines * (uint64_t)(cfg.cells > 0 ? cfg.cells - 1 : 0);
    uint64_t expected_forward_mvm = mvm_per_dir_per_sweep * cfg.sweeps;
    uint64_t expected_backward_mvm = mvm_per_dir_per_sweep * cfg.sweeps;
    uint64_t expected_trsv =
        (uint64_t)cfg.lines * (uint64_t)cfg.cells * cfg.sweeps;
    if (cfg.run_patha) {
        counts_ok &= stats.patha_mvm_forward == expected_forward_mvm;
        counts_ok &= stats.patha_mvm_backward == expected_backward_mvm;
        counts_ok &= stats.software_trsv == expected_trsv;
        counts_ok &= stats.vector_sub_ops ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= stats.tmp_result_stores ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= stats.tmp_result_loads ==
                     expected_forward_mvm + expected_backward_mvm;
    }

    if (cfg.run_reference && cfg.run_patha && cfg.check)
        cmp = compare_solution(&problem, 1.0e-10, 1.0e-10);

    int config_match = patha_default_config_match(&cfg);
    print_report(&cfg, &stats, &cmp, counts_ok, config_match);
    print_trace(&problem, &cfg);

    int pass = counts_ok;
    if (cfg.run_reference && cfg.run_patha && cfg.check)
        pass &= (cmp.mismatch_count == 0);
    printf("%s\n", pass ? "LUSGS_STEP1_PASS" : "LUSGS_STEP1_FAIL");

    free_problem(&problem);
    return pass ? 0 : 1;
}
