#ifndef QUAD_STREAM_H
#define QUAD_STREAM_H

#include <stdint.h>

enum {
    SCREEN_HEIGHT = 160,
    MAX_COMMANDS_PER_LINE = 12,
    QUAD_LINE_BYTES = 196,
};

/* One mid-scanline affine-background update. */
typedef struct {
    uint16_t at_cycle;       /* Cycles from the beginning of HDraw. */
    int16_t pa;              /* Signed 8.8 texture-U step per pixel. */
    int16_t pc;              /* Signed 8.8 texture-V step per pixel. */
    uint16_t alignment_padding;
    int32_t reference_x;     /* Signed 24.8 reference point at screen x=0. */
    int32_t reference_y;
} AffineCommand;

/* Complete display-list record for one scanline. */
typedef struct {
    uint16_t window_horizontal;
    uint16_t command_count;
    AffineCommand commands[MAX_COMMANDS_PER_LINE];
} QuadScanline;

typedef struct {
    QuadScanline scanlines[SCREEN_HEIGHT];
} QuadFrame;

_Static_assert(sizeof(AffineCommand) == 16, "assembly command stride");
_Static_assert(sizeof(QuadScanline) == QUAD_LINE_BYTES,
               "assembly scanline stride");

#endif
