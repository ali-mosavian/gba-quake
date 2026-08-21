#include <stdint.h>

#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))
#define REG_DISPCNT REG16(0x04000000)
#define REG_VCOUNT REG16(0x04000006)
#define REG_DMA3SAD REG32(0x040000d4)
#define REG_DMA3DAD REG32(0x040000d8)
#define REG_DMA3CNT REG32(0x040000dc)
#define PAL ((volatile uint16_t *)0x05000000)

#include "generated/cube_reference_frames.h"

static void dma_frame(const uint16_t *src, volatile uint16_t *dst) {
    REG_DMA3SAD = (uint32_t)src;
    REG_DMA3DAD = (uint32_t)dst;
    REG_DMA3CNT = (1u << 31) | 19200u;
}

int main(void) {
    REG_DISPCNT = 0;
    const uint16_t colors[6] = {0x001f, 0x03e0, 0x7c00, 0x03ff, 0x7c1f, 0x7fe0};
    PAL[0] = 0;
    for (unsigned f = 0; f < 6; ++f) {
        PAL[1 + f * 2] = colors[f];
        PAL[2 + f * 2] = 0x7fff;
    }
    unsigned frame = 0, page = 0, hold = 0;
    dma_frame(cube_reference_frames[0], (volatile uint16_t *)0x06000000);
    REG_DISPCNT = 4u | (1u << 10); /* Mode 4, BG2. */
    for (;;) {
        while (REG_VCOUNT != 160) {}
        if (++hold == 3) {
            hold = 0;
            frame = (frame + 1) % CUBE_REFERENCE_FRAME_COUNT;
            page ^= 1;
            volatile uint16_t *back = (volatile uint16_t *)(page ? 0x0600a000 : 0x06000000);
            dma_frame(cube_reference_frames[frame], back);
            REG_DISPCNT = (uint16_t)(4u | (1u << 10) | (page << 4));
        }
        while (REG_VCOUNT == 160) {}
    }
}
