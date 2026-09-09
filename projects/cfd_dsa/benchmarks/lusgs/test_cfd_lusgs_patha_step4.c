#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 5
#define MATRIX_BYTES (N * N * sizeof(double))
#define VECTOR_BYTES (N * sizeof(double))

#define LUSGS_FLAG_WRITE_DQ    (1u << 0)
#define LUSGS_FLAG_UPDATE_Q    (1u << 1)
#define LUSGS_FLAG_CHECK_BOUNDS (1u << 2)
#define LUSGS_FLAG_TRACE       (1u << 3)

#define LUSGS_STATUS_COMPLETE          0ull
#define LUSGS_STATUS_BUSY              1ull
#define LUSGS_STATUS_BAD_DESCRIPTOR    2ull
#define LUSGS_STATUS_BAD_SHAPE         3ull
#define LUSGS_STATUS_BAD_STRIDE        4ull
#define LUSGS_STATUS_BAD_ALIGNMENT     5ull
#define LUSGS_STATUS_UNSUPPORTED_FLAGS 6ull
#define LUSGS_STATUS_BAD_TOKEN         7ull
#define LUSGS_STATUS_UNSUPPORTED_STAGE 8ull
#define LUSGS_STATUS_SPM_CAPACITY      9ull
#define LUSGS_STATUS_WATCHDOG_TIMEOUT  10ull
#define LUSGS_STATUS_UNEXPECTED_COMPLETION 11ull
#define LUSGS_STATUS_INTERNAL_STATE_ERROR 12ull

typedef struct {
    uint64_t lu_a_base;
    uint64_t c_base;
    uint64_t bbar_base;
    uint64_t rhs_base;
    uint64_t dqstar_base;
    uint64_t dq_base;
    uint64_t q_base;
    uint32_t n_lines;
    uint32_t n_cells;
    uint32_t line_stride_bytes;
    uint32_t matrix_cell_stride_bytes;
    uint32_t vector_cell_stride_bytes;
    uint32_t flags;
    uint32_t tile_cells;
    double omega;
} CfdLusgsDescriptor;

typedef struct {
    size_t lines;
    size_t cells;
    double *lu_a;
    double *c_mat;
    double *b_bar;
    double *rhs;
    double *dq_star_ref;
    double *dq_ref;
    double *dq_star_step2;
    double *dq_step2;
    double *dq_star_step3;
    double *dq_step3;
    double *q_initial;
    double *q_ref;
    double *q_step3;
} Problem;

typedef struct {
    size_t lines;
    size_t cells;
    char stage;
    size_t contexts;
    size_t tile_cells;
    int update_q;
    double omega;
    int check;
    int trace_enable;
    size_t trace_lines;
    size_t trace_cells;
    const char *error_case;
    int snapshot_test;
    int lifecycle_test;
    int wrong_path_launch_test;
    int wrong_path_wait_test;
    int overlap_test;
    int step4_patha_real;
    int step4_trsv_real;
    int step4_vec5_real;
    uint64_t overlap_iters;
} Config;

typedef struct {
    double max_abs_error;
    double max_rel_error;
    uint64_t mismatch_count;
    long first_line;
    long first_cell;
    long first_lane;
    double reference_value;
    double step2_value;
    double step3_value;
} CompareResult;

static inline uint32_t
enc_lusgs_launch(int xtok, int xdesc)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           (0b11111u << 15) | ((xdesc & 31) << 10) |
           (0b00001u << 5) | (xtok & 31);
}

static inline uint32_t
enc_lusgs_wait(int xstatus, int xtok)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           (0b11110u << 15) | ((xtok & 31) << 10) |
           (0b00010u << 5) | (xstatus & 31);
}

static inline uint64_t
read_cycle_counter(void)
{
#if defined(__aarch64__)
    register uint64_t x0 asm("x0") = 0;
    __asm__ __volatile__(".long 0xff070110\n\t" : "+r"(x0) :: "memory");
    return x0 * 2u;
#else
    return 0;
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

static double
trsv5_mul_sub(double acc, double lhs, double rhs)
{
    volatile double product = lhs * rhs;
    return acc - product;
}

static void
trsv5_software(const double lu[N * N], double value[N])
{
    for (int k = 0; k < 4; k++) {
        value[k] /= lu[k * N + k];
        for (int i = k + 1; i < 5; i++)
            value[i] = trsv5_mul_sub(value[i], lu[i * N + k], value[k]);
    }
    value[4] /= lu[4 * N + 4];
    for (int k = 3; k >= 0; k--) {
        for (int i = k + 1; i < 5; i++)
            value[k] = trsv5_mul_sub(value[k], lu[k * N + i], value[i]);
    }
}

static void
mvm5_software(const double mat[N * N], const double vec[N], double out[N])
{
    for (int r = 0; r < N; r++) {
        double acc = 0.0;
        for (int c = 0; c < N; c++) {
            volatile double product = mat[r * N + c] * vec[c];
            acc += product;
        }
        out[r] = acc;
    }
}

static void
copy_vec(double dst[N], const double src[N])
{
    for (int i = 0; i < N; i++)
        dst[i] = src[i];
}

static double
signed_scale(size_t line, size_t cell, int r, int c, double base)
{
    unsigned code = (unsigned)((line + 1) * 17 + (cell + 3) * 13 +
                               (size_t)(r + 5) * 7 + (size_t)(c + 11) * 3);
    double sign = (code & 1u) ? -1.0 : 1.0;
    return sign * base * (double)(1u + (code % 5u));
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
    p->dq_star_step2 = xcalloc(vec_count, sizeof(double), "dq_star_step2");
    p->dq_step2 = xcalloc(vec_count, sizeof(double), "dq_step2");
    p->dq_star_step3 = xcalloc(vec_count, sizeof(double), "dq_star_step3");
    p->dq_step3 = xcalloc(vec_count, sizeof(double), "dq_step3");
    p->q_initial = xcalloc(vec_count, sizeof(double), "q_initial");
    p->q_ref = xcalloc(vec_count, sizeof(double), "q_ref");
    p->q_step3 = xcalloc(vec_count, sizeof(double), "q_step3");

    for (size_t line = 0; line < lines; line++) {
        for (size_t cell = 0; cell < cells; cell++) {
            double *lu = mat_at(p->lu_a, p, line, cell);
            double *cm = mat_at(p->c_mat, p, line, cell);
            double *bb = mat_at(p->b_bar, p, line, cell);
            double *rhs = vec_at(p->rhs, p, line, cell);
            double *q = vec_at(p->q_initial, p, line, cell);
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
                double sign = (code & 1u) ? -1.0 : 1.0;
                rhs[lane] = sign * (0.40 + 0.031 * (double)(line + 1) +
                                    0.019 * (double)(cell + 1) +
                                    0.013 * (double)(lane + 1));
                q[lane] = 0.20 + 0.01 * (double)(line + 1) +
                          0.003 * (double)(cell + 1) +
                          0.002 * (double)(lane + 1);
            }
        }
    }
    memcpy(p->q_ref, p->q_initial, vec_count * sizeof(double));
    memcpy(p->q_step3, p->q_initial, vec_count * sizeof(double));
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
    free(p->dq_star_step2);
    free(p->dq_step2);
    free(p->dq_star_step3);
    free(p->dq_step3);
    free(p->q_initial);
    free(p->q_ref);
    free(p->q_step3);
}

