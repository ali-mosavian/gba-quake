#include <stdint.h>

#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))
#define REG_DISPCNT REG16(0x04000000)
#define REG_VCOUNT REG16(0x04000006)
#define REG_BG2CNT REG16(0x0400000c)
#define REG_BG2PA REG16(0x04000020)
#define REG_BG2PB REG16(0x04000022)
#define REG_BG2PC REG16(0x04000024)
#define REG_BG2PD REG16(0x04000026)
#define REG_BG2X REG32(0x04000028)
#define REG_BG2Y REG32(0x0400002c)
#define REG_WIN0H REG16(0x04000040)
#define REG_WIN0V REG16(0x04000044)
#define REG_WININ REG16(0x04000048)
#define REG_WINOUT REG16(0x0400004a)
#define PAL ((volatile uint16_t *)0x05000000)
#define VRAM16 ((volatile uint16_t *)0x06000000)
#define MAP16 ((volatile uint16_t *)0x06004000)

typedef struct { uint16_t at; int16_t pa, pc; uint16_t pad; int32_t x, y; } CubeCmd;
typedef struct { uint16_t winh, count; CubeCmd cmd[12]; } CubeLine;
typedef struct { CubeLine line[160]; } CubeFrame;
_Static_assert(sizeof(CubeCmd) == 16, "assembly command stride");
_Static_assert(sizeof(CubeLine) == 196, "assembly line stride");

#include "generated/cube_frames.h"

extern void raster_cube_frame(const CubeLine *lines);

__attribute__((section(".ewram"), aligned(4)))
static CubeFrame frame_buffer;

static void copy_frame(unsigned index) {
    const uint32_t *src = (const uint32_t *)&cube_frames[index];
    uint32_t *dst = (uint32_t *)&frame_buffer;
    for (unsigned i = 0; i < sizeof(CubeFrame) / 4; ++i) dst[i] = src[i];
}

static void make_atlas(void) {
    PAL[0] = 0x0000;
    const uint16_t colors[6] = {0x001f, 0x03e0, 0x7c00, 0x03ff, 0x7c1f, 0x7fe0};
    for (unsigned f = 0; f < 6; ++f) {
        PAL[1 + f * 2] = colors[f];
        PAL[2 + f * 2] = 0x7fff;
    }
    /* 128x128 atlas (256 unique tiles), repeated by the 256x256 affine map. */
    for (unsigned my = 0; my < 32; ++my)
        for (unsigned mx = 0; mx < 32; mx += 2) {
            unsigned a = (my & 15) * 16 + (mx & 15);
            unsigned b = (my & 15) * 16 + ((mx + 1) & 15);
            MAP16[(my * 32 + mx) >> 1] = (uint16_t)(a | (b << 8));
        }
    for (unsigned y = 0; y < 128; ++y) {
        for (unsigned x = 0; x < 128; x += 2) {
            unsigned panel_x = x >> 5, panel_y = y >> 5;
            unsigned face = panel_y * 3 + panel_x;
            uint8_t c0 = 0, c1 = 0;
            if (panel_x < 3 && panel_y < 2) {
                unsigned g0 = (((x & 31) >> 2) ^ ((y & 31) >> 2)) & 1;
                unsigned g1 = ((((x + 1) & 31) >> 2) ^ ((y & 31) >> 2)) & 1;
                c0 = (uint8_t)(1 + face * 2 + g0);
                c1 = (uint8_t)(1 + face * 2 + g1);
            }
            unsigned tile = (y >> 3) * 16 + (x >> 3);
            unsigned in_tile = (y & 7) * 8 + (x & 7);
            VRAM16[(tile * 64 + in_tile) >> 1] = (uint16_t)(c0 | (c1 << 8));
        }
    }
}

int main(void) {
    REG_DISPCNT = 0;
    make_atlas();
    REG_BG2CNT = (8u << 8) | (1u << 13) | (1u << 14); /* CBB0, SBB8, wrap, 256 */
    REG_BG2PA = 0x100; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 0x100;
    REG_BG2X = 0; REG_BG2Y = 0;
    REG_WIN0V = 160;
    REG_WININ = 1u << 2;
    REG_WINOUT = 0;
    REG_WIN0H = 0;
    REG_DISPCNT = 2u | (1u << 10) | (1u << 13);

    unsigned frame = 0, hold = 0;
    copy_frame(0);
    for (;;) {
        while (REG_VCOUNT != 160) {}
        if (++hold == 3) {
            hold = 0;
            frame = (frame + 1) % CUBE_FRAME_COUNT;
            copy_frame(frame); /* 31,360 bytes, copied during VBlank. */
        }
        while (REG_VCOUNT != 0) {}
        raster_cube_frame(frame_buffer.line);
    }
}
