#include <stdint.h>

#include "gba_hardware.h"
#include "dual_affine_stream.h"
#include "generated/runtime_cube_luts.h"

extern void raster_dual_frame(const DualAffineScanline *scanlines);

enum {
    SCREEN_HEIGHT = 160,
    SCREEN_WIDTH = 240,
    PIXEL_CENTER_Q8 = 128,
    FOCAL_LENGTH = 128,
    DEFAULT_CAMERA_DISTANCE_Q8 = 4 * 256,
    MIN_CAMERA_DISTANCE_Q8 = 3 * 256,
    MAX_CAMERA_DISTANCE_Q8 = 6 * 256,
    MIN_FACE_RUN_PIXELS = 2,
    LAST_SAFE_VBLANK_LINE = 225,
};

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
} Vector3;

typedef struct {
    int32_t x;              /* Screen coordinate, Q8. */
    int32_t y;
    int32_t inverse_depth;  /* 1/z, Q16. */
} ProjectedVertex;

typedef struct {
    int32_t origin_x;
    int32_t origin_y;
    int32_t origin_inverse_depth;
    int32_t origin_u_over_depth;
    int32_t origin_v_over_depth;
    int32_t inverse_depth_dx_q12;
    int32_t inverse_depth_dy_q12;
    int32_t u_over_depth_dx_q12;
    int32_t u_over_depth_dy_q12;
    int32_t v_over_depth_dx_q12;
    int32_t v_over_depth_dy_q12;
} PerspectivePlane;

typedef struct {
    int left;
    int right;
    unsigned face;
    unsigned layer;
} FaceSpan;

typedef struct {
    uint8_t angle_x;
    uint8_t angle_y;
    int camera_distance_q8;
    ProjectedVertex vertices[8];
    Vector3 rotated_normals[6];
    Vector3 rotated_centers[6];
    PerspectivePlane face_planes[6];
    uint8_t selected_faces[2];
} CubePose;

typedef struct {
    uint8_t angle_x;
    uint8_t angle_y;
    int camera_distance_q8;
    unsigned next_scanline;
    CubePose pose;
} FrameBuilder;

/* Vertex indices wind counter-clockwise when viewed from outside each face. */
static const uint8_t face_vertices[6][4] = {
    {0, 4, 7, 3}, /* -X */
    {1, 2, 6, 5}, /* +X */
    {0, 1, 5, 4}, /* -Y */
    {3, 7, 6, 2}, /* +Y */
    {0, 3, 2, 1}, /* -Z */
    {4, 5, 6, 7}, /* +Z */
};

static const Vector3 object_vertices[8] = {
    {-256, -256, -256}, { 256, -256, -256},
    { 256,  256, -256}, {-256,  256, -256},
    {-256, -256,  256}, { 256, -256,  256},
    { 256,  256,  256}, {-256,  256,  256},
};

static const Vector3 face_normals[6] = {
    {-256, 0, 0}, {256, 0, 0}, {0, -256, 0},
    {0, 256, 0}, {0, 0, -256}, {0, 0, 256},
};

static const Vector3 face_centers[6] = {
    {-256, 0, 0}, {256, 0, 0}, {0, -256, 0},
    {0, 256, 0}, {0, 0, -256}, {0, 0, 256},
};

__attribute__((section(".ewram"), aligned(4)))
static DualAffineFrame command_frames[2];

static int32_t multiply_q14(int32_t value, int32_t scale)
{
    return (value * scale) >> 14;
}

static void prepare_face_planes(CubePose *pose);
static int face_is_visible(const CubePose *pose, unsigned face);

static Vector3 rotate_vector(Vector3 vector, int sine_x, int cosine_x,
                             int sine_y, int cosine_y)
{
    /* Object-to-camera rotation: Ry * Rx. */
    int32_t after_x_y = multiply_q14(vector.y, cosine_x) -
                        multiply_q14(vector.z, sine_x);
    int32_t after_x_z = multiply_q14(vector.y, sine_x) +
                        multiply_q14(vector.z, cosine_x);
    Vector3 result = {
        multiply_q14(vector.x, cosine_y) +
            multiply_q14(after_x_z, sine_y),
        after_x_y,
        -multiply_q14(vector.x, sine_y) +
            multiply_q14(after_x_z, cosine_y),
    };
    return result;
}

