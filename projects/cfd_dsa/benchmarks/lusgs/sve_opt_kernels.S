// SPDX-License-Identifier: BSD-3-Clause
// Hand-optimized 512-bit Arm SVE kernels for single-line 5x5 block LU-SGS.
// Keeps 5x5 LU factors, 5-column RHS panels, and recurrence vectors resident
// in SVE Z registers with zero intermediate spills and pre-transposed column
// storage for unit-stride warm TRSV and MVM scans.

.text

// Helper macro: transpose 5x5 FP64 matrix from [sp, #0] (row-major)
// to [dst_reg] (column-major: col*5 + row), using x9..x13 and d26..d30.
.macro TRANSPOSE5_SP_TO dst_reg
    // col 0..4
    .irp col, 0, 1, 2, 3, 4
        ldr d26, [sp, #(\col * 8 + 0)]
        ldr d27, [sp, #(\col * 8 + 40)]
        ldr d28, [sp, #(\col * 8 + 80)]
        ldr d29, [sp, #(\col * 8 + 120)]
        ldr d30, [sp, #(\col * 8 + 160)]
        str d26, [\dst_reg, #(\col * 40 + 0)]
        str d27, [\dst_reg, #(\col * 40 + 8)]
        str d28, [\dst_reg, #(\col * 40 + 16)]
        str d29, [\dst_reg, #(\col * 40 + 24)]
        str d30, [\dst_reg, #(\col * 40 + 32)]
    .endr
.endm

// Helper macro: in-register 5-column panel TRSM using LU rows in z0..z4 and
// reciprocals in z5. Input matrix rows are in z16..z20, p0 = vl5.
.macro SVE_TRSM5_INREG
    // Forward substitution: k = 0..4
    dup z21.d, z5.d[0]
    fmul z16.d, p0/m, z16.d, z21.d
    dup z22.d, z1.d[0]; fmls z17.d, p0/m, z16.d, z22.d
    dup z23.d, z2.d[0]; fmls z18.d, p0/m, z16.d, z23.d
    dup z24.d, z3.d[0]; fmls z19.d, p0/m, z16.d, z24.d
    dup z25.d, z4.d[0]; fmls z20.d, p0/m, z16.d, z25.d

    dup z21.d, z5.d[1]
    fmul z17.d, p0/m, z17.d, z21.d
    dup z23.d, z2.d[1]; fmls z18.d, p0/m, z17.d, z23.d
    dup z24.d, z3.d[1]; fmls z19.d, p0/m, z17.d, z24.d
    dup z25.d, z4.d[1]; fmls z20.d, p0/m, z17.d, z25.d

    dup z21.d, z5.d[2]
    fmul z18.d, p0/m, z18.d, z21.d
    dup z24.d, z3.d[2]; fmls z19.d, p0/m, z18.d, z24.d
    dup z25.d, z4.d[2]; fmls z20.d, p0/m, z18.d, z25.d

    dup z21.d, z5.d[3]
    fmul z19.d, p0/m, z19.d, z21.d
    dup z25.d, z4.d[3]; fmls z20.d, p0/m, z19.d, z25.d

    dup z21.d, z5.d[4]
    fmul z20.d, p0/m, z20.d, z21.d

    // Backward substitution: k = 4..1 (unit U)
    dup z22.d, z0.d[4]; fmls z16.d, p0/m, z20.d, z22.d
    dup z23.d, z1.d[4]; fmls z17.d, p0/m, z20.d, z23.d
    dup z24.d, z2.d[4]; fmls z18.d, p0/m, z20.d, z24.d
    dup z25.d, z3.d[4]; fmls z19.d, p0/m, z20.d, z25.d

    dup z22.d, z0.d[3]; fmls z16.d, p0/m, z19.d, z22.d
    dup z23.d, z1.d[3]; fmls z17.d, p0/m, z19.d, z23.d
    dup z24.d, z2.d[3]; fmls z18.d, p0/m, z19.d, z24.d

    dup z22.d, z0.d[2]; fmls z16.d, p0/m, z18.d, z22.d
    dup z23.d, z1.d[2]; fmls z17.d, p0/m, z18.d, z23.d

    dup z22.d, z0.d[1]; fmls z16.d, p0/m, z17.d, z22.d
.endm

// int sve_opt_preprocess_line(
//     size_t cells, int transformed,
//     const double *d_mat, const double *l_mat, const double *u_mat,
//     double *lu_col, double *recip, double *lbar_col, double *ubar_col)
// x0=cells, w1=transformed, x2=d_mat, x3=l_mat, x4=u_mat,
// x5=lu_col, x6=recip, x7=lbar_col, [sp+0]=ubar_col (9th arg on stack!)
.global sve_opt_preprocess_line
.type sve_opt_preprocess_line,%function
sve_opt_preprocess_line:
    ldr x15, [sp]               // 9th arg: ubar_col
    sub sp, sp, #288
    stp x19, x20, [sp, #208]
    stp x21, x22, [sp, #224]
    stp x23, x24, [sp, #240]
    stp x25, x26, [sp, #256]
    stp x27, x28, [sp, #272]

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

    // Predicates:
    // p0 = 0x1f (vl5)
    // p1 = 0x1e (1..4), p2 = 0x1c (2..4), p3 = 0x18 (3..4), p4 = 0x10 (4)
    ptrue p0.d, vl5
    mov x9, #1; whilelo p5.d, xzr, x9; bic p1.b, p0/z, p0.b, p5.b
    mov x9, #2; whilelo p6.d, xzr, x9; bic p2.b, p0/z, p0.b, p6.b
    mov x9, #3; whilelo p7.d, xzr, x9; bic p3.b, p0/z, p0.b, p7.b
    mov x9, #4; whilelo p8.d, xzr, x9; bic p4.b, p0/z, p0.b, p8.b

    fmov d31, #1.0
    mov x14, #0x7ff0
    lsl x14, x14, #48           // exponent mask for NaN/Inf check

.Lprep_loop:
    cmp x28, x19
    b.hs .Lprep_ok

    // Load 5x5 D rows into z0..z4
    mov x9, #40
    ld1d {z0.d}, p0/z, [x21]
    add x10, x21, #40
    ld1d {z1.d}, p0/z, [x10]
    add x10, x21, #80
    ld1d {z2.d}, p0/z, [x10]
    add x10, x21, #120
    ld1d {z3.d}, p0/z, [x10]
    add x10, x21, #160
    ld1d {z4.d}, p0/z, [x10]

    // Pivot k=0: d0 = z0.d[0]
    fmov x11, d0
    and x12, x11, x14
    cmp x12, x14
    b.eq .Lprep_fail
    fcmp d0, #0.0
    b.eq .Lprep_fail
    fdiv d16, d31, d0
    str d16, [x25, #0]
    dup z16.d, z16.d[0]
    fmul z0.d, p1/m, z0.d, z16.d
    dup z17.d, z1.d[0]; fmls z1.d, p1/m, z0.d, z17.d
    dup z18.d, z2.d[0]; fmls z2.d, p1/m, z0.d, z18.d
    dup z19.d, z3.d[0]; fmls z3.d, p1/m, z0.d, z19.d
    dup z20.d, z4.d[0]; fmls z4.d, p1/m, z0.d, z20.d

    // Pivot k=1: z1.d[1]
    dup z16.d, z1.d[1]
    fmov x11, d16
    and x12, x11, x14
    cmp x12, x14
    b.eq .Lprep_fail
    fcmp d16, #0.0
    b.eq .Lprep_fail
    fdiv d16, d31, d16
    str d16, [x25, #8]
    dup z16.d, z16.d[0]
    fmul z1.d, p2/m, z1.d, z16.d
    dup z18.d, z2.d[1]; fmls z2.d, p2/m, z1.d, z18.d
    dup z19.d, z3.d[1]; fmls z3.d, p2/m, z1.d, z19.d
    dup z20.d, z4.d[1]; fmls z4.d, p2/m, z1.d, z20.d

    // Pivot k=2: z2.d[2]
    dup z16.d, z2.d[2]
    fmov x11, d16
    and x12, x11, x14
    cmp x12, x14
    b.eq .Lprep_fail
    fcmp d16, #0.0
    b.eq .Lprep_fail
    fdiv d16, d31, d16
    str d16, [x25, #16]
    dup z16.d, z16.d[0]
    fmul z2.d, p3/m, z2.d, z16.d
    dup z19.d, z3.d[2]; fmls z3.d, p3/m, z2.d, z19.d
    dup z20.d, z4.d[2]; fmls z4.d, p3/m, z2.d, z20.d

    // Pivot k=3: z3.d[3]
    dup z16.d, z3.d[3]
    fmov x11, d16
    and x12, x11, x14
    cmp x12, x14
    b.eq .Lprep_fail
    fcmp d16, #0.0
    b.eq .Lprep_fail
    fdiv d16, d31, d16
    str d16, [x25, #24]
    dup z16.d, z16.d[0]
    fmul z3.d, p4/m, z3.d, z16.d
    dup z20.d, z4.d[3]; fmls z4.d, p4/m, z3.d, z20.d

    // Pivot k=4: z4.d[4]
    dup z16.d, z4.d[4]
    fmov x11, d16
    and x12, x11, x14
    cmp x12, x14
    b.eq .Lprep_fail
    fcmp d16, #0.0
    b.eq .Lprep_fail
    fdiv d16, d31, d16
    str d16, [x25, #32]

    // Store LU rows to [sp] and transpose to lu_col (column-major)
    st1d {z0.d}, p0, [sp]
    add x10, sp, #40;  st1d {z1.d}, p0, [x10]
    add x10, sp, #80;  st1d {z2.d}, p0, [x10]
    add x10, sp, #120; st1d {z3.d}, p0, [x10]
    add x10, sp, #160; st1d {z4.d}, p0, [x10]
    TRANSPOSE5_SP_TO x24

    cbz w20, .Lprep_next        // raw mode: skip Lbar/Ubar

    // Load reciprocals[0..4] into z5 for in-register Lbar/Ubar TRSM
    ld1d {z5.d}, p0/z, [x25]

    // If i > 0: transform L -> lbar_col
    cbz x28, .Lprep_ubar
    ld1d {z16.d}, p0/z, [x22]
    add x10, x22, #40;  ld1d {z17.d}, p0/z, [x10]
    add x10, x22, #80;  ld1d {z18.d}, p0/z, [x10]
    add x10, x22, #120; ld1d {z19.d}, p0/z, [x10]
    add x10, x22, #160; ld1d {z20.d}, p0/z, [x10]
    SVE_TRSM5_INREG
    st1d {z16.d}, p0, [sp]
    add x10, sp, #40;  st1d {z17.d}, p0, [x10]
    add x10, sp, #80;  st1d {z18.d}, p0, [x10]
    add x10, sp, #120; st1d {z19.d}, p0, [x10]
    add x10, sp, #160; st1d {z20.d}, p0, [x10]
    TRANSPOSE5_SP_TO x26

.Lprep_ubar:
    // If i + 1 < cells: transform U -> ubar_col
    add x11, x28, #1
    cmp x11, x19
    b.hs .Lprep_next
    ld1d {z16.d}, p0/z, [x23]
    add x10, x23, #40;  ld1d {z17.d}, p0/z, [x10]
    add x10, x23, #80;  ld1d {z18.d}, p0/z, [x10]
    add x10, x23, #120; ld1d {z19.d}, p0/z, [x10]
    add x10, x23, #160; ld1d {z20.d}, p0/z, [x10]
    SVE_TRSM5_INREG
    st1d {z16.d}, p0, [sp]
    add x10, sp, #40;  st1d {z17.d}, p0, [x10]
    add x10, sp, #80;  st1d {z18.d}, p0, [x10]
    add x10, sp, #120; st1d {z19.d}, p0, [x10]
    add x10, sp, #160; st1d {z20.d}, p0, [x10]
    TRANSPOSE5_SP_TO x27

.Lprep_next:
    add x21, x21, #200
    add x22, x22, #200
    add x23, x23, #200
    add x24, x24, #200
    add x25, x25, #40
    add x26, x26, #200
    add x27, x27, #200
    add x28, x28, #1
    b .Lprep_loop

.Lprep_ok:
    mov w0, #1
    b .Lprep_ret
.Lprep_fail:
    mov w0, #0
.Lprep_ret:
    ldp x19, x20, [sp, #208]
    ldp x21, x22, [sp, #224]
    ldp x23, x24, [sp, #240]
    ldp x25, x26, [sp, #256]
    ldp x27, x28, [sp, #272]
    add sp, sp, #288
    ret
.size sve_opt_preprocess_line, .-sve_opt_preprocess_line


// void sve_opt_sweep_pretransform_line(
//     size_t cells, const double *lu_col, const double *recip,
//     const double *lbar_col, const double *ubar_col,
//     const double *rhs, double *y, double *dq)
// x0=cells, x1=lu_col, x2=recip, x3=lbar_col, x4=ubar_col, x5=rhs, x6=y, x7=dq
.global sve_opt_sweep_pretransform_line
.type sve_opt_sweep_pretransform_line,%function
sve_opt_sweep_pretransform_line:
    // All p0..p15 and z0..z7, z16..z31 are caller-saved in AAPCS64 SVE!
    // Predicates set ONCE for the entire line:
    // p0  = 0x1f (lanes 0..4)
    // p1  = 0x01 (lane 0), p2 = 0x02 (lane 1), p3 = 0x04 (lane 2),
    // p4  = 0x08 (lane 3), p5 = 0x10 (lane 4)
    // p6  = 0x1e (1..4),   p7 = 0x1c (2..4),   p8 = 0x18 (3..4)
    // p9  = 0x0f (0..3),   p10= 0x07 (0..2),   p11= 0x03 (0..1)
    ptrue p0.d, vl5
    mov x9, #1; whilelo p1.d,  xzr, x9     // 0x01
    mov x9, #2; whilelo p12.d, xzr, x9     // 0x03
    mov x9, #3; whilelo p11.d, xzr, x9     // 0x07
    mov x9, #4; whilelo p10.d, xzr, x9     // 0x0f
    bic p2.b, p0/z, p12.b, p1.b            // 0x02
    bic p3.b, p0/z, p11.b, p12.b           // 0x04
    bic p4.b, p0/z, p10.b, p11.b           // 0x08
    bic p5.b, p0/z, p0.b,  p10.b           // 0x10
    bic p6.b, p0/z, p0.b,  p1.b            // 0x1e
    bic p8.b, p0/z, p0.b,  p12.b           // 0x1c
    bic p9.b, p0/z, p0.b,  p11.b           // 0x18

    mov x9, #0                  // i = 0
    mov x11, x6                 // y_ptr = y
.Lsve_fwd_loop:
    cmp x9, x0
    b.hs .Lsve_back_init

    // Load R[i] -> z6, recip[i] -> z5, lu_col[i, 0..4] -> z0..z4
    ld1d {z6.d}, p0/z, [x5]
    ld1d {z5.d}, p0/z, [x2]
    ld1d {z0.d}, p0/z, [x1]
    add x10, x1, #40;  ld1d {z1.d}, p0/z, [x10]
    add x10, x1, #80;  ld1d {z2.d}, p0/z, [x10]
    add x10, x1, #120; ld1d {z3.d}, p0/z, [x10]
    add x10, x1, #160; ld1d {z4.d}, p0/z, [x10]

    // 5-lane cross-row SVE Crout TRSV (exact IEEE-754 order)
    // p0=0x1f; p1=0x01, p2=0x02, p3=0x04, p4=0x08, p5=0x10; p6=0x1e, p7=scratch
    // p8=0x1c, p9=0x18, p10=0x0f, p11=0x07, p12=0x03
    fmul z6.d, p1/m, z6.d, z5.d
    dup z7.d, z6.d[0]
    fmls z6.d, p6/m, z0.d, z7.d

    fmul z6.d, p2/m, z6.d, z5.d
    dup z7.d, z6.d[1]
    mov p7.b, p8.b
    fmls z6.d, p7/m, z1.d, z7.d

    fmul z6.d, p3/m, z6.d, z5.d
    dup z7.d, z6.d[2]
    mov p7.b, p9.b
    fmls z6.d, p7/m, z2.d, z7.d

    fmul z6.d, p4/m, z6.d, z5.d
    dup z7.d, z6.d[3]
    fmls z6.d, p5/m, z3.d, z7.d

    fmul z6.d, p5/m, z6.d, z5.d

    dup z7.d, z6.d[4]
    mov p7.b, p10.b
    fmls z6.d, p7/m, z4.d, z7.d

    dup z7.d, z6.d[3]
    mov p7.b, p11.b
    fmls z6.d, p7/m, z3.d, z7.d

    dup z7.d, z6.d[2]
    mov p7.b, p12.b
    fmls z6.d, p7/m, z2.d, z7.d

    dup z7.d, z6.d[1]
    fmls z6.d, p1/m, z1.d, z7.d

    // If i > 0: y_i = b_i - Lbar_i * y_{i-1} (y_{i-1} resident in z25!)
    cbz x9, .Lsve_fwd_store
    ld1d {z16.d}, p0/z, [x3]
    add x10, x3, #40;  ld1d {z17.d}, p0/z, [x10]
    add x10, x3, #80;  ld1d {z18.d}, p0/z, [x10]
    add x10, x3, #120; ld1d {z19.d}, p0/z, [x10]
    add x10, x3, #160; ld1d {z20.d}, p0/z, [x10]
    dup z21.d, z25.d[0]; fmls z6.d, p0/m, z16.d, z21.d
    dup z22.d, z25.d[1]; fmls z6.d, p0/m, z17.d, z22.d
    dup z23.d, z25.d[2]; fmls z6.d, p0/m, z18.d, z23.d
    dup z24.d, z25.d[3]; fmls z6.d, p0/m, z19.d, z24.d
    dup z21.d, z25.d[4]; fmls z6.d, p0/m, z20.d, z21.d

.Lsve_fwd_store:
    st1d {z6.d}, p0, [x11]
    mov z25.d, z6.d             // carry y_i in register z25
    add x1, x1, #200
    add x2, x2, #40
    add x3, x3, #200
    add x5, x5, #40
    add x11, x11, #40
    add x9, x9, #1
    b .Lsve_fwd_loop

.Lsve_back_init:
    // Store dq[cells - 1] = y[cells - 1] (already in z25!)
    sub x9, x0, #1              // cursor = cells - 1
    mov x12, #40
    mul x13, x9, x12
    add x14, x7, x13            // dq_ptr = &dq[cells - 1]
    st1d {z25.d}, p0, [x14]
    cbz x9, .Lsve_done

    add x11, x6, x13            // y_ptr = &y[cells - 1]
    mov x12, #200
    mul x13, x9, x12
    add x15, x4, x13            // ubar_ptr = &ubar_col[cells - 1]

.Lsve_back_loop:
    sub x11, x11, #40
    sub x14, x14, #40
    sub x15, x15, #200

    ld1d {z6.d}, p0/z, [x11]    // y[i]
    ld1d {z16.d}, p0/z, [x15]
    add x10, x15, #40;  ld1d {z17.d}, p0/z, [x10]
    add x10, x15, #80;  ld1d {z18.d}, p0/z, [x10]
    add x10, x15, #120; ld1d {z19.d}, p0/z, [x10]
    add x10, x15, #160; ld1d {z20.d}, p0/z, [x10]
    dup z21.d, z25.d[0]; fmls z6.d, p0/m, z16.d, z21.d
    dup z22.d, z25.d[1]; fmls z6.d, p0/m, z17.d, z22.d
    dup z23.d, z25.d[2]; fmls z6.d, p0/m, z18.d, z23.d
    dup z24.d, z25.d[3]; fmls z6.d, p0/m, z19.d, z24.d
    dup z21.d, z25.d[4]; fmls z6.d, p0/m, z20.d, z21.d

    st1d {z6.d}, p0, [x14]
    mov z25.d, z6.d             // carry dq_i in register z25
    subs x9, x9, #1
    b.ne .Lsve_back_loop

.Lsve_done:
    ret
.size sve_opt_sweep_pretransform_line, .-sve_opt_sweep_pretransform_line
.section .note.GNU-stack,"",%progbits
