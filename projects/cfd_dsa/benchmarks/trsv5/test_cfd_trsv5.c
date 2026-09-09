#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 5
#define SPM_BASE 0x70000000ULL
#define SPM_LU_BASE SPM_BASE
#define SPM_RHS_BASE (SPM_BASE + 25ULL * sizeof(double))

typedef struct {
    uint64_t solves;
    uint64_t trsv5_lat;
    uint64_t trsv5_count;
    const char *trsv5_mode;
    int trace_enable;
    uint64_t trace_solves;
    const char *trace_file;
} Config;

typedef struct {
    uint64_t expected_solves;
    uint64_t actual_solves;
    uint64_t expected_div_ops;
    uint64_t actual_div_ops;
    uint64_t expected_mulsub_ops;
    uint64_t actual_mulsub_ops;
    uint64_t cycles;
    double max_abs_error;
    double max_rel_error;
    uint64_t mismatch_count;
    long first_mismatch_index;
    long first_mismatch_lane;
    double first_reference;
    double first_hardware;
} Stats;

static inline uint32_t
enc_trsv5_lu_spm(int zd, int lu_base, int rhs_base)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (0U << 22) | (0U << 20) |
           ((lu_base & 0x1f) << 15) | ((rhs_base & 0x1f) << 10) |
           (0U << 5) | (zd & 0x1f);
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

static int
parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !end || *end != '\0')
        return 0;
    *out = (uint64_t)value;
    return 1;
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
    cfg->solves = 1;
    cfg->trsv5_lat = 60;
    cfg->trsv5_count = 1;
    cfg->trsv5_mode = "zreg-coarse";
    cfg->trace_enable = 0;
    cfg->trace_solves = 16;
    cfg->trace_file = "m5out/trsv5_trace.csv";

    int idx = 1;
    if (idx < argc && strncmp(argv[idx], "--", 2) != 0) {
        if (!parse_u64(argv[idx], &cfg->solves) || cfg->solves == 0) {
            fprintf(stderr, "invalid solve count: %s\n", argv[idx]);
            exit(2);
        }
        idx++;
    }

    for (; idx < argc; idx++) {
        const char *arg = argv[idx];
        const char *value;
        uint64_t parsed = 0;
        if ((value = option_value(argc, argv, &idx, arg,
                                  "--trsv5-lat")) != NULL) {
            if (!parse_u64(value, &parsed) || parsed <= 1) {
                fprintf(stderr, "invalid --trsv5-lat: %s\n", value);
                exit(2);
            }
            cfg->trsv5_lat = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-count")) != NULL) {
            if (!parse_u64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --trsv5-count: %s\n", value);
                exit(2);
            }
            cfg->trsv5_count = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-mode")) != NULL) {
            cfg->trsv5_mode = value;
        } else if (!strcmp(arg, "--trsv5-trace-enable")) {
            cfg->trace_enable = 1;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-trace-solves")) != NULL) {
            if (!parse_u64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --trsv5-trace-solves: %s\n", value);
                exit(2);
            }
            cfg->trace_solves = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-trace-file")) != NULL) {
            cfg->trace_file = value;
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg);
            exit(2);
        }
    }
}

static double
signed_term(uint64_t idx, int r, int c, double scale)
{
    uint64_t code = (idx + 1) * 1103515245ULL +
                    (uint64_t)(r + 3) * 97ULL +
                    (uint64_t)(c + 5) * 53ULL;
    double sign = (code & 1ULL) ? -1.0 : 1.0;
    return sign * scale * (double)(1 + (code % 7ULL));
}

static void
make_case(uint64_t idx, double lu[N][N], double rhs[N])
{
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (r == c) {
                lu[r][c] = 2.25 + 0.11 * (double)(r + 1) +
                           0.00003 * (double)(idx % 1009ULL);
            } else if (r > c) {
                lu[r][c] = signed_term(idx, r, c, 0.004);
            } else {
                lu[r][c] = signed_term(idx + 13, r, c, 0.003);
            }
        }
        rhs[r] = signed_term(idx + 29, r, 0, 0.021) +
                 0.25 + 0.007 * (double)(idx % 17ULL);
    }
}

static double
trsv5_mul_sub(double acc, double lhs, double rhs)
{
    volatile double product = lhs * rhs;
    return acc - product;
}

