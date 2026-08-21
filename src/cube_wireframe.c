#include <stdint.h>

#include "gba_hardware.h"
#include "generated/runtime_cube_luts.h"

enum {
    SCREEN_WIDTH = 240,
    SCREEN_HEIGHT = 160,
    PAGE_HALFWORDS = SCREEN_WIDTH * SCREEN_HEIGHT / 2,
    MODE4_PAGE_0 = 0x06000000,
    MODE4_PAGE_1 = 0x0600a000,
    DISPLAY_MODE_4 = 4,
    DISPLAY_PAGE_SELECT = 1u << 4,
    FOCAL_LENGTH = 128,
    DEFAULT_DISTANCE_Q8 = 4 * 256,
    MIN_DISTANCE_Q8 = 3 * 256,
    MAX_DISTANCE_Q8 = 6 * 256,
};

typedef struct {
    int16_t x;
    int16_t y;
} ScreenPoint;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
} Vector3;

static const Vector3 cube_vertices[8] = {
    {-256, -256, -256}, { 256, -256, -256},
    { 256,  256, -256}, {-256,  256, -256},
    {-256, -256,  256}, { 256, -256,  256},
    { 256,  256,  256}, {-256,  256,  256},
};

static const uint8_t cube_edges[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static int32_t multiply_q14(int32_t value, int32_t scale)
{
    return (value * scale) >> 14;
}

static Vector3 rotate_vertex(Vector3 vertex, uint8_t angle_x, uint8_t angle_y)
{
    int sine_x = sine_q14[angle_x];
    int cosine_x = sine_q14[(uint8_t)(angle_x + 64)];
    int sine_y = sine_q14[angle_y];
    int cosine_y = sine_q14[(uint8_t)(angle_y + 64)];
    int32_t y = multiply_q14(vertex.y, cosine_x) -
                multiply_q14(vertex.z, sine_x);
    int32_t z = multiply_q14(vertex.y, sine_x) +
                multiply_q14(vertex.z, cosine_x);
    Vector3 result = {
        multiply_q14(vertex.x, cosine_y) + multiply_q14(z, sine_y),
        y,
        -multiply_q14(vertex.x, sine_y) + multiply_q14(z, cosine_y),
    };
    return result;
}

static void project_cube(ScreenPoint projected[8], uint8_t angle_x,
                         uint8_t angle_y, int camera_distance_q8)
{
    for (unsigned i = 0; i < 8; ++i) {
        Vector3 vertex = rotate_vertex(cube_vertices[i], angle_x, angle_y);
        int32_t depth = vertex.z + camera_distance_q8;
        projected[i].x = (int16_t)(SCREEN_WIDTH / 2 +
            FOCAL_LENGTH * vertex.x / depth);
        projected[i].y = (int16_t)(SCREEN_HEIGHT / 2 +
            FOCAL_LENGTH * vertex.y / depth);
    }
}

static void clear_page(volatile uint16_t *page)
{
    for (unsigned i = 0; i < PAGE_HALFWORDS; ++i)
        page[i] = 0;
}

static void put_pixel(volatile uint16_t *page, int x, int y, uint8_t color)
{
    if ((unsigned)x >= SCREEN_WIDTH || (unsigned)y >= SCREEN_HEIGHT)
        return;
    unsigned index = (unsigned)y * (SCREEN_WIDTH / 2) + (unsigned)x / 2;
    uint16_t pixels = page[index];
    if (x & 1)
        pixels = (uint16_t)((pixels & 0x00ff) | (color << 8));
    else
        pixels = (uint16_t)((pixels & 0xff00) | color);
    page[index] = pixels;
}

static int absolute(int value) { return value < 0 ? -value : value; }

static void draw_line(volatile uint16_t *page, ScreenPoint start,
                      ScreenPoint end, uint8_t color)
{
    int x = start.x;
    int y = start.y;
    int dx = absolute(end.x - start.x);
    int dy = -absolute(end.y - start.y);
    int step_x = start.x < end.x ? 1 : -1;
    int step_y = start.y < end.y ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        put_pixel(page, x, y, color);
        if (x == end.x && y == end.y)
            break;
        int twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            x += step_x;
        }
        if (twice_error <= dx) {
            error += dx;
            y += step_y;
        }
    }
}

static uint16_t read_keys(void)
{
    return (uint16_t)(~REG_KEYINPUT) & 0x03ff;
}

static void wait_for_vblank(void)
{
    while (REG_VCOUNT >= SCREEN_HEIGHT) {}
    while (REG_VCOUNT < SCREEN_HEIGHT) {}
}

int main(void)
{
    uint8_t angle_x = 24;
    uint8_t angle_y = 24;
    int camera_distance_q8 = DEFAULT_DISTANCE_Q8;
    int auto_rotate = 1;
    uint16_t previous_keys = 0;
    unsigned visible_page = 0;

    REG_DISPCNT = 0;
    BG_PALETTE[0] = 0x0000;
    BG_PALETTE[1] = 0x7fff;
    BG_PALETTE[2] = 0x03ff;
    REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2;

    for (;;) {
        uint16_t keys = read_keys();
        uint16_t pressed = keys & (uint16_t)~previous_keys;
        previous_keys = keys;
        if (keys & KEY_LEFT)  angle_y -= 2;
        if (keys & KEY_RIGHT) angle_y += 2;
        if (keys & KEY_UP)    angle_x -= 2;
        if (keys & KEY_DOWN)  angle_x += 2;
        if (keys & KEY_A && camera_distance_q8 > MIN_DISTANCE_Q8)
            camera_distance_q8 -= 8;
        if (keys & KEY_B && camera_distance_q8 < MAX_DISTANCE_Q8)
            camera_distance_q8 += 8;
        if (pressed & KEY_START)
            auto_rotate = !auto_rotate;
        if (pressed & KEY_SELECT) {
            angle_x = 24;
            angle_y = 24;
            camera_distance_q8 = DEFAULT_DISTANCE_Q8;
        }
        if (auto_rotate && !(keys & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)))
            ++angle_y;

        unsigned draw_page = visible_page ^ 1;
        volatile uint16_t *page = (volatile uint16_t *)(
            draw_page ? MODE4_PAGE_1 : MODE4_PAGE_0);
        ScreenPoint projected[8];
        project_cube(projected, angle_x, angle_y, camera_distance_q8);
        clear_page(page);
        for (unsigned edge = 0; edge < 12; ++edge) {
            draw_line(page, projected[cube_edges[edge][0]],
                      projected[cube_edges[edge][1]],
                      edge < 4 ? 2 : 1);
        }

        wait_for_vblank();
        visible_page = draw_page;
        REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2 |
                      (visible_page ? DISPLAY_PAGE_SELECT : 0);
    }
}
