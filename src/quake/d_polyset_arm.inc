/* draw_textured_polygon_arm - perspective-correct convex polygon rasterizer.
 *
 * Seeded from the compiler's output for draw_textured_polygon_reference in
 * d_scan.c, then hand optimized. THIS FILE IS AUTHORITATIVE: regenerating it
 * with scripts/extract_polygon_asm.py reproduces only the unoptimized
 * baseline. Keep the C reference behaviourally identical so it stays usable
 * for diffing.
 *
 * In:   r0  const TextureVertex *vertices, screen-space, already near-plane
 *           clipped and in consistent winding order. Per vertex:
 *             +0  int32 x               integer screen x
 *             +4  int32 y               integer screen y
 *             +8  int32 xq8, +12 yq8    Q8 sub-pixel screen position
 *             +16 int32 inverse_depth   65536 / z
 *             +20 int32 u_over_depth    Q8 texel u, premultiplied by 1/z
 *             +24 int32 v_over_depth    Q8 texel v, premultiplied by 1/z
 *       r1  unsigned vertex_count (3..33)
 *       r2  uint16_t texture_index into bsp_textures[]
 * Out:  none. Writes logical_framebuffer[] and sets bits in
 *       texture_coverage[y][word]; both are file-static EWRAM arrays reached
 *       by absolute address, so this routine is not reentrant.
 * Clobbers: r0-r12; allocates a 620-byte stack frame in IWRAM.
 *
 * Assumptions:
 *   - Faces arrive in near-to-far order. Coverage is first-hit: a pixel whose
 *     coverage bit is already set is skipped, which replaces a depth buffer
 *     and is only valid for opaque, correctly ordered world surfaces.
 *   - Coverage is four 32-bit words per 120-pixel row. Perspective segments
 *     are aligned to those 32-pixel word boundaries so each segment touches
 *     exactly one coverage word; a word reading 0xffffffff is rejected whole.
 *   - Perspective is exact only at segment endpoints, at most 32 pixels apart;
 *     u and v step affinely in between. That approximation is intended.
 *   - Texture width and height must be powers of two; wrapping is a mask.
 *     The extractor pads non-power-of-two textures up to the next power.
 *   - Texture dimensions are hoisted into registers once per polygon. Do not
 *     reintroduce per-texel loads of the descriptor; that cost 27K cycles.
 *   - Runs from IWRAM. Executing it from ROM costs roughly 4x, which was the
 *     single largest measured regression in this renderer. */