static void prepare_pose(CubePose *pose, uint8_t angle_x, uint8_t angle_y,
                         int camera_distance_q8)
{
    int sine_x = sine_q14[angle_x];
    int cosine_x = sine_q14[(uint8_t)(angle_x + 64)];
    int sine_y = sine_q14[angle_y];
    int cosine_y = sine_q14[(uint8_t)(angle_y + 64)];

    pose->angle_x = angle_x;
    pose->angle_y = angle_y;
    pose->camera_distance_q8 = camera_distance_q8;

    for (unsigned i = 0; i < 8; ++i) {
        Vector3 rotated = rotate_vector(object_vertices[i], sine_x, cosine_x,
                                        sine_y, cosine_y);
        int32_t camera_z = rotated.z + camera_distance_q8;
        pose->vertices[i].x = 120 * 256 +
            (FOCAL_LENGTH * rotated.x * 256) / camera_z;
        pose->vertices[i].y = 80 * 256 +
            (FOCAL_LENGTH * rotated.y * 256) / camera_z;
        pose->vertices[i].inverse_depth = (1 << 24) / camera_z;
    }

    for (unsigned face = 0; face < 6; ++face) {
        pose->rotated_normals[face] = rotate_vector(
            face_normals[face], sine_x, cosine_x, sine_y, cosine_y);
        pose->rotated_centers[face] = rotate_vector(
            face_centers[face], sine_x, cosine_x, sine_y, cosine_y);
        pose->rotated_centers[face].z += camera_distance_q8;
    }
    prepare_face_planes(pose);

    int64_t best_area[2] = {-1, -1};
    pose->selected_faces[0] = 0;
    pose->selected_faces[1] = 0;
    for (unsigned face = 0; face < 6; ++face) {
        if (!face_is_visible(pose, face))
            continue;
        int64_t twice_area = 0;
        for (unsigned corner = 0; corner < 4; ++corner) {
            const ProjectedVertex *a =
                &pose->vertices[face_vertices[face][corner]];
            const ProjectedVertex *b =
                &pose->vertices[face_vertices[face][(corner + 1) & 3]];
            twice_area += (int64_t)a->x * b->y - (int64_t)a->y * b->x;
        }
        if (twice_area < 0)
            twice_area = -twice_area;
        if (twice_area > best_area[0]) {
            best_area[1] = best_area[0];
            pose->selected_faces[1] = pose->selected_faces[0];
            best_area[0] = twice_area;
            pose->selected_faces[0] = (uint8_t)face;
        } else if (twice_area > best_area[1]) {
            best_area[1] = twice_area;
            pose->selected_faces[1] = (uint8_t)face;
        }
    }
}

static int face_is_visible(const CubePose *pose, unsigned face)
{
    const Vector3 *normal = &pose->rotated_normals[face];
    const Vector3 *center = &pose->rotated_centers[face];
    return normal->x * center->x + normal->y * center->y +
           normal->z * center->z < 0;
}

static int32_t interpolate(int32_t start, int32_t end, int32_t fraction_q16)
{
    return start + (int32_t)(((int64_t)(end - start) * fraction_q16) >> 16);
}

static void face_texture_corner(unsigned face, unsigned corner,
                                int32_t *u_q8, int32_t *v_q8)
{
    static const uint8_t atlas_x[6] = {0, 32, 64, 0, 32, 64};
    static const uint8_t atlas_y[6] = {0, 0, 0, 32, 32, 32};
    static const uint8_t corner_u[4] = {1, 30, 30, 1};
    static const uint8_t corner_v[4] = {1, 1, 30, 30};
    *u_q8 = (atlas_x[face] + corner_u[corner]) * 256;
    *v_q8 = (atlas_y[face] + corner_v[corner]) * 256;
}

