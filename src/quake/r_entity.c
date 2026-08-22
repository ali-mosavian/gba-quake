/* Brush entities: the map's own moving parts.
 *
 * Each func_door / func_button / func_door_secret owns a BSP submodel -- a
 * face range and a clip hull -- and translates along a fixed direction
 * between its built (closed) position and an open position the extractor
 * derived the way Quake's game code would. Everything heavy was settled at
 * bake time: the faces carry lightmaps and texture coordinates like any
 * world face, and both are anchored to the brush, so a moved door is the
 * same face data drawn at an offset. What lives here is the little that is
 * genuinely runtime: a state machine per entity, the trigger wiring, the
 * collision hook, and the teleporter.
 */

enum {
    ENTITY_CLOSED = 0, ENTITY_OPENING, ENTITY_OPEN, ENTITY_CLOSING,
    ENTITY_KIND_DOOR = 0, ENTITY_KIND_BUTTON, ENTITY_KIND_SECRET,
    ENTITY_KIND_TELEPORT, ENTITY_KIND_WALL,
    /* How far outside a brush's box the player's presence counts as a
     * touch. Quake spawns explicit trigger fields around doors; a fixed
     * apron approximates them well enough for these six movers. */
    ENTITY_TOUCH_APRON = 48,
};

typedef struct {
    uint8_t state;
    /* Position along the move, 0 at built, distance<<8 at open, Q8 units. */
    int32_t travel_q8;
    int32_t wait_ticks;      /* at 16384 Hz, counting down while open */
    uint8_t fired;           /* a button fires its target once per press */
} EntityState;

static EntityState entity_states[MAPS_MAX_ENTITIES];

static int entity_is_solid(const MapEntity *entity)
{
    return entity->kind <= ENTITY_KIND_SECRET ||
           entity->kind == ENTITY_KIND_WALL;
}

/* Current world offset of one axis, Q8. */
static int32_t entity_offset_q8(unsigned index, unsigned axis)
{
    return (bsp_entities[index].direction_q8[axis] *
            entity_states[index].travel_q8) >> 8;
}

static void entity_begin(unsigned index, int opening)
{
    entity_states[index].state = opening ? ENTITY_OPENING : ENTITY_CLOSING;
}

static void entity_fire(uint16_t target)
{
    if (!target) return;
    for (unsigned i = 0; i < BSP_ENTITY_COUNT; ++i) {
        const MapEntity *entity = &bsp_entities[i];
        if (entity->targetname != target) continue;
        /* A START_OPEN door rests at its open end and works in reverse. */
        int resting_open = entity->flags & 1;
        if (entity_states[i].state == (resting_open ? ENTITY_OPEN : ENTITY_CLOSED))
            entity_begin(i, !resting_open);
    }
}

/* Against the mover's CURRENT box, not its built one: a plat rests a full
 * travel below where it was built, and testing the built box meant the
 * player standing on the lowered plat was nowhere near it. */
static int entity_player_near(unsigned index, const Vector *position)
{
    const MapEntity *entity = &bsp_entities[index];
    for (unsigned axis = 0; axis < 3; ++axis) {
        int32_t offset = entity_offset_q8(index, axis);
        int32_t value = (&position->x)[axis];
        int32_t low = Q8_FROM_INT(entity->mins[axis] - ENTITY_TOUCH_APRON) + offset;
        int32_t high = Q8_FROM_INT(entity->maxs[axis] + ENTITY_TOUCH_APRON) + offset;
        if (value < low || value > high) return 0;
    }
    return 1;
}

/* Advance every mover and handle touches. `position` may be 0 in builds
 * without a player. Returns nonzero if the player must be teleported, with
 * the destination written through the pointers. */
