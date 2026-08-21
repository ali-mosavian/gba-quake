/* Player movement: clip-hull tracing, sliding, gravity and step climbing.
 *
 * Follows Quake's SV_RecursiveHullCheck / SV_FlyMove / SV_WalkMove. The map's
 * hull 1 clipnodes were already expanded by qbsp for a 32x32x56 box, so the
 * player is traced through them as a point and no box sweeping is needed here.
 *
 * Everything is Q8 world units. Positions match the camera's existing Q8
 * coordinates; velocities are Q8 units per second and integrate at a fixed
 * 64 Hz substep, which makes the per-step position delta a shift rather than
 * a divide and keeps physics independent of the frame rate. */

enum {
    CONTENTS_EMPTY = -1,
    CONTENTS_SOLID = -2,
    /* Quake's DIST_EPSILON, 1/32 of a unit. Keeps the trace endpoint just
     * clear of the surface so the next trace does not start solid. */
    MOVE_EPSILON_Q8 = 8,
    PHYSICS_HZ = 64,
    PHYSICS_SHIFT = 6,                       /* log2(PHYSICS_HZ) */
    GRAVITY_STEP_Q8 = (800 * 256) / PHYSICS_HZ,
    STEP_HEIGHT_Q8 = Q8_FROM_INT(18),        /* Quake's STEPSIZE */
    MAX_WALK_SPEED_Q8 = Q8_FROM_INT(200),
    ACCELERATE_STEP_Q8 = (900 * 256) / PHYSICS_HZ,
    STOP_SPEED_Q8 = Q8_FROM_INT(20),
    FRICTION_STEP = (4 * 256) / PHYSICS_HZ,  /* Q8 fraction shed per step */
    JUMP_SPEED_Q8 = Q8_FROM_INT(270),
    /* 140 degrees a second, Quake's default yawspeed, as Q8 of a
     * 256-unit turn per 64 Hz substep. */
    TURN_RATE_Q8 = 400,
    /* Timer 3 runs at 16.78 MHz / 1024 = 16384 Hz, so a 64 Hz substep is
     * exactly 256 ticks and the remainder can be carried in the low bits. */
    TIMER_TICKS_PER_STEP = 16384 / PHYSICS_HZ,
    /* cos of the steepest slope still counted as floor, Q14 */
    FLOOR_NORMAL_Q14 = (int)(0.7 * 16384),
    TRACE_ONE = 1 << 16,                     /* Q16 fraction of a whole move */
};

typedef struct { int32_t x, y, z; } Vector;

typedef struct {
    int32_t fraction;        /* Q16 of the attempted move that completed */
    Vector end;
    int16_t nx, ny, nz;      /* Q14 normal of the surface stopped against */
    uint8_t all_solid, start_solid, hit;
} Trace;

static void entity_clip_trace(Vector start, Vector end, Trace *trace);

/* Signed distance from a point to a clip plane, in Q8 units.
 *
 * Uses the Q8 plane distance rather than the renderer's whole-unit one: half
 * a unit is invisible on screen but would let the player sink into floors. */
static int32_t clip_plane_distance(unsigned plane_index, Vector point)
{
    const MapPlane *plane = &bsp_planes[plane_index];
    int64_t sum = (int64_t)plane->nx * point.x + (int64_t)plane->ny * point.y +
                  (int64_t)plane->nz * point.z;
    return (int32_t)(sum >> 14) - bsp_plane_distance_q8[plane_index];
}

static int hull_point_contents(int node, Vector point)
{
    while (node >= 0) {
        const MapClipNode *entry = &bsp_clipnodes[node];
        node = entry->children[
            clip_plane_distance((unsigned)entry->plane, point) >= 0 ? 0 : 1];
    }
    return node;
}

static Vector vector_lerp(Vector a, Vector b, int32_t fraction_q16)
{
    Vector result = {
        a.x + multiply_q16(b.x - a.x, fraction_q16),
        a.y + multiply_q16(b.y - a.y, fraction_q16),
        a.z + multiply_q16(b.z - a.z, fraction_q16)
    };
    return result;
}

