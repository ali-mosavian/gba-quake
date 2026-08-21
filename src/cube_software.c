#include <stdint.h>

#include "gba_hardware.h"

enum {
    LOW_RES_WIDTH = 120,
    LOW_RES_HEIGHT = 80,
    DISPLAY_HEIGHT = 160,
    MODE4_PAGE_WORDS = 19200,
    MODE4_PAGE_1 = 0x06000000,
    MODE4_PAGE_2 = 0x0600a000,
    DISPLAY_MODE_4 = 4,
    DISPLAY_FRAME_SELECT = 1u << 4,
    PLANE_COEFFICIENTS = 3,
    INVERSE_DEPTH_PLANE = 0,
    U_OVER_DEPTH_PLANE = 3,
    V_OVER_DEPTH_PLANE = 6,
};

/* Each generated plane is A*x + B*y + C. The renderer interpolates inverse
 * depth, U/depth, and V/depth, then performs the final divide through a LUT. */
typedef struct {
    uint16_t face;
    int16_t vertex_xy[6];
    int32_t planes[3 * PLANE_COEFFICIENTS];
} SoftwareTriangle;

typedef struct {
    uint16_t triangle_count;
    SoftwareTriangle triangles[12];
} SoftwareFrame;

#include "generated/software_cube_data.h"

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }

static int edge_function(int ax, int ay, int bx, int by, int x, int y)
{
    return (x - ax) * (by - ay) - (y - ay) * (bx - ax);
}

static void clear_framebuffer(volatile uint16_t *framebuffer)
{
    for (unsigned i = 0; i < MODE4_PAGE_WORDS; ++i)
        framebuffer[i] = 0;
}

static int evaluate_plane(const int32_t plane[3], int x, int y)
{
    return plane[0] * x + plane[1] * y + plane[2];
}

static void draw_triangle(const SoftwareTriangle *triangle,
                          volatile uint16_t *framebuffer)
{
    int ax = triangle->vertex_xy[0];
    int ay = triangle->vertex_xy[1];
    int bx = triangle->vertex_xy[2];
    int by = triangle->vertex_xy[3];
    int cx = triangle->vertex_xy[4];
    int cy = triangle->vertex_xy[5];

    /* Vertices use four fractional bits. Samples are taken at pixel centers. */
    int min_x = maximum(0, minimum(ax, minimum(bx, cx)) >> 4);
    int max_x = minimum(LOW_RES_WIDTH - 1,
                        (maximum(ax, maximum(bx, cx)) + 15) >> 4);
    int min_y = maximum(0, minimum(ay, minimum(by, cy)) >> 4);
    int max_y = minimum(LOW_RES_HEIGHT - 1,
                        (maximum(ay, maximum(by, cy)) + 15) >> 4);

    for (int y = min_y; y <= max_y; ++y) {
        int sample_x = (min_x << 4) + 8;
        int sample_y = (y << 4) + 8;
        int edge_ab = edge_function(ax, ay, bx, by, sample_x, sample_y);
        int edge_bc = edge_function(bx, by, cx, cy, sample_x, sample_y);
        int edge_ca = edge_function(cx, cy, ax, ay, sample_x, sample_y);
        const int32_t *inverse_depth_plane =
            &triangle->planes[INVERSE_DEPTH_PLANE];
        const int32_t *u_over_depth_plane =
            &triangle->planes[U_OVER_DEPTH_PLANE];
        const int32_t *v_over_depth_plane =
            &triangle->planes[V_OVER_DEPTH_PLANE];
        int inverse_depth = evaluate_plane(inverse_depth_plane, min_x, y);
        int u_over_depth = evaluate_plane(u_over_depth_plane, min_x, y);
        int v_over_depth = evaluate_plane(v_over_depth_plane, min_x, y);

        for (int x = min_x; x <= max_x; ++x) {
            if (edge_ab <= 0 && edge_bc <= 0 && edge_ca <= 0) {
                int reciprocal_index = inverse_depth >> 4;
                if (reciprocal_index > 0 && reciprocal_index < 2048) {
                    int reciprocal = reciprocal_q16[reciprocal_index];
                    int texture_u = ((u_over_depth >> 4) * reciprocal) >> 16;
                    int texture_v = ((v_over_depth >> 4) * reciprocal) >> 16;
                    uint8_t color = (uint8_t)(1 + triangle->face * 2 +
                        (((texture_u >> 2) ^ (texture_v >> 2)) & 1));
                    uint16_t two_pixels = color | (color << 8);

                    /* One low-resolution sample becomes a 2x2 Mode 4 block. */
                    framebuffer[(unsigned)y * 240 + (unsigned)x] = two_pixels;
                    framebuffer[(unsigned)y * 240 + (unsigned)x + 120] = two_pixels;
                }
            }

            edge_ab += (by - ay) * 16;
            edge_bc += (cy - by) * 16;
            edge_ca += (ay - cy) * 16;
            inverse_depth += inverse_depth_plane[0];
            u_over_depth += u_over_depth_plane[0];
            v_over_depth += v_over_depth_plane[0];
        }
    }
}

static void load_cube_palette(void)
{
    static const uint16_t face_colors[6] = {
        0x001f, 0x03e0, 0x7c00, 0x03ff, 0x7c1f, 0x7fe0,
    };

    BG_PALETTE[0] = 0;
    for (unsigned face = 0; face < 6; ++face) {
        BG_PALETTE[1 + face * 2] = face_colors[face];
        BG_PALETTE[2 + face * 2] = 0x7fff;
    }
}

static void wait_for_vblank_start(void)
{
    while (REG_VCOUNT >= DISPLAY_HEIGHT) {}
    while (REG_VCOUNT < DISPLAY_HEIGHT) {}
}

int main(void)
{
    unsigned frame_index = 0;
    unsigned page = 0;

    REG_DISPCNT = 0;
    load_cube_palette();
    REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2;

    for (;;) {
        page ^= 1;
        volatile uint16_t *framebuffer = (volatile uint16_t *)(
            page ? MODE4_PAGE_2 : MODE4_PAGE_1);
        const SoftwareFrame *frame = &sw_frames[frame_index];

        clear_framebuffer(framebuffer);
        for (unsigned i = 0; i < frame->triangle_count; ++i)
            draw_triangle(&frame->triangles[i], framebuffer);

        frame_index = (frame_index + 1) % SW_FRAME_COUNT;
        wait_for_vblank_start();
        REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2 |
                      (page ? DISPLAY_FRAME_SELECT : 0);
    }
}
