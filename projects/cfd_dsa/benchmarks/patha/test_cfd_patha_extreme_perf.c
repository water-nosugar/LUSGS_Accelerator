/*
 * test_cfd_extreme_perf.c  -- CFD-DSA gem5 Performance Benchmark (v4.0)
 * ================================================================
 * Architecture: ARMv8-A + CFD-DSA Extension
 *
 * Scheme-B benchmark:
 *   - Reuse lmat5_spm row=0 as vector load from SPM
 *   - Ping/Pong matrix and vector are all sourced from SPM
 * ================================================================
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
#define SPM_MATRIX_ROWS 5

#define SPM_BASE 0x70000000ULL
#define SPM_BANK_STRIDE 0x1000ULL

#define SPM_PING_BANK_BASE (SPM_BASE)
#define SPM_PONG_BANK_BASE (SPM_BASE + SPM_BANK_STRIDE)

#define SPM_PING_MAT_BASE (SPM_PING_BANK_BASE)
#define SPM_PING_VEC_BASE (SPM_PING_BANK_BASE + (SPM_MATRIX_ROWS * SPM_STRIDE_BYTES))
#define SPM_PONG_MAT_BASE (SPM_PONG_BANK_BASE)
#define SPM_PONG_VEC_BASE (SPM_PONG_BANK_BASE + (SPM_MATRIX_ROWS * SPM_STRIDE_BYTES))

#define RESULT_STORE_BYTES 64
#define RESULT_PRED_BYTES 40

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

static inline uint32_t
enc_patha_stream_ld5(int slot, int field, int xbase)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (0U << 22) | (0b01U << 20) |
           ((slot & 1) << 19) | ((field & 7) << 16) |
           ((xbase & 0x1f) << 10);
}

static inline uint32_t
enc_patha_dotp_stream(int slot, int lane)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (0U << 22) | (0b10U << 20) |
           ((slot & 1) << 19) | ((lane & 7) << 16);
}

static inline uint32_t
enc_patha_store5_result(int slot, int xbase)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (0U << 22) | (0b11U << 20) |
           ((slot & 1) << 19) | ((xbase & 0x1f) << 10);
}

static inline uint32_t
enc_patha_store_drain(int xbase)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (0U << 22) | (0b11U << 20) |
           (0b1111U << 15) | ((xbase & 0x1f) << 10);
}

static void
ref_matvec5(const double mat[N][N], const double vec[N], double out[N])
{
    for (int i = 0; i < N; i++) {
        double acc = 0.0;
        for (int j = 0; j < N; j++) {
            acc += mat[i][j] * vec[j];
        }
        out[i] = acc;
    }
}

static int
almost_equal5(const double a[N], const double b[N], double tol)
{
    for (int i = 0; i < N; i++) {
        if (fabs(a[i] - b[i]) > tol) {
            return 0;
        }
    }
    return 1;
}

static int
verify(const char *name, const double got[N], const double expect[N], double tol)
{
    int ok = 1;
    for (int i = 0; i < N; i++) {
        if (fabs(got[i] - expect[i]) > tol) {
            printf("  MISMATCH at %s[%d]: got %.6f, expect %.6f\n",
                   name, i, got[i], expect[i]);
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
    for (int i = 0; i < N; i++) {
        printf("%.4f%s", v[i], (i == N - 1) ? "" : ", ");
    }
    printf("]\n");
}

static inline void
gem5_m5_reset_stats(void)
{
    register uint64_t x0 asm("x0") = 0;
    register uint64_t x1 asm("x1") = 0;
    __asm__ __volatile__(".long 0xff400110\n\t"
                         : "+r"(x0), "+r"(x1) :: "memory");
}

static inline void
gem5_m5_dump_stats(void)
{
    register uint64_t x0 asm("x0") = 0;
    register uint64_t x1 asm("x1") = 0;
    __asm__ __volatile__(".long 0xff410110\n\t"
                         : "+r"(x0), "+r"(x1) :: "memory");
}

static void
dma_copy_matrix_to_spm(const double src[N][N], uint64_t spm_base)
{
    volatile double *dst = (volatile double *)(uintptr_t)spm_base;
    for (int r = 0; r < SPM_MATRIX_ROWS; r++) {
        for (int c = 0; c < N; c++) {
            dst[r * SPM_STRIDE_ELEMS + c] = src[r][c];
        }
    }
}

static void
dma_copy_vector_to_spm(const double src[N], uint64_t spm_base)
{
    volatile double *dst = (volatile double *)(uintptr_t)spm_base;
    for (int c = 0; c < N; c++) {
        dst[c] = src[c];
    }
}

__attribute__((noinline)) static void
batch_dsa_cfd_insn_pingpong_spm(double *result_ptr, uint64_t iters)
{
    const uint32_t lp_ping[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1), enc_lmat5_spm(2, 21, 2),
        enc_lmat5_spm(3, 21, 3), enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv_ping = enc_lmat5_spm(0, 23, 5);

    const uint32_t lp_pong[5] = {
        enc_lmat5_spm(0, 22, 6), enc_lmat5_spm(1, 22, 7), enc_lmat5_spm(2, 22, 8),
        enc_lmat5_spm(3, 22, 9), enc_lmat5_spm(4, 22, 10)
    };
    const uint32_t lv_pong = enc_lmat5_spm(0, 24, 12);

    const uint32_t dp_ping[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5), enc_dotp_row(2, 2, 5),
        enc_dotp_row(3, 3, 5), enc_dotp_row(4, 4, 5)
    };
    const uint32_t dp_pong[5] = {
        enc_dotp_row(0, 6, 12), enc_dotp_row(1, 7, 12), enc_dotp_row(2, 8, 12),
        enc_dotp_row(3, 9, 12), enc_dotp_row(4, 10, 12)
    };

    const uint32_t pack_ping = enc_pack_acc(11, 16);
    const uint32_t pack_pong = enc_pack_acc(13, 16);

    __asm__ __volatile__(
        "mov x21, %[ping_mat]\n\t"
        "mov x22, %[pong_mat]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x11, %[r]\n\t"
        "mov x12, %[iters]\n\t"

        /* Prologue: load Ping matrix and Ping vector from SPM. */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        "1:\n\t"
        /* Prefetch Pong matrix + Pong vector from SPM. */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t"
        ".long %[qv]\n\t"

        /* Compute Ping: dotp(Z0..Z4, Z5) -> X16..X20 -> Z11. */
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packp]\n\t"
        "str z11, [x11]\n\t"
        "add x11, x11, #64\n\t"

        /* Prefetch next Ping matrix + Ping vector from SPM. */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        /* Compute Pong: dotp(Z6..Z10, Z12) -> X16..X20 -> Z13. */
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packq]\n\t"
        "str z13, [x11]\n\t"
        "add x11, x11, #64\n\t"

        "subs x12, x12, #2\n\t"
        "b.gt 1b\n\t"
        :
        : [r] "r"((uint64_t)(uintptr_t)result_ptr),
          [iters] "r"(iters),
          [ping_mat] "r"(SPM_PING_MAT_BASE),
          [pong_mat] "r"(SPM_PONG_MAT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [pm0] "n"(lp_ping[0]), [pm1] "n"(lp_ping[1]), [pm2] "n"(lp_ping[2]),
          [pm3] "n"(lp_ping[3]), [pm4] "n"(lp_ping[4]),
          [pv] "n"(lv_ping),
          [qm0] "n"(lp_pong[0]), [qm1] "n"(lp_pong[1]), [qm2] "n"(lp_pong[2]),
          [qm3] "n"(lp_pong[3]), [qm4] "n"(lp_pong[4]),
          [qv] "n"(lv_pong),
          [dpp0] "n"(dp_ping[0]), [dpp1] "n"(dp_ping[1]), [dpp2] "n"(dp_ping[2]),
          [dpp3] "n"(dp_ping[3]), [dpp4] "n"(dp_ping[4]),
          [dpq0] "n"(dp_pong[0]), [dpq1] "n"(dp_pong[1]), [dpq2] "n"(dp_pong[2]),
          [dpq3] "n"(dp_pong[3]), [dpq4] "n"(dp_pong[4]),
          [packp] "n"(pack_ping),
          [packq] "n"(pack_pong)
        : "memory", "cc",
          "x11", "x12",
          "x15", "x16", "x17", "x18", "x19", "x20",
          "x25", "x26", "x27", "x28",
          "x21", "x22", "x23", "x24",
          "z0", "z1", "z2", "z3", "z4", "z5",
          "z6", "z7", "z8", "z9", "z10", "z11", "z12", "z13"
    );
}