/* Quake's SV_RecursiveHullCheck. Returns 0 once the trace has been stopped. */
static int recursive_hull_check(int node, int32_t start_frac, int32_t end_frac,
                                Vector p1, Vector p2, Trace *trace)
{
    if (node < 0) {
        if (node != CONTENTS_SOLID) trace->all_solid = 0;
        else if (!trace->hit) trace->start_solid = 1;
        return 1;
    }

    const MapClipNode *entry = &bsp_clipnodes[node];
    unsigned plane_index = (unsigned)entry->plane;
    int32_t t1 = clip_plane_distance(plane_index, p1);
    int32_t t2 = clip_plane_distance(plane_index, p2);

    if (t1 >= 0 && t2 >= 0)
        return recursive_hull_check(entry->children[0], start_frac, end_frac,
                                    p1, p2, trace);
    if (t1 < 0 && t2 < 0)
        return recursive_hull_check(entry->children[1], start_frac, end_frac,
                                    p1, p2, trace);

    /* Crosses the plane: split the segment at the crossing, backed off by the
     * epsilon so the near piece stops just short of the surface. */
    int near_side = t1 < 0 ? 1 : 0;
    int32_t offset = t1 < 0 ? MOVE_EPSILON_Q8 : -MOVE_EPSILON_Q8;
    int32_t fraction = signed_ratio_q16_lut(t1 + offset, t1 - t2,
                                            clipping_reciprocal_q24);
    if (fraction < 0) fraction = 0;
    if (fraction > TRACE_ONE) fraction = TRACE_ONE;

    int32_t mid_frac = start_frac + multiply_q16(end_frac - start_frac, fraction);
    Vector mid = vector_lerp(p1, p2, fraction);

    if (!recursive_hull_check(entry->children[near_side], start_frac, mid_frac,
                              p1, mid, trace))
        return 0;

    if (hull_point_contents(entry->children[near_side ^ 1], mid) != CONTENTS_SOLID)
        return recursive_hull_check(entry->children[near_side ^ 1], mid_frac,
                                    end_frac, mid, p2, trace);

    if (trace->all_solid) return 0;          /* never left solid */

    if (trace->fraction > mid_frac) {
        const MapPlane *plane = &bsp_planes[plane_index];
        trace->fraction = mid_frac;
        trace->end = mid;
        trace->hit = 1;
        /* Report the normal facing back along the move. */
        if (near_side) {
            trace->nx = (int16_t)-plane->nx;
            trace->ny = (int16_t)-plane->ny;
            trace->nz = (int16_t)-plane->nz;
        } else {
            trace->nx = plane->nx;
            trace->ny = plane->ny;
            trace->nz = plane->nz;
        }
    }
    return 0;
}

static Trace trace_player(Vector start, Vector end)
{
    Trace trace;
    trace.fraction = TRACE_ONE;
    trace.end = end;
    trace.nx = trace.ny = trace.nz = 0;
    trace.all_solid = 1;
    trace.start_solid = 0;
    trace.hit = 0;
    recursive_hull_check(BSP_PLAYER_HULL_HEAD, 0, TRACE_ONE, start, end, &trace);
    /* Moving brushes clip the same trace through their own hulls. Declared
     * here and defined in r_entity.c, which the unity build includes after
     * this file. */
    entity_clip_trace(start, end, &trace);
    if (trace.fraction == TRACE_ONE) trace.end = end;
    return trace;
}

/* --- movement ------------------------------------------------------------ */

enum { STOP_EPSILON_Q8 = 26 };   /* 0.1 units */

static int32_t dot_q14_q8(int16_t nx, int16_t ny, int16_t nz, Vector v)
{
    return (int32_t)(((int64_t)nx * v.x + (int64_t)ny * v.y +
                      (int64_t)nz * v.z) >> 14);
}

