/* BSP surface texture coordinates, near clipping, projection, and submission. */
/* The texture axes and origin live in Game Pak ROM and are identical for every
 * vertex of a face, so they are lifted onto the IWRAM stack once per face
 * rather than re-read per vertex. */
typedef struct {
    int32_t axis[2][4];
    int32_t u_base_q8, v_base_q8;
    uint16_t texture;
} FaceTexture;

static FaceTexture face_texture(const RuntimeFace *face)
{
    const MapFace *source = &bsp_faces[face->source_face];
    const MapTexInfo *info = &bsp_texinfo[source->texinfo];
    FaceTexture result;
    for (unsigned axis = 0; axis < 2; ++axis)
        for (unsigned term = 0; term < 4; ++term)
            result.axis[axis][term] = info->axis[axis][term];
    result.u_base_q8 = source->u_base_q8;
    result.v_base_q8 = source->v_base_q8;
    result.texture = info->texture;
    return result;
}

/* Quake texture coordinates are world-space: s = dot(vertex, axis) + offset.
 * The axes are Q12, so the dot product is Q12 texels and >>4 leaves Q8.
 * Subtracting the face's own origin keeps the value near zero instead of
 * several thousand texels, which is what the u/z pipeline needs to stay
 * precise. The origin is a multiple of the texture size, so wrapping is
 * unaffected. */
static void world_texture_coordinates(MapVertex world, const FaceTexture *texture,
                                      int32_t *u_q8, int32_t *v_q8)
{
    *u_q8 = ((texture->axis[0][0] * world.x + texture->axis[0][1] * world.y +
              texture->axis[0][2] * world.z + texture->axis[0][3]) >> 4)
            - texture->u_base_q8;
    *v_q8 = ((texture->axis[1][0] * world.x + texture->axis[1][1] * world.y +
              texture->axis[1][2] * world.z + texture->axis[1][3]) >> 4)
            - texture->v_base_q8;
}

extern void draw_textured_polygon_arm(const TextureVertex *vertices,
                                      unsigned vertex_count, uint16_t texture_index);

static ClipTextureVertex clip_texture_vertex(unsigned vertex,
                                             const FaceTexture *texture)
{
    CameraPoint camera = camera_cache[vertex];
    ClipTextureVertex result = {camera.horizontal, camera.vertical, camera.depth, 0, 0};
    world_texture_coordinates(runtime_vertices[vertex], texture,
                              &result.u_q8, &result.v_q8);
    return result;
}

/* Camera lookup, texture coordinates and projection for one ring vertex,
 * written straight into the output ring.
 *
 * Kept as one function on purpose: composing the two halves meant the 20-byte
 * intermediate went out to the stack and came straight back, and the ring had
 * to be walked twice because the near-plane test needed the depth first. */
static void project_ring_vertex(unsigned vertex, const FaceTexture *texture,
                                TextureVertex *out)
{
    CameraPoint camera = camera_cache[vertex];
    int32_t u_q8, v_q8;
    world_texture_coordinates(runtime_vertices[vertex], texture, &u_q8, &v_q8);
    int depth_index = maximum(1, minimum(4095, Q8_TO_INT(camera.depth + 128)));
    int32_t inverse_depth = projection_reciprocal_q16[depth_index];
    int32_t across = (int32_t)(((int64_t)camera.horizontal * inverse_depth) >> 12);
    int32_t down = (int32_t)(((int64_t)camera.vertical * inverse_depth) >> 12);
    out->xq8 = Q8_FROM_INT(60) + ((across * FOCAL_LENGTH) >> 4);
    out->yq8 = Q8_FROM_INT(40) - ((down * FOCAL_LENGTH) >> 4);
    out->x = Q8_TO_INT(out->xq8);
    out->y = Q8_TO_INT(out->yq8);
    out->inverse_depth = inverse_depth;
    out->u_over_depth = Q8_MUL(u_q8, inverse_depth);
    out->v_over_depth = Q8_MUL(v_q8, inverse_depth);
}

static ClipTextureVertex interpolate_clip_vertex(const ClipTextureVertex *a,
                                                  const ClipTextureVertex *b,
                                                  int32_t fraction)
{
    ClipTextureVertex result;
    result.horizontal = a->horizontal + multiply_q16(b->horizontal - a->horizontal, fraction);
    result.vertical = a->vertical + multiply_q16(b->vertical - a->vertical, fraction);
    result.depth = a->depth + multiply_q16(b->depth - a->depth, fraction);
    result.u_q8 = a->u_q8 + multiply_q16(b->u_q8 - a->u_q8, fraction);
    result.v_q8 = a->v_q8 + multiply_q16(b->v_q8 - a->v_q8, fraction);
    return result;
}

