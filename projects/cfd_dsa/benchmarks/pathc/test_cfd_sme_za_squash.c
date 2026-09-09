/*
 * test_cfd_sme_za_squash.c -- CFD-SME ZA speculative squash stress
 *
 * The wrong path contains ready cfdsme_za_outer5_step instructions using a
 * deliberately different matrix/vector.  A taken branch dependent on an SPM
 * memory load should squash that fall-through path on O3.  The final result
 * must match only the correct-path MVM.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define N 5
#define SPM_STRIDE_ELEMS 5
#define SPM_STRIDE_BYTES 40
#define SPM_CORRECT_AT_BASE  0x70000000ULL
#define SPM_CORRECT_VEC_BASE (SPM_CORRECT_AT_BASE + 5 * SPM_STRIDE_BYTES)
#define SPM_WRONG_AT_BASE    0x70001000ULL
#define SPM_WRONG_VEC_BASE   (SPM_WRONG_AT_BASE + 5 * SPM_STRIDE_BYTES)
#define SPM_FLAG_BASE        0x70002000ULL

#define M5OP_RESET_STATS 0x40
#define M5OP_DUMP_STATS  0x41
#define M5OP_ENC(func)   (0xff000110U | ((uint32_t)(func) << 16))

#define SME_SMSTART       0xd503477fU
#define SME_SMSTOP        0xd503467fU
#define SME_ZERO_ZA       0xc00800ffU
#define SME_MOVA_Z11_FINAL 0xc0c2800bU

static inline uint32_t
enc_lmat5_spm(int row, int rs1, int zd)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b0U << 24) |
           ((row & 7) << 21) | ((rs1 & 0x1f) << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_cfdsme_za_outer5_step(int zcol, int zvec, int k)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (1U << 22) | (0U << 21) | ((k & 7) << 18) | (0b110U << 15) |
           ((zcol & 0x1f) << 10) | ((zvec & 0x1f) << 5);
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

static void
dma_copy_matrix_transposed_to_spm(const double src[N][N], uint64_t base)
{
    volatile double *dst = (volatile double *)(uintptr_t)base;
    for (int k = 0; k < N; k++) {
        for (int i = 0; i < N; i++)
            dst[k * SPM_STRIDE_ELEMS + i] = src[i][k];
    }
}

static void
dma_copy_vector_to_spm(const double src[N], uint64_t base)
{
    volatile double *dst = (volatile double *)(uintptr_t)base;
    for (int i = 0; i < N; i++)
        dst[i] = src[i];
}

static int
verify_vec(const double got[N], const double expect[N])
{
    int ok = 1;
    for (int i = 0; i < N; i++) {
        if (fabs(got[i] - expect[i]) > 1e-8) {
            printf("  MISMATCH result[%d]: got %.8f, expect %.8f\n",
                   i, got[i], expect[i]);
            ok = 0;
        }
    }
    printf("ZA squash stress: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static double squash_result[N] __attribute__((aligned(64)));

__attribute__((noinline)) static void
run_za_squash_stress(double *result)
{
    __asm__ __volatile__(
        "mov x21, %[correct_at]\n\t"
        "mov x22, %[wrong_at]\n\t"
        "mov x23, %[correct_vec]\n\t"
        "mov x24, %[wrong_vec]\n\t"
        "mov x25, %[flag]\n\t"
        "mov x10, %[result]\n\t"
        "mov x12, xzr\n\t"       /* MOVA slice register; CFD override returns final column */

        /* Preload both correct and wrong operands before the branch. */
        ".long 0x000002e5\n\t" /* z5  = correct x */
        ".long 0x000002a0\n\t" /* z0  = correct col0 */
        ".long 0x002002a1\n\t" /* z1  = correct col1 */
        ".long 0x004002a2\n\t" /* z2  = correct col2 */
        ".long 0x006002a3\n\t" /* z3  = correct col3 */
        ".long 0x008002a4\n\t" /* z4  = correct col4 */
        ".long 0x0000030c\n\t" /* z12 = wrong x */
        ".long 0x000002c6\n\t" /* z6  = wrong col0 */
        ".long 0x002002c7\n\t" /* z7  = wrong col1 */
        ".long 0x004002c8\n\t" /* z8  = wrong col2 */
        ".long 0x006002c9\n\t" /* z9  = wrong col3 */
        ".long 0x008002ca\n\t" /* z10 = wrong col4 */
        "dsb sy\n\t"

        ".long 0xc00800ff\n\t" /* ZERO {ZA} */
        "ldr w9, [x25]\n\t"
        "cbnz w9, 1f\n\t"

        /*
         * Predicted fall-through wrong path.  If these execute
         * speculatively, commit squash must discard their ZA updates.
         */
        ".long 0x01c31980\n\t" /* wrong outer0: z6,  z12, #0 */
        ".long 0x01c71d80\n\t" /* wrong outer1: z7,  z12, #1 */
        ".long 0x01cb2180\n\t" /* wrong outer2: z8,  z12, #2 */
        ".long 0x01cf2580\n\t" /* wrong outer3: z9,  z12, #3 */
        ".long 0x01d32980\n\t" /* wrong outer4: z10, z12, #4 */
        "b 2f\n\t"

        "1:\n\t"
        ".long 0xc00800ff\n\t" /* redirected correct-path MVM starts fresh */
        ".long 0x000002e5\n\t"
        ".long 0x000002a0\n\t"
        ".long 0x002002a1\n\t"
        "dsb sy\n\t"
        ".long 0x01c300a0\n\t"
        ".long 0x004002a0\n\t"
        ".long 0x01c704a0\n\t"
        ".long 0x006002a1\n\t"
        ".long 0x01cb00a0\n\t"
        ".long 0x008002a0\n\t"
        ".long 0x01cf04a0\n\t"
        ".long 0x01d300a0\n\t"
        "2:\n\t"
        "dsb sy\n\t"
        ".long 0xc0c2800b\n\t" /* MOVA final Path C ZA column -> z11 */
        "ptrue p0.d, vl5\n\t"
        "st1d {z11.d}, p0, [x10]\n\t"
        :
        : [correct_at] "r"(SPM_CORRECT_AT_BASE),
          [wrong_at] "r"(SPM_WRONG_AT_BASE),
          [correct_vec] "r"(SPM_CORRECT_VEC_BASE),
          [wrong_vec] "r"(SPM_WRONG_VEC_BASE),
          [flag] "r"(SPM_FLAG_BASE),
          [result] "r"(result)
        : "memory", "cc", "x9", "x10", "x12", "x21", "x22", "x23", "x24", "x25",
          "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7", "z8", "z9",
          "z10", "z11", "z12");
}

