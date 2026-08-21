/* Texture-plane math, scan conversion, and the bit-exact C reference drawer. */
/* Work counters are opt-in: tallying them costs about 157K cycles a frame,
 * which is a quarter of the entire 30 FPS budget. COUNT is defined in
 * r_state.c so the movement code can use it too. */
/* x/y are the snapped pixel the coverage and edge walkers use; xq8/yq8 are
 * where the vertex actually projected. The gradient fit must use the sub-pixel
 * position, otherwise the fitted plane shears by up to half a pixel of lever
 * arm and the texture visibly slides whenever a vertex crosses a pixel edge. */
typedef struct {
    int32_t x, y;
    int32_t xq8, yq8;
    int32_t inverse_depth, u_over_depth, v_over_depth;
} TextureVertex;

typedef struct {
    int32_t horizontal, vertical, depth;
    int32_t u_q8, v_q8;
} ClipTextureVertex;

/* Screen-space planes for 1/z, u/z and v/z. Origins are the Q8 position and
 * the value at vertex a; the gradients are Q16 per whole pixel. */
typedef struct {
    int32_t origin_xq8, origin_yq8;
    int32_t inverse_depth, inverse_depth_dx, inverse_depth_dy;
    int32_t u_over_depth, u_over_depth_dx, u_over_depth_dy;
    int32_t v_over_depth, v_over_depth_dx, v_over_depth_dy;
} TexturePlane;

typedef struct {
    int32_t low_y, high_y;
    int32_t x_q8, step_q8;
} TextureEdgeWalker;

/* Clamp a pixel delta into 15 bits so a cross product of two of them stays
 * inside 32 bits with room for the subtraction. */
static int32_t clamp_rank(int32_t value)
{
    if (value > 16383) return 16383;
    if (value < -16383) return -16383;
    return value;
}

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }

/* Bit length of a non-zero value, without __builtin_clz.
 *
 * ARMv4T has no CLZ instruction, so the builtin becomes a call to libgcc's
 * __clzsi2 -- a Thumb routine living in ROM, reached from ARM code in IWRAM
 * through an interworking branch. At one call per polygon edge that was most
 * of what edge-walker construction cost. */
static inline __attribute__((always_inline)) unsigned bit_length(uint32_t value)
{
    unsigned bits = 1;
    if (value >> 16) { bits += 16; value >>= 16; }
    if (value >> 8)  { bits += 8;  value >>= 8; }
    if (value >> 4)  { bits += 4;  value >>= 4; }
    if (value >> 2)  { bits += 2;  value >>= 2; }
    if (value >> 1)  { bits += 1; }
    return bits;
}

static int32_t divide_s64_s32(int64_t numerator, int32_t denominator)
{
    if (!denominator) return 0;
    int negative = (numerator < 0) != (denominator < 0);
    uint64_t dividend = numerator < 0 ? (uint64_t)-numerator : (uint64_t)numerator;
    uint32_t divisor = denominator < 0 ? (uint32_t)-denominator : (uint32_t)denominator;
    unsigned bits = bit_length(divisor);
    unsigned shift = bits > 15 ? bits - 15 : 0;
    uint32_t normalized_divisor = divisor >> shift;
    uint64_t normalized_dividend = dividend >> shift;
    uint32_t quotient = (uint32_t)((normalized_dividend *
        clipping_reciprocal_q24[normalized_divisor]) >> 24);
    return negative ? -(int32_t)quotient : (int32_t)quotient;
}

/* value / intervals for the short runs between perspective corrections. */
static int divide_short_span(int value, unsigned intervals)
{
    static const uint16_t reciprocal_q16_small[32] = {
        0, 65535, 32768, 21845, 16384, 13107, 10923, 9362,
        8192, 7282, 6554, 5958, 5461, 5041, 4681, 4369,
        4096, 3855, 3641, 3449, 3277, 3121, 2979, 2849,
        2731, 2621, 2521, 2427, 2341, 2260, 2185, 2114
    };
    if (!intervals) return 0;
    if (intervals == 1) return value;
    return Q16_TO_INT((int64_t)value * reciprocal_q16_small[intervals]);
}

/* Reciprocal of one polygon's fit determinant, prepared once and reused for
 * all six gradients.
 *
 * Normalising the determinant per gradient meant repeating the same shift loop
 * six times over the same value, which is most of what the per-face front end
 * used to cost. */
typedef struct {
    uint32_t reciprocal;    /* 2^24 / (|determinant| >> shift) */
    unsigned shift;
    int negative;
    int ok;
} PlaneSolver;

