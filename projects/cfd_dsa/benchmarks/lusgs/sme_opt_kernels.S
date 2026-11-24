// SPDX-License-Identifier: BSD-3-Clause
// Hand-optimized architectural Arm SME streaming-mode + resident-ZA kernels
// for single-line 5x5 block LU-SGS.
// - Enters streaming mode (smstart) ONCE per phase (zero per-op smstart/smstop).
// - Keeps 5x5 LU in za1.d and 5-column RHS panels / scan accumulators in za2.d.
// - Uses architectural vertical ZA slice extracts (za1v/za2v) for zero-copy
//   column-major export and direct LU-to-TRSM column feeding.

.text

// Helper macro: in-ZA 5-column panel TRSM in za2.d using LU columns in za1v.d
// and reciprocals in z5. Predicates: p0=0x1f, p1=0x1e, p2=0x1c, p3=0x18,
// p4=0x10, p9=0x0f, p10=0x07, p11=0x03, p12=0x01.
.macro SME_TRSM5_IN_ZA2
    // Forward substitution: k = 0..4
    mov w12, #0
    mov z0.d, p0/m, za2h.d[w12, 0]
    dup z16.d, z5.d[0]
    fmul z0.d, p0/m, z0.d, z16.d
    mov za2h.d[w12, 0], p0/m, z0.d
    mov z1.d, p1/m, za1v.d[w12, 0]
    fmops za2.d, p1/m, p0/m, z1.d, z0.d

    mov w12, #1
    mov z0.d, p0/m, za2h.d[w12, 0]
    dup z16.d, z5.d[1]
    fmul z0.d, p0/m, z0.d, z16.d
    mov za2h.d[w12, 0], p0/m, z0.d
    mov z1.d, p2/m, za1v.d[w12, 0]
    fmops za2.d, p2/m, p0/m, z1.d, z0.d

    mov w12, #2
    mov z0.d, p0/m, za2h.d[w12, 0]
    dup z16.d, z5.d[2]
    fmul z0.d, p0/m, z0.d, z16.d
    mov za2h.d[w12, 0], p0/m, z0.d
    mov z1.d, p3/m, za1v.d[w12, 0]
    fmops za2.d, p3/m, p0/m, z1.d, z0.d

    mov w12, #3
    mov z0.d, p0/m, za2h.d[w12, 0]
    dup z16.d, z5.d[3]
    fmul z0.d, p0/m, z0.d, z16.d
    mov za2h.d[w12, 0], p0/m, z0.d
    mov z1.d, p4/m, za1v.d[w12, 0]
    fmops za2.d, p4/m, p0/m, z1.d, z0.d

    mov w12, #4
    mov z0.d, p0/m, za2h.d[w12, 0]
    dup z16.d, z5.d[4]
    fmul z0.d, p0/m, z0.d, z16.d
    mov za2h.d[w12, 0], p0/m, z0.d

    // Backward substitution: k = 4..1 (unit U)
    // p5=0x0f, p6=0x07, p7=scratch, p8=0x03, p9=0x01
    mov z1.d, p5/m, za1v.d[w12, 0]
    fmops za2.d, p5/m, p0/m, z1.d, z0.d

    mov w12, #3
    mov z0.d, p0/m, za2h.d[w12, 0]
    mov z1.d, p6/m, za1v.d[w12, 0]
    fmops za2.d, p6/m, p0/m, z1.d, z0.d

    mov w12, #2
    mov z0.d, p0/m, za2h.d[w12, 0]
    mov p7.b, p8.b
    mov z1.d, p7/m, za1v.d[w12, 0]
    fmops za2.d, p7/m, p0/m, z1.d, z0.d

    mov w12, #1
    mov z0.d, p0/m, za2h.d[w12, 0]
    mov p7.b, p9.b
    mov z1.d, p7/m, za1v.d[w12, 0]
    fmops za2.d, p7/m, p0/m, z1.d, z0.d
.endm

