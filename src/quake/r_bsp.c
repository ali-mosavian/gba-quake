/* BSP/PVS traversal, face rejection, edge lists, and vertex transformation. */
static int absolute(int value) { return value < 0 ? -value : value; }

static unsigned find_camera_leaf(int32_t x, int32_t y, int32_t z)
{
    int node = 0;
    while (node >= 0) {
        const MapNode *entry = &bsp_nodes[node];
        int32_t distance = plane_distance_q8_arm(&bsp_planes[entry->plane], x, y, z);
        node = entry->children[distance >= 0 ? 0 : 1];
    }
    return (unsigned)(-node - 1);
}

static void append_leaf_faces(unsigned leaf_index)
{
    if (leaf_index >= BSP_LEAF_COUNT) return;
    const MapLeaf *leaf = &bsp_leaves[leaf_index];
    unsigned end = leaf->first_mark + leaf->mark_count;
    for (unsigned mark = leaf->first_mark; mark < end; ++mark) {
        unsigned face = bsp_marksurfaces[mark];
        if (face_stamp[face] == candidate_stamp) continue;
        face_stamp[face] = candidate_stamp;
        candidate_faces[candidate_face_count++] = (uint16_t)face;
    }
}

#ifdef BSP_TEXTURED
static void append_candidate_faces_ordered(int node_index)
{
    if (node_index < 0) return;
    const MapNode *node = &bsp_nodes[node_index];
    unsigned near_side = node_near_side[node_index];
    append_candidate_faces_ordered(node->children[near_side]);
    unsigned end = node->first_face + node->face_count;
    for (unsigned face = node->first_face; face < end; ++face)
        if (face_stamp[face] == candidate_stamp)
            candidate_faces[candidate_face_count++] = (uint16_t)face;
    append_candidate_faces_ordered(node->children[near_side ^ 1]);
}
#endif

static void rebuild_candidate_faces(unsigned camera_leaf, int32_t camera_x,
                                    int32_t camera_y, int32_t camera_z)
{
    if (++candidate_stamp == 0) {
        for (unsigned i = 0; i < BSP_FACE_COUNT; ++i) face_stamp[i] = 0;
        candidate_stamp = 1;
    }
    candidate_face_count = 0;
    append_leaf_faces(camera_leaf);
    int32_t offset = bsp_leaves[camera_leaf].visibility;
    if (offset < 0) {
        for (unsigned leaf = 1; leaf < BSP_LEAF_COUNT; ++leaf) append_leaf_faces(leaf);
        goto cache_faces;
    }
    unsigned leaf = 1;
    while (leaf < BSP_LEAF_COUNT && (unsigned)offset < BSP_VISIBILITY_BYTES) {
        uint8_t value = bsp_visibility[offset++];
        if (value) {
            for (unsigned bit = 0; bit < 8 && leaf + bit < BSP_LEAF_COUNT; ++bit)
                if (value & (1u << bit)) append_leaf_faces(leaf + bit);
            leaf += 8;
        } else if ((unsigned)offset < BSP_VISIBILITY_BYTES) {
            leaf += (unsigned)bsp_visibility[offset++] * 8;
        }
    }
cache_faces:
#ifdef BSP_TEXTURED
    for (unsigned node = 0; node < BSP_NODE_COUNT; ++node) {
        int32_t distance = plane_distance_q8_arm(&bsp_planes[bsp_nodes[node].plane],
                                                 camera_x, camera_y, camera_z);
        node_near_side[node] = distance >= 0 ? 0 : 1;
    }
    candidate_face_count = 0;
    append_candidate_faces_ordered(0);
#else
    (void)camera_x; (void)camera_y; (void)camera_z;
#endif
    for (unsigned i = 0; i < candidate_face_count; ++i) {
        unsigned face_index = candidate_faces[i];
        const MapFace *face = &bsp_faces[face_index];
        const MapPlane *plane = &bsp_planes[face->plane];
        /* Fold the face's side flag into the stored normal, once per leaf
         * change rather than once per candidate per frame. A back-facing test
         * then reduces to a sign check with no branch on side and no extra
         * load. */
        int flip = face->side ? -1 : 1;
        runtime_faces[i] = (RuntimeFace){
            (int16_t)(flip * plane->nx), (int16_t)(flip * plane->ny),
            (int16_t)(flip * plane->nz), (int16_t)(flip * plane->distance),
            face->center_x, face->center_y, face->center_z, face->radius,
            face->first_edge, (uint16_t)face_index, face->edge_count, face->side
        };
    }
}

static __attribute__((unused)) CameraPoint to_camera(MapVertex vertex, int32_t camera_x,
                             int32_t camera_y, int32_t camera_z,
                             int sine, int cosine)
{
    int32_t dx = Q8_FROM_INT(vertex.x) - camera_x;
    int32_t dy = Q8_FROM_INT(vertex.y) - camera_y;
    CameraPoint result = {
        Q14_DOT2(-sine, dx, cosine, dy),
        Q8_FROM_INT(vertex.z) - camera_z,
        Q14_DOT2(cosine, dx, sine, dy),
    };
    return result;
}

