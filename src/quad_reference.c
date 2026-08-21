#include <stdint.h>

#include "gba_hardware.h"
#include "generated/quad_reference_frames.h"

enum {
    DISPLAY_HEIGHT = 160,
    MODE4_PAGE_WORDS = 19200,
    MODE4_PAGE_1 = 0x06000000,
    MODE4_PAGE_2 = 0x0600a000,
    DISPLAY_MODE_4 = 4,
    DISPLAY_FRAME_SELECT = 1u << 4,
    DMA_ENABLE = 1u << 31,
};

#define REG_DMA3_SOURCE      GBA_REG32(0x040000d4)
#define REG_DMA3_DESTINATION GBA_REG32(0x040000d8)
#define REG_DMA3_CONTROL     GBA_REG32(0x040000dc)

static void copy_mode4_frame(const uint16_t *source,
                             volatile uint16_t *destination)
{
    REG_DMA3_SOURCE = (uint32_t)source;
    REG_DMA3_DESTINATION = (uint32_t)destination;
    REG_DMA3_CONTROL = DMA_ENABLE | MODE4_PAGE_WORDS;
}

static void wait_for_vblank_start(void)
{
    while (REG_VCOUNT != DISPLAY_HEIGHT) {}
}

static void wait_for_vblank_to_continue(void)
{
    while (REG_VCOUNT == DISPLAY_HEIGHT) {}
}

int main(void)
{
    enum { DISPLAY_FRAMES_PER_POSE = 3 };
    unsigned frame_index = 0;
    unsigned visible_page = 0;
    unsigned pose_hold_count = 0;

    REG_DISPCNT = 0;
    BG_PALETTE[0] = 0;
    BG_PALETTE[1] = 0x001f;
    BG_PALETTE[2] = 0x7fff;
    copy_mode4_frame(quad_reference_frames[0],
                     (volatile uint16_t *)MODE4_PAGE_1);
    REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2;

    for (;;) {
        wait_for_vblank_start();
        if (++pose_hold_count == DISPLAY_FRAMES_PER_POSE) {
            pose_hold_count = 0;
            frame_index = (frame_index + 1) % QUAD_REFERENCE_FRAME_COUNT;
            visible_page ^= 1;

            volatile uint16_t *next_page = (volatile uint16_t *)(
                visible_page ? MODE4_PAGE_2 : MODE4_PAGE_1);
            copy_mode4_frame(quad_reference_frames[frame_index], next_page);
            REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2 |
                          (visible_page ? DISPLAY_FRAME_SELECT : 0);
        }
        wait_for_vblank_to_continue();
    }
}
