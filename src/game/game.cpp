#include "game_internal.h"
#include <stdlib.h>
#include <string.h>

const float GAME_GRAVITY = 20.0f;
const float GAME_STEP_HEIGHT = 0.35f;
static const float WALK_SPEED = 6.0f;
static const float AIR_ACCEL = 18.0f;
static const float JUMP_SPEED = 7.0f;
static const float INSIDE_SKIN = 0.02f;
static const float KILL_HEIGHT = -600.0f;   // fell off the world: back to the hub

// ---------------------------------------------------------------------------
// Ships
//
// A ship is a timetable, not a teleporter: it sits at a dock for dock_seconds,
// then flies a rise / cruise / descend path to the other dock. Whatever is inside
// its volume when it moves moves with it, so boarding is walking onto the deck,
// and any number of players (later: cargo) travel by standing there.
//
// The timetable is read off universe time rather than integrated, so every server
// and every client computes the same position for the same moment.
// ---------------------------------------------------------------------------
static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

v3 game_ship_position(const Ship *ship, float t01, bool to_planet) {
    v3 from = to_planet ? ship->hub_dock : ship->planet_dock;
    v3 to = to_planet ? ship->planet_dock : ship->hub_dock;
    float top = (from.y > to.y ? from.y : to.y) + ship->cruise_height;
    // A fifth of the flight going up, a fifth coming down, level in between.
    if (t01 < 0.2f) {
        v3 p = from;
        p.y = from.y + (top - from.y) * smooth(t01 / 0.2f);
        return p;
    }
    if (t01 > 0.8f) {
        v3 p = to;
        p.y = top + (to.y - top) * smooth((t01 - 0.8f) / 0.2f);
        return p;
    }
    v3 p = v3_lerp(from, to, smooth((t01 - 0.2f) / 0.6f));
    p.y = top;
    return p;
}

static bool inside_box(v3 point, v3 center, v3 half) {
    return fabsf(point.x - center.x) <= half.x && fabsf(point.y - center.y) <= half.y
        && fabsf(point.z - center.z) <= half.z;
}

v3 game_ship_at_time(Ship *ship, double universe_time) {
    // docked at the hub, flying out, docked at the planet, flying back, repeat.
    double leg = (double)ship->dock_seconds + (double)ship->flight_seconds;
    double period = 2.0 * leg;
    double t = fmod(universe_time + (double)ship->phase, period);
    if (t < 0.0) t += period;
    bool second_leg = t >= leg;
    if (second_leg) t -= leg;
    if (t < (double)ship->dock_seconds) {
        ship->state = SHIP_DOCKED;
        ship->stop = second_leg ? STOP_PLANET : STOP_HUB;
        ship->timer = (float)t;
        return second_leg ? ship->planet_dock : ship->hub_dock;
    }
    ship->state = SHIP_FLYING;
    ship->stop = second_leg ? STOP_HUB : STOP_PLANET;
    ship->timer = (float)(t - (double)ship->dock_seconds);
    return game_ship_position(ship, ship->timer / ship->flight_seconds, !second_leg);
}

double game_time_of_tick(const game *g, uint64_t tick) {
    return g->universe_base + (double)tick / (double)GAME_TICK_HZ;
}

static void system_ships(cw_world *w, cw_phase, float) {
    game *g = game_of(w);
    double time = game_time_of_tick(g, w->tick + 1);   // where this tick ends
    cw_query ships = cw_query_begin(g->q_ships);
    cw_entity_id id;
    while (cw_query_next(&ships, &id)) {
        Ship *ship = CW_GET(w, id, g->c_ship, Ship);
        Transform *ship_t = CW_GET(w, id, g->c_transform, Transform);
        v3 before = ship_t->position, after = game_ship_at_time(ship, time);
        ship_t->position = after;
        v3 delta = v3_sub(after, before);
        if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f) continue;

        // Carry whatever is aboard, tested against the volume before the move.
        cw_query movers = cw_query_begin(g->q_players);
        cw_entity_id mover;
        while (cw_query_next(&movers, &mover)) {
            Transform *t = CW_GET(w, mover, g->c_transform, Transform);
            if (inside_box(t->position, before, ship->half)) {
                t->position = v3_add(t->position, delta);   // previous stays: the camera interpolates with the hull
            }
        }
    }
}