static int entity_update(uint32_t ticks, const Vector *position,
                         Vector *teleport_to, int32_t *teleport_yaw_q8)
{
    int teleported = 0;
    for (unsigned i = 0; i < BSP_ENTITY_COUNT; ++i) {
        const MapEntity *entity = &bsp_entities[i];
        EntityState *state = &entity_states[i];
        if (entity->kind == ENTITY_KIND_TELEPORT) {
            if (position && entity_player_near(i, position) &&
                teleport_to) {
                teleport_to->x = Q8_FROM_INT(BSP_TELEPORT_X);
                teleport_to->y = Q8_FROM_INT(BSP_TELEPORT_Y);
                teleport_to->z = Q8_FROM_INT(BSP_TELEPORT_Z);
                *teleport_yaw_q8 = Q8_FROM_INT(BSP_TELEPORT_YAW);
                teleported = 1;
            }
            continue;
        }
        if (!entity_is_solid(entity)) continue;

        /* Touch triggers: buttons and the secret door respond to the player
         * directly; doors with a targetname wait to be fired. */
        int resting = state->state == ENTITY_CLOSED ||
                      state->state == ENTITY_OPEN;
        if (position && resting &&
            (entity->kind == ENTITY_KIND_BUTTON ||
             entity->kind == ENTITY_KIND_SECRET || !entity->targetname) &&
            entity->kind != ENTITY_KIND_WALL &&
            entity_player_near(i, position)) {
            int resting_open = (entity->flags & 1) ? state->state == ENTITY_OPEN
                                                   : state->state == ENTITY_CLOSED;
            if (resting_open) {
                entity_begin(i, !((entity->flags & 1)));
                state->fired = 0;
            }
        }

        int32_t full = (int32_t)entity->distance << 8;
        int32_t step = (int32_t)((entity->speed * ticks) >> 6);
        switch (state->state) {
        case ENTITY_OPENING:
            state->travel_q8 += step;
            if (state->travel_q8 >= full) {
                state->travel_q8 = full;
                state->state = ENTITY_OPEN;
                state->wait_ticks = (int32_t)entity->wait_64ths << 8;
                if (entity->kind == ENTITY_KIND_BUTTON && !state->fired) {
                    state->fired = 1;
                    entity_fire(entity->target);
                }
            }
            break;
        case ENTITY_CLOSING:
            state->travel_q8 -= step;
            if (state->travel_q8 <= 0) {
                state->travel_q8 = 0;
                state->state = ENTITY_CLOSED;
                /* A START_OPEN door's "open" rest state is travel 0; give it
                 * its wait before it returns. */
                if (entity->flags & 1) {
                    state->wait_ticks = (int32_t)entity->wait_64ths << 8;
                    state->state = ENTITY_CLOSED;
                }
            }
            break;
        case ENTITY_OPEN:
        case ENTITY_CLOSED:
            if (state->wait_ticks > 0) {
                state->wait_ticks -= (int32_t)ticks;
                if (state->wait_ticks <= 0) {
                    /* Return journey, in whichever direction rest lies. */
                    int at_full = state->travel_q8 > 0;
                    entity_begin(i, !at_full);
                }
            }
            break;
        }
    }
    return teleported;
}

/* Initial state: START_OPEN movers rest at their far end. */
static void entity_init(void)
{
    for (unsigned i = 0; i < BSP_ENTITY_COUNT; ++i) {
        if (bsp_entities[i].flags & 1) {
            entity_states[i].travel_q8 = (int32_t)bsp_entities[i].distance << 8;
            entity_states[i].state = ENTITY_OPEN;
        }
    }
}

/* Collision: the player's trace runs through each solid mover's own clip
 * hull, translated to the mover's current position by shifting the segment
 * the other way. Nearest hit wins; the world trace has already run. */
static void entity_clip_trace(Vector start, Vector end, Trace *trace)
{
    for (unsigned i = 0; i < BSP_ENTITY_COUNT; ++i) {
        const MapEntity *entity = &bsp_entities[i];
        if (!entity_is_solid(entity)) continue;
        Vector offset = {entity_offset_q8(i, 0), entity_offset_q8(i, 1),
                         entity_offset_q8(i, 2)};
        /* Reject on the segment's box against the mover's box before any
         * hull walk. Without this, every physics trace recursed through all
         * six movers' clip trees -- their planes are infinite, so even a
         * trace across the map splits at each one -- and collision cost
         * more than drawing the movers did. The 40-unit margin covers the
         * hull's own expansion by the player box. */
        int rejected = 0;
        for (unsigned axis = 0; axis < 3 && !rejected; ++axis) {
            int32_t a = (&start.x)[axis], b = (&end.x)[axis];
            int32_t low = a < b ? a : b, high = a < b ? b : a;
            int32_t off = (&offset.x)[axis];
            if (high < Q8_FROM_INT(entity->mins[axis] - 40) + off ||
                low > Q8_FROM_INT(entity->maxs[axis] + 40) + off)
                rejected = 1;
        }
        if (rejected) continue;
        Vector local_start = {start.x - offset.x, start.y - offset.y,
                              start.z - offset.z};
        Vector local_end = {end.x - offset.x, end.y - offset.y,
                            end.z - offset.z};
        Trace local;
        local.fraction = TRACE_ONE;
        local.end = local_end;
        local.nx = local.ny = local.nz = 0;
        local.all_solid = 1;
        local.start_solid = 0;
        local.hit = 0;
        recursive_hull_check(entity->hull_head, 0, TRACE_ONE,
                             local_start, local_end, &local);
        if (local.hit && local.fraction < trace->fraction) {
            *trace = local;
            trace->end.x += offset.x;
            trace->end.y += offset.y;
            trace->end.z += offset.z;
        }
    }
}