__attribute__((noinline)) static void
batch_dsa_cfd_insn_pingpong_spm_pred40(double *result_ptr, uint64_t iters)
{
    const uint32_t lp_ping[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1), enc_lmat5_spm(2, 21, 2),
        enc_lmat5_spm(3, 21, 3), enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv_ping = enc_lmat5_spm(0, 23, 5);

    const uint32_t lp_pong[5] = {
        enc_lmat5_spm(0, 22, 6), enc_lmat5_spm(1, 22, 7), enc_lmat5_spm(2, 22, 8),
        enc_lmat5_spm(3, 22, 9), enc_lmat5_spm(4, 22, 10)
    };
    const uint32_t lv_pong = enc_lmat5_spm(0, 24, 12);

    const uint32_t dp_ping[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5), enc_dotp_row(2, 2, 5),
        enc_dotp_row(3, 3, 5), enc_dotp_row(4, 4, 5)
    };
    const uint32_t dp_pong[5] = {
        enc_dotp_row(0, 6, 12), enc_dotp_row(1, 7, 12), enc_dotp_row(2, 8, 12),
        enc_dotp_row(3, 9, 12), enc_dotp_row(4, 10, 12)
    };

    const uint32_t pack_ping = enc_pack_acc(11, 16);
    const uint32_t pack_pong = enc_pack_acc(13, 16);

    __asm__ __volatile__(
        "mov x21, %[ping_mat]\n\t"
        "mov x22, %[pong_mat]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x11, %[r]\n\t"
        "mov x12, %[iters]\n\t"
        "ptrue p0.d, vl5\n\t"

        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        "1:\n\t"
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t"
        ".long %[qv]\n\t"

        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "add x11, x11, #40\n\t"

        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t"
        "add x11, x11, #40\n\t"

        "subs x12, x12, #2\n\t"
        "b.gt 1b\n\t"
        :
        : [r] "r"((uint64_t)(uintptr_t)result_ptr),
          [iters] "r"(iters),
          [ping_mat] "r"(SPM_PING_MAT_BASE),
          [pong_mat] "r"(SPM_PONG_MAT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [pm0] "n"(lp_ping[0]), [pm1] "n"(lp_ping[1]), [pm2] "n"(lp_ping[2]),
          [pm3] "n"(lp_ping[3]), [pm4] "n"(lp_ping[4]),
          [pv] "n"(lv_ping),
          [qm0] "n"(lp_pong[0]), [qm1] "n"(lp_pong[1]), [qm2] "n"(lp_pong[2]),
          [qm3] "n"(lp_pong[3]), [qm4] "n"(lp_pong[4]),
          [qv] "n"(lv_pong),
          [dpp0] "n"(dp_ping[0]), [dpp1] "n"(dp_ping[1]), [dpp2] "n"(dp_ping[2]),
          [dpp3] "n"(dp_ping[3]), [dpp4] "n"(dp_ping[4]),
          [dpq0] "n"(dp_pong[0]), [dpq1] "n"(dp_pong[1]), [dpq2] "n"(dp_pong[2]),
          [dpq3] "n"(dp_pong[3]), [dpq4] "n"(dp_pong[4]),
          [packp] "n"(pack_ping),
          [packq] "n"(pack_pong)
        : "memory", "cc",
          "x11", "x12",
          "x15", "x16", "x17", "x18", "x19", "x20",
          "x25", "x26", "x27", "x28",
          "x21", "x22", "x23", "x24",
          "p0",
          "z0", "z1", "z2", "z3", "z4", "z5",
          "z6", "z7", "z8", "z9", "z10", "z11", "z12", "z13"
    );
}

