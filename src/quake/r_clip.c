/* Projected-edge outcodes and near/screen clipping. */
static unsigned outcode(ScreenPoint point)
{
    return (point.x < 0 ? 1u : 0u) | (point.x >= 120 ? 2u : 0u) |
           (point.y < 0 ? 4u : 0u) | (point.y >= 80 ? 8u : 0u);
}

static int interpolate(int start, int end, int fraction)
{
    return start + multiply_q16(end - start, fraction);
}

static HOT int clip_to_screen(ScreenPoint *a, ScreenPoint *b)
{
    for (;;) {
        unsigned ca = outcode(*a), cb = outcode(*b);
        if (!(ca | cb)) return 1;
        if (ca & cb) return 0;
        unsigned code = ca ? ca : cb;
        int x, y, fraction;
        if (code & 4) {
            y = 0; fraction = signed_ratio_q16_lut(y - a->y, b->y - a->y, clipping_reciprocal_q24);
            x = interpolate(a->x, b->x, fraction);
        } else if (code & 8) {
            y = 79; fraction = signed_ratio_q16_lut(y - a->y, b->y - a->y, clipping_reciprocal_q24);
            x = interpolate(a->x, b->x, fraction);
        } else if (code & 2) {
            x = 119; fraction = signed_ratio_q16_lut(x - a->x, b->x - a->x, clipping_reciprocal_q24);
            y = interpolate(a->y, b->y, fraction);
        } else {
            x = 0; fraction = signed_ratio_q16_lut(x - a->x, b->x - a->x, clipping_reciprocal_q24);
            y = interpolate(a->y, b->y, fraction);
        }
        if (code == ca) { a->x = x; a->y = y; }
        else { b->x = x; b->y = y; }
    }
}

static HOT int clip_edge(unsigned edge_index, ScreenPoint *screen_a, ScreenPoint *screen_b)
{
    MapEdge edge = runtime_edges[edge_index];
    CameraPoint a = camera_cache[edge.first], b = camera_cache[edge.second];
    unsigned code_a = vertex_outcode[edge.first];
    unsigned code_b = vertex_outcode[edge.second];
    if ((code_a & 16) && (code_b & 16)) {
        ++trivial_reject_count;
        return 0;
    }
    if (!(code_a & 16) && !(code_b & 16)) {
        unsigned screen_a_code = code_a & 15;
        unsigned screen_b_code = code_b & 15;
        if (screen_a_code & screen_b_code) {
            ++trivial_reject_count;
            return 0;
        }
        *screen_a = screen_cache[edge.first];
        *screen_b = screen_cache[edge.second];
        if (!(screen_a_code | screen_b_code)) {
            ++trivial_accept_count;
            return 1;
        }
        ++screen_clip_count;
        return clip_to_screen(screen_a, screen_b);
    }
    ++near_clip_count;
    if (a.depth < NEAR_PLANE_Q8 || b.depth < NEAR_PLANE_Q8) {
        CameraPoint *behind = a.depth < NEAR_PLANE_Q8 ? &a : &b;
        CameraPoint *ahead = behind == &a ? &b : &a;
        int32_t fraction = signed_ratio_q16_lut(NEAR_PLANE_Q8 - behind->depth,
                                                ahead->depth - behind->depth,
                                                clipping_reciprocal_q24);
        behind->horizontal += multiply_q16(ahead->horizontal - behind->horizontal, fraction);
        behind->vertical += multiply_q16(ahead->vertical - behind->vertical, fraction);
        behind->depth = NEAR_PLANE_Q8;
    }
    if (a.depth == camera_cache[edge.first].depth) *screen_a = screen_cache[edge.first];
    else *screen_a = (ScreenPoint){60 + projection_offset_lut(a.horizontal, a.depth, projection_reciprocal_q16),
                                   40 - projection_offset_lut(a.vertical, a.depth, projection_reciprocal_q16)};
    if (b.depth == camera_cache[edge.second].depth) *screen_b = screen_cache[edge.second];
    else *screen_b = (ScreenPoint){60 + projection_offset_lut(b.horizontal, b.depth, projection_reciprocal_q16),
                                   40 - projection_offset_lut(b.vertical, b.depth, projection_reciprocal_q16)};
    if (outcode(*screen_a) | outcode(*screen_b)) ++screen_clip_count;
    return clip_to_screen(screen_a, screen_b);
}
