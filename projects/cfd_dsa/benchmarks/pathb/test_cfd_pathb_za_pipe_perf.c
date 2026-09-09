/*
 * test_cfd_pathb_za_pipe_perf.c -- Path B SME-ZA spatial pipeline benchmark
 *
 * Path B:
 *   6 x lmat5_spm
 *   1 x official ARM SME ZERO {ZA}
 *   5 x cfdsme_mvm5_pipe_step
 *   1 x official ARM SME MOVA ZA final column -> Z
 *   1 x SVE predicated st1d (5 FP64 lanes)
 *
 * Partial sums stay in ZA columns:
 *   ZA[:,0] = A[:,0] * x[0]
 *   ZA[:,k] = ZA[:,k-1] + A[:,k] * x[k], k=1..4
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 5
#define CL 64
#define DEFAULT_BENCH_ITERS 50000ULL

#define SPM_STRIDE_ELEMS 5
#define SPM_STRIDE_BYTES 40

#define SPM_PING_AT_BASE  0x70000000ULL
#define SPM_PING_VEC_BASE (SPM_PING_AT_BASE + 5 * SPM_STRIDE_BYTES)

#define SPM_PONG_AT_BASE  0x70001000ULL
#define SPM_PONG_VEC_BASE (SPM_PONG_AT_BASE + 5 * SPM_STRIDE_BYTES)

#define RESULT_STORE_BYTES 40

#define M5OP_RESET_STATS 0x40
#define M5OP_DUMP_STATS  0x41
#define M5OP_ENC(func)   (0xff000110U | ((uint32_t)(func) << 16))

#define SME_SMSTART      0xd503477fU
#define SME_SMSTOP       0xd503467fU
#define SME_ZERO_ZA      0xc00800ffU
#define SME_MOVA_Z11_PATHB_FINAL 0xc0c2800bU
#define SME_MOVA_Z13_PATHB_FINAL 0xc0c2800dU

static inline uint32_t
enc_lmat5_spm(int row, int rs1, int zd)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b0U << 24) |
           ((row & 7) << 21) | (0U << 20) |
           ((rs1 & 0x1f) << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_cfdsme_mvm5_pipe_step(int zcol, int zvec, int k)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (1U << 22) | (1U << 21) | ((k & 7) << 18) |
           (0b110U << 15) |
           ((zcol & 0x1f) << 10) | ((zvec & 0x1f) << 5);
}

static void
ref_matvec5(const double mat[N][N], const double vec[N], double out[N])
{
    for (int i = 0; i < N; i++) {
        double acc = 0.0;
        for (int k = 0; k < N; k++)
            acc += mat[i][k] * vec[k];
        out[i] = acc;
    }
}

static int
almost_equal5(const double a[N], const double b[N], double tol)
{
    for (int i = 0; i < N; i++) {
        if (fabs(a[i] - b[i]) > tol)
            return 0;
    }
    return 1;
}

static int
verify_pair(const char *name, const double got_ping[N],
            const double expect_ping[N], const double got_pong[N],
            const double expect_pong[N], double tol)
{
    int ok = 1;
    for (int i = 0; i < N; i++) {
        if (fabs(got_ping[i] - expect_ping[i]) > tol) {
            printf("  MISMATCH at %s ping[%d]: got %.8f, expect %.8f\n",
                   name, i, got_ping[i], expect_ping[i]);
            ok = 0;
        }
        if (fabs(got_pong[i] - expect_pong[i]) > tol) {
            printf("  MISMATCH at %s pong[%d]: got %.8f, expect %.8f\n",
                   name, i, got_pong[i], expect_pong[i]);
            ok = 0;
        }
    }
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

static void
print_vec(const char *name, const double v[N])
{
    printf("%s = [", name);
    for (int i = 0; i < N; i++)
        printf("%.4f%s", v[i], (i == N - 1) ? "" : ", ");
    printf("]\n");
}

static void
dma_copy_matrix_transposed_to_spm(const double src_row_major[N][N],
                                  uint64_t spm_at_base)
{
    volatile double *dst = (volatile double *)(uintptr_t)spm_at_base;
    for (int k = 0; k < N; k++) {
        for (int i = 0; i < N; i++)
            dst[k * SPM_STRIDE_ELEMS + i] = src_row_major[i][k];
    }
}

static void
dma_copy_vector_to_spm(const double src[N], uint64_t spm_base)
{
    volatile double *dst = (volatile double *)(uintptr_t)spm_base;
    for (int i = 0; i < N; i++)
        dst[i] = src[i];
}

static inline void
spm_commit_barrier(void)
{
    __asm__ __volatile__("dsb sy\n\tisb\n\t" ::: "memory");
}

static inline void
m5_reset_stats_inline(void)
{
    const uint32_t op = M5OP_ENC(M5OP_RESET_STATS);
    __asm__ __volatile__(
        "mov x0, xzr\n\t"
        "mov x1, xzr\n\t"
        ".long %[op]\n\t"
        :
        : [op] "n"(op)
        : "memory", "x0", "x1");
}

static inline void
m5_dump_stats_inline(void)
{
    const uint32_t op = M5OP_ENC(M5OP_DUMP_STATS);
    __asm__ __volatile__(
        "mov x0, xzr\n\t"
        "mov x1, xzr\n\t"
        ".long %[op]\n\t"
        :
        : [op] "n"(op)
        : "memory", "x0", "x1");
}

static inline void
sme_enter(void)
{
    __asm__ __volatile__(
        ".long %[smstart]\n\t"
        "ptrue p0.d, vl5\n\t"
        :
        : [smstart] "n"(SME_SMSTART)
        : "memory", "p0");
}

static inline void
sme_exit(void)
{
    __asm__ __volatile__(
        ".long %[smstop]\n\t"
        :
        : [smstop] "n"(SME_SMSTOP)
        : "memory");
}

/*
 * Path B: z0/z1 stream ping columns, z6-z10 stream pong columns, z5/z12
 * hold x.  MOVA uses the gem5 PathB override to read ZA[:,4].
 */