static void
solve_lusgs(const Problem *p, double *dq_star, double *dq)
{
    for (size_t line = 0; line < p->lines; line++) {
        double value[N];
        copy_vec(value, vec_const_at(p->rhs, p, line, 0));
        trsv5_software(mat_const_at(p->lu_a, p, line, 0), value);
        copy_vec(vec_at(dq_star, p, line, 0), value);
        for (size_t cell = 1; cell < p->cells; cell++) {
            double tmp[N];
            double rhs_prime[N];
            mvm5_software(mat_const_at(p->c_mat, p, line, cell),
                          vec_const_at(dq_star, p, line, cell - 1), tmp);
            for (int lane = 0; lane < N; lane++)
                rhs_prime[lane] =
                    vec_const_at(p->rhs, p, line, cell)[lane] - tmp[lane];
            trsv5_software(mat_const_at(p->lu_a, p, line, cell), rhs_prime);
            copy_vec(vec_at(dq_star, p, line, cell), rhs_prime);
        }
        copy_vec(vec_at(dq, p, line, p->cells - 1),
                 vec_const_at(dq_star, p, line, p->cells - 1));
        for (size_t cc = p->cells - 1; cc > 0; cc--) {
            size_t cell = cc - 1;
            double tmp[N];
            mvm5_software(mat_const_at(p->b_bar, p, line, cell),
                          vec_const_at(dq, p, line, cell + 1), tmp);
            for (int lane = 0; lane < N; lane++)
                vec_at(dq, p, line, cell)[lane] =
                    vec_const_at(dq_star, p, line, cell)[lane] - tmp[lane];
        }
    }
}

static void
update_q(double *q, const double *dq, const Problem *p, double omega)
{
    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            double *qv = vec_at(q, p, line, cell);
            const double *dv = vec_const_at(dq, p, line, cell);
            for (int lane = 0; lane < N; lane++)
                qv[lane] += omega * dv[lane];
        }
    }
}

static CompareResult
compare_vectors(const Problem *p, const double *ref, const double *step2,
                const double *step3)
{
    CompareResult r;
    memset(&r, 0, sizeof(r));
    r.first_line = -1;
    r.first_cell = -1;
    r.first_lane = -1;
    const double abs_tol = 0.0;
    const double rel_tol = 0.0;
    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            for (int lane = 0; lane < N; lane++) {
                size_t idx = ((line * p->cells) + cell) * N + lane;
                double diff = fabs(step3[idx] - ref[idx]);
                double denom = fabs(ref[idx]) > 1e-300 ? fabs(ref[idx]) : 1.0;
                double rel = diff / denom;
                if (diff > r.max_abs_error)
                    r.max_abs_error = diff;
                if (rel > r.max_rel_error)
                    r.max_rel_error = rel;
                if (diff > abs_tol && rel > rel_tol) {
                    if (r.mismatch_count == 0) {
                        r.first_line = (long)line;
                        r.first_cell = (long)cell;
                        r.first_lane = lane;
                        r.reference_value = ref[idx];
                        r.step2_value = step2[idx];
                        r.step3_value = step3[idx];
                    }
                    r.mismatch_count++;
                }
            }
        }
    }
    return r;
}

__attribute__((noinline)) static uint64_t
lusgs_launch_wait(CfdLusgsDescriptor *desc, uint64_t *token_out,
                  uint64_t *busy_polls_out)
{
#if defined(__aarch64__)
    const uint32_t launch = enc_lusgs_launch(10, 9);
    const uint32_t wait = enc_lusgs_wait(11, 10);
    register uint64_t desc_reg asm("x9") = (uint64_t)(uintptr_t)desc;
    register uint64_t token_reg asm("x10") = 0;
    register uint64_t status_reg asm("x11") = 0;
    uint64_t busy_polls = 0;
    __asm__ __volatile__(
        ".long %[launch]\n\t"
        : "=r"(token_reg)
        : "r"(desc_reg), [launch] "n"(launch)
        : "memory", "cc");
    do {
        __asm__ __volatile__(
            ".long %[wait]\n\t"
            : "=r"(status_reg)
            : "r"(token_reg), [wait] "n"(wait)
            : "memory", "cc");
        if (status_reg == LUSGS_STATUS_BUSY) {
            busy_polls++;
            __asm__ __volatile__("" ::: "memory");
        }
    } while (status_reg == LUSGS_STATUS_BUSY && busy_polls < 10000000ull);
    *token_out = token_reg;
    *busy_polls_out = busy_polls;
    return status_reg;
#else
    (void)desc;
    *token_out = 1;
    *busy_polls_out = 1;
    return 0;
#endif
}

