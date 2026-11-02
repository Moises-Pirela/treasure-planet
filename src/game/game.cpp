#include "game.h"
#include <string.h>

// World data reachable from a system. Systems get only the world, and the world's
// user pointer is how they find the component ids and queries.
static game *game_of(cw_world *w) { return (game *)w->user; }

static const float GRAVITY = 20.0f;
static const float WALK_SPEED = 6.0f;
static const float AIR_ACCEL = 18.0f;
static const float JUMP_SPEED = 7.0f;
static const float STEP_HEIGHT = 0.35f;
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
// current and the previous position.
static void system_attachments(cw_world *w, cw_phase, float) {
    game *g = game_of(w);
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&parts, &id)) {
        const Attachment *a = CW_GET(w, id, g->c_attachment, Attachment);
        const Transform *ship_t = CW_GET(w, a->ship, g->c_transform, Transform);
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        if (!ship_t) continue;
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

// Moves along one axis and stops at the first solid crossed. A solid the player is
// well inside (a ship landed on them) is ignored, so nobody can get stuck; "well"
// because a carried player can sit a rounding error below the deck they stand on,
// and that deck must still hold them up.
// Returns true when something blocked the move.
static bool move_axis(game *g, Transform *t, int axis, float amount, bool can_step) {
    cw_world *w = &g->world;
    v3 from = t->position, to = from;
    *axis_of(&to, axis) += amount;
    bool blocked = false;
    cw_query solids = cw_query_begin(g->q_solids);
    cw_entity_id id;
    while (cw_query_next(&solids, &id)) {
        const Solid *solid = CW_GET(w, id, g->c_solid, Solid);
        v3 center = CW_GET(w, id, g->c_transform, Transform)->position;
        v3 core = v3_make(solid->half.x - INSIDE_SKIN, solid->half.y - INSIDE_SKIN, solid->half.z - INSIDE_SKIN);
        if (!overlaps(to, center, solid->half) || overlaps(from, center, core)) continue;
        float top = center.y + solid->half.y;
        if (can_step && top - from.y <= STEP_HEIGHT) {
            v3 stepped = to;
            stepped.y = top;
            bool clear = true;
            cw_query others = cw_query_begin(g->q_solids);
            cw_entity_id other;
            while (clear && cw_query_next(&others, &other)) {
                const Solid *s = CW_GET(w, other, g->c_solid, Solid);
                clear = !overlaps(stepped, CW_GET(w, other, g->c_transform, Transform)->position, s->half);
            }
            if (clear) {
                to.y = top;
                continue;
            }
        }
        blocked = true;
        v3 half = solid->half;
        float near_face = *axis_of(&center, axis) - (amount > 0.0f ? 1.0f : -1.0f) * *axis_of(&half, axis);
        float reach = axis == 1 ? (amount > 0.0f ? PLAYER_HEIGHT : 0.0f) : (amount > 0.0f ? PLAYER_RADIUS : -PLAYER_RADIUS);
        *axis_of(&to, axis) = near_face - reach;
    }
    t->position = to;
    return blocked;
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
        p->velocity.y -= GRAVITY * dt;

        bool was_grounded = p->grounded;
        if (move_axis(g, t, 0, p->velocity.x * dt, was_grounded)) p->velocity.x = 0.0f;
        if (move_axis(g, t, 2, p->velocity.z * dt, was_grounded)) p->velocity.z = 0.0f;
        float falling = p->velocity.y;
        bool hit = move_axis(g, t, 1, falling * dt, false);
        p->grounded = hit && falling <= 0.0f;
        if (hit) p->velocity.y = 0.0f;

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

    cw_component_id base = cw_comp_block(w, 8);
    g->c_transform = cw_cid(base, 0);
    g->c_solid = cw_cid(base, 1);
    g->c_attachment = cw_cid(base, 2);
    g->c_visual = cw_cid(base, 3);
    g->c_input = cw_cid(base, 4);
    g->c_player = cw_cid(base, 5);
    g->c_ship = cw_cid(base, 6);
    g->c_planet = cw_cid(base, 7);
    CW_REGISTER(w, g->c_transform, Transform);
    CW_REGISTER(w, g->c_solid, Solid);
    CW_REGISTER(w, g->c_attachment, Attachment);
    CW_REGISTER(w, g->c_visual, Visual);
    CW_REGISTER(w, g->c_input, PlayerInput);
    CW_REGISTER(w, g->c_player, Player);
    CW_REGISTER(w, g->c_ship, Ship);
    CW_REGISTER(w, g->c_planet, Planet);
    cw_comp_set_transient(w, g->c_input, true);

    cw_component_id solids[] = {g->c_transform, g->c_solid};
    cw_component_id players[] = {g->c_transform, g->c_player, g->c_input};
    cw_component_id ships[] = {g->c_transform, g->c_ship};
    cw_component_id parts[] = {g->c_transform, g->c_attachment};
    g->q_solids = cw_query_create(w, cw_filter_all(cw_mask(solids, 2)));
    g->q_players = cw_query_create(w, cw_filter_all(cw_mask(players, 3)));
    g->q_ships = cw_query_create(w, cw_filter_all(cw_mask(ships, 2)));
    g->q_attachments = cw_query_create(w, cw_filter_all(cw_mask(parts, 2)));
    cw_component_id all_players[] = {g->c_transform, g->c_player};
    g->q_all_players = cw_query_create(w, cw_filter_all(cw_mask(all_players, 2)));

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
    d.name = "players";
    d.fn = system_players;
    d.order = 20;
    cw_sim_register(&g->sim, d);
}

void game_destroy(game *g) {
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
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_player, &p, sizeof(p));
    if (with_input) cw_comp_set(w, id, g->c_input, &in, sizeof(in));
    cw_comp_set(w, id, g->c_visual, &visual, sizeof(visual));
    return id;
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

int game_place_at(const game *g, v3 point) {
    for (int i = 0; i < g->place_count; ++i)
        if (inside_box(point, g->places[i].center, g->places[i].half)) return i;
    return -1;
}

int game_find_place(const game *g, const char *name) {
    for (int i = 0; i < g->place_count; ++i)
        if (strcmp(g->places[i].name, name) == 0) return i;
    return -1;
}
