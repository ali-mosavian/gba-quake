/* BSP plane evaluation and batched vertex transform/project routines. */
.syntax unified
.cpu arm7tdmi
.arm
.section .iwram,"ax",%progbits
.align 2
/* plane_distance_q8_arm - signed distance from a point to a BSP plane.
 *
 * In:   r0  const MapPlane * {int16 nx, ny, nz; int16 distance}
 *           nx/ny/nz are Q14 unit-normal components (float * 16384);
 *           distance is in whole world units
 *       r1  Q8 world x        r2  Q8 world y        r3  Q8 world z
 * Out:  r0  signed Q8 distance; >= 0 means the point is on the plane's front
 *           side, which is BSP child index 0
 * Clobbers: r0, r1, r2, r3, r12 (r4-r7 are saved and restored).
 *
 * Assumptions:
 *   - The three products are accumulated at full 64-bit width before the Q14
 *     shift. Narrowing to 32 bits first overflows for far-from-origin points
 *     and silently flips the reported side of the plane.
 *   - `distance` is scaled by 256 here, so the plane record stores it as an
 *     integer; sub-unit plane offsets from the BSP are lost at extract time.
 *   - Used both for camera-leaf descent and for back-face rejection, so its
 *     sign convention must match MapFace.side. */
.global plane_distance_q8_arm
.type plane_distance_q8_arm, %function
plane_distance_q8_arm:
    push    {r4-r7, lr}
    ldrsh   r12, [r0]
    smull   r4, r5, r1, r12
    ldrsh   r12, [r0, #2]
    smull   r6, r7, r2, r12
    adds    r4, r4, r6
    adc     r5, r5, r7
    ldrsh   r12, [r0, #4]
    smull   r6, r7, r3, r12
    adds    r4, r4, r6
    adc     r5, r5, r7
    mov     r4, r4, lsr #14
    orr     r4, r4, r5, lsl #18
    ldrsh   r1, [r0, #6]
    sub     r0, r4, r1, lsl #8
    pop     {r4-r7, lr}
    bx      lr
.size plane_distance_q8_arm, .-plane_distance_q8_arm

/* transform_vertices_arm - fused world->camera->screen->outcode vertex pass.
 *
 * In:   r0  const TransformArgs * laid out word by word as:
 *             +0  const uint16_t *indices   frame vertex index list
 *             +4  uint32_t count            number of indices
 *             +8  const MapVertex *vertices int16 x,y,z world positions
 *             +12 CameraPoint *output       {int32 horizontal, vertical, depth}
 *             +16 int32_t camera_x (Q8)
 *             +20 int32_t camera_y (Q8)
 *             +24 int32_t camera_z (Q8)
 *             +28 int32_t sine   (Q14)
 *             +32 int32_t cosine (Q14)
 *             +36 ScreenPoint *screen_output {int32 x, y}
 *             +40 uint8_t *outcodes
 *             +44 const uint16_t *projection_reciprocal (65536 / depth)
 * Out:  none; writes output[], screen_output[] and outcodes[] INDEXED BY THE
 *       VERTEX ID, not by position in the index list, so the arrays must be
 *       sized for the whole map, not for `count`.
 * Clobbers: r0-r3, r12 (r4-r11 saved); uses 32 bytes of stack.
 *
 * Outcode bits: 1 left of screen, 2 right, 4 above, 8 below, 16 behind the
 * near plane. Bit 16 is exclusive: when it is set, the screen fields are not
 * written and must not be read.
 *
 * Assumptions:
 *   - Yaw-only camera. Pitch and roll are not represented; `vertical` is a
 *     plain world-z difference with no rotation applied.
 *   - Near plane is hard-coded as depth < 2048 (Q8, i.e. 8 world units) and
 *     matches NEAR_PLANE_Q8 in r_state.c by convention only.
 *   - Screen centre (60, 40), focal length 56 and the 120x80 outcode bounds
 *     are hard-coded to match SCREEN_WIDTH/SCREEN_HEIGHT.
 *   - The depth index is ROUNDED, (depth + 128) >> 8, and clamped to
 *     [1, 4095]; see the note in projection_offset_lut about keeping any
 *     separately computed 1/z on the same rounding.
 *   - Rotation uses 64-bit accumulation before the Q14 shift for the same
 *     overflow reason as plane_distance_q8_arm. */
.global transform_vertices_arm
.type transform_vertices_arm, %function
transform_vertices_arm:
    push    {r4-r11, lr}
    sub     sp, sp, #32
    ldr     r4, [r0]
    ldr     r5, [r0, #4]
    ldr     r6, [r0, #8]
    ldr     r7, [r0, #12]
    ldr     r8, [r0, #16]
    str     r8, [sp]
    ldr     r8, [r0, #20]
    str     r8, [sp, #4]
    ldr     r8, [r0, #24]
    str     r8, [sp, #8]
    ldr     r8, [r0, #28]
    str     r8, [sp, #12]
    ldr     r8, [r0, #32]
    str     r8, [sp, #16]
    ldr     r8, [r0, #36]
    str     r8, [sp, #20]
    ldr     r8, [r0, #40]
    str     r8, [sp, #24]
    ldr     r8, [r0, #44]
    str     r8, [sp, #28]
1:  cmp     r5, #0
    beq     4f
    ldrh    r0, [r4], #2
    add     r1, r0, r0, lsl #1
    add     r1, r6, r1, lsl #1
    ldrsh   r2, [r1]
    ldrsh   r3, [r1, #2]
    ldrsh   r12, [r1, #4]
    mov     r2, r2, lsl #8
    ldr     r8, [sp]
    sub     r2, r2, r8
    mov     r3, r3, lsl #8
    ldr     r8, [sp, #4]
    sub     r3, r3, r8
    mov     r12, r12, lsl #8
    ldr     r8, [sp, #8]
    sub     r12, r12, r8
    add     r1, r0, r0, lsl #1
    add     r1, r7, r1, lsl #2
    str     r12, [r1, #4]
    ldr     r12, [sp, #12]
    rsb     r12, r12, #0
    smull   r8, r9, r2, r12
    ldr     r12, [sp, #16]
    smull   r10, r11, r3, r12
    adds    r8, r8, r10
    adc     r9, r9, r11
    mov     r8, r8, lsr #14
    orr     r8, r8, r9, lsl #18
    str     r8, [r1]
    ldr     r12, [sp, #16]
    smull   r8, r9, r2, r12
    ldr     r12, [sp, #12]
    smull   r10, r11, r3, r12
    adds    r8, r8, r10
    adc     r9, r9, r11
    mov     r8, r8, lsr #14
    orr     r8, r8, r9, lsl #18
    str     r8, [r1, #8]
    cmp     r8, #2048
    blt     3f
    add     r9, r8, #128
    mov     r9, r9, lsr #8
    cmp     r9, #4096
    movhs   r9, #4096
    subhs   r9, r9, #1
    ldr     r10, [sp, #28]
    add     r10, r10, r9, lsl #1
    ldrh    r9, [r10]
    ldr     r2, [r1]
    smull   r10, r11, r2, r9
    mov     r10, r10, lsr #12
    orr     r10, r10, r11, lsl #20
    mov     r11, #56
    mul     r10, r11, r10
    mov     r10, r10, asr #12
    add     r10, r10, #60
    ldr     r3, [sp, #20]
    add     r3, r3, r0, lsl #3
    str     r10, [r3]
    ldr     r2, [r1, #4]
    smull   r8, r11, r2, r9
    mov     r8, r8, lsr #12
    orr     r8, r8, r11, lsl #20
    mov     r11, #56
    mul     r8, r11, r8
    mov     r8, r8, asr #12
    rsb     r11, r8, #40
    str     r11, [r3, #4]
    mov     r2, #0
    cmp     r10, #0
    orrlt   r2, r2, #1
    cmp     r10, #120
    orrge   r2, r2, #2
    cmp     r11, #0
    orrlt   r2, r2, #4
    cmp     r11, #80
    orrge   r2, r2, #8
    ldr     r3, [sp, #24]
    strb    r2, [r3, r0]
    b       5f
3:  ldr     r3, [sp, #24]
    mov     r2, #16
    strb    r2, [r3, r0]
5:
    subs    r5, r5, #1
    b       1b
4:  add     sp, sp, #32
    pop     {r4-r11, lr}
    bx      lr
.size transform_vertices_arm, .-transform_vertices_arm