__attribute__((noinline)) static uint64_t
lusgs_launch_only(CfdLusgsDescriptor *desc)
{
#if defined(__aarch64__)
    const uint32_t launch = enc_lusgs_launch(10, 9);
    register uint64_t desc_reg asm("x9") = (uint64_t)(uintptr_t)desc;
    register uint64_t token_reg asm("x10") = 0;
    __asm__ __volatile__(
        ".long %[launch]\n\t"
        : "=r"(token_reg)
        : "r"(desc_reg), [launch] "n"(launch)
        : "memory", "cc");
    return token_reg;
#else
    (void)desc;
    return 1;
#endif
}

__attribute__((noinline)) static uint64_t
lusgs_wait_only(uint64_t token)
{
#if defined(__aarch64__)
    const uint32_t wait = enc_lusgs_wait(11, 10);
    register uint64_t token_reg asm("x10") = token;
    register uint64_t status_reg asm("x11") = 0;
    __asm__ __volatile__(
        ".long %[wait]\n\t"
        : "=r"(status_reg)
        : "r"(token_reg), [wait] "n"(wait)
        : "memory", "cc");
    return status_reg;
#else
    (void)token;
    return 0;
#endif
}

static uint64_t
lusgs_wait_until_done(uint64_t token, uint64_t *busy_polls_out)
{
    uint64_t busy_polls = 0;
    uint64_t status = lusgs_wait_only(token);
    while (status == LUSGS_STATUS_BUSY && busy_polls < 10000000ull) {
        busy_polls++;
        status = lusgs_wait_only(token);
    }
    *busy_polls_out = busy_polls;
    return status;
}

__attribute__((noinline)) static uint64_t
independent_work(uint64_t iters)
{
    volatile uint64_t acc = 0x1234u;
    volatile double fp = 1.0;
    for (uint64_t i = 0; i < iters; i++) {
        acc += (i ^ (acc << 1)) + 0x9e3779b97f4a7c15ull;
        fp += (double)((i & 15u) + 1u) * 0.0000001;
        fp *= 0.999999999;
    }
    return acc ^ (uint64_t)(fp * 1000000.0);
}

static void
clear_outputs(Problem *p)
{
    size_t vec_count = p->lines * p->cells * N;
    memset(p->dq_star_step3, 0, vec_count * sizeof(double));
    memset(p->dq_step3, 0, vec_count * sizeof(double));
    memcpy(p->q_step3, p->q_initial, vec_count * sizeof(double));
}

static int
parse_bool_arg(const char *s)
{
    return strcmp(s, "1") == 0 || strcmp(s, "true") == 0 ||
           strcmp(s, "yes") == 0 || strcmp(s, "on") == 0;
}

static const char *
arg_value(int argc, char **argv, int *i)
{
    char *eq = strchr(argv[*i], '=');
    if (eq)
        return eq + 1;
    if (*i + 1 >= argc)
        return "";
    (*i)++;
    return argv[*i];
}

static void
parse_args(int argc, char **argv, Config *cfg)
{
    cfg->lines = 1;
    cfg->cells = 17;
    cfg->stage = 'A';
    cfg->contexts = 1;
    cfg->tile_cells = 1;
    cfg->update_q = 0;
    cfg->omega = 1.0;
    cfg->check = 1;
    cfg->trace_enable = 0;
    cfg->trace_lines = 1;
    cfg->trace_cells = 4;
    cfg->error_case = NULL;
    cfg->snapshot_test = 0;
    cfg->lifecycle_test = 0;
    cfg->wrong_path_launch_test = 0;
    cfg->wrong_path_wait_test = 0;
    cfg->overlap_test = 0;
    cfg->step4_patha_real = 0;
    cfg->step4_trsv_real = 0;
    cfg->step4_vec5_real = 0;
    cfg->overlap_iters = 100000;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--lusgs-lines", 13) == 0) {
            cfg->lines = strtoull(arg_value(argc, argv, &i), NULL, 0);
        } else if (strncmp(argv[i], "--lusgs-cells", 13) == 0) {
            cfg->cells = strtoull(arg_value(argc, argv, &i), NULL, 0);
        } else if (strncmp(argv[i], "--lusgs-step3-stage", 19) == 0) {
            const char *v = arg_value(argc, argv, &i);
            cfg->stage = v && *v ? v[0] : 'A';
        } else if (strncmp(argv[i], "--lusgs-contexts", 16) == 0) {
            cfg->contexts = strtoull(arg_value(argc, argv, &i), NULL, 0);
        } else if (strncmp(argv[i], "--lusgs-tile-cells", 18) == 0) {
            cfg->tile_cells = strtoull(arg_value(argc, argv, &i), NULL, 0);
        } else if (strncmp(argv[i], "--lusgs-update-q", 16) == 0) {
            cfg->update_q = parse_bool_arg(arg_value(argc, argv, &i));
        } else if (strncmp(argv[i], "--lusgs-omega", 13) == 0) {
            cfg->omega = strtod(arg_value(argc, argv, &i), NULL);
        } else if (strcmp(argv[i], "--lusgs-controller-trace-enable") == 0) {
            cfg->trace_enable = 1;
        } else if (strncmp(argv[i], "--lusgs-controller-trace-lines", 30) == 0) {
            cfg->trace_lines = strtoull(arg_value(argc, argv, &i), NULL, 0);
        } else if (strncmp(argv[i], "--lusgs-controller-trace-cells", 30) == 0) {
            cfg->trace_cells = strtoull(arg_value(argc, argv, &i), NULL, 0);
        } else if (strncmp(argv[i], "--lusgs-error-case", 18) == 0) {
            cfg->error_case = arg_value(argc, argv, &i);
        } else if (strcmp(argv[i], "--lusgs-step4-snapshot-test") == 0) {
            cfg->snapshot_test = 1;
        } else if (strcmp(argv[i], "--lusgs-step4-lifecycle-test") == 0) {
            cfg->lifecycle_test = 1;
        } else if (strcmp(argv[i], "--lusgs-step4-wrong-path-launch") == 0) {
            cfg->wrong_path_launch_test = 1;
        } else if (strcmp(argv[i], "--lusgs-step4-wrong-path-wait") == 0) {
            cfg->wrong_path_wait_test = 1;
        } else if (strcmp(argv[i], "--lusgs-step4-overlap-test") == 0) {
            cfg->overlap_test = 1;
        } else if (strncmp(argv[i], "--lusgs-step4-patha-real", 24) == 0) {
            cfg->step4_patha_real =
                parse_bool_arg(arg_value(argc, argv, &i));
        } else if (strncmp(argv[i], "--lusgs-step4-trsv-real", 23) == 0) {
            cfg->step4_trsv_real =
                parse_bool_arg(arg_value(argc, argv, &i));
        } else if (strncmp(argv[i], "--lusgs-step4-vec5-real", 23) == 0) {
            cfg->step4_vec5_real =
                parse_bool_arg(arg_value(argc, argv, &i));
        } else if (strncmp(argv[i], "--lusgs-step4-overlap-iters", 27) == 0) {
            cfg->overlap_iters = strtoull(arg_value(argc, argv, &i), NULL, 0);
        }
    }
}

