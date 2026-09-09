#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 5
#define SPM_BASE 0x70000000ULL
#define SPM_TRSV_MAT_BASE (SPM_BASE + 0x3000ULL)
#define SPM_TRSV_RHS_BASE (SPM_TRSV_MAT_BASE + (N * N * sizeof(double)))
#define SPM_TRSM5_LU_BASE (SPM_BASE + 0x3800ULL)
#define SPM_TRSM5_RHS_BASE (SPM_TRSM5_LU_BASE + (N * N * sizeof(double)))
#define SPM_TRSM5_OUT_BASE (SPM_TRSM5_RHS_BASE + (N * N * sizeof(double)))
#define SPM_TRSM5_DUAL_LU_BASE (SPM_BASE + 0x4000ULL)
#define SPM_TRSM5_DUAL_C_BASE \
    (SPM_TRSM5_DUAL_LU_BASE + (N * N * sizeof(double)))
#define SPM_TRSM5_DUAL_DINV_BASE (SPM_BASE + 0x4200ULL)
#define SPM_TRSM5_DUAL_LBAR_BASE \
    (SPM_TRSM5_DUAL_DINV_BASE + (N * N * sizeof(double)))
#define SPM_TRSM5_COEFF3_BASE (SPM_BASE + 0x4800ULL)
#define SPM_TRSM5_COEFF3_LU (SPM_TRSM5_COEFF3_BASE)
#define SPM_TRSM5_COEFF3_L (SPM_TRSM5_COEFF3_BASE + 0x100ULL)
#define SPM_TRSM5_COEFF3_U (SPM_TRSM5_COEFF3_BASE + 0x200ULL)
#define SPM_TRSM5_COEFF3_DI (SPM_TRSM5_COEFF3_BASE + 0x300ULL)
#define SPM_TRSM5_COEFF3_LB (SPM_TRSM5_COEFF3_BASE + 0x400ULL)
#define SPM_TRSM5_COEFF3_UB (SPM_TRSM5_COEFF3_BASE + 0x500ULL)
#define LUSGS_FLAG_WRITE_DQ (1u << 0)
#define LUSGS_FLAG_CHECK_BOUNDS (1u << 2)

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

static inline uint32_t
enc_pack_acc(int zd, int xn)
{
    return (0b00000u << 27) | (0b00u << 25) | (0b1u << 24) |
           (0b1u << 23) | ((xn & 31) << 5) | (zd & 31);
}

static inline uint32_t
enc_pack_sub_acc(int zd, int base, int xn)
{
    return (0b00000u << 27) | (0b00u << 25) | (0b11u << 23) |
           (0b00001u << 15) | ((base & 31) << 10) |
           ((xn & 31) << 5) | (zd & 31);
}

static inline uint32_t
enc_trsv5_lu_spm(int zd, int lu_base, int rhs_base)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           ((lu_base & 31) << 15) | ((rhs_base & 31) << 10) |
           (0u << 5) | (zd & 31);
}

static inline uint32_t
enc_trsv5_lu_spm_zrhs(int zd, int lu_base, int zrhs)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           ((lu_base & 31) << 15) | ((zrhs & 31) << 10) |
           (0b10001u << 5) | (zd & 31);
}

static inline uint32_t
enc_trsm5_mrhs_spm(int lu_base, int rhs_base, int out_base)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           ((lu_base & 31) << 15) | ((rhs_base & 31) << 10) |
           ((out_base & 31) << 5) | 0b11101u;
}

static inline uint32_t
enc_trsm5_inv_lbar_spm(void)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           (21u << 15) | (22u << 10) | (23u << 5) | 24u;
}

static inline uint32_t
enc_trsm5_coeff3_spm(void)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           (20u << 15) | (21u << 10) | (22u << 5) | 23u;
}

static inline uint32_t
enc_vec5_sub_z(int zd, int zsrc0, int zsrc1)
{
    return (0b00000u << 27) | (0b00u << 25) |
           (0b11u << 23) | (0u << 22) | (0b00u << 20) |
           ((zsrc0 & 31) << 15) | ((zsrc1 & 31) << 10) |
           (0b10110u << 5) | (zd & 31);
}

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

static inline uint32_t
enc_coeff_pre_launch(int xtok, int xdesc)
{
    return (0b11u << 23) | (0b11101u << 15) |
           ((xdesc & 31) << 10) | (0b00011u << 5) | (xtok & 31);
}

