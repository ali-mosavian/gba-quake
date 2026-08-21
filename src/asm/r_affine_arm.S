/* Legacy affine-background raster-racing experiments and register streams. */
.syntax unified
.cpu arm7tdmi
.arm
.section .iwram,"ax",%progbits
.align 2
/* Hardware and stream-layout constants shared conceptually with
 * gba_hardware.h and quad_stream.h. Keep the static assertions in the C header
 * in sync if this layout changes. */
.equ IO_BASE,                 0x04000000
.equ TIMER0_BASE,             0x04000100
.equ REG_DISPSTAT_OFFSET,     0x04
.equ REG_VCOUNT_OFFSET,       0x06
.equ REG_BG2PA_OFFSET,        0x20
.equ REG_BG2PC_OFFSET,        0x24
.equ REG_BG2X_OFFSET,         0x28
.equ REG_BG2Y_OFFSET,         0x2c
.equ REG_WIN0H_OFFSET,        0x40
.equ DISPSTAT_HBLANK,         0x02
.equ TIMER_ENABLE_FULL_SPEED, 0x80
.equ SCANLINE_BYTES,          196
.equ COMMAND_BYTES,           16
.equ COMMANDS_OFFSET,         4
/* raster_frame - hardware probe: issue eight timed affine updates per line.
 *
 * In:   r0  test id: 1 = pa_pc only, 2 = xy only, 3 = combined32 immediates,
 *           anything else = full stream (stream32 / window)
 *       r1  EWRAM command stream, 8 records of 16 bytes:
 *             +0 uint16 target cycle   +2 int16 BG2PA   +4 int16 BG2PC
 *             +8 int32 BG2X           +12 int32 BG2Y
 *       r2  EWRAM uint16[8] receiving the cycle each update actually issued
 * Out:  none; r2[] is filled with observed issue cycles
 * Clobbers: r0-r12.
 *
 * Assumptions:
 *   - Busy-waits on VCOUNT then on DISPSTAT leaving HBlank, so the timer zero
 *     origin is our observed start of HDraw, not the true pixel-zero. The
 *     observed cycles therefore include the setup cost and cannot read 0.
 *   - Timer 0 runs at one tick per CPU cycle; four cycles is one pixel.
 *   - Test id 3 deliberately avoids EWRAM reads so assumption 3 (register
 *     writes land mid-scanline) is tested without a memory dependency.
 *   - Instrumented lines are 8..151 only.
 *   - Returns with `bx lr` because ARM7TDMI `pop {..., pc}` does NOT restore
 *     Thumb state; using pc here crashes the Thumb caller. */
.global raster_frame
.type raster_frame, %function
raster_frame:
    push    {r4-r11, lr}
    mov     r8, r0                  @ test id
    mov     r9, r1                  @ EWRAM command stream
    mov     r10, r2                 @ EWRAM observed timestamps
    ldr     r4, =0x04000000
    ldr     r3, =0x04000100         @ timer 0 base (halfword offsets are limited)
    mov     r5, #8                  @ first instrumented line