/* Remove the component of `in` that heads into the surface. */
static Vector clip_velocity(Vector in, int16_t nx, int16_t ny, int16_t nz)
{
    int32_t backoff = dot_q14_q8(nx, ny, nz, in);
    Vector out = {
        in.x - (int32_t)(((int64_t)nx * backoff) >> 14),
        in.y - (int32_t)(((int64_t)ny * backoff) >> 14),
        in.z - (int32_t)(((int64_t)nz * backoff) >> 14)
    };
    if (out.x > -STOP_EPSILON_Q8 && out.x < STOP_EPSILON_Q8) out.x = 0;
    if (out.y > -STOP_EPSILON_Q8 && out.y < STOP_EPSILON_Q8) out.y = 0;
    if (out.z > -STOP_EPSILON_Q8 && out.z < STOP_EPSILON_Q8) out.z = 0;
    return out;
}

/* One physics substep of movement, sliding along whatever it hits.
 *
 * Quake's SV_FlyMove. Up to four surfaces per step: the first is slid along,
 * a second is treated as a crease and the motion projected onto the two
 * planes' intersection, which is what stops a player sticking in corners. */
static Vector slide_move(Vector position, Vector *velocity)
{
    Vector primal = *velocity;
    int32_t time_left = TRACE_ONE;
    int16_t plane_nx[2], plane_ny[2], plane_nz[2];
    unsigned planes = 0;

    for (unsigned bump = 0; bump < 4; ++bump) {
        if (!velocity->x && !velocity->y && !velocity->z) break;
        Vector end = {
            position.x + multiply_q16(velocity->x >> PHYSICS_SHIFT, time_left),
            position.y + multiply_q16(velocity->y >> PHYSICS_SHIFT, time_left),
            position.z + multiply_q16(velocity->z >> PHYSICS_SHIFT, time_left)
        };
        Trace trace = trace_player(position, end);

        if (trace.all_solid) {          /* wedged: give up this step */
            COUNT(wedged_steps, 1);
            velocity->z = 0;
            return position;
        }
        if (trace.fraction > 0) position = trace.end;
        if (trace.fraction >= TRACE_ONE) break;
        COUNT(blocked_steps, 1);

        time_left -= multiply_q16(time_left, trace.fraction);

        if (planes < 2) {
            plane_nx[planes] = trace.nx;
            plane_ny[planes] = trace.ny;
            plane_nz[planes] = trace.nz;
            ++planes;
        }

        if (planes == 1) {
            *velocity = clip_velocity(primal, plane_nx[0], plane_ny[0], plane_nz[0]);
        } else {
            /* Slide along the crease where the two surfaces meet. */
            int32_t cx = (int32_t)(((int64_t)plane_ny[0] * plane_nz[1] -
                                    (int64_t)plane_nz[0] * plane_ny[1]) >> 14);
            int32_t cy = (int32_t)(((int64_t)plane_nz[0] * plane_nx[1] -
                                    (int64_t)plane_nx[0] * plane_nz[1]) >> 14);
            int32_t cz = (int32_t)(((int64_t)plane_nx[0] * plane_ny[1] -
                                    (int64_t)plane_ny[0] * plane_nx[1]) >> 14);
            int32_t along = dot_q14_q8((int16_t)cx, (int16_t)cy, (int16_t)cz,
                                       *velocity);
            velocity->x = (int32_t)(((int64_t)cx * along) >> 14);
            velocity->y = (int32_t)(((int64_t)cy * along) >> 14);
            velocity->z = (int32_t)(((int64_t)cz * along) >> 14);
            /* Turned back on itself: stop rather than reverse. */
            if ((int64_t)velocity->x * primal.x + (int64_t)velocity->y * primal.y +
                (int64_t)velocity->z * primal.z <= 0) {
                velocity->x = velocity->y = velocity->z = 0;
                break;
            }
        }
    }
    return position;
}

static int on_floor(const Trace *trace)
{
    return trace->hit && trace->nz > FLOOR_NORMAL_Q14;
}

