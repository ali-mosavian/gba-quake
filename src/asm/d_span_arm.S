/* Hand-written textured span filler for one polygon scanline.
 *
 * NOT IN THE BUILD. Measured bit-exact against the C kernel and 57K cycles a
 * frame SLOWER (1,312,087 against 1,254,964 at the DM1 spawn), so it is kept
 * as a result rather than as production code.
 *
 * Why it loses: the per-scanline boundary still forces the sweep state -
 * span, right and the three plane accumulators - out to a stack frame and
 * back on every segment, roughly fifteen extra transfers across about 1,200
 * segments, plus a prologue and epilogue on every one of ~1,000 rows. The C
 * version is inlined into the polygon routine, so the compiler keeps all of
 * that in registers for the whole face and never marshals it at all.
 *
 * This is the same lesson the earlier per-segment kernel taught one level
 * down: assembly only wins here at a boundary wide enough that the state
 * never round-trips. The next boundary worth trying is the whole polygon.
 * Re-enable by restoring $(BUILD)/d_span_arm.o to ASM_OBJECTS and adding
 * -DBSP_TEXTURED_ASM_SPANS=1 to the bsp_textured rule. */
.syntax unified
.cpu arm7tdmi
.arm
.section .iwram,"ax",%progbits
.align 2

/* draw_texture_row_arm - fill every segment of one polygon scanline.
 *
 * In:   r0  const TextureRowArgs *, words in this order:
 *             +0  uint8_t  *row_pixels        logical framebuffer + y * 120
 *             +4  uint32_t *coverage_row      &texture_coverage[y][0]
 *             +8  const uint8_t *texture      texel base (EWRAM cache or ROM)
 *             +12 uint32_t texture_width      power of two
 *             +16 uint32_t u_mask             texture_width - 1
 *             +20 uint32_t v_mask             texture_height - 1
 *             +24 int32_t  left               first pixel of the row span
 *             +28 int32_t  right              last pixel, inclusive
 *             +32 int32_t  inverse_depth      1/z at `left`, Q16
 *             +36 int32_t  u_over_depth       u/z at `left`, Q4
 *             +40 int32_t  v_over_depth       v/z at `left`, Q4
 *             +44 int32_t  inverse_depth_dx   per-pixel step, Q16
 *             +48 int32_t  u_over_depth_dx    per-pixel step, Q4
 *             +52 int32_t  v_over_depth_dx    per-pixel step, Q4
 *             +56 const uint32_t *reciprocal  clipping_reciprocal_q24
 * Out:  none. Writes row_pixels[left..right] and sets bits in coverage_row.
 * Clobbers: r0-r12, lr; 40 bytes of stack. The args block is not modified.
 *
 * Called once per scanline, not once per segment: an earlier per-segment
 * assembly kernel lost 27K cycles a frame purely to rebuilding its arguments
 * about a thousand times. Constants live in the caller's args block and the
 * sweep state on this routine's own frame, both in IWRAM at one cycle a word.
 *
 * Three segment paths, in the order they are tested:
 *   hidden - every coverage bit already set; skipped before any perspective
 *            work, which is about a third of segments at the DM1 spawn
 *   clear  - no coverage bit set; enters fully unrolled straight-line code at
 *            a computed offset, so no counter, branch or coverage test is
 *            paid per pixel, and one OR records the whole run
 *   mixed  - partially covered; a plain counted loop with a per-pixel test
 *
 * Assumptions:
 *   - Segments are cut on global 32-pixel boundaries, so each maps to exactly
 *     one coverage word and never exceeds the perspective interval.
 *   - Faces arrive near-to-far and coverage is first-hit.
 *   - Perspective is exact at each segment start; u and v step affinely from
 *     there, which is the intended <=32 pixel approximation.
 *   - texture_width and texture_height are powers of two; wrapping is a mask.
 *   - The depth index is clamped to [16, 32767]: 16 matches the 4095-unit far
 *     clamp the projection uses, and it bounds the reciprocal so the SMULL
 *     product cannot leave 64 bits.
 *   - Must execute from IWRAM. From ROM this loop costs roughly four times as
 *     much, which dwarfs every saving it makes. */

