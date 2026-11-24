// SPDX-License-Identifier: BSD-3-Clause
// Freestanding AArch64 check of the warm 5x5 mapping using architectural ZA.
// The Crout factors give D = L * U, with unit diagonal in U.
.text
.global _start
_start:
    adr x0, factors
    adr x1, inv_diagonal
    adr x2, rhs
    smstart
    cntd x3
    cmp x3, #5
    b.lo failed
    mov x4, #5
    whilelo p0.d, xzr, x4
    ptrue p1.d, vl1
    zero {za}
    mov w12, #0
    ld1d {z0.d}, p0/z, [x2]
    mov za0h.d[w12, 0], p0/m, z0.d
    ld1d {z6.d}, p0/z, [x1]
    index z5.d, #0, #1

    // A contiguous source row enters a vertical slice; horizontal slice k
    // then reads factor[r,k] in lane r.
    mov w13, #0
load_factors:
    ld1d {z0.d}, p0/z, [x0]
    mov za1v.d[w13, 0], p0/m, z0.d
    add x0, x0, #40
    add w13, w13, #1
    cmp w13, #5
    b.lo load_factors

    mov w13, #0
forward:
    dup z4.d, x13
    cmpeq p2.d, p0/z, z5.d, z4.d
    mov z0.d, p0/m, za0h.d[w12, 0]
    fmul z0.d, p2/m, z0.d, z6.d
    mov za0h.d[w12, 0], p2/m, z0.d
    cmp w13, #4
    b.eq backward
    cmphi p2.d, p0/z, z5.d, z4.d
    bl update_rows
    add w13, w13, #1
    b forward

backward:
    dup z4.d, x13
    cmphi p2.d, p0/z, z4.d, z5.d
    bl update_rows
    subs w13, w13, #1
    b.ne backward
    mov z0.d, p0/m, za0h.d[w12, 0]
    st1d {z0.d}, p0, [x2]
    smstop

    // Compare all result bits, including the sentinel beyond the predicate.
    adr x0, expected
    mov x3, #6
check:
    ldr x4, [x0], #8
    ldr x5, [x2], #8
    cmp x4, x5
    b.ne failed
    subs x3, x3, #1
    b.ne check
    mov x0, #1
    adr x1, pass_message
    mov x2, #(pass_end - pass_message)
    mov x8, #64
    svc #0
    mov x0, #0
    b exit_guest
failed:
    mov x0, #1
exit_guest:
    mov x8, #93
    svc #0

update_rows:
    mov z1.d, p1/m, za0v.d[w13, 0]
    mov z2.d, p0/m, za1h.d[w13, 0]
    fmops za0.d, p1/m, p2/m, z1.d, z2.d
    ret

.section .rodata
.balign 8
factors:
    .double  2,  1, -1,  2,  1
    .double  1,  4,  2, -1,  3
    .double -2,  1,  1,  2, -1
    .double  3, -1,  2,  2,  1
    .double  2,  1, -3,  1,  4
inv_diagonal:
    .double 0.5, 0.25, 1, 0.5, 0.25
expected:
    .double 1, -2, 3, -4, 5, 12345
pass_message:
    .ascii "SME_VERTICAL_RHS_ISA_PASS\n"
pass_end:
.data
.balign 8
rhs:
    .double -14, 85, 27, -62, 60, 12345
