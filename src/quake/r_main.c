/* Input, frame scheduling, profiling, page expansion, and the renderer entry. */
#define CAMERA_INPUT 1
static __attribute__((unused)) uint32_t last_substeps;
/* Invariant check: the player origin must never be inside solid. */
static uint32_t solid_frames;
static uint32_t total_substeps;
static int32_t player_contents_now;
static uint16_t read_keys(void) { return (uint16_t)(~REG_KEYINPUT) & 0x03ff; }

#ifdef CAMERA_INPUT
/* Physics runs at a fixed 64 Hz regardless of how long a frame took, so the
 * player moves and falls at the same rate whether the view is cheap or
 * expensive. The renderer's own cycle count gives the elapsed time: one
 * substep per 262144 cycles is 64 Hz on a 16.78 MHz clock. */
static void update_player(Player *player, uint16_t keys, uint16_t pressed,
                          uint32_t frame_cycles)
{
    /* Real elapsed time, from a free-running timer read at the same point
     * every frame, so it includes the VBlank wait. The renderer's own cycle
     * count misses that wait entirely and ran the clock about a third slow.
     * The leftover ticks are carried rather than truncated away. */
    static uint16_t last_tick;
    static uint32_t tick_remainder;
    uint16_t now = REG_TM3D;
    tick_remainder += (uint16_t)(now - last_tick);   /* 16-bit wrap is fine */
    last_tick = now;
    unsigned substeps = tick_remainder / TIMER_TICKS_PER_STEP;
    tick_remainder -= substeps * TIMER_TICKS_PER_STEP;
    if (substeps > 8) {                  /* never spiral after a slow frame */
        substeps = 8;
        tick_remainder = 0;
    }
    (void)frame_cycles;
    last_substeps = substeps;
    total_substeps += substeps;

    /* Turning is on the same clock as everything else, so the view sweeps at
     * one rate whether a frame took 4 substeps or 8. */
    int turn = (keys & KEY_RIGHT ? 1 : 0) - (keys & KEY_LEFT ? 1 : 0);
    player->yaw_q8 += turn * TURN_RATE_Q8 * (int)substeps;

    int forward = (keys & KEY_UP ? 1 : 0) - (keys & KEY_DOWN ? 1 : 0);
    int strafe = (keys & KEY_R ? 1 : 0) - (keys & KEY_L ? 1 : 0);
    uint8_t yaw = player_yaw(player);
    int sine = sine_q14[yaw];
    int cosine = sine_q14[(uint8_t)(yaw + 64)];
    int32_t wish_x = forward * cosine - strafe * sine;
    int32_t wish_y = forward * sine + strafe * cosine;

    for (unsigned step = 0; step < substeps; ++step)
        player_step(player, wish_x, wish_y, (pressed & KEY_A) != 0);
}
#endif

static void wait_for_vblank(void)
{
    while (REG_VCOUNT >= 160) {}
    while (REG_VCOUNT < 160) {}
}

