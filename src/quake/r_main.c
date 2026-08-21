/* Input, frame scheduling, profiling, page expansion, and the renderer entry. */
#ifndef BSP_AUTO_WALK
#define CAMERA_INPUT 1
#endif
static uint16_t read_keys(void) { return (uint16_t)(~REG_KEYINPUT) & 0x03ff; }

#ifdef CAMERA_INPUT
static void update_camera(uint16_t keys, int32_t *x, int32_t *y, uint8_t *yaw)
{
    if (keys & KEY_LEFT) *yaw -= 2;
    if (keys & KEY_RIGHT) *yaw += 2;
    int forward = (keys & KEY_UP ? 2 : 0) - (keys & KEY_DOWN ? 2 : 0);
    int strafe = (keys & KEY_A ? -2 : 0) + (keys & KEY_B ? 2 : 0);
    int sine = sine_q14[*yaw], cosine = sine_q14[(uint8_t)(*yaw + 64)];
    *x += (forward * cosine - strafe * sine) >> 6;
    *y += (forward * sine + strafe * cosine) >> 6;
}
#endif

static void wait_for_vblank(void)
{
    while (REG_VCOUNT >= 160) {}
    while (REG_VCOUNT < 160) {}
}

int main(void)
{
    int32_t camera_x = Q8_FROM_INT(BSP_SPAWN_X);
    int32_t camera_y = Q8_FROM_INT(BSP_SPAWN_Y);
    int32_t camera_z = Q8_FROM_INT(BSP_SPAWN_Z) + EYE_HEIGHT_Q8;
    uint8_t yaw = BSP_SPAWN_YAW;
    unsigned visible_page = 0;
    uint16_t previous_keys = 0;
    for (unsigned i = 0; i < BSP_VERTEX_COUNT; ++i) runtime_vertices[i] = bsp_vertices[i];
    for (unsigned i = 0; i < BSP_EDGE_COUNT; ++i) runtime_edges[i] = bsp_edges[i];
    REG_WAITCNT = WAITCNT_FAST_ROM;
    REG_MEMCTRL = MEMCTRL_FAST_EWRAM;
    REG_DISPCNT = 0;
    BG_PALETTE[0] = 0;
#ifdef BSP_TEXTURED
#ifndef BSP_TEXTURED_SOLID
    for (unsigned texture = 0;
         texture < sizeof(bsp_textures) / sizeof(bsp_textures[0]); ++texture)
        texture_cache_offsets[texture] = 0xffff;
#endif
    for (unsigned color = 0; color < 256; ++color) BG_PALETTE[color] = bsp_palette[color];
#else
    BG_PALETTE[1] = 0x7fff; BG_PALETTE[2] = 0x03ff;
#endif
    REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2;

    for (;;) {
#ifdef BSP_AUTO_WALK
        /* Deterministic camera path. Keeps motion tests and cycle
         * measurements reproducible frame for frame, which manual input
         * cannot be. */
        {
            static uint32_t walk_frame;
            uint32_t step = walk_frame++;
            camera_x = Q8_FROM_INT(BSP_SPAWN_X);
            camera_y = Q8_FROM_INT(BSP_SPAWN_Y) + Q8_FROM_INT((int32_t)(step % 96));
            yaw = (uint8_t)(BSP_SPAWN_YAW + (step / 96) * 8);
            if (step % 96 == 0) cached_camera_leaf = INVALID_LEAF;
        }
#endif
        uint16_t keys = read_keys(), pressed = keys & (uint16_t)~previous_keys;
        previous_keys = keys;
#ifndef BSP_AUTO_WALK
        if (pressed & KEY_START) {
            camera_x = Q8_FROM_INT(BSP_SPAWN_X);
            camera_y = Q8_FROM_INT(BSP_SPAWN_Y);
            yaw = BSP_SPAWN_YAW; cached_camera_leaf = INVALID_LEAF;
        } else update_camera(keys, &camera_x, &camera_y, &yaw);
#else
        (void)pressed;
#endif

        if (++frame_stamp == 0) {
            for (unsigned i = 0; i < BSP_EDGE_COUNT; ++i) edge_stamp[i] = 0;
            for (unsigned i = 0; i < BSP_VERTEX_COUNT; ++i) vertex_stamp[i] = 0;
            frame_stamp = 1;
        }

        degenerate_face_count = 0;
        drawn_face_count = drawn_row_count = drawn_span_count = 0;
        pixel_iteration_count = texel_sample_count = 0;
        span_clear_count = span_hidden_count = span_mixed_count = 0;
        texture_rom_fallbacks = 0;
        profile_timer_start();
        clear_logical_framebuffer(logical_framebuffer);
        uint32_t after_clear = profile_timer_read();
        unsigned camera_leaf = find_camera_leaf(camera_x, camera_y, camera_z);
        if (camera_leaf != cached_camera_leaf) {
            rebuild_candidate_faces(camera_leaf, camera_x, camera_y, camera_z);
            cached_camera_leaf = camera_leaf;
        }
        uint32_t after_pvs = profile_timer_read();
        build_frame_lists(camera_x, camera_y, camera_z, yaw);
        uint32_t after_cull = profile_timer_read();
        transform_project_frame_vertices(camera_x, camera_y, camera_z, yaw);
        uint32_t after_transform = profile_timer_read();
        uint32_t after_projection = profile_timer_read();
#ifdef BSP_TEXTURED
        render_textured_faces();
#else
        render_frame_edges();
#endif
        uint32_t after_clipping = profile_timer_read();
        uint32_t after_lines = after_clipping;
        unsigned draw_page = visible_page ^ 1;
        volatile uint16_t *page = (volatile uint16_t *)(draw_page ? MODE4_PAGE_1 : MODE4_PAGE_0);
        expand_logical_framebuffer(logical_framebuffer, page);
        uint32_t after_expand = profile_timer_read();

        bsp_profile = (BspProfile){
            after_clear, after_pvs - after_clear, after_cull - after_pvs,
            after_transform - after_cull, after_projection - after_transform,
            after_clipping - after_projection, after_lines - after_clipping,
            after_expand - after_lines, after_expand, candidate_face_count,
            accepted_face_count, frame_edge_count, frame_vertex_count,
            drawn_edge_count, (uint16_t)camera_leaf,
            trivial_accept_count, trivial_reject_count,
            near_clip_count, screen_clip_count, degenerate_face_count,
            drawn_face_count, drawn_row_count, drawn_span_count,
            pixel_iteration_count, texel_sample_count,
            span_clear_count, span_hidden_count, span_mixed_count,
#if defined(BSP_TEXTURED) && !defined(BSP_TEXTURED_SOLID)
            texture_rom_fallbacks, texture_cache_used
#else
            0, 0
#endif
        };

        wait_for_vblank();
        visible_page = draw_page;
        REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2 |
                      (visible_page ? DISPLAY_PAGE_SELECT : 0);
    }
}