static PlaneSolver plane_solver(int64_t determinant)
{
    PlaneSolver solver = {0, 0, 0, 0};
    if (!determinant) return solver;
    uint64_t magnitude = determinant < 0 ? (uint64_t)-determinant
                                         : (uint64_t)determinant;
    solver.negative = determinant < 0;
    /* Normalise into the 15-bit table. ARMv4T has no CLZ and stepping one bit
     * at a time costs ~25 iterations for the determinants off-screen faces
     * produce, so measure the bit length first and shift exactly once. The
     * shift must land the value inside [2^14, 2^15): overshooting throws away
     * significant bits and silently scales every gradient. */
    unsigned bits = (magnitude >> 32) ? 32 + bit_length((uint32_t)(magnitude >> 32))
                                      : bit_length((uint32_t)magnitude);
    solver.shift = bits > 15 ? bits - 15 : 0;
    magnitude >>= solver.shift;
    solver.reciprocal = clipping_reciprocal_q24[magnitude];
    solver.ok = 1;
    return solver;
}

/* (numerator << 24) / determinant, in Q16 per whole pixel.
 *
 * Table entries never exceed 2^24, so a numerator below 2^39 cannot make the
 * product leave 64 bits. Halving the numerator while dropping one from the
 * shift preserves the value at the cost of one bit, which a per-pixel gradient
 * can spare. */
static int32_t plane_solve(const PlaneSolver *solver, int64_t numerator,
                          unsigned fraction_bits, int *ok)
{
    int negative = (numerator < 0) != solver->negative;
    uint64_t value = numerator < 0 ? (uint64_t)-numerator : (uint64_t)numerator;
    unsigned shift = solver->shift;
    while (value >= (1ULL << 39)) {
        if (!shift) { *ok = 0; return 0; }
        value >>= 1;
        --shift;
    }
    uint64_t quotient = ((value * solver->reciprocal) >> shift) >> (16 - fraction_bits);
    if (quotient > 0x7fffffffu) { *ok = 0; return 0; }
    return negative ? -(int32_t)quotient : (int32_t)quotient;
}

/* Screen-space gradients of one interpolated attribute, fitted through three
 * sub-pixel vertices that share the prepared determinant. */
static void texture_gradients(int32_t a0, int32_t a1, int32_t a2,
                              int64_t bx, int64_t by, int64_t cx, int64_t cy,
                              const PlaneSolver *solver, unsigned fraction_bits,
                              int32_t *dx, int32_t *dy, int *ok)
{
    int64_t d1 = a1 - a0, d2 = a2 - a0;
    *dx = plane_solve(solver, d1 * cy - d2 * by, fraction_bits, ok);
    *dy = plane_solve(solver, bx * d2 - cx * d1, fraction_bits, ok);
}

/* 1/z is small and its gradient is a fraction of a unit per pixel, so it needs
 * the fractional bits. u/z and v/z are large and steeply sloped, so they need
 * headroom rather than precision: a face's texture extent is not bounded by the
 * per-face origin (that only removes whole multiples of the texture size), and
 * a large floor reaches u/z values that overflow a 32-bit Q8 accumulator.
 * Measured against an exact reference, Q4 costs 0.0007 texels over Q16 while
 * leaving sixteen times the range. */
enum { INVERSE_DEPTH_BITS = 16, OVER_DEPTH_BITS = 4 };

/* Value of a plane at the centre of pixel (x, y).
 *
 * Stays 64-bit: the fit is anchored at vertex a, which for a face crossing the
 * near plane can project thousands of pixels off screen, so the extrapolated
 * value at x = 0 does not fit a 32-bit Q8 accumulator even though every value
 * actually on the face does. Rows are stepped at this width and narrowed once
 * the left edge brings them back on-face. */
static int64_t plane_value(int32_t at_origin, int32_t dx, int32_t dy,
                           int32_t origin_xq8, int32_t origin_yq8,
                           int x, int y, unsigned fraction_bits)
{
    int32_t across = Q8_FROM_INT(x) + 128 - origin_xq8;
    int32_t down = Q8_FROM_INT(y) + 128 - origin_yq8;
    return ((int64_t)at_origin << fraction_bits) +
           (((int64_t)dx * across + (int64_t)dy * down) >> 8);
}

/* Recover a Q8 texel coordinate from the u/z and 1/z planes.
 *
 * The index is clamped to [16, 32767]: 16 corresponds to the 4095-unit far
 * clamp the projection uses, and keeping the low end away from 1 bounds the
 * reciprocal so the product cannot leave 64 bits. The table must be the
 * unsaturated uint32 one - reciprocal_q24 is uint16 and pins every entry
 * below index 256, i.e. everything further away than 256 world units. */