__attribute__((noinline)) static void
batch_dsa_cfd_insn_pingpong_spm_pred40_stride64(double *result_ptr, uint64_t iters)
{
    const uint32_t lp_ping[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1), enc_lmat5_spm(2, 21, 2),
        enc_lmat5_spm(3, 21, 3), enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv_ping = enc_lmat5_spm(0, 23, 5);

    const uint32_t lp_pong[5] = {
        enc_lmat5_spm(0, 22, 6), enc_lmat5_spm(1, 22, 7), enc_lmat5_spm(2, 22, 8),
        enc_lmat5_spm(3, 22, 9), enc_lmat5_spm(4, 22, 10)
    };
    const uint32_t lv_pong = enc_lmat5_spm(0, 24, 12);

    const uint32_t dp_ping[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5), enc_dotp_row(2, 2, 5),
        enc_dotp_row(3, 3, 5), enc_dotp_row(4, 4, 5)
    };
    const uint32_t dp_pong[5] = {
        enc_dotp_row(0, 6, 12), enc_dotp_row(1, 7, 12), enc_dotp_row(2, 8, 12),
        enc_dotp_row(3, 9, 12), enc_dotp_row(4, 10, 12)
    };

    const uint32_t pack_ping = enc_pack_acc(11, 16);
    const uint32_t pack_pong = enc_pack_acc(13, 16);

    __asm__ __volatile__(
        "mov x21, %[ping_mat]\n\t"
        "mov x22, %[pong_mat]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x11, %[r]\n\t"
        "mov x12, %[iters]\n\t"
        "ptrue p0.d, vl5\n\t"

        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        "1:\n\t"
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t"
        ".long %[qv]\n\t"

        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "add x11, x11, #64\n\t"

        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t"
        "add x11, x11, #64\n\t"

        "subs x12, x12, #2\n\t"
        "b.gt 1b\n\t"
        :
        : [r] "r"((uint64_t)(uintptr_t)result_ptr),
          [iters] "r"(iters),
          [ping_mat] "r"(SPM_PING_MAT_BASE),
          [pong_mat] "r"(SPM_PONG_MAT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [pm0] "n"(lp_ping[0]), [pm1] "n"(lp_ping[1]), [pm2] "n"(lp_ping[2]),
          [pm3] "n"(lp_ping[3]), [pm4] "n"(lp_ping[4]),
          [pv] "n"(lv_ping),
          [qm0] "n"(lp_pong[0]), [qm1] "n"(lp_pong[1]), [qm2] "n"(lp_pong[2]),
          [qm3] "n"(lp_pong[3]), [qm4] "n"(lp_pong[4]),
          [qv] "n"(lv_pong),
          [dpp0] "n"(dp_ping[0]), [dpp1] "n"(dp_ping[1]), [dpp2] "n"(dp_ping[2]),
          [dpp3] "n"(dp_ping[3]), [dpp4] "n"(dp_ping[4]),
          [dpq0] "n"(dp_pong[0]), [dpq1] "n"(dp_pong[1]), [dpq2] "n"(dp_pong[2]),
          [dpq3] "n"(dp_pong[3]), [dpq4] "n"(dp_pong[4]),
          [packp] "n"(pack_ping),
          [packq] "n"(pack_pong)
        : "memory", "cc",
          "x11", "x12",
          "x15", "x16", "x17", "x18", "x19", "x20",
          "x25", "x26", "x27", "x28",
          "x21", "x22", "x23", "x24",
          "p0",
          "z0", "z1", "z2", "z3", "z4", "z5",
          "z6", "z7", "z8", "z9", "z10", "z11", "z12", "z13"
    );
}

__attribute__((noinline)) static void
batch_dsa_cfd_insn_pingpong_spm_pred40_unroll4(double *result_ptr, uint64_t iters)
{
    const uint32_t lp_ping[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1), enc_lmat5_spm(2, 21, 2),
        enc_lmat5_spm(3, 21, 3), enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv_ping = enc_lmat5_spm(0, 23, 5);

    const uint32_t lp_pong[5] = {
        enc_lmat5_spm(0, 22, 6), enc_lmat5_spm(1, 22, 7), enc_lmat5_spm(2, 22, 8),
        enc_lmat5_spm(3, 22, 9), enc_lmat5_spm(4, 22, 10)
    };
    const uint32_t lv_pong = enc_lmat5_spm(0, 24, 12);

    const uint32_t dp_ping[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5), enc_dotp_row(2, 2, 5),
        enc_dotp_row(3, 3, 5), enc_dotp_row(4, 4, 5)
    };
    const uint32_t dp_pong[5] = {
        enc_dotp_row(0, 6, 12), enc_dotp_row(1, 7, 12), enc_dotp_row(2, 8, 12),
        enc_dotp_row(3, 9, 12), enc_dotp_row(4, 10, 12)
    };

    const uint32_t pack_ping = enc_pack_acc(11, 16);
    const uint32_t pack_pong = enc_pack_acc(13, 16);

    __asm__ __volatile__(
        "mov x21, %[ping_mat]\n\t"
        "mov x22, %[pong_mat]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x11, %[r]\n\t"
        "mov x12, %[iters]\n\t"
        "ptrue p0.d, vl5\n\t"

        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        "1:\n\t"
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t"
        ".long %[qv]\n\t"

        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "add x11, x11, #40\n\t"

        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t"
        "add x11, x11, #40\n\t"

        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t"
        ".long %[qv]\n\t"

        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "add x11, x11, #40\n\t"

        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t"
        ".long %[pv]\n\t"

        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t"
        "add x11, x11, #40\n\t"

        "subs x12, x12, #4\n\t"
        "b.gt 1b\n\t"
        :
        : [r] "r"((uint64_t)(uintptr_t)result_ptr),
          [iters] "r"(iters),
          [ping_mat] "r"(SPM_PING_MAT_BASE),
          [pong_mat] "r"(SPM_PONG_MAT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [pm0] "n"(lp_ping[0]), [pm1] "n"(lp_ping[1]), [pm2] "n"(lp_ping[2]),
          [pm3] "n"(lp_ping[3]), [pm4] "n"(lp_ping[4]),
          [pv] "n"(lv_ping),
          [qm0] "n"(lp_pong[0]), [qm1] "n"(lp_pong[1]), [qm2] "n"(lp_pong[2]),
          [qm3] "n"(lp_pong[3]), [qm4] "n"(lp_pong[4]),
          [qv] "n"(lv_pong),
          [dpp0] "n"(dp_ping[0]), [dpp1] "n"(dp_ping[1]), [dpp2] "n"(dp_ping[2]),
          [dpp3] "n"(dp_ping[3]), [dpp4] "n"(dp_ping[4]),
          [dpq0] "n"(dp_pong[0]), [dpq1] "n"(dp_pong[1]), [dpq2] "n"(dp_pong[2]),
          [dpq3] "n"(dp_pong[3]), [dpq4] "n"(dp_pong[4]),
          [packp] "n"(pack_ping),
          [packq] "n"(pack_pong)
        : "memory", "cc",
          "x11", "x12",
          "x15", "x16", "x17", "x18", "x19", "x20",
          "x25", "x26", "x27", "x28",
          "x21", "x22", "x23", "x24",
          "p0",
          "z0", "z1", "z2", "z3", "z4", "z5",
          "z6", "z7", "z8", "z9", "z10", "z11", "z12", "z13"
    );
}

