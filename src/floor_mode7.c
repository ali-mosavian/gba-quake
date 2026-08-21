/* Proof: the PPU draws one floor plane in perspective, by itself.
 *
 * A camera that cannot pitch or roll sees a horizontal plane as one scaled,
 * rotated line of a flat image per scanline. That is exactly what an affine
 * background does, so the whole floor costs the CPU nothing but a table: 160
 * register sets, written into BG2's affine state by an HBlank-triggered DMA
 * with destination reload. The earlier quad experiments in this repo polled
 * and rewrote the registers mid-scanline and flickered; the DMA path is
 * latched by hardware once per line and cannot.
 *
 * D-pad walks, L/R turn, A rises, B sinks. The plan is pre-lit offline by
 * generate_floor_plan.py from the same map data the software renderer uses.
 */
#include <stdint.h>
#include "gba_hardware.h"
#include "generated/floor_plan.h"

enum {
    SCREEN_W = 240, SCREEN_H = 160,
    HORIZON = SCREEN_H / 2,
    /* The software renderer's 56-pixel focal length is for a 120-pixel
     * logical screen; the PPU draws the floor at native 240, so the focal
     * doubles and vertical rows map through the same doubling. */
    FOCAL = 112,
    EYE_HEIGHT = 44,
};

static const int16_t sine_q14[256] = {
#define S(i) (int16_t)(0.5 + 16383.5 * __builtin_sin((i) * 6.283185307 / 256))
#define R(i) S(i), S(i + 1), S(i + 2), S(i + 3), S(i + 4), S(i + 5), S(i + 6), S(i + 7)
    R(0), R(8), R(16), R(24), R(32), R(40), R(48), R(56),
    R(64), R(72), R(80), R(88), R(96), R(104), R(112), R(120),
    R(128), R(136), R(144), R(152), R(160), R(168), R(176), R(184),
    R(192), R(200), R(208), R(216), R(224), R(232), R(240), R(248),
#undef R
#undef S
};

/* One HBlank's worth of BG2 state, in register order 0x04000020..0x2f. */
typedef struct {
    int16_t pa, pb, pc, pd;
    int32_t x, y;
} AffineLine;

/* Double-buffered: the DMA walks one table while the CPU fills the other, so
 * a slow frame can never feed the raster a half-built line. */
static AffineLine line_table[2][SCREEN_H];

/* 65536 / dy for the perspective divide, one entry per row below the
 * horizon. */
static uint16_t row_reciprocal[HORIZON + 1];

static void build_table(AffineLine *table, int32_t cam_x_q8, int32_t cam_y_q8,
                        int32_t height_q8, uint8_t yaw)
{
    int32_t sine = sine_q14[yaw], cosine = sine_q14[(uint8_t)(yaw + 64)];
    for (int y = 0; y < SCREEN_H; ++y) {
        /* Table entry y is loaded during HBlank of line y and so takes
         * effect on line y + 1. */
        int row = y + 1 - HORIZON;
        AffineLine *line = &table[y];
        if (row <= 0) {
            /* Above the horizon: point the (non-wrapping) background outside
             * its own map, which displays as transparent. */
            line->pa = line->pb = line->pc = line->pd = 0;
            line->x = line->y = (int32_t)0x07000000;
            continue;
        }
        /* Ground distance of this row: d = h * FOCAL / row, world Q8. */
        int32_t distance_q8 =
            (int32_t)(((int64_t)height_q8 * FOCAL * row_reciprocal[row]) >> 16);
        /* World units per screen pixel along the scanline is d / FOCAL; the
         * affine step is that in plan pixels as 8.8, i.e. d * 256 / (112*8),
         * folded into one multiply. 74899 = 2^24 * 256 / (112*8*256). */
        int32_t step_88 = (int32_t)(((int64_t)distance_q8 * 74899) >> 24);
        /* Screen +x is the camera's right, (-sin, cos); rows march along the
         * forward vector (cos, sin). */
        line->pa = (int16_t)((-sine * step_88) >> 14);
        line->pc = (int16_t)((cosine * step_88) >> 14);
        line->pb = 0;
        line->pd = 0;
        /* Line start: camera + forward * d - right * (SCREEN_W/2) * step.
         * All in world Q8, then rebased to the plan and narrowed to the
         * register's 20.8. One plan pixel is 8 world units, so Q8 world
         * to 8.8 plan is a shift by 3. */
        int32_t half_span_q8 = step_88 * (SCREEN_W / 2) * 8 >> 8;
        int32_t world_x_q8 = cam_x_q8 + ((cosine * distance_q8) >> 14) +
                             ((sine * half_span_q8) >> 14);
        int32_t world_y_q8 = cam_y_q8 + ((sine * distance_q8) >> 14) -
                             ((cosine * half_span_q8) >> 14);
        line->x = (world_x_q8 - (FLOOR_PLAN_ORIGIN_X << 8)) >> 3;
        line->y = (world_y_q8 - (FLOOR_PLAN_ORIGIN_Y << 8)) >> 3;
    }
}