static __attribute__((unused)) int32_t texel_from_planes(
    int32_t over_depth, int32_t inverse_depth_q16)
{
    int32_t index = inverse_depth_q16 >> INVERSE_DEPTH_BITS;
    if (index < 16) index = 16;
    else if (index > 32767) index = 32767;
    return (int32_t)(((int64_t)over_depth *
                      clipping_reciprocal_q24[index]) >> (24 - OVER_DEPTH_BITS));
}

#ifndef BSP_TEXTURED_SOLID
static __attribute__((unused)) const uint8_t *texture_pixels_for(
    uint16_t texture_index)
{
    const MapTexture *texture = &bsp_textures[texture_index];
    uint16_t cached_offset = texture_cache_offsets[texture_index];
    if (cached_offset != 0xffff)
        return texture_cache + cached_offset;

    unsigned size = texture->width * texture->height;
    if (size > (unsigned)TEXTURE_CACHE_BYTES - texture_cache_used) {
        COUNT(texture_rom_fallbacks, 1);
        return bsp_texture_pixels + texture->offset;   /* slow Game Pak path */
    }

    cached_offset = texture_cache_used;
    const uint32_t *source = (const uint32_t *)(bsp_texture_pixels + texture->offset);
    uint32_t *destination = (uint32_t *)(texture_cache + cached_offset);
    for (unsigned word = 0; word < size / 4; ++word)
        destination[word] = source[word];
    texture_cache_offsets[texture_index] = cached_offset;
    texture_cache_used += size;
    return texture_cache + cached_offset;
}
#endif

/* Edge walkers at sub-pixel precision.
 *
 * Stepping from the snapped integer vertex made the silhouette jump a whole
 * pixel at a time, and left coverage disagreeing with the interpolants, which
 * are sampled at pixel centres. Here a row belongs to an edge when the row's
 * centre lies within it -- ceil(y - 1/2), the top-left rule -- and the
 * starting x is pre-stepped from the true edge position to that first centre.
 *
 * Frustum clipping is what makes this safe: every coordinate is now bounded to
 * the viewport, so the Q8 slope and its pre-step stay small and exact. */
static unsigned build_edge_walkers(const TextureVertex *vertices,
                                   unsigned vertex_count,
                                   TextureEdgeWalker *walkers)
{
    unsigned count = 0;
    for (unsigned edge = 0; edge < vertex_count; ++edge) {
        const TextureVertex *first = &vertices[edge];
        const TextureVertex *second = &vertices[edge + 1 == vertex_count ? 0 : edge + 1];
        const TextureVertex *low = first->yq8 < second->yq8 ? first : second;
        const TextureVertex *high = low == first ? second : first;
        int32_t height = high->yq8 - low->yq8;
        if (!height) continue;
        int first_row = (low->yq8 + 128) >> 8;
        int last_row = (high->yq8 + 128) >> 8;      /* exclusive */
        if (first_row >= last_row) continue;
        int32_t step = divide_s64_s32(((int64_t)(high->xq8 - low->xq8)) << 8, height);
        int32_t to_centre = (Q8_FROM_INT(first_row) + 128) - low->yq8;
        int32_t start = low->xq8 + (int32_t)(((int64_t)step * to_centre) >> 8);
        walkers[count++] = (TextureEdgeWalker){first_row, last_row, start, step};
    }
    return count;
}

static int polygon_scanline(TextureEdgeWalker *walkers,
                            unsigned walker_count, int y, int *left, int *right)
{
    int32_t minimum_x = 0, maximum_x = 0;
    int count = 0;
    for (unsigned edge = 0; edge < walker_count; ++edge) {
        TextureEdgeWalker *walker = &walkers[edge];
        if (y < walker->low_y || y >= walker->high_y) continue;
        int32_t x = walker->x_q8;
        walker->x_q8 += walker->step_q8;
        if (!count || x < minimum_x) minimum_x = x;
        if (!count || x > maximum_x) maximum_x = x;
        ++count;
    }
    if (count < 2) return 0;
    /* Same rule across the row: a pixel is covered when its centre falls
     * inside the span, so the right edge is exclusive. */
    *left = (minimum_x + 128) >> 8;
    *right = ((maximum_x + 128) >> 8) - 1;
    if (*left < 0) *left = 0;
    if (*right >= SCREEN_WIDTH) *right = SCREEN_WIDTH - 1;
    return *left <= *right;
}

/* Executing this from Game Pak ROM costs roughly 4x: ARM instructions are 32
 * bits and the cartridge bus is 16, and the span loops branch too much for the
 * prefetcher to help. When this routine is the active drawer it must live in
 * IWRAM; when it is only kept as a diff reference against the assembly it stays
 * in ROM so it does not displace the real kernel. */