static uint64_t
expected_error_status(const Config *cfg)
{
    if (!cfg->error_case || !*cfg->error_case)
        return LUSGS_STATUS_COMPLETE;
    if (strcmp(cfg->error_case, "bad-shape") == 0)
        return LUSGS_STATUS_BAD_SHAPE;
    if (strcmp(cfg->error_case, "bad-stride") == 0)
        return LUSGS_STATUS_BAD_STRIDE;
    if (strcmp(cfg->error_case, "bad-align") == 0)
        return LUSGS_STATUS_BAD_ALIGNMENT;
    if (strcmp(cfg->error_case, "bad-flags") == 0)
        return LUSGS_STATUS_UNSUPPORTED_FLAGS;
    if (strcmp(cfg->error_case, "missing-q") == 0)
        return LUSGS_STATUS_BAD_DESCRIPTOR;
    if (strcmp(cfg->error_case, "spm-capacity") == 0)
        return LUSGS_STATUS_SPM_CAPACITY;
    if (strcmp(cfg->error_case, "stage-a-multiline") == 0 ||
        strcmp(cfg->error_case, "unsupported-stage") == 0)
        return LUSGS_STATUS_UNSUPPORTED_STAGE;
    if (strcmp(cfg->error_case, "bad-shape-then-legal") == 0)
        return LUSGS_STATUS_BAD_SHAPE;
    if (strcmp(cfg->error_case, "watchdog") == 0 ||
        strcmp(cfg->error_case, "trsv-watchdog") == 0 ||
        strcmp(cfg->error_case, "trsv-watchdog-then-legal") == 0)
        return LUSGS_STATUS_WATCHDOG_TIMEOUT;
    if (strcmp(cfg->error_case, "trsv-stale-generation") == 0)
        return LUSGS_STATUS_COMPLETE;
    if (strcmp(cfg->error_case, "trsv-bad-request-id-then-legal") == 0 ||
        strcmp(cfg->error_case, "vec5-duplicate-then-legal") == 0)
        return LUSGS_STATUS_UNEXPECTED_COMPLETION;
    if (strcmp(cfg->error_case, "bad-token") == 0)
        return LUSGS_STATUS_BAD_TOKEN;
    if (strcmp(cfg->error_case, "busy") == 0)
        return LUSGS_STATUS_BUSY;
    return UINT64_MAX;
}

static int
error_case_then_legal(const char *name)
{
    return strcmp(name, "bad-shape-then-legal") == 0 ||
           strcmp(name, "trsv-watchdog-then-legal") == 0 ||
           strcmp(name, "trsv-bad-request-id-then-legal") == 0 ||
           strcmp(name, "vec5-duplicate-then-legal") == 0;
}

static void
apply_error_case(CfdLusgsDescriptor *desc, Problem *p, const Config *cfg)
{
    if (!cfg->error_case || !*cfg->error_case)
        return;
    if (strcmp(cfg->error_case, "bad-shape") == 0) {
        desc->n_cells = 0;
    } else if (strcmp(cfg->error_case, "bad-stride") == 0) {
        desc->vector_cell_stride_bytes = VECTOR_BYTES - 8;
    } else if (strcmp(cfg->error_case, "bad-align") == 0) {
        desc->rhs_base += 1;
    } else if (strcmp(cfg->error_case, "bad-flags") == 0) {
        desc->flags |= (1u << 31);
    } else if (strcmp(cfg->error_case, "missing-q") == 0) {
        desc->flags |= LUSGS_FLAG_UPDATE_Q;
        desc->q_base = 0;
    } else if (strcmp(cfg->error_case, "spm-capacity") == 0) {
        desc->tile_cells = 1000;
    } else if (strcmp(cfg->error_case, "stage-a-multiline") == 0 ||
               strcmp(cfg->error_case, "unsupported-stage") == 0) {
        desc->n_lines = p->lines >= 2 ? 2 : 1;
        if (desc->n_lines < 2)
            desc->n_lines = 2;
    }
}

