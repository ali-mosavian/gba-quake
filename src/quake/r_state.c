/* Shared renderer types, generated map data, memory arenas, and profiling state. */
#include <stdint.h>
#include "../gba_hardware.h"
#include "../generated/runtime_cube_luts.h"
#include "r_fixed.h"

extern int projection_offset_lut(int32_t, int32_t, const uint16_t *);
extern int32_t signed_ratio_q16_lut(int32_t, int32_t, const uint32_t *);
extern int32_t multiply_q16(int32_t, int32_t);
extern void clear_logical_framebuffer(uint8_t *);
extern void expand_logical_framebuffer(const uint8_t *, volatile uint16_t *);
extern int32_t plane_distance_q8_arm(const void *, int32_t, int32_t, int32_t);
extern void transform_vertices_arm(const void *);
extern void draw_texture_row_arm(const void *);

/* Argument block for draw_texture_row_arm. The field order is the routine's
 * ABI: src/asm/d_span_arm.S indexes it by byte offset. */
typedef struct {
    uint8_t *row_pixels;
    uint32_t *coverage_row;
    const uint8_t *texture;
    uint32_t texture_width;
    uint32_t u_mask;
    uint32_t v_mask;
    int32_t left, right;
    int32_t inverse_depth, u_over_depth, v_over_depth;
    int32_t inverse_depth_dx, u_over_depth_dx, v_over_depth_dx;
    const uint32_t *reciprocal;
} TextureRowArgs;

enum {
    SCREEN_WIDTH = 120, SCREEN_HEIGHT = 80,
    MODE4_PAGE_0 = 0x06000000, MODE4_PAGE_1 = 0x0600a000,
    DISPLAY_MODE_4 = 4, DISPLAY_PAGE_SELECT = 1u << 4,
    FOCAL_LENGTH = 56, NEAR_PLANE_Q8 = Q8_FROM_INT(8),
    EYE_HEIGHT_Q8 = Q8_FROM_INT(22), INVALID_LEAF = 0xffff,
    /* Room for a face ring plus one vertex per clip plane. */
    CLIP_RING_MAX = 40,
    /* Pixels between perspective corrections. Both must divide 32 so a
     * segment never straddles two coverage words. Measured against an exact
     * reference on a wall seen at an angle: 32 px costs 2.72 texels of mean
     * error, 16 px costs 0.86, 8 px costs 0.72. The long interval is used
     * where the depth barely changes across a run. */
    PERSPECTIVE_SPAN_LONG = 32,
    PERSPECTIVE_SPAN_SHORT = 16,
};

typedef struct { int16_t x, y, z; } MapVertex;
typedef struct { uint16_t first, second; } MapEdge;
typedef struct { int16_t nx, ny, nz, distance; } MapPlane;
typedef struct {
    uint16_t plane; int16_t children[2];
    uint16_t first_face, face_count;
} MapNode;
typedef struct {
    uint16_t plane; uint8_t side; int32_t first_edge; uint8_t edge_count; uint16_t texinfo;
    int16_t center_x, center_y, center_z; uint16_t radius;
    /* Quake-style per-face texture origin, a whole multiple of the texture
     * size so masking still wraps identically. Absolute world texture
     * coordinates run to thousands of texels and cost u/z its precision. */
    int32_t u_base_q8, v_base_q8;
    /* Start of this face's vertex ring in bsp_face_vertices[], resolved at
     * build time so the runtime never walks surfedge -> edge -> vertex. */
    int32_t first_vertex;
} MapFace;
/* Clip-hull node. qbsp pre-expands hull 1 by the 32x32x56 player box, so the
 * player traces through it as a single point. Negative children are contents,
 * not node indices. */
typedef struct { int32_t plane; int16_t children[2]; } MapClipNode;
typedef struct { int32_t axis[2][4]; uint16_t texture; } MapTexInfo;
typedef struct { uint32_t offset; uint16_t width, height; } MapTexture;
typedef struct { int16_t contents; int32_t visibility; uint16_t first_mark, mark_count; } MapLeaf;
typedef struct { int32_t horizontal, vertical, depth; } CameraPoint;
typedef struct { int32_t x, y; } ScreenPoint;
typedef struct {
    int16_t nx, ny, nz, distance;
    int16_t center_x, center_y, center_z;
    uint16_t radius;
    int32_t first_edge;
    uint16_t source_face;
    uint8_t edge_count, side;
    int32_t first_vertex;
} RuntimeFace;
typedef struct {
    const uint16_t *indices; uint32_t count; const MapVertex *vertices;
    CameraPoint *output; int32_t camera_x, camera_y, camera_z;
    int32_t sine, cosine;
    ScreenPoint *screen_output; uint8_t *outcodes;
    const uint16_t *projection_reciprocal;
} TransformArgs;

#include "../generated/bsp_wireframe_map.h"

#define EWRAM __attribute__((section(".ewram"), aligned(4)))
/* Plain .bss lands in IWRAM under the devkitARM GBA script. IWRAM is a 32-bit
 * bus at one cycle per word; EWRAM is 16-bit and costs about six. Anything
 * touched per pixel or swept every frame belongs here, budget permitting. */
#define IWRAM_DATA __attribute__((aligned(4)))
#define HOT __attribute__((section(".iwram"), noinline))
#if defined(BSP_TEXTURED) && !defined(BSP_TEXTURED_SOLID) && \
    !defined(BSP_TEXTURED_NO_COVERAGE) && !defined(BSP_TEXTURED_C_REFERENCE)
