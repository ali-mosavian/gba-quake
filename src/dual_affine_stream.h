#ifndef DUAL_AFFINE_STREAM_H
#define DUAL_AFFINE_STREAM_H

#include <stdint.h>

enum {
    DUAL_SCREEN_HEIGHT = 160,
    DUAL_MAX_COMMANDS = 12,
    DUAL_SCANLINE_BYTES = 200,
    DUAL_COMMAND_PREINSTALL = 1u << 1,
};

typedef struct {
    uint16_t at_cycle;
    uint16_t flags; /* bit 0: BG3 rather than BG2; bit 1: install in HBlank */
    int16_t pa;
    int16_t pc;
    int32_t reference_x;
    int32_t reference_y;
} DualAffineCommand;

typedef struct {
    uint16_t window_0_horizontal;
    uint16_t window_1_horizontal;
    uint16_t command_count;
    uint16_t padding;
    DualAffineCommand commands[DUAL_MAX_COMMANDS];
} DualAffineScanline;

typedef struct {
    DualAffineScanline scanlines[DUAL_SCREEN_HEIGHT];
} DualAffineFrame;

_Static_assert(sizeof(DualAffineCommand) == 16, "dual command stride");
_Static_assert(sizeof(DualAffineScanline) == DUAL_SCANLINE_BYTES,
               "dual scanline stride");

#endif