static int
run_error_case(CfdLusgsDescriptor *desc, Problem *p, const Config *cfg)
{
    uint64_t expected = expected_error_status(cfg);
    if (expected == UINT64_MAX) {
        printf("lusgs.error.case = %s\n", cfg->error_case);
        printf("lusgs.error.reason = unknown\n");
        printf("LUSGS_STEP4A_ERROR_FAIL\n");
        return 1;
    }

    uint64_t token = 0;
    uint64_t status = 0;
    uint64_t token2 = 0;
    uint64_t cleanup = 0;
    uint64_t cleanup_busy_polls = 0;
    if (strcmp(cfg->error_case, "bad-token") == 0) {
        token = lusgs_launch_only(desc);
        status = lusgs_wait_only(token + 1);
        cleanup = lusgs_wait_until_done(token, &cleanup_busy_polls);
    } else if (strcmp(cfg->error_case, "busy") == 0) {
        token = lusgs_launch_only(desc);
        token2 = lusgs_launch_only(desc);
        cleanup = lusgs_wait_until_done(token, &cleanup_busy_polls);
        status = token2 == 0 && cleanup == LUSGS_STATUS_COMPLETE ?
                 LUSGS_STATUS_BUSY : UINT64_MAX;
    } else if (strcmp(cfg->error_case, "bad-shape-then-legal") == 0) {
        CfdLusgsDescriptor bad = *desc;
        bad.n_cells = 0;
        uint64_t busy_polls = 0;
        status = lusgs_launch_wait(&bad, &token, &busy_polls);
        clear_outputs(p);
        cleanup = lusgs_launch_wait(desc, &token2, &cleanup_busy_polls);
    } else if (error_case_then_legal(cfg->error_case)) {
        uint64_t busy_polls = 0;
        status = lusgs_launch_wait(desc, &token, &busy_polls);
        clear_outputs(p);
        cleanup = lusgs_launch_wait(desc, &token2, &cleanup_busy_polls);
    } else {
        apply_error_case(desc, p, cfg);
        uint64_t busy_polls = 0;
        status = lusgs_launch_wait(desc, &token, &busy_polls);
    }

    printf("lusgs.error.case = %s\n", cfg->error_case);
    printf("lusgs.error.expectedStatus = %llu\n",
           (unsigned long long)expected);
    printf("lusgs.error.status = %llu\n", (unsigned long long)status);
    printf("lusgs.error.token = %llu\n", (unsigned long long)token);
    if (strcmp(cfg->error_case, "busy") == 0) {
        printf("lusgs.error.secondToken = %llu\n",
               (unsigned long long)token2);
        printf("lusgs.error.cleanupStatus = %llu\n",
               (unsigned long long)cleanup);
        printf("lusgs.error.cleanupBusyPolls = %llu\n",
               (unsigned long long)cleanup_busy_polls);
    } else if (error_case_then_legal(cfg->error_case)) {
        printf("lusgs.error.legalToken = %llu\n",
               (unsigned long long)token2);
        printf("lusgs.error.legalStatus = %llu\n",
               (unsigned long long)cleanup);
        printf("lusgs.error.legalBusyPolls = %llu\n",
               (unsigned long long)cleanup_busy_polls);
    } else if (strcmp(cfg->error_case, "bad-token") == 0) {
        printf("lusgs.error.cleanupStatus = %llu\n",
               (unsigned long long)cleanup);
        printf("lusgs.error.cleanupBusyPolls = %llu\n",
               (unsigned long long)cleanup_busy_polls);
    }
    int pass = status == expected;
    if (error_case_then_legal(cfg->error_case))
        pass = pass && cleanup == LUSGS_STATUS_COMPLETE && token2 != 0;
    printf("%s\n", pass ? "LUSGS_STEP4A_ERROR_PASS" :
                          "LUSGS_STEP4A_ERROR_FAIL");
    return pass ? 0 : 1;
}

static int
run_snapshot_test(CfdLusgsDescriptor *desc, Problem *p, const Config *cfg)
{
    (void)cfg;
    uint64_t token = lusgs_launch_only(desc);
    uint64_t status = LUSGS_STATUS_BUSY;
    uint64_t pre_modify_busy = 0;
    for (int i = 0; i < 128 && status == LUSGS_STATUS_BUSY; i++) {
        status = lusgs_wait_only(token);
        if (status == LUSGS_STATUS_BUSY)
            pre_modify_busy++;
    }
    desc->n_cells = 0;
    desc->rhs_base = 0;
    desc->flags |= (1u << 31);
    uint64_t post_busy = 0;
    if (status == LUSGS_STATUS_BUSY)
        status = lusgs_wait_until_done(token, &post_busy);

    CompareResult cmp = compare_vectors(p, p->dq_ref, p->dq_ref,
                                        p->dq_step3);
    printf("lusgs.snapshot.token = %llu\n", (unsigned long long)token);
    printf("lusgs.snapshot.preModifyBusyPolls = %llu\n",
           (unsigned long long)pre_modify_busy);
    printf("lusgs.snapshot.postModifyBusyPolls = %llu\n",
           (unsigned long long)post_busy);
    printf("lusgs.snapshot.status = %llu\n", (unsigned long long)status);
    printf("lusgs.snapshot.mismatch_count = %llu\n",
           (unsigned long long)cmp.mismatch_count);
    int pass = token != 0 && status == LUSGS_STATUS_COMPLETE &&
               cmp.mismatch_count == 0;
    printf("%s\n", pass ? "LUSGS_STEP4A_SNAPSHOT_PASS" :
                          "LUSGS_STEP4A_SNAPSHOT_FAIL");
    return pass ? 0 : 1;
}

static int
run_lifecycle_test(CfdLusgsDescriptor *desc, Problem *p, const Config *cfg)
{
    uint64_t token = lusgs_launch_only(desc);
    uint64_t work_start = read_cycle_counter();
    uint64_t sink = independent_work(cfg->overlap_iters);
    uint64_t work_end = read_cycle_counter();
    uint64_t bad = lusgs_wait_only(token + 17);
    uint64_t busy = 0;
    uint64_t status = lusgs_wait_until_done(token, &busy);
    CompareResult cmp = compare_vectors(p, p->dq_ref, p->dq_ref,
                                        p->dq_step3);

    clear_outputs(p);
    uint64_t token2 = 0;
    uint64_t busy2 = 0;
    uint64_t status2 = lusgs_launch_wait(desc, &token2, &busy2);
    CompareResult cmp2 = compare_vectors(p, p->dq_ref, p->dq_ref,
                                         p->dq_step3);

    printf("lusgs.lifecycle.token = %llu\n", (unsigned long long)token);
    printf("lusgs.lifecycle.badTokenStatus = %llu\n",
           (unsigned long long)bad);
    printf("lusgs.lifecycle.correctStatus = %llu\n",
           (unsigned long long)status);
    printf("lusgs.lifecycle.correctBusyPolls = %llu\n",
           (unsigned long long)busy);
    printf("lusgs.lifecycle.secondToken = %llu\n",
           (unsigned long long)token2);
    printf("lusgs.lifecycle.secondStatus = %llu\n",
           (unsigned long long)status2);
    printf("lusgs.lifecycle.independentWorkStartCycle = %llu\n",
           (unsigned long long)work_start);
    printf("lusgs.lifecycle.independentWorkEndCycle = %llu\n",
           (unsigned long long)work_end);
    printf("lusgs.lifecycle.workSink = %llu\n",
           (unsigned long long)sink);
    printf("lusgs.lifecycle.mismatch_count = %llu\n",
           (unsigned long long)(cmp.mismatch_count + cmp2.mismatch_count));
    int pass = token != 0 && bad == LUSGS_STATUS_BAD_TOKEN &&
               status == LUSGS_STATUS_COMPLETE && token2 != 0 &&
               status2 == LUSGS_STATUS_COMPLETE &&
               cmp.mismatch_count == 0 && cmp2.mismatch_count == 0;
    printf("%s\n", pass ? "LUSGS_STEP4A_LIFECYCLE_PASS" :
                          "LUSGS_STEP4A_LIFECYCLE_FAIL");
    return pass ? 0 : 1;
}