/* Generated from the bit-exact C reference; hand optimization baseline. */
.section .iwram,"ax",%progbits
.align 2
.arm
.global draw_textured_polygon_arm
.type draw_textured_polygon_arm, %function
draw_textured_polygon_arm:
	@ Function supports interworking.
	@ args = 0, pretend = 0, frame = 616
	@ frame_needed = 0, uses_anonymous_args = 0
	push	{r4, r5, r6, r7, r8, r9, r10, fp, lr}
	mov	r10, r1
	ldr	ip, [r0]
	ldr	r3, [r0, #20]
	sub	sp, sp, #620
	str	r2, [sp, #4]
	mov	r7, #1
	mov	r8, ip
	str	ip, [sp, #20]
	ldr	r1, [r0, #4]
	ldr	lr, [r0, #24]
	str	r0, [sp, #12]
	str	r0, [sp, #16]
	str	r10, [sp, #24]
	sub	r3, r3, ip
	add	r2, r0, #40
	sub	r9, r10, #2
	b	.Lpolygon_39
.Lpolygon_126:
	cmp	ip, #0
	bne	.Lpolygon_125
	add	r7, r7, #1
.Lpolygon_39:
	mov	r5, r3
	sub	r4, lr, r1
	ldm	r2, {r3, lr}
	sub	r6, lr, r1
	sub	r3, r3, r8
	mul	r0, r6, r5
	mul	ip, r4, r3
	cmp	r9, r7
	mov	fp, r2
	sub	ip, r0, ip
	add	r2, r2, #20
	bne	.Lpolygon_126
	cmp	ip, #0
	str	r3, [sp, #8]
	ldr	r8, [sp, #12]
	ldr	r0, [sp, #16]
	ldr	r10, [sp, #24]
	bne	.Lpolygon_40
.Lpolygon_37:
	add	sp, sp, #620
	@ sp needed
	pop	{r4, r5, r6, r7, r8, r9, r10, fp, lr}
	bx	lr
.Lpolygon_125:
	str	r3, [sp, #8]
	ldr	r8, [sp, #12]
	ldr	r0, [sp, #16]
	ldr	r10, [sp, #24]
.Lpolygon_40:
	ldr	r3, [sp, #4]
	ldr	r2, .Lpolygon_129
	lsl	r3, r3, #1
	str	r3, [sp, #32]
	ldrh	r3, [r2, r3]
	ldr	r2, .Lpolygon_129+4
	add	lr, r7, r7, lsl #2
	cmp	r3, r2
	add	r2, r0, lr, lsl #2
	str	lr, [sp, #28]
	str	r2, [sp, #12]
	beq	.Lpolygon_41
	ldr	r2, .Lpolygon_129+8
	add	r3, r3, r2
	str	r3, [sp, #84]
.Lpolygon_42:
	ldr	r3, [sp, #4]
	str	r3, [sp, #72]
	ldr	r3, [sp, #12]
	str	r1, [sp, #36]
	ldr	r3, [r3, #8]
	ldr	r1, [r0, #8]
	ldr	r2, [r0, #16]
	str	r10, [sp, #64]
	sub	r10, r3, r1
	ldr	r3, [fp, #8]
	str	r2, [sp, #32]
	sub	r3, r3, r1
	lsr	r2, ip, #31
	smull	r7, r9, r10, r6
	str	r3, [sp, #16]
	str	r2, [sp, #4]
	smull	r3, r2, r4, r3
	subs	r3, r7, r3
	eor	r7, ip, ip, asr #31
	sub	r7, r7, ip, asr #31
	str	r1, [sp, #68]
	ldr	r1, [r0, #12]
	str	r0, [sp, #60]
	mov	r0, r7
	sbc	r9, r9, r2
	str	r1, [sp, #28]
	str	r3, [sp, #24]
	bl	__clzsi2
	rsb	r2, r0, #32
	cmp	r2, #15
	movcc	r2, #15
	ldr	r3, [sp, #24]
	asr	ip, r9, #31
	ldr	r1, [sp, #4]
	eor	r3, r3, r9, asr #31
	subs	r3, r3, ip
	eor	r0, r9, r9, asr #31
	sub	lr, r2, #15
	sbc	r0, r0, ip
	cmp	r1, r9, lsr #31
	ldr	r1, .Lpolygon_129+12
	rsb	r9, lr, #32
	lsr	ip, r7, lr
	lsr	r3, r3, lr
	ldr	r7, [r1, ip, lsl #2]
	orr	r3, r3, r0, lsl r9
	sub	r2, r2, #47
	orr	r3, r3, r0, lsr r2
	umull	r3, r2, r7, r3
	lsr	r0, r0, lr
	mla	r2, r7, r0, r2
	lsr	r3, r3, #24
	orr	r3, r3, r2, lsl #8
	str	r3, [sp, #44]
	rsbne	r3, r3, #0
	strne	r3, [sp, #44]
	ldr	ip, [sp, #16]
	ldr	r3, [sp, #8]
	smull	ip, r0, r5, ip
	smull	r3, r2, r10, r3
	subs	r3, ip, r3
	sbc	r0, r0, r2
	asr	ip, r0, #31
	eor	r3, r3, r0, asr #31
	subs	r3, r3, ip
	eor	r2, r0, r0, asr #31
	sbc	r2, r2, ip
	lsr	r3, r3, lr
	sub	r10, lr, #32
	ldr	ip, [sp, #4]
	orr	r3, r3, r2, lsl r9
	orr	r3, r3, r2, lsr r10
	cmp	ip, r0, lsr #31
	umull	r3, r0, r7, r3
	lsr	r2, r2, lr
	mla	r0, r7, r2, r0
	lsr	r3, r3, #24
	orr	r3, r3, r0, lsl #8
	str	r3, [sp, #56]
	rsbne	r3, r3, #0
	strne	r3, [sp, #56]
	ldr	r3, [sp, #12]
	ldr	r0, [sp, #28]
	ldr	r3, [r3, #12]
	sub	r2, r3, r0
	ldr	r3, [fp, #12]
	str	r2, [sp, #16]
	sub	r3, r3, r0
	smull	r2, r0, r6, r2
	mov	ip, r2
	str	r3, [sp, #24]
	smull	r3, r2, r4, r3
	subs	r3, ip, r3
	sbc	r0, r0, r2
	asr	ip, r0, #31
	eor	r3, r3, r0, asr #31
	subs	r3, r3, ip
	eor	r2, r0, r0, asr #31
	sbc	r2, r2, ip
	lsr	r3, r3, lr
	ldr	ip, [sp, #4]
	orr	r3, r3, r2, lsl r9
	orr	r3, r3, r2, lsr r10
	cmp	ip, r0, lsr #31
	umull	r3, r0, r7, r3
	lsr	r2, r2, lr
	mla	r0, r7, r2, r0
	ldr	ip, [sp, #24]
	lsr	r3, r3, #24
	orr	r3, r3, r0, lsl #8
	smull	ip, r0, r5, ip
	str	r3, [sp, #40]
	rsbne	r3, r3, #0
	strne	r3, [sp, #40]
	str	r0, [sp, #24]
	ldr	r3, [sp, #8]
	ldr	r0, [sp, #16]
	smull	r3, r2, r0, r3
	ldr	r1, [sp, #36]
	ldr	r0, [sp, #24]
	subs	r3, ip, r3
	sbc	r0, r0, r2
	asr	ip, r0, #31
	eor	r3, r3, r0, asr #31
	subs	r3, r3, ip
	eor	r2, r0, r0, asr #31
	sbc	r2, r2, ip
	lsr	r3, r3, lr
	orr	r3, r3, r2, lsl r9
	ldr	ip, [sp, #4]
	orr	r3, r3, r2, lsr r10
	cmp	ip, r0, lsr #31
	umull	r3, r0, r7, r3
	lsr	r2, r2, lr
	mla	r0, r7, r2, r0
	lsr	r3, r3, #24
	orr	r3, r3, r0, lsl #8
	str	r3, [sp, #52]
	rsbne	r3, r3, #0
	strne	r3, [sp, #52]
	ldr	r3, [sp, #12]
	ldr	r0, [sp, #32]
	ldr	r3, [r3, #16]
	ldr	r2, [fp, #16]
	sub	r3, r3, r0
	sub	fp, r2, r0
	str	r3, [sp, #12]
	smull	r2, r3, r6, r3
	mov	r6, r2
	smull	r4, r2, fp, r4
	subs	r4, r6, r4
	sbc	r2, r3, r2
	asr	ip, r2, #31
	eor	r4, r4, r2, asr #31
	subs	r4, r4, ip
	eor	r0, r2, r2, asr #31
	sbc	r0, r0, ip
	lsr	r3, r4, lr
	orr	r3, r3, r0, lsl r9
	ldr	ip, [sp, #4]
	orr	r3, r3, r0, lsr r10
	cmp	ip, r2, lsr #31
	umull	r3, r2, r7, r3
	lsr	r0, r0, lr
	mla	r2, r7, r0, r2
	lsr	r3, r3, #24
	orr	r3, r3, r2, lsl #8
	str	r3, [sp, #36]
	rsbne	r3, r3, #0
	strne	r3, [sp, #36]
	add	r3, sp, #8
	ldm	r3, {r3, ip}
	smull	r5, r2, fp, r5
	smull	r3, r0, ip, r3
	subs	r3, r5, r3
	sbc	r0, r2, r0
	asr	ip, r0, #31
	eor	r3, r3, r0, asr #31
	subs	r3, r3, ip
	eor	r2, r0, r0, asr #31
	sbc	r2, r2, ip
	lsr	r3, r3, lr
	orr	r3, r3, r2, lsl r9
	ldr	ip, [sp, #4]
	orr	r3, r3, r2, lsr r10
	cmp	ip, r0, lsr #31
	umull	r3, r0, r7, r3
	lsr	r2, r2, lr
	mla	r0, r7, r2, r0
	lsr	r3, r3, #24
	orr	r3, r3, r0, lsl #8
	str	r3, [sp, #48]
	rsbne	r3, r3, #0
	strne	r3, [sp, #48]
	mov	r3, #0
	mov	r7, r1
	mov	fp, r3
	ldr	r9, [sp, #60]
	str	r1, [sp, #24]
	str	r8, [sp, #76]
	ldr	r2, [sp, #64]
	b	.Lpolygon_61
.Lpolygon_89:
	mov	r0, r8
	sub	r6, r8, r7
	mov	r8, r7
	mov	r7, r0
	add	ip, sp, #88
	add	r10, ip, r3, lsl #4
	lsl	ip, r1, #8
	str	ip, [sp, #4]
	add	r3, r3, #1
.Lpolygon_88:
	sub	r5, r5, r1
	asr	r1, r5, #24
	mov	r0, r6
	str	r2, [sp, #16]
	str	r3, [sp, #12]
	str	r1, [sp, #8]
	bl	__clzsi2
	rsb	r0, r0, #32
	cmp	r0, #15
	movcc	r0, #15
	ldr	r1, [sp, #8]
	lsl	r4, r5, #8
	asr	ip, r5, #31
	eor	r4, r4, r5, asr #31
	subs	r4, r4, ip
	eor	r5, r1, r5, asr #31
	sbc	r5, r5, ip
	ldr	r3, .Lpolygon_129+12
	sub	ip, r0, #15
	lsr	r6, r6, ip
	cmp	r1, #0
	lsr	r1, r4, ip
	rsb	r4, r0, #47
	ldr	lr, [r3, r6, lsl #2]
	orr	r1, r1, r5, lsl r4
	sub	r0, r0, #47
	orr	r1, r1, r5, lsr r0
	umull	r6, r1, lr, r1
	lsr	ip, r5, ip
	mla	r1, lr, ip, r1
	lsr	r6, r6, #24
	orr	r6, r6, r1, lsl #8
	ldr	r3, [sp, #12]
	ldr	r2, [sp, #16]
	rsblt	r6, r6, #0
.Lpolygon_59:
	ldr	r1, [sp, #4]
	cmp	r2, fp
	str	r8, [r10]
	str	r7, [r10, #4]
	str	r1, [r10, #8]
	str	r6, [r10, #12]
	beq	.Lpolygon_60
.Lpolygon_122:
	ldr	r7, [r9, #24]
	add	r9, r9, #20
.Lpolygon_61:
	add	fp, fp, #1
	cmp	r2, fp
	beq	.Lpolygon_56
	ldr	r8, [r9, #24]
	cmp	r8, r7
	beq	.Lpolygon_122
	ldr	r5, [r9, #20]
.Lpolygon_58:
	cmp	r7, r8
	ldr	r1, [r9]
	blt	.Lpolygon_89
	add	r0, sp, #88
	add	r10, r0, r3, lsl #4
	subs	r6, r7, r8
	lsl	r0, r5, #8
	str	r0, [sp, #4]
	addeq	r3, r3, #1
	beq	.Lpolygon_59
	mov	r0, r1
	add	r3, r3, #1
	mov	r1, r5
	mov	r5, r0
	b	.Lpolygon_88
.Lpolygon_56:
	ldr	r8, [sp, #24]
	cmp	r8, r7
	ldrne	r5, [sp, #20]
	bne	.Lpolygon_58
.Lpolygon_60:
	ldr	r1, [sp, #24]
	mov	r2, r1
	mov	ip, r1
	mov	r6, r3
	ldr	r3, [sp, #64]
	sub	r0, r3, #1
	ldr	r3, [sp, #60]
	add	r0, r0, r0, lsl #2
	ldr	r8, [sp, #76]
	add	r0, r3, r0, lsl #2
.Lpolygon_62:
	ldr	r3, [r8, #24]
	cmp	ip, r3
	movge	ip, r3
	add	r8, r8, #20
	cmp	r2, r3
	movlt	r2, r3
	cmp	r0, r8
	bne	.Lpolygon_62
	cmp	r2, #79
	movge	r2, #79
	cmp	r6, #0
	bic	r0, ip, ip, asr #31
	beq	.Lpolygon_63
	add	r3, sp, #88
	add	lr, r3, r6, lsl #4
.Lpolygon_65:
	ldr	ip, [r3]
	cmp	ip, r0
	bge	.Lpolygon_64
	ldr	r4, [r3, #4]
	cmp	r4, r0
	addgt	r4, r3, #8
	ldmgt	r4, {r4, r5}
	sub	ip, r0, ip
	mlagt	ip, r5, ip, r4
	strgt	ip, [r3, #8]
.Lpolygon_64:
	add	r3, r3, #16
	cmp	lr, r3
	bne	.Lpolygon_65
.Lpolygon_63:
	cmp	r2, r0
	blt	.Lpolygon_37
	add	r3, r2, #1
	subs	r4, r6, #0
	str	r3, [sp, #60]
	beq	.Lpolygon_37
	ldr	r3, [sp, #20]
	add	r2, sp, #32
	ldm	r2, {r2, r9}
	rsb	r3, r3, #0
	mla	r2, r9, r3, r2
	ldr	r7, [sp, #44]
	ldr	lr, [sp, #68]
	mla	lr, r7, r3, lr
	ldr	ip, [sp, #28]
	ldr	r8, [sp, #40]
	mla	ip, r8, r3, ip
	mov	r3, r2
	sub	r2, r0, r1
	ldr	r1, [sp, #56]
	mla	r1, r2, r1, lr
	mov	lr, r1
	ldr	r5, [sp, #52]
	ldr	r6, [sp, #48]
	mla	r5, r2, r5, ip
	mla	r6, r2, r6, r3
	ldr	r3, .Lpolygon_129+16
	ldr	r2, [sp, #72]
	add	r10, r3, r2, lsl #3
	ldr	r10, [r10, #4]
	mov	r9, r10, lsl #16
	mov	r9, r9, lsr #16
	str	r9, [sp, #608]
	mov	r10, r10, lsr #16
	sub	r10, r10, #1
	add	r3, sp, #88
	add	ip, r3, r4, lsl #4
	lsl	r3, r7, #5
	str	r3, [sp, #20]
	lsl	r3, r8, #5
	str	r3, [sp, #24]
	lsl	r3, r9, #5
	str	r3, [sp, #28]
.Lpolygon_67:
	mov	r8, #0
	mov	r7, r8
	mov	r9, r8
	add	r3, sp, #88
.Lpolygon_73:
	ldr	r2, [r3]
	cmp	r2, r0
	bgt	.Lpolygon_69
	ldr	r2, [r3, #4]
	cmp	r2, r0
	ble	.Lpolygon_69
	rsbs	r1, r9, #1
	movcc	r1, #0
	add	r2, r3, #8
	ldm	r2, {r2, r4}
	cmp	r7, r2, asr #8
	add	r4, r4, r2
	str	r4, [r3, #8]
	movle	r4, r1
	orrgt	r4, r1, #1
	cmp	r4, #0
	movne	r7, r2, asr #8
	cmp	r8, r2, asr #8
	orrlt	r1, r1, #1
	cmp	r1, #0
	movne	r8, r2, asr #8
	add	r9, r9, #1
.Lpolygon_69:
	add	r3, r3, #16
	cmp	r3, ip
	bne	.Lpolygon_73
	cmp	r9, #1
	ble	.Lpolygon_78
	cmp	r7, #0
	blt	.Lpolygon_127
	cmp	r8, #119
	movge	r8, #119
.Lpolygon_77:
	cmp	r7, r8
	bgt	.Lpolygon_78
	ldr	r3, [sp, #40]
	mla	r3, r7, r3, r5
	mov	r2, r3
	ldr	r3, [sp, #36]
	ldr	fp, [sp, #44]
	mla	r3, r7, r3, r6
	mla	fp, r7, fp, lr
	str	r3, [sp, #4]
.Lpolygon_76:
	rsb	r3, r0, r0, lsl #4
	str	r3, [sp, #32]
	str	r8, [sp, #8]
	ldr	r9, [sp, #608]
	mov	r3, fp
	mov	r8, r2
	ldr	r1, [sp, #84]
	str	r0, [sp, #64]
	str	r1, [sp, #12]
	str	r6, [sp, #68]
	str	r5, [sp, #72]
	str	lr, [sp, #76]
	str	ip, [sp, #80]
.Lpolygon_84:
	ldr	r1, [sp, #8]
	orr	r2, r7, #31
	ldr	r4, .Lpolygon_129+20
	cmp	r2, r1
	movge	r2, r1
	str	r2, [sp, #604]
	cmp	r3, r4
	movlt	r1, r3
	movge	r1, r4
	cmp	r1, #1
	movlt	r1, #1
	ldr	r5, [sp, #4]
	mov	lr, r5
	ldr	r0, .Lpolygon_129+24
	lsl	r1, r1, #1
	mov	fp, r0
	ldrh	r0, [r0, r1]
	smull	ip, r1, r0, r8
	lsr	ip, ip, #16
	orr	ip, ip, r1, lsl #16
	smull	lr, r1, r0, lr
	subs	r6, r2, r7
	lsr	r0, lr, #16
	orr	r0, r0, r1, lsl #16
	beq	.Lpolygon_96
	ldr	lr, [sp, #44]
	mla	lr, r6, lr, r3
	cmp	lr, r4
	movge	lr, r4
	mov	r1, r5
	cmp	lr, #1
	movlt	lr, #1
	ldr	r5, [sp, #36]
	mla	r1, r5, r6, r1
	lsl	lr, lr, #1
	ldrh	r5, [fp, lr]
	mov	fp, r1
	ldr	r4, [sp, #40]
	mla	r4, r6, r4, r8
	smull	r4, lr, r5, r4
	smull	r5, r1, fp, r5
	lsr	r4, r4, #16
	lsr	r5, r5, #16
	orr	r4, r4, lr, lsl #16
	orr	r5, r5, r1, lsl #16
	cmp	r6, #1
	sub	r4, r4, ip
	sub	r5, r5, r0
	beq	.Lpolygon_80
	mov	fp, r5
	ldr	lr, .Lpolygon_129+28
	lsl	r1, r6, #1
	ldrh	r1, [lr, r1]
	smull	lr, r4, r1, r4
	smull	r1, r5, fp, r1
	lsr	lr, lr, #16
	lsr	r1, r1, #16
	orr	r4, lr, r4, lsl #16
	orr	r5, r1, r5, lsl #16
.Lpolygon_80:
	cmp	r2, r7
	blt	.Lpolygon_81
.Lpolygon_79:
	str	r8, [sp, #600]
	ldr	r2, [sp, #32]
	add	lr, r7, r2, lsl #3
	ldr	r2, .Lpolygon_129+36
	str	r3, [sp, #16]
	ldr	r3, [sp, #12]
	add	lr, lr, r2
	ldr	r2, [sp, #64]
	mov	fp, r7, lsr #5
	add	fp, fp, r2, lsl #2
	ldr	r2, .Lpolygon_129+32
	add	fp, r2, fp, lsl #2
	str	fp, [sp, #596]
	ldr	r8, [fp]
	cmn	r8, #1
	beq	.Lpolygon_coverage_full
	and	r1, r7, #31
	mov	r2, #1
	mov	r1, r2, lsl r1
	add	r6, r6, #1
	.Lpolygon_83:
	tst	r8, r1
	bne	.Lpolygon_82
	and	r2, r10, r0, asr #8
	mla	r2, r9, r2, r3
	sub	fp, r9, #1
	and	fp, fp, ip, asr #8
	ldrb	r2, [r2, fp]	@ zero_extendqisi2
	strb	r2, [lr]
	orr	r8, r8, r1
	.Lpolygon_82:
	add	lr, lr, #1
	add	ip, ip, r4
	add	r0, r0, r5
	mov	r1, r1, lsl #1
	subs	r6, r6, #1
	bne	.Lpolygon_83
	ldr	r2, [sp, #596]
	str	r8, [r2]
	ldr	r8, [sp, #600]
	b	.Lpolygon_coverage_done
	.Lpolygon_coverage_full:
	ldr	r8, [sp, #600]
	.Lpolygon_coverage_done:
	str	r3, [sp, #12]
	ldr	r3, [sp, #16]
.Lpolygon_81:
	ldr	r2, [sp, #8]
	ldr	r7, [sp, #604]
	add	r7, r7, #1
	cmp	r2, r7
	ldr	r2, [sp, #20]
	add	r3, r3, r2
	ldr	r2, [sp, #24]
	ldr	r1, [sp, #28]
	add	r8, r8, r2
	ldr	r2, [sp, #4]
	add	r2, r2, r1
	str	r2, [sp, #4]
	bge	.Lpolygon_84
	add	r0, sp, #64
	ldm	r0, {r0, r6}
	ldr	r3, [sp, #60]
	add	r0, r0, #1
	add	r5, sp, #72
	cmp	r0, r3
	ldm	r5, {r5, lr}
	ldr	ip, [sp, #80]
	beq	.Lpolygon_37
.Lpolygon_85:
	ldr	r3, [sp, #56]
	add	lr, lr, r3
	ldr	r3, [sp, #52]
	add	r5, r5, r3
	ldr	r3, [sp, #48]
	add	r6, r6, r3
	b	.Lpolygon_67
.Lpolygon_96:
	mov	r5, r6
	mov	r4, r6
	b	.Lpolygon_79
.Lpolygon_78:
	ldr	r3, [sp, #60]
	add	r0, r0, #1
	cmp	r0, r3
	bne	.Lpolygon_85
	add	sp, sp, #620
	@ sp needed
	pop	{r4, r5, r6, r7, r8, r9, r10, fp, lr}
	bx	lr
.Lpolygon_127:
	cmp	r8, #119
	movle	r7, #0
	ble	.Lpolygon_77
	mov	r2, r5
	mov	fp, lr
	mov	r8, #119
	mov	r7, #0
	str	r6, [sp, #4]
	b	.Lpolygon_76
.Lpolygon_41:
	ldr	r2, .Lpolygon_129+16
	ldr	lr, [sp, #4]
	add	r3, r2, lr, lsl #3
	ldr	r9, [r2, lr, lsl #3]
	ldrh	r2, [r3, #4]
	ldrh	r3, [r3, #6]
	mul	r2, r3, r2
	ldr	r3, .Lpolygon_129+40
	ldrh	r7, [r3]
	rsb	r3, r7, #32768
	cmp	r2, r3
	str	r2, [sp, #24]
	bhi	.Lpolygon_128
	lsrs	r3, r2, #2
	str	r3, [sp, #16]
	ldreq	r2, .Lpolygon_129+8
	beq	.Lpolygon_44
	sub	r3, r3, #1
	cmp	r3, #13
	ldr	r2, .Lpolygon_129+8
	ldr	r3, .Lpolygon_129+44
	bls	.Lpolygon_45
	add	r1, r2, r7
	add	lr, r3, r9
	eor	r1, r1, lr
	tst	r1, #7
	bne	.Lpolygon_45
	str	fp, [sp, #20]
	str	ip, [sp, #36]
	mov	fp, r3
	mov	ip, r2
	lsr	r1, lr, #2
	ands	r1, r1, #1
	str	r1, [sp, #8]
	ldrne	r1, [r3, r9]
	ldr	r4, [sp, #8]
	strne	r1, [r2, r7]
	ldr	r1, [sp, #16]
	and	lr, lr, #4
	sub	r6, r1, r4
	add	r1, r7, lr
	add	r1, r2, r1
	add	lr, lr, r9
	lsr	r4, r6, #1
	add	lr, r3, lr
	add	r5, r1, r4, lsl #3
.Lpolygon_47:
	ldmia	lr!, {r2-r3}
	stm	r1!, {r2-r3}
	cmp	r5, r1
	bne	.Lpolygon_47
	cmp	r6, r4, lsl #1
	mov	r2, ip
	mov	r3, fp
	ldr	ip, [sp, #36]
	ldr	fp, [sp, #20]
	beq	.Lpolygon_48
	ldr	r1, [sp, #8]
	add	r4, r1, r4, lsl #1
	add	r1, r9, r4, lsl #2
	ldr	r5, [sp, #16]
	ldr	lr, [r3, r1]
	add	r1, r4, #1
	cmp	r5, r1
	add	r1, r7, r4, lsl #2
	str	lr, [r2, r1]
	lsl	lr, r4, #2
	bls	.Lpolygon_48
	add	r4, r4, #2
	add	r1, lr, #4
	cmp	r5, r4
	add	r4, r1, r9
	ldr	r4, [r3, r4]
	add	r1, r7, r1
	str	r4, [r2, r1]
	bls	.Lpolygon_48
	add	lr, lr, #8
	add	r1, lr, r9
	ldr	r3, [r3, r1]
	add	lr, r7, lr
	str	r3, [r2, lr]
.Lpolygon_48:
	ldr	r3, [sp, #28]
	ldr	r5, [r0, r3, lsl #2]
	ldr	r3, [sp, #12]
	ldr	lr, [r0]
	ldr	r4, [r3, #4]
	ldr	r3, [fp]
	ldr	r1, [r0, #4]
	ldr	r6, [fp, #4]
	sub	r3, r3, lr
	str	lr, [sp, #20]
	str	r3, [sp, #8]
	sub	r5, r5, lr
	sub	r6, r6, r1
	sub	r4, r4, r1
.Lpolygon_44:
	ldr	r3, .Lpolygon_129
	ldr	lr, [sp, #32]
	strh	r7, [r3, lr]	@ movhi
	ldr	r3, [sp, #24]
	ldr	lr, .Lpolygon_129+40
	add	r3, r7, r3
	strh	r3, [lr]	@ movhi
	add	r3, r2, r7
	str	r3, [sp, #84]
	b	.Lpolygon_42
.Lpolygon_128:
	ldr	r3, .Lpolygon_129+44
	add	r3, r9, r3
	str	r3, [sp, #84]
	b	.Lpolygon_42
.Lpolygon_45:
	ldr	r1, [sp, #16]
	ldr	lr, .Lpolygon_129+48
	add	r3, r3, r9
	add	r1, r3, r1, lsl #2
	add	lr, lr, r7
.Lpolygon_49:
	ldr	r4, [r3], #4
	cmp	r1, r3
	str	r4, [lr, #4]!
	bne	.Lpolygon_49
	b	.Lpolygon_48
.Lpolygon_130:
	.align	2
.Lpolygon_129:
	.word	texture_cache_offsets
	.word	65535
	.word	texture_cache
	.word	clipping_reciprocal_q24
	.word	bsp_textures
	.word	32767
	.word	reciprocal_q24
	.word	reciprocal_q16_small.0
	.word	texture_coverage
	.word	logical_framebuffer
	.word	texture_cache_used
	.word	bsp_texture_pixels
	.word	texture_cache-4
.size draw_textured_polygon_arm, .-draw_textured_polygon_arm