int main(void)
{
    REG_WAITCNT = WAITCNT_FAST_ROM;
    REG_MEMCTRL = MEMCTRL_FAST_EWRAM;
    REG_DISPCNT = 0;
    for (unsigned i = 0; i < 256; ++i) BG_PALETTE[i] = floor_plan_palette[i];
    volatile uint16_t *tiles = BG_TILES;
    for (unsigned i = 0; i < sizeof(floor_plan_tiles) / 2; ++i)
        tiles[i] = (uint16_t)(floor_plan_tiles[2 * i] |
                              (floor_plan_tiles[2 * i + 1] << 8));
    /* Map base block 16 = 0x06008000, one byte per affine map entry. */
    volatile uint16_t *map = (volatile uint16_t *)0x06008000;
    for (unsigned i = 0; i < sizeof(floor_plan_map) / 2; ++i)
        map[i] = (uint16_t)(floor_plan_map[2 * i] |
                            (floor_plan_map[2 * i + 1] << 8));
    for (unsigned row = 1; row <= HORIZON; ++row)
        row_reciprocal[row] = (uint16_t)(65536 / row);

    REG_BG2CNT = BG_SIZE_256 | (16u << 8);   /* char 0, map block 16 */
    REG_DISPCNT = DISPLAY_MODE_2 | DISPLAY_BG2;

    int32_t cam_x = FLOOR_SPAWN_X << 8, cam_y = FLOOR_SPAWN_Y << 8;
    int32_t height = (EYE_HEIGHT) << 8;
    uint8_t yaw = 0;
    unsigned page = 0;
    for (;;) {
        uint16_t keys = (uint16_t)~REG_KEYINPUT;
        if (keys & KEY_LEFT) yaw = (uint8_t)(yaw + 2);
        if (keys & KEY_RIGHT) yaw = (uint8_t)(yaw - 2);
        int32_t sine = sine_q14[yaw], cosine = sine_q14[(uint8_t)(yaw + 64)];
        if (keys & KEY_UP) { cam_x += cosine >> 4; cam_y += sine >> 4; }
        if (keys & KEY_DOWN) { cam_x -= cosine >> 4; cam_y -= sine >> 4; }
        if (keys & KEY_A && height < (400 << 8)) height += 256;
        if (keys & KEY_B && height > (12 << 8)) height -= 256;

        build_table(line_table[page], cam_x, cam_y, height, yaw);

        while (REG_VCOUNT >= SCREEN_H) { }
        while (REG_VCOUNT < SCREEN_H) { }
        /* In VBlank: retarget the DMA at the fresh table and set line 0's
         * registers by hand -- the first HBlank load only affects line 1. */
        REG_DMA0CNT = 0;
        REG_DMA0SAD = (uint32_t)(uintptr_t)line_table[page];
        REG_DMA0DAD = 0x04000020;
        REG_DMA0CNT = DMA_ENABLE | DMA_REPEAT | DMA_32BIT | DMA_AT_HBLANK |
                      DMA_DST_RELOAD | (sizeof(AffineLine) / 4);
        AffineLine first;
        first = line_table[page][0];
        first.pa = 0; first.pc = 0;
        first.x = first.y = (int32_t)0x07000000;
        REG_BG2PA = (uint16_t)first.pa;
        REG_BG2PC = (uint16_t)first.pc;
        REG_BG2PB = 0;
        REG_BG2PD = 0;
        REG_BG2X = first.x;
        REG_BG2Y = first.y;
        page ^= 1;
    }
}