1:  ldrh    r0, [r4, #6]           @ VCOUNT
    cmp     r0, r5
    bne     1b
2:  ldrh    r0, [r4, #4]           @ wait until not HBlank: visible x~=0
    tst     r0, #2
    bne     2b

    mov     r0, #0
    strh    r0, [r3, #2]           @ timer 0 stop
    strh    r0, [r3]
    mov     r0, #0x80               @ timer enable, 1 cycle/tick
    strh    r0, [r3, #2]
    mov     r6, #0                  @ command index

3:  add     r11, r9, r6, lsl #4    @ sizeof(Command) = 16
    cmp     r8, #3                  @ combined32 uses immediate values
    moveq   r7, r6, lsl #7          @ no EWRAM dependency: 128 cycles each
    ldrhne  r7, [r11]               @ stream32/window read the command
4:  ldrh    r0, [r3]
    cmp     r0, r7
    blo     4b
    add     r12, r10, r6, lsl #1
    strh    r0, [r12]               @ actual cycle of update

    cmp     r8, #3
    beq     8f
    cmp     r8, #2
    beq     6f
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #0x20]        @ BG2PA
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #0x24]        @ BG2PC
    cmp     r8, #1
    beq     7f
6:  ldr     r0, [r11, #8]
    str     r0, [r4, #0x28]        @ BG2X
    ldr     r0, [r11, #12]
    str     r0, [r4, #0x2c]        @ BG2Y
    b       7f
8:  tst     r6, #1                  @ hard-coded combined test pattern
    moveq   r0, #0x180
    movne   r0, #0x80
    strh    r0, [r4, #0x20]
    moveq   r0, #-0x40
    movne   r0, #0x40
    strh    r0, [r4, #0x24]
    mov     r0, r6, lsl #13         @ i * 32 in 24.8 fixed point
    str     r0, [r4, #0x28]
    mov     r0, r6, lsl #11         @ i * 8 in 24.8 fixed point
    str     r0, [r4, #0x2c]
7:  add     r6, r6, #1
    cmp     r6, #8
    blo     3b

    add     r5, r5, #1
    cmp     r5, #152
    blo     1b
    mov     r0, #0
    strh    r0, [r3, #2]
    pop     {r4-r11, lr}
    bx      lr                      @ ARM7TDMI: switch back to Thumb explicitly
.size raster_frame, .-raster_frame

/* raster_cube_frame - falsified experiment: race the affine registers for a
 * multi-face cube. Retained so the failure stays reproducible.
 *
 * In:   r0  160 CubeLine records of 196 bytes each:
 *             +0 uint16 WIN0H   +2 uint16 command count
 *             +4 up to 12 commands of 16 bytes, layout as in raster_frame
 * Out:  none
 * Clobbers: r0-r12.
 *
 * Assumptions:
 *   - WIN0H is installed after VCOUNT matches, which is already inside the
 *     line; this is one reason the left edge is unstable.
 *   - Commands are read from EWRAM DURING the visible period. That is the
 *     defect this routine documents: the reads miss their deadlines and the
 *     output flickers even with a frozen command buffer.
 *   - Superseded by raster_staged_frame. Do not use for new work. */
.align 2
.global raster_cube_frame
.type raster_cube_frame, %function
raster_cube_frame:
    push    {r4-r11, lr}
    mov     r9, r0                  @ current line record
    ldr     r4, =0x04000000
    ldr     r3, =0x04000100
    mov     r5, #0
9:  ldrh    r0, [r4, #6]
    cmp     r0, r5
    bne     9b
    ldrh    r0, [r9]
    strh    r0, [r4, #0x40]        @ WIN0H for this scanline
10: ldrh    r0, [r4, #4]
    tst     r0, #2
    bne     10b
    mov     r0, #0
    strh    r0, [r3, #2]
    strh    r0, [r3]
    mov     r0, #0x80
    strh    r0, [r3, #2]
    mov     r6, #0
    ldrh    r8, [r9, #2]
    cmp     r8, #0
    beq     13f
11: add     r11, r9, r6, lsl #4
    add     r11, r11, #4
    ldrh    r7, [r11]
12: ldrh    r0, [r3]
    cmp     r0, r7
    blo     12b
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #0x20]
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #0x24]
    ldr     r0, [r11, #8]
    str     r0, [r4, #0x28]
    ldr     r0, [r11, #12]
    str     r0, [r4, #0x2c]
    add     r6, r6, #1
    cmp     r6, r8
    blo     11b
13: add     r9, r9, #196
    add     r5, r5, #1
    cmp     r5, #160
    blo     9b
    mov     r0, #0
    strh    r0, [r3, #2]
    pop     {r4-r11, lr}
    bx      lr
.size raster_cube_frame, .-raster_cube_frame

/* raster_staged_frame - the affine path that actually works in mGBA.
 *
 * In:   r0  160 consecutive 196-byte scanline records in ROM or EWRAM:
 *             +0 uint16 WIN0H   +2 uint16 command count (0..12)
 *             +4 up to 12 commands of 16 bytes:
 *                  +0 uint16 target cycle within HDraw
 *                  +2 int16 BG2PA   +4 int16 BG2PC
 *                  +8 int32 BG2X   +12 int32 BG2Y
 * Out:  none
 * Clobbers: r0-r12. Runs for one whole frame and returns after line 159.
 *
 * Timing contract, in order, per scanline:
 *   1. Copy the 196-byte record into the IWRAM `staged_scanline` buffer.
 *   2. Install WIN0H and command zero BEFORE HDraw begins.
 *   3. Wait for VCOUNT, then for DISPSTAT to leave HBlank.
 *   4. Issue commands 1..N against timer 0, reading only IWRAM.
 *
 * Assumptions:
 *   - No EWRAM or ROM access happens during the visible period. Breaking this
 *     is what made the earlier polling version flicker.
 *   - Record size is exactly 196 bytes = 12 blocks of 16 plus one word; the
 *     copy loop is hard-coded to that shape and quad_stream.h carries the
 *     matching compile-time size checks.
 *   - Command target cycles must be at least ~8 pixels (32 cycles) apart, or
 *     the four register writes cannot complete before the next deadline.
 *   - The caller must publish a whole new frame pointer only during VBlank;
 *     this routine does not double buffer.
 *   - Timer 0 is reset per scanline and is owned exclusively by this routine.
 */
.align 2
.global raster_staged_frame
.type raster_staged_frame, %function
raster_staged_frame:
    push    {r4-r11, lr}
    mov     r9, r0                  @ next ROM scanline record
    ldr     r4, =IO_BASE
    ldr     r3, =TIMER0_BASE
    ldr     r10, =staged_scanline   @ IWRAM working copy
    mov     r5, #0                  @ destination screen y

.Lstage_next_scanline:
    /* Copy exactly 196 bytes: twelve 16-byte blocks plus one final word. */
    mov     r11, r10
    mov     r7, #12
.Lcopy_scanline_blocks:
    ldmia   r9!, {r0-r2, r6}
    stmia   r11!, {r0-r2, r6}
    subs    r7, r7, #1
    bne     .Lcopy_scanline_blocks
    ldr     r0, [r9], #4
    str     r0, [r11]

    /* Window and command zero are installed before HDraw starts. This avoids
     * both a late left edge and visible-period accesses to slower ROM/EWRAM. */
    ldrh    r0, [r10]
    strh    r0, [r4, #REG_WIN0H_OFFSET]
    ldrh    r8, [r10, #2]
    cmp     r8, #0
    beq     .Lwait_for_target_line
    add     r11, r10, #COMMANDS_OFFSET
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #REG_BG2PA_OFFSET]
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #REG_BG2PC_OFFSET]
    ldr     r0, [r11, #8]
    str     r0, [r4, #REG_BG2X_OFFSET]
    ldr     r0, [r11, #12]
    str     r0, [r4, #REG_BG2Y_OFFSET]

.Lwait_for_target_line:
    ldrh    r0, [r4, #REG_VCOUNT_OFFSET]
    cmp     r0, r5
    bne     .Lwait_for_target_line
.Lwait_for_hdraw:
    ldrh    r0, [r4, #REG_DISPSTAT_OFFSET]
    tst     r0, #DISPSTAT_HBLANK
    bne     .Lwait_for_hdraw

    /* Timer zero measures cycles from our observed start of HDraw. */
    mov     r0, #0
    strh    r0, [r3, #2]
    strh    r0, [r3]
    mov     r0, #TIMER_ENABLE_FULL_SPEED
    strh    r0, [r3, #2]
    mov     r6, #1                  @ command zero was installed above

.Lissue_visible_commands:
    cmp     r6, r8
    bhs     .Lwait_for_hblank
    add     r11, r10, r6, lsl #4   @ COMMAND_BYTES == 16
    add     r11, r11, #COMMANDS_OFFSET
    ldrh    r7, [r11]
.Lwait_for_command_cycle:
    ldrh    r0, [r3]
    cmp     r0, r7
    blo     .Lwait_for_command_cycle
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #REG_BG2PA_OFFSET]
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #REG_BG2PC_OFFSET]
    ldr     r0, [r11, #8]
    str     r0, [r4, #REG_BG2X_OFFSET]
    ldr     r0, [r11, #12]
    str     r0, [r4, #REG_BG2Y_OFFSET]
    add     r6, r6, #1
    b       .Lissue_visible_commands

.Lwait_for_hblank:
    ldrh    r0, [r4, #REG_DISPSTAT_OFFSET]
    tst     r0, #DISPSTAT_HBLANK
    beq     .Lwait_for_hblank
    add     r5, r5, #1
    cmp     r5, #160
    blo     .Lstage_next_scanline
    mov     r0, #0
    strh    r0, [r3, #2]
    pop     {r4-r11, lr}
    bx      lr
.size raster_staged_frame, .-raster_staged_frame

.align 2
staged_scanline:
    .space 196

/* raster_dual_frame - two affine layers, one window each, for adjacent faces.
 *
 * In:   r0  160 consecutive 200-byte scanline records in EWRAM:
 *             +0 uint16 WIN0H   +2 uint16 WIN1H   +4 uint16 command count
 *             +8 commands of 16 bytes:
 *                  +0 uint16 target cycle
 *                  +2 uint16 flags: bit0 layer (0 = BG2, 1 = BG3),
 *                                   bit1 preinstall during HBlank
 *                  +4 int16 PA   +6 int16 PC   +8 int32 X   +12 int32 Y
 * Out:  none
 * Clobbers: r0-r12, and DMA3.
 *
 * Assumptions:
 *   - DMA3 moves the 50 words into IWRAM because a CPU copy plus two
 *     preinstalled mappings does not fit in HBlank when the source is EWRAM.
 *     DMA3 is therefore reserved for this routine for the whole frame.
 *   - BG3's affine registers sit exactly 0x10 above BG2's, which is what the
 *     `layer << 4` offset relies on.
 *   - Layer ownership must be decided once per generated frame, not per
 *     scanline. Choosing the two widest faces per line makes the second layer
 *     alternate near a three-face corner and produces a visible zipper.
 *   - Only two independently mapped surfaces per scanline are representable;
 *     a third visible face is dropped. That is a hardware limit, not a bug.
 *   - Experimental. The supported interactive path is the software renderer. */
.align 2
.global raster_dual_frame
.type raster_dual_frame, %function
raster_dual_frame:
    push    {r4-r11, lr}
    mov     r9, r0
    ldr     r4, =IO_BASE
    ldr     r3, =TIMER0_BASE
    ldr     r10, =dual_staged_scanline
    mov     r5, #0

.Ldual_next_line:
    /* A CPU copy plus two preinstalled mappings does not reliably fit in
     * HBlank when the source is EWRAM. DMA3 moves the 50 words synchronously;
     * the CPU remains responsible for every timing-sensitive register write. */
    ldr     r2, =0x040000d4         @ DMA3SAD
    str     r9, [r2]
    str     r10, [r2, #4]
    ldr     r0, =0x84000032         @ enable, 32-bit, 50 words
    str     r0, [r2, #8]
    add     r9, r9, #200

    ldrh    r0, [r10]
    strh    r0, [r4, #0x40]        @ WIN0H
    ldrh    r0, [r10, #2]
    strh    r0, [r4, #0x42]        @ WIN1H
    ldrh    r8, [r10, #4]          @ command count
    mov     r6, #0
.Ldual_preinstall_loop:
    cmp     r6, r8
    bhs     .Ldual_wait_line
    add     r11, r10, r6, lsl #4
    add     r11, r11, #8
    ldrh    r7, [r11, #2]
    tst     r7, #2
    beq     .Ldual_preinstall_next
    and     r7, r7, #1
    mov     r7, r7, lsl #4         @ BG3 registers are BG2 + 0x10
    ldrsh   r0, [r11, #4]
    add     r12, r4, r7
    strh    r0, [r12, #0x20]
    ldrsh   r0, [r11, #6]
    strh    r0, [r12, #0x24]
    ldr     r0, [r11, #8]
    str     r0, [r12, #0x28]
    ldr     r0, [r11, #12]
    str     r0, [r12, #0x2c]
.Ldual_preinstall_next:
    add     r6, r6, #1
    b       .Ldual_preinstall_loop

.Ldual_wait_line:
    ldrh    r0, [r4, #REG_VCOUNT_OFFSET]
    cmp     r0, r5
    bne     .Ldual_wait_line
.Ldual_wait_hdraw:
    ldrh    r0, [r4, #REG_DISPSTAT_OFFSET]
    tst     r0, #DISPSTAT_HBLANK
    bne     .Ldual_wait_hdraw
    mov     r0, #0
    strh    r0, [r3, #2]
    strh    r0, [r3]
    mov     r0, #TIMER_ENABLE_FULL_SPEED
    strh    r0, [r3, #2]
    mov     r6, #0

.Ldual_visible_loop:
    cmp     r6, r8
    bhs     .Ldual_wait_hblank
    add     r11, r10, r6, lsl #4
    add     r11, r11, #8
    ldrh    r7, [r11, #2]
    tst     r7, #2
    bne     .Ldual_visible_next
    ldrh    r12, [r11]
.Ldual_wait_cycle:
    ldrh    r0, [r3]
    cmp     r0, r12
    blo     .Ldual_wait_cycle
    and     r7, r7, #1
    mov     r7, r7, lsl #4
    add     r12, r4, r7
    ldrsh   r0, [r11, #4]
    strh    r0, [r12, #0x20]
    ldrsh   r0, [r11, #6]
    strh    r0, [r12, #0x24]
    ldr     r0, [r11, #8]
    str     r0, [r12, #0x28]
    ldr     r0, [r11, #12]
    str     r0, [r12, #0x2c]
.Ldual_visible_next:
    add     r6, r6, #1
    b       .Ldual_visible_loop

.Ldual_wait_hblank:
    ldrh    r0, [r4, #REG_DISPSTAT_OFFSET]
    tst     r0, #DISPSTAT_HBLANK
    beq     .Ldual_wait_hblank
    add     r5, r5, #1
    cmp     r5, #160
    blo     .Ldual_next_line
    mov     r0, #0
    strh    r0, [r3, #2]
    pop     {r4-r11, lr}
    bx      lr
.size raster_dual_frame, .-raster_dual_frame

.align 2
dual_staged_scanline:
    .space 200