__attribute__((noinline)) static void
batch_dsa_cfd_insn_pingpong_spm_pred40_unroll8_seq(double *result_ptr,
                                                   uint64_t iters)
{
    const uint32_t lp_ping[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1), enc_lmat5_spm(2, 21, 2),
        enc_lmat5_spm(3, 21, 3), enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv_ping = enc_lmat5_spm(0, 23, 5);

    const uint32_t lp_pong[5] = {
        enc_lmat5_spm(0, 22, 6), enc_lmat5_spm(1, 22, 7), enc_lmat5_spm(2, 22, 8),
        enc_lmat5_spm(3, 22, 9), enc_lmat5_spm(4, 22, 10)
    };
    const uint32_t lv_pong = enc_lmat5_spm(0, 24, 12);

    const uint32_t dp_ping[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5), enc_dotp_row(2, 2, 5),
        enc_dotp_row(3, 3, 5), enc_dotp_row(4, 4, 5)
    };
    const uint32_t dp_pong[5] = {
        enc_dotp_row(0, 6, 12), enc_dotp_row(1, 7, 12), enc_dotp_row(2, 8, 12),
        enc_dotp_row(3, 9, 12), enc_dotp_row(4, 10, 12)
    };

    const uint32_t pack_ping = enc_pack_acc(11, 16);
    const uint32_t pack_pong = enc_pack_acc(13, 16);

    __asm__ __volatile__(
        "mov x21, %[ping_mat]\n\t"
        "mov x22, %[pong_mat]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x11, %[r]\n\t"
        "mov x12, %[iters]\n\t"
        "ptrue p0.d, vl5\n\t"

        "1:\n\t"
        /* 0: Ping */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t" ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        /* 1: Pong */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t" ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        /* 2: Ping */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t" ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        /* 3: Pong */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t" ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        /* 4: Ping */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t" ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        /* 5: Pong */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t" ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        /* 6: Ping */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t" ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        /* 7: Pong */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t" ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        "subs x12, x12, #8\n\t"
        "b.gt 1b\n\t"
        :
        : [r] "r"((uint64_t)(uintptr_t)result_ptr),
          [iters] "r"(iters),
          [ping_mat] "r"(SPM_PING_MAT_BASE),
          [pong_mat] "r"(SPM_PONG_MAT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [pm0] "n"(lp_ping[0]), [pm1] "n"(lp_ping[1]), [pm2] "n"(lp_ping[2]),
          [pm3] "n"(lp_ping[3]), [pm4] "n"(lp_ping[4]), [pv] "n"(lv_ping),
          [qm0] "n"(lp_pong[0]), [qm1] "n"(lp_pong[1]), [qm2] "n"(lp_pong[2]),
          [qm3] "n"(lp_pong[3]), [qm4] "n"(lp_pong[4]), [qv] "n"(lv_pong),
          [dpp0] "n"(dp_ping[0]), [dpp1] "n"(dp_ping[1]), [dpp2] "n"(dp_ping[2]),
          [dpp3] "n"(dp_ping[3]), [dpp4] "n"(dp_ping[4]),
          [dpq0] "n"(dp_pong[0]), [dpq1] "n"(dp_pong[1]), [dpq2] "n"(dp_pong[2]),
          [dpq3] "n"(dp_pong[3]), [dpq4] "n"(dp_pong[4]),
          [packp] "n"(pack_ping), [packq] "n"(pack_pong)
        : "memory", "cc",
          "x11", "x12", "x15", "x16", "x17", "x18", "x19", "x20",
          "x25", "x26", "x27", "x28",
          "x21", "x22", "x23", "x24", "p0",
          "z0", "z1", "z2", "z3", "z4", "z5",
          "z6", "z7", "z8", "z9", "z10", "z11", "z12", "z13"
    );
}