static inline uint32_t
enc_coeff_pre_wait(int xstatus, int xtok)
{
    return (0b11u << 23) | (0b11100u << 15) |
           ((xtok & 31) << 10) | (0b00100u << 5) | (xstatus & 31);
}

static uint64_t
d2u(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static int
check_close(const double *a, const double *b)
{
    for (int i = 0; i < N; i++) {
        if (memcmp(&a[i], &b[i], sizeof(double)) != 0)
            return 0;
    }
    return 1;
}

static int
test_pack(void)
{
    double out[N] = {};
    double expect[N] = {1.25, -2.5, 3.75, -4.0, 5.5};
#if defined(__aarch64__)
    const uint32_t pack = enc_pack_acc(11, 16);
    register uint64_t x16 asm("x16") = d2u(expect[0]);
    register uint64_t x17 asm("x17") = d2u(expect[1]);
    register uint64_t x18 asm("x18") = d2u(expect[2]);
    register uint64_t x19 asm("x19") = d2u(expect[3]);
    register uint64_t x20 asm("x20") = d2u(expect[4]);
    register uint64_t x11 asm("x11") = (uint64_t)(uintptr_t)out;
    __asm__ __volatile__(
        "ptrue p0.d, vl5\n\t"
        ".long %[pack]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        :
        : "r"(x16), "r"(x17), "r"(x18), "r"(x19), "r"(x20),
          "r"(x11), [pack] "n"(pack)
        : "memory", "cc", "p0", "z11");
#else
    memcpy(out, expect, sizeof(out));
#endif
    return check_close(out, expect);
}

static int
test_pack_sub(void)
{
    double acc[N] = {1.25, -2.5, 3.75, -4.0, 5.5};
    static const double base[N] = {2.0, 1.0, 4.0, -3.0, 8.0};
    static const double expect[N] = {0.75, 3.5, 0.25, 1.0, 2.5};
    double out[N] = {};
#if defined(__aarch64__)
    const uint32_t pack_sub = enc_pack_sub_acc(11, 12, 16);
    register uint64_t x16 asm("x16") = d2u(acc[0]);
    register uint64_t x17 asm("x17") = d2u(acc[1]);
    register uint64_t x18 asm("x18") = d2u(acc[2]);
    register uint64_t x19 asm("x19") = d2u(acc[3]);
    register uint64_t x20 asm("x20") = d2u(acc[4]);
    __asm__ __volatile__(
        "mov x12, %[base]\n\t"
        "mov x11, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[pack_sub]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        :
        : "r"(x16), "r"(x17), "r"(x18), "r"(x19), "r"(x20),
          [base] "r"((uint64_t)(uintptr_t)base),
          [out] "r"((uint64_t)(uintptr_t)out),
          [pack_sub] "n"(pack_sub)
        : "memory", "cc", "x11", "x12", "p0", "z11");
#else
    for (int lane = 0; lane < N; lane++)
        out[lane] = base[lane] - acc[lane];
#endif
    int ok = check_close(out, expect);
    if (!ok) {
        printf("decode.patha_pack_sub5.actual =");
        for (int lane = 0; lane < N; lane++)
            printf(" %.17g", out[lane]);
        printf("\n");
    }
    return ok;
}

static int
test_trsv(void)
{
    double expect[N] = {0.5, 0.25, -0.75, 0.125, 1.0};
    double out[N] = {};
    volatile double *lu = (volatile double *)(uintptr_t)SPM_TRSV_MAT_BASE;
    volatile double *rhs = (volatile double *)(uintptr_t)SPM_TRSV_RHS_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            lu[r * N + c] = (r == c) ? 1.0 : 0.0;
        rhs[r] = expect[r];
    }
#if defined(__aarch64__)
    const uint32_t trsv = enc_trsv5_lu_spm(11, 21, 23);
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x23, %[rhs]\n\t"
        "mov x11, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[trsv]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        :
        : [lu] "r"((uint64_t)SPM_TRSV_MAT_BASE),
          [rhs] "r"((uint64_t)SPM_TRSV_RHS_BASE),
          [out] "r"((uint64_t)(uintptr_t)out),
          [trsv] "n"(trsv)
        : "memory", "cc", "x11", "x21", "x23", "p0", "z11");