static int32_t horizontal_reach(Vector from, Vector to)
{
    int32_t dx = (to.x - from.x) >> 8, dy = (to.y - from.y) >> 8;
    return dx * dx + dy * dy;
}

/* Quake's SV_WalkMove: take the plain move and a stepped-up one, keep
 * whichever covered more ground, and only accept the step if it lands on
 * something walkable. */
static Vector walk_move(Vector position, Vector *velocity)
{
    Vector plain_velocity = *velocity;
    Vector plain = slide_move(position, &plain_velocity);

    Vector raised = position;
    raised.z += STEP_HEIGHT_Q8;
    Trace up = trace_player(position, raised);
    if (!up.start_solid) {
        Vector stepped_velocity = *velocity;
        Vector stepped = slide_move(up.end, &stepped_velocity);
        Vector lowered = stepped;
        lowered.z -= STEP_HEIGHT_Q8;
        Trace down = trace_player(stepped, lowered);
        if (on_floor(&down) &&
            horizontal_reach(position, down.end) >
            horizontal_reach(position, plain)) {
            if (down.end.z > position.z + Q8_FROM_INT(1)) ++steps_climbed;
            /* Keep the horizontal result of the step, but not its vertical
             * velocity: the player walked up, it did not launch. */
            stepped_velocity.z = plain_velocity.z;
            *velocity = stepped_velocity;
            return down.end;
        }
    }
    *velocity = plain_velocity;
    return plain;
}

/* Approximate magnitude, |a| + |b|/2 on the octagon. Within about 5% of the
 * true length, which is ample for clamping a walk speed and costs no square
 * root. */
static int32_t approximate_length(int32_t x, int32_t y)
{
    if (x < 0) x = -x;
    if (y < 0) y = -y;
    return x > y ? x + (y >> 1) : y + (x >> 1);
}

typedef struct {
    Vector position;
    Vector velocity;
    /* Q8 so the turn rate can be tied to the fixed physics substep instead of
     * the frame rate; the renderer only ever needs the whole-unit angle. */
    int32_t yaw_q8;
    uint8_t on_ground;
} Player;

static uint8_t player_yaw(const Player *player)
{
    return (uint8_t)(player->yaw_q8 >> 8);
}

/* One 64 Hz substep: friction, acceleration, gravity, then the move. */
static __attribute__((unused)) void player_step(Player *player, int32_t wish_x, int32_t wish_y, int jump)
{
    Vector below = player->position;
    below.z -= Q8_FROM_INT(1);
    Trace ground = trace_player(player->position, below);
    player->on_ground = (uint8_t)on_floor(&ground);

    if (player->on_ground) {
        /* Exponential friction, v *= (1 - k). Quake ramps this against a stop
         * speed instead; this form matches its 4/second decay closely and
         * needs neither a square root nor a divide. */
        player->velocity.x -= (player->velocity.x * FRICTION_STEP) >> 8;
        player->velocity.y -= (player->velocity.y * FRICTION_STEP) >> 8;
        if (player->velocity.z < 0) player->velocity.z = 0;
        if (jump) {
            player->velocity.z = JUMP_SPEED_Q8;
            player->on_ground = 0;
        }
    } else {
        player->velocity.z -= GRAVITY_STEP_Q8;
    }

    player->velocity.x += (int32_t)(((int64_t)wish_x * ACCELERATE_STEP_Q8) >> 14);
    player->velocity.y += (int32_t)(((int64_t)wish_y * ACCELERATE_STEP_Q8) >> 14);

    int32_t speed = approximate_length(player->velocity.x, player->velocity.y);
    if (speed > MAX_WALK_SPEED_Q8) {
        int32_t scale = signed_ratio_q16_lut(MAX_WALK_SPEED_Q8, speed,
                                             clipping_reciprocal_q24);
        player->velocity.x = multiply_q16(player->velocity.x, scale);
        player->velocity.y = multiply_q16(player->velocity.y, scale);
    }

    player->position = walk_move(player->position, &player->velocity);
}