__attribute__((noinline)) static void
batch_dsa_cfd_insn_pingpong_spm_pred40_overlap_unroll8(double *result_ptr,
                                                       uint64_t iters)
{
    const uint32_t lp_ping[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1), enc_lmat5_spm(2, 21, 2),
        enc_lmat5_spm(3, 21, 3), enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv_ping = enc_lmat5_spm(0, 23, 5);

    const uint32_t lp_pong[5] = {
        enc_lmat5_spm(0, 22, 6), enc_lmat5_spm(1, 22, 7), enc_lmat5_spm(2, 22, 8),
        enc_lmat5_spm(3, 22, 9), enc_lmat5_spm(4, 22, 10)
    };
    const uint32_t lv_pong = enc_lmat5_spm(0, 24, 12);

    const uint32_t dp_ping[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5), enc_dotp_row(2, 2, 5),
        enc_dotp_row(3, 3, 5), enc_dotp_row(4, 4, 5)
    };
    const uint32_t dp_pong[5] = {
        enc_dotp_row(0, 6, 12), enc_dotp_row(1, 7, 12), enc_dotp_row(2, 8, 12),
        enc_dotp_row(3, 9, 12), enc_dotp_row(4, 10, 12)
    };

    const uint32_t pack_ping = enc_pack_acc(11, 16);
    const uint32_t pack_pong = enc_pack_acc(13, 16);

    __asm__ __volatile__(
        "mov x21, %[ping_mat]\n\t"
        "mov x22, %[pong_mat]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x11, %[r]\n\t"
        "mov x12, %[iters]\n\t"
        "ptrue p0.d, vl5\n\t"

        "1:\n\t"
        /* M0/M1: fill both Z ping/pong sets before consuming either slot. */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        /* M2: refill ping while pong result remains live. */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        /* M3 */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        /* M4 */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        /* M5 */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        /* M6 */
        ".long %[pm0]\n\t" ".long %[pm1]\n\t" ".long %[pm2]\n\t"
        ".long %[pm3]\n\t" ".long %[pm4]\n\t" ".long %[pv]\n\t"
        ".long %[dpp0]\n\t" ".long %[dpp1]\n\t" ".long %[dpp2]\n\t"
        ".long %[dpp3]\n\t" ".long %[dpp4]\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        /* M7 and drain both live slots. */
        ".long %[qm0]\n\t" ".long %[qm1]\n\t" ".long %[qm2]\n\t"
        ".long %[qm3]\n\t" ".long %[qm4]\n\t" ".long %[qv]\n\t"
        ".long %[dpq0]\n\t" ".long %[dpq1]\n\t" ".long %[dpq2]\n\t"
        ".long %[dpq3]\n\t" ".long %[dpq4]\n\t"
        ".long %[packp]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"
        ".long %[packq]\n\t"
        "st1d {z13.d}, p0, [x11]\n\t" "add x11, x11, #40\n\t"

        "subs x12, x12, #8\n\t"
        "b.gt 1b\n\t"
        :
        : [r] "r"((uint64_t)(uintptr_t)result_ptr),
          [iters] "r"(iters),
          [ping_mat] "r"(SPM_PING_MAT_BASE),
          [pong_mat] "r"(SPM_PONG_MAT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [pm0] "n"(lp_ping[0]), [pm1] "n"(lp_ping[1]), [pm2] "n"(lp_ping[2]),
          [pm3] "n"(lp_ping[3]), [pm4] "n"(lp_ping[4]), [pv] "n"(lv_ping),
          [qm0] "n"(lp_pong[0]), [qm1] "n"(lp_pong[1]), [qm2] "n"(lp_pong[2]),
          [qm3] "n"(lp_pong[3]), [qm4] "n"(lp_pong[4]), [qv] "n"(lv_pong),
          [dpp0] "n"(dp_ping[0]), [dpp1] "n"(dp_ping[1]), [dpp2] "n"(dp_ping[2]),
          [dpp3] "n"(dp_ping[3]), [dpp4] "n"(dp_ping[4]),
          [dpq0] "n"(dp_pong[0]), [dpq1] "n"(dp_pong[1]), [dpq2] "n"(dp_pong[2]),
          [dpq3] "n"(dp_pong[3]), [dpq4] "n"(dp_pong[4]),
          [packp] "n"(pack_ping), [packq] "n"(pack_pong)
        : "memory", "cc",
          "x11", "x12", "x15", "x16", "x17", "x18", "x19", "x20",
          "x25", "x26", "x27", "x28",
          "x21", "x22", "x23", "x24", "p0",
          "z0", "z1", "z2", "z3", "z4", "z5",
          "z6", "z7", "z8", "z9", "z10", "z11", "z12", "z13"
    );
}

