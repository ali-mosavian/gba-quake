/* Low-level software line drawing and wireframe edge submission. */
static HOT void draw_line(uint8_t *buffer, ScreenPoint start, ScreenPoint end, uint8_t color)
{
    int x = start.x, y = start.y;
    int dx = absolute(end.x - x), dy = absolute(end.y - y);
    int sx = x < end.x ? 1 : -1, sy = y < end.y ? 1 : -1;
    if (dy == 0) {
        if (x > end.x) { int swap = x; x = end.x; end.x = swap; }
        uint8_t *pixel = buffer + y * 120 + x;
        for (; x <= end.x; ++x) *pixel++ = color;
    } else if (dx == 0) {
        if (y > end.y) { int swap = y; y = end.y; end.y = swap; }
        uint8_t *pixel = buffer + y * 120 + x;
        for (; y <= end.y; ++y, pixel += 120) *pixel = color;
    } else if (dx >= dy) {
        int error = dx >> 1;
        for (;;) {
            buffer[y * 120 + x] = color;
            if (x == end.x) break;
            x += sx; error -= dy;
            if (error < 0) { y += sy; error += dx; }
        }
    } else {
        int error = dy >> 1;
        for (;;) {
            buffer[y * 120 + x] = color;
            if (y == end.y) break;
            y += sy; error -= dx;
            if (error < 0) { x += sx; error += dy; }
        }
    }
}

static HOT __attribute__((unused)) void render_frame_edges(void)
{
    drawn_edge_count = 0;
    trivial_accept_count = trivial_reject_count = 0;
    near_clip_count = screen_clip_count = 0;
    for (unsigned i = 0; i < frame_edge_count; ++i) {
        ScreenPoint first, second;
        unsigned edge = frame_edges[i];
        if (!clip_edge(edge, &first, &second)) continue;
        draw_line(logical_framebuffer, first, second, edge & 1 ? 1 : 2);
        ++drawn_edge_count;
    }
}