int main(void)
{
    /* The spawn entity's origin is the player origin; the eye sits above it. */
    Player player;
    player.position.x = Q8_FROM_INT(BSP_SPAWN_X);
    player.position.y = Q8_FROM_INT(BSP_SPAWN_Y);
    player.position.z = Q8_FROM_INT(BSP_SPAWN_Z);
    player.velocity.x = player.velocity.y = player.velocity.z = 0;
    /* A fixed pose to benchmark from. The scripted walk cannot be used to
     * compare two builds: they run at different speeds, so at any given
     * wall-clock second their cameras are somewhere different and the frames
     * are not comparable. Turning on the spot is deterministic, and one
     * quarter turn changes a corridor view into an oblique one -- which is
     * exactly the axis along which per-view optimisations differ. */
    player.yaw_q8 = Q8_FROM_INT(BSP_SPAWN_YAW) + BSP_YAW_OFFSET_Q8;
    player.on_ground = 0;
    int32_t camera_x = player.position.x;
    int32_t camera_y = player.position.y;
    int32_t camera_z = player.position.z + EYE_HEIGHT_Q8;
    uint8_t yaw = player_yaw(&player);
    unsigned visible_page = 0;
    uint16_t previous_keys = 0;
    for (unsigned i = 0; i < BSP_VERTEX_COUNT; ++i) runtime_vertices[i] = bsp_vertices[i];
    for (unsigned i = 0; i < BSP_EDGE_COUNT; ++i) runtime_edges[i] = bsp_edges[i];
    REG_WAITCNT = WAITCNT_FAST_ROM;
    REG_TM3CNT = 0;
    REG_TM3D = 0;
    REG_TM3CNT = TIMER_ENABLE | TIMER_DIV1024;
    REG_MEMCTRL = MEMCTRL_FAST_EWRAM;
    REG_DISPCNT = 0;
    BG_PALETTE[0] = 0;
#ifdef BSP_TEXTURED
#ifndef BSP_TEXTURED_SOLID
    for (unsigned texture = 0;
         texture < sizeof(bsp_textures) / sizeof(bsp_textures[0]); ++texture)
        texture_cache_offsets[texture] = 0xffff;
#ifndef BSP_TEXTURED_NO_LIGHT
    for (unsigned i = 0; i < sizeof(bsp_shade_table) / 4; ++i)
        ((uint32_t *)shade_table)[i] = ((const uint32_t *)bsp_shade_table)[i];
#endif
#endif
    for (unsigned color = 0; color < 256; ++color) BG_PALETTE[color] = bsp_palette[color];
#else
    BG_PALETTE[1] = 0x7fff; BG_PALETTE[2] = 0x03ff;
#endif
    REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2;

    for (;;) {
#ifdef BSP_AUTO_WALK
        /* Deterministic input rather than a deterministic camera: this drives
         * the same physics the player does, so a run exercises collision,
         * gravity and step climbing and is still reproducible frame for
         * frame. Walk forward, then turn, repeatedly. */
        {
            static uint32_t walk_frame;
            uint32_t step = walk_frame++;
            /* Walk, then turn a little, so walls are met at a range of
             * angles rather than always head-on. */
            uint16_t scripted = (step % 160 < 144) ? KEY_UP : KEY_RIGHT;
            update_player(&player, scripted, 0, bsp_profile.total_cycles);
            camera_x = player.position.x;
            camera_y = player.position.y;
            camera_z = player.position.z + EYE_HEIGHT_Q8;
            yaw = player_yaw(&player);
        }
#endif
        uint16_t keys = read_keys(), pressed = keys & (uint16_t)~previous_keys;
        previous_keys = keys;
#ifndef BSP_AUTO_WALK
        if (pressed & KEY_START) {
            player.position.x = Q8_FROM_INT(BSP_SPAWN_X);
            player.position.y = Q8_FROM_INT(BSP_SPAWN_Y);
            player.position.z = Q8_FROM_INT(BSP_SPAWN_Z);
            player.velocity.x = player.velocity.y = player.velocity.z = 0;
            player.yaw_q8 = Q8_FROM_INT(BSP_SPAWN_YAW) + BSP_YAW_OFFSET_Q8;
            cached_camera_leaf = INVALID_LEAF;
        } else {
            update_player(&player, keys, pressed, bsp_profile.total_cycles);
        }
        camera_x = player.position.x;
        camera_y = player.position.y;
        camera_z = player.position.z + EYE_HEIGHT_Q8;
        yaw = player_yaw(&player);
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
        span_flat_v_count = texel_flat_v_count = 0;
        span_flat_u_count = texel_flat_u_count = 0;
        for (unsigned i = 0; i < 6; ++i) texel_cross_bucket[i] = 0;
        texture_rom_fallbacks = near_clipped_faces = 0;
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
            texture_rom_fallbacks, texture_cache_used, near_clipped_faces,
#else
            0, 0, 0,
#endif
#ifdef CAMERA_INPUT
            player.position.x, player.position.y, player.position.z,
            player.velocity.z, player.on_ground, last_substeps,
            player_contents_now, solid_frames, steps_climbed,
            total_substeps, player.yaw_q8,
            approximate_length(player.velocity.x, player.velocity.y),
            wedged_steps, blocked_steps,
#else
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
#endif
            span_flat_v_count, texel_flat_v_count,
            span_flat_u_count, texel_flat_u_count,
            texel_cross_bucket[0], texel_cross_bucket[1], texel_cross_bucket[2],
            texel_cross_bucket[3], texel_cross_bucket[4], texel_cross_bucket[5]
        };

#ifdef CAMERA_INPUT
        player_contents_now = hull_point_contents(BSP_PLAYER_HULL_HEAD,
                                                  player.position);
        if (player_contents_now == CONTENTS_SOLID) ++solid_frames;
#endif
        wait_for_vblank();
        visible_page = draw_page;
        REG_DISPCNT = DISPLAY_MODE_4 | DISPLAY_BG2 |
                      (visible_page ? DISPLAY_PAGE_SELECT : 0);
    }
}
