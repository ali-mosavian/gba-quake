/* Does real hardware apply BG2X/BG2Y writes mid-scanline?
 *
 * The beam-raced frame came out corrupted on hardware in a way that ignored
 * both a timing recalibration and atlas padding, which no small-offset
 * failure can. The one theory fitting everything -- including the quad
 * experiment's success -- is that the affine reference point is latched per
 * line: PA/PC apply mid-draw, X/Y do not. The quad could not see this
 * because its pieces continue one plane's walk, so each piece's X/Y is
 * where the walk already was. Spans jump between faces; they need the jump.
 *
 * This ROM asks the question directly, one half per screen half:
 *   top:    command at mid-line changes ONLY X/Y, to a different texture.
 *   bottom: command at mid-line changes ONLY PA, zooming the same texture.
 * Read the photo: a clean texture change at mid-screen on top means X/Y
 * writes land mid-line and the beam architecture stands; the top half
 * continuing its left texture across the seam means they do not, and only
 * per-line-anchored techniques survive. The bottom half is the control --
 * the quad already proved PA mid-line on hardware, so it must show a zoom
 * seam or this ROM itself is broken.
 */
#include <stdint.h>
#include "gba_hardware.h"
#include "generated/beam_frame.h"

extern void raster_beam_frame(const uint8_t *counts, const uint32_t *commands,
                              uint32_t lines);

static uint32_t commands[80 * 2 * 4];
static uint8_t counts[80];

static void put(unsigned index, unsigned cycle, int pa, int pc,
                int32_t x, int32_t y)
{
    commands[index * 4 + 0] = (uint32_t)(cycle & 0xffff) |
                              ((uint32_t)(pa & 0xffff) << 16);
    commands[index * 4 + 1] = (uint32_t)(pc & 0xffff);
    commands[index * 4 + 2] = (uint32_t)x;
    commands[index * 4 + 3] = (uint32_t)y;
}

int main(void)
{
    REG_DISPCNT = 0;
    for (unsigned i = 0; i < 256; ++i) BG_PALETTE[i] = beam_palette[i];
    volatile uint16_t *tiles = BG_TILES;
    for (unsigned i = 0; i < sizeof(beam_tiles) / 2; ++i)
        tiles[i] = (uint16_t)(beam_tiles[2 * i] | (beam_tiles[2 * i + 1] << 8));
    volatile uint16_t *map = (volatile uint16_t *)0x06008000;
    for (unsigned i = 0; i < sizeof(beam_map) / 2; ++i)
        map[i] = (uint16_t)(beam_map[2 * i] | (beam_map[2 * i + 1] << 8));

    const int32_t *a = beam_placements[0];
    const int32_t *b = beam_placements[1];
    for (unsigned line = 0; line < 80; ++line) {
        counts[line] = 2;
        int32_t ay = ((a[1] + (line % (a[3] * a[5]))) << 8);
        int32_t by = ((b[1] + (line % (b[3] * b[5]))) << 8);
        if (line < 40) {
            /* X/Y jump at logical x=60, slope unchanged. */
            put(line * 2 + 0, 0, 0x0080, 0, a[0] << 8, ay);
            put(line * 2 + 1, 60 * 8, 0x0080, 0, b[0] << 8, by);
        } else {
            /* PA change only: the control half. */
            put(line * 2 + 0, 0, 0x0080, 0, a[0] << 8, ay);
            put(line * 2 + 1, 60 * 8, 0x0200, 0,
                (a[0] << 8) + 60 * 0x80, ay);
        }
    }

    REG_BG2CNT = (3u << 14) | (16u << 8);
    REG_BG2PB = 0;
    REG_BG2PD = 0;
    REG_DISPCNT = DISPLAY_MODE_2 | DISPLAY_BG2;

    for (;;) {
        while (REG_VCOUNT >= 160) { }
        while (REG_VCOUNT < 160) { }
        raster_beam_frame(counts, commands, 80);
    }
}