#else
    memcpy(out, expect, sizeof(out));
#endif
    return check_close(out, expect);
}

static int
test_trsv_zrhs(void)
{
    static const double expect[N] = {-0.5, 0.75, 1.25, -1.5, 2.0};
    double out[N] = {};
    volatile double *lu = (volatile double *)(uintptr_t)SPM_TRSV_MAT_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            lu[r * N + c] = (r == c) ? 1.0 : 0.0;
    }
#if defined(__aarch64__)
    const uint32_t trsv = enc_trsv5_lu_spm_zrhs(11, 21, 10);
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x10, %[rhs]\n\t"
        "mov x11, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        "ld1d {z10.d}, p0/z, [x10]\n\t"
        ".long %[trsv]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        :
        : [lu] "r"((uint64_t)SPM_TRSV_MAT_BASE),
          [rhs] "r"((uint64_t)(uintptr_t)expect),
          [out] "r"((uint64_t)(uintptr_t)out),
          [trsv] "n"(trsv)
        : "memory", "cc", "x10", "x11", "x21", "p0", "z10", "z11");
#else
    memcpy(out, expect, sizeof(out));
#endif
    return check_close(out, expect);
}

static int
test_trsm5_mrhs(void)
{
    volatile double *lu = (volatile double *)(uintptr_t)SPM_TRSM5_LU_BASE;
    volatile double *rhs = (volatile double *)(uintptr_t)SPM_TRSM5_RHS_BASE;
    volatile double *out = (volatile double *)(uintptr_t)SPM_TRSM5_OUT_BASE;
    double expect[N * N];
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            lu[row * N + col] = row == col ? 1.0 : 0.0;
            expect[row * N + col] =
                0.125 * (double)(1 + row * N + col);
            rhs[row * N + col] = expect[row * N + col];
            out[row * N + col] = 0.0;
        }
    }
#if defined(__aarch64__)
    const uint32_t trsm = enc_trsm5_mrhs_spm(21, 23, 24);
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x23, %[rhs]\n\t"
        "mov x24, %[out]\n\t"
        ".long %[trsm]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [lu] "r"((uint64_t)SPM_TRSM5_LU_BASE),
          [rhs] "r"((uint64_t)SPM_TRSM5_RHS_BASE),
          [out] "r"((uint64_t)SPM_TRSM5_OUT_BASE),
          [trsm] "n"(trsm)
        : "memory", "cc", "x21", "x23", "x24");
#else
    for (int i = 0; i < N * N; i++)
        out[i] = expect[i];
#endif
    for (int i = 0; i < N * N; i++) {
        if (out[i] != expect[i])
            return 0;
    }
    return 1;
}

static int
test_trsm5_inv_lbar(void)
{
    volatile double *lu =
        (volatile double *)(uintptr_t)SPM_TRSM5_DUAL_LU_BASE;
    volatile double *c =
        (volatile double *)(uintptr_t)SPM_TRSM5_DUAL_C_BASE;
    double d_inv[N * N] = {};
    double l_bar[N * N] = {};
    double identity[N * N] = {};
    double expect_c[N * N];
    uint64_t completion_token = 0;
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            const int idx = row * N + col;
            identity[idx] = row == col ? 1.0 : 0.0;
            expect_c[idx] = 0.03125 * (double)(1 + idx);
            lu[idx] = identity[idx];
            c[idx] = expect_c[idx];
        }
    }
#if defined(__aarch64__)
    const uint32_t dual = enc_trsm5_inv_lbar_spm();
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x22, %[c]\n\t"
        "mov x23, %[dinv]\n\t"
        "mov x24, %[lbar]\n\t"
        "mov x26, #41\n\t"
        "dsb st\n\t"
        ".long %[dual]\n\t"
        "mov %[token], x25\n\t"
        : [token] "=&r"(completion_token)
        : [lu] "r"((uint64_t)SPM_TRSM5_DUAL_LU_BASE),
          [c] "r"((uint64_t)SPM_TRSM5_DUAL_C_BASE),
          [dinv] "r"((uint64_t)SPM_TRSM5_DUAL_DINV_BASE),
          [lbar] "r"((uint64_t)SPM_TRSM5_DUAL_LBAR_BASE),
          [dual] "n"(dual)
        : "memory", "cc", "x21", "x22", "x23", "x24", "x25", "x26");