static int32_t plane_gradient(int32_t attribute_0, int32_t attribute_1,
                              int32_t attribute_2, int32_t coordinate_1,
                              int32_t coordinate_2, int64_t determinant)
{
    int64_t numerator =
        (int64_t)(attribute_1 - attribute_0) * coordinate_1 -
        (int64_t)(attribute_2 - attribute_0) * coordinate_2;
    return (int32_t)((numerator << 12) / determinant);
}

static void prepare_face_planes(CubePose *pose)
{
    for (unsigned face = 0; face < 6; ++face) {
        /* Corners 0, 1, and 3 form a non-degenerate triangle on the quad. */
        const ProjectedVertex *v0 =
            &pose->vertices[face_vertices[face][0]];
        const ProjectedVertex *v1 =
            &pose->vertices[face_vertices[face][1]];
        const ProjectedVertex *v2 =
            &pose->vertices[face_vertices[face][3]];
        int32_t x1 = v1->x - v0->x;
        int32_t y1 = v1->y - v0->y;
        int32_t x2 = v2->x - v0->x;
        int32_t y2 = v2->y - v0->y;
        int64_t determinant = (int64_t)x1 * y2 - (int64_t)x2 * y1;
        int32_t u0, texture_v0, u1, texture_v1, u2, texture_v2;
        face_texture_corner(face, 0, &u0, &texture_v0);
        face_texture_corner(face, 1, &u1, &texture_v1);
        face_texture_corner(face, 3, &u2, &texture_v2);
        int32_t uq0 = (u0 * v0->inverse_depth) >> 8;
        int32_t uq1 = (u1 * v1->inverse_depth) >> 8;
        int32_t uq2 = (u2 * v2->inverse_depth) >> 8;
        int32_t vq0 = (texture_v0 * v0->inverse_depth) >> 8;
        int32_t vq1 = (texture_v1 * v1->inverse_depth) >> 8;
        int32_t vq2 = (texture_v2 * v2->inverse_depth) >> 8;
        PerspectivePlane *plane = &pose->face_planes[face];

        plane->origin_x = v0->x;
        plane->origin_y = v0->y;
        plane->origin_inverse_depth = v0->inverse_depth;
        plane->origin_u_over_depth = uq0;
        plane->origin_v_over_depth = vq0;
        plane->inverse_depth_dx_q12 = plane_gradient(
            v0->inverse_depth, v1->inverse_depth, v2->inverse_depth,
            y2, y1, determinant);
        plane->inverse_depth_dy_q12 = plane_gradient(
            v0->inverse_depth, v2->inverse_depth, v1->inverse_depth,
            x1, x2, determinant);
        plane->u_over_depth_dx_q12 = plane_gradient(
            uq0, uq1, uq2, y2, y1, determinant);
        plane->u_over_depth_dy_q12 = plane_gradient(
            uq0, uq2, uq1, x1, x2, determinant);
        plane->v_over_depth_dx_q12 = plane_gradient(
            vq0, vq1, vq2, y2, y1, determinant);
        plane->v_over_depth_dy_q12 = plane_gradient(
            vq0, vq2, vq1, x1, x2, determinant);
    }
}

static int intersect_face(const CubePose *pose, unsigned face, int screen_y,
                          FaceSpan *span)
{
    int32_t intersections[2];
    unsigned count = 0;
    int32_t sample_y = screen_y * 256 + PIXEL_CENTER_Q8;

    if (!face_is_visible(pose, face))
        return 0;

    for (unsigned edge = 0; edge < 4 && count < 2; ++edge) {
        unsigned next = (edge + 1) & 3;
        const ProjectedVertex *a = &pose->vertices[face_vertices[face][edge]];
        const ProjectedVertex *b = &pose->vertices[face_vertices[face][next]];
        int32_t min_y = a->y < b->y ? a->y : b->y;
        int32_t max_y = a->y > b->y ? a->y : b->y;

        /* Half-open edge inclusion is the vertical half of the top-left rule. */
        if (sample_y < min_y || sample_y >= max_y || a->y == b->y)
            continue;

        int32_t fraction = (int32_t)(
            ((int64_t)(sample_y - a->y) << 16) / (b->y - a->y));
        intersections[count] = interpolate(a->x, b->x, fraction);
        ++count;
    }

    if (count != 2)
        return 0;
    if (intersections[0] > intersections[1]) {
        int32_t temporary = intersections[0];
        intersections[0] = intersections[1];
        intersections[1] = temporary;
    }

    /* A pixel belongs to the face when its center lies inside the edge pair. */
    int left = (intersections[0] + 127) >> 8;
    int right = (intersections[1] + 127) >> 8;
    if (left < 0)
        left = 0;
    if (right > SCREEN_WIDTH)
        right = SCREEN_WIDTH;
    if (right - left < MIN_FACE_RUN_PIXELS)
        return 0;

    span->left = left;
    span->right = right;
    span->face = face;
    return 1;
}