static int
run_wrong_path_launch_test(CfdLusgsDescriptor *desc, Problem *p,
                           const Config *cfg)
{
    (void)cfg;
    volatile int guard = 1;
    uint64_t wrong_token = 0;
    if (guard == 0)
        wrong_token = lusgs_launch_only(desc);
    uint64_t bad = lusgs_wait_only(1);
    uint64_t token = 0;
    uint64_t busy = 0;
    uint64_t status = lusgs_launch_wait(desc, &token, &busy);
    CompareResult cmp = compare_vectors(p, p->dq_ref, p->dq_ref,
                                        p->dq_step3);
    printf("lusgs.wrongPathLaunch.architecturalToken = %llu\n",
           (unsigned long long)wrong_token);
    printf("lusgs.wrongPathLaunch.badTokenStatus = %llu\n",
           (unsigned long long)bad);
    printf("lusgs.wrongPathLaunch.legalToken = %llu\n",
           (unsigned long long)token);
    printf("lusgs.wrongPathLaunch.legalStatus = %llu\n",
           (unsigned long long)status);
    printf("lusgs.wrongPathLaunch.mismatch_count = %llu\n",
           (unsigned long long)cmp.mismatch_count);
    int pass = wrong_token == 0 && bad == LUSGS_STATUS_BAD_TOKEN &&
               token != 0 && status == LUSGS_STATUS_COMPLETE &&
               cmp.mismatch_count == 0;
    printf("%s\n", pass ? "LUSGS_STEP4A_WRONG_PATH_LAUNCH_PASS" :
                          "LUSGS_STEP4A_WRONG_PATH_LAUNCH_FAIL");
    return pass ? 0 : 1;
}

static int
run_wrong_path_wait_test(CfdLusgsDescriptor *desc, Problem *p,
                         const Config *cfg)
{
    uint64_t token = lusgs_launch_only(desc);
    uint64_t sink = independent_work(cfg->overlap_iters);
    volatile int guard = 1;
    uint64_t wrong_status = UINT64_MAX;
    if (guard == 0)
        wrong_status = lusgs_wait_only(token);
    uint64_t bad = lusgs_wait_only(token + 1);
    uint64_t busy = 0;
    uint64_t status = lusgs_wait_until_done(token, &busy);
    CompareResult cmp = compare_vectors(p, p->dq_ref, p->dq_ref,
                                        p->dq_step3);
    printf("lusgs.wrongPathWait.token = %llu\n",
           (unsigned long long)token);
    printf("lusgs.wrongPathWait.architecturalStatus = %llu\n",
           (unsigned long long)wrong_status);
    printf("lusgs.wrongPathWait.badTokenStatus = %llu\n",
           (unsigned long long)bad);
    printf("lusgs.wrongPathWait.correctStatus = %llu\n",
           (unsigned long long)status);
    printf("lusgs.wrongPathWait.correctBusyPolls = %llu\n",
           (unsigned long long)busy);
    printf("lusgs.wrongPathWait.workSink = %llu\n",
           (unsigned long long)sink);
    printf("lusgs.wrongPathWait.mismatch_count = %llu\n",
           (unsigned long long)cmp.mismatch_count);
    int pass = token != 0 && wrong_status == UINT64_MAX &&
               bad == LUSGS_STATUS_BAD_TOKEN &&
               status == LUSGS_STATUS_COMPLETE && cmp.mismatch_count == 0;
    printf("%s\n", pass ? "LUSGS_STEP4A_WRONG_PATH_WAIT_PASS" :
                          "LUSGS_STEP4A_WRONG_PATH_WAIT_FAIL");
    return pass ? 0 : 1;
}

static int
run_overlap_test(CfdLusgsDescriptor *desc, Problem *p, const Config *cfg)
{
    uint64_t launch_start = read_cycle_counter();
    uint64_t token = lusgs_launch_only(desc);
    uint64_t controller_start = read_cycle_counter();
    uint64_t work_start = read_cycle_counter();
    uint64_t sink = independent_work(cfg->overlap_iters);
    uint64_t work_end = read_cycle_counter();
    uint64_t busy = 0;
    uint64_t status = lusgs_wait_until_done(token, &busy);
    uint64_t successful_wait = read_cycle_counter();
    CompareResult cmp = compare_vectors(p, p->dq_ref, p->dq_ref,
                                        p->dq_step3);
    uint64_t overlap_start = work_start > controller_start ?
                             work_start : controller_start;
    uint64_t overlap_end = work_end < successful_wait ?
                           work_end : successful_wait;
    uint64_t overlap = overlap_end > overlap_start ?
                       overlap_end - overlap_start : 0;
    printf("lusgs.overlap.launchStartCycle = %llu\n",
           (unsigned long long)launch_start);
    printf("lusgs.overlap.controllerStartCycle = %llu\n",
           (unsigned long long)controller_start);
    printf("lusgs.overlap.independentWorkStartCycle = %llu\n",
           (unsigned long long)work_start);
    printf("lusgs.overlap.independentWorkEndCycle = %llu\n",
           (unsigned long long)work_end);
    printf("lusgs.overlap.controllerCompleteCycle = %llu\n",
           (unsigned long long)successful_wait);
    printf("lusgs.overlap.successfulWaitCycle = %llu\n",
           (unsigned long long)successful_wait);
    printf("lusgs.overlap.overlapCycles = %llu\n",
           (unsigned long long)overlap);
    printf("lusgs.overlap.busyPolls = %llu\n", (unsigned long long)busy);
    printf("lusgs.overlap.status = %llu\n", (unsigned long long)status);
    printf("lusgs.overlap.workSink = %llu\n", (unsigned long long)sink);
    printf("lusgs.overlap.launchFlags = IsNonSpeculative,IsSerializeBefore,IsSerializeAfter\n");
    printf("lusgs.overlap.mismatch_count = %llu\n",
           (unsigned long long)cmp.mismatch_count);
    int pass = token != 0 && status == LUSGS_STATUS_COMPLETE &&
               overlap > 0 && cmp.mismatch_count == 0;
    printf("%s\n", pass ? "LUSGS_STEP4A_OVERLAP_PASS" :
                          "LUSGS_STEP4A_OVERLAP_FAIL");
    return pass ? 0 : 1;
}