__attribute__((noinline)) static void
run_pathb_za_pipe_pingpong(double *ping_result, double *pong_result,
                           uint64_t iters)
{
    (void)enc_lmat5_spm;
    (void)enc_cfdsme_mvm5_pipe_step;

    __asm__ __volatile__(
        "mov x20, %[iters]\n\t"
        "mov x21, %[ping_at]\n\t"
        "mov x22, %[pong_at]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x10, %[rping]\n\t"
        "mov x11, %[rpong]\n\t"
        "mov x12, xzr\n\t"
        ".long 0x000002e5\n\t" /* ping x:    lmat5_spm z5, [x23,#0] */
        ".long 0x000002a0\n\t" /* ping col0: lmat5_spm z0, [x21,#0] */
        ".long 0x002002a1\n\t" /* ping col1: lmat5_spm z1, [x21,#40] */
        "dsb sy\n\t"
        "1:\n\t"

        ".long 0xc00800ff\n\t" /* ZERO {ZA} */
        ".long 0x01e300a0\n\t" /* pipe0: ZA[:,0] = z0*x0 */
        ".long 0x0000030c\n\t" /* pong x */
        ".long 0x004002a0\n\t" /* ping col2 */
        ".long 0x01e704a0\n\t" /* pipe1: ZA[:,1] = ZA[:,0] + z1*x1 */
        ".long 0x000002c6\n\t" /* pong col0 */
        ".long 0x006002a1\n\t" /* ping col3 */
        ".long 0x01eb00a0\n\t" /* pipe2 */
        ".long 0x002002c7\n\t" /* pong col1 */
        ".long 0x008002a0\n\t" /* ping col4 */
        ".long 0x01ef04a0\n\t" /* pipe3 */
        ".long 0x004002c8\n\t" /* pong col2 */
        ".long 0x01f300a0\n\t" /* pipe4: final in ZA[:,4] */
        ".long 0x006002c9\n\t" /* pong col3 */
        ".long 0xc0c2800b\n\t" /* MOVA final PathB ZA[:,4] -> z11 */
        ".long 0x008002ca\n\t" /* pong col4 */
        "st1d {z11.d}, p0, [x10]\n\t"

        ".long 0xc00800ff\n\t" /* ZERO {ZA} */
        ".long 0x01e31980\n\t" /* pong pipe0 */
        ".long 0x000002e5\n\t" /* next ping x */
        ".long 0x01e71d80\n\t" /* pong pipe1 */
        ".long 0x000002a0\n\t" /* next ping col0 */
        ".long 0x01eb2180\n\t" /* pong pipe2 */
        ".long 0x002002a1\n\t" /* next ping col1 */
        ".long 0x01ef2580\n\t" /* pong pipe3 */
        ".long 0x01f32980\n\t" /* pong pipe4 */
        ".long 0xc0c2800d\n\t" /* MOVA final PathB ZA[:,4] -> z13 */
        "st1d {z13.d}, p0, [x11]\n\t"
        "subs x20, x20, #2\n\t"
        "b.le 2f\n\t"
        "b 1b\n\t"
        "2:\n\t"
        :
        : [iters] "r"(iters),
          [ping_at] "r"(SPM_PING_AT_BASE),
          [pong_at] "r"(SPM_PONG_AT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [rping] "r"(ping_result),
          [rpong] "r"(pong_result)
        : "memory", "cc", "x10", "x11", "x12", "x20", "x21", "x22",
          "x23", "x24", "z0", "z1", "z5", "z6", "z7", "z8", "z9",
          "z10", "z11", "z12", "z13");
}

static double *heap_ping;
static double *heap_pong;
static void *heap_raw;

static void
setup_buffers(void)
{
    heap_raw = malloc(2 * RESULT_STORE_BYTES + CL);
    if (!heap_raw)
        return;
    uintptr_t aligned = ((uintptr_t)heap_raw + CL - 1) & ~(uintptr_t)(CL - 1);
    heap_ping = (double *)aligned;
    heap_pong = (double *)(aligned + RESULT_STORE_BYTES);
}

int
main(int argc, char **argv)
{
    uint64_t bench_iters = DEFAULT_BENCH_ITERS;
    uint64_t requested_iters = DEFAULT_BENCH_ITERS;
    int pass = 1;
    volatile uint64_t barrier = 0;

    if (argc > 1) {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(argv[1], &end, 0);
        if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0) {
            fprintf(stderr, "Invalid benchmark iteration count: %s\n", argv[1]);
            return 1;
        }
        requested_iters = (uint64_t)parsed;
        bench_iters = requested_iters;
    }
    if (bench_iters & 1ULL)
        bench_iters++;

    setup_buffers();
    if (!heap_raw) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    const double ping_A[N][N] = {
        { 1.20, -0.70,  2.30,  0.40, -1.10},
        { 0.50,  3.10, -0.20,  1.70,  0.90},
        {-2.00,  0.30,  1.40, -0.80,  2.20},
        { 0.75, -1.25,  0.60,  2.50, -0.45},
        { 1.80,  0.20, -1.60,  0.95,  3.30},
    };
    const double pong_A[N][N] = {
        {-0.40,  2.60,  0.85, -1.30,  1.10},
        { 1.70, -0.90,  2.20,  0.35, -2.40},
        { 0.60,  1.15, -1.75,  2.80,  0.25},
        {-2.10,  0.45,  1.05, -0.65,  3.70},
        { 2.40, -1.55,  0.15,  1.90, -0.80},
    };
    const double ping_x[N] = { 0.90, -1.30, 2.10, -0.55, 1.75 };
    const double pong_x[N] = { -2.20, 0.65, -1.40, 2.75, 0.35 };

    double expect_ping[N];
    double expect_pong[N];
    ref_matvec5(ping_A, ping_x, expect_ping);
    ref_matvec5(pong_A, pong_x, expect_pong);

    if (almost_equal5(expect_ping, expect_pong, 1e-9)) {
        fprintf(stderr, "Invalid test vectors: ping and pong results match.\n");
        return 1;
    }

    dma_copy_matrix_transposed_to_spm(ping_A, SPM_PING_AT_BASE);
    dma_copy_vector_to_spm(ping_x, SPM_PING_VEC_BASE);
    dma_copy_matrix_transposed_to_spm(pong_A, SPM_PONG_AT_BASE);
    dma_copy_vector_to_spm(pong_x, SPM_PONG_VEC_BASE);
    spm_commit_barrier();

    printf("========================================================\n");
    printf("  CFD DSA -- PATH B SME-ZA PIPELINE MVM BENCHMARK\n");
    printf("========================================================\n");
    printf("SPM layout per tile: 5x40B A^T columns + 1x40B vector\n");
    printf("Result store width: %dB (predicated st1d, first 5 FP64 lanes)\n",
           RESULT_STORE_BYTES);
    printf("PathB final result: ZA[0:4,4] -> Z result\n");
    print_vec("expect_ping", expect_ping);
    print_vec("expect_pong", expect_pong);
    printf("benchmark iters(requested/aligned) = %lu/%lu\n",
           (unsigned long)requested_iters, (unsigned long)bench_iters);

    memset(heap_ping, 0, RESULT_STORE_BYTES);
    memset(heap_pong, 0, RESULT_STORE_BYTES);
    sme_enter();
    m5_reset_stats_inline();
    run_pathb_za_pipe_pingpong(heap_ping, heap_pong, bench_iters);
    m5_dump_stats_inline();
    sme_exit();

    pass &= verify_pair("Ping/Pong PathB ZA-pipe MVM", heap_ping, expect_ping,
                        heap_pong, expect_pong, 1e-3);
    pass &= verify_pair("Benchmark PathB ZA-pipe MVM", heap_ping, expect_ping,
                        heap_pong, expect_pong, 1e-3);
    barrier += (uint64_t)heap_ping[0] + (uint64_t)heap_pong[0];

    printf("\n========================================================\n");
    printf("  Barrier: %lu\n", (unsigned long)barrier);
    printf("  Overall: %s\n", pass ? "PASS" : "FAIL");
    printf("========================================================\n");

    free(heap_raw);
    return pass ? 0 : 1;
}
