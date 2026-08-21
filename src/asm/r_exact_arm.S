/* Cycle-exact mid-scanline affine feeding.
 *
 * raster_staged_frame flickers because both of its clocks wobble: the
 * per-command wait is a three-instruction poll with ~7 cycles of granularity,
 * and the per-line timer is restarted from a DISPSTAT poll with the same
 * granularity, so every seam lands within about +-2 pixels of where it was
 * asked to and lands somewhere different next frame. This routine removes
 * both sources rather than shrinking them:
 *
 *   - Timer 0 is restarted by HBLANK DMA, not by the CPU. DMA channels 0 and
 *     1 each write one 32-bit constant to TM0D/TM0CNT at every HBlank --
 *     channel 0 disables, channel 1 re-enables, and the enable edge reloads
 *     the counter. The DMA trigger and transfer time are hardware-fixed, so
 *     the timer's zero is the same distance from the true HBlank on every
 *     line of every frame. The CPU never touches the timer.
 *   - Each command waits coarsely until its deadline is near, then reads the
 *     timer ONCE and branches into a NOP sled by the remaining count. After
 *     the read the instruction stream is deterministic, so the register
 *     writes land on the same cycle every time. Any constant offset shifts
 *     every seam by the same amount, which is invisible; only variance ever
 *     was visible.
 *
 * In:   r0  160 consecutive 196-byte scanline records, format as
 *           raster_staged_frame: +0 uint16 WIN0H, +2 uint16 count,
 *           +4 commands of 16 bytes {u16 target cycle in HDraw, i16 PA,
 *           i16 PC, pad, i32 X, i32 Y}.
 * Out:  none. Runs for one frame, returns after line 159.
 * Clobbers: r0-r12. Owns timer 0 and DMA channels 0 and 1 for the frame.
 *
 * Assumptions:
 *   - Target cycles are measured from the start of HDraw; the timer zero is
 *     the start of the PREVIOUS HBlank, so 272 cycles are added, minus the
 *     fixed cost of the read-compute-branch tail, calibrated below.
 *   - Commands at least 12 cycles apart (3 pixels); the four register
 *     writes plus the sled entry need that long.
 *   - HBlank DMA does not fire during VBlank, so line 0 has no fresh anchor
 *     and must carry no commands. The quad data never draws line 0.
 *   - Interrupts are off; nothing else uses DMA0/DMA1 while this runs.
 */
.equ IO_BASE,            0x04000000
.equ REG_VCOUNT_OFFSET,  0x06
.equ REG_WIN0H_OFFSET,   0x40
.equ REG_BG2PA_OFFSET,   0x20
.equ REG_BG2PC_OFFSET,   0x24
.equ REG_BG2X_OFFSET,    0x28
.equ REG_BG2Y_OFFSET,    0x2c
.equ TIMER0_BASE,        0x04000100
.equ DMA0_BASE,          0x040000b0
/* enable(31) | hblank(29:28=10) | 32-bit(26) | repeat(25) | src fixed
 * (24:23=10) | dest fixed (22:21=10) | one unit. */
.equ DMA_TIMER_KICK,     0xa7400001
.equ HBLANK_CYCLES,      272
/* Fixed cycles between the timer read and the first register write landing,
 * through the compute and sled entry. A constant error only shifts every
 * seam uniformly; this value centres the sled so delays stay in range. */
.equ TAIL_OVERHEAD,      16
.equ SLED_SLOTS,         64
.equ COMMANDS_OFFSET,    4