int
main(int argc, char **argv)
{
    Config cfg;
    parse_args(argc, argv, &cfg);
    if (cfg.lines == 0 || cfg.cells == 0 || cfg.contexts == 0 ||
        cfg.tile_cells == 0) {
        printf("LUSGS_STEP4A_FAIL\n");
        return 1;
    }

    Problem p;
    init_problem(&p, cfg.lines, cfg.cells);

    uint64_t ref_start = read_cycle_counter();
    solve_lusgs(&p, p.dq_star_ref, p.dq_ref);
    if (cfg.update_q)
        update_q(p.q_ref, p.dq_ref, &p, cfg.omega);
    uint64_t ref_cycles = read_cycle_counter() - ref_start;

    uint64_t step2_start = read_cycle_counter();
    solve_lusgs(&p, p.dq_star_step2, p.dq_step2);
    uint64_t step2_cycles = read_cycle_counter() - step2_start;

    // Fault in controller output pages before functional Proxy writes.
    clear_outputs(&p);

    CfdLusgsDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.lu_a_base = (uint64_t)(uintptr_t)p.lu_a;
    desc.c_base = (uint64_t)(uintptr_t)p.c_mat;
    desc.bbar_base = (uint64_t)(uintptr_t)p.b_bar;
    desc.rhs_base = (uint64_t)(uintptr_t)p.rhs;
    desc.dqstar_base = (uint64_t)(uintptr_t)p.dq_star_step3;
    desc.dq_base = (uint64_t)(uintptr_t)p.dq_step3;
    desc.q_base = (uint64_t)(uintptr_t)p.q_step3;
    desc.n_lines = (uint32_t)cfg.lines;
    desc.n_cells = (uint32_t)cfg.cells;
    desc.line_stride_bytes = 0;
    desc.matrix_cell_stride_bytes = MATRIX_BYTES;
    desc.vector_cell_stride_bytes = VECTOR_BYTES;
    desc.flags = LUSGS_FLAG_WRITE_DQ | LUSGS_FLAG_CHECK_BOUNDS;
    if (cfg.update_q)
        desc.flags |= LUSGS_FLAG_UPDATE_Q;
    if (cfg.trace_enable)
        desc.flags |= LUSGS_FLAG_TRACE;
    desc.tile_cells = (uint32_t)cfg.tile_cells;
    desc.omega = cfg.omega;

    if (cfg.snapshot_test) {
        gem5_m5_reset_stats();
        int rc = run_snapshot_test(&desc, &p, &cfg);
        gem5_m5_dump_stats();
        free_problem(&p);
        return rc;
    }

    if (cfg.lifecycle_test) {
        gem5_m5_reset_stats();
        int rc = run_lifecycle_test(&desc, &p, &cfg);
        gem5_m5_dump_stats();
        free_problem(&p);
        return rc;
    }

    if (cfg.wrong_path_launch_test) {
        gem5_m5_reset_stats();
        int rc = run_wrong_path_launch_test(&desc, &p, &cfg);
        gem5_m5_dump_stats();
        free_problem(&p);
        return rc;
    }

    if (cfg.wrong_path_wait_test) {
        gem5_m5_reset_stats();
        int rc = run_wrong_path_wait_test(&desc, &p, &cfg);
        gem5_m5_dump_stats();
        free_problem(&p);
        return rc;
    }

    if (cfg.overlap_test) {
        gem5_m5_reset_stats();
        int rc = run_overlap_test(&desc, &p, &cfg);
        gem5_m5_dump_stats();
        free_problem(&p);
        return rc;
    }

    if (cfg.error_case && *cfg.error_case) {
        gem5_m5_reset_stats();
        int rc = run_error_case(&desc, &p, &cfg);
        gem5_m5_dump_stats();
        free_problem(&p);
        return rc;
    }

    gem5_m5_reset_stats();
    uint64_t token = 0;
    uint64_t busy_polls = 0;
    uint64_t step3_start = read_cycle_counter();
    uint64_t status = lusgs_launch_wait(&desc, &token, &busy_polls);
    uint64_t step3_cycles = read_cycle_counter() - step3_start;
    gem5_m5_dump_stats();

    CompareResult step2_ref =
        compare_vectors(&p, p.dq_ref, p.dq_ref, p.dq_step2);
    CompareResult step3_ref =
        compare_vectors(&p, p.dq_ref, p.dq_step2, p.dq_step3);
    CompareResult step3_step2 =
        compare_vectors(&p, p.dq_step2, p.dq_step2, p.dq_step3);
    CompareResult q_cmp;
    memset(&q_cmp, 0, sizeof(q_cmp));
    q_cmp.first_line = -1;
    q_cmp.first_cell = -1;
    q_cmp.first_lane = -1;
    if (cfg.update_q)
        q_cmp = compare_vectors(&p, p.q_ref, p.q_ref, p.q_step3);

    uint64_t expected_patha = 2ull * cfg.lines * (cfg.cells - 1);
    uint64_t expected_trsv = cfg.lines * cfg.cells;
    uint64_t expected_copy = 2ull * cfg.lines;
    uint64_t expected_sub = expected_patha;
    uint64_t expected_axpy = cfg.update_q ? cfg.lines * cfg.cells : 0;
    uint64_t expected_patha_matld = 6ull * expected_patha;
    uint64_t expected_patha_dotp = 5ull * expected_patha;
    uint64_t expected_patha_pack = expected_patha;
    uint64_t expected_trsv_div = 5ull * expected_trsv;
    uint64_t expected_trsv_forward_fma = 10ull * expected_trsv;
    uint64_t expected_trsv_backward_fma = 10ull * expected_trsv;
    uint64_t expected_vec5_lane_ops =
        5ull * (expected_copy + expected_sub + expected_axpy);

    printf("lusgs.step4.stage = %c\n", cfg.stage);
    printf("lusgs.step4.pathaReal = %d\n", cfg.step4_patha_real);
    printf("lusgs.step4.trsvReal = %d\n", cfg.step4_trsv_real);
    printf("lusgs.step4.vec5Real = %d\n", cfg.step4_vec5_real);
    printf("lusgs.lines = %zu\n", cfg.lines);
    printf("lusgs.cells = %zu\n", cfg.cells);
    printf("lusgs.contexts = %zu\n", cfg.contexts);
    printf("lusgs.tileCells = %zu\n", cfg.tile_cells);
    printf("lusgs.updateQ = %d\n", cfg.update_q);
    printf("lusgs.omega = %.17g\n", cfg.omega);
    printf("lusgs.reference.cycles = %llu\n", (unsigned long long)ref_cycles);
    printf("lusgs.step2.cycles = %llu\n", (unsigned long long)step2_cycles);
    printf("lusgs.step4.cpuSubmitWaitCycles = %llu\n",
           (unsigned long long)step3_cycles);
    printf("lusgs.step4.launchToken = %llu\n", (unsigned long long)token);
    printf("lusgs.step4.waitStatus = %llu\n", (unsigned long long)status);
    printf("lusgs.step4.waitBusyPolls = %llu\n",
           (unsigned long long)busy_polls);
    printf("lusgs.expected.pathaMvmTotal = %llu\n",
           (unsigned long long)expected_patha);
    printf("lusgs.expected.trsv5 = %llu\n",
           (unsigned long long)expected_trsv);
    printf("lusgs.expected.vec5Copy = %llu\n",
           (unsigned long long)expected_copy);
    printf("lusgs.expected.vec5Sub = %llu\n",
           (unsigned long long)expected_sub);
    printf("lusgs.expected.vec5Axpy = %llu\n",
           (unsigned long long)expected_axpy);
    printf("lusgs.expected.pathaMatLd = %llu\n",
           (unsigned long long)expected_patha_matld);
    printf("lusgs.expected.pathaDotp = %llu\n",
           (unsigned long long)expected_patha_dotp);
    printf("lusgs.expected.pathaPack = %llu\n",
           (unsigned long long)expected_patha_pack);
    printf("lusgs.expected.trsvDiv = %llu\n",
           (unsigned long long)expected_trsv_div);
    printf("lusgs.expected.trsvForwardFma = %llu\n",
           (unsigned long long)expected_trsv_forward_fma);
    printf("lusgs.expected.trsvBackwardFma = %llu\n",
           (unsigned long long)expected_trsv_backward_fma);
    printf("lusgs.expected.vec5LaneOps = %llu\n",
           (unsigned long long)expected_vec5_lane_ops);
    printf("lusgs.step2_vs_reference.max_abs_error = %.17g\n",
           step2_ref.max_abs_error);
    printf("lusgs.step2_vs_reference.max_rel_error = %.17g\n",
           step2_ref.max_rel_error);
    printf("lusgs.step2_vs_reference.mismatch_count = %llu\n",
           (unsigned long long)step2_ref.mismatch_count);
    printf("lusgs.step4_vs_reference.max_abs_error = %.17g\n",
           step3_ref.max_abs_error);
    printf("lusgs.step4_vs_reference.max_rel_error = %.17g\n",
           step3_ref.max_rel_error);
    printf("lusgs.step4_vs_reference.mismatch_count = %llu\n",
           (unsigned long long)step3_ref.mismatch_count);
    printf("lusgs.step4_vs_step2.max_abs_error = %.17g\n",
           step3_step2.max_abs_error);
    printf("lusgs.step4_vs_step2.max_rel_error = %.17g\n",
           step3_step2.max_rel_error);
    printf("lusgs.step4_vs_step2.mismatch_count = %llu\n",
           (unsigned long long)step3_step2.mismatch_count);
    if (cfg.update_q) {
        printf("lusgs.q_update.max_abs_error = %.17g\n",
               q_cmp.max_abs_error);
        printf("lusgs.q_update.max_rel_error = %.17g\n",
               q_cmp.max_rel_error);
        printf("lusgs.q_update.mismatch_count = %llu\n",
               (unsigned long long)q_cmp.mismatch_count);
    }

    int pass = status == 0 && token != 0 && busy_polls > 0 &&
               step2_ref.mismatch_count == 0 &&
               step3_ref.mismatch_count == 0 &&
               step3_step2.mismatch_count == 0 &&
               (!cfg.update_q || q_cmp.mismatch_count == 0);
    if (cfg.step4_trsv_real && cfg.step4_vec5_real)
        printf("%s\n", pass ? "LUSGS_STEP4C_PASS" : "LUSGS_STEP4C_FAIL");
    else if (cfg.step4_patha_real)
        printf("%s\n", pass ? "LUSGS_STEP4B_PASS" : "LUSGS_STEP4B_FAIL");
    else
        printf("%s\n", pass ? "LUSGS_STEP4A_PASS" : "LUSGS_STEP4A_FAIL");

    free_problem(&p);
    return pass ? 0 : 1;
}
