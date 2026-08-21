#ifndef GBA_HARDWARE_H
#define GBA_HARDWARE_H

#include <stdint.h>

#define GBA_REG16(address) (*(volatile uint16_t *)(address))
#define GBA_REG32(address) (*(volatile uint32_t *)(address))

#define REG_DISPCNT GBA_REG16(0x04000000)
/* Game Pak wait-state control. The BIOS leaves this at 4/2 with the prefetch
 * buffer disabled, which is the slowest setting the cartridge bus supports.
 * 0x4317 selects 3 wait states on a first ROM access, 1 on a sequential one,
 * and enables the prefetcher. Every commercial cartridge sets this. */
#define REG_WAITCNT GBA_REG16(0x04000204)
#define WAITCNT_FAST_ROM 0x4317
/* Undocumented internal memory control. The reset value 0x0d000020 gives
 * EWRAM two wait states; 0x0e000020 gives it one, so every EWRAM access gets
 * about a third faster. Widely used by GBA homebrew and known to work on
 * GBA and GBA SP silicon, but it is NOT part of the documented register set,
 * and a DS running in GBA compatibility mode hangs on it. Treat as
 * hardware-unverified alongside the rest of this project's timing claims. */
#define REG_MEMCTRL GBA_REG32(0x04000800)
#define MEMCTRL_FAST_EWRAM 0x0e000020
#define REG_VCOUNT  GBA_REG16(0x04000006)
#define REG_BG2CNT  GBA_REG16(0x0400000c)
#define REG_BG2PB   GBA_REG16(0x04000022)
#define REG_BG2PD   GBA_REG16(0x04000026)
#define REG_BG3CNT  GBA_REG16(0x0400000e)
#define REG_BG3PB   GBA_REG16(0x04000032)
#define REG_BG3PD   GBA_REG16(0x04000036)
#define REG_WIN0V   GBA_REG16(0x04000044)
#define REG_WIN1V   GBA_REG16(0x04000046)
#define REG_WININ   GBA_REG16(0x04000048)
#define REG_WINOUT  GBA_REG16(0x0400004a)
#define REG_KEYINPUT GBA_REG16(0x04000130)
#define REG_TM0D     GBA_REG16(0x04000100)
#define REG_TM0CNT   GBA_REG16(0x04000102)
#define REG_TM1D     GBA_REG16(0x04000104)
#define REG_TM1CNT   GBA_REG16(0x04000106)

#define BG_PALETTE ((volatile uint16_t *)0x05000000)
#define BG_TILES   ((volatile uint16_t *)0x06000000)
#define BG_MAP     ((volatile uint16_t *)0x06004000)

enum {
    DISPLAY_MODE_2 = 2,
    DISPLAY_BG2 = 1u << 10,
    DISPLAY_BG3 = 1u << 11,
    DISPLAY_WIN0 = 1u << 13,
    DISPLAY_WIN1 = 1u << 14,
    WINDOW_BG2 = 1u << 2,
    WINDOW_BG3 = 1u << 3,
    BG_SIZE_128 = 0u << 14,
    BG_WRAP = 1u << 13,
    BG_MAP_BLOCK_8 = 8u << 8,

    KEY_A = 1u << 0,
    KEY_B = 1u << 1,
    KEY_SELECT = 1u << 2,
    KEY_START = 1u << 3,
    KEY_RIGHT = 1u << 4,
    KEY_LEFT = 1u << 5,
    KEY_UP = 1u << 6,
    KEY_DOWN = 1u << 7,
};

#endif