static unsigned clip_texture_polygon_near(const ClipTextureVertex *input,
                                          unsigned count,
                                          ClipTextureVertex *output)
{
    unsigned output_count = 0;
    ClipTextureVertex previous = input[count - 1];
    int previous_inside = previous.depth >= NEAR_PLANE_Q8;
    for (unsigned i = 0; i < count; ++i) {
        ClipTextureVertex current = input[i];
        int current_inside = current.depth >= NEAR_PLANE_Q8;
        if (current_inside != previous_inside) {
            int fraction = signed_ratio_q16_lut(NEAR_PLANE_Q8 - previous.depth,
                                                current.depth - previous.depth,
                                                clipping_reciprocal_q24);
            output[output_count++] = interpolate_clip_vertex(&previous, &current, fraction);
        }
        if (current_inside) output[output_count++] = current;
        previous = current;
        previous_inside = current_inside;
    }
    return output_count;
}

/* One reciprocal per vertex, shared by the screen position and by 1/z.
 *
 * Using separate lookups here was a real defect: the screen position rounded
 * the depth index while the inverse depth truncated it, so the two disagreed
 * and the fitted texture plane sheared. Screen coordinates are kept at Q8 so
 * the gradient fit sees where the vertex really landed rather than which pixel
 * it snapped to; the integer fields remain for coverage and edge walking. */
static TextureVertex project_texture_vertex(const ClipTextureVertex *source)
{
    int depth_index = maximum(1, minimum(4095, Q8_TO_INT(source->depth + 128)));
    int32_t inverse_depth = projection_reciprocal_q16[depth_index];
    /* horizontal is Q8 and inverse_depth is 65536/z, so the product is
     * h * 2^24 / z; >>12 then *56 >>4 leaves 56 * h / z in Q8 pixels. */
    int32_t across = (int32_t)(((int64_t)source->horizontal * inverse_depth) >> 12);
    int32_t down = (int32_t)(((int64_t)source->vertical * inverse_depth) >> 12);
    TextureVertex result;
    result.xq8 = Q8_FROM_INT(60) + ((across * FOCAL_LENGTH) >> 4);
    result.yq8 = Q8_FROM_INT(40) - ((down * FOCAL_LENGTH) >> 4);
    result.x = Q8_TO_INT(result.xq8);
    result.y = Q8_TO_INT(result.yq8);
    result.inverse_depth = inverse_depth;
    result.u_over_depth = Q8_MUL(source->u_q8, inverse_depth);
    result.v_over_depth = Q8_MUL(source->v_q8, inverse_depth);
    return result;
}

static HOT void render_textured_faces(void)
{
    for (unsigned y = 0; y < SCREEN_HEIGHT; ++y)
        for (unsigned word = 0; word < 4; ++word)
            texture_coverage[y][word] = 0;
    for (unsigned accepted = 0; accepted < accepted_face_count; ++accepted) {
        const RuntimeFace *face = &runtime_faces[frame_faces[accepted]];
        if (face->edge_count < 3) continue;
        FaceTexture face_texture_info = face_texture(face);
        unsigned count = minimum(face->edge_count, 32);
        /* Resolve the vertex ring once. surfedge -> edge -> vertex is three
         * dependent loads across ROM and EWRAM, and the ring is walked twice
         * on the clipped path. */
        uint16_t ring[32];
        int32_t behind = 0;
        TextureVertex projected[33];
        for (unsigned i = 0; i < count; ++i) {
            unsigned vertex = bsp_face_vertices[face->first_vertex + i];
            ring[i] = (uint16_t)vertex;
            /* OR accumulates the sign bit, so this is negative if ANY
             * vertex is nearer than the near plane. */
            behind |= camera_cache[vertex].depth - NEAR_PLANE_Q8;
            /* Projected unconditionally. Four faces in 122 turn out to need
             * clipping and redo this below; paying for those is cheaper than
             * walking the ring a second time for the other 118. */
            project_ring_vertex(vertex, &face_texture_info, &projected[i]);
        }
        if (behind < 0) {
            /* Near clipping needed. Materialise the clip ring, which is the
             * only reason the intermediate array exists. */
            COUNT(near_clipped_faces, 1);
            ClipTextureVertex unclipped[32], clipped[33];
            for (unsigned i = 0; i < count; ++i)
                unclipped[i] = clip_texture_vertex(ring[i], &face_texture_info);
            count = clip_texture_polygon_near(unclipped, count, clipped);
            if (count < 3) continue;
            for (unsigned i = 0; i < count; ++i)
                projected[i] = project_texture_vertex(&clipped[i]);
        }
#ifdef BSP_TEXTURED_NO_POLYGON
        degenerate_face_count += (uint16_t)(projected[0].x + projected[0].u_over_depth);
        continue;
#endif
        uint16_t texture = face_texture_info.texture;
#if defined(BSP_TEXTURED_SOLID) || defined(BSP_TEXTURED_NO_COVERAGE) || \
    defined(BSP_TEXTURED_C_REFERENCE)
        draw_textured_polygon_reference(projected, count, texture);
#else
        draw_textured_polygon_arm(projected, count, texture);
#endif
    }
}