static void
trsv5_reference(const double lu[N][N], const double rhs[N], double result[N])
{
    double value[N];
    for (int i = 0; i < N; ++i)
        value[i] = rhs[i];

    for (int k = 0; k < 4; ++k) {
        value[k] /= lu[k][k];
        for (int i = k + 1; i < 5; ++i)
            value[i] = trsv5_mul_sub(value[i], lu[i][k], value[k]);
    }
    value[4] /= lu[4][4];
    for (int k = 3; k >= 0; --k) {
        for (int i = k + 1; i < 5; ++i)
            value[k] = trsv5_mul_sub(value[k], lu[k][i], value[i]);
    }

    for (int i = 0; i < N; ++i)
        result[i] = value[i];
}

static void
stage_inputs(const double lu[N][N], const double rhs[N])
{
    volatile double *lu_dst = (volatile double *)(uintptr_t)SPM_LU_BASE;
    volatile double *rhs_dst = (volatile double *)(uintptr_t)SPM_RHS_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            lu_dst[r * N + c] = lu[r][c];
    }
    for (int i = 0; i < N; i++)
        rhs_dst[i] = rhs[i];
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy\n\tisb\n\t" ::: "memory");
#else
    __sync_synchronize();
#endif
}

__attribute__((noinline)) static void
trsv5_hardware(const double lu[N][N], const double rhs[N], double result[N])
{
    stage_inputs(lu, rhs);

#if defined(__aarch64__)
    const uint32_t trsv = enc_trsv5_lu_spm(11, 21, 23);
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x23, %[rhs]\n\t"
        "mov x11, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[trsv]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [lu] "r"((uint64_t)SPM_LU_BASE),
          [rhs] "r"((uint64_t)SPM_RHS_BASE),
          [out] "r"((uint64_t)(uintptr_t)result),
          [trsv] "n"(trsv)
        : "memory", "cc", "x11", "x21", "x23", "p0", "z11");
#else
    trsv5_reference(lu, rhs, result);
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

static void
write_trace(const Config *cfg, const double *hardware,
            const uint64_t *trace_start, const uint64_t *trace_end,
            uint64_t trace_count)
{
    if (!cfg->trace_enable)
        return;
    FILE *fp = fopen(cfg->trace_file, "w");
    if (!fp) {
        fprintf(stderr, "warning: could not open trace file %s: %s\n",
                cfg->trace_file, strerror(errno));
        return;
    }
    fprintf(fp, "cycle,seqNum,instruction,solve_index,line,cell,phase,"
                "configured_latency,input_ready,execute,complete,writeback,"
                "squashed,result_lane0,result_lane1,result_lane2,result_lane3,"
                "result_lane4\n");
    for (uint64_t i = 0; i < trace_count; i++) {
        const double *res = &hardware[i * N];
        uint64_t start = trace_start ? trace_start[i] : 0;
        uint64_t done = trace_end ? trace_end[i] : start + cfg->trsv5_lat;
        fprintf(fp, "%lu,%lu,trsv5_lu_spm,%lu,-1,-1,standalone,%lu,%lu,%lu,"
                    "%lu,%lu,0,%.17g,%.17g,%.17g,%.17g,%.17g\n",
                (unsigned long)start, (unsigned long)i, (unsigned long)i,
                (unsigned long)cfg->trsv5_lat, (unsigned long)start,
                (unsigned long)start, (unsigned long)done,
                (unsigned long)done,
                res[0], res[1], res[2], res[3], res[4]);
    }
    fclose(fp);
}

