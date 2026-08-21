/* BSP surface texture coordinates, near clipping, projection, and submission. */
/* Texture coordinates are read, not computed.
 *
 * s and t are a function of the world vertex and the face's texinfo alone, so
 * evaluating them per frame evaluates a constant -- six multiplies and eight
 * ROM axis reads per ring vertex, measured at 93K cycles a frame at the spawn.
 * The extractor bakes them into bsp_face_texcoords[], one pair per ring entry
 * with the face's texture origin already subtracted. Per ring entry rather
 * than per vertex because a vertex shared by three faces has three different
 * pairs: each face subtracts its own origin.
 *
 * What is left of the per-face texture state is the texture index, so there
 * is no longer a record to lift onto the stack. */
static uint16_t face_texture(const RuntimeFace *face)
{
    return bsp_texinfo[bsp_faces[face->source_face].texinfo].texture;
}

static ClipTextureVertex clip_texture_vertex(unsigned vertex, unsigned ring_index)
{
    CameraPoint camera = camera_cache[vertex];
    ClipTextureVertex result = {camera.horizontal, camera.vertical, camera.depth,
                                bsp_face_texcoords[2 * ring_index],
                                bsp_face_texcoords[2 * ring_index + 1]};
    return result;
}

extern void draw_textured_polygon_arm(const TextureVertex *vertices,
                                      unsigned vertex_count, uint16_t texture_index);

/* Camera lookup, texture coordinates and projection for one ring vertex,
 * written straight into the output ring.
 *
 * Kept as one function on purpose: composing the two halves meant the 20-byte
 * intermediate went out to the stack and came straight back, and the ring had
 * to be walked twice because the near-plane test needed the depth first. */