int
main(void)
{
    const double correct_A[N][N] = {
        {1.5, -2.0, 0.5, 3.0, -1.0},
        {0.25, 1.75, -3.5, 2.0, 0.75},
        {-1.25, 0.5, 2.25, -0.75, 4.0},
        {3.5, -1.0, 1.25, 0.5, -2.25},
        {-0.5, 2.5, -1.5, 1.0, 3.25}
    };
    const double wrong_A[N][N] = {
        {9.0, 8.0, 7.0, 6.0, 5.0},
        {-8.0, -7.0, -6.0, -5.0, -4.0},
        {4.5, -3.5, 2.5, -1.5, 0.5},
        {10.0, 20.0, 30.0, 40.0, 50.0},
        {-9.0, 1.0, -8.0, 2.0, -7.0}
    };
    const double correct_x[N] = {0.75, -1.25, 2.0, -0.5, 1.5};
    const double wrong_x[N] = {5.0, -4.0, 3.0, -2.0, 1.0};
    double expect[N];
    for (int i = 0; i < N; i++)
        squash_result[i] = 0.0;

    ref_matvec5(correct_A, correct_x, expect);
    dma_copy_matrix_transposed_to_spm(correct_A, SPM_CORRECT_AT_BASE);
    dma_copy_vector_to_spm(correct_x, SPM_CORRECT_VEC_BASE);
    dma_copy_matrix_transposed_to_spm(wrong_A, SPM_WRONG_AT_BASE);
    dma_copy_vector_to_spm(wrong_x, SPM_WRONG_VEC_BASE);
    *(volatile uint32_t *)(uintptr_t)SPM_FLAG_BASE = 1;
    __asm__ __volatile__("dsb sy\n\tisb\n\t" ::: "memory");

    sme_enter();
    m5_reset_stats_inline();
    run_za_squash_stress(squash_result);
    m5_dump_stats_inline();
    sme_exit();

    const int ok = verify_vec(squash_result, expect);
    printf("Overall: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