#ifdef BSP_TEXTURED_C_REFERENCE
#define REFERENCE_POLYGON_SECTION ".iwram"
#else
#define REFERENCE_POLYGON_SECTION ".text.reference_polygon"
#endif
static __attribute__((used, noinline, section(REFERENCE_POLYGON_SECTION)))
void draw_textured_polygon_reference(
                                      const TextureVertex *vertices,
                                      unsigned vertex_count, uint16_t texture_index,
                                      const MapFaceLight *face_light)
{
    /* Fit through the widest-spread triple rather than the first non-degenerate
     * one. A long thin Quake face can present three nearly collinear leading
     * vertices, and fitting to those amplifies every rounding error in the
     * gradients. */
    const TextureVertex *a = &vertices[0], *b = 0, *c = 0;
    int32_t best_spread = 0;
    for (unsigned i = 1; i + 1 < vertex_count; ++i) {
        const TextureVertex *candidate_b = &vertices[i];
        const TextureVertex *candidate_c = &vertices[i + 1];
        /* Ranking only, so whole pixels clamped to 15 bits are plenty and the
         * cross product stays in 32. Doing this at Q8 in 64-bit purely to
         * compare magnitudes cost 591 cycles a face. */
        int32_t bx = clamp_rank(candidate_b->x - a->x);
        int32_t by = clamp_rank(candidate_b->y - a->y);
        int32_t cx = clamp_rank(candidate_c->x - a->x);
        int32_t cy = clamp_rank(candidate_c->y - a->y);
        int32_t spread = bx * cy - cx * by;
        if (spread < 0) spread = -spread;
        if (spread > best_spread) {
            best_spread = spread;
            b = candidate_b;
            c = candidate_c;
        }
    }
    if (!b) return;
#if defined(BSP_TEXTURED_SOLID) || defined(BSP_TEXTURED_NO_FETCH) || \
    defined(BSP_TEXTURED_NO_LIGHT)
    (void)face_light;
#endif
    /* The winner's determinant, once, at full Q8 precision for the fit. */
    int64_t determinant = (int64_t)(b->xq8 - a->xq8) * (c->yq8 - a->yq8) -
                          (int64_t)(c->xq8 - a->xq8) * (b->yq8 - a->yq8);
#ifdef BSP_TEXTURED_NO_GRADIENTS
    degenerate_face_count += (uint16_t)determinant;   /* defeat DCE */
    return;
#endif
    PlaneSolver solver = plane_solver(determinant);
    if (!solver.ok) return;
    int64_t bx = b->xq8 - a->xq8, cx = c->xq8 - a->xq8;
    int64_t by = b->yq8 - a->yq8, cy = c->yq8 - a->yq8;
#if !defined(BSP_TEXTURED_SOLID) && !defined(BSP_TEXTURED_NO_FETCH)
    /* Width and height are ROM halfwords; reading them per texel cost 27K
     * cycles a frame when this was last measured. */
    const MapTexture *texture = &bsp_textures[texture_index];
    const uint8_t *texture_pixels = texture_pixels_for(texture_index);
    const unsigned texture_width = texture->width;
    const unsigned u_mask = texture_width - 1;
    const unsigned v_mask = texture->height - 1;
#ifndef BSP_TEXTURED_NO_LIGHT
    /* Hoisted for the same reason the texture width is: these are ROM reads
     * through a pointer, and the segment loop below would repeat them. */
    const int32_t luxel_base = face_light->base;
    const int32_t luxel_width = face_light->width;
#endif
    /* Row offset without a multiply.
     *
     * Borrowed from the mgl 8-bit affine span filler, which keeps the v
     * integer already shifted by log2(width) and wraps it with a mask that is
     * shifted to match, so the x86 addressing mode adds the row and column for
     * free. ARM cannot add two index registers in one load, but it gets the
     * shift free inside the AND via the barrel shifter, so the multiply still
     * goes away.
     *
     * Masking a pre-shifted value with (height-1) << k is exactly equivalent
     * to masking then shifting: the mask's low k zero bits discard precisely
     * the fractional bits the shift brought up. The result is bit-identical. */
    unsigned width_shift = 0;
    while ((1u << width_shift) < texture_width) ++width_shift;
    const unsigned row_mask = v_mask << width_shift;
    const unsigned u_wrap = (texture_width << 8) - 1;
    const unsigned v_wrap = (texture->height << 8) - 1;
#else
    (void)texture_index;
#endif
    TexturePlane plane = {a->xq8, a->yq8, a->inverse_depth, 0, 0,
                          a->u_over_depth, 0, 0, a->v_over_depth, 0, 0};
    int ok = 1;
    texture_gradients(a->inverse_depth, b->inverse_depth, c->inverse_depth,
                      bx, by, cx, cy, &solver, INVERSE_DEPTH_BITS,
                      &plane.inverse_depth_dx, &plane.inverse_depth_dy, &ok);
    texture_gradients(a->u_over_depth, b->u_over_depth, c->u_over_depth,
                      bx, by, cx, cy, &solver, OVER_DEPTH_BITS,
                      &plane.u_over_depth_dx, &plane.u_over_depth_dy, &ok);
    texture_gradients(a->v_over_depth, b->v_over_depth, c->v_over_depth,
                      bx, by, cx, cy, &solver, OVER_DEPTH_BITS,
                      &plane.v_over_depth_dx, &plane.v_over_depth_dy, &ok);
    if (!ok) { ++degenerate_face_count; return; }
    COUNT(drawn_face_count, 1);

#ifdef BSP_TEXTURED_NO_WALKERS
    degenerate_face_count += (uint16_t)(plane.inverse_depth_dx + plane.u_over_depth_dy);
    return;
#endif
    TextureEdgeWalker walkers[CLIP_RING_MAX];
    unsigned walker_count = build_edge_walkers(vertices, vertex_count, walkers);
#ifdef BSP_TEXTURED_NO_ROWS
    degenerate_face_count += (uint16_t)(walker_count + walkers[0].x_q8);
    return;
#endif
    /* Perspective interval, chosen per face.
     *
     * The affine error between corrections scales with how much the depth
     * changes across the run, so a surface facing the camera, or a distant
     * one, is served perfectly well by the long interval; it is the large,
     * obliquely-viewed wall or floor that needs the short one. Comparing the
     * 1/z gradient against 1/z gives that directly, once per face. */
    int32_t depth_drift = plane.inverse_depth_dx < 0 ? -plane.inverse_depth_dx
                                                     : plane.inverse_depth_dx;
    int span_mask_bits =
        ((int64_t)depth_drift * (PERSPECTIVE_SPAN_LONG * PERSPECTIVE_SPAN_LONG) >
         ((int64_t)plane.inverse_depth << INVERSE_DEPTH_BITS))
            ? PERSPECTIVE_SPAN_SHORT - 1 : PERSPECTIVE_SPAN_LONG - 1;

    int min_y = vertices[0].y, max_y = vertices[0].y;
    for (unsigned i = 1; i < vertex_count; ++i) {
        min_y = minimum(min_y, vertices[i].y);
        max_y = maximum(max_y, vertices[i].y);
    }
    min_y = maximum(0, min_y);
    max_y = minimum(SCREEN_HEIGHT - 1, max_y);
    for (unsigned edge = 0; edge < walker_count; ++edge) {
        if (walkers[edge].low_y < min_y && walkers[edge].high_y > min_y)
            walkers[edge].x_q8 += walkers[edge].step_q8 *
                                  (min_y - walkers[edge].low_y);
    }
    /* Row accumulators are Q16 and 64-bit: u/z reaches a few million near the
     * near plane, which would leave a 32-bit Q16 accumulator. Only the per-row
     * and per-span steps pay for this; the pixel loop below stays 32-bit. */
#ifdef BSP_TEXTURED_ASM_SPANS
    /* Constant for the whole polygon; only the per-row fields change below. */
    TextureRowArgs row_args;
    row_args.texture = texture_pixels;
    row_args.texture_width = texture_width;
    row_args.u_mask = u_mask;
    row_args.v_mask = v_mask;
    row_args.inverse_depth_dx = plane.inverse_depth_dx;
    row_args.u_over_depth_dx = plane.u_over_depth_dx;
    row_args.v_over_depth_dx = plane.v_over_depth_dx;
    row_args.reciprocal = clipping_reciprocal_q24;
#endif
    int64_t inv_row = plane_value(plane.inverse_depth, plane.inverse_depth_dx,
                                  plane.inverse_depth_dy, plane.origin_xq8,
                                  plane.origin_yq8, 0, min_y, INVERSE_DEPTH_BITS);
    int64_t uoz_row = plane_value(plane.u_over_depth, plane.u_over_depth_dx,
                                  plane.u_over_depth_dy, plane.origin_xq8,
                                  plane.origin_yq8, 0, min_y, OVER_DEPTH_BITS);
    int64_t voz_row = plane_value(plane.v_over_depth, plane.v_over_depth_dx,
                                  plane.v_over_depth_dy, plane.origin_xq8,
                                  plane.origin_yq8, 0, min_y, OVER_DEPTH_BITS);
    for (int y = min_y; y <= max_y; ++y) {
        int left, right;
        if (!polygon_scanline(walkers, walker_count, y, &left, &right)) {
            inv_row += plane.inverse_depth_dy;
            uoz_row += plane.u_over_depth_dy;
            voz_row += plane.v_over_depth_dy;
            continue;
        }
#ifdef BSP_TEXTURED_NO_SPANS
        degenerate_face_count += (uint16_t)(left + right);  /* defeat DCE */
        inv_row += plane.inverse_depth_dy;
        uoz_row += plane.u_over_depth_dy;
        voz_row += plane.v_over_depth_dy;
        continue;
#endif
        /* Narrow once per row: from here to the end of the row every value is
         * on the face, so 32 bits suffice and the span loop stays 32-bit. */
        int32_t inv0 = (int32_t)(inv_row + (int64_t)plane.inverse_depth_dx * left);
        int32_t uoz0 = (int32_t)(uoz_row + (int64_t)plane.u_over_depth_dx * left);
        int32_t voz0 = (int32_t)(voz_row + (int64_t)plane.v_over_depth_dx * left);
#ifdef BSP_TEXTURED_ASM_SPANS
        /* One call per scanline. The kernel walks every 32-pixel segment of
         * the row itself, so the argument block is built once here rather than
         * once per segment. */
        row_args.row_pixels = logical_framebuffer + y * SCREEN_WIDTH;
        row_args.coverage_row = texture_coverage[y];
        row_args.left = left;
        row_args.right = right;
        row_args.inverse_depth = inv0;
        row_args.u_over_depth = uoz0;
        row_args.v_over_depth = voz0;
        draw_texture_row_arm(&row_args);
#else
        COUNT(drawn_row_count, 1);
        /* Segments stop at the next 32-pixel boundary, so a segment never
         * straddles two coverage words and is never longer than the 32-pixel
         * perspective interval. */
        for (int span = left; span <= right; ) {
            int end = minimum(right, span | span_mask_bits);
            COUNT(drawn_span_count, 1);
            int segment_pixels = end - span;
            /* Occlusion is decided before any perspective work. Faces
             * arrive near-to-far, so about a third of segments are already
             * fully covered; testing one mask first saves them four reciprocal
             * multiplies each. (An earlier attempt at early rejection lost,
             * but that was against per-pixel byte coverage, where the test
             * cost as much as the work it skipped.) */
            uint8_t *row = logical_framebuffer + y * SCREEN_WIDTH;
#ifndef BSP_TEXTURED_NO_COVERAGE
            uint32_t *coverage_word = &texture_coverage[y][span >> 5];
            uint32_t covered = *coverage_word;
            /* Bits this segment occupies. Segments are cut on 32-pixel
             * boundaries, so one never straddles two coverage words. */
            uint32_t span_mask = (0xffffffffu >> (31 - (unsigned)(end & 31))) &
                                 (0xffffffffu << (unsigned)(span & 31));
            uint32_t blocked = covered & span_mask;
            if (blocked == span_mask) {
                COUNT(span_hidden_count, 1);
                inv0 += plane.inverse_depth_dx * (segment_pixels + 1);
                uoz0 += plane.u_over_depth_dx * (segment_pixels + 1);
                voz0 += plane.v_over_depth_dx * (segment_pixels + 1);
                span = end + 1;
                continue;
            }
#else
            uint32_t blocked = 0;
#endif
            /* Perspective-correct at BOTH ends of the segment, with the
             * affine line fitted across the pair.
             *
             * Deriving the step from the gradient at the start alone is
             * cheaper and looks harmless on the tiny faces that dominate the
             * span-count average, but it anchors the affine line at one end
             * and lets it drift the whole way across. On a wall large enough
             * to fill a segment, seen at an angle so depth varies 10-20%
             * across it, that measured 6.82 texels of mean error against an
             * exact reference and 38.9 at worst -- visible as the texture
             * swimming as the camera turns. Fitting across both ends brings
             * it to 2.72, and halving the interval to 16 pixels to 0.86. */
            int32_t inv1 = inv0 + plane.inverse_depth_dx * segment_pixels;
            int32_t uoz1 = uoz0 + plane.u_over_depth_dx * segment_pixels;
            int32_t voz1 = voz0 + plane.v_over_depth_dx * segment_pixels;
            int u = texel_from_planes(uoz0, inv0);
            int v = texel_from_planes(voz0, inv0);
            int u1 = texel_from_planes(uoz1, inv1);
            int v1 = texel_from_planes(voz1, inv1);
            int du = divide_short_span(u1 - u, (unsigned)segment_pixels);
            int dv = divide_short_span(v1 - v, (unsigned)segment_pixels);
#if !defined(BSP_TEXTURED_SOLID) && !defined(BSP_TEXTURED_NO_FETCH) && \
    !defined(BSP_TEXTURED_NO_LIGHT)
            /* One luxel for the whole segment, taken at its midpoint.
             *
             * A luxel is 16 world units and a segment is at most 32 screen
             * pixels, so a segment spans two or three luxels at most, and the
             * lightmap is smooth at that scale: half of all adjacent luxel
             * pairs on this map are identical and 95% are within two shade
             * rows of each other. Interpolating the light per pixel instead
             * was built and measured, and cost 117,713 cycles a frame against
             * this version's 67,738 -- nearly twice the price for a difference
             * the segment-boundary column statistics cannot separate from the
             * texture's own noise.
             *
             * Sampled BEFORE the wrap below: u and v are still absolute texel
             * coordinates here, and a texture that tiles across the face wraps
             * them back to the same few texels while the lightmap must keep
             * running across the whole surface.
             *
             * The shift is one past BSP_LUXEL_SHIFT because the two ends are
             * summed rather than averaged; halving is folded into it.
             *
             * No per-axis clamp. Every face vertex is checked at build time
             * to land at least one luxel inside its block, and a point inside
             * a segment cannot stray much further: the affine error between
             * perspective corrections is bounded at 0.86 texels at this mip,
             * which is a tenth of a luxel. The replicated border covers the
             * rest. The mask is the only bounds test, and it is there so that
             * a segment surviving near-plane clipping with a very large 1/z
             * cannot read past the array -- it wraps instead of clamping
             * because at that point one segment is already wrong either way,
             * and wrapping is one instruction. */
            unsigned luxel_index = (unsigned)(
                luxel_base + (((v + v1) >> (BSP_LUXEL_SHIFT + 1)) * luxel_width) +
                             (((u + u1) >> (BSP_LUXEL_SHIFT + 1))));
            const uint8_t *const shade_row =
                shade_table +
                ((unsigned)bsp_lightmap_luxels[luxel_index & BSP_LUXEL_MASK] << 8);
#endif
#if !defined(BSP_TEXTURED_SOLID) && !defined(BSP_TEXTURED_NO_FETCH)
            /* Nearest texel, with Hecker's direction-stable tie break.
             *
             * Truncating u and v biases every lookup half a texel toward -u
             * and -v. Rounding fixes the bias but leaves exact halves landing
             * on whichever side the rounding happens to favour, so a texel
             * boundary alternates as a gradient changes sign. Adding 128 when
             * the gradient is positive and 127 when it is negative is the
             * fixed-point form of floor(x + 1/2) and ceil(x - 1/2), which
             * breaks the tie consistently against the direction of travel.
             *
             * Applied once per segment: the bias is a constant and survives
             * the per-pixel stepping, and du and dv cannot change sign inside
             * a segment. */
            /* (x >> 31) is -1 for a negative gradient and 0 otherwise, so
             * this is 128 or 127 without a branch. */
            u += 128 + (du >> 31);
            v += 128 + (dv >> 31);
            /* Wrap once here so the pre-scaled v cannot leave 32 bits, then
             * fold the row shift into v and its step. Both are per segment,
             * not per pixel. */
            u &= (int)u_wrap;
            v = (v & (int)v_wrap) << width_shift;
            dv <<= width_shift;
#endif
#ifdef BSP_TEXTURED_SOLID
            (void)u; (void)v; (void)du; (void)dv;   /* coverage-only diagnostic */
#endif
#ifdef BSP_TEXTURED_SOLID
#define TEXEL() (1u)
#elif defined(BSP_TEXTURED_NO_FETCH)
/* Keeps the perspective math, the u/v stepping and the stores, but replaces
 * the two masks, the row multiply and the texture load with something cheap.
 * The gap to the real build is the address-computation-plus-fetch budget. */
#define TEXEL() ((uint32_t)((u ^ v) >> 8) & 255u)
#elif defined(BSP_TEXTURED_NO_LIGHT)
/* v is pre-scaled by log2(width), so this is one masked shift per axis plus an
 * add - no row multiply. */
#define TEXEL() ((uint32_t)texture_pixels[ \
                    (((unsigned)(v >> 8)) & row_mask) + \
                    (((unsigned)(u >> 8)) & u_mask)])
#else
/* v is pre-scaled by log2(width), so the texel is one masked shift per axis
 * plus an add - no row multiply. The shade lookup that follows turns that
 * texel into the palette index it takes at this segment's light level, and
 * costs exactly one more byte load: the row pointer is a loop invariant. */
#define TEXEL() ((uint32_t)shade_row[texture_pixels[ \
                    (((unsigned)(v >> 8)) & row_mask) + \
                    (((unsigned)(u >> 8)) & u_mask)]])
