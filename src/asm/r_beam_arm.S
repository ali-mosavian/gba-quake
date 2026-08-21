/* Beam-raced display list: the PPU draws a whole frame of polygon spans.
 *
 * The display list lives in IWRAM: one command per span, sixteen bytes,
 * {u16 issue cycle within HDraw, i16 PA, i16 PC, u16 pad, i32 X, i32 Y}.
 * The frame is 120 logical pixels doubled by the affine matrix, so one
 * logical pixel is eight beam cycles, and each logical line's commands are
 * fed twice, on both screen lines of the pair. PB = PD = 0 keeps the PPU
 * from advancing the reference point on its own.
 *
 * Timing is the mechanism hardware validated in raster_exact_frame: timer 0
 * restarted every HBlank by two DMA channels (hardware-fixed latency), each
 * command issued through one timer read and a computed NOP sled. Command
 * zero of every line is installed during the preceding HBlank.
 *
 * In:   r0  uint8 counts[lines]   commands per logical line
 *       r1  command stream, 16 bytes each, lines back to back
 *       r2  logical line count (80)
 * Out:  none. Runs one full display frame.
 * Clobbers: r0-r12. Owns timer 0 and DMA channels 0 and 1 for the frame.
 *
 * Assumptions:
 *   - Screen lines 0 and 1 have no fresh HBlank anchor (HBlank DMA pauses
 *     in VBlank); their commands after the first are issued unslept. One
 *     logical line at the top of the screen carries that error.
 *   - Commands are at least 24 cycles (3 logical pixels) apart; the
 *     generator enforces it.
 */
.equ IO_BASE,            0x04000000
.equ REG_DISPSTAT_OFF,   0x04
.equ REG_VCOUNT_OFF,     0x06
.equ REG_BG2PA_OFF,      0x20
.equ REG_BG2PC_OFF,      0x24
.equ REG_BG2X_OFF,       0x28
.equ REG_BG2Y_OFF,       0x2c
.equ TIMER0_BASE,        0x04000100
.equ DMA0_BASE,          0x040000b0
.equ DMA_TIMER_KICK,     0xa7400001
.equ HBLANK_CYCLES,      272
.equ TAIL_OVERHEAD,      16
.equ SLED_SLOTS,         64
.equ DISPSTAT_HBLANK,    0x02

.section .iwram, "ax"
.arm
.align 2
.global raster_beam_frame
.type raster_beam_frame, %function
raster_beam_frame:
    push    {r4-r11, lr}
    mov     r9, r0                  @ counts
    mov     r10, r1                 @ command stream cursor
    mov     r14, r2                 @ logical lines remaining
    ldr     r4, =IO_BASE
    ldr     r3, =TIMER0_BASE

    mov     r0, #0                  @ timer free-running, DMA pair restarts it
    strh    r0, [r3, #2]
    strh    r0, [r3]
    mov     r0, #0x80
    strh    r0, [r3, #2]
    ldr     r1, =DMA0_BASE
    ldr     r0, =timer_stop_word
    str     r0, [r1]
    mov     r2, r3
    str     r2, [r1, #4]
    ldr     r0, =DMA_TIMER_KICK
    str     r0, [r1, #8]
    ldr     r0, =timer_start_word
    str     r0, [r1, #12]
    str     r2, [r1, #16]
    ldr     r0, =DMA_TIMER_KICK
    str     r0, [r1, #20]

    mov     r5, #0                  @ screen line

.Lnext_logical_line:
    ldrb    r8, [r9], #1            @ commands on this logical line
    mov     r6, #2                  @ two screen lines per logical line
.Lfeed_pass:
    mov     r11, r10                @ rewind to the line's first command

    /* Wait for the HBlank before the target screen line, then install
     * command zero so the left edge is correct from the first pixel. */
1:  ldrh    r0, [r4, #REG_VCOUNT_OFF]
    add     r0, r0, #1
    cmp     r0, r5
    bne     1b
2:  ldrh    r0, [r4, #REG_DISPSTAT_OFF]
    tst     r0, #DISPSTAT_HBLANK
    beq     2b
    cmp     r8, #0
    beq     .Lpass_done
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #REG_BG2PA_OFF]
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #REG_BG2PC_OFF]
    ldr     r0, [r11, #8]
    str     r0, [r4, #REG_BG2X_OFF]
    ldr     r0, [r11, #12]
    str     r0, [r4, #REG_BG2Y_OFF]
    add     r11, r11, #16
    mov     r7, #1

.Lnext_command:
    cmp     r7, r8
    bhs     .Lpass_done
    ldrh    r0, [r11]               @ issue cycle within HDraw
    add     r12, r0, #HBLANK_CYCLES - TAIL_OVERHEAD
    sub     r2, r12, #SLED_SLOTS - 8
3:  ldrh    r0, [r3]
    cmp     r0, r2
    blo     3b
    ldrh    r0, [r3]
    subs    r0, r12, r0
    movlt   r0, #0
    cmp     r0, #SLED_SLOTS - 1
    movgt   r0, #SLED_SLOTS - 1
    rsb     r0, r0, #SLED_SLOTS - 1
    add     pc, pc, r0, lsl #2
    nop
.rept SLED_SLOTS
    nop
.endr
    ldrsh   r0, [r11, #2]
    strh    r0, [r4, #REG_BG2PA_OFF]
    ldrsh   r0, [r11, #4]
    strh    r0, [r4, #REG_BG2PC_OFF]
    ldr     r0, [r11, #8]
    str     r0, [r4, #REG_BG2X_OFF]
    ldr     r0, [r11, #12]
    str     r0, [r4, #REG_BG2Y_OFF]
    add     r11, r11, #16
    add     r7, r7, #1
    b       .Lnext_command

.Lpass_done:
    add     r5, r5, #1
    subs    r6, r6, #1
    bne     .Lfeed_pass
    mov     r0, r8
    add     r10, r10, r0, lsl #4    @ past this line's commands
    subs    r14, r14, #1
    bne     .Lnext_logical_line

    mov     r0, #0                  @ release DMA and the timer
    ldr     r1, =DMA0_BASE
    str     r0, [r1, #8]
    str     r0, [r1, #20]
    strh    r0, [r3, #2]
    pop     {r4-r11, lr}
    bx      lr
.size raster_beam_frame, .-raster_beam_frame

.align 2
timer_stop_word:
    .word 0x00000000
timer_start_word:
    .word 0x00800000