int
main(int argc, char **argv)
{
    Config cfg;
    Stats stats;
    parse_args(argc, argv, &cfg);
    memset(&stats, 0, sizeof(stats));
    stats.expected_solves = cfg.solves;
    stats.expected_div_ops = cfg.solves * 5ULL;
    stats.expected_mulsub_ops = cfg.solves * 20ULL;
    stats.first_mismatch_index = -1;
    stats.first_mismatch_lane = -1;

    double *reference = xcalloc((size_t)cfg.solves * N, sizeof(double),
                                "reference");
    double *hardware = xcalloc((size_t)cfg.solves * N, sizeof(double),
                               "hardware");
    uint64_t trace_count =
        cfg.trace_enable && cfg.trace_solves < cfg.solves ?
        cfg.trace_solves : (cfg.trace_enable ? cfg.solves : 0);
    uint64_t *trace_start = trace_count ?
        (uint64_t *)calloc((size_t)trace_count, sizeof(uint64_t)) : NULL;
    uint64_t *trace_end = trace_count ?
        (uint64_t *)calloc((size_t)trace_count, sizeof(uint64_t)) : NULL;
    if (trace_count && (!trace_start || !trace_end)) {
        fprintf(stderr, "trace allocation failed\n");
        return 2;
    }

    for (uint64_t i = 0; i < cfg.solves; i++) {
        double lu[N][N];
        double rhs[N];
        make_case(i, lu, rhs);
        trsv5_reference(lu, rhs, &reference[i * N]);
    }

    gem5_m5_reset_stats();
    uint64_t start = read_cycle_counter();
    for (uint64_t i = 0; i < cfg.solves; i++) {
        double lu[N][N];
        double rhs[N];
        make_case(i, lu, rhs);
        if (i < trace_count)
            trace_start[i] = read_cycle_counter();
        trsv5_hardware(lu, rhs, &hardware[i * N]);
        if (i < trace_count)
            trace_end[i] = read_cycle_counter();
        stats.actual_solves++;
    }
    uint64_t end = read_cycle_counter();
    gem5_m5_dump_stats();
    stats.cycles = (end >= start) ? (end - start) : 0;
    stats.actual_div_ops = stats.actual_solves * 5ULL;
    stats.actual_mulsub_ops = stats.actual_solves * 20ULL;

    for (uint64_t i = 0; i < cfg.solves; i++) {
        for (int lane = 0; lane < N; lane++) {
            double ref = reference[i * N + lane];
            double got = hardware[i * N + lane];
            double abs_err = fabs(got - ref);
            double rel_err = abs_err / fmax(fabs(ref), 1.0e-14);
            if (abs_err > stats.max_abs_error)
                stats.max_abs_error = abs_err;
            if (rel_err > stats.max_rel_error)
                stats.max_rel_error = rel_err;
            if (memcmp(&got, &ref, sizeof(double)) != 0) {
                if (stats.mismatch_count == 0) {
                    stats.first_mismatch_index = (long)i;
                    stats.first_mismatch_lane = lane;
                    stats.first_reference = ref;
                    stats.first_hardware = got;
                }
                stats.mismatch_count++;
            }
        }
    }

    write_trace(&cfg, hardware, trace_start, trace_end, trace_count);

    int counts_ok = stats.actual_solves == stats.expected_solves &&
                    stats.actual_div_ops == stats.expected_div_ops &&
                    stats.actual_mulsub_ops == stats.expected_mulsub_ops;
    int pass = counts_ok && stats.mismatch_count == 0;

    printf("TRSV5 standalone benchmark\n");
    printf("trsv5.mode = %s\n", cfg.trsv5_mode);
    printf("trsv5.latencyConfigured = %lu\n", (unsigned long)cfg.trsv5_lat);
    printf("trsv5.countConfigured = %lu\n", (unsigned long)cfg.trsv5_count);
    printf("trsv5.expectedSolves = %lu\n", (unsigned long)stats.expected_solves);
    printf("trsv5.actualSolves = %lu\n", (unsigned long)stats.actual_solves);
    printf("trsv5.expectedDivOps = %lu\n", (unsigned long)stats.expected_div_ops);
    printf("trsv5.actualDivOps = %lu\n", (unsigned long)stats.actual_div_ops);
    printf("trsv5.expectedMulSubOps = %lu\n",
           (unsigned long)stats.expected_mulsub_ops);
    printf("trsv5.actualMulSubOps = %lu\n",
           (unsigned long)stats.actual_mulsub_ops);
    printf("trsv5.cycles = %lu\n", (unsigned long)stats.cycles);
    printf("trsv5.cyclesPerSolve = %.6f\n",
           cfg.solves ? (double)stats.cycles / (double)cfg.solves : 0.0);
    printf("trsv5.countsMatch = %d\n", counts_ok);
    printf("max_abs_error = %.17g\n", stats.max_abs_error);
    printf("max_rel_error = %.17g\n", stats.max_rel_error);
    printf("mismatch_count = %lu\n", (unsigned long)stats.mismatch_count);
    printf("first_mismatch_index = %ld\n", stats.first_mismatch_index);
    printf("first_mismatch_lane = %ld\n", stats.first_mismatch_lane);
    printf("reference_value = %.17g\n", stats.first_reference);
    printf("hardware_value = %.17g\n", stats.first_hardware);
    printf("%s\n", pass ? "TRSV5_PASS" : "TRSV5_FAIL");

    free(reference);
    free(hardware);
    free(trace_start);
    free(trace_end);
    return pass ? 0 : 1;
}