.section .iwram, "ax"
.arm
.align 2
.global raster_exact_frame
.type raster_exact_frame, %function
raster_exact_frame:
    push    {r4-r11, lr}
    mov     r9, r0
    ldr     r4, =IO_BASE
    ldr     r3, =TIMER0_BASE

    /* Timer 0 free-running at full speed, reload 0; the DMA pair restarts it
     * every HBlank from here on. */
    mov     r0, #0
    strh    r0, [r3, #2]
    strh    r0, [r3]
    mov     r0, #0x80
    strh    r0, [r3, #2]
    ldr     r0, =timer_stop_word
    ldr     r1, =DMA0_BASE
    str     r0, [r1]                @ DMA0SAD
    add     r2, r3, #0              @ DMA0DAD = TM0D/TM0CNT
    str     r2, [r1, #4]
    ldr     r0, =DMA_TIMER_KICK
    str     r0, [r1, #8]
    ldr     r0, =timer_start_word
    str     r0, [r1, #12]           @ DMA1SAD
    str     r2, [r1, #16]
    ldr     r0, =DMA_TIMER_KICK
    str     r0, [r1, #20]

    ldr     r10, =exact_scanline
    mov     r5, #0

.Lnext_line:
    /* Stage the record into IWRAM while the previous line draws. */
    mov     r11, r10
    mov     r7, #12
1:  ldmia   r9!, {r0-r2, r6}
    stmia   r11!, {r0-r2, r6}
    subs    r7, r7, #1
    bne     1b
    ldr     r0, [r9], #4
    str     r0, [r11]

    ldrh    r0, [r10]
    strh    r0, [r4, #REG_WIN0H_OFFSET]
    ldrh    r8, [r10, #2]
    cmp     r8, #0
    beq     .Lline_done_waitless
    /* Command zero before the line starts, as the staged routine does. */
    add     r11, r10, #COMMANDS_OFFSET
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #REG_BG2PA_OFFSET]
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #REG_BG2PC_OFFSET]
    ldr     r0, [r11, #8]
    str     r0, [r4, #REG_BG2X_OFFSET]
    ldr     r0, [r11, #12]
    str     r0, [r4, #REG_BG2Y_OFFSET]

    /* Reach the target line. Poll granularity here only selects the line;
     * the deadlines below come from the DMA-anchored timer. */
2:  ldrh    r0, [r4, #REG_VCOUNT_OFFSET]
    cmp     r0, r5
    bne     2b

    mov     r6, #1
.Lnext_command:
    cmp     r6, r8
    bhs     .Lline_done
    add     r11, r10, r6, lsl #4
    add     r11, r11, #COMMANDS_OFFSET
    ldrh    r7, [r11]
    add     r7, r7, #HBLANK_CYCLES - TAIL_OVERHEAD
    sub     r12, r7, #SLED_SLOTS - 8
3:  ldrh    r0, [r3]                @ coarse: within sled range?
    cmp     r0, r12
    blo     3b
    ldrh    r0, [r3]                @ the one exact read
    subs    r0, r7, r0              @ cycles still to burn
    movlt   r0, #0
    cmp     r0, #SLED_SLOTS - 1
    movgt   r0, #SLED_SLOTS - 1
    rsb     r0, r0, #SLED_SLOTS - 1 @ slots to skip
    add     pc, pc, r0, lsl #2
    nop                             @ pipeline slot of the add above
.rept SLED_SLOTS
    nop
.endr
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #REG_BG2PA_OFFSET]
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #REG_BG2PC_OFFSET]
    ldr     r0, [r11, #8]
    str     r0, [r4, #REG_BG2X_OFFSET]
    ldr     r0, [r11, #12]
    str     r0, [r4, #REG_BG2Y_OFFSET]
    add     r6, r6, #1
    b       .Lnext_command

.Lline_done:
    /* Past the last command; wait out the line so staging cannot outrun the
     * beam. The timer resets at HBlank, so "still after the last deadline"
     * doubles as "still this line". */
.Lline_done_waitless:
4:  ldrh    r0, [r4, #REG_VCOUNT_OFFSET]
    cmp     r0, r5
    beq     4b
    add     r5, r5, #1
    cmp     r5, #160
    blo     .Lnext_line

    /* Give the timer and DMA back. */
    mov     r0, #0
    ldr     r1, =DMA0_BASE
    str     r0, [r1, #8]
    str     r0, [r1, #20]
    strh    r0, [r3, #2]
    pop     {r4-r11, lr}
    bx      lr
.size raster_exact_frame, .-raster_exact_frame

.align 2
/* 32-bit constants the DMA pair writes over TM0D+TM0CNT: low half is the
 * reload value, high half the control. Disable, then enable; the 0-to-1
 * enable edge reloads the counter. */
timer_stop_word:
    .word 0x00000000
timer_start_word:
    .word 0x00800000
exact_scanline:
    .space 196