// int sme_opt_preprocess_line(
//     size_t cells, int transformed,
//     const double *d_mat, const double *l_mat, const double *u_mat,
//     double *lu_col, double *recip, double *lbar_col, double *ubar_col)
.global sme_opt_preprocess_line
.type sme_opt_preprocess_line,%function
sme_opt_preprocess_line:
    ldr x15, [sp]               // 9th arg: ubar_col
    sub sp, sp, #144
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp d8,  d9,  [sp, #80]
    stp d10, d11, [sp, #96]
    stp d12, d13, [sp, #112]
    stp d14, d15, [sp, #128]

    mov x19, x0                 // cells
    mov w20, w1                 // transformed
    mov x21, x2                 // d_ptr
    mov x22, x3                 // l_ptr
    mov x23, x4                 // u_ptr
    mov x24, x5                 // lu_col_ptr
    mov x25, x6                 // recip_ptr
    mov x26, x7                 // lbar_col_ptr
    mov x27, x15                // ubar_col_ptr
    mov x28, #0                 // i = 0

    smstart

    // Predicates inside streaming mode:
    // p0=0x1f; p9=0x01, p8=0x03, p6=0x07, p5=0x0f
    // p1=0x1e, p2=0x1c, p3=0x18, p4=0x10; p7=scratch
    ptrue p0.d, vl5
    mov x9, #1; whilelo p9.d, xzr, x9
    mov x9, #2; whilelo p8.d, xzr, x9
    mov x9, #3; whilelo p6.d, xzr, x9
    mov x9, #4; whilelo p5.d, xzr, x9
    bic p1.b, p0/z, p0.b, p9.b
    bic p2.b, p0/z, p0.b, p8.b
    bic p3.b, p0/z, p0.b, p6.b
    bic p4.b, p0/z, p0.b, p5.b

    fmov d31, #1.0
    mov x14, #0x7ff0
    lsl x14, x14, #48

.Lsme_prep_loop:
    cmp x28, x19
    b.hs .Lsme_prep_ok

    // Load 5x5 D rows into za1h.d[0..4]
    mov w12, #0; ld1d {z0.d}, p0/z, [x21];       mov za1h.d[w12, 0], p0/m, z0.d
    add x10, x21, #40;  mov w12, #1; ld1d {z0.d}, p0/z, [x10]; mov za1h.d[w12, 0], p0/m, z0.d
    add x10, x21, #80;  mov w12, #2; ld1d {z0.d}, p0/z, [x10]; mov za1h.d[w12, 0], p0/m, z0.d
    add x10, x21, #120; mov w12, #3; ld1d {z0.d}, p0/z, [x10]; mov za1h.d[w12, 0], p0/m, z0.d
    add x10, x21, #160; mov w12, #4; ld1d {z0.d}, p0/z, [x10]; mov za1h.d[w12, 0], p0/m, z0.d

    // In-ZA 5x5 Crout LU factorization in za1.d
    // k = 0
    mov w12, #0
    mov z0.d, p0/m, za1h.d[w12, 0]
    fmov x11, d0
    and x13, x11, x14; cmp x13, x14; b.eq .Lsme_prep_fail
    fcmp d0, #0.0; b.eq .Lsme_prep_fail
    fdiv d16, d31, d0
    str d16, [x25, #0]
    dup z16.d, z16.d[0]
    fmul z0.d, p1/m, z0.d, z16.d
    mov za1h.d[w12, 0], p1/m, z0.d
    mov z1.d, p1/m, za1v.d[w12, 0]
    fmops za1.d, p1/m, p1/m, z1.d, z0.d

    // k = 1
    mov w12, #1
    mov z0.d, p0/m, za1h.d[w12, 0]
    dup z16.d, z0.d[1]
    fmov x11, d16
    and x13, x11, x14; cmp x13, x14; b.eq .Lsme_prep_fail
    fcmp d16, #0.0; b.eq .Lsme_prep_fail
    fdiv d16, d31, d16
    str d16, [x25, #8]
    dup z16.d, z16.d[0]
    fmul z0.d, p2/m, z0.d, z16.d
    mov za1h.d[w12, 0], p2/m, z0.d
    mov z1.d, p2/m, za1v.d[w12, 0]
    fmops za1.d, p2/m, p2/m, z1.d, z0.d

    // k = 2
    mov w12, #2
    mov z0.d, p0/m, za1h.d[w12, 0]
    dup z16.d, z0.d[2]
    fmov x11, d16
    and x13, x11, x14; cmp x13, x14; b.eq .Lsme_prep_fail
    fcmp d16, #0.0; b.eq .Lsme_prep_fail
    fdiv d16, d31, d16
    str d16, [x25, #16]
    dup z16.d, z16.d[0]
    fmul z0.d, p3/m, z0.d, z16.d
    mov za1h.d[w12, 0], p3/m, z0.d
    mov z1.d, p3/m, za1v.d[w12, 0]
    fmops za1.d, p3/m, p3/m, z1.d, z0.d

    // k = 3
    mov w12, #3
    mov z0.d, p0/m, za1h.d[w12, 0]
    dup z16.d, z0.d[3]
    fmov x11, d16
    and x13, x11, x14; cmp x13, x14; b.eq .Lsme_prep_fail
    fcmp d16, #0.0; b.eq .Lsme_prep_fail
    fdiv d16, d31, d16
    str d16, [x25, #24]
    dup z16.d, z16.d[0]
    fmul z0.d, p4/m, z0.d, z16.d
    mov za1h.d[w12, 0], p4/m, z0.d
    mov z1.d, p4/m, za1v.d[w12, 0]
    fmops za1.d, p4/m, p4/m, z1.d, z0.d

    // k = 4
    mov w12, #4
    mov z0.d, p0/m, za1h.d[w12, 0]
    dup z16.d, z0.d[4]
    fmov x11, d16
    and x13, x11, x14; cmp x13, x14; b.eq .Lsme_prep_fail
    fcmp d16, #0.0; b.eq .Lsme_prep_fail
    fdiv d16, d31, d16
    str d16, [x25, #32]

    // Export LU columns directly via vertical ZA reads (zero transpose!)
    mov w12, #0; mov z0.d, p0/m, za1v.d[w12, 0]; st1d {z0.d}, p0, [x24]
    add x10, x24, #40;  mov w12, #1; mov z0.d, p0/m, za1v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x24, #80;  mov w12, #2; mov z0.d, p0/m, za1v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x24, #120; mov w12, #3; mov z0.d, p0/m, za1v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x24, #160; mov w12, #4; mov z0.d, p0/m, za1v.d[w12, 0]; st1d {z0.d}, p0, [x10]

    cbz w20, .Lsme_prep_next

    // Load reciprocals[0..4] into z5
    ld1d {z5.d}, p0/z, [x25]

    // If i > 0: transform L -> lbar_col inside za2.d
    cbz x28, .Lsme_prep_ubar
    mov w12, #0; ld1d {z0.d}, p0/z, [x22];       mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x22, #40;  mov w12, #1; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x22, #80;  mov w12, #2; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x22, #120; mov w12, #3; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x22, #160; mov w12, #4; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    SME_TRSM5_IN_ZA2
    mov w12, #0; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x26]
    add x10, x26, #40;  mov w12, #1; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x26, #80;  mov w12, #2; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x26, #120; mov w12, #3; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x26, #160; mov w12, #4; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]

.Lsme_prep_ubar:
    add x11, x28, #1
    cmp x11, x19
    b.hs .Lsme_prep_next
    mov w12, #0; ld1d {z0.d}, p0/z, [x23];       mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x23, #40;  mov w12, #1; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x23, #80;  mov w12, #2; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x23, #120; mov w12, #3; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    add x10, x23, #160; mov w12, #4; ld1d {z0.d}, p0/z, [x10]; mov za2h.d[w12, 0], p0/m, z0.d
    SME_TRSM5_IN_ZA2
    mov w12, #0; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x27]
    add x10, x27, #40;  mov w12, #1; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x27, #80;  mov w12, #2; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x27, #120; mov w12, #3; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]
    add x10, x27, #160; mov w12, #4; mov z0.d, p0/m, za2v.d[w12, 0]; st1d {z0.d}, p0, [x10]

.Lsme_prep_next:
    add x21, x21, #200
    add x22, x22, #200
    add x23, x23, #200
    add x24, x24, #200
    add x25, x25, #40
    add x26, x26, #200
    add x27, x27, #200
    add x28, x28, #1
    b .Lsme_prep_loop

.Lsme_prep_ok:
    mov w0, #1
    b .Lsme_prep_ret
.Lsme_prep_fail:
    mov w0, #0
.Lsme_prep_ret:
    smstop
    ldp d14, d15, [sp, #128]
    ldp d12, d13, [sp, #112]
    ldp d10, d11, [sp, #96]
    ldp d8,  d9,  [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp, #0]
    add sp, sp, #144
    ret
.size sme_opt_preprocess_line, .-sme_opt_preprocess_line


// void sme_opt_sweep_pretransform_line(
//     size_t cells, const double *lu_col, const double *recip,
//     const double *lbar_col, const double *ubar_col,
//     const double *rhs, double *y, double *dq)
.global sme_opt_sweep_pretransform_line
.type sme_opt_sweep_pretransform_line,%function
sme_opt_sweep_pretransform_line:
    stp d8,  d9,  [sp, #-64]!
    stp d10, d11, [sp, #16]
    stp d12, d13, [sp, #32]
    stp d14, d15, [sp, #48]

    smstart

    // Predicates set ONCE for the entire sweep:
    // p0=0x1f; p1=0x01, p2=0x02, p3=0x04, p4=0x08, p5=0x10; p6=0x1e, p7=scratch
    // p8=0x1c, p9=0x18, p10=0x0f, p11=0x07, p12=0x03
    ptrue p0.d, vl5
    mov x9, #1; whilelo p1.d,  xzr, x9
    mov x9, #2; whilelo p12.d, xzr, x9
    mov x9, #3; whilelo p11.d, xzr, x9
    mov x9, #4; whilelo p10.d, xzr, x9
    bic p2.b, p0/z, p12.b, p1.b
    bic p3.b, p0/z, p11.b, p12.b
    bic p4.b, p0/z, p10.b, p11.b
    bic p5.b, p0/z, p0.b,  p10.b
    bic p6.b, p0/z, p0.b,  p1.b
    bic p8.b, p0/z, p0.b,  p12.b
    bic p9.b, p0/z, p0.b,  p11.b

    mov w12, #0                 // always row 0 of za2h.d
    mov x9, #0                  // i = 0
    mov x11, x6                 // y_ptr = y

.Lsme_fwd_loop:
    cmp x9, x0
    b.hs .Lsme_back_init

    // Load R[i] -> za2h.d[0], recip[i] -> z5, lu_col[i, 0..4] -> z0..z4
    ld1d {z6.d}, p0/z, [x5]
    mov za2h.d[w12, 0], p0/m, z6.d
    ld1d {z5.d}, p0/z, [x2]
    ld1d {z0.d}, p0/z, [x1]
    add x10, x1, #40;  ld1d {z1.d}, p0/z, [x10]
    add x10, x1, #80;  ld1d {z2.d}, p0/z, [x10]
    add x10, x1, #120; ld1d {z3.d}, p0/z, [x10]
    add x10, x1, #160; ld1d {z4.d}, p0/z, [x10]

    // In-ZA 5-lane cross-row Crout TRSV inside za2h.d[0] using FMOPS!
    // k = 0 (z6 already holds R[i]!)
    fmul z6.d, p1/m, z6.d, z5.d
    mov za2h.d[w12, 0], p1/m, z6.d
    dup z7.d, z6.d[0]
    fmops za2.d, p1/m, p6/m, z7.d, z0.d

    // k = 1
    mov z6.d, p2/m, za2h.d[w12, 0]
    fmul z6.d, p2/m, z6.d, z5.d
    mov za2h.d[w12, 0], p2/m, z6.d
    dup z7.d, z6.d[1]
    mov p7.b, p8.b
    fmops za2.d, p1/m, p7/m, z7.d, z1.d

    // k = 2
    mov z6.d, p3/m, za2h.d[w12, 0]
    fmul z6.d, p3/m, z6.d, z5.d
    mov za2h.d[w12, 0], p3/m, z6.d
    dup z7.d, z6.d[2]
    mov p7.b, p9.b
    fmops za2.d, p1/m, p7/m, z7.d, z2.d

    // k = 3
    mov z6.d, p4/m, za2h.d[w12, 0]
    fmul z6.d, p4/m, z6.d, z5.d
    mov za2h.d[w12, 0], p4/m, z6.d
    dup z7.d, z6.d[3]
    fmops za2.d, p1/m, p5/m, z7.d, z3.d

    // k = 4
    mov z6.d, p5/m, za2h.d[w12, 0]
    fmul z6.d, p5/m, z6.d, z5.d
    mov za2h.d[w12, 0], p5/m, z6.d
    dup z7.d, z6.d[4]
    mov p7.b, p10.b
    fmops za2.d, p1/m, p7/m, z7.d, z4.d

    // Backward k = 3..1
    mov z6.d, p4/m, za2h.d[w12, 0]
    dup z7.d, z6.d[3]
    mov p7.b, p11.b
    fmops za2.d, p1/m, p7/m, z7.d, z3.d

    mov z6.d, p3/m, za2h.d[w12, 0]
    dup z7.d, z6.d[2]
    mov p7.b, p12.b
    fmops za2.d, p1/m, p7/m, z7.d, z2.d

    mov z6.d, p2/m, za2h.d[w12, 0]
    dup z7.d, z6.d[1]
    fmops za2.d, p1/m, p1/m, z7.d, z1.d

    // Forward scan directly in za2h.d[0]: y_i = b_i - Lbar_i * y_{i-1}
    cbz x9, .Lsme_fwd_store
    ld1d {z16.d}, p0/z, [x3]
    add x10, x3, #40;  ld1d {z17.d}, p0/z, [x10]
    add x10, x3, #80;  ld1d {z18.d}, p0/z, [x10]
    add x10, x3, #120; ld1d {z19.d}, p0/z, [x10]
    add x10, x3, #160; ld1d {z20.d}, p0/z, [x10]
    dup z21.d, z25.d[0]; fmops za2.d, p1/m, p0/m, z21.d, z16.d
    dup z22.d, z25.d[1]; fmops za2.d, p1/m, p0/m, z22.d, z17.d
    dup z23.d, z25.d[2]; fmops za2.d, p1/m, p0/m, z23.d, z18.d
    dup z24.d, z25.d[3]; fmops za2.d, p1/m, p0/m, z24.d, z19.d
    dup z21.d, z25.d[4]; fmops za2.d, p1/m, p0/m, z21.d, z20.d

.Lsme_fwd_store:
    mov z25.d, p0/m, za2h.d[w12, 0]
    st1d {z25.d}, p0, [x11]
    add x1, x1, #200
    add x2, x2, #40
    add x3, x3, #200
    add x5, x5, #40
    add x11, x11, #40
    add x9, x9, #1
    b .Lsme_fwd_loop

.Lsme_back_init:
    sub x9, x0, #1
    mov x13, #40
    mul x14, x9, x13
    add x15, x7, x14            // dq_ptr = &dq[cells - 1]
    st1d {z25.d}, p0, [x15]
    cbz x9, .Lsme_done

    add x11, x6, x14            // y_ptr = &y[cells - 1]
    mov x13, #200
    mul x14, x9, x13
    add x14, x4, x14            // ubar_ptr = &ubar_col[cells - 1]

.Lsme_back_loop:
    sub x11, x11, #40
    sub x15, x15, #40
    sub x14, x14, #200

    ld1d {z6.d}, p0/z, [x11]
    mov za2h.d[w12, 0], p0/m, z6.d
    ld1d {z16.d}, p0/z, [x14]
    add x10, x14, #40;  ld1d {z17.d}, p0/z, [x10]
    add x10, x14, #80;  ld1d {z18.d}, p0/z, [x10]
    add x10, x14, #120; ld1d {z19.d}, p0/z, [x10]
    add x10, x14, #160; ld1d {z20.d}, p0/z, [x10]
    dup z21.d, z25.d[0]; fmops za2.d, p1/m, p0/m, z21.d, z16.d
    dup z22.d, z25.d[1]; fmops za2.d, p1/m, p0/m, z22.d, z17.d
    dup z23.d, z25.d[2]; fmops za2.d, p1/m, p0/m, z23.d, z18.d
    dup z24.d, z25.d[3]; fmops za2.d, p1/m, p0/m, z24.d, z19.d
    dup z21.d, z25.d[4]; fmops za2.d, p1/m, p0/m, z21.d, z20.d

    mov z25.d, p0/m, za2h.d[w12, 0]
    st1d {z25.d}, p0, [x15]
    subs x9, x9, #1
    b.ne .Lsme_back_loop

.Lsme_done:
    smstop
    ldp d14, d15, [sp, #48]
    ldp d12, d13, [sp, #32]
    ldp d10, d11, [sp, #16]
    ldp d8,  d9,  [sp], #64
    ret
.size sme_opt_sweep_pretransform_line, .-sme_opt_sweep_pretransform_line
.section .note.GNU-stack,"",%progbits