#else
    volatile double *d_fallback =
        (volatile double *)(uintptr_t)SPM_TRSM5_DUAL_DINV_BASE;
    volatile double *l_fallback =
        (volatile double *)(uintptr_t)SPM_TRSM5_DUAL_LBAR_BASE;
    for (int i = 0; i < N * N; i++) {
        d_fallback[i] = identity[i];
        l_fallback[i] = expect_c[i];
    }
    completion_token = 42;
#endif
    const uintptr_t token_offset = (uintptr_t)(completion_token - 42);
    volatile const double *d_out = (volatile const double *)(uintptr_t)
        (SPM_TRSM5_DUAL_DINV_BASE + token_offset);
    volatile const double *l_out = (volatile const double *)(uintptr_t)
        (SPM_TRSM5_DUAL_LBAR_BASE + token_offset);
    for (int i = 0; i < N * N; i++) {
        d_inv[i] = d_out[i];
        l_bar[i] = l_out[i];
    }
    if (completion_token != 42) {
        printf("decode.trsm5_inv_lbar.token = %lu (expected 42)\n",
               (unsigned long)completion_token);
        return 0;
    }
    for (int i = 0; i < N * N; i++) {
        if (d_inv[i] != identity[i] || l_bar[i] != expect_c[i]) {
            printf("decode.trsm5_inv_lbar.first_mismatch = %d "
                   "dinv=%.17g/%.17g lbar=%.17g/%.17g\n",
                   i, d_inv[i], identity[i], l_bar[i], expect_c[i]);
            return 0;
        }
    }
    return 1;
}

static int
test_trsm5_coeff3(void)
{
    volatile double *lu =
        (volatile double *)(uintptr_t)SPM_TRSM5_COEFF3_LU;
    volatile double *lower =
        (volatile double *)(uintptr_t)SPM_TRSM5_COEFF3_L;
    volatile double *upper =
        (volatile double *)(uintptr_t)SPM_TRSM5_COEFF3_U;
    double identity[N * N] = {};
    double expect_l[N * N];
    double expect_u[N * N];
    uint64_t token = 0;
    for (int i = 0; i < N * N; i++) {
        identity[i] = (i / N) == (i % N) ? 1.0 : 0.0;
        expect_l[i] = 0.015625 * (double)(i + 1);
        expect_u[i] = -0.0078125 * (double)(i + 1);
        lu[i] = identity[i];
        lower[i] = expect_l[i];
        upper[i] = expect_u[i];
    }
#if defined(__aarch64__)
    const uint32_t coeff3 = enc_trsm5_coeff3_spm();
    __asm__ __volatile__(
        "mov x10, %[lu]\n\t"
        "mov x11, %[lower]\n\t"
        "mov x12, %[upper]\n\t"
        "mov x13, %[dinv]\n\t"
        "mov x14, %[lbar]\n\t"
        "mov x15, %[ubar]\n\t"
        "mov x26, #73\n\t"
        "dsb st\n\t"
        ".long %[coeff3]\n\t"
        "dsb sy\n\t"
        "mov %[token], x25\n\t"
        : [token] "=&r"(token)
        : [lu] "r"((uint64_t)SPM_TRSM5_COEFF3_LU),
          [lower] "r"((uint64_t)SPM_TRSM5_COEFF3_L),
          [upper] "r"((uint64_t)SPM_TRSM5_COEFF3_U),
          [dinv] "r"((uint64_t)SPM_TRSM5_COEFF3_DI),
          [lbar] "r"((uint64_t)SPM_TRSM5_COEFF3_LB),
          [ubar] "r"((uint64_t)SPM_TRSM5_COEFF3_UB),
          [coeff3] "n"(coeff3)
        : "memory", "cc", "x10", "x11", "x12", "x13", "x14", "x15",
          "x25", "x26");
#else
    volatile double *di_fallback =
        (volatile double *)(uintptr_t)SPM_TRSM5_COEFF3_DI;
    volatile double *lb_fallback =
        (volatile double *)(uintptr_t)SPM_TRSM5_COEFF3_LB;
    volatile double *ub_fallback =
        (volatile double *)(uintptr_t)SPM_TRSM5_COEFF3_UB;
    for (int i = 0; i < N * N; i++) {
        di_fallback[i] = identity[i];
        lb_fallback[i] = expect_l[i];
        ub_fallback[i] = expect_u[i];
    }
    token = 74;
#endif
    uintptr_t offset = token - 74;
    volatile const double *di = (volatile const double *)(uintptr_t)
        (SPM_TRSM5_COEFF3_DI + offset);
    volatile const double *lb = (volatile const double *)(uintptr_t)
        (SPM_TRSM5_COEFF3_LB + offset);
    volatile const double *ub = (volatile const double *)(uintptr_t)
        (SPM_TRSM5_COEFF3_UB + offset);
    if (token != 74) {
        printf("decode.trsm5_coeff3.token = %lu (expected 74)\n",
               (unsigned long)token);
        return 0;
    }
    for (int i = 0; i < N * N; i++) {
        if (di[i] != identity[i] || lb[i] != expect_l[i] ||
            ub[i] != expect_u[i]) {
            printf("decode.trsm5_coeff3.first_mismatch = %d "
                   "dinv=%.17g/%.17g lbar=%.17g/%.17g "
                   "ubar=%.17g/%.17g\n", i, di[i], identity[i],
                   lb[i], expect_l[i], ub[i], expect_u[i]);
            printf("decode.trsm5_coeff3.regions0 = "
                   "lu=%.17g lower=%.17g upper=%.17g "
                   "dinv=%.17g lbar=%.17g ubar=%.17g\n",
                   lu[0], lower[0], upper[0], di[0], lb[0], ub[0]);
            return 0;
        }
    }
    return 1;
}

