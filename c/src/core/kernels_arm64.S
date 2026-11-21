/*
 * Paint (C port) - the kernels of kernels.h for 64-bit ARM (Apple silicon)
 *
 * Each does what its C in kernels.c does, to the byte (tests/test_kernels.c
 * checks). Apple's calling convention: arguments in x0-x7, x9-x17 free to use,
 * x18 not to be touched, x19-x28 and the low halves of v8-v15 kept.
 */
#define SYM(name) _##name

    .text
    .arch_extension crc

/* ---- crc32: w0 crc, x1 p, x2 n ---- */

    .globl SYM(k_crc32)
    .p2align 2
SYM(k_crc32):
    mvn     w0, w0
    subs    x2, x2, #8
    b.lo    2f
1:  ldr     x3, [x1], #8
    crc32x  w0, w0, x3
    subs    x2, x2, #8
    b.hs    1b
2:  adds    x2, x2, #8                  // 0-7 bytes left
    b.eq    4f
3:  ldrb    w3, [x1], #1
    crc32b  w0, w0, w3
    subs    x2, x2, #1
    b.ne    3b
4:  mvn     w0, w0
    ret

/* ---- adler32: w0 adler, x1 p, x2 n ----
 * 16 bytes at a time, up to 5552 (when the sums could next overflow): over L bytes
 * x[0..L), a += sum x, and b += L a + sum (L - j) x[j], which for 16s c is
 * 16 x (sum of the 16s' sums before each) + sum of (16 - i) x[16c + i]. */

    .globl SYM(k_adler32)
    .p2align 2
SYM(k_adler32):
    and     w3, w0, #0xffff             // a
    lsr     w4, w0, #16                 // b
    mov     w9, #65521
    adr     x10, 9f
    ldr     q7, [x10]                   // 16, 15, ..., 1
1:  lsr     x5, x2, #4                  // 16s left
    cbz     x5, 3f
    mov     x6, #347
    cmp     x5, x6
    csel    x5, x5, x6, lo
    sub     x2, x2, x5, lsl #4
    lsl     x6, x5, #4
    madd    x4, x6, x3, x4              // b += L a
    movi    v0.4s, #0                   // byte sums
    movi    v1.4s, #0                   // sums of them before each 16
    movi    v2.4s, #0                   // weighted sums
2:  ldr     q3, [x1], #16
    add     v1.4s, v1.4s, v0.4s
    uaddlp  v4.8h, v3.16b
    uadalp  v0.4s, v4.8h
    umull   v4.8h, v3.8b, v7.8b
    umull2  v5.8h, v3.16b, v7.16b
    uadalp  v2.4s, v4.8h
    uadalp  v2.4s, v5.8h
    subs    x5, x5, #1
    b.ne    2b
    addv    s0, v0.4s
    addv    s1, v1.4s
    addv    s2, v2.4s
    fmov    w5, s0
    add     x3, x3, x5
    fmov    w5, s1
    add     x4, x4, x5, lsl #4
    fmov    w5, s2
    add     x4, x4, x5
    udiv    x5, x3, x9
    msub    x3, x5, x9, x3
    udiv    x5, x4, x9
    msub    x4, x5, x9, x4
    b       1b
3:  cbz     x2, 5f                      // fewer than 16 left
4:  ldrb    w5, [x1], #1
    add     w3, w3, w5
    add     w4, w4, w3
    subs    x2, x2, #1
    b.ne    4b
    udiv    w5, w3, w9
    msub    w3, w5, w9, w3
    udiv    w5, w4, w9
    msub    w4, w5, w9, w4
5:  orr     w0, w3, w4, lsl #16
    ret
    .p2align 4
9:  .byte   16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1

/* ---- PNG rows ----
 * One loop for both: 16 bytes at a time, all five filters of them (v0 none, v4 sub,
 * v5 up, v6 average, v7 Paeth); costs add up |byte| of each, filter stores one.
 * x0 out (0 for costs), x1 row, x2 prev, x3 n, w4 filter, x5 cost */

    .globl SYM(k_png_costs)
    .p2align 2
SYM(k_png_costs):                       // x0 row, x1 prev, x2 n, x3 cost
    mov     x5, x3
    mov     x3, x2
    mov     x2, x1
    mov     x1, x0
    mov     x0, #0
    b       0f
    .globl SYM(k_png_filter)
    .p2align 2
