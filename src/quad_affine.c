#include <stdint.h>

#include "gba_hardware.h"
#include "quad_stream.h"
#include "generated/quad_affine_frames.h"

extern void raster_cube_frame(const QuadScanline *scanlines);
extern void raster_staged_frame(const QuadScanline *scanlines);

#ifndef QUAD_STAGED
/* The older path reads a mutable frame from EWRAM during HDraw. It remains as
 * a comparison for the staged path, whose visible-period reads are all IWRAM. */
__attribute__((section(".ewram"), aligned(4)))
static QuadFrame ewram_frame;

static void copy_frame_to_ewram(unsigned frame_index)
{
    const uint32_t *source = (const uint32_t *)&quad_frames[frame_index];
    uint32_t *destination = (uint32_t *)&ewram_frame;

    for (unsigned i = 0; i < sizeof(ewram_frame) / sizeof(uint32_t); ++i)
        destination[i] = source[i];
}
#endif

static void load_checkerboard_texture(void)
{
    BG_PALETTE[0] = 0x0000;
    BG_PALETTE[1] = 0x001f;
    BG_PALETTE[2] = 0x7fff;

    /* An affine map stores one byte per tile; each 16-bit write below installs
     * two adjacent entries in the 32x32 map. */
    for (unsigned map_y = 0; map_y < 32; ++map_y) {
        for (unsigned map_x = 0; map_x < 32; map_x += 2) {
            unsigned left_tile = (map_y & 15) * 16 + (map_x & 15);
            unsigned right_tile = (map_y & 15) * 16 + ((map_x + 1) & 15);
            BG_MAP[(map_y * 32 + map_x) / 2] =
                (uint16_t)(left_tile | (right_tile << 8));
        }
    }

    /* 128x128 texture, arranged as 8bpp 8x8 tiles. */
    for (unsigned y = 0; y < 128; ++y) {
        for (unsigned x = 0; x < 128; x += 2) {
            uint8_t left = 1 + (((x >> 2) ^ (y >> 2)) & 1);
            uint8_t right = 1 + ((((x + 1) >> 2) ^ (y >> 2)) & 1);
            unsigned tile = (y >> 3) * 16 + (x >> 3);
            unsigned pixel_in_tile = (y & 7) * 8 + (x & 7);
            BG_TILES[(tile * 64 + pixel_in_tile) / 2] =
                (uint16_t)(left | (right << 8));
        }
    }
}

static void configure_affine_background(void)
{
    REG_DISPCNT = 0;
    load_checkerboard_texture();

    REG_BG2CNT = BG_MAP_BLOCK_8 | BG_WRAP | BG_SIZE_128;
    REG_BG2PB = 0;
    REG_BG2PD = 0x0100;

    REG_WIN0V = SCREEN_HEIGHT;
    REG_WININ = WINDOW_BG2;
    REG_WINOUT = 0;
    REG_DISPCNT = DISPLAY_MODE_2 | DISPLAY_BG2 | DISPLAY_WIN0;
}

static void wait_for_vblank(void)
{
    while (REG_VCOUNT != SCREEN_HEIGHT) {}
}

int main(void)
{
    enum { INITIAL_FRAME = 3, DISPLAY_FRAMES_PER_POSE = 3 };
    unsigned frame_index = INITIAL_FRAME;
    unsigned pose_hold_count = 0;

    configure_affine_background();

#ifndef QUAD_STAGED
    copy_frame_to_ewram(frame_index);
#endif

    for (;;) {
        wait_for_vblank();

        if (++pose_hold_count == DISPLAY_FRAMES_PER_POSE) {
            pose_hold_count = 0;
#ifndef QUAD_STATIC
            frame_index = (frame_index + 1) % QUAD_FRAME_COUNT;
#ifndef QUAD_STAGED
            copy_frame_to_ewram(frame_index);
#endif
#endif
        }

#ifdef QUAD_STAGED
        raster_staged_frame(quad_frames[frame_index].scanlines);
#else
        while (REG_VCOUNT != 0) {}
        raster_cube_frame(ewram_frame.scanlines);
#endif
    }
}