static int
test_vec5_sub_z(void)
{
    static const double lhs[N] = {3.0, -2.0, 1.5, 8.0, -0.25};
    static const double rhs[N] = {1.0, 4.0, -0.5, 3.0, 0.75};
    static const double expect[N] = {2.0, -6.0, 2.0, 5.0, -1.0};
    double out[N] = {};
#if defined(__aarch64__)
    const uint32_t sub = enc_vec5_sub_z(12, 10, 11);
    __asm__ __volatile__(
        "mov x10, %[lhs]\n\t"
        "mov x11, %[rhs]\n\t"
        "mov x12, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        "ld1d {z10.d}, p0/z, [x10]\n\t"
        "ld1d {z11.d}, p0/z, [x11]\n\t"
        ".long %[sub]\n\t"
        "st1d {z12.d}, p0, [x12]\n\t"
        :
        : [lhs] "r"((uint64_t)(uintptr_t)lhs),
          [rhs] "r"((uint64_t)(uintptr_t)rhs),
          [out] "r"((uint64_t)(uintptr_t)out),
          [sub] "n"(sub)
        : "memory", "cc", "x10", "x11", "x12", "p0",
          "z10", "z11", "z12");
#else
    for (int i = 0; i < N; i++)
        out[i] = lhs[i] - rhs[i];
#endif
    return check_close(out, expect);
}

static int
test_lusgs(void)
{
    double lu[N * N] = {};
    double c[N * N] = {};
    double b[N * N] = {};
    double rhs[N] = {0.7, -0.2, 0.3, -0.4, 0.5};
    double dqstar[N] = {};
    double dq[N] = {};
    for (int i = 0; i < N; i++)
        lu[i * N + i] = 1.0;
    CfdLusgsDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.lu_a_base = (uint64_t)(uintptr_t)lu;
    desc.c_base = (uint64_t)(uintptr_t)c;
    desc.bbar_base = (uint64_t)(uintptr_t)b;
    desc.rhs_base = (uint64_t)(uintptr_t)rhs;
    desc.dqstar_base = (uint64_t)(uintptr_t)dqstar;
    desc.dq_base = (uint64_t)(uintptr_t)dq;
    desc.n_lines = 1;
    desc.n_cells = 1;
    desc.matrix_cell_stride_bytes = N * N * sizeof(double);
    desc.vector_cell_stride_bytes = N * sizeof(double);
    desc.flags = LUSGS_FLAG_WRITE_DQ | LUSGS_FLAG_CHECK_BOUNDS;
    desc.tile_cells = 1;
    desc.omega = 1.0;
#if defined(__aarch64__)
    const uint32_t launch = enc_lusgs_launch(10, 9);
    const uint32_t wait = enc_lusgs_wait(11, 10);
    register uint64_t desc_reg asm("x9") = (uint64_t)(uintptr_t)&desc;
    register uint64_t token_reg asm("x10") = 0;
    register uint64_t status_reg asm("x11") = 0;
    __asm__ __volatile__(
        ".long %[launch]\n\t"
        : "=r"(token_reg)
        : "r"(desc_reg), [launch] "n"(launch)
        : "memory", "cc");
    __asm__ __volatile__(
        ".long %[wait]\n\t"
        : "=r"(status_reg)
        : "r"(token_reg), [wait] "n"(wait)
        : "memory", "cc");
    if (token_reg == 0 || status_reg != 0)
        return 0;
#else
    memcpy(dq, rhs, sizeof(dq));
#endif
    return check_close(dq, rhs);
}