// Ship parts are derived state: hull transform plus a fixed offset, for both the
// current and the previous position. A craft's offsets turn with it.
static void system_attachments(cw_world *w, cw_phase, float) {
    game *g = game_of(w);
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&parts, &id)) {
        const Attachment *a = CW_GET(w, id, g->c_attachment, Attachment);
        const Transform *ship_t = CW_GET(w, a->ship, g->c_transform, Transform);
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        if (!ship_t || !cw_comp_has(w, a->ship, g->c_transform)) continue;
        if (cw_comp_has(w, a->ship, g->c_craft)) {
            const Craft *c = CW_GET(w, a->ship, g->c_craft, Craft);
            t->position = v3_add(ship_t->position, quat_rotate(c->orientation, a->offset));
            t->previous = v3_add(ship_t->previous, quat_rotate(c->previous_orientation, a->offset));
            continue;
        }
        t->position = v3_add(ship_t->position, a->offset);
        t->previous = v3_add(ship_t->previous, a->offset);
    }
}

// ---------------------------------------------------------------------------
// Players: walking is the only way anywhere
// ---------------------------------------------------------------------------
static bool overlaps(v3 feet, v3 center, v3 half) {
    return feet.x + PLAYER_RADIUS > center.x - half.x && feet.x - PLAYER_RADIUS < center.x + half.x
        && feet.y + PLAYER_HEIGHT > center.y - half.y && feet.y < center.y + half.y
        && feet.z + PLAYER_RADIUS > center.z - half.z && feet.z - PLAYER_RADIUS < center.z + half.z;
}

static float *axis_of(v3 *v, int axis) { return axis == 0 ? &v->x : axis == 1 ? &v->y : &v->z; }

// ---------------------------------------------------------------------------
// The solid grid: every solid listed under each 2D cell (x, z) it touches, sorted
// by cell key so a cell's solids are one binary search away. Rebuilt every tick,
// since ship parts move; it is a few dozen entries.
// ---------------------------------------------------------------------------
static int32_t cell_key(int cx, int cz) { return (int32_t)((cx & 0xffff) << 16) | (int32_t)(cz & 0xffff); }
static int cell_of(float v) { return (int)floorf(v / GAME_GRID_CELL); }

static int compare_grid(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : x > y;
}

static void grid_push(game *g, int32_t key, cw_entity_id id) {
    if (g->grid_count == g->grid_capacity) {
        g->grid_capacity = g->grid_capacity ? g->grid_capacity * 2 : 256;
        g->grid = (decltype(g->grid))realloc(g->grid, (size_t)g->grid_capacity * sizeof(*g->grid));
    }
    g->grid[g->grid_count].key = key;
    g->grid[g->grid_count].id = id;
    ++g->grid_count;
}

static int compare_craft_parts(const void *a, const void *b) {
    const cw_entity_id *x = (const cw_entity_id *)a, *y = (const cw_entity_id *)b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

// Whether a part belongs to a craft, whose parts turn and are never in the grid.
static bool craft_part(game *g, cw_entity_id id) {
    cw_world *w = &g->world;
    return cw_comp_has(w, id, g->c_attachment) && cw_comp_has(w, CW_GET(w, id, g->c_attachment, Attachment)->ship, g->c_craft);
}

static void system_solid_grid(cw_world *w, cw_phase, float) {
    game *g = game_of(w);
    g->grid_count = 0;
    g->craft_part_count = 0;
    cw_query solids = cw_query_begin(g->q_solids);
    cw_entity_id id;
    while (cw_query_next(&solids, &id)) {
        if (craft_part(g, id)) {
            if (g->craft_part_count == g->craft_part_capacity) {
                g->craft_part_capacity = g->craft_part_capacity ? g->craft_part_capacity * 2 : 64;
                g->craft_parts = (decltype(g->craft_parts))realloc(g->craft_parts, (size_t)g->craft_part_capacity * sizeof(*g->craft_parts));
            }
            g->craft_parts[g->craft_part_count].craft = CW_GET(w, id, g->c_attachment, Attachment)->ship;
            g->craft_parts[g->craft_part_count].part = id;
            ++g->craft_part_count;
            continue;
        }
        const Solid *solid = CW_GET(w, id, g->c_solid, Solid);
        v3 c = CW_GET(w, id, g->c_transform, Transform)->position;
        int x0 = cell_of(c.x - solid->half.x - PLAYER_RADIUS), x1 = cell_of(c.x + solid->half.x + PLAYER_RADIUS);
        int z0 = cell_of(c.z - solid->half.z - PLAYER_RADIUS), z1 = cell_of(c.z + solid->half.z + PLAYER_RADIUS);
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) grid_push(g, cell_key(x, z), id);
    }
    if (g->grid_count > 1) qsort(g->grid, (size_t)g->grid_count, sizeof(*g->grid), compare_grid);
    if (g->craft_part_count > 1)
        qsort(g->craft_parts, (size_t)g->craft_part_count, sizeof(*g->craft_parts), compare_craft_parts);
}

int game_craft_part_range(const game *g, cw_entity_id craft, int *first) {
    int lo = 0, hi = g->craft_part_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g->craft_parts[mid].craft < craft) lo = mid + 1;
        else hi = mid;
    }
    *first = lo;
    int n = 0;
    while (lo + n < g->craft_part_count && g->craft_parts[lo + n].craft == craft) ++n;
    return n;
}