#endif
/* Texels are fetched one at a time and cannot be batched: u and v step
 * independently, so consecutive pixels are never adjacent in the texture.
 *
 * The resulting pixels ARE consecutive in the framebuffer, so wider stores are
 * possible, and packing four texels into one word store was measured here at
 * -3.6K cycles. It is not used: the framebuffer is in IWRAM, where a byte
 * store and a word store both cost one cycle, so packing only trades three
 * stores for three ORs. Reaching a word store also means aligning the
 * destination first -- unaligned wide stores do not fault on ARM7TDMI, they
 * write to the aligned address -- and splitting the run into lead, aligned
 * body and tail costs three dispatches instead of one, measured at +30K. Ten
 * times what the wider stores return. */
#define SHIFT_ONE() do { u += du; v += dv; } while (0)
#define WRITE_ONE() do { *destination++ = (uint8_t)TEXEL(); SHIFT_ONE(); } while (0)
            /* Byte stores, deliberately. Packing four texels into one word
             * store was measured both here and with the framebuffer still in
             * EWRAM, and lost either way: spans average about five pixels, so
             * an aligned free quad rarely exists, and the packing shifts cost
             * more than the stores they replace. */
            if (!blocked) {
                /* Nothing in front of this run, so no per-pixel coverage test
                 * is needed: one mask OR at the end records the whole run.
                 *
                 * The run itself dispatches once into fully unrolled code
                 * rather than looping. Segments here average about five
                 * pixels, so a counted loop spends much of its time on the
                 * counter and the branch instead of on texels. Falling through
                 * a single body keeps one copy of the logic for every length. */
                COUNT(span_clear_count, 1);
                COUNT(texel_sample_count, (uint32_t)segment_pixels + 1);
                uint8_t *destination = row + span;
                switch ((unsigned)segment_pixels + 1) {
                case 32: WRITE_ONE(); __attribute__((fallthrough));
                case 31: WRITE_ONE(); __attribute__((fallthrough));
                case 30: WRITE_ONE(); __attribute__((fallthrough));
                case 29: WRITE_ONE(); __attribute__((fallthrough));
                case 28: WRITE_ONE(); __attribute__((fallthrough));
                case 27: WRITE_ONE(); __attribute__((fallthrough));
                case 26: WRITE_ONE(); __attribute__((fallthrough));
                case 25: WRITE_ONE(); __attribute__((fallthrough));
                case 24: WRITE_ONE(); __attribute__((fallthrough));
                case 23: WRITE_ONE(); __attribute__((fallthrough));
                case 22: WRITE_ONE(); __attribute__((fallthrough));
                case 21: WRITE_ONE(); __attribute__((fallthrough));
                case 20: WRITE_ONE(); __attribute__((fallthrough));
                case 19: WRITE_ONE(); __attribute__((fallthrough));
                case 18: WRITE_ONE(); __attribute__((fallthrough));
                case 17: WRITE_ONE(); __attribute__((fallthrough));
                case 16: WRITE_ONE(); __attribute__((fallthrough));
                case 15: WRITE_ONE(); __attribute__((fallthrough));
                case 14: WRITE_ONE(); __attribute__((fallthrough));
                case 13: WRITE_ONE(); __attribute__((fallthrough));
                case 12: WRITE_ONE(); __attribute__((fallthrough));
                case 11: WRITE_ONE(); __attribute__((fallthrough));
                case 10: WRITE_ONE(); __attribute__((fallthrough));
                case  9: WRITE_ONE(); __attribute__((fallthrough));
                case  8: WRITE_ONE(); __attribute__((fallthrough));
                case  7: WRITE_ONE(); __attribute__((fallthrough));
                case  6: WRITE_ONE(); __attribute__((fallthrough));
                case  5: WRITE_ONE(); __attribute__((fallthrough));
                case  4: WRITE_ONE(); __attribute__((fallthrough));
                case  3: WRITE_ONE(); __attribute__((fallthrough));
                case  2: WRITE_ONE(); __attribute__((fallthrough));
                case  1: WRITE_ONE(); break;
                default: break;
                }
#ifndef BSP_TEXTURED_NO_COVERAGE
                *coverage_word = covered | span_mask;
#endif
            }
#ifndef BSP_TEXTURED_NO_COVERAGE
            else {
                COUNT(span_mixed_count, 1);
                for (int x = span; x <= end; ++x) {
                    uint32_t bit = 1u << (x & 31);
                    if (!(covered & bit)) {
                        covered |= bit;
                        COUNT(texel_sample_count, 1);
                        row[x] = (uint8_t)TEXEL();
                    }
                    SHIFT_ONE();
                }
                *coverage_word = covered;
            }
#endif
#undef TEXEL
#undef SHIFT_ONE
#undef WRITE_ONE
            int advance = segment_pixels + 1;
            inv0 += plane.inverse_depth_dx * advance;
            uoz0 += plane.u_over_depth_dx * advance;
            voz0 += plane.v_over_depth_dx * advance;
            span = end + 1;
        }
#endif
        inv_row += plane.inverse_depth_dy;
        uoz_row += plane.u_over_depth_dy;
        voz_row += plane.v_over_depth_dy;
    }
}