static void sample_texture(const PerspectivePlane *plane, int32_t sample_x,
                           int32_t sample_y, int32_t *u_q8, int32_t *v_q8)
{
    int32_t dx = sample_x - plane->origin_x;
    int32_t dy = sample_y - plane->origin_y;
    int32_t inverse_depth = plane->origin_inverse_depth + (int32_t)(
        ((int64_t)plane->inverse_depth_dx_q12 * dx +
         (int64_t)plane->inverse_depth_dy_q12 * dy) >> 12);
    int32_t u_over_depth = plane->origin_u_over_depth + (int32_t)(
        ((int64_t)plane->u_over_depth_dx_q12 * dx +
         (int64_t)plane->u_over_depth_dy_q12 * dy) >> 12);
    int32_t v_over_depth = plane->origin_v_over_depth + (int32_t)(
        ((int64_t)plane->v_over_depth_dx_q12 * dx +
         (int64_t)plane->v_over_depth_dy_q12 * dy) >> 12);

    if (inverse_depth < 1)
        inverse_depth = 1;
    if (inverse_depth > 32767)
        inverse_depth = 32767;
    *u_q8 = (u_over_depth * reciprocal_q24[inverse_depth]) >> 16;
    *v_q8 = (v_over_depth * reciprocal_q24[inverse_depth]) >> 16;
}

static void append_face_commands(const CubePose *pose, unsigned screen_y,
                                 DualAffineScanline *line,
                                 const FaceSpan *span, unsigned layer)
{
    int width = span->right - span->left;
    int piece_count = (width + 31) / 32;

    for (int piece = 0; piece < piece_count; ++piece) {
        if (line->command_count == DUAL_MAX_COMMANDS)
            return;
        int start = span->left + (width * piece) / piece_count;
        int end = span->left + (width * (piece + 1)) / piece_count;
        int32_t start_x = start * 256 + PIXEL_CENTER_Q8;
        int32_t end_x = (end - 1) * 256 + PIXEL_CENTER_Q8;
        int32_t sample_y = screen_y * 256 + PIXEL_CENTER_Q8;
        int32_t u0, v0, u1, v1;
        const PerspectivePlane *plane = &pose->face_planes[span->face];
        sample_texture(plane, start_x, sample_y, &u0, &v0);
        sample_texture(plane, end_x, sample_y, &u1, &v1);
        int intervals = end - start - 1;
        int pa = intervals > 0 ? (u1 - u0) / intervals : 0;
        int pc = intervals > 0 ? (v1 - v0) / intervals : 0;

        DualAffineCommand *command = &line->commands[line->command_count++];
        command->at_cycle = (uint16_t)(start * 4);
        command->flags = (uint16_t)layer;
        if (piece == 0)
            command->flags |= DUAL_COMMAND_PREINSTALL;
        command->pa = (int16_t)pa;
        command->pc = (int16_t)pc;
        command->reference_x = u0 - pa * start + (pa >= 0 ? 128 : 127);
        command->reference_y = v0 - pc * start + (pc >= 0 ? 128 : 127);
    }
}

static void sort_commands(DualAffineScanline *line)
{
    for (unsigned i = 1; i < line->command_count; ++i) {
        DualAffineCommand value = line->commands[i];
        unsigned j = i;
        while (j && line->commands[j - 1].at_cycle > value.at_cycle) {
            line->commands[j] = line->commands[j - 1];
            --j;
        }
        line->commands[j] = value;
    }
}

