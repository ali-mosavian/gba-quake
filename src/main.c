#include <stdint.h>

#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))
#define REG_DISPCNT REG16(0x04000000)
#define REG_DISPSTAT REG16(0x04000004)
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
#define REG_TM0D REG16(0x04000100)
#define REG_TM0CNT REG16(0x04000102)

#define VRAM ((volatile uint16_t *)0x06000000)
#define PAL ((volatile uint16_t *)0x05000000)
#define MAP ((volatile uint8_t *)0x06004000)

enum { MODE_AFFINE = 2, BG2_ENABLE = 1u << 10, WIN0_ENABLE = 1u << 13 };

typedef struct {
    uint16_t at_cycle;
    int16_t pa;
    int16_t pc;
    int32_t x;
    int32_t y;
} Command;

/* Explicitly EWRAM: this is the representation later renderer work would fill. */
__attribute__((section(".ewram"), aligned(4)))
volatile Command command_stream[8];

__attribute__((section(".ewram"), aligned(4)))
volatile uint16_t observed_cycles[8];

extern void raster_frame(unsigned test_id, const volatile Command *commands,
                         volatile uint16_t *observed);

static void make_checkerboard(void) {
    PAL[0] = 0x0000;
    PAL[1] = 0x7fff;
    PAL[2] = 0x001f;
    PAL[3] = 0x03e0;
    PAL[4] = 0x7c00;

    /* Four solid 8bpp tiles make register discontinuities unambiguous. */
    for (unsigned tile = 0; tile < 4; ++tile) {
        uint16_t packed = (uint16_t)((tile + 1) | ((tile + 1) << 8));
        for (unsigned i = 0; i < 32; ++i) VRAM[tile * 32 + i] = packed;
    }
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x)
            MAP[y * 32 + x] = (uint8_t)(((x >> 1) ^ (y >> 1)) & 3);
}

static void affine_identity(void) {
    REG_BG2PA = 0x0100; REG_BG2PB = 0;
    REG_BG2PC = 0;      REG_BG2PD = 0x0100;
    REG_BG2X = 0;       REG_BG2Y = 0;
}

static void make_commands(void) {
    for (unsigned i = 0; i < 8; ++i) {
        command_stream[i].at_cycle = (uint16_t)(i * 128);
        command_stream[i].pa = (i & 1) ? 0x0080 : 0x0180;
        command_stream[i].pc = (i & 1) ? 0x0040 : -0x0040;
        command_stream[i].x = (int32_t)(i * 32) << 8;
        command_stream[i].y = (int32_t)(i * 8) << 8;
        observed_cycles[i] = 0xffff;
    }
}

int main(void) {
    REG_DISPCNT = 0;
    make_checkerboard();
    make_commands();
    REG_BG2CNT = (0u << 2) | (8u << 8) | (1u << 13) | (1u << 14);
    affine_identity();

#if defined(TEST_WINDOW)
    REG_WIN0H = (40u << 8) | 200u;
    REG_WIN0V = (24u << 8) | 136u;
    REG_WININ = 1u << 2; /* BG2 inside WIN0. Window masks use bits 0..5. */
    REG_WINOUT = 0;
    REG_DISPCNT = MODE_AFFINE | BG2_ENABLE | WIN0_ENABLE;
#else
    REG_DISPCNT = MODE_AFFINE | BG2_ENABLE;
#endif

    for (;;) {
        while (REG_VCOUNT != 160) {}
        while (REG_VCOUNT == 160) {}
#if defined(TEST_BASELINE)
        affine_identity();
#elif defined(TEST_PA_PC)
        raster_frame(1, command_stream, observed_cycles);
#elif defined(TEST_XY)
        raster_frame(2, command_stream, observed_cycles);
#elif defined(TEST_COMBINED32)
        raster_frame(3, command_stream, observed_cycles);
#elif defined(TEST_STREAM32)
        raster_frame(4, command_stream, observed_cycles);
#elif defined(TEST_WINDOW)
        raster_frame(5, command_stream, observed_cycles);
#endif
    }
}
