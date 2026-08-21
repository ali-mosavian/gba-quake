/* Per-face vertex projection: camera lookup, texture coordinates, projection.
 *
 * NOT IN THE BUILD. Bit-exact against the C it replaces and 20% SLOWER: the
 * face front end went from 290,892 cycles to 348,349, or 1,818 per face to
 * 2,177.
 *
 * Why it loses, and it is the same reason every hand-written kernel in this
 * project has lost: the C version is inlined into render_textured_faces, so
 * the compiler allocates registers across the entire face loop and keeps axis
 * terms and table bases live between vertices. A separate routine cannot. It
 * pays a nine-register prologue and seven argument loads per face, and then
 * reloads all ten axis terms on every vertex because eight axis values plus
 * six base pointers plus the accumulator do not fit the register file.
 *
 * The pattern across three attempts -- per 32-pixel segment, per scanline,
 * and here per face ring -- is that assembly only wins at a boundary wide
 * enough that state never leaves registers. For this renderer that means the
 * whole face pipeline as one routine (project, fit, walk, scan, fill), not a
 * stage of it. The one place hand assembly did win was replacing a libgcc
 * call, __clzsi2, which was a function call the compiler could not avoid.
 *
 * Re-enable by restoring $(BUILD)/r_project_arm.o to ASM_OBJECTS and the
 * RingArgs call site in r_surf.c. */
.syntax unified
.cpu arm7tdmi
.arm
.section .iwram,"ax",%progbits
.align 2

/* project_face_ring_arm - project one face's whole vertex ring.
 *
 * In:   r0  const RingArgs *, words in this order:
 *             +0  const uint16_t *ring       bsp_face_vertices + first_vertex
 *             +4  uint32_t count             vertices in the ring
 *             +8  TextureVertex *output      28 bytes each
 *             +12 const int32_t *terms       10 words: 4 u axis, 4 v axis,
 *                                            u_base_q8, v_base_q8
 *             +16 const CameraPoint *camera  12 bytes each, by vertex index
 *             +20 const MapVertex *vertices  6 bytes each, by vertex index
 *             +24 const uint16_t *reciprocal projection_reciprocal_q16[4096]
 * Out:  r0  bitmask of clip planes the ring crosses, as r_surf.c defines them:
 *             1 near, 2 right, 4 left, 8 bottom, 16 top
 * Clobbers: r0-r12, lr; 8 bytes of stack.
 *
 * One call per face rather than per vertex. The ring pointer, the three table
 * bases, the axis pointer and the accumulating plane mask all stay in
 * registers across every vertex of the face, which is the point: the compiler
 * spent 40% of this function's instructions on loads and stores against
 * thirteen multiplies in the whole thing.
 *
 * TextureVertex layout, 28 bytes: x, y, xq8, yq8, inverse_depth,
 * u_over_depth, v_over_depth.
 *
 * Assumptions:
 *   - Screen centre (60, 40), focal length 56, viewport 120x80, depth index
 *     rounded and clamped to [1, 4095]: identical to project_ring_vertex in
 *     r_surf.c, which remains the reference implementation.
 *   - Texture axes are Q12 against whole-unit world coordinates, so the dot
 *     product is Q12 texels and >>4 leaves Q8.
 *   - A vertex behind the near plane is still projected; the caller acts on
 *     the returned mask, not on the garbage coordinates.
 *   - count >= 1.
 *   - Must run from IWRAM. */