static void build_scanline(const CubePose *pose, unsigned screen_y,
                           DualAffineScanline *line)
{
    FaceSpan spans[3];
    unsigned span_count = 0;

    line->window_0_horizontal = 0;
    line->window_1_horizontal = 0;
    line->command_count = 0;
    line->padding = 0;
    for (unsigned layer = 0; layer < 2; ++layer) {
        unsigned face = pose->selected_faces[layer];
        if (intersect_face(pose, face, screen_y, &spans[span_count])) {
            spans[span_count].layer = layer;
            ++span_count;
        }
    }
    if (!span_count)
        return;

    /* Each selected face owns one affine layer for the entire pose. */
    for (unsigned i = 0; i < span_count; ++i) {
        unsigned layer = spans[i].layer;
        uint16_t window = (uint16_t)(
            (spans[i].left << 8) | spans[i].right);
        if (layer == 0)
            line->window_0_horizontal = window;
        else
            line->window_1_horizontal = window;
        append_face_commands(pose, screen_y, line, &spans[i], layer);
    }
    sort_commands(line);
}

static void begin_frame(FrameBuilder *builder, uint8_t angle_x, uint8_t angle_y,
                        int camera_distance_q8)
{
    builder->angle_x = angle_x;
    builder->angle_y = angle_y;
    builder->camera_distance_q8 = camera_distance_q8;
    builder->next_scanline = 0;
    prepare_pose(&builder->pose, angle_x, angle_y, camera_distance_q8);
}

static int build_frame_slice(FrameBuilder *builder, DualAffineFrame *destination,
                             unsigned line_count)
{
    unsigned end = builder->next_scanline + line_count;
    if (end > SCREEN_HEIGHT)
        end = SCREEN_HEIGHT;
    while (builder->next_scanline < end) {
        build_scanline(&builder->pose, builder->next_scanline,
                       &destination->scanlines[builder->next_scanline]);
        ++builder->next_scanline;
    }
    return builder->next_scanline == SCREEN_HEIGHT;
}

static int build_during_vblank(FrameBuilder *builder, DualAffineFrame *destination)
{
    /* Leave two VBlank scanlines of margin for publication and raster entry.
     * The amount of geometry completed adapts to the actual pose/workload. */
    while (builder->next_scanline < SCREEN_HEIGHT &&
           REG_VCOUNT >= SCREEN_HEIGHT &&
           REG_VCOUNT < LAST_SAFE_VBLANK_LINE) {
        build_scanline(&builder->pose, builder->next_scanline,
                       &destination->scanlines[builder->next_scanline]);
        ++builder->next_scanline;
    }
    return builder->next_scanline == SCREEN_HEIGHT;
}

static void load_cube_texture_atlas(void)
{
    static const uint16_t face_colors[6] = {
        0x001f, 0x03e0, 0x7c00, 0x03ff, 0x7c1f, 0x7fe0,
    };

    BG_PALETTE[0] = 0;
    for (unsigned face = 0; face < 6; ++face) {
        BG_PALETTE[1 + face * 2] = face_colors[face];
        BG_PALETTE[2 + face * 2] = 0x7fff;
    }

    for (unsigned map_y = 0; map_y < 32; ++map_y) {
        for (unsigned map_x = 0; map_x < 32; map_x += 2) {
            unsigned left = (map_y & 15) * 16 + (map_x & 15);
            unsigned right = (map_y & 15) * 16 + ((map_x + 1) & 15);
            BG_MAP[(map_y * 32 + map_x) / 2] =
                (uint16_t)(left | (right << 8));
        }
    }

    for (unsigned y = 0; y < 128; ++y) {
        for (unsigned x = 0; x < 128; x += 2) {
            unsigned panel_x = x >> 5;
            unsigned panel_y = y >> 5;
            unsigned face = panel_y * 3 + panel_x;
            uint8_t left = 0;
            uint8_t right = 0;
            if (panel_x < 3 && panel_y < 2) {
                left = (uint8_t)(1 + face * 2 +
                    ((((x & 31) >> 2) ^ ((y & 31) >> 2)) & 1));
                right = (uint8_t)(1 + face * 2 +
                    (((((x + 1) & 31) >> 2) ^ ((y & 31) >> 2)) & 1));
            }
            unsigned tile = (y >> 3) * 16 + (x >> 3);
            unsigned pixel = (y & 7) * 8 + (x & 7);
            BG_TILES[(tile * 64 + pixel) / 2] =
                (uint16_t)(left | (right << 8));
        }
    }
}

