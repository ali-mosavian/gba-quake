/* One frozen frame of the Quake renderer, drawn entirely by the PPU.
 *
 * The display list is the software renderer's visible spans at the spawn,
 * computed offline by generate_beam_frame.py: each span is one affine-BG
 * command, and raster_beam_frame issues them against the beam with the
 * cycle-exact mechanism hardware validated on the quad. The CPU never
 * touches a pixel; its whole job is 1,175 register writes per frame.
 *
 * Textures live in the affine map as replicated blocks (tiling cannot be
 * expressed by an affine walk), 241 tiles of the 256 an 8-bit map allows.
 * Unlit: the PPU samples raw texels, and lighting is the architecture's
 * open problem. Static: this artifact prices feeding, spacing and the
 * atlas, not the front end.
 */
#include <stdint.h>
#include "gba_hardware.h"
#include "generated/beam_frame.h"

extern void raster_beam_frame(const uint8_t *counts, const uint32_t *commands,
                              uint32_t lines);

/* The whole display list, IWRAM-resident so every command read is one
 * cycle. 18.8KB of the 32; nothing else in this ROM wants the space. */
__attribute__((aligned(4)))
static uint32_t iwram_commands[BEAM_COMMAND_COUNT * 4];
static uint8_t iwram_counts[BEAM_LINE_COUNT];

int main(void)
{
#ifndef GBA_SLOW_ROM
    REG_WAITCNT = WAITCNT_FAST_ROM;
#endif
    REG_DISPCNT = 0;
    for (unsigned i = 0; i < 256; ++i) BG_PALETTE[i] = beam_palette[i];
    volatile uint16_t *tiles = BG_TILES;
    for (unsigned i = 0; i < sizeof(beam_tiles) / 2; ++i)
        tiles[i] = (uint16_t)(beam_tiles[2 * i] | (beam_tiles[2 * i + 1] << 8));
    volatile uint16_t *map = (volatile uint16_t *)0x06008000;
    for (unsigned i = 0; i < sizeof(beam_map) / 2; ++i)
        map[i] = (uint16_t)(beam_map[2 * i] | (beam_map[2 * i + 1] << 8));
    for (unsigned i = 0; i < BEAM_COMMAND_COUNT * 4; ++i)
        iwram_commands[i] = beam_commands[i];
    for (unsigned i = 0; i < BEAM_LINE_COUNT; ++i)
        iwram_counts[i] = beam_counts[i];

    /* 1024x1024 8bpp affine BG: char base 0, map base block 16. The matrix
     * stays identity; every PA/PC/X/Y comes from the display list, and
     * PB = PD = 0 stops the reference point drifting between commands. */
    REG_BG2CNT = (3u << 14) | (16u << 8);
    REG_BG2PB = 0;
    REG_BG2PD = 0;
    REG_DISPCNT = DISPLAY_MODE_2 | DISPLAY_BG2;

    for (;;) {
        while (REG_VCOUNT >= 160) { }
        while (REG_VCOUNT < 160) { }
        raster_beam_frame(iwram_counts, iwram_commands, BEAM_LINE_COUNT);
    }
}