.global project_face_ring_arm
.type project_face_ring_arm, %function
project_face_ring_arm:
    push    {r4-r11, lr}
    sub     sp, sp, #8
    ldr     r4, [r0, #0]                @ ring
    ldr     r11, [r0, #4]               @ count
    ldr     r5, [r0, #8]                @ output
    ldr     r8, [r0, #12]               @ axis terms
    ldr     r6, [r0, #16]               @ camera cache
    ldr     r7, [r0, #20]               @ world vertices
    ldr     r9, [r0, #24]               @ reciprocal table
    mov     r10, #0                     @ plane mask

.Lvertex:
    ldrh    r0, [r4], #2                @ vertex index

    add     r12, r6, r0, lsl #2
    add     r12, r12, r0, lsl #3        @ camera + index * 12
    ldmia   r12, {r1, r2, r3}           @ horizontal, vertical, depth

    cmp     r3, #2048                   @ NEAR_PLANE_Q8, 8 units
    orrlt   r10, r10, #1

    add     r12, r3, #128               @ round to whole units
    mov     r12, r12, asr #8
    cmp     r12, #1
    movlt   r12, #1
    cmp     r12, #4096
    movge   r12, #4096
    subge   r12, r12, #1                @ 4095 is not an ARM immediate
    add     r12, r9, r12, lsl #1
    ldrh    r12, [r12]                  @ inv = 65536 / z

    /* xq8 = (60<<8) + (((horizontal * inv) >> 12) * 56 >> 4), 56 = 8 * 7 */
    smull   r3, lr, r1, r12             @ depth is dead now, reuse r3
    mov     r3, r3, lsr #12
    orr     r3, r3, lr, lsl #20
    rsb     lr, r3, r3, lsl #3
    mov     lr, lr, lsl #3
    mov     r1, #15360
    add     r1, r1, lr, asr #4          @ xq8

    smull   r3, lr, r2, r12
    mov     r3, r3, lsr #12
    orr     r3, r3, lr, lsl #20
    rsb     lr, r3, r3, lsl #3
    mov     lr, lr, lsl #3
    mov     r2, #10240
    sub     r2, r2, lr, asr #4          @ yq8

    cmp     r1, #30720                  @ 120 << 8
    orrgt   r10, r10, #2
    cmp     r1, #0
    orrlt   r10, r10, #4
    cmp     r2, #20480                  @ 80 << 8
    orrgt   r10, r10, #8
    cmp     r2, #0
    orrlt   r10, r10, #16

    mov     r3, r1, asr #8
    str     r3, [r5, #0]                @ x
    mov     r3, r2, asr #8
    str     r3, [r5, #4]                @ y
    str     r1, [r5, #8]                @ xq8
    str     r2, [r5, #12]               @ yq8
    str     r12, [r5, #16]              @ inverse_depth

    /* Texture coordinates. inv stays in r12 for the two long multiplies. */
    add     lr, r7, r0, lsl #1
    add     lr, lr, r0, lsl #2          @ vertices + index * 6
    str     lr, [sp, #4]
    ldrsh   r0, [lr]                    @ world x
    ldrsh   r1, [lr, #2]                @ world y
    ldrsh   lr, [lr, #4]                @ world z

    ldr     r2, [r8, #0]
    mul     r3, r2, r0
    ldr     r2, [r8, #4]
    mla     r3, r2, r1, r3
    ldr     r2, [r8, #8]
    mla     r3, r2, lr, r3
    ldr     r2, [r8, #12]
    add     r3, r3, r2
    mov     r3, r3, asr #4
    ldr     r2, [r8, #32]               @ u_base_q8
    sub     r3, r3, r2                  @ u in Q8 texels
    str     r3, [sp, #0]

    ldr     r2, [r8, #16]
    mul     r3, r2, r0
    ldr     r2, [r8, #20]
    mla     r3, r2, r1, r3
    ldr     r2, [r8, #24]
    mla     r3, r2, lr, r3
    ldr     r2, [r8, #28]
    add     r3, r3, r2
    mov     r3, r3, asr #4
    ldr     r2, [r8, #36]               @ v_base_q8
    sub     r3, r3, r2                  @ v in Q8 texels

    smull   r0, r1, r3, r12
    mov     r0, r0, lsr #8
    orr     r0, r0, r1, lsl #24
    str     r0, [r5, #24]               @ v_over_depth

    ldr     r3, [sp, #0]
    smull   r0, r1, r3, r12
    mov     r0, r0, lsr #8
    orr     r0, r0, r1, lsl #24
    str     r0, [r5, #20]               @ u_over_depth

    add     r5, r5, #28
    subs    r11, r11, #1
    bne     .Lvertex

    mov     r0, r10
    add     sp, sp, #8
    pop     {r4-r11, lr}
    bx      lr
.size project_face_ring_arm, .-project_face_ring_arm