/* Culling runs in whole world units, not Q8.
 *
 * Both tests are conservative bounding-sphere rejections whose radius is
 * already rounded up, so the camera's sub-unit position cannot change the
 * outcome once the radius carries a unit of slack for the rounding. Working
 * in integer units keeps every product inside 32 bits -- a Q14 normal is 15
 * bits against a 13-bit world coordinate -- where the Q8 form needed 15 by 21
 * and therefore a 64-bit multiply on every term.
 *
 * The camera is ROUNDED to the nearest unit rather than truncated, so the
 * centre-to-camera vector is off by at most 0.87 units, which the ceiling
 * already applied to the stored radius absorbs. Truncating instead needs an
 * explicit unit of slack, and that extra slack was measured to admit two more
 * faces than it saved culling work. */
static inline __attribute__((always_inline)) int face_is_in_view(
                           const RuntimeFace *face, int32_t camera_x,
                           int32_t camera_y, int32_t camera_z,
                           int sine, int cosine)
{
    int32_t dx = face->center_x - camera_x;
    int32_t dy = face->center_y - camera_y;
    int32_t depth = (cosine * dx + sine * dy) >> 14;
    int32_t horizontal = absolute((cosine * dy - sine * dx) >> 14);
    int32_t vertical = absolute(face->center_z - camera_z);
    int radius = face->radius;
    if (depth + radius < 8) return 0;
    if (horizontal > radius && (horizontal - radius) * FOCAL_LENGTH > depth * 60) return 0;
    if (vertical > radius && (vertical - radius) * FOCAL_LENGTH > depth * 40) return 0;
    return 1;
}

/* Signed side of a face plane, in Q14 world units, from an integer camera
 * position. Only the sign is used, and a Q14 normal against a 13-bit
 * coordinate stays inside 32 bits, so no 64-bit multiply is needed. */
static inline __attribute__((always_inline)) int32_t runtime_plane_side(
                            const RuntimeFace *face, int32_t x, int32_t y, int32_t z)
{
    return face->nx * x + face->ny * y + face->nz * z -
           ((int32_t)face->distance << 14);
}

static void append_vertex(unsigned vertex)
{
    if (vertex_stamp[vertex] == frame_stamp) return;
    vertex_stamp[vertex] = frame_stamp;
    frame_vertices[frame_vertex_count++] = (uint16_t)vertex;
}

static __attribute__((unused)) void append_edge(unsigned edge)
{
    if (edge_stamp[edge] == frame_stamp) return;
    edge_stamp[edge] = frame_stamp;
    frame_edges[frame_edge_count++] = (uint16_t)edge;
    append_vertex(runtime_edges[edge].first);
    append_vertex(runtime_edges[edge].second);
}

static HOT void build_frame_lists(int32_t camera_x, int32_t camera_y,
                              int32_t camera_z, uint8_t yaw)
{
    int sine = sine_q14[yaw], cosine = sine_q14[(uint8_t)(yaw + 64)];
    int32_t camera_ix = Q8_TO_INT(camera_x + 128);
    int32_t camera_iy = Q8_TO_INT(camera_y + 128);
    int32_t camera_iz = Q8_TO_INT(camera_z + 128);
    frame_edge_count = frame_vertex_count = accepted_face_count = 0;
    for (unsigned i = 0; i < candidate_face_count; ++i) {
        const RuntimeFace *face = &runtime_faces[i];
        /* The normal already carries the face's side, so the camera being on
         * or behind the plane is a single sign test. */
        if (runtime_plane_side(face, camera_ix, camera_iy, camera_iz) >= 0) continue;
        if (!face_is_in_view(face, camera_ix, camera_iy, camera_iz, sine, cosine)) continue;
#ifdef BSP_TEXTURED
        frame_faces[accepted_face_count] = (uint16_t)i;
#endif
        ++accepted_face_count;
        for (unsigned edge_index = 0; edge_index < face->edge_count; ++edge_index) {
            int edge = bsp_surfedges[face->first_edge + edge_index];
#ifdef BSP_TEXTURED
            /* The textured path never consumes frame_edges; it needs only the
             * unique vertex list for the batched transform. */
            MapEdge entry = runtime_edges[(unsigned)(edge < 0 ? -edge : edge)];
            append_vertex(entry.first);
            append_vertex(entry.second);
#else
            append_edge((unsigned)(edge < 0 ? -edge : edge));
#endif
        }
    }
}

static void transform_project_frame_vertices(int32_t camera_x, int32_t camera_y,
                                             int32_t camera_z, uint8_t yaw)
{
    TransformArgs args = {
        frame_vertices, frame_vertex_count, runtime_vertices, camera_cache,
        camera_x, camera_y, camera_z, sine_q14[yaw], sine_q14[(uint8_t)(yaw + 64)],
        screen_cache, vertex_outcode, projection_reciprocal_q16
    };
    transform_vertices_arm(&args);
}