SYM(k_png_filter):                      // x0 out, x1 row, x2 prev, x3 n, w4 filter
0:  sub     sp, sp, #128                // [0, 32) the last bytes, [32, 112) the five filtered
    movi    v16.4s, #0
    movi    v17.4s, #0
    movi    v18.4s, #0
    movi    v19.4s, #0
    movi    v20.4s, #0
    movi    v21.16b, #0                 // the 16 bytes before, of the row (none: zeros)
    movi    v22.16b, #0                 // and of the row above
    adr     x9, 9f
    ldr     q31, [x9]                   // 0, 1, ..., 15
1:  cbz     x3, 8f
    mov     x10, #16                    // bytes in this 16
    cmp     x3, #16
    b.lo    2f
    ldr     q0, [x1], #16
    ldr     q1, [x2], #16
    sub     x3, x3, #16
    b       3f
2:  mov     x10, x3                     // the last 4, 8 or 12: zeros after them
    stp     xzr, xzr, [sp]
    stp     xzr, xzr, [sp, #16]
    add     x13, sp, #16
    mov     x11, #0
21: ldr     w12, [x1, x11]
    str     w12, [sp, x11]
    ldr     w12, [x2, x11]
    str     w12, [x13, x11]
    add     x11, x11, #4
    cmp     x11, x3
    b.lo    21b
    ldp     q0, q1, [sp]
    mov     x3, #0
3:  ext     v2.16b, v21.16b, v0.16b, #12    // a: the pixel to the left
    ext     v3.16b, v22.16b, v1.16b, #12    // c: above that
    mov     v21.16b, v0.16b
    mov     v22.16b, v1.16b
    sub     v4.16b, v0.16b, v2.16b
    sub     v5.16b, v0.16b, v1.16b
    uhadd   v6.16b, v2.16b, v1.16b
    sub     v6.16b, v0.16b, v6.16b
    uabd    v23.16b, v1.16b, v3.16b         // pa = |b - c|
    uabd    v24.16b, v2.16b, v3.16b         // pb = |a - c|
    uaddl   v25.8h, v2.8b, v1.8b
    uaddl2  v26.8h, v2.16b, v1.16b
    ushll   v27.8h, v3.8b, #1
    ushll2  v28.8h, v3.16b, #1
    uabd    v25.8h, v25.8h, v27.8h          // pc = |a + b - 2c|,
    uabd    v26.8h, v26.8h, v28.8h
    uqxtn   v25.8b, v25.8h                  // which over 255 only needs to be more than pa and pb
    uqxtn2  v25.16b, v26.8h
    cmhs    v26.16b, v24.16b, v23.16b
    cmhs    v27.16b, v25.16b, v23.16b
    and     v26.16b, v26.16b, v27.16b       // pa <= pb and pa <= pc
    cmhs    v27.16b, v25.16b, v24.16b       // pb <= pc
    bsl     v27.16b, v1.16b, v3.16b
    bit     v27.16b, v2.16b, v26.16b
    sub     v7.16b, v0.16b, v27.16b
    cbz     x0, 5f
    stp     q0, q4, [sp, #32]
    stp     q5, q6, [sp, #64]
    str     q7, [sp, #96]
    add     x11, sp, #32
    ldr     q2, [x11, w4, uxtw #4]
    cmp     x10, #16
    b.lo    4f
    str     q2, [x0], #16
    b       1b
4:  str     q2, [sp]
    mov     x11, #0
41: ldr     w12, [sp, x11]
    str     w12, [x0, x11]
    add     x11, x11, #4
    cmp     x11, x10
    b.lo    41b
    b       1b
.macro COST v, acc                      // |byte| as signed, the bytes past the row left out
    neg     v27.16b, \v\().16b
    umin    \v\().16b, \v\().16b, v27.16b
    and     \v\().16b, \v\().16b, v26.16b
    uaddlp  v27.8h, \v\().16b
    uadalp  \acc\().4s, v27.8h
.endm
5:  dup     v26.16b, w10
    cmhi    v26.16b, v26.16b, v31.16b
    COST    v0, v16
    COST    v4, v17
    COST    v5, v18
    COST    v6, v19
    COST    v7, v20
    b       1b
8:  cbnz    x0, 81f
    addv    s16, v16.4s
    addv    s17, v17.4s
    addv    s18, v18.4s
    addv    s19, v19.4s
    addv    s20, v20.4s
    stp     s16, s17, [x5]
    stp     s18, s19, [x5, #8]
    str     s20, [x5, #16]
81: add     sp, sp, #128
    ret
    .p2align 4
9:  .byte   0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15

/* ---- unfilter: x0 cur, x1 raw, x2 prior, x3 bpp, x4 n, w5 filter ----
 * A byte at a time (a byte depends on the one bpp before), as stb does; "up" 16 at a time */

    .globl SYM(k_unfilter)
    .p2align 2
SYM(k_unfilter):
    mov     x6, x4
    cbnz    w5, 1f
    mov     x2, x4
    b       _memcpy
1:  cmp     w5, #2
    b.ne    3f
2:  subs    x6, x6, #16                 // up
    b.lo    21f
    ldr     q0, [x1], #16
    ldr     q1, [x2], #16
    add     v0.16b, v0.16b, v1.16b
    str     q0, [x0], #16
    b       2b
21: adds    x6, x6, #16
    b.eq    9f
22: ldrb    w9, [x1], #1
    ldrb    w10, [x2], #1
    add     w9, w9, w10
    strb    w9, [x0], #1
    subs    x6, x6, #1
    b.ne    22b
    ret
3:  mov     x6, #0                      // k
    sub     x7, x0, x3                  // a = x7[k], c = x8[k]
    sub     x8, x2, x3
4:  cmp     x6, x4
    b.hs    9f
    ldrb    w9, [x1, x6]                // x
    ldrb    w10, [x2, x6]               // b
    mov     w11, #0                     // a
    mov     w12, #0                     // c
    cmp     x6, x3
    b.lo    5f
    ldrb    w11, [x7, x6]
    ldrb    w12, [x8, x6]
5:  mov     w13, w11                    // 1: sub
    cmp     w5, #3
    b.lo    8f
    add     w14, w11, w10
    lsr     w13, w14, #1                // 3: average
    b.eq    8f
    lsr     w13, w11, #1                // 5: average on the first row
    cmp     w5, #4
    b.ne    8f
    add     w15, w12, w12, lsl #1       // 4: Paeth, as stb computes it
    sub     w15, w15, w14               // 3c - (a + b)
    cmp     w11, w10
    csel    w16, w11, w10, lo           // lo
    csel    w17, w10, w11, lo           // hi
    cmp     w17, w15
    csel    w13, w16, w12, le
    cmp     w15, w16
    csel    w13, w17, w13, le
8:  add     w9, w9, w13
    strb    w9, [x0, x6]
    add     x6, x6, #1
    b       4b
9:  ret

/* ---- inflate: x0 the k_inflate_t ----
 * x1 in, x2 in_end - 16, x3 out, x4 out_end - 274, x5 out_start, x6 x7 the length and
 * distance tables, x8 bits (nb of them, 56 or more after a refill), w10 held: what stb
 * would hold; x17 the return address */

.macro REFILL                           // 8 bytes in; nb |= 56
    ldr     x12, [x1]
    lsl     x12, x12, x9
    orr     x8, x8, x12
    eor     w12, w9, #63
    add     x1, x1, x12, lsr #3
    orr     w9, w9, #56
.endm

    .globl SYM(k_inflate)
    .p2align 2
SYM(k_inflate):
    mov     x17, x30
    ldp     x1, x2, [x0]
    sub     x2, x2, #16
    ldp     x3, x5, [x0, #16]
    ldr     x4, [x0, #32]
    sub     x4, x4, #274
    ldp     x6, x7, [x0, #40]
    ldp     w8, w9, [x0, #56]
    mov     w10, w9
1:  cmp     x1, x2
    b.hi    97f                         // near the end of the input
    cmp     x3, x4
    b.hi    97f                         // or of the output
    REFILL
    mov     x15, x6
    bl      20f
    tbnz    w12, #31, 98f
    cmp     w12, #256
    b.hi    2f
    b.eq    96f
    strb    w12, [x3], #1
    b       1b
2:  sub     w12, w12, #257              // a length
    cmp     w12, #29
    b.hs    98f
    add     w13, w12, #3
    cmp     w12, #8
    b.lo    3f
    mov     w13, #258
    cmp     w12, #28
    b.eq    3f
    lsr     w14, w12, #2
    sub     w14, w14, #1                // extra bits
    and     w13, w12, #3
    add     w13, w13, #4
    lsl     w13, w13, w14
    add     w13, w13, #3
    bl      30f
    add     w13, w13, w16
3:  REFILL
    mov     x15, x7
    bl      20f
    tbnz    w12, #31, 98f
    cmp     w12, #30
    b.hs    98f
    add     w11, w12, #1                // the distance
    cmp     w12, #4
    b.lo    4f
    lsr     w14, w12, #1
    sub     w14, w14, #1
    and     w11, w12, #1
    add     w11, w11, #2
    lsl     w11, w11, w14
    add     w11, w11, #1
    bl      30f
    add     w11, w11, w16
4:  sub     x12, x3, x5
    cmp     x12, x11
    b.lo    99f
    sub     x12, x3, x11                // copy from
    add     x13, x3, x13                // to here (up to 15 more are written)
    cmp     w11, #16
    b.lo    5f
41: ldr     q0, [x12], #16
    str     q0, [x3], #16
    cmp     x3, x13
    b.lo    41b
    b       8f
5:  cmp     w11, #8
    b.lo    6f
51: ldr     x14, [x12], #8
    str     x14, [x3], #8
    cmp     x3, x13
    b.lo    51b
    b       8f
6:  cmp     w11, #1
    b.ne    7f
    ld1r    {v0.16b}, [x12]             // a run of one byte
61: str     q0, [x3], #16
    cmp     x3, x13
    b.lo    61b
    b       8f
7:  ldrb    w14, [x12], #1
    strb    w14, [x3], #1
    cmp     x3, x13
    b.lo    7b
8:  mov     x3, x13
    b       1b

    /* a symbol from table x15, as stbi__zhuffman_decode: w12 (-1: a bad code); uses x14-x16 */
20: mov     w14, #32                    // stb refills when it holds fewer than 16
    sub     w14, w14, w10
    and     w14, w14, #0xfffffff8
    cmp     w10, #16
    csel    w14, w14, wzr, lt
    add     w10, w10, w14
    and     w12, w8, #511
    ldrh    w12, [x15, x12, lsl #1]
    cbz     w12, 22f
    lsr     w16, w12, #9
    and     w12, w12, #511
21: lsr     x8, x8, x16
    sub     w9, w9, w16
    sub     w10, w10, w16
    ret
22: rbit    w14, w8                     // longer than 9 bits: stb's slow way
    lsr     w14, w14, #16
    mov     w16, #10
23: add     x12, x15, x16, lsl #2
    ldr     w12, [x12, #1056]           // maxcode[s]
    cmp     w14, w12
    b.lt    24f
    add     w16, w16, #1
    b       23b
24: cmp     w16, #16
    b.hs    25f
    mov     w12, #16
    sub     w12, w12, w16
    lsr     w14, w14, w12
    add     x12, x15, x16, lsl #1
    ldrh    w12, [x12, #1024]           // firstcode[s]
    sub     w14, w14, w12
    add     x12, x15, x16, lsl #1
    ldrh    w12, [x12, #1124]           // firstsymbol[s]
    add     w14, w14, w12
    cmp     w14, #288
    b.ge    25f
    add     x12, x15, w14, sxtw
    ldrb    w12, [x12, #1156]           // size[b]
    cmp     w12, w16
    b.ne    25f
    add     x12, x15, w14, sxtw #1
    ldrh    w12, [x12, #1444]           // value[b]
    b       21b
25: mov     w12, #-1
    ret

    /* w14 (1-13) extra bits: w16; uses x12 */
30: mov     w12, #32                    // stb refills when it holds fewer than asked for
    sub     w12, w12, w10
    and     w12, w12, #0xfffffff8
    cmp     w10, w14
    csel    w12, w12, wzr, lt
    add     w10, w10, w12
    neg     w12, w14
    lsl     x16, x8, x12
    lsr     x16, x16, x12
    lsr     x8, x8, x14
    sub     w9, w9, w14
    sub     w10, w10, w14
    ret

96: mov     w11, #1                     // the end of the block
    b       90f
97: mov     w11, #0                     // stb finishes
    b       90f
98: mov     w11, #-1                    // a bad code
    b       90f
99: mov     w11, #-2                    // too far back
90: sub     w12, w9, w10                // hand back what stb would hold
    sub     x1, x1, x12, lsr #3
    mov     x12, #1
    lsl     x12, x12, x10
    sub     x12, x12, #1
    and     x8, x8, x12
    str     x1, [x0]
    str     x3, [x0, #16]
    stp     w8, w10, [x0, #56]
    mov     w0, w11
    ret     x17

/* ---- deflate: x0 the k_deflate_t ----
 * x19 data, x20 out, x21 end - K_ROOM, x22 bits, w23 nbits, w24 i, w25 len, w26 cap,
 * x27 chains, x28 counts */

.macro HASH r                           // stbiw__zhash, kept to 14 bits
    eor     \r, \r, \r, lsl #3
    add     \r, \r, \r, lsr #5
    eor     \r, \r, \r, lsl #4
    add     \r, \r, \r, lsr #17
    eor     \r, \r, \r, lsl #25
    add     \r, \r, \r, lsr #6
    and     \r, \r, #16383
.endm
.macro PUT v, n                         // n bits of v
    lsl     x16, \v, x23
    orr     x22, x22, x16
    add     w23, w23, \n
.endm

    .globl SYM(k_deflate)
    .p2align 2
SYM(k_deflate):
    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    ldp     x19, x20, [x0]
    ldp     x21, x22, [x0, #16]
    sub     x21, x21, #16
    ldp     w23, w24, [x0, #32]
    ldp     w25, w26, [x0, #40]
    ldp     x27, x28, [x0, #48]
1:  mov     w9, #0
    cmp     x20, x21
    b.hi    90f                         // no room
    cmp     w24, w25
    b.ge    89f                         // the end
    sub     w9, w25, #3
    cmp     w24, w9
    b.ge    70f                         // the last 3 bytes go as they are
    ldr     w9, [x19, w24, uxtw]        // the bytes at i (i + 3 < len)
    and     w1, w9, #0xffffff
    HASH    w1
    mul     w2, w1, w26
    add     x2, x27, x2, lsl #2         // the chain
    ldrh    w3, [x28, x1, lsl #1]       // its length
    sub     w4, w25, w24
    mov     w5, #258
    cmp     w4, w5
    csel    w4, w4, w5, lo              // the longest match there can be
    mov     w5, #3                      // best
    mov     w6, #-1                     // where
    mov     w7, #-32768
    add     w7, w24, w7                 // positions after this are near enough
    add     x8, x19, w24, uxtw
    mov     x10, #0
2:  cmp     w10, w3
    b.hs    3f
    ldr     w11, [x2, x10, lsl #2]
    add     x10, x10, #1
    cmp     w11, w7
    b.le    2b
    add     x12, x19, w11, uxtw
    sub     w13, w5, #1                 // as long as the best only if its last byte matches
    ldrb    w14, [x12, x13]
    ldrb    w15, [x8, x13]
    cmp     w14, w15
    b.ne    2b
    bl      80f
    cmp     w13, w5
    b.lo    2b
    mov     w5, w13
    mov     w6, w11
    b       2b
3:  cmp     w3, w26                     // keep this position: a full chain drops its older half
    b.ne    32f
    lsr     w3, w26, #1
    add     x12, x2, x3, lsl #2
    mov     x13, #0
31: ldr     w14, [x12, x13, lsl #2]
    str     w14, [x2, x13, lsl #2]
    add     x13, x13, #1
    cmp     x13, x3
    b.lo    31b
32: str     w24, [x2, x3, lsl #2]
    add     w3, w3, #1
    strh    w3, [x28, x1, lsl #1]
    tbnz    w6, #31, 70f                // no match
    sub     w4, w25, w24                // a longer one at i + 1?
    sub     w4, w4, #1
    mov     w12, #258
    cmp     w4, w12
    csel    w4, w4, w12, lo
    cmp     w5, w4
    b.hs    5f
    lsr     w1, w9, #8
    HASH    w1
    mul     w2, w1, w26
    add     x2, x27, x2, lsl #2
    ldrh    w3, [x28, x1, lsl #1]
    mov     w7, #-32767
    add     w7, w24, w7
    add     x8, x8, #1
    mov     x10, #0
4:  cmp     w10, w3
    b.hs    5f
    ldr     w11, [x2, x10, lsl #2]
    add     x10, x10, #1
    cmp     w11, w7
    b.le    4b
    add     x12, x19, w11, uxtw
    ldrb    w14, [x12, x5]
    ldrb    w15, [x8, x5]
    cmp     w14, w15
    b.ne    4b
    bl      80f
    cmp     w13, w5
    b.ls    4b
    b       70f                         // longer: this byte goes as it is
5:  sub     w1, w5, #3                  // the length's code (l = best - 3)
    mov     w2, #0                      // extra bits
    mov     w3, w1
    cmp     w1, #8
    b.lo    6f
    mov     w3, #28
    cmp     w5, #258
    b.eq    6f
    clz     w2, w1
    mov     w12, #29
    sub     w2, w12, w2
    lsr     w3, w1, w2
    and     w3, w3, #3
    add     w3, w3, w2, lsl #2
    add     w3, w3, #4
6:  add     w3, w3, #1                  // symbol - 256
    cmp     w3, #23
    b.hi    61f
    rbit    w3, w3
    lsr     w3, w3, #25
    mov     w12, #7
    b       62f
61: add     w3, w3, #(0xC0 - 24)
    rbit    w3, w3
    lsr     w3, w3, #24
    mov     w12, #8
62: PUT     x3, w12
    mov     w12, #1
    lsl     w12, w12, w2
    sub     w12, w12, #1
    and     w3, w1, w12
    PUT     x3, w2
    sub     w1, w24, w6                 // the distance's code (d - 1)
    sub     w1, w1, #1
    mov     w2, #0
    mov     w3, w1
    cmp     w1, #2
    b.lo    63f
    clz     w2, w1
    mov     w12, #30
    sub     w2, w12, w2
    lsr     w3, w1, w2
    and     w3, w3, #1
    add     w3, w3, w2, lsl #1
    add     w3, w3, #2
63: rbit    w3, w3
    lsr     w3, w3, #27
    mov     w12, #5
    PUT     x3, w12
    mov     w12, #1
    lsl     w12, w12, w2
    sub     w12, w12, #1
    and     w3, w1, w12
    PUT     x3, w2
    add     w24, w24, w5
    b       71f
70: ldrb    w1, [x19, w24, uxtw]        // a literal: 8 bits up to 143, 9 after
    add     w24, w24, #1
    cmp     w1, #144
    add     w2, w1, #0x30
    add     w3, w1, #0x100
    csel    w2, w2, w3, lo
    mov     w12, #8
    cinc    w12, w12, hs
    rbit    w2, w2
    neg     w3, w12
    lsr     w2, w2, w3
    PUT     x2, w12
71: str     x22, [x20]                  // the whole bytes out
    add     x20, x20, x23, lsr #3
    and     w12, w23, #0xfffffff8
    lsr     x22, x22, x12
    and     w23, w23, #7
    b       1b

    /* match length of x12 and x8, up to w4: w13; uses x14-x16 */
80: mov     w13, #0
81: add     w14, w13, #8
    cmp     w14, w4
    b.hi    83f
    ldr     x15, [x12, x13]
    ldr     x16, [x8, x13]
    eor     x15, x15, x16
    cbnz    x15, 82f
    mov     w13, w14
    b       81b
82: rbit    x15, x15
    clz     x15, x15
    add     w13, w13, w15, lsr #3
    ret
83: cmp     w13, w4
    b.hs    84f
    ldrb    w15, [x12, x13]
    ldrb    w16, [x8, x13]
    cmp     w15, w16
    b.ne    84f
    add     w13, w13, #1
    b       83b
84: ret

89: add     w23, w23, #7                // the end of the block: seven 0 bits
    mov     w9, #1
90: str     x20, [x0, #8]
    str     x22, [x0, #24]
    stp     w23, w24, [x0, #32]
    mov     w0, w9
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret

    .subsections_via_symbols
