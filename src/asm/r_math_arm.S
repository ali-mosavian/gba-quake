/* Projection and fixed-point helpers copied to IWRAM at startup. */
.syntax unified
.cpu arm7tdmi
.arm
.section .iwram,"ax",%progbits
.align 2

/* projection_offset_lut - perspective-divide one camera axis to screen pixels.
 *
 * In:   r0  signed Q8 camera-space coordinate (horizontal or vertical)
 *       r1  Q8 camera-space depth, must be > 0
 *       r2  base of projection_reciprocal_q16[4096], entry d = 65536 / d
 * Out:  r0  signed integer pixel offset from the screen centre, 56 * r0 / r1
 * Clobbers: r0, r1, r3, r12. Leaves r2 and all callee-saved registers intact.
 *
 * Assumptions:
 *   - The depth index is ROUNDED, not truncated: (r1 + 128) >> 8. Callers that
 *     also need 1/z must derive it from the same rounded index, or the screen
 *     position and the depth will disagree and textures will slide.
 *   - Depth is clamped to [1, 4095] world units, matching the table length.
 *     Geometry must already be near-plane clipped; this clamp is a guard, not
 *     a substitute for clipping.
 *   - |r0| * table entry stays inside 64 bits (SMULL) and the intermediate
 *     r3 * 56 stays inside 32 bits. Holds for |coordinate / depth| < 9320,
 *     which the near plane at 8 units guarantees for this map.
 *   - ARM-only: SMULL keeps the multiply native instead of calling the Thumb
 *     64-bit software helper. */
.global projection_offset_lut
.type projection_offset_lut, %function
projection_offset_lut:
    add     r1, r1, #128
    mov     r1, r1, lsr #8
    cmp     r1, #1
    movlo   r1, #1
    cmp     r1, #4096
    movhs   r1, #4096
    subhs   r1, r1, #1
    add     r1, r2, r1, lsl #1
    ldrh    r3, [r1]
    smull   r1, r12, r0, r3
    mov     r3, r1, lsr #12
    orr     r3, r3, r12, lsl #20
    mov     r12, #56
    mul     r0, r3, r12
    mov     r0, r0, asr #12
    bx      lr
.size projection_offset_lut, .-projection_offset_lut

/* signed_ratio_q16_lut - divide-free signed ratio for clip parameters.
 *
 * In:   r0  signed numerator
 *       r1  signed denominator
 *       r2  base of clipping_reciprocal_q24[32768], entry n = 2^24 / n,
 *           UNSATURATED (a uint32 table; a uint16 one would clip every entry
 *           below index 256 and silently corrupt the result)
 * Out:  r0  Q16 ratio r0 / r1
 * Clobbers: r0, r1, r2, r3, r12.
 *
 * Assumptions:
 *   - A zero denominator returns 0 rather than trapping; callers rely on that
 *     for degenerate edges.
 *   - Numerator and denominator are normalized together by right shifting
 *     until the denominator is below 32768, so the sign and ratio survive but
 *     low numerator bits are discarded. Precision is ~15 bits of mantissa.
 *   - The caller is expected to pass |r0| <= |r1| (a clip parameter in
 *     [-1, 1]); larger ratios overflow the Q16 result.
 *   - UMULL is used after the operands are made positive, so the table entry
 *     may use the full unsigned 32-bit range. */
.global signed_ratio_q16_lut
.type signed_ratio_q16_lut, %function
signed_ratio_q16_lut:
    cmp     r1, #0
    moveq   r0, #0
    bxeq    lr
    eor     r12, r0, r1
    cmp     r0, #0
    rsbmi   r0, r0, #0
    cmp     r1, #0
    rsbmi   r1, r1, #0
1:  cmp     r1, #32768
    blo     2f
    mov     r0, r0, lsr #1
    mov     r1, r1, lsr #1
    b       1b
2:  ldr     r3, [r2, r1, lsl #2]
    umull   r2, r1, r3, r0
    mov     r0, r2, lsr #8
    orr     r0, r0, r1, lsl #24
    cmp     r12, #0
    rsbmi   r0, r0, #0
    bx      lr
.size signed_ratio_q16_lut, .-signed_ratio_q16_lut

/* multiply_q16 - Q16 fixed-point multiply using the native long multiply.
 *
 * In:   r0  signed Q16 value
 *       r1  signed Q16 value
 * Out:  r0  signed Q16 product, (r0 * r1) >> 16 evaluated at full 64-bit width
 * Clobbers: r0, r2, r3.
 *
 * Assumptions:
 *   - The true product fits in 32 bits after the shift; no saturation is done.
 *   - Exists so Thumb callers avoid __aeabi_lmul; ARM SMULL does it in one
 *     instruction. */
.global multiply_q16
.type multiply_q16, %function
multiply_q16:
    smull   r2, r3, r0, r1
    mov     r0, r2, lsr #16
    orr     r0, r0, r3, lsl #16
    bx      lr
.size multiply_q16, .-multiply_q16