__asm__(".include \"src/quake/d_polyset_arm.inc\"\n");
#endif
EWRAM static uint16_t candidate_faces[BSP_FACE_COUNT];
EWRAM static RuntimeFace runtime_faces[BSP_FACE_COUNT];
EWRAM static MapVertex runtime_vertices[BSP_VERTEX_COUNT];
EWRAM static MapEdge runtime_edges[BSP_EDGE_COUNT];
EWRAM static uint16_t frame_edges[BSP_EDGE_COUNT];
EWRAM static uint16_t frame_vertices[BSP_VERTEX_COUNT];
#ifdef BSP_TEXTURED
EWRAM static uint16_t frame_faces[BSP_FACE_COUNT];
EWRAM static uint8_t node_near_side[BSP_NODE_COUNT];
/* One bit per pixel, four words per 120-pixel row. Perspective segments are
 * cut on 32-pixel boundaries so each one touches exactly one word: it is
 * loaded once, tested and set in a register, stored once, and a word reading
 * all ones skips the segment outright. */
IWRAM_DATA static uint32_t texture_coverage[SCREEN_HEIGHT][4];
#ifndef BSP_TEXTURED_SOLID
/* Downsampling shrank the working set enough that this no longer overflows:
 * at mip 1 the spawn view caches 8KB where mip 0 filled all 32KB and still
 * fell back to ROM for the rest.
 *
 * It stays in EWRAM. IWRAM would make texel reads one cycle instead of three,
 * but IWRAM also holds the framebuffer, the coverage bitmap, the hot code and
 * the stack, and the face loop alone takes a 2,352-byte frame; a 10KB cache
 * there left too little stack and the renderer ran away to 78M cycles. */
enum { TEXTURE_CACHE_BYTES = 32 * 1024 };
EWRAM static uint8_t texture_cache[TEXTURE_CACHE_BYTES];
EWRAM static uint16_t texture_cache_offsets[
    sizeof(bsp_textures) / sizeof(bsp_textures[0])];
static uint16_t texture_cache_used;
#endif
#endif
EWRAM static uint16_t face_stamp[BSP_FACE_COUNT];
EWRAM static uint16_t edge_stamp[BSP_EDGE_COUNT];
EWRAM static uint16_t vertex_stamp[BSP_VERTEX_COUNT];
EWRAM static uint8_t vertex_outcode[BSP_VERTEX_COUNT];
EWRAM static CameraPoint camera_cache[BSP_VERTEX_COUNT];
EWRAM static ScreenPoint screen_cache[BSP_VERTEX_COUNT];
IWRAM_DATA static uint8_t logical_framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT];

static uint16_t candidate_face_count, frame_edge_count, frame_vertex_count;
static uint16_t drawn_edge_count, accepted_face_count;
static uint16_t trivial_accept_count, trivial_reject_count;
static uint16_t degenerate_face_count;
/* Render-pass work counters, so span setup and texel cost can be told apart. */
static uint32_t drawn_face_count, drawn_row_count, drawn_span_count;
/* How often the step-up branch of walk_move actually won. */
static uint32_t steps_climbed;
static uint32_t pixel_iteration_count, texel_sample_count;
static uint32_t span_clear_count, span_hidden_count, span_mixed_count;
static uint32_t texture_rom_fallbacks, near_clipped_faces;
static uint16_t near_clip_count, screen_clip_count;
static uint16_t cached_camera_leaf = INVALID_LEAF;
static uint16_t frame_stamp = 1, candidate_stamp = 1;

typedef struct {
    uint32_t clear_cycles, pvs_rebuild_cycles, face_cull_cycles;
    uint32_t transform_cycles, projection_cycles, clipping_cycles;
    uint32_t line_draw_cycles, expand_cycles, total_cycles;
    uint16_t candidate_faces, accepted_faces, unique_edges;
    uint16_t unique_vertices, drawn_edges, camera_leaf;
    uint16_t trivial_accepted, trivial_rejected;
    uint16_t near_clipped, screen_clipped;
    uint16_t degenerate_faces;
    uint32_t drawn_faces, drawn_rows, drawn_spans;
    uint32_t pixel_iterations, texel_samples;
    uint32_t spans_clear, spans_hidden, spans_mixed;
    uint32_t rom_fallbacks, cache_bytes, near_clipped_faces;
    int32_t player_x, player_y, player_z, player_velocity_z;
    uint32_t player_on_ground, player_substeps;
    int32_t player_contents;
    uint32_t player_solid_frames;
    uint32_t steps_climbed;
    uint32_t total_substeps;
    int32_t player_yaw_q8;
} BspProfile;
EWRAM volatile BspProfile bsp_profile;

static void profile_timer_start(void)
{
    REG_TM0CNT = 0; REG_TM1CNT = 0; REG_TM0D = 0; REG_TM1D = 0;
    REG_TM1CNT = 0x0084; REG_TM0CNT = 0x0080;
}

static uint32_t profile_timer_read(void)
{
    uint16_t high_before, low, high_after;
    do { high_before = REG_TM1D; low = REG_TM0D; high_after = REG_TM1D; }
    while (high_before != high_after);
    return ((uint32_t)high_after << 16) | low;
}
