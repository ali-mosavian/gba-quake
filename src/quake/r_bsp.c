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
        runtime_faces[i] = (RuntimeFace){
            plane->nx, plane->ny, plane->nz, plane->distance,
            face->center_x, face->center_y, face->center_z, face->radius,
            face->first_edge, (uint16_t)face_index, face->edge_count, face->side
        };
    }
}

static CameraPoint to_camera(MapVertex vertex, int32_t camera_x,
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

static inline __attribute__((always_inline)) int face_is_in_view(
                           const RuntimeFace *face, int32_t camera_x,
                           int32_t camera_y, int32_t camera_z,
                           int sine, int cosine)
{
    MapVertex center = {face->center_x, face->center_y, face->center_z};
    CameraPoint point = to_camera(center, camera_x, camera_y, camera_z, sine, cosine);
    int radius = face->radius;
    int depth = Q8_TO_INT(point.depth);
    int horizontal = absolute(Q8_TO_INT(point.horizontal));
    int vertical = absolute(Q8_TO_INT(point.vertical));
    if (depth + radius < 8) return 0;
    if (horizontal > radius && (horizontal - radius) * FOCAL_LENGTH > depth * 60) return 0;
    if (vertical > radius && (vertical - radius) * FOCAL_LENGTH > depth * 40) return 0;
    return 1;
}

static inline __attribute__((always_inline)) int32_t runtime_plane_distance_q8(
                            const RuntimeFace *face, int32_t x, int32_t y, int32_t z)
{
    int64_t sum = (int64_t)face->nx * x + (int64_t)face->ny * y +
                  (int64_t)face->nz * z;
    return Q14_TO_INT(sum) - Q8_FROM_INT(face->distance);
}

static void append_vertex(unsigned vertex)
{
    if (vertex_stamp[vertex] == frame_stamp) return;
    vertex_stamp[vertex] = frame_stamp;
    frame_vertices[frame_vertex_count++] = (uint16_t)vertex;
}

static void append_edge(unsigned edge)
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
    frame_edge_count = frame_vertex_count = accepted_face_count = 0;
    for (unsigned i = 0; i < candidate_face_count; ++i) {
        const RuntimeFace *face = &runtime_faces[i];
        int32_t distance = runtime_plane_distance_q8(face, camera_x, camera_y, camera_z);
        if ((!face->side && distance >= 0) || (face->side && distance <= 0)) continue;
        if (!face_is_in_view(face, camera_x, camera_y, camera_z, sine, cosine)) continue;
#ifdef BSP_TEXTURED
        frame_faces[accepted_face_count] = (uint16_t)i;
#endif
        ++accepted_face_count;
        for (unsigned edge_index = 0; edge_index < face->edge_count; ++edge_index) {
            int edge = bsp_surfedges[face->first_edge + edge_index];
            append_edge((unsigned)(edge < 0 ? -edge : edge));
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