.equ ARG_ROW,        0
.equ ARG_COVERAGE,   4
.equ ARG_TEXTURE,    8
.equ ARG_WIDTH,     12
.equ ARG_UMASK,     16
.equ ARG_VMASK,     20
.equ ARG_LEFT,      24
.equ ARG_RIGHT,     28
.equ ARG_INV,       32
.equ ARG_UOZ,       36
.equ ARG_VOZ,       40
.equ ARG_INV_DX,    44
.equ ARG_UOZ_DX,    48
.equ ARG_VOZ_DX,    52
.equ ARG_RECIP,     56

.equ FRAME_ARGS,     0
.equ FRAME_SPAN,     4
.equ FRAME_RIGHT,    8
.equ FRAME_INV,     12
.equ FRAME_UOZ,     16
.equ FRAME_VOZ,     20
.equ FRAME_COVPTR,  24
.equ FRAME_MASK,    28
.equ FRAME_COUNT,   32
.equ FRAME_SIZE,    40

.global draw_texture_row_arm
.type draw_texture_row_arm, %function
draw_texture_row_arm:
    push    {r4-r11, lr}
    sub     sp, sp, #FRAME_SIZE
    str     r0, [sp, #FRAME_ARGS]
    ldr     r1, [r0, #ARG_LEFT]
    str     r1, [sp, #FRAME_SPAN]
    ldr     r1, [r0, #ARG_RIGHT]
    str     r1, [sp, #FRAME_RIGHT]
    ldr     r1, [r0, #ARG_INV]
    str     r1, [sp, #FRAME_INV]
    ldr     r1, [r0, #ARG_UOZ]
    str     r1, [sp, #FRAME_UOZ]
    ldr     r1, [r0, #ARG_VOZ]
    str     r1, [sp, #FRAME_VOZ]

.Lsegment:
    ldr     r9, [sp, #FRAME_SPAN]
    ldr     r10, [sp, #FRAME_RIGHT]
    cmp     r9, r10
    bgt     .Lrow_done
    orr     r11, r9, #31                @ end = min(right, span | 31)
    cmp     r11, r10
    movgt   r11, r10
    sub     r12, r11, r9
    add     r12, r12, #1                @ count
    str     r12, [sp, #FRAME_COUNT]

    /* span_mask = (~0 >> (31 - (end & 31))) & (~0 << (span & 31)) */
    and     r0, r11, #31
    rsb     r0, r0, #31
    mvn     r10, #0
    mov     r10, r10, lsr r0
    and     r0, r9, #31
    mvn     r1, #0
    mov     r1, r1, lsl r0
    and     r10, r10, r1
    str     r10, [sp, #FRAME_MASK]

    ldr     r0, [sp, #FRAME_ARGS]
    ldr     r1, [r0, #ARG_COVERAGE]
    mov     r2, r9, lsr #5
    add     r1, r1, r2, lsl #2
    str     r1, [sp, #FRAME_COVPTR]
    ldr     r2, [r1]                    @ covered
    and     r3, r2, r10
    cmp     r3, r10
    beq     .Ladvance                   @ hidden: no perspective work at all

    /* recip = reciprocal[clamp(inverse_depth >> 16, 16, 32767)] */
    ldr     r4, [sp, #FRAME_INV]
    mov     r5, r4, asr #16
    cmp     r5, #16
    movlt   r5, #16
    cmp     r5, #0x8000
    movge   r5, #0x8000
    subge   r5, r5, #1
    ldr     r6, [r0, #ARG_RECIP]
    ldr     r6, [r6, r5, lsl #2]        @ r6 = reciprocal, < 2^24

    /* u, v, du, dv in Q8 texels: (over_depth * recip) >> (24 - 4). */
    ldr     r7, [sp, #FRAME_UOZ]
    smull   r8, lr, r7, r6
    mov     r8, r8, lsr #20
    orr     r8, r8, lr, lsl #12         @ u
    ldr     r7, [sp, #FRAME_VOZ]
    smull   r11, lr, r7, r6
    mov     r11, r11, lsr #20
    orr     r11, r11, lr, lsl #12       @ v
    ldr     r7, [r0, #ARG_UOZ_DX]
    smull   r5, lr, r7, r6
    mov     r5, r5, lsr #20
    orr     r5, r5, lr, lsl #12         @ du
    ldr     r7, [r0, #ARG_VOZ_DX]
    smull   r4, lr, r7, r6
    mov     r4, r4, lsr #20
    orr     r4, r4, lr, lsl #12         @ dv

    /* Load the pixel-loop constants and settle the register assignment:
     * r0 = u, r1 = v, r2 = du, r3 = dv, r4 = texture, r5 = u_mask,
     * r6 = v_mask, r7 = width, r8 = destination. */
    mov     r0, r8                      @ u
    mov     r1, r11                     @ v
    mov     r2, r5                      @ du
    mov     r3, r4                      @ dv
    ldr     r7, [sp, #FRAME_ARGS]
    ldr     r8, [r7, #ARG_ROW]
    add     r8, r8, r9                  @ destination = row + span
    ldr     r4, [r7, #ARG_TEXTURE]
    ldr     r5, [r7, #ARG_UMASK]
    ldr     r6, [r7, #ARG_VMASK]
    ldr     r7, [r7, #ARG_WIDTH]

    ldr     r12, [sp, #FRAME_COVPTR]
    ldr     r12, [r12]
    ldr     r10, [sp, #FRAME_MASK]
    tst     r12, r10
    bne     .Lmixed                     @ partially covered

    /* Fully clear: record the whole run, then enter the unrolled body. */
    orr     r12, r12, r10
    ldr     r10, [sp, #FRAME_COVPTR]
    str     r12, [r10]
    ldr     r12, [sp, #FRAME_COUNT]
    mov     r11, #28
    mul     r11, r12, r11
    adr     r10, .Lfill_base
    add     r10, r10, #896              @ 32 blocks * 28 bytes
    sub     pc, r10, r11

    /* Thirty-two identical seven-instruction blocks. Entry is computed from
     * the run length, so a run of n pixels executes exactly n blocks. */
.Lfill_base:
.rept 32
    and     r9, r5, r0, asr #8
    and     r10, r6, r1, asr #8
    mla     r9, r10, r7, r9
    ldrb    r12, [r4, r9]
    strb    r12, [r8], #1
    add     r0, r0, r2
    add     r1, r1, r3
.endr
.Lfill_end:
    b       .Ladvance

.Lmixed:
    ldr     r9, [sp, #FRAME_SPAN]
    and     r9, r9, #31
    mov     lr, #1
    mov     lr, lr, lsl r9              @ bit for the current pixel
    ldr     r10, [sp, #FRAME_COUNT]
.Lmixed_loop:
    ldr     r12, [sp, #FRAME_COVPTR]
    ldr     r12, [r12]
    tst     r12, lr
    bne     .Lmixed_next
    orr     r12, r12, lr
    ldr     r9, [sp, #FRAME_COVPTR]
    str     r12, [r9]
    and     r9, r5, r0, asr #8
    and     r12, r6, r1, asr #8
    mla     r9, r12, r7, r9
    ldrb    r12, [r4, r9]
    strb    r12, [r8]
.Lmixed_next:
    add     r8, r8, #1
    mov     lr, lr, lsl #1
    add     r0, r0, r2
    add     r1, r1, r3
    subs    r10, r10, #1
    bne     .Lmixed_loop

.Ladvance:
    /* span += count; step the three plane accumulators by the same amount. */
    ldr     r0, [sp, #FRAME_ARGS]
    ldr     r12, [sp, #FRAME_COUNT]
    ldr     r9, [sp, #FRAME_SPAN]
    add     r9, r9, r12
    str     r9, [sp, #FRAME_SPAN]
    ldr     r1, [r0, #ARG_INV_DX]
    ldr     r2, [sp, #FRAME_INV]
    mla     r2, r1, r12, r2
    str     r2, [sp, #FRAME_INV]
    ldr     r1, [r0, #ARG_UOZ_DX]
    ldr     r2, [sp, #FRAME_UOZ]
    mla     r2, r1, r12, r2
    str     r2, [sp, #FRAME_UOZ]
    ldr     r1, [r0, #ARG_VOZ_DX]
    ldr     r2, [sp, #FRAME_VOZ]
    mla     r2, r1, r12, r2
    str     r2, [sp, #FRAME_VOZ]
    b       .Lsegment

.Lrow_done:
    add     sp, sp, #FRAME_SIZE
    pop     {r4-r11, lr}
    bx      lr
.size draw_texture_row_arm, .-draw_texture_row_arm