__attribute__((noinline)) static void
batch_dsa_cfd_patha_stream_dsa(double *result_ptr, uint64_t iters)
{
    const uint32_t lp_ping[6] = {
        enc_patha_stream_ld5(0, 0, 21), enc_patha_stream_ld5(0, 1, 21),
        enc_patha_stream_ld5(0, 2, 21), enc_patha_stream_ld5(0, 3, 21),
        enc_patha_stream_ld5(0, 4, 21), enc_patha_stream_ld5(0, 5, 23)
    };
    const uint32_t lp_pong[6] = {
        enc_patha_stream_ld5(1, 0, 22), enc_patha_stream_ld5(1, 1, 22),
        enc_patha_stream_ld5(1, 2, 22), enc_patha_stream_ld5(1, 3, 22),
        enc_patha_stream_ld5(1, 4, 22), enc_patha_stream_ld5(1, 5, 24)
    };
    const uint32_t dp_ping[5] = {
        enc_patha_dotp_stream(0, 0), enc_patha_dotp_stream(0, 1),
        enc_patha_dotp_stream(0, 2), enc_patha_dotp_stream(0, 3),
        enc_patha_dotp_stream(0, 4)
    };
    const uint32_t dp_pong[5] = {
        enc_patha_dotp_stream(1, 0), enc_patha_dotp_stream(1, 1),
        enc_patha_dotp_stream(1, 2), enc_patha_dotp_stream(1, 3),
        enc_patha_dotp_stream(1, 4)
    };
    const uint32_t st_ping = enc_patha_store5_result(0, 11);
    const uint32_t st_pong = enc_patha_store5_result(1, 11);

    __asm__ __volatile__(
        "mov x21, %[ping_mat]\n\t"
        "mov x22, %[pong_mat]\n\t"
        "mov x23, %[ping_vec]\n\t"
        "mov x24, %[pong_vec]\n\t"
        "mov x11, %[r]\n\t"
        "mov x12, %[iters]\n\t"

        /*
         * Prologue: vector + row0 are enough before lane0 can enter the dotp
         * engine.  Later rows are filled as the lane chain advances.
         */
        ".long %[plv]\n\t" ".long %[pl0]\n\t"

        "1:\n\t"
        /* M0/M1: compute slot0 while filling slot1, then refill slot0. */
        ".long %[dpp0]\n\t" ".long %[qlv]\n\t"
        ".long %[pl1]\n\t" ".long %[ql0]\n\t"
        ".long %[dpp1]\n\t" ".long %[ql1]\n\t"
        ".long %[pl2]\n\t" ".long %[dpp2]\n\t"
        ".long %[ql2]\n\t" ".long %[pl3]\n\t"
        ".long %[dpp3]\n\t" ".long %[ql3]\n\t"
        ".long %[pl4]\n\t" ".long %[dpp4]\n\t"
        ".long %[stp]\n\t" ".long %[dpq0]\n\t"
        ".long %[plv]\n\t" ".long %[dpq1]\n\t"
        ".long %[pl0]\n\t" ".long %[dpq2]\n\t"
        ".long %[ql4]\n\t" ".long %[dpq3]\n\t"
        ".long %[dpq4]\n\t" ".long %[stq]\n\t"

        /* M2/M3 */
        ".long %[dpp0]\n\t" ".long %[qlv]\n\t"
        ".long %[pl1]\n\t" ".long %[ql0]\n\t"
        ".long %[dpp1]\n\t" ".long %[ql1]\n\t"
        ".long %[pl2]\n\t" ".long %[dpp2]\n\t"
        ".long %[ql2]\n\t" ".long %[pl3]\n\t"
        ".long %[dpp3]\n\t" ".long %[ql3]\n\t"
        ".long %[pl4]\n\t" ".long %[dpp4]\n\t"
        ".long %[stp]\n\t" ".long %[dpq0]\n\t"
        ".long %[plv]\n\t" ".long %[dpq1]\n\t"
        ".long %[pl0]\n\t" ".long %[dpq2]\n\t"
        ".long %[ql4]\n\t" ".long %[dpq3]\n\t"
        ".long %[dpq4]\n\t" ".long %[stq]\n\t"

        /* M4/M5 */
        ".long %[dpp0]\n\t" ".long %[qlv]\n\t"
        ".long %[pl1]\n\t" ".long %[ql0]\n\t"
        ".long %[dpp1]\n\t" ".long %[ql1]\n\t"
        ".long %[pl2]\n\t" ".long %[dpp2]\n\t"
        ".long %[ql2]\n\t" ".long %[pl3]\n\t"
        ".long %[dpp3]\n\t" ".long %[ql3]\n\t"
        ".long %[pl4]\n\t" ".long %[dpp4]\n\t"
        ".long %[stp]\n\t" ".long %[dpq0]\n\t"
        ".long %[plv]\n\t" ".long %[dpq1]\n\t"
        ".long %[pl0]\n\t" ".long %[dpq2]\n\t"
        ".long %[ql4]\n\t" ".long %[dpq3]\n\t"
        ".long %[dpq4]\n\t" ".long %[stq]\n\t"

        /* M6/M7 */
        ".long %[dpp0]\n\t" ".long %[qlv]\n\t"
        ".long %[pl1]\n\t" ".long %[ql0]\n\t"
        ".long %[dpp1]\n\t" ".long %[ql1]\n\t"
        ".long %[pl2]\n\t" ".long %[dpp2]\n\t"
        ".long %[ql2]\n\t" ".long %[pl3]\n\t"
        ".long %[dpp3]\n\t" ".long %[ql3]\n\t"
        ".long %[pl4]\n\t" ".long %[dpp4]\n\t"
        ".long %[stp]\n\t" ".long %[dpq0]\n\t"
        ".long %[plv]\n\t" ".long %[dpq1]\n\t"
        ".long %[pl0]\n\t" ".long %[dpq2]\n\t"
        ".long %[ql4]\n\t" ".long %[dpq3]\n\t"
        ".long %[dpq4]\n\t" ".long %[stq]\n\t"

        "subs x12, x12, #8\n\t"
        "b.gt 1b\n\t"
        ".long 0x01b7ac00\n\t"
        :
        : [r] "r"((uint64_t)(uintptr_t)result_ptr),
          [iters] "r"(iters),
          [ping_mat] "r"(SPM_PING_MAT_BASE),
          [pong_mat] "r"(SPM_PONG_MAT_BASE),
          [ping_vec] "r"(SPM_PING_VEC_BASE),
          [pong_vec] "r"(SPM_PONG_VEC_BASE),
          [pl0] "n"(lp_ping[0]), [pl1] "n"(lp_ping[1]), [pl2] "n"(lp_ping[2]),
          [pl3] "n"(lp_ping[3]), [pl4] "n"(lp_ping[4]), [plv] "n"(lp_ping[5]),
          [ql0] "n"(lp_pong[0]), [ql1] "n"(lp_pong[1]), [ql2] "n"(lp_pong[2]),
          [ql3] "n"(lp_pong[3]), [ql4] "n"(lp_pong[4]), [qlv] "n"(lp_pong[5]),
          [dpp0] "n"(dp_ping[0]), [dpp1] "n"(dp_ping[1]), [dpp2] "n"(dp_ping[2]),
          [dpp3] "n"(dp_ping[3]), [dpp4] "n"(dp_ping[4]),
          [dpq0] "n"(dp_pong[0]), [dpq1] "n"(dp_pong[1]), [dpq2] "n"(dp_pong[2]),
          [dpq3] "n"(dp_pong[3]), [dpq4] "n"(dp_pong[4]),
          [stp] "n"(st_ping), [stq] "n"(st_pong)
        : "memory", "cc",
          "x5", "x6", "x7", "x8", "x9",
          "x10", "x11", "x12", "x13", "x14", "x15",
          "x16", "x17", "x18", "x19", "x20",
          "x21", "x22", "x23", "x24"
    );
}

