/* Logical framebuffer clear and Mode 4 pixel-doubling routines. */
.syntax unified
.cpu arm7tdmi
.arm
.section .iwram,"ax",%progbits
.align 2
/* clear_logical_framebuffer - zero the 120x80 logical framebuffer.
 *
 * In:   r0  base of the 9600-byte logical framebuffer, MUST be word aligned
 * Out:  none
 * Clobbers: r0, r1, r2.
 *
 * Assumptions:
 *   - The size is hard-coded as 2400 words (120 * 80 bytes). Changing
 *     SCREEN_WIDTH or SCREEN_HEIGHT in r_state.c requires changing the
 *     constant here as well; nothing checks it at build time.
 *   - Palette index 0 is the clear colour.
 *   - The buffer lives in EWRAM, so this costs one 16-bit bus pair per word;
 *     it is deliberately a straight-line store loop rather than DMA because
 *     the caller runs it outside VBlank. */
.global clear_logical_framebuffer
.type clear_logical_framebuffer, %function
clear_logical_framebuffer:
    push    {r4-r9, lr}
    mov     r1, #0
    mov     r2, #0
    mov     r3, #0
    mov     r4, #0
    mov     r5, #0
    mov     r6, #0
    mov     r7, #0
    mov     r8, #0
    mov     r9, #300               @ 2400 words / 8 per store multiple
1:  stmia   r0!, {r1-r8}
    subs    r9, r9, #1
    bne     1b
    pop     {r4-r9, lr}
    bx      lr
.size clear_logical_framebuffer, .-clear_logical_framebuffer

/* expand_logical_framebuffer - 2x pixel-double 120x80 into a Mode 4 page.
 *
 * In:   r0  base of the 120x80 8-bit logical framebuffer (word aligned)
 *       r1  base of the target Mode 4 page, 0x06000000 or 0x0600a000
 * Out:  none
 * Clobbers: r0, r1, r2, r3, r4, r5, r6.
 *
 * Assumptions:
 *   - Reads two source pixels at a time and writes one word to each of two
 *     consecutive destination rows, so every source pixel becomes a 2x2 block.
 *   - Dimensions are hard-coded: 80 source rows, 60 word-pairs per row, a
 *     240-byte destination stride. Kept in sync with r_state.c by hand.
 *   - VRAM cannot be written a byte at a time on GBA; every store here is a
 *     full word, which is also why the source must stay byte-packed.
 *   - The caller must pass the HIDDEN page. Writing the visible page tears. */
.global expand_logical_framebuffer
.type expand_logical_framebuffer, %function
expand_logical_framebuffer:
    push    {r4-r11, lr}
    mov     r4, #80                @ source rows
    mov     r12, #0xff
1:  mov     r3, #15                @ 15 groups of 8 source pixels = 120
    add     r2, r1, #240           @ the duplicated destination row
2:  ldmia   r0!, {r5, r6}          @ 8 source pixels, one sequential EWRAM burst
    /* b3b2b1b0 -> (b1b1b0b0, b3b3b2b2), twice. */
    and     r7, r12, r5
    orr     r7, r7, r7, lsl #8
    and     r11, r5, #0xff00
    orr     r7, r7, r11, lsl #8
    orr     r7, r7, r11, lsl #16
    mov     r11, r5, lsr #16
    and     r8, r12, r11
    orr     r8, r8, r8, lsl #8
    and     r11, r11, #0xff00
    orr     r8, r8, r11, lsl #8
    orr     r8, r8, r11, lsl #16
    and     r9, r12, r6
    orr     r9, r9, r9, lsl #8
    and     r11, r6, #0xff00
    orr     r9, r9, r11, lsl #8
    orr     r9, r9, r11, lsl #16
    mov     r11, r6, lsr #16
    and     r10, r12, r11
    orr     r10, r10, r10, lsl #8
    and     r11, r11, #0xff00
    orr     r10, r10, r11, lsl #8
    orr     r10, r10, r11, lsl #16
    stmia   r1!, {r7-r10}          @ sequential VRAM bursts beat single stores
    stmia   r2!, {r7-r10}
    subs    r3, r3, #1
    bne     2b
    add     r1, r1, #240
    subs    r4, r4, #1
    bne     1b
    pop     {r4-r11, lr}
    bx      lr
.size expand_logical_framebuffer, .-expand_logical_framebuffer
