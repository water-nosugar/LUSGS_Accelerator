// SPDX-License-Identifier: BSD-3-Clause
.text
.global sme_stream_bytes
.type sme_stream_bytes,%function
sme_stream_bytes:
    rdsvl x0, #1
    ret
.size sme_stream_bytes, .-sme_stream_bytes

// Base AAPCS64 entry, called from a non-streaming C guest with private ZA.
// x0 = accumulator, x1 = row stride in bytes, x2/x3 = left/right vectors,
// x4/x5 = active rows/columns (1..5), w6 selects subtraction.
.global sme_rank1
.type sme_rank1,%function
sme_rank1:
    stp d8,  d9,  [sp, #-64]!
    stp d10, d11, [sp, #16]
    stp d12, d13, [sp, #32]
    stp d14, d15, [sp, #48]
    smstart
    whilelo p0.d, xzr, x4
    whilelo p1.d, xzr, x5
    ld1d {z0.d}, p0/z, [x2]
    ld1d {z1.d}, p1/z, [x3]
    mov x7, x0
    mov w12, #0
1:
    ld1d {z2.d}, p1/z, [x7]
    mov za0h.d[w12, 0], p1/m, z2.d
    add x7, x7, x1
    add w12, w12, #1
    cmp x12, x4
    b.lo 1b
    cbz w6, 2f
    fmops za0.d, p0/m, p1/m, z0.d, z1.d
    b 3f
2:
    fmopa za0.d, p0/m, p1/m, z0.d, z1.d
3:
    mov w12, #0
4:
    mov z2.d, p1/m, za0h.d[w12, 0]
    st1d {z2.d}, p1, [x0]
    add x0, x0, x1
    add w12, w12, #1
    cmp x12, x4
    b.lo 4b
    smstop
    ldp d10, d11, [sp, #16]
    ldp d12, d13, [sp, #32]
    ldp d14, d15, [sp, #48]
    ldp d8,  d9,  [sp], #64
    ret
.size sme_rank1, .-sme_rank1
.section .note.GNU-stack,"",%progbits