// Visits every solid listed in the cell of `at` (each solid is listed in all the
// cells its padded box touches, so one cell is enough).
#define GAME_GRID_MAX_NEARBY GAME_MAX_BOXES
int game_solids_near(const game *g, v3 at, cw_entity_id *out) {
    int32_t key = cell_key(cell_of(at.x), cell_of(at.z));
    int lo = 0, hi = g->grid_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g->grid[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    int n = 0;
    for (int i = lo; i < g->grid_count && g->grid[i].key == key && n < GAME_GRID_MAX_NEARBY; ++i) out[n++] = g->grid[i].id;
    return n;
}

// Moves along one axis and stops at the first box crossed. A box the player is
// well inside (a ship landed on them) is ignored, so nobody can get stuck; "well"
// because a carried player can sit a rounding error below the deck they stand on,
// and that deck must still hold them up. The boxes are the world's solids around
// the player, or a craft's parts in its frame for one aboard it.
// Returns true when something blocked the move.
static bool move_axis(v3 *position, int axis, float amount, bool can_step, const game_box *boxes, int count) {
    v3 from = *position, to = from;
    *axis_of(&to, axis) += amount;
    bool blocked = false;
    for (int k = 0; k < count; ++k) {
        v3 center = boxes[k].center, half = boxes[k].half;
        v3 core = v3_make(half.x - INSIDE_SKIN, half.y - INSIDE_SKIN, half.z - INSIDE_SKIN);
        if (!overlaps(to, center, half) || overlaps(from, center, core)) continue;
        float top = center.y + half.y;
        if (can_step && top - from.y <= GAME_STEP_HEIGHT) {
            v3 stepped = to;
            stepped.y = top;
            bool clear = true;
            for (int m = 0; clear && m < count; ++m) clear = !overlaps(stepped, boxes[m].center, boxes[m].half);
            if (clear) {
                to.y = top;
                continue;
            }
        }
        blocked = true;
        float near_face = *axis_of(&center, axis) - (amount > 0.0f ? 1.0f : -1.0f) * *axis_of(&half, axis);
        float reach = axis == 1 ? (amount > 0.0f ? PLAYER_HEIGHT : 0.0f) : (amount > 0.0f ? PLAYER_RADIUS : -PLAYER_RADIUS);
        *axis_of(&to, axis) = near_face - reach;
    }
    *position = to;
    return blocked;
}

static bool move_axis_world(game *g, v3 *position, int axis, float amount, bool can_step) {
    v3 to = *position;
    *axis_of(&to, axis) += amount;
    cw_entity_id nearby[GAME_GRID_MAX_NEARBY];
    game_box boxes[GAME_GRID_MAX_NEARBY];
    int count = game_solids_near(g, to, nearby);
    for (int k = 0; k < count; ++k) {
        boxes[k].center = CW_GET(&g->world, nearby[k], g->c_transform, Transform)->position;
        boxes[k].half = CW_GET(&g->world, nearby[k], g->c_solid, Solid)->half;
    }
    return move_axis(position, axis, amount, can_step, boxes, count);
}

// The velocity a player's input asks for, in the frame they walk in: walking on
// the ground, steering a little in the air, jumping, falling.
static void steer(Player *p, const PlayerInput *in, float dt) {
    float length = sqrtf(in->forward * in->forward + in->strafe * in->strafe);
    float norm = length > 1.0f ? 1.0f / length : 1.0f;
    float sin_yaw = sinf(in->yaw), cos_yaw = cosf(in->yaw);
    float wish_x = (-sin_yaw * in->forward + cos_yaw * in->strafe) * norm * WALK_SPEED;
    float wish_z = (-cos_yaw * in->forward - sin_yaw * in->strafe) * norm * WALK_SPEED;
    if (p->grounded) {
        p->velocity.x = wish_x;
        p->velocity.z = wish_z;
        if (in->jump) {
            p->velocity.y = JUMP_SPEED;
            p->grounded = false;
        }
    } else {
        float step = AIR_ACCEL * dt;
        float dx = wish_x - p->velocity.x, dz = wish_z - p->velocity.z;
        float gap = sqrtf(dx * dx + dz * dz);
        if (gap > step) { dx *= step / gap; dz *= step / gap; }
        p->velocity.x += dx;
        p->velocity.z += dz;
    }
    p->velocity.y -= GAME_GRAVITY * dt;
    if (p->swing_cooldown > 0.0f) p->swing_cooldown -= dt;   // the swing itself is the server's call
}

static bool in_cabin(const Craft *c, v3 local) {
    return inside_box(local, c->cabin_center, c->cabin_half);
}

// Whether a part of the craft is underfoot, or close enough under the feet to be
// stepped down to: a player going down its stairs stays aboard.
static bool supported(v3 local, const game_box *boxes, int count) {
    v3 probe = local;
    probe.y -= GAME_STEP_HEIGHT + 0.05f;
    for (int k = 0; k < count; ++k) {
        const game_box *b = &boxes[k];
        float top = b->center.y + b->half.y;
        if (top > local.y + 0.01f || top < probe.y) continue;
        if (fabsf(local.x - b->center.x) < b->half.x + PLAYER_RADIUS && fabsf(local.z - b->center.z) < b->half.z + PLAYER_RADIUS)
            return true;
    }
    return false;
}

static void board(game *g, cw_entity_id player, cw_entity_id craft, Transform *t, Player *p) {
    const Craft *c = CW_GET(&g->world, craft, g->c_craft, Craft);
    Aboard a = {};
    a.craft = craft;
    a.position = a.previous = game_craft_from_world(g, craft, t->position);
    p->velocity = quat_unrotate(c->orientation, v3_sub(p->velocity, game_craft_point_velocity(g, craft, t->position)));
    cw_comp_set(&g->world, player, g->c_aboard, &a, sizeof(a));
}

static void leave(game *g, cw_entity_id player, cw_entity_id craft, Transform *t, Player *p) {
    const Craft *c = CW_GET(&g->world, craft, g->c_craft, Craft);
    const Aboard *a = CW_GET(&g->world, player, g->c_aboard, Aboard);
    t->position = game_craft_to_world(g, craft, a->position);
    p->velocity = v3_add(quat_rotate(c->orientation, p->velocity), game_craft_point_velocity(g, craft, t->position));
    cw_comp_remove(&g->world, player, g->c_aboard);
}

// A player in the world's frame against the crafts around them. Each craft's parts
// are boxes in its own frame, so the player is taken into that frame (as an upright
// box there: exact for a craft on the level, near enough for one that is not) and
// stepped up onto, or pushed out of, whatever they overlap. Returns the craft they
// ended up standing on, if any.
static cw_entity_id collide_crafts(game *g, Transform *t, Player *p, bool can_step) {
    cw_world *w = &g->world;
    cw_entity_id standing = CW_INVALID_ENTITY;
    cw_query crafts = cw_query_begin(g->q_crafts);
    cw_entity_id craft;
    while (cw_query_next(&crafts, &craft)) {
        const Craft *c = CW_GET(w, craft, g->c_craft, Craft);
        v3 local = game_craft_from_world(g, craft, t->position);
        if (v3_length(local) > c->radius + PLAYER_HEIGHT + 1.0f) continue;
        game_box boxes[GAME_MAX_BOXES];
        int count = game_craft_boxes(g, craft, boxes, GAME_MAX_BOXES);
        v3 velocity = quat_unrotate(c->orientation, p->velocity);
        bool moved = false;
        for (int k = 0; k < count; ++k) {
            const game_box *b = &boxes[k];
            if (!overlaps(local, b->center, b->half)) continue;
            float top = b->center.y + b->half.y;
            if (top - local.y <= GAME_STEP_HEIGHT && (can_step || velocity.y <= 0.0f)) {
                v3 stepped = local;
                stepped.y = top;
                bool clear = true;
                for (int m = 0; clear && m < count; ++m) clear = m == k || !overlaps(stepped, boxes[m].center, boxes[m].half);
                if (clear) {
                    local = stepped;
                    if (velocity.y < 0.0f) velocity.y = 0.0f;
                    standing = craft;
                    moved = true;
                    continue;
                }
            }
            // Out along the shallowest way: each of the six faces, how far it is.
            float out[6] = {
                local.x + PLAYER_RADIUS - (b->center.x - b->half.x), (b->center.x + b->half.x) - (local.x - PLAYER_RADIUS),
                local.y + PLAYER_HEIGHT - (b->center.y - b->half.y), top - local.y,
                local.z + PLAYER_RADIUS - (b->center.z - b->half.z), (b->center.z + b->half.z) - (local.z - PLAYER_RADIUS)};
            int best = 0;
            for (int f = 1; f < 6; ++f)
                if (out[f] < out[best]) best = f;
            float sign = (best & 1) ? 1.0f : -1.0f;
            *axis_of(&local, best / 2) += sign * out[best];
            float *v = axis_of(&velocity, best / 2);
            if (*v * sign < 0.0f) *v = 0.0f;
            if (best == 3) standing = craft;
            moved = true;
        }
        if (moved) {
            t->position = game_craft_to_world(g, craft, local);
            p->velocity = quat_rotate(c->orientation, velocity);
        }
    }
    if (standing != CW_INVALID_ENTITY) p->grounded = true;
    return standing;
}

// The helm is free when nobody is at it, or whoever was has gone.
static bool helm_free(game *g, cw_entity_id craft, const Craft *c) {
    return c->pilot == CW_INVALID_ENTITY || game_aboard(g, c->pilot) != craft;
}

// A player aboard a craft: in its frame, on its parts, under its gravity, which
// pulls towards its deck whichever way the craft is turned. At the helm they stand
// still and their input flies the craft instead (craft.cpp).
static void walk_aboard(game *g, cw_entity_id id, cw_entity_id craft, Transform *t, Player *p, const PlayerInput *in,
                        float dt) {
    cw_world *w = &g->world;
    Craft *c = CW_GET(w, craft, g->c_craft, Craft);
    Aboard *a = CW_GET(w, id, g->c_aboard, Aboard);
    if (in->use) {
        if (c->pilot == id) {
            c->pilot = CW_INVALID_ENTITY;
        } else if (helm_free(g, craft, c) && v3_length(v3_sub(a->position, c->helm)) <= CRAFT_HELM_REACH
                   && game_can_pilot(g, id, craft)) {
            c->pilot = id;
        }
    }
    if (c->pilot == id) {
        a->position = c->helm;
        p->velocity = v3_make(0, 0, 0);
        p->grounded = true;
        if (p->swing_cooldown > 0.0f) p->swing_cooldown -= dt;
        t->position = game_craft_to_world(g, craft, a->position);
        return;
    }
    game_box boxes[GAME_MAX_BOXES];
    int count = game_craft_boxes(g, craft, boxes, GAME_MAX_BOXES);
    steer(p, in, dt);
    bool was_grounded = p->grounded;
    if (move_axis(&a->position, 0, p->velocity.x * dt, was_grounded, boxes, count)) p->velocity.x = 0.0f;
    if (move_axis(&a->position, 2, p->velocity.z * dt, was_grounded, boxes, count)) p->velocity.z = 0.0f;
    float falling = p->velocity.y;
    bool hit = move_axis(&a->position, 1, falling * dt, false, boxes, count);
    p->grounded = hit && falling <= 0.0f;
    if (hit) p->velocity.y = 0.0f;
    // Off the deck and over nothing of the craft's: back in the world's frame.
    if (!in_cabin(c, a->position) && !p->grounded && !supported(a->position, boxes, count)) {
        leave(g, id, craft, t, p);
        return;
    }
    t->position = game_craft_to_world(g, craft, a->position);
}

static void system_players(cw_world *w, cw_phase, float dt) {
    game *g = game_of(w);
    cw_query players = cw_query_begin(g->q_players);
    cw_entity_id id;
    while (cw_query_next(&players, &id)) {
        Player *p = CW_GET(w, id, g->c_player, Player);
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        const PlayerInput *in = CW_GET(w, id, g->c_input, PlayerInput);
        p->yaw = in->yaw;
        p->pitch = in->pitch;

        cw_entity_id craft = game_aboard(g, id);
        if (craft != CW_INVALID_ENTITY) {
            walk_aboard(g, id, craft, t, p, in, dt);
            continue;
        }
        if (cw_comp_has(w, id, g->c_aboard)) cw_comp_remove(w, id, g->c_aboard);   // its craft is gone

        steer(p, in, dt);
        bool was_grounded = p->grounded;
        if (move_axis_world(g, &t->position, 0, p->velocity.x * dt, was_grounded)) p->velocity.x = 0.0f;
        if (move_axis_world(g, &t->position, 2, p->velocity.z * dt, was_grounded)) p->velocity.z = 0.0f;
        float falling = p->velocity.y;
        bool hit = move_axis_world(g, &t->position, 1, falling * dt, false);
        p->grounded = hit && falling <= 0.0f;
        if (hit) p->velocity.y = 0.0f;

        // The land of a planet holds a player up like any floor would.
        float ground = game_ground_y(g, t->position);
        if (t->position.y < ground) {
            t->position.y = ground;
            if (falling <= 0.0f) {
                p->grounded = true;
                p->velocity.y = 0.0f;
            }
        }

        // Crafts, in their own frames; standing on one, or stepping into its cabin, is boarding it.
        if (cw_query_count(g->q_crafts)) {
            cw_entity_id standing = collide_crafts(g, t, p, was_grounded);
            cw_query crafts = cw_query_begin(g->q_crafts);
            cw_entity_id candidate;
            while (standing == CW_INVALID_ENTITY && cw_query_next(&crafts, &candidate))
                if (in_cabin(CW_GET(w, candidate, g->c_craft, Craft), game_craft_from_world(g, candidate, t->position)))
                    standing = candidate;
            if (standing != CW_INVALID_ENTITY) {
                board(g, id, standing, t, p);
                continue;
            }
        }

        if (t->position.y < KILL_HEIGHT) {
            t->position = t->previous = g->spawn;
            p->velocity = v3_make(0, 0, 0);
        }
    }
}

static void system_remember_positions(cw_world *w, cw_phase, float) {
    game *g = game_of(w);
    cw_query all = cw_query_iter(w, cw_mask(&g->c_transform, 1));
    cw_entity_id id;
    while (cw_query_next(&all, &id)) {
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        t->previous = t->position;
    }
    cw_query crafts = cw_query_begin(g->q_crafts);
    while (cw_query_next(&crafts, &id)) {
        Craft *c = CW_GET(w, id, g->c_craft, Craft);
        c->previous_orientation = c->orientation;
    }
    cw_query aboard = cw_query_iter(w, cw_mask(&g->c_aboard, 1));
    while (cw_query_next(&aboard, &id)) {
        Aboard *a = CW_GET(w, id, g->c_aboard, Aboard);
        a->previous = a->position;
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void game_init(game *g, cw_sim_role role) {
    memset(g, 0, sizeof(*g));
    g->role = role;
    cw_world *w = &g->world;
    cw_world_init(w, g);
    cw_sim_init(&g->sim, role);

    cw_component_id base = cw_comp_block(w, 14);
    g->c_transform = cw_cid(base, 0);
    g->c_solid = cw_cid(base, 1);
    g->c_attachment = cw_cid(base, 2);
    g->c_visual = cw_cid(base, 3);
    g->c_input = cw_cid(base, 4);
    g->c_player = cw_cid(base, 5);
    g->c_ship = cw_cid(base, 6);
    g->c_planet = cw_cid(base, 7);
    g->c_equipment = cw_cid(base, 8);
    g->c_loot = cw_cid(base, 9);
    g->c_owned = cw_cid(base, 10);
    g->c_craft = cw_cid(base, 11);
    g->c_part = cw_cid(base, 12);
    g->c_aboard = cw_cid(base, 13);
    CW_REGISTER(w, g->c_transform, Transform);
    CW_REGISTER(w, g->c_solid, Solid);
    CW_REGISTER(w, g->c_attachment, Attachment);
    CW_REGISTER(w, g->c_visual, Visual);
    CW_REGISTER(w, g->c_input, PlayerInput);
    CW_REGISTER(w, g->c_player, Player);
    CW_REGISTER(w, g->c_ship, Ship);
    CW_REGISTER(w, g->c_planet, Planet);
    CW_REGISTER(w, g->c_equipment, Equipment);
    CW_REGISTER(w, g->c_loot, Loot);
    CW_REGISTER(w, g->c_owned, Owned);
    CW_REGISTER(w, g->c_craft, Craft);
    CW_REGISTER(w, g->c_part, Part);
    CW_REGISTER(w, g->c_aboard, Aboard);
    cw_comp_set_transient(w, g->c_input, true);

    cw_component_id solids[] = {g->c_transform, g->c_solid};
    cw_component_id players[] = {g->c_transform, g->c_player, g->c_input};
    cw_component_id ships[] = {g->c_transform, g->c_ship};
    cw_component_id parts[] = {g->c_transform, g->c_attachment};
    cw_component_id planets[] = {g->c_transform, g->c_planet};
    g->q_solids = cw_query_create(w, cw_filter_all(cw_mask(solids, 2)));
    g->q_players = cw_query_create(w, cw_filter_all(cw_mask(players, 3)));
    g->q_ships = cw_query_create(w, cw_filter_all(cw_mask(ships, 2)));
    g->q_attachments = cw_query_create(w, cw_filter_all(cw_mask(parts, 2)));
    g->q_planets = cw_query_create(w, cw_filter_all(cw_mask(planets, 2)));
    cw_component_id all_players[] = {g->c_transform, g->c_player};
    g->q_all_players = cw_query_create(w, cw_filter_all(cw_mask(all_players, 2)));
    cw_component_id loot[] = {g->c_transform, g->c_loot};
    g->q_loot = cw_query_create(w, cw_filter_all(cw_mask(loot, 2)));
    cw_component_id crafts[] = {g->c_transform, g->c_craft};
    g->q_crafts = cw_query_create(w, cw_filter_all(cw_mask(crafts, 2)));

    // Every movement system is shared: a client runs them on its own player and on
    // the ships, which is prediction. Only what a server adds is host-only.
    cw_system_desc d = {};
    d.name = "remember_positions";
    d.phases = CW_PHASE(CW_EARLY_UPDATE);
    d.fn = system_remember_positions;
    d.authority = CW_SHARED;
    d.order = 0;
    cw_sim_register(&g->sim, d);
    d.name = "ships";
    d.phases = CW_PHASE(CW_UPDATE);
    d.fn = system_ships;
    d.order = 10;
    cw_sim_register(&g->sim, d);
    d.name = "attachments";
    d.fn = system_attachments;
    d.order = 15;
    cw_sim_register(&g->sim, d);
    d.name = "solid_grid";
    d.fn = system_solid_grid;
    d.order = 17;
    cw_sim_register(&g->sim, d);
    d.name = "crafts";
    d.fn = game_system_crafts;
    d.order = 18;                // after the grid it collides with, before the players aboard
    cw_sim_register(&g->sim, d);
    d.name = "players";
    d.fn = system_players;
    d.order = 20;
    cw_sim_register(&g->sim, d);
    // Only the host hands out crafts and tows them: a client would be making
    // entities the server never heard of.
    d.name = "yard";
    d.fn = game_system_yard;
    d.order = 25;
    d.authority = CW_HOST_ONLY;
    cw_sim_register(&g->sim, d);
}

void game_destroy(game *g) {
    free(g->grid);
    free(g->craft_parts);
    cw_sim_destroy(&g->sim);
    cw_world_destroy(&g->world);   // releases the cached queries with it
}

void game_tick(game *g) { cw_sim_tick(&g->sim, &g->world, 1.0f / (float)GAME_TICK_HZ); }

void game_place_ships(game *g) {
    cw_world *w = &g->world;
    double time = game_time_of_tick(g, w->tick);
    cw_query ships = cw_query_begin(g->q_ships);
    cw_entity_id id;
    while (cw_query_next(&ships, &id)) {
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        t->position = t->previous = game_ship_at_time(CW_GET(w, id, g->c_ship, Ship), time);
    }
    system_attachments(w, CW_UPDATE, 0.0f);
}

void game_replay_tick(game *g) {
    cw_sim_replay(&g->sim, &g->world, 1.0f / (float)GAME_TICK_HZ);   // shared CW_UPDATE systems only
    ++g->world.tick;
}

v3 game_player_color(cw_entity_id id) {
    // A hue from the id, at a saturation and brightness that reads well against stone.
    uint32_t h = (uint32_t)id * 2654435761u;
    float hue = (float)(h >> 8) / 16777216.0f * 6.0f;
    int sector = (int)hue;
    float f = hue - (float)sector, lo = 0.25f, hi = 0.9f;
    float up = lo + (hi - lo) * f, down = hi - (hi - lo) * f;
    switch (sector % 6) {
    case 0: return v3_make(hi, up, lo);
    case 1: return v3_make(down, hi, lo);
    case 2: return v3_make(lo, hi, up);
    case 3: return v3_make(lo, down, hi);
    case 4: return v3_make(up, lo, hi);
    default: return v3_make(hi, lo, down);
    }
}

cw_entity_id game_spawn_player(game *g, v3 position, bool with_input) {
    cw_world *w = &g->world;
    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {position, position};
    Player p = {};
    PlayerInput in = {};
    Visual visual = {MESH_CAPSULE, v3_make(1, 1, 1), game_player_color(id)};
    Equipment equipment = {};
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_player, &p, sizeof(p));
    if (with_input) cw_comp_set(w, id, g->c_input, &in, sizeof(in));
    cw_comp_set(w, id, g->c_visual, &visual, sizeof(visual));
    cw_comp_set(w, id, g->c_equipment, &equipment, sizeof(equipment));
    return id;
}

cw_entity_id game_spawn_loot(game *g, v3 position, const item *it) {
    cw_world *w = &g->world;
    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {position, position};
    Loot loot = {*it};
    float rgb[3];
    item_rarity_color(it->rarity, rgb);
    Visual visual = {MESH_CUBE, v3_make(0.25f, 0.25f, 0.25f), v3_make(rgb[0], rgb[1], rgb[2])};
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_loot, &loot, sizeof(loot));
    cw_comp_set(w, id, g->c_visual, &visual, sizeof(visual));
    return id;
}

void game_set_owner(game *g, cw_entity_id id, uint64_t account) {
    Owned owned = {account};
    cw_comp_set(&g->world, id, g->c_owned, &owned, sizeof(owned));
}
uint64_t game_owner(game *g, cw_entity_id id) {
    return cw_comp_has(&g->world, id, g->c_owned) ? CW_GET(&g->world, id, g->c_owned, Owned)->account : 0;
}

cw_entity_id game_loot_at(game *g, v3 feet) {
    cw_world *w = &g->world;
    cw_query loot = cw_query_begin(g->q_loot);
    cw_entity_id id;
    while (cw_query_next(&loot, &id)) {
        v3 at = CW_GET(w, id, g->c_transform, Transform)->position;
        float dx = at.x - feet.x, dz = at.z - feet.z;
        if (dx * dx + dz * dz <= LOOT_REACH * LOOT_REACH && fabsf(at.y - feet.y) < 1.5f) return id;
    }
    return CW_INVALID_ENTITY;
}

void game_despawn_player(game *g, cw_entity_id id) { cw_world_mark_destroy(&g->world, id); }

cw_entity_id game_ship_at(game *g, v3 point) {
    cw_world *w = &g->world;
    cw_query ships = cw_query_begin(g->q_ships);
    cw_entity_id id;
    while (cw_query_next(&ships, &id)) {
        const Ship *ship = CW_GET(w, id, g->c_ship, Ship);
        if (inside_box(point, CW_GET(w, id, g->c_transform, Transform)->position, ship->half)) return id;
    }
    return CW_INVALID_ENTITY;
}

cw_entity_id game_find_ship(game *g, const char *name) {
    cw_world *w = &g->world;
    cw_query ships = cw_query_begin(g->q_ships);
    cw_entity_id id;
    while (cw_query_next(&ships, &id))
        if (strcmp(CW_GET(w, id, g->c_ship, Ship)->name, name) == 0) return id;
    return CW_INVALID_ENTITY;
}

// The land under a point: the highest of the planets' surfaces there. Planets the
// point is nowhere near cost a distance test.
float game_ground_y(game *g, v3 position) {
    cw_world *w = &g->world;
    float best = -1e30f;
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (cw_query_next(&planets, &id)) {
        const Planet *planet = CW_GET(w, id, g->c_planet, Planet);
        if (planet->land.radius <= 0.0f) continue;
        v3 centre = CW_GET(w, id, g->c_transform, Transform)->position;
        float reach = planet->land.radius + planet->land.relief;
        v3 away = v3_sub(position, centre);
        if (v3_dot(away, away) > (reach + 200.0f) * (reach + 200.0f)) continue;
        float y = game_planet_surface_y(&planet->land, centre, position);
        if (y > best) best = y;
    }
    return best;
}

int game_place_at(const game *g, v3 point) {
    // Doors and exits stand inside zone places, so they are looked for first; a
    // landmark or a yard terminal is a name for a spot, not a way anywhere.
    for (int i = 0; i < g->place_count; ++i)
        if ((g->places[i].kind == PLACE_ENTRANCE || g->places[i].kind == PLACE_EXIT)
            && inside_box(point, g->places[i].center, g->places[i].half)) return i;
    for (int i = 0; i < g->place_count; ++i)
        if (g->places[i].kind == PLACE_ZONE && inside_box(point, g->places[i].center, g->places[i].half)) return i;
    return -1;
}

int game_find_place(const game *g, const char *name) {
    for (int i = 0; i < g->place_count; ++i)
        if (strcmp(g->places[i].name, name) == 0) return i;
    return -1;
}

void game_declare_place(game *g, const char *name, int kind, v3 center, v3 half, v3 spawn, const char *destination) {
    if (g->place_count == GAME_MAX_PLACES) return;
    game_place *place = &g->places[g->place_count++];
    memset(place, 0, sizeof(*place));
    strncpy(place->name, name, GAME_NAME_CAPACITY - 1);
    place->kind = kind;
    place->center = center;
    place->half = half;
    place->spawn = spawn;
    if (destination) strncpy(place->destination, destination, GAME_NAME_CAPACITY - 1);
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Closest distance between segments p1-q1 and p2-q2 (Ericson, Real-Time Collision
// Detection, 5.1.9).
static float segment_distance(v3 p1, v3 q1, v3 p2, v3 q2) {
    const float eps = 1e-6f;
    v3 d1 = v3_sub(q1, p1), d2 = v3_sub(q2, p2), r = v3_sub(p1, p2);
    float a = v3_dot(d1, d1), e = v3_dot(d2, d2), f = v3_dot(d2, r);
    float s, t;
    if (a <= eps && e <= eps) return v3_length(r);
    if (a <= eps) {
        s = 0.0f;
        t = clamp01(f / e);
    } else {
        float c = v3_dot(d1, r);
        if (e <= eps) {
            t = 0.0f;
            s = clamp01(-c / a);
        } else {
            float b = v3_dot(d1, d2), denominator = a * e - b * b;
            s = denominator != 0.0f ? clamp01((b * f - c * e) / denominator) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = clamp01(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clamp01((b - c) / a);
            }
        }
    }
    return v3_length(v3_sub(v3_add(p1, v3_scale(d1, s)), v3_add(p2, v3_scale(d2, t))));
}

bool game_swing_hits(v3 eye, v3 direction, v3 feet) {
    v3 reach = v3_add(eye, v3_scale(direction, PLAYER_REACH));
    v3 bottom = v3_add(feet, v3_make(0, PLAYER_RADIUS, 0)), top = v3_add(feet, v3_make(0, PLAYER_HEIGHT - PLAYER_RADIUS, 0));
    return segment_distance(eye, reach, bottom, top) <= PLAYER_RADIUS + 0.05f;
}