static double *heap_result;
static void *heap_result_raw;
static size_t heap_result_bytes;

static void
setup_buffers(size_t result_bytes)
{
    heap_result_bytes = result_bytes + CL;
    heap_result_raw = malloc(heap_result_bytes);
    if (!heap_result_raw) {
        fprintf(stderr, "Failed to allocate result buffer (%zu bytes)\n",
                heap_result_bytes);
        exit(1);
    }
    heap_result = (double *)(((uintptr_t)heap_result_raw + CL - 1) & ~(CL - 1));
}

static double *
result_slot(uint64_t index, size_t stride)
{
    return (double *)((uint8_t *)heap_result + index * stride);
}

int
main(int argc, char **argv)
{
    uint64_t bench_iters = DEFAULT_BENCH_ITERS;
    int pass = 1;
    volatile uint64_t barrier = 0;
    int pred40_store = 0;
    int store5_store = 0;
    size_t result_stride = 0;
    unsigned patha_unroll = 2;
    int patha_internal_buffer = 1;
    unsigned patha_buffer_depth = 2;
    int patha_streaming = 0;
    const char *patha_kernel = "baseline";

    if (argc > 1) {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(argv[1], &end, 0);
        if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0) {
            fprintf(stderr, "Invalid benchmark iteration count: %s\n", argv[1]);
            return 1;
        }
        bench_iters = (uint64_t)parsed;
    }
    if (argc > 2) {
        if (strcmp(argv[2], "pred40") == 0) {
            pred40_store = 1;
        } else if (strcmp(argv[2], "store5") == 0) {
            pred40_store = 1;
            store5_store = 1;
        } else if (strcmp(argv[2], "full64") != 0) {
            fprintf(stderr, "Invalid Path A store mode: %s\n", argv[2]);
            return 1;
        }
    }
    result_stride = pred40_store ? RESULT_PRED_BYTES : RESULT_STORE_BYTES;
    if (argc > 3) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[3], &end, 0);
        if (errno != 0 || end == argv[3] || *end != '\0') {
            fprintf(stderr, "Invalid Path A result stride: %s\n", argv[3]);
            return 1;
        }
        result_stride = (size_t)parsed;
    }
    if (pred40_store) {
        if (result_stride != RESULT_PRED_BYTES &&
            result_stride != RESULT_STORE_BYTES) {
            fprintf(stderr, "pred40/store5 result stride must be 40 or 64 bytes\n");
            return 1;
        }
        if (store5_store && result_stride != RESULT_PRED_BYTES) {
            fprintf(stderr, "store5 result stride must be 40 bytes\n");
            return 1;
        }
    } else if (result_stride != RESULT_STORE_BYTES) {
        fprintf(stderr, "full64 result stride must be 64 bytes\n");
        return 1;
    }
    if (argc > 4) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[4], &end, 0);
        if (errno != 0 || end == argv[4] || *end != '\0') {
            fprintf(stderr, "Invalid Path A unroll: %s\n", argv[4]);
            return 1;
        }
        if (parsed != 2 && parsed != 4 && parsed != 8) {
            fprintf(stderr, "Path A unroll must be 2, 4, or 8\n");
            return 1;
        }
        patha_unroll = (unsigned)parsed;
    }
    if (argc > 5) {
        if (strcmp(argv[5], "internal-buffer") == 0) {
            patha_internal_buffer = 1;
        } else if (strcmp(argv[5], "xregs") == 0) {
            patha_internal_buffer = 0;
        } else {
            fprintf(stderr, "Invalid Path A accumulator mode: %s\n", argv[5]);
            return 1;
        }
    }
    if (argc > 6) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[6], &end, 0);
        if (errno != 0 || end == argv[6] || *end != '\0') {
            fprintf(stderr, "Invalid Path A result buffer depth: %s\n", argv[6]);
            return 1;
        }
        if (parsed != 1 && parsed != 2 && parsed != 4) {
            fprintf(stderr, "Path A result buffer depth must be 1, 2, or 4\n");
            return 1;
        }
        patha_buffer_depth = (unsigned)parsed;
    }
    if (argc > 7) {
        if (strcmp(argv[7], "1") == 0 || strcmp(argv[7], "true") == 0 ||
            strcmp(argv[7], "on") == 0) {
            patha_streaming = 1;
        } else if (strcmp(argv[7], "0") == 0 ||
                   strcmp(argv[7], "false") == 0 ||
                   strcmp(argv[7], "off") == 0) {
            patha_streaming = 0;
        } else {
            fprintf(stderr, "Invalid Path A streaming flag: %s\n", argv[7]);
            return 1;
        }
    }
    if (argc > 8) {
        if (strcmp(argv[8], "baseline") != 0 &&
            strcmp(argv[8], "stream-scheduled") != 0 &&
            strcmp(argv[8], "stream-dsa") != 0) {
            fprintf(stderr, "Invalid Path A kernel: %s\n", argv[8]);
            return 1;
        }
        patha_kernel = argv[8];
    }
    const uint64_t align = (patha_unroll == 8) ? 8 :
                           ((patha_unroll == 4) ? 4 : 2);
    if (bench_iters % align) {
        bench_iters += align - (bench_iters % align);
    }

    const size_t result_slots = (bench_iters > 2) ? (size_t)bench_iters : 2;
    setup_buffers(result_slots * result_stride + RESULT_STORE_BYTES);

    const double ping_mat[N][N] = {
        {1.0, 2.0, 3.0, 4.0, 5.0},
        {0.5, 1.5, 2.5, 3.5, 4.5},
        {0.1, 0.2, 0.3, 0.4, 0.5},
        {1.0, 0.0, 1.0, 0.0, 1.0},
        {2.0, 2.0, 2.0, 2.0, 2.0},
    };
    const double pong_mat[N][N] = {
        {1.3, -0.5, 2.2, 0.0, 1.0},
        {0.0, 1.0, 0.0, 1.0, 0.0},
        {2.0, 0.0, 1.0, 0.0, 2.0},
        {0.5, 0.5, 0.5, 0.5, 0.5},
        {-1.0, 1.0, -1.0, 1.0, -1.0},
    };
    const double ping_vec[N] = {1.0, -2.0, 0.5, 3.0, -1.0};
    const double pong_vec[N] = {2.0, 1.5, -0.5, -1.0, 4.0};

    double expect_ping[N];
    double expect_pong[N];
    ref_matvec5(ping_mat, ping_vec, expect_ping);
    ref_matvec5(pong_mat, pong_vec, expect_pong);

    if (almost_equal5(expect_ping, expect_pong, 1e-9)) {
        fprintf(stderr, "Invalid test vectors: ping/pong expected outputs are equal.\n");
        return 1;
    }

    dma_copy_matrix_to_spm(ping_mat, SPM_PING_MAT_BASE);
    dma_copy_vector_to_spm(ping_vec, SPM_PING_VEC_BASE);
    dma_copy_matrix_to_spm(pong_mat, SPM_PONG_MAT_BASE);
    dma_copy_vector_to_spm(pong_vec, SPM_PONG_VEC_BASE);

    printf("========================================================\n");
    printf("  CFD DSA -- PURE PERFORMANCE BENCHMARK (Scheme-B)\n");
    printf("========================================================\n");
    printf("SPM layout per tile: 5x40B matrix + 1x40B vector\n");
    printf("Result store width: %dB (%s)\n",
           pred40_store ? RESULT_PRED_BYTES : RESULT_STORE_BYTES,
           store5_store ? "Path A store5_result, first 5 FP64 lanes" :
           pred40_store ? "predicated st1d, first 5 FP64 lanes"
                        : "str zXX, SVE VL=512-bit");
    printf("Result stride: %zuB (%s layout)\n",
           result_stride,
           (result_stride == RESULT_PRED_BYTES) ? "compact 5-lane result"
                                                : "64B-spaced result");
    printf("Path A unroll: %u%s\n", patha_unroll,
           (patha_unroll == 8) ? " (true unroll8 kernel)" : "");
    printf("Path A accumulator: %s, buffer depth: %u\n",
           patha_internal_buffer ? "internal-buffer" : "xregs",
           patha_buffer_depth);
    printf("Path A streaming: %s, kernel: %s\n",
           patha_streaming ? "on" : "off", patha_kernel);
    print_vec("expect_ping", expect_ping);
    print_vec("expect_pong", expect_pong);
    printf("benchmark iters(requested/aligned) = %lu/%lu\n",
           (unsigned long)((argc > 1) ? strtoull(argv[1], NULL, 0) : DEFAULT_BENCH_ITERS),
           (unsigned long)bench_iters);

    printf("\n--- Correctness: one ping/pong pair ---\n");
    memset(heap_result, 0, heap_result_bytes - CL);
    if (pred40_store && result_stride == RESULT_PRED_BYTES)
        batch_dsa_cfd_insn_pingpong_spm_pred40(heap_result, 2);
    else if (pred40_store)
        batch_dsa_cfd_insn_pingpong_spm_pred40_stride64(heap_result, 2);
    else
        batch_dsa_cfd_insn_pingpong_spm(heap_result, 2);
    pass &= verify("pair_final_should_be_pong",
                   result_slot(1, result_stride), expect_pong, 1e-3);

    printf("\n--- Benchmark: SPM Ping/Pong (%lu iters) ---\n", (unsigned long)bench_iters);
    memset(heap_result, 0, heap_result_bytes - CL);
    gem5_m5_reset_stats();
    if (pred40_store && result_stride == RESULT_PRED_BYTES &&
        patha_unroll == 8 && patha_internal_buffer && patha_buffer_depth >= 2 &&
        patha_streaming && strcmp(patha_kernel, "stream-dsa") == 0)
        batch_dsa_cfd_patha_stream_dsa(heap_result, bench_iters);
    else if (pred40_store && result_stride == RESULT_PRED_BYTES &&
        patha_unroll == 8 && patha_internal_buffer && patha_buffer_depth >= 2 &&
        (patha_streaming || strcmp(patha_kernel, "stream-scheduled") == 0 ||
         strcmp(patha_kernel, "baseline") == 0))
        batch_dsa_cfd_insn_pingpong_spm_pred40_overlap_unroll8(heap_result, bench_iters);
    else if (pred40_store && result_stride == RESULT_PRED_BYTES &&
             patha_unroll == 8)
        batch_dsa_cfd_insn_pingpong_spm_pred40_unroll8_seq(heap_result, bench_iters);
    else if (pred40_store && result_stride == RESULT_PRED_BYTES && patha_unroll >= 4)
        batch_dsa_cfd_insn_pingpong_spm_pred40_unroll4(heap_result, bench_iters);
    else if (pred40_store && result_stride == RESULT_PRED_BYTES)
        batch_dsa_cfd_insn_pingpong_spm_pred40(heap_result, bench_iters);
    else if (pred40_store)
        batch_dsa_cfd_insn_pingpong_spm_pred40_stride64(heap_result, bench_iters);
    else
        batch_dsa_cfd_insn_pingpong_spm(heap_result, bench_iters);
    gem5_m5_dump_stats();
    double *last_result = result_slot(bench_iters - 1, result_stride);
    pass &= verify("benchmark_final_should_be_pong",
                   last_result, expect_pong, 1e-3);
    barrier += (uint64_t)last_result[0];

    printf("\n========================================================\n");
    printf("  Barrier: %lu\n", (unsigned long)barrier);
    printf("  Overall: %s\n", pass ? "PASS" : "FAIL");
    printf("========================================================\n");

    free(heap_result_raw);
    return pass ? 0 : 1;
}