static void configure_display(void)
{
    REG_DISPCNT = 0;
    load_cube_texture_atlas();
    REG_BG2CNT = BG_MAP_BLOCK_8 | BG_WRAP | BG_SIZE_128;
    REG_BG3CNT = BG_MAP_BLOCK_8 | BG_WRAP | BG_SIZE_128;
    REG_BG2PB = 0;
    REG_BG2PD = 0x0100;
    REG_BG3PB = 0;
    REG_BG3PD = 0x0100;
    REG_WIN0V = SCREEN_HEIGHT;
    REG_WIN1V = SCREEN_HEIGHT;
    REG_WININ = WINDOW_BG2 | (WINDOW_BG3 << 8);
    REG_WINOUT = 0;
    REG_DISPCNT = DISPLAY_MODE_2 | DISPLAY_BG2 | DISPLAY_BG3 |
                  DISPLAY_WIN0 | DISPLAY_WIN1;
}

static uint16_t read_keys(void)
{
    return (uint16_t)(~REG_KEYINPUT) & 0x03ff;
}

static void update_controls(uint16_t keys, uint16_t pressed,
                            uint8_t *angle_x, uint8_t *angle_y,
                            int *camera_distance_q8, int *auto_rotate)
{
    if (keys & KEY_LEFT)  *angle_y -= 2;
    if (keys & KEY_RIGHT) *angle_y += 2;
    if (keys & KEY_UP)    *angle_x -= 2;
    if (keys & KEY_DOWN)  *angle_x += 2;
    if (keys & KEY_A && *camera_distance_q8 > MIN_CAMERA_DISTANCE_Q8)
        *camera_distance_q8 -= 8;
    if (keys & KEY_B && *camera_distance_q8 < MAX_CAMERA_DISTANCE_Q8)
        *camera_distance_q8 += 8;
    if (pressed & KEY_START)
        *auto_rotate = !*auto_rotate;
    if (pressed & KEY_SELECT) {
        *angle_x = 24;
        *angle_y = 24;
        *camera_distance_q8 = DEFAULT_CAMERA_DISTANCE_Q8;
    }
    if (*auto_rotate && !(keys & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)))
        *angle_y += 1;
}

int main(void)
{
    uint8_t angle_x = 24;
    uint8_t angle_y = 24;
    int camera_distance_q8 = DEFAULT_CAMERA_DISTANCE_Q8;
    int auto_rotate = 1;
    unsigned display_buffer = 0;
    unsigned build_buffer = 1;
    uint16_t previous_keys = 0;
    FrameBuilder builder;

    /* Produce a complete first frame while the display is disabled. */
    begin_frame(&builder, angle_x, angle_y, camera_distance_q8);
    build_frame_slice(&builder, &command_frames[display_buffer], SCREEN_HEIGHT);
    begin_frame(&builder, angle_x, angle_y, camera_distance_q8);
    configure_display();

    for (;;) {
        while (REG_VCOUNT != SCREEN_HEIGHT) {}
        uint16_t keys = read_keys();
        uint16_t pressed = keys & (uint16_t)~previous_keys;
        previous_keys = keys;
        update_controls(keys, pressed, &angle_x, &angle_y,
                        &camera_distance_q8, &auto_rotate);

        if (build_during_vblank(&builder, &command_frames[build_buffer])) {
            unsigned temporary = display_buffer;
            display_buffer = build_buffer;
            build_buffer = temporary;
            begin_frame(&builder, angle_x, angle_y, camera_distance_q8);
        }

        raster_dual_frame(command_frames[display_buffer].scanlines);
    }
}