static int
test_coeff_preprocess_control(void)
{
#if defined(__aarch64__)
    const uint32_t launch = enc_coeff_pre_launch(10, 9);
    const uint32_t wait = enc_coeff_pre_wait(11, 10);
    register uint64_t desc_reg asm("x9") = 0;
    register uint64_t token_reg asm("x10") = UINT64_MAX;
    register uint64_t status_reg asm("x11") = 0;
    __asm__ __volatile__(
        ".long %[launch]\n\t"
        : "=r"(token_reg)
        : "r"(desc_reg), [launch] "n"(launch)
        : "memory", "cc");
    __asm__ __volatile__(
        ".long %[wait]\n\t"
        : "=r"(status_reg)
        : "r"(token_reg), [wait] "n"(wait)
        : "memory", "cc");
    return token_reg == 0 && status_reg == 5;
#else
    return 1;
#endif
}

int
main(void)
{
    int pack_ok = test_pack();
    int pack_sub_ok = test_pack_sub();
    int trsv_ok = test_trsv();
    int trsv_zrhs_ok = test_trsv_zrhs();
    int trsm5_mrhs_ok = test_trsm5_mrhs();
    int trsm5_inv_lbar_ok = test_trsm5_inv_lbar();
    int trsm5_coeff3_ok = test_trsm5_coeff3();
    int vec5_sub_ok = test_vec5_sub_z();
    int lusgs_ok = test_lusgs();
    int coeff_pre_ctrl_ok = test_coeff_preprocess_control();
    printf("decode.pack_acc = %s\n", pack_ok ? "PASS" : "FAIL");
    printf("decode.patha_pack_sub5 = %s\n",
           pack_sub_ok ? "PASS" : "FAIL");
    printf("decode.trsv5 = %s\n", trsv_ok ? "PASS" : "FAIL");
    printf("decode.trsv5_lu_spm_zrhs = %s\n",
           trsv_zrhs_ok ? "PASS" : "FAIL");
    printf("decode.trsm5_mrhs_spm = %s\n",
           trsm5_mrhs_ok ? "PASS" : "FAIL");
    printf("decode.trsm5_inv_lbar_spm = %s\n",
           trsm5_inv_lbar_ok ? "PASS" : "FAIL");
    printf("decode.trsm5_coeff3_spm = %s\n",
           trsm5_coeff3_ok ? "PASS" : "FAIL");
    printf("decode.vec5_sub_z = %s\n", vec5_sub_ok ? "PASS" : "FAIL");
    printf("decode.lusgs_step3 = %s\n", lusgs_ok ? "PASS" : "FAIL");
    printf("decode.coeff_preprocess_control = %s\n",
           coeff_pre_ctrl_ok ? "PASS" : "FAIL");
    printf("%s\n", (pack_ok && pack_sub_ok && trsv_ok && trsv_zrhs_ok &&
                    trsm5_mrhs_ok && trsm5_inv_lbar_ok && trsm5_coeff3_ok &&
                    vec5_sub_ok && lusgs_ok && coeff_pre_ctrl_ok) ?
           "DECODE_EXCLUSIVE_PASS" : "DECODE_EXCLUSIVE_FAIL");
    return (pack_ok && pack_sub_ok && trsv_ok && trsv_zrhs_ok &&
            trsm5_mrhs_ok && trsm5_inv_lbar_ok && trsm5_coeff3_ok &&
            vec5_sub_ok && lusgs_ok && coeff_pre_ctrl_ok) ?
           0 : 1;
}