static void project_ring_vertex(unsigned vertex, unsigned ring_index,
                                TextureVertex *out)
{
    CameraPoint camera = camera_cache[vertex];
    int32_t u_q8 = bsp_face_texcoords[2 * ring_index];
    int32_t v_q8 = bsp_face_texcoords[2 * ring_index + 1];
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

/* Signed distance to one clip plane, in camera space.
 *
 * plane 0 is the near plane; 1..4 are the frustum sides, expressed so that a
 * vertex is inside when the result is >= 0:
 *   right   60*z - 56*h      left  60*z + 56*h
 *   bottom  40*z - 56*v      top   40*z + 56*v
 * which is exactly screen x in [0,120] and y in [0,80] after projection. */
static int32_t clip_distance(const ClipTextureVertex *vertex, unsigned plane)
{
    switch (plane) {
    case 0: return vertex->depth - NEAR_PLANE_Q8;
    case 1: return 60 * vertex->depth - FOCAL_LENGTH * vertex->horizontal;
    case 2: return 60 * vertex->depth + FOCAL_LENGTH * vertex->horizontal;
    case 3: return 40 * vertex->depth - FOCAL_LENGTH * vertex->vertical;
    default: return 40 * vertex->depth + FOCAL_LENGTH * vertex->vertical;
    }
}

/* Sutherland-Hodgman against one plane. */
static unsigned clip_polygon_plane(const ClipTextureVertex *input, unsigned count,
                                   ClipTextureVertex *output, unsigned plane)
{
    unsigned written = 0;
    ClipTextureVertex previous = input[count - 1];
    int32_t previous_distance = clip_distance(&previous, plane);
    for (unsigned i = 0; i < count; ++i) {
        ClipTextureVertex current = input[i];
        int32_t current_distance = clip_distance(&current, plane);
        if ((previous_distance < 0) != (current_distance < 0)) {
            int fraction = signed_ratio_q16_lut(-previous_distance,
                                                current_distance - previous_distance,
                                                clipping_reciprocal_q24);
            output[written++] = interpolate_clip_vertex(&previous, &current, fraction);
        }
        if (current_distance >= 0) output[written++] = current;
        previous = current;
        previous_distance = current_distance;
    }
    return written;
}

/* Clip against the near plane and the four frustum sides.
 *
 * The sides are not needed for coverage -- the rasteriser already clamps spans
 * to the viewport -- they are needed for precision. A wall seen obliquely
 * throws its far vertices thousands of pixels outside a 120-pixel screen, and
 * the plane fit through coordinates that large loses most of its significant
 * bits. Measured against an exact reference, a screen-filling wall was off by
 * 836 texels on average and 10,026 at worst; clipping first bounds every
 * projected coordinate to the viewport and brings that to 0.21 and 1.65. That
 * is the texture swim visible when turning to face a wall at an angle. */
static unsigned clip_texture_polygon(ClipTextureVertex *ring, unsigned count,
                                     ClipTextureVertex *scratch, unsigned planes)
{
    /* Only the planes the face actually crosses. Clipping against one plane
     * cannot push a vertex outside another: every vertex it creates lies on a
     * segment between two existing ones, so it stays inside the convex hull
     * of a polygon that already satisfied the others. */
    for (unsigned plane = 0; plane < 5 && count >= 3; ++plane) {
        if (!(planes & (1u << plane))) continue;
        count = clip_polygon_plane(ring, count, scratch, plane);
        for (unsigned i = 0; i < count; ++i) ring[i] = scratch[i];
    }
    return count;
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

/* Draw one mover's faces at its current offset.
 *
 * Movers are drawn BEFORE the world, coverage-first. The rooms behind a
 * closed door are still in the PVS -- vis treats doors as open -- so drawing
 * the door after the world would find its doorway already covered by the
 * room beyond and vanish. Drawing it first is correct except where world
 * geometry stands between the camera and the mover, which for these six
 * (doors in doorways, buttons on walls) is a fringe case accepted as such.
 *
 * The vertices are transformed here rather than by the frame's batched
 * transform: no leaf references a submodel's faces, so its vertices are
 * never in the frame set. The offset is added in world space before the
 * rotate; texture coordinates and lightmaps ride along unchanged because
 * both were baked against the brush's own geometry. */
static HOT void render_entity(unsigned index)
{
    const MapEntity *entity = &bsp_entities[index];
    int32_t offset_x = entity_offset_q8(index, 0);
    int32_t offset_y = entity_offset_q8(index, 1);
    int32_t offset_z = entity_offset_q8(index, 2);
    int32_t sine = entity_camera_sine, cosine = entity_camera_cosine;
    /* One conservative sphere test for the whole mover. */
    {
        int32_t cx = ((entity->mins[0] + entity->maxs[0]) << 7) + offset_x;
        int32_t cy = ((entity->mins[1] + entity->maxs[1]) << 7) + offset_y;
        int32_t cz = ((entity->mins[2] + entity->maxs[2]) << 7) + offset_z;
        int32_t dx = Q8_TO_INT(cx - entity_camera_x);
        int32_t dy = Q8_TO_INT(cy - entity_camera_y);
        int32_t dz = Q8_TO_INT(cz - entity_camera_z);
        int radius = (entity->maxs[0] - entity->mins[0] +
                      entity->maxs[1] - entity->mins[1] +
                      entity->maxs[2] - entity->mins[2]) >> 1;
        int32_t depth = (cosine * dx + sine * dy) >> 14;
        if (depth + radius < 8) return;
        int32_t horizontal = cosine * dy - sine * dx;
        horizontal = (horizontal < 0 ? -horizontal : horizontal) >> 14;
        if (horizontal > radius &&
            (horizontal - radius) * FOCAL_LENGTH > depth * 60) return;
        if (dz < 0) dz = -dz;
        if (dz > radius && (dz - radius) * FOCAL_LENGTH > depth * 40) return;
    }
#ifdef BSP_ENTITY_CULL_ONLY
    return;
#endif
    int32_t camera_ix = Q8_TO_INT(entity_camera_x - offset_x + 128);
    int32_t camera_iy = Q8_TO_INT(entity_camera_y - offset_y + 128);
    int32_t camera_iz = Q8_TO_INT(entity_camera_z - offset_z + 128);
    for (unsigned f = 0; f < entity->face_count; ++f) {
        unsigned face_index = entity->first_face + f;
        const MapFace *face = &bsp_faces[face_index];
        const MapPlane *plane = &bsp_planes[face->plane];
        int flip = face->side ? -1 : 1;
        /* Back-face: the camera in the mover's own space. */
        int64_t side = (int64_t)flip *
            ((int64_t)plane->nx * camera_ix + (int64_t)plane->ny * camera_iy +
             (int64_t)plane->nz * camera_iz -
             ((int64_t)plane->distance << 14));
        if (side <= 0) continue;
        unsigned count = minimum(face->edge_count, CLIP_RING_MAX - 8);
        /* Statics, not stack: this runs on top of the world path's already
         * deep IWRAM stack, and 2.7KB more of ring buffers before the drawer's
         * own frame overflowed into .bss -- which corrupted the culling
         * statics and showed up, deceptively, as a 456K cycle "speedup" from
         * 55 vanished faces. The pass is not reentrant, so shared EWRAM
         * buffers cost nothing but a slightly slower bus on ~40 faces. */
        EWRAM static TextureVertex projected[CLIP_RING_MAX];
        EWRAM static ClipTextureVertex polygon[CLIP_RING_MAX],
                                       scratch[CLIP_RING_MAX];
        /* Project first and clip only what crosses, exactly like the world
         * path. The first version forced the full four-plane clip on every
         * face -- from ARM code in ROM, where each instruction is two bus
         * fetches -- and those two decisions together cost 170K a frame at
         * a pose where five mover faces are visible. */
        unsigned planes_crossed = 0;
        for (unsigned i = 0; i < count; ++i) {
            unsigned ring_index = (unsigned)face->first_vertex + i;
            MapVertex world = bsp_vertices[bsp_face_vertices[ring_index]];
            int32_t wx = Q8_FROM_INT(world.x) + offset_x - entity_camera_x;
            int32_t wy = Q8_FROM_INT(world.y) + offset_y - entity_camera_y;
            int32_t wz = Q8_FROM_INT(world.z) + offset_z - entity_camera_z;
            int32_t depth = (int32_t)(((int64_t)cosine * wx +
                                       (int64_t)sine * wy) >> 14);
            int32_t horizontal = (int32_t)(((int64_t)cosine * wy -
                                            (int64_t)sine * wx) >> 14);
            polygon[i].horizontal = horizontal;
            polygon[i].vertical = wz;
            polygon[i].depth = depth;
            polygon[i].u_q8 = bsp_face_texcoords[2 * ring_index];
            polygon[i].v_q8 = bsp_face_texcoords[2 * ring_index + 1];
            if (depth < NEAR_PLANE_Q8) { planes_crossed |= 1u; continue; }
            projected[i] = project_texture_vertex(&polygon[i]);
            if (projected[i].xq8 > Q8_FROM_INT(SCREEN_WIDTH)) planes_crossed |= 2u;
            if (projected[i].xq8 < 0) planes_crossed |= 4u;
            if (projected[i].yq8 > Q8_FROM_INT(SCREEN_HEIGHT)) planes_crossed |= 8u;
            if (projected[i].yq8 < 0) planes_crossed |= 16u;
        }
        if (planes_crossed & 1u) planes_crossed = 31u;
        if (planes_crossed) {
            count = clip_texture_polygon(polygon, count, scratch, planes_crossed);
            if (count < 3) continue;
            for (unsigned i = 0; i < count; ++i)
                projected[i] = project_texture_vertex(&polygon[i]);
        }
#ifndef BSP_ENTITY_NO_DRAW_CALL
        draw_textured_polygon_reference(
            projected, count,
            bsp_texinfo[face->texinfo].texture,
            &bsp_face_lights[face_index]);
#else
        degenerate_face_count += (uint16_t)projected[0].x;
#endif
    }
}

static HOT void render_textured_faces(void)
{
    for (unsigned y = 0; y < SCREEN_HEIGHT; ++y)
        for (unsigned word = 0; word < 4; ++word)
            texture_coverage[y][word] = 0;
#ifndef BSP_NO_ENTITY_DRAW
    for (unsigned entity = 0; entity < BSP_ENTITY_COUNT; ++entity)
        if (entity_is_solid(&bsp_entities[entity]))
            render_entity(entity);
#endif
    for (unsigned accepted = 0; accepted < accepted_face_count; ++accepted) {
        const RuntimeFace *face = &runtime_faces[frame_faces[accepted]];
        if (face->edge_count < 3) continue;
        uint16_t texture = face_texture(face);
        unsigned count = minimum(face->edge_count, CLIP_RING_MAX - 8);
        /* Resolve the vertex ring once. surfedge -> edge -> vertex is three
         * dependent loads across ROM and EWRAM, and the ring is walked twice
         * on the clipped path. */
        uint16_t ring[CLIP_RING_MAX];
        unsigned planes = 0;
        TextureVertex projected[CLIP_RING_MAX];
        for (unsigned i = 0; i < count; ++i) {
            unsigned ring_index = (unsigned)face->first_vertex + i;
            unsigned vertex = bsp_face_vertices[ring_index];
            ring[i] = (uint16_t)vertex;
            /* Note which clip planes this face crosses. The near plane is
             * needed for coverage; the four sides are needed for precision,
             * because a vertex projecting far outside the viewport costs the
             * plane fit most of its significant bits. */
            if (camera_cache[vertex].depth < NEAR_PLANE_Q8) planes |= 1u;
            /* Projected unconditionally; faces that turn out to need clipping
             * redo it below, and that is cheaper than walking the ring twice
             * for the ones that do not. */
            project_ring_vertex(vertex, ring_index, &projected[i]);
            if (projected[i].xq8 > Q8_FROM_INT(SCREEN_WIDTH)) planes |= 2u;
            if (projected[i].xq8 < 0) planes |= 4u;
            if (projected[i].yq8 > Q8_FROM_INT(SCREEN_HEIGHT)) planes |= 8u;
            if (projected[i].yq8 < 0) planes |= 16u;
        }
        /* A vertex behind the near plane has a meaningless projection, so its
         * side-plane flags cannot be trusted, and near clipping can move the
         * polygon far off screen anyway. Clip such faces against everything. */
        if (planes & 1u) planes = 31u;
#ifdef BSP_NO_CLIP
        planes = 0;   /* diagnostic: measure the clip path's cost */
#endif
        if (planes) {
            COUNT(near_clipped_faces, 1);
            ClipTextureVertex polygon[CLIP_RING_MAX], scratch[CLIP_RING_MAX];
            for (unsigned i = 0; i < count; ++i)
                polygon[i] = clip_texture_vertex(
                    ring[i], (unsigned)face->first_vertex + i);
            count = clip_texture_polygon(polygon, count, scratch, planes);
            if (count < 3) continue;
            for (unsigned i = 0; i < count; ++i)
                projected[i] = project_texture_vertex(&polygon[i]);
        }
#ifdef BSP_TEXTURED_NO_POLYGON
        degenerate_face_count += (uint16_t)(projected[0].x + projected[0].u_over_depth);
        continue;
#endif
#if defined(BSP_TEXTURED_SOLID) || defined(BSP_TEXTURED_NO_COVERAGE) || \
    defined(BSP_TEXTURED_C_REFERENCE)
        draw_textured_polygon_reference(projected, count, texture,
                                        &bsp_face_lights[face->source_face]);
#else
        draw_textured_polygon_arm(projected, count, texture);
#endif
    }
}
