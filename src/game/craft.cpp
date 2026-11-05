// Crafts: ships players own and fly. A craft is a rigid body made of parts; its
// mass, inertia, thrust and torque are the sum of what its parts are, and it moves
// by Newton's laws under gravity, its thrusters and whatever it touches. The same
// code flies it on the server and, one tick ahead, on its pilot's client, so it
// must stay a pure function of the state and the pilot's inputs. What parts a
// craft has, and so how it flies, comes from its loadout (ships.cpp).
//
//   pilot input -> flight computer -> thruster groups + gyros -> integration -> contacts
//
// Contacts are against the land of planets and the world's solids (axis-aligned
// boxes), each part of the craft an oriented box: separating axes find how deep a
// pair overlaps, the corners inside give the contact points, and a few rounds of
// sequential impulses with friction settle them. A craft that sits still on the
// ground for half a second comes to rest and costs nothing until its pilot or the
// loss of its support wakes it. Crafts do not collide with each other yet.
#include "game_internal.h"
#include <stdlib.h>
#include <string.h>

// Contacts and the solver.
static const float RESTITUTION = 0.2f;       // of the speed a hard landing bounces back with
static const float BOUNCE_SPEED = 2.0f;      // m/s of impact below which nothing bounces
static const float FRICTION = 0.7f;
static const float CONTACT_SLOP = 0.005f;    // metres of overlap left alone, so resting contacts persist
static const float CORRECTION = 0.8f;        // of the remaining overlap pushed out per tick
static const int SOLVER_ROUNDS = 8;
static const int QUIET_TICKS = 30;           // ticks nearly still on the ground before it rests
static const float QUIET_SPEED = 0.2f, QUIET_SPIN = 0.1f;
#define MAX_CONTACTS 128
static_assert(SHIP_MAX_PARTS <= GAME_MAX_BOXES, "a player aboard collides with every part of a craft");

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
static float axis_component(v3 v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }
static v3 unit_axis(int k) { return v3_make(k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f); }

static Craft *craft_get(game *g, cw_entity_id id) {
    return cw_comp_has(&g->world, id, g->c_craft) ? CW_GET(&g->world, id, g->c_craft, Craft) : NULL;
}

static float smooth_between(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

// The field is a few waves across the system, in kilometres, turning the way the
// current flows and swelling its strength; their phases move with time, each at its
// own period, so the pattern drifts. Phases are taken modulo a turn in double
// before they become floats: universe time is large, and a float of it is coarse.
v3 game_solar_current(const game *g, v3 position, double time) {
    if (!g || !g->currents) return v3_make(0, 0, 0);
    const double turn = 6.283185307179586;
    float t1 = (float)fmod(time * (turn / 300.0), turn), t2 = (float)fmod(time * (turn / 470.0), turn);
    float t3 = (float)fmod(time * (turn / 610.0), turn);
    float x = position.x * 0.001f, y = position.y * 0.001f, z = position.z * 0.001f;
    float heading = 0.9f + 1.4f * sinf(0.8f * x + 0.3f * z + t1) + 0.9f * sinf(-0.5f * x + 0.9f * z + 1.7f + t2)
                  + 0.5f * sinf(1.2f * x - 1.0f * z + 4.1f);
    float swell = 0.6f * sinf(1.1f * x + 0.6f * z + t3) + 0.4f * sinf(-0.8f * x + 1.3f * z + 2.3f + t1);
    float strength = (SOLAR_CURRENT_MIN + SOLAR_CURRENT_MAX) * 0.5f + (SOLAR_CURRENT_MAX - SOLAR_CURRENT_MIN) * 0.5f * swell;
    float rise = 0.12f * sinf(0.6f * x - 0.8f * z + 1.0f * y + t2);
    // `heading` in look_direction's sense; a little up or down, the rest across.
    float across = strength * sqrtf(1.0f - rise * rise);
    return v3_make(-sinf(heading) * across, rise * strength, -cosf(heading) * across);
}

v3 game_solar_current_at_tick(const game *g, v3 position, uint64_t tick) {
    return game_solar_current(g, position, game_time_of_tick(g, tick));
}

v3 game_gravity(game *g, v3 position) {
    cw_world *w = &g->world;
    v3 pull = v3_make(0, 0, 0);
    cw_query sources = cw_query_begin(g->q_gravity);
    cw_entity_id id;
    while (cw_query_next(&sources, &id)) {
        const Gravity *source = CW_GET(w, id, g->c_gravity, Gravity);
        v3 away = v3_sub(position, CW_GET(w, id, g->c_transform, Transform)->position);
        float distance = v3_length(away);
        if (distance >= source->reach || source->reach <= source->radius) continue;
        // Fading to nothing at the reach: a field of the world's down from the edge of
        // its radius, a planet's from halfway out to its reach (where the inverse
        // square has already taken most of it).
        float fade_from = source->kind == GRAVITY_DOWN ? source->radius : (source->radius + source->reach) * 0.5f;
        float fade = 1.0f - smooth_between(fade_from, source->reach, distance);
        if (source->kind == GRAVITY_DOWN) {
            pull.y -= source->strength * fade;
        } else if (distance > 1e-3f) {
            // Inside the surface it falls off to nothing at the centre, as a uniform ball's would.
            float ratio = distance >= source->radius ? source->radius / distance : distance / source->radius;
            float strength = source->strength * (distance >= source->radius ? ratio * ratio : ratio) * fade;
            pull = v3_sub(pull, v3_scale(away, strength / distance));
        }
    }
    return pull;
}

v3 game_craft_to_world(game *g, cw_entity_id craft, v3 local) {
    const Craft *c = craft_get(g, craft);
    if (!c) return local;
    return v3_add(CW_GET(&g->world, craft, g->c_transform, Transform)->position, quat_rotate(c->orientation, local));
}

v3 game_craft_from_world(game *g, cw_entity_id craft, v3 world) {
    const Craft *c = craft_get(g, craft);
    if (!c) return world;
    return quat_unrotate(c->orientation, v3_sub(world, CW_GET(&g->world, craft, g->c_transform, Transform)->position));
}

v3 game_craft_point_velocity(game *g, cw_entity_id craft, v3 world_point) {
    const Craft *c = craft_get(g, craft);
    if (!c) return v3_make(0, 0, 0);
    v3 com = game_craft_to_world(g, craft, c->center);
    return v3_add(c->velocity, v3_cross(quat_rotate(c->orientation, c->spin), v3_sub(world_point, com)));
}

void game_craft_pose(game *g, cw_entity_id craft, float alpha, v3 *position, quat *orientation) {
    const Craft *c = craft_get(g, craft);
    const Transform *t = CW_GET(&g->world, craft, g->c_transform, Transform);
    *position = v3_lerp(t->previous, t->position, alpha);
    *orientation = c ? quat_nlerp(c->previous_orientation, c->orientation, alpha) : quat_identity();
}

cw_entity_id game_aboard(game *g, cw_entity_id player) {
    cw_world *w = &g->world;
    if (!cw_comp_has(w, player, g->c_aboard)) return CW_INVALID_ENTITY;
    cw_entity_id craft = CW_GET(w, player, g->c_aboard, Aboard)->craft;
    return craft_get(g, craft) ? craft : CW_INVALID_ENTITY;
}

v3 game_player_frame(game *g, cw_entity_id player, cw_entity_id *frame) {
    cw_world *w = &g->world;
    cw_entity_id craft = game_aboard(g, player);
    if (craft != CW_INVALID_ENTITY) {
        *frame = craft;
        return CW_GET(w, player, g->c_aboard, Aboard)->position;
    }
    v3 at = CW_GET(w, player, g->c_transform, Transform)->position;
    *frame = game_ship_at(g, at);
    if (*frame == CW_INVALID_ENTITY) return at;
    return v3_sub(at, CW_GET(w, *frame, g->c_transform, Transform)->position);
}

v3 game_frame_to_world(game *g, cw_entity_id frame, v3 point) {
    cw_world *w = &g->world;
    if (frame == CW_INVALID_ENTITY || !cw_comp_has(w, frame, g->c_transform)) return point;
    if (craft_get(g, frame)) return game_craft_to_world(g, frame, point);
    return v3_add(CW_GET(w, frame, g->c_transform, Transform)->position, point);
}

void game_player_view(game *g, cw_entity_id player, v3 *eye, v3 *direction) {
    cw_world *w = &g->world;
    const Player *p = CW_GET(w, player, g->c_player, Player);
    cw_entity_id craft = game_aboard(g, player);
    v3 look = look_direction(p->yaw, p->pitch);
    if (craft == CW_INVALID_ENTITY) {
        *eye = v3_add(CW_GET(w, player, g->c_transform, Transform)->position, v3_make(0, PLAYER_EYE, 0));
        *direction = look;
        return;
    }
    const Aboard *a = CW_GET(w, player, g->c_aboard, Aboard);
    *eye = game_craft_to_world(g, craft, v3_add(a->position, v3_make(0, PLAYER_EYE, 0)));
    *direction = quat_rotate(craft_get(g, craft)->orientation, look);
}

// ---------------------------------------------------------------------------
// Parts
// ---------------------------------------------------------------------------
int game_craft_boxes(game *g, cw_entity_id craft, game_box *out, int capacity) {
    cw_world *w = &g->world;
    int first = 0, count = game_craft_part_range(g, craft, &first), n = 0;
    for (int i = first; i < first + count && n < capacity; ++i) {
        cw_entity_id part = g->craft_parts[i].part;
        if (!cw_comp_has(w, part, g->c_solid)) continue;
        out[n].center = CW_GET(w, part, g->c_attachment, Attachment)->offset;
        out[n].half = CW_GET(w, part, g->c_solid, Solid)->half;
        ++n;
    }
    return n;
}

void game_place_craft_parts(game *g, cw_entity_id craft) {
    cw_world *w = &g->world;
    const Craft *c = craft_get(g, craft);
    const Transform *ct = CW_GET(w, craft, g->c_transform, Transform);
    if (!c) return;
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&parts, &id)) {
        const Attachment *a = CW_GET(w, id, g->c_attachment, Attachment);
        if (a->ship != craft) continue;
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        t->position = v3_add(ct->position, quat_rotate(c->orientation, a->offset));
        t->previous = v3_add(ct->previous, quat_rotate(c->previous_orientation, a->offset));
    }
}

// A thruster pushes one way along the axis it mostly points along.
int game_part_group(const Part *part) {
    return part->kind == PART_THRUSTER ? ship_thrust_group(part->thrust) : -1;
}

void game_craft_refit(game *g, cw_entity_id craft) {
    cw_world *w = &g->world;
    Craft *c = craft_get(g, craft);
    if (!c) return;
    // Its parts as a design lists them, and the same sums a shipyard does.
    ship_part parts[SHIP_MAX_PARTS];
    int count = 0;
    cw_query all = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (count < SHIP_MAX_PARTS && cw_query_next(&all, &id)) {
        if (!cw_comp_has(w, id, g->c_part)) continue;
        const Attachment *a = CW_GET(w, id, g->c_attachment, Attachment);
        if (a->ship != craft) continue;
        const Part *part = CW_GET(w, id, g->c_part, Part);
        ship_part *p = &parts[count++];
        *p = {};
        p->kind = part->kind;
        p->offset = a->offset;
        p->half = cw_comp_has(w, id, g->c_solid) ? CW_GET(w, id, g->c_solid, Solid)->half : v3_make(0, 0, 0);
        p->mass = part->mass;
        p->thrust = part->thrust;
        p->torque = part->torque;
        p->mast = part->mast;
        p->component = part->component;
    }
    ship_body body;
    ship_body_of(parts, count, &body);
    c->mass = body.mass;
    c->center = body.center;
    c->inertia = body.inertia;
    c->radius = body.radius;
    c->draft = body.draft;
    for (int k = 0; k < CRAFT_GROUPS; ++k) {
        c->thrust[k] = body.thrust[k];
        c->thrust_torque[k] = body.thrust_torque[k];
    }
    c->gyro = body.gyro;
    c->masts = body.masts;
    for (int m = 0; m < CRAFT_MAX_MASTS; ++m) {
        craft_sail *sail = &c->sails[m];
        sail->area = body.sails[m].area;
        sail->center = body.sails[m].center;
        sail->drive = body.sails[m].drive;
        sail->sharpness = body.sails[m].sharpness;
    }
}

bool game_ship_stats(const ship_loadout *loadout, ship_stats *out) {
    ship_design *d = (ship_design *)malloc(sizeof(ship_design));
    bool ok = d && ship_design_build(loadout, GAME_GRAVITY, d);
    if (ok) *out = d->stats;
    free(d);
    return ok;
}

cw_entity_id game_spawn_craft(game *g, const ship_loadout *loadout, v3 floor, float heading, uint64_t owner, int berth) {
    cw_world *w = &g->world;
    ship_loadout standard = ship_default_loadout(HULL_SLOOP);
    if (!loadout) loadout = &standard;
    ship_design *d = (ship_design *)malloc(sizeof(ship_design));
    if (!d || !ship_design_build(loadout, GAME_GRAVITY, d)) {
        free(d);
        return CW_INVALID_ENTITY;
    }
    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {floor, floor};
    Craft c = {};
    strncpy(c.name, ship_hull(loadout->hull)->name, GAME_NAME_CAPACITY - 1);
    c.loadout = *loadout;
    c.cabin_center = d->cabin_center;
    c.cabin_half = d->cabin_half;
    c.helm = d->helm;
    c.pilot = CW_INVALID_ENTITY;
    for (int m = 0; m < CRAFT_MAX_MASTS; ++m) {
        c.sails[m].hand = CW_INVALID_ENTITY;
        c.sails[m].station = d->stations[m];
    }
    c.berth = berth;
    c.dock = CW_INVALID_ENTITY;
    c.docking = DOCK_NONE;
    c.orientation = c.previous_orientation = quat_from_yaw(heading);
    c.assist = true;
    for (int k = 0; k < CRAFT_GROUPS; ++k) c.top_speed[k] = d->stats.top_speed[k];
    c.top_rate = d->stats.top_rate;
    c.response = d->stats.response;
    c.turn_response = d->stats.turn_response;
    c.hull_drag = d->stats.hull_drag;
    Barred barred = {};
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_craft, &c, sizeof(c));
    cw_comp_set(w, id, g->c_barred, &barred, sizeof(barred));
    if (owner) game_set_owner(g, id, owner);
    for (int i = 0; i < d->part_count; ++i) {
        const ship_part *row = &d->parts[i];
        cw_entity_id part = cw_world_create_entity(w);
        Transform pt = {floor, floor};
        Solid solid = {row->half};
        Visual visual = {MESH_CUBE, row->half, row->color};
        Attachment a = {id, row->offset};
        Part p = {row->kind, row->mass, row->thrust, row->torque, row->mast, row->component, row->paint, row->base};
        cw_comp_set(w, part, g->c_transform, &pt, sizeof(pt));
        cw_comp_set(w, part, g->c_solid, &solid, sizeof(solid));
        cw_comp_set(w, part, g->c_visual, &visual, sizeof(visual));
        cw_comp_set(w, part, g->c_attachment, &a, sizeof(a));
        cw_comp_set(w, part, g->c_part, &p, sizeof(p));
    }
    free(d);
    game_craft_refit(g, id);
    // Seated on the floor, its lowest part on it, and still.
    Craft *placed = CW_GET(w, id, g->c_craft, Craft);
    Transform *pt = CW_GET(w, id, g->c_transform, Transform);
    pt->position = pt->previous = v3_add(floor, v3_make(0, placed->draft, 0));
    placed->resting = true;
    game_place_craft_parts(g, id);
    return id;
}

void game_despawn_craft(game *g, cw_entity_id craft) {
    cw_world *w = &g->world;
    if (!craft_get(g, craft)) return;
    // Everyone aboard is let go of where they stand, moving as the deck did.
    cw_component_id aboard_mask[] = {g->c_aboard, g->c_player, g->c_transform};
    cw_query aboard = cw_query_iter(w, cw_mask(aboard_mask, 3));
    cw_entity_id id;
    while (cw_query_next(&aboard, &id)) {
        Aboard *a = CW_GET(w, id, g->c_aboard, Aboard);
        if (a->craft != craft) continue;
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        Player *p = CW_GET(w, id, g->c_player, Player);
        t->position = t->previous = game_craft_to_world(g, craft, a->position);
        p->velocity = v3_add(game_craft_point_velocity(g, craft, t->position),
                             quat_rotate(CW_GET(w, craft, g->c_craft, Craft)->orientation, p->velocity));
        cw_comp_remove(w, id, g->c_aboard);
    }
    cw_query parts = cw_query_begin(g->q_attachments);
    while (cw_query_next(&parts, &id))
        if (CW_GET(w, id, g->c_attachment, Attachment)->ship == craft) cw_world_mark_destroy(w, id);
    cw_world_mark_destroy(w, craft);
}

void game_craft_to_berth(game *g, cw_entity_id craft) {
    cw_world *w = &g->world;
    Craft *c = craft_get(g, craft);
    if (!c || c->berth < 0 || c->berth >= g->berth_count) return;
    const game_berth *b = &g->berths[c->berth];
    Transform *t = CW_GET(w, craft, g->c_transform, Transform);
    t->position = t->previous = v3_add(b->floor, v3_make(0, c->draft, 0));
    c->orientation = c->previous_orientation = quat_from_yaw(b->heading);
    c->velocity = c->spin = v3_make(0, 0, 0);
    c->resting = true;
    c->quiet_ticks = 0;
    c->dock = CW_INVALID_ENTITY;
    c->docking = DOCK_NONE;
    for (int k = 0; k < CRAFT_GROUPS; ++k) c->throttle[k] = 0.0f;
    game_place_craft_parts(g, craft);
}

// ---------------------------------------------------------------------------
// Docks
// ---------------------------------------------------------------------------
cw_entity_id game_spawn_dock(game *g, const char *name, v3 floor, float heading, int size) {
    cw_world *w = &g->world;
    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {floor, floor};
    Dock d = {};
    strncpy(d.name, name, GAME_NAME_CAPACITY - 1);
    d.size = size;
    d.forward = look_direction(heading, 0.0f);
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_dock, &d, sizeof(d));
    return id;
}

static const Dock *dock_get(game *g, cw_entity_id dock) {
    cw_world *w = &g->world;
    return cw_world_is_valid(w, dock) && cw_comp_has(w, dock, g->c_dock) ? CW_GET(w, dock, g->c_dock, Dock) : NULL;
}

static bool dock_takes(const Dock *d, const Craft *c) {
    const hull_spec *h = ship_hull(c->loadout.hull);
    return d && h && d->size >= h->berth;
}

bool game_dock_free(game *g, cw_entity_id dock, cw_entity_id except) {
    cw_world *w = &g->world;
    if (!dock_get(g, dock)) return false;
    v3 floor = CW_GET(w, dock, g->c_transform, Transform)->position;
    cw_query crafts = cw_query_begin(g->q_crafts);
    cw_entity_id id;
    while (cw_query_next(&crafts, &id)) {
        if (id == except) continue;
        const Craft *c = CW_GET(w, id, g->c_craft, Craft);
        if (c->dock == dock && c->docking != DOCK_NONE) return false;
        // Sitting on it, docked or not.
        v3 off = v3_sub(CW_GET(w, id, g->c_transform, Transform)->position, floor);
        if (fabsf(off.y) < 8.0f && off.x * off.x + off.z * off.z < 36.0f) return false;
    }
    return true;
}

cw_entity_id game_dock_in_reach(game *g, cw_entity_id craft) {
    cw_world *w = &g->world;
    const Craft *c = craft_get(g, craft);
    if (!c) return CW_INVALID_ENTITY;
    v3 at = CW_GET(w, craft, g->c_transform, Transform)->position;
    // The nearest of her own size first, so a small ship leaves the large docks to
    // those that need them; failing that, the nearest larger.
    const hull_spec *h = ship_hull(c->loadout.hull);
    for (int pass = 0; pass < 2; ++pass) {
        cw_entity_id best = CW_INVALID_ENTITY;
        float nearest = CRAFT_DOCK_REACH;
        cw_query docks = cw_query_begin(g->q_docks);
        cw_entity_id id;
        while (cw_query_next(&docks, &id)) {
            const Dock *d = CW_GET(w, id, g->c_dock, Dock);
            if (pass == 0 && h && d->size != h->berth) continue;
            float distance = v3_length(v3_sub(CW_GET(w, id, g->c_transform, Transform)->position, at));
            if (distance > nearest || !dock_takes(d, c) || !game_dock_free(g, id, craft)) continue;
            nearest = distance;
            best = id;
        }
        if (best != CW_INVALID_ENTITY) return best;
    }
    return CW_INVALID_ENTITY;
}

bool game_craft_airborne(game *g, cw_entity_id craft) {
    const Craft *c = craft_get(g, craft);
    if (!c || c->resting) return false;
    float altitude = game_craft_altitude(g, craft);
    return altitude < 0.0f || altitude > 2.0f;
}

bool game_craft_start_docking(game *g, cw_entity_id craft, cw_entity_id dock) {
    cw_world *w = &g->world;
    Craft *c = craft_get(g, craft);
    const Dock *d = dock_get(g, dock);
    if (!c || !d || !dock_takes(d, c) || !game_dock_free(g, dock, craft)) return false;
    v3 at = CW_GET(w, craft, g->c_transform, Transform)->position;
    if (v3_length(v3_sub(CW_GET(w, dock, g->c_transform, Transform)->position, at)) > CRAFT_DOCK_REACH) return false;
    c->dock = dock;
    c->docking = DOCK_APPROACH;
    return true;
}

void game_craft_lay_up(game *g, cw_entity_id craft, cw_entity_id dock) {
    cw_world *w = &g->world;
    Craft *c = craft_get(g, craft);
    const Dock *d = dock_get(g, dock);
    if (!c || !d) return;
    Transform *t = CW_GET(w, craft, g->c_transform, Transform);
    t->position = t->previous = v3_add(CW_GET(w, dock, g->c_transform, Transform)->position, v3_make(0, c->draft, 0));
    c->orientation = c->previous_orientation = quat_from_yaw(atan2f(-d->forward.x, -d->forward.z));
    c->velocity = c->spin = v3_make(0, 0, 0);
    c->resting = true;
    c->quiet_ticks = 0;
    c->mode = CRAFT_LANDING;
    c->dock = dock;
    c->docking = DOCK_DOCKED;
    for (int k = 0; k < CRAFT_GROUPS; ++k) c->throttle[k] = 0.0f;
    game_place_craft_parts(g, craft);
}

cw_entity_id game_craft_of(game *g, uint64_t owner) {
    cw_query crafts = cw_query_begin(g->q_crafts);
    cw_entity_id id;
    while (cw_query_next(&crafts, &id))
        if (game_owner(g, id) == owner) return id;
    return CW_INVALID_ENTITY;
}

bool game_can_pilot(game *g, cw_entity_id player, cw_entity_id craft) {
    uint64_t owner = game_owner(g, craft);
    return owner == 0 || owner == game_owner(g, player);
}

bool game_can_work_sails(game *g, cw_entity_id player, cw_entity_id craft) { return game_may_board(g, player, craft); }

bool game_may_board(game *g, cw_entity_id player, cw_entity_id craft) {
    cw_world *w = &g->world;
    if (!cw_comp_has(w, craft, g->c_barred)) return true;
    uint64_t account = game_owner(g, player);
    if (!account) return true;
    const Barred *b = CW_GET(w, craft, g->c_barred, Barred);
    for (int i = 0; i < CRAFT_BARRED; ++i)
        if (b->account[i] == account && w->tick < b->until[i]) return false;
    return true;
}

int game_craft_crew(game *g, cw_entity_id craft, cw_entity_id *out, int capacity) {
    cw_world *w = &g->world;
    cw_component_id aboard_mask[] = {g->c_aboard, g->c_player};
    cw_query aboard = cw_query_iter(w, cw_mask(aboard_mask, 2));
    cw_entity_id id;
    int n = 0;
    while (cw_query_next(&aboard, &id)) {
        if (CW_GET(w, id, g->c_aboard, Aboard)->craft != craft) continue;
        if (n < capacity) out[n] = id;
        ++n;
    }
    return n;
}

static v3 way_down(game *g, const Craft *c, v3 at);

void game_kick(game *g, cw_entity_id craft, cw_entity_id player) {
    cw_world *w = &g->world;
    Craft *c = craft_get(g, craft);
    if (!c || !cw_world_is_valid(w, player) || !cw_comp_has(w, player, g->c_player)) return;
    // Barred first, by account, in the slot that lifts soonest if all are taken.
    uint64_t account = game_owner(g, player);
    if (account && cw_comp_has(w, craft, g->c_barred)) {
        Barred *b = CW_GET(w, craft, g->c_barred, Barred);
        int slot = 0;
        for (int i = 0; i < CRAFT_BARRED; ++i) {
            if (b->account[i] == account || b->until[i] <= w->tick) { slot = i; break; }
            if (b->until[i] < b->until[slot]) slot = i;
        }
        b->account[slot] = account;
        b->until[slot] = w->tick + (uint64_t)(CRAFT_BAR_SECONDS * GAME_TICK_HZ);
    }
    if (c->pilot == player) c->pilot = CW_INVALID_ENTITY;
    for (int m = 0; m < CRAFT_MAX_MASTS; ++m)
        if (c->sails[m].hand == player) c->sails[m].hand = CW_INVALID_ENTITY;
    if (game_aboard(g, player) != craft) return;
    // Off behind the stern, at the height of its keel: the ground at the foot of its
    // stairs, for a craft that is down. A craft down at an edge may have nothing
    // behind it: then beside it, or ahead of it, wherever there is ground to stand on.
    Transform *t = CW_GET(w, player, g->c_transform, Transform);
    Player *p = CW_GET(w, player, g->c_player, Player);
    float aside = c->cabin_half.x + 2.5f, behind = c->cabin_center.z + c->cabin_half.z + 3.5f;
    const v3 spots[] = {v3_make(0.0f, -c->draft, behind), v3_make(aside, -c->draft, c->cabin_center.z),
                        v3_make(-aside, -c->draft, c->cabin_center.z), v3_make(0.0f, -c->draft, c->cabin_center.z - c->cabin_half.z - 3.5f)};
    v3 off = game_craft_to_world(g, craft, spots[0]);
    if (c->resting) {
        v3 down = way_down(g, c, CW_GET(w, craft, g->c_transform, Transform)->position);
        for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); ++i) {
            v3 spot = game_craft_to_world(g, craft, spots[i]);
            if (game_raycast(g, v3_sub(spot, v3_scale(down, 0.5f)), down, 2.0f) < 2.0f) {
                off = spot;
                break;
            }
        }
    }
    // A few centimetres up, to come down onto the ground rather than start in it.
    off = v3_sub(off, v3_scale(way_down(g, c, off), 0.05f));
    t->position = t->previous = off;
    p->velocity = game_craft_point_velocity(g, craft, t->position);
    p->grounded = false;
    cw_comp_remove(w, player, g->c_aboard);
}

int game_kick_all(game *g, cw_entity_id craft, cw_entity_id keep) {
    cw_entity_id crew[64];
    int n = game_craft_crew(g, craft, crew, 64), kicked = 0;
    for (int i = 0; i < n && i < 64; ++i) {
        if (crew[i] == keep) continue;
        game_kick(g, craft, crew[i]);
        ++kicked;
    }
    return kicked;
}

int game_craft_charts(game *g, cw_entity_id craft, cw_entity_id *out, int capacity) {
    cw_world *w = &g->world;
    int n = 0;
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&parts, &id)) {
        if (CW_GET(w, id, g->c_attachment, Attachment)->ship != craft || !cw_comp_has(w, id, g->c_part)) continue;
        if (CW_GET(w, id, g->c_part, Part)->kind != PART_CHART) continue;
        if (n < capacity) out[n] = id;
        ++n;
    }
    return n;
}

void game_chart_fit(game *g, v3 *centre, float *extent) {
    cw_world *w = &g->world;
    // The box around the hub (the origin) and every planet, whole; its middle, and
    // the farthest reach of any of them from it.
    v3 low = v3_make(0, 0, 0), high = v3_make(0, 0, 0);
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (cw_query_next(&planets, &id)) {
        v3 at = CW_GET(w, id, g->c_transform, Transform)->position;
        float r = CW_GET(w, id, g->c_planet, Planet)->land.radius;
        low = v3_make(fminf(low.x, at.x - r), fminf(low.y, at.y - r), fminf(low.z, at.z - r));
        high = v3_make(fmaxf(high.x, at.x + r), fmaxf(high.y, at.y + r), fmaxf(high.z, at.z + r));
    }
    *centre = v3_scale(v3_add(low, high), 0.5f);
    float reach = 100.0f;
    planets = cw_query_begin(g->q_planets);
    while (cw_query_next(&planets, &id)) {
        float r = v3_length(v3_sub(CW_GET(w, id, g->c_transform, Transform)->position, *centre))
                + CW_GET(w, id, g->c_planet, Planet)->land.radius;
        if (r > reach) reach = r;
    }
    if (v3_length(*centre) > reach) reach = v3_length(*centre);
    *extent = reach * 1.1f;   // a margin, so the farthest is not at the very edge
}

int game_free_berth(game *g, int hull) {
    const hull_spec *h = ship_hull(hull);
    if (!h) return -1;
    // Berths of its own size first, so a sloop leaves the large ones to those that need them.
    for (int pass = 0; pass < 2; ++pass)
        for (int b = 0; b < g->berth_count; ++b) {
            if (pass == 0 ? g->berths[b].size != h->berth : g->berths[b].size < h->berth) continue;
            bool taken = false;
            cw_query crafts = cw_query_begin(g->q_crafts);
            cw_entity_id id;
            while (!taken && cw_query_next(&crafts, &id)) taken = CW_GET(&g->world, id, g->c_craft, Craft)->berth == b;
            if (!taken) return b;
        }
    return -1;
}

cw_entity_id game_call_craft(game *g, cw_entity_id player, const ship_loadout *order) {
    uint64_t owner = game_owner(g, player);
    ship_loadout standard = ship_default_loadout(HULL_SLOOP);
    const ship_loadout *wanted = order ? order : &g->yard_order;
    if (!ship_loadout_valid(wanted)) wanted = &standard;
    cw_entity_id craft = game_craft_of(g, owner);
    if (craft != CW_INVALID_ENTITY) {
        Craft *c = CW_GET(&g->world, craft, g->c_craft, Craft);
        cw_entity_id crew[1];
        // Rebuilt to a new order only with nobody aboard, and a berth for it.
        if (order && !ship_loadout_equal(&c->loadout, wanted) && game_craft_crew(g, craft, crew, 1) == 0) {
            int old = c->berth;
            c->berth = -1;   // its own berth is free for the new one
            int berth = game_free_berth(g, wanted->hull);
            c->berth = old;
            if (berth >= 0) {
                game_despawn_craft(g, craft);
                CW_GET(&g->world, craft, g->c_craft, Craft)->berth = -1;   // gone at the end of the tick: not holding a berth
                return game_spawn_craft(g, wanted, g->berths[berth].floor, g->berths[berth].heading, owner, berth);
            }
        }
        if (c->berth >= 0) game_craft_to_berth(g, craft);
        return craft;
    }
    int berth = game_free_berth(g, wanted->hull);
    if (berth < 0) return CW_INVALID_ENTITY;
    return game_spawn_craft(g, wanted, g->berths[berth].floor, g->berths[berth].heading, owner, berth);
}

cw_entity_id game_refit_craft(game *g, cw_entity_id player, const ship_loadout *order) {
    if (!ship_loadout_valid(order)) return game_craft_of(g, game_owner(g, player));
    cw_entity_id craft = game_craft_of(g, game_owner(g, player));
    cw_entity_id crew[1];
    if (craft != CW_INVALID_ENTITY && game_craft_crew(g, craft, crew, 1) > 0) return craft;   // not under their feet
    return game_call_craft(g, player, order);   // built, rebuilt, or brought round as she is
}

void game_set_ship_order(game *g, cw_entity_id player, const ship_loadout *loadout) {
    if (!cw_world_is_valid(&g->world, player) || !ship_loadout_valid(loadout)) return;
    ShipOrder order = {*loadout};
    cw_comp_set(&g->world, player, g->c_ship_order, &order, sizeof(order));
}

// ---------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------
struct contact {
    v3 point, normal;            // the normal points out of what the craft touches, into the craft
    float depth;
};

struct obb {
    v3 center, axis[3], half;
};

static v3 obb_corner(const obb *b, int i) {
    v3 p = b->center;
    p = v3_add(p, v3_scale(b->axis[0], (i & 1) ? b->half.x : -b->half.x));
    p = v3_add(p, v3_scale(b->axis[1], (i & 2) ? b->half.y : -b->half.y));
    return v3_add(p, v3_scale(b->axis[2], (i & 4) ? b->half.z : -b->half.z));
}

// Separating axes between a part (an oriented box) and a solid (axis-aligned, centre
// `b`, half `hb`): false when some axis separates them, else the axis of least
// overlap, pointing from the solid into the part, and the overlap along it. The
// faces' axes are preferred over the edges' unless an edge's is clearly shallower,
// so that a box resting on a floor keeps the floor's normal.
static bool box_overlap(const obb *a, v3 b, v3 hb, v3 *normal, float *depth) {
    v3 between = v3_sub(a->center, b);
    float best = 1e30f;
    v3 best_axis = v3_make(0, 1, 0);
    for (int test = 0; test < 15; ++test) {
        v3 l;
        if (test < 3) l = unit_axis(test);
        else if (test < 6) l = a->axis[test - 3];
        else {
            l = v3_cross(unit_axis((test - 6) / 3), a->axis[(test - 6) % 3]);
            float length = v3_length(l);
            if (length < 1e-4f) continue;   // parallel edges: a face axis covers it
            l = v3_scale(l, 1.0f / length);
        }
        float ra = a->half.x * fabsf(v3_dot(a->axis[0], l)) + a->half.y * fabsf(v3_dot(a->axis[1], l))
                 + a->half.z * fabsf(v3_dot(a->axis[2], l));
        float rb = hb.x * fabsf(l.x) + hb.y * fabsf(l.y) + hb.z * fabsf(l.z);
        float along = v3_dot(between, l);
        float overlap = ra + rb - fabsf(along);
        if (overlap < 0.0f) return false;
        float weighed = test < 6 ? overlap : overlap * 1.05f + 0.01f;
        if (weighed < best) {
            best = weighed;
            *depth = overlap;
            best_axis = along < 0.0f ? v3_scale(l, -1.0f) : l;
        }
    }
    *normal = best_axis;
    return true;
}

static bool inside_aabb(v3 p, v3 center, v3 half, float margin) {
    return fabsf(p.x - center.x) <= half.x + margin && fabsf(p.y - center.y) <= half.y + margin
        && fabsf(p.z - center.z) <= half.z + margin;
}

static void push_contact(contact *out, int *count, v3 point, v3 normal, float depth) {
    if (*count >= MAX_CONTACTS) return;
    out[*count].point = point;
    out[*count].normal = normal;
    out[*count].depth = depth;
    ++*count;
}

// Where a part and a solid touch: the part's corners inside the solid; failing those
// the solid's corners inside the part; failing both (edge on edge), the part's
// deepest point along the normal.
static void box_contacts(const obb *a, v3 b, v3 hb, contact *out, int *count) {
    v3 n;
    float depth = 0.0f;
    if (!box_overlap(a, b, hb, &n, &depth)) return;
    float solid_top = v3_dot(b, n) + hb.x * fabsf(n.x) + hb.y * fabsf(n.y) + hb.z * fabsf(n.z);
    int found = 0;
    for (int i = 0; i < 8; ++i) {
        v3 corner = obb_corner(a, i);
        if (!inside_aabb(corner, b, hb, 0.02f)) continue;
        float d = solid_top - v3_dot(corner, n);
        if (d <= 0.0f) continue;
        push_contact(out, count, corner, n, d < depth ? d : depth);
        ++found;
    }
    if (found) return;
    float part_bottom = v3_dot(a->center, n) - (a->half.x * fabsf(v3_dot(a->axis[0], n)) + a->half.y * fabsf(v3_dot(a->axis[1], n))
                                                + a->half.z * fabsf(v3_dot(a->axis[2], n)));
    for (int i = 0; i < 8; ++i) {
        v3 corner = v3_make(b.x + ((i & 1) ? hb.x : -hb.x), b.y + ((i & 2) ? hb.y : -hb.y), b.z + ((i & 4) ? hb.z : -hb.z));
        v3 local = v3_sub(corner, a->center);
        if (fabsf(v3_dot(local, a->axis[0])) > a->half.x + 0.02f || fabsf(v3_dot(local, a->axis[1])) > a->half.y + 0.02f
            || fabsf(v3_dot(local, a->axis[2])) > a->half.z + 0.02f) continue;
        float d = v3_dot(corner, n) - part_bottom;
        if (d <= 0.0f) continue;
        push_contact(out, count, corner, n, d < depth ? d : depth);
        ++found;
    }
    if (found) return;
    v3 deepest = a->center;
    for (int k = 0; k < 3; ++k) {
        float h = axis_component(a->half, k);
        deepest = v3_add(deepest, v3_scale(a->axis[k], v3_dot(a->axis[k], n) > 0.0f ? -h : h));
    }
    push_contact(out, count, deepest, n, depth);
}

// Where a point is inside a planet's land: how deep, measured out from its centre,
// and the land's normal there (from the heights a little way either side), so a
// craft can come down anywhere on a planet, not only on its top.
static bool planet_contact(const PlanetLand *land, v3 centre, v3 point, v3 *normal, float *depth) {
    v3 out = v3_sub(point, centre);
    float distance = v3_length(out);
    if (distance < 1e-3f) return false;
    v3 up = v3_scale(out, 1.0f / distance);
    float surface = land->radius + game_planet_height(land, up);
    if (distance >= surface) return false;
    v3 side = v3_normalize(v3_cross(up, fabsf(up.y) < 0.9f ? v3_make(0, 1, 0) : v3_make(1, 0, 0)));
    v3 along = v3_cross(up, side);
    float step = 0.5f / land->radius;
    v3 d1 = v3_normalize(v3_add(up, v3_scale(side, step))), d2 = v3_normalize(v3_add(up, v3_scale(along, step)));
    v3 p0 = v3_scale(up, surface);
    v3 p1 = v3_scale(d1, land->radius + game_planet_height(land, d1));
    v3 p2 = v3_scale(d2, land->radius + game_planet_height(land, d2));
    v3 n = v3_normalize(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)));
    if (v3_dot(n, up) < 0.0f) n = v3_scale(n, -1.0f);
    if (v3_dot(n, n) < 0.5f) n = up;
    *normal = n;
    *depth = (surface - distance) * v3_dot(n, up);
    return true;
}

// Every contact of the craft posed at `position` and `q` with the land and the solids.
static int gather_contacts(game *g, cw_entity_id craft, const Craft *c, v3 position, quat q, contact *out) {
    cw_world *w = &g->world;
    int count = 0;
    // The land, of the planets near enough for it to matter.
    cw_entity_id lands[4];
    int land_count = 0;
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (land_count < 4 && cw_query_next(&planets, &id)) {
        const Planet *planet = CW_GET(w, id, g->c_planet, Planet);
        if (planet->land.radius <= 0.0f) continue;
        float reach = planet->land.radius + planet->land.relief + c->radius + 1.0f;
        if (v3_length(v3_sub(position, CW_GET(w, id, g->c_transform, Transform)->position)) < reach) lands[land_count++] = id;
    }
    // The solids around it, from every grid cell its sphere touches, each once.
    cw_entity_id solids[GAME_MAX_BOXES];
    int solid_count = 0;
    float reach = c->radius + 0.5f;
    int x0 = (int)floorf((position.x - reach) / GAME_GRID_CELL), x1 = (int)floorf((position.x + reach) / GAME_GRID_CELL);
    int z0 = (int)floorf((position.z - reach) / GAME_GRID_CELL), z1 = (int)floorf((position.z + reach) / GAME_GRID_CELL);
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z) {
            cw_entity_id cell[GAME_MAX_BOXES];
            int n = game_solids_near(g, v3_make(((float)x + 0.5f) * GAME_GRID_CELL, 0, ((float)z + 0.5f) * GAME_GRID_CELL), cell);
            for (int k = 0; k < n && solid_count < GAME_MAX_BOXES; ++k) {
                bool seen = false;
                for (int m = 0; m < solid_count && !seen; ++m) seen = solids[m] == cell[k];
                if (seen) continue;
                v3 center = CW_GET(w, cell[k], g->c_transform, Transform)->position;
                v3 half = CW_GET(w, cell[k], g->c_solid, Solid)->half;
                if (v3_length(v3_sub(center, position)) > c->radius + v3_length(half)) continue;
                solids[solid_count++] = cell[k];
            }
        }
    if (land_count == 0 && solid_count == 0) return 0;

    int first = 0, parts = game_craft_part_range(g, craft, &first);
    for (int i = first; i < first + parts; ++i) {
        cw_entity_id part = g->craft_parts[i].part;
        if (!cw_comp_has(w, part, g->c_solid)) continue;
        obb box;
        box.center = v3_add(position, quat_rotate(q, CW_GET(w, part, g->c_attachment, Attachment)->offset));
        for (int k = 0; k < 3; ++k) box.axis[k] = quat_rotate(q, unit_axis(k));
        box.half = CW_GET(w, part, g->c_solid, Solid)->half;
        for (int l = 0; l < land_count; ++l) {
            const PlanetLand *land = &CW_GET(w, lands[l], g->c_planet, Planet)->land;
            v3 centre = CW_GET(w, lands[l], g->c_transform, Transform)->position;
            for (int k = 0; k < 8; ++k) {
                v3 corner = obb_corner(&box, k), n;
                float depth;
                if (planet_contact(land, centre, corner, &n, &depth)) push_contact(out, &count, corner, n, depth);
            }
        }
        for (int k = 0; k < solid_count; ++k)
            box_contacts(&box, CW_GET(w, solids[k], g->c_transform, Transform)->position,
                         CW_GET(w, solids[k], g->c_solid, Solid)->half, out, &count);
    }
    return count;
}

// The inverse inertia applied to a world-space vector: into the craft's axes,
// divided by the principal moments, and back.
static v3 inverse_inertia(const Craft *c, v3 v) {
    v3 local = quat_unrotate(c->orientation, v);
    local = v3_make(local.x / c->inertia.x, local.y / c->inertia.y, local.z / c->inertia.z);
    return quat_rotate(c->orientation, local);
}

// Sequential impulses: each contact in turn takes the impulse that stops its point
// sinking further (or bounces it, after a hard landing) and friction up to its share,
// a few rounds over so the contacts agree; then what still overlaps is pushed out.
// Returns the change of velocity the contacts made, m/s: how hard it hit.
static float resolve_contacts(Craft *c, Transform *t, const contact *contacts, int count) {
    if (count == 0 || c->mass <= 0.0f) return 0.0f;
    v3 velocity_before = c->velocity;
    v3 com = v3_add(t->position, quat_rotate(c->orientation, c->center));
    v3 spin = quat_rotate(c->orientation, c->spin);   // in the world's axes while solving
    float normal_impulse[MAX_CONTACTS], target[MAX_CONTACTS];
    v3 friction_impulse[MAX_CONTACTS];
    float inverse_mass = 1.0f / c->mass;
    for (int i = 0; i < count; ++i) {
        v3 r = v3_sub(contacts[i].point, com);
        float approach = v3_dot(v3_add(c->velocity, v3_cross(spin, r)), contacts[i].normal);
        target[i] = approach < -BOUNCE_SPEED ? -RESTITUTION * approach : 0.0f;
        normal_impulse[i] = 0.0f;
        friction_impulse[i] = v3_make(0, 0, 0);
    }
    for (int round = 0; round < SOLVER_ROUNDS; ++round) {
        for (int i = 0; i < count; ++i) {
            v3 n = contacts[i].normal, r = v3_sub(contacts[i].point, com);
            v3 point_velocity = v3_add(c->velocity, v3_cross(spin, r));
            float k = inverse_mass + v3_dot(n, v3_cross(inverse_inertia(c, v3_cross(r, n)), r));
            float wanted = (target[i] - v3_dot(point_velocity, n)) / k;
            float total = normal_impulse[i] + wanted > 0.0f ? normal_impulse[i] + wanted : 0.0f;
            v3 impulse = v3_scale(n, total - normal_impulse[i]);
            normal_impulse[i] = total;
            c->velocity = v3_add(c->velocity, v3_scale(impulse, inverse_mass));
            spin = v3_add(spin, inverse_inertia(c, v3_cross(r, impulse)));

            point_velocity = v3_add(c->velocity, v3_cross(spin, r));
            v3 sliding = v3_sub(point_velocity, v3_scale(n, v3_dot(point_velocity, n)));
            float speed = v3_length(sliding);
            if (speed < 1e-6f) continue;
            v3 along = v3_scale(sliding, 1.0f / speed);
            float kt = inverse_mass + v3_dot(along, v3_cross(inverse_inertia(c, v3_cross(r, along)), r));
            v3 held = v3_add(friction_impulse[i], v3_scale(along, -speed / kt));
            float limit = FRICTION * normal_impulse[i], length = v3_length(held);
            if (length > limit) held = v3_scale(held, limit / length);
            v3 change = v3_sub(held, friction_impulse[i]);
            friction_impulse[i] = held;
            c->velocity = v3_add(c->velocity, v3_scale(change, inverse_mass));
            spin = v3_add(spin, inverse_inertia(c, v3_cross(r, change)));
        }
    }
    c->spin = quat_unrotate(c->orientation, spin);
    // Out of the overlap, along the normals, without counting the same push twice
    // for contacts that share a normal.
    v3 push = v3_make(0, 0, 0);
    for (int i = 0; i < count; ++i) {
        float needed = contacts[i].depth - CONTACT_SLOP - v3_dot(push, contacts[i].normal);
        if (needed > 0.0f) push = v3_add(push, v3_scale(contacts[i].normal, needed * CORRECTION));
    }
    t->position = v3_add(t->position, push);
    return v3_length(v3_sub(c->velocity, velocity_before));
}

// ---------------------------------------------------------------------------
// Altitude
// ---------------------------------------------------------------------------
// The way down for a craft: gravity's, or its own where there is none.
static v3 way_down(game *g, const Craft *c, v3 at) {
    v3 pull = game_gravity(g, at);
    float strength = v3_length(pull);
    return strength > 1e-3f ? v3_scale(pull, 1.0f / strength) : quat_rotate(c->orientation, v3_make(0, -1, 0));
}

// How far along `direction` (unit) from `from` a ray meets an axis-aligned box;
// -1 when it does not. A start inside the box meets it at once.
static float ray_box(v3 from, v3 direction, v3 center, v3 half) {
    float enter = 0.0f, leave = 1e30f;
    for (int k = 0; k < 3; ++k) {
        float o = axis_component(from, k) - axis_component(center, k), d = axis_component(direction, k);
        float h = axis_component(half, k);
        if (fabsf(d) < 1e-8f) {
            if (fabsf(o) > h) return -1.0f;
            continue;
        }
        float t0 = (-h - o) / d, t1 = (h - o) / d;
        if (t0 > t1) { float swap = t0; t0 = t1; t1 = swap; }
        if (t0 > enter) enter = t0;
        if (t1 < leave) leave = t1;
        if (enter > leave) return -1.0f;
    }
    return enter;
}

static float altitude_of(game *g, cw_entity_id craft, const Craft *c, v3 position) {
    cw_world *w = &g->world;
    v3 down = way_down(g, c, position);
    v3 from = v3_add(position, v3_scale(down, c->draft));   // about its lowest point, for a craft on the level
    float best = -1.0f;
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (cw_query_next(&planets, &id)) {
        const PlanetLand *land = &CW_GET(w, id, g->c_planet, Planet)->land;
        if (land->radius <= 0.0f) continue;
        v3 out = v3_sub(from, CW_GET(w, id, g->c_transform, Transform)->position);
        float distance = v3_length(out);
        if (distance > land->radius + land->relief + CRAFT_ALTITUDE_RANGE || distance < 1e-3f) continue;
        float height = distance - (land->radius + game_planet_height(land, v3_scale(out, 1.0f / distance)));
        if (height < 0.0f) height = 0.0f;
        if (height <= CRAFT_ALTITUDE_RANGE && (best < 0.0f || height < best)) best = height;
    }
    // The solids under it: those in the grid cells along the way down.
    cw_entity_id seen[GAME_MAX_BOXES];
    int seen_count = 0;
    for (int sample = 0; sample <= 4; ++sample) {
        cw_entity_id cell[GAME_MAX_BOXES];
        int n = game_solids_near(g, v3_add(from, v3_scale(down, CRAFT_ALTITUDE_RANGE * (float)sample / 4.0f)), cell);
        for (int k = 0; k < n; ++k) {
            bool known = false;
            for (int m = 0; m < seen_count && !known; ++m) known = seen[m] == cell[k];
            if (known || seen_count == GAME_MAX_BOXES) continue;
            seen[seen_count++] = cell[k];
            float t = ray_box(from, down, CW_GET(w, cell[k], g->c_transform, Transform)->position,
                              CW_GET(w, cell[k], g->c_solid, Solid)->half);
            if (t >= 0.0f && t <= CRAFT_ALTITUDE_RANGE && (best < 0.0f || t < best)) best = t;
        }
    }
    (void)craft;
    return best;
}

float game_raycast(game *g, v3 from, v3 direction, float range) {
    cw_world *w = &g->world;
    float best = range;
    // The solids in the grid cells along the way.
    cw_entity_id seen[GAME_MAX_BOXES];
    int seen_count = 0;
    int samples = 1 + (int)(range / (GAME_GRID_CELL * 0.5f));
    for (int sample = 0; sample <= samples; ++sample) {
        cw_entity_id cell[GAME_MAX_BOXES];
        int n = game_solids_near(g, v3_add(from, v3_scale(direction, range * (float)sample / (float)samples)), cell);
        for (int k = 0; k < n; ++k) {
            bool known = false;
            for (int m = 0; m < seen_count && !known; ++m) known = seen[m] == cell[k];
            if (known || seen_count == GAME_MAX_BOXES) continue;
            seen[seen_count++] = cell[k];
            float t = ray_box(from, direction, CW_GET(w, cell[k], g->c_transform, Transform)->position,
                              CW_GET(w, cell[k], g->c_solid, Solid)->half);
            if (t >= 0.0f && t < best) best = t;
        }
    }
    // The land, stepped along a metre at a time where a planet is near enough.
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (cw_query_next(&planets, &id)) {
        const PlanetLand *land = &CW_GET(w, id, g->c_planet, Planet)->land;
        if (land->radius <= 0.0f) continue;
        v3 centre = CW_GET(w, id, g->c_transform, Transform)->position;
        if (v3_length(v3_sub(from, centre)) > land->radius + land->relief + range) continue;
        for (float t = 0.0f; t < best; t += 1.0f) {
            v3 out = v3_sub(v3_add(from, v3_scale(direction, t)), centre);
            float distance = v3_length(out);
            if (distance < 1e-3f || distance < land->radius + game_planet_height(land, v3_scale(out, 1.0f / distance))) {
                best = t > 1.0f ? t - 1.0f : 0.0f;
                break;
            }
        }
    }
    return best;
}

void game_chase_view(game *g, cw_entity_id craft, v3 origin, quat orientation, v3 *eye, v3 *forward, v3 *up) {
    const Craft *c = craft_get(g, craft);
    float size = c && c->radius > 0.0f ? c->radius : 5.0f;
    v3 centre = v3_add(origin, quat_rotate(orientation, v3_make(0, 1.0f, 0)));
    v3 wanted = v3_add(origin, quat_rotate(orientation, v3_make(0, size * CRAFT_CHASE_HEIGHT, size * CRAFT_CHASE_DISTANCE)));
    v3 back = v3_sub(wanted, centre);
    float length = v3_length(back);
    if (length > 1e-3f) {
        // Short of anything between the craft and where the camera wants to be.
        v3 way = v3_scale(back, 1.0f / length);
        float clear = game_raycast(g, centre, way, length);
        if (clear < length) wanted = v3_add(centre, v3_scale(way, clear > 0.5f ? clear - 0.5f : 0.0f));
    }
    v3 ahead = v3_add(origin, quat_rotate(orientation, v3_make(0, size * 0.2f, -size * 4.0f)));
    *eye = wanted;
    *forward = v3_normalize(v3_sub(ahead, wanted));
    *up = quat_rotate(orientation, v3_make(0, 1, 0));
}

float game_craft_altitude(game *g, cw_entity_id craft) {
    const Craft *c = craft_get(g, craft);
    if (!c) return -1.0f;
    return altitude_of(g, craft, c, CW_GET(&g->world, craft, g->c_transform, Transform)->position);
}

float game_craft_climb(game *g, cw_entity_id craft) {
    const Craft *c = craft_get(g, craft);
    if (!c) return 0.0f;
    v3 pull = game_gravity(g, CW_GET(&g->world, craft, g->c_transform, Transform)->position);
    float strength = v3_length(pull);
    v3 up = strength > 1e-3f ? v3_scale(pull, -1.0f / strength) : v3_make(0, 1, 0);
    return v3_dot(c->velocity, up);
}

// ---------------------------------------------------------------------------
// Flight
// ---------------------------------------------------------------------------
static float clamp1(float v) { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }
static v3 mul(v3 a, v3 b) { return v3_make(a.x * b.x, a.y * b.y, a.z * b.z); }
static float clamp_abs(float v, float limit) { return v < -limit ? -limit : v > limit ? limit : v; }

// The pilot's input, when there is a pilot at the helm who has one.
// A client holds inputs for its own player only, and no Aboard for anyone else: a
// pilot without an input there is someone else's business (the snapshots'), and
// only the host may decide that a pilot it cannot see has gone.
static const PlayerInput *pilot_input(game *g, cw_entity_id craft, Craft *c) {
    cw_world *w = &g->world;
    if (c->pilot == CW_INVALID_ENTITY) return NULL;
    bool exists = cw_world_is_valid(w, c->pilot) && cw_comp_has(w, c->pilot, g->c_player);
    if (!exists) {
        if (g->role == CW_HOST) c->pilot = CW_INVALID_ENTITY;
        return NULL;
    }
    if (!cw_comp_has(w, c->pilot, g->c_input)) return NULL;
    if (game_aboard(g, c->pilot) != craft) {   // no longer aboard: the helm is free
        c->pilot = CW_INVALID_ENTITY;
        return NULL;
    }
    return CW_GET(w, c->pilot, g->c_input, PlayerInput);
}

// A sail hand's input, the same way: someone aboard, at that mast's sail station.
static const PlayerInput *hand_input(game *g, cw_entity_id craft, Craft *c, int mast) {
    cw_world *w = &g->world;
    cw_entity_id hand = c->sails[mast].hand;
    if (hand == CW_INVALID_ENTITY) return NULL;
    bool exists = cw_world_is_valid(w, hand) && cw_comp_has(w, hand, g->c_player);
    if (!exists) {
        if (g->role == CW_HOST) c->sails[mast].hand = CW_INVALID_ENTITY;
        return NULL;
    }
    if (!cw_comp_has(w, hand, g->c_input)) return NULL;
    if (game_aboard(g, hand) != craft) {
        c->sails[mast].hand = CW_INVALID_ENTITY;
        return NULL;
    }
    return CW_GET(w, hand, g->c_input, PlayerInput);
}

// Fires the thruster groups for a force in the craft's axes: along each axis the
// group that pushes that way, at the throttle that makes the force or at full when
// it cannot. Returns the force made; adds the groups' torque to `torque`.
static v3 fire_thrusters(Craft *c, v3 wanted, v3 *torque) {
    v3 made = v3_make(0, 0, 0);
    for (int k = 0; k < CRAFT_GROUPS; ++k) c->throttle[k] = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        float along = axis_component(wanted, axis);
        int group = axis * 2 + (along < 0.0f ? 1 : 0);
        if (along == 0.0f || c->thrust[group] <= 0.0f) continue;
        float throttle = fabsf(along) / c->thrust[group];
        if (throttle > 1.0f) throttle = 1.0f;
        c->throttle[group] = throttle;
        float force = throttle * c->thrust[group] * (along < 0.0f ? -1.0f : 1.0f);
        made = v3_add(made, v3_scale(unit_axis(axis), force));
        *torque = v3_add(*torque, v3_scale(c->thrust_torque[group], throttle));
    }
    return made;
}

// The best brace for a flow in the craft's axes, the speed a sail braced at `brace`
// drives it towards, and how well it is trimmed (the cosine of its error, raised to
// its cloth's sharpness, keeping its sign). The angle of the flow off the bow,
// `off` (0: flowing the way the bow points; to starboard positive), makes both: the
// best brace is near half of it, the speed is the flow's running before it, 0.95 of
// it across it and 0.4 of it beating into it.
static void sail_trim(v3 flow, float brace, float sharpness, float *best, float *target, float *trim) {
    float strength = v3_length(flow);
    float off = atan2f(flow.x, -flow.z);
    *best = off * 0.444f;
    if (*best > CRAFT_BRACE_MAX) *best = CRAFT_BRACE_MAX;
    if (*best < -CRAFT_BRACE_MAX) *best = -CRAFT_BRACE_MAX;
    float c = cosf(off), s = sinf(off);
    float ratio = 0.4f + 0.3f * (1.0f + c) + 0.25f * s * s;
    float error = cosf(brace - *best);
    *trim = sharpness == 1.0f || sharpness <= 0.0f ? error : copysignf(powf(fabsf(error), sharpness), error);
    *target = strength * ratio * *trim;
}

void game_craft_sailing(game *g, cw_entity_id craft, int mast, float *best_brace, float *sailing_speed) {
    const Craft *c = craft_get(g, craft);
    *best_brace = *sailing_speed = 0.0f;
    if (!c || mast < 0 || mast >= c->masts) return;
    const craft_sail *sail = &c->sails[mast];
    v3 at = game_craft_to_world(g, craft, sail->center);
    float trim;
    sail_trim(quat_unrotate(c->orientation, game_solar_current_at_tick(g, at, g->world.tick)), sail->brace, sail->sharpness, best_brace,
              sailing_speed, &trim);
}

int game_craft_station_of(const Craft *c, cw_entity_id player) {
    if (player == CW_INVALID_ENTITY) return -1;
    for (int m = 0; m < c->masts && m < CRAFT_MAX_MASTS; ++m)
        if (c->sails[m].hand == player) return m;
    return -1;
}

void game_craft_sail_totals(const Craft *c, float *set, float *fill) {
    float area = 0.0f;
    *set = *fill = 0.0f;
    for (int m = 0; m < c->masts && m < CRAFT_MAX_MASTS; ++m) {
        area += c->sails[m].area;
        *set += c->sails[m].set * c->sails[m].area;
        *fill += c->sails[m].fill * c->sails[m].area;
    }
    if (area > 0.0f) {
        *set /= area;
        *fill /= area;
    }
}

// Whether a pilot in landing mode asks for anything but to stay down: a craft that
// has touched down rests until they do.
static bool asks_to_move(const PlayerInput *in) {
    return in->forward != 0.0f || in->strafe != 0.0f || in->lift > 0.0f || in->turn_yaw != 0.0f;
}

// The spin (in the craft's axes) that turns it back towards `up`, the way up in its
// axes, at CRAFT_ALIGN_RATE per radian off: about its pitch and roll axes only.
static v3 level_spin(v3 up) {
    v3 axis = v3_make(up.z, 0.0f, -up.x);
    float length = v3_length(axis);
    float angle = acosf(up.y > 1.0f ? 1.0f : up.y < -1.0f ? -1.0f : up.y);
    if (length < 1e-6f) return angle > 1.0f ? v3_make(0, 0, CRAFT_ALIGN_RATE * angle) : v3_make(0, 0, 0);   // upside down: roll over
    return v3_scale(axis, CRAFT_ALIGN_RATE * angle / length);
}

// Whether a pilot has a hand on any of the controls.
static bool at_controls(const PlayerInput *in) {
    return in->forward != 0.0f || in->strafe != 0.0f || in->lift != 0.0f || in->turn_pitch != 0.0f || in->turn_yaw != 0.0f
        || in->turn_roll != 0.0f;
}

// The docking autopilot: the inputs a pilot in landing mode would give, in the
// frame level with the ground. First to a point CRAFT_DOCK_HOVER over the dock
// (climbing first, when below it, so the way in clears what stands about the
// dock), turned to the dock's heading; then straight down onto it, corrections
// only past a few decimetres, so that at the floor she asks for nothing and powers
// down. False when the dock is gone.
static bool dock_autopilot(game *g, Craft *c, const Transform *t, PlayerInput *out) {
    cw_world *w = &g->world;
    const Dock *d = dock_get(g, c->dock);
    if (!d) return false;
    v3 up = v3_scale(way_down(g, c, t->position), -1.0f);
    // Where she rests: her keel on the dock's floor, which is level with the world
    // (a deck is a box), whatever the way down is, a little off the pole of a planet.
    v3 rest = v3_add(CW_GET(w, c->dock, g->c_transform, Transform)->position, v3_make(0, c->draft, 0));
    v3 goal = c->docking == DOCK_APPROACH ? v3_add(rest, v3_scale(up, CRAFT_DOCK_HOVER)) : rest;
    v3 error = v3_sub(goal, t->position);
    v3 bow = quat_rotate(c->orientation, v3_make(0, 0, -1));
    v3 ahead = v3_sub(bow, v3_scale(up, v3_dot(bow, up)));
    if (v3_length(ahead) < 1e-3f) return false;
    ahead = v3_normalize(ahead);
    v3 right = v3_cross(ahead, up);
    v3 heading = v3_sub(d->forward, v3_scale(up, v3_dot(d->forward, up)));
    heading = v3_length(heading) > 1e-3f ? v3_normalize(heading) : ahead;
    float ef = v3_dot(error, ahead), er = v3_dot(error, right), eu = v3_dot(error, up);
    float turn = atan2f(v3_dot(v3_cross(ahead, heading), up), v3_dot(ahead, heading));   // positive: the dock's heading is to port
    *out = {};
    out->landing = true;
    out->turn_yaw = clamp1(-turn * 2.5f);
    if (c->docking == DOCK_APPROACH) {
        float reach = eu > 3.0f ? 0.3f : 1.0f;
        out->forward = clamp1(ef / 8.0f) * reach;
        out->strafe = clamp1(er / 8.0f) * reach;
        out->lift = clamp1(eu / 4.0f);
        if (sqrtf(ef * ef + er * er) < 1.0f && fabsf(turn) < 0.05f && fabsf(eu) < 1.5f && v3_length(c->velocity) < 1.5f)
            c->docking = DOCK_DESCEND;
    } else {
        // Down, correcting on the way; the last half metre, nothing but down.
        float altitude = altitude_of(g, CW_INVALID_ENTITY, c, t->position);
        bool last = altitude >= 0.0f && altitude < 0.5f;
        out->forward = !last && fabsf(ef) > 0.3f ? clamp1(ef / 4.0f) : 0.0f;
        out->strafe = !last && fabsf(er) > 0.3f ? clamp1(er / 4.0f) : 0.0f;
        if (last || fabsf(turn) < 0.02f) out->turn_yaw = 0.0f;
        out->lift = -1.0f;
    }
    return true;
}

static void craft_step(game *g, cw_entity_id id, Craft *c, Transform *t, const PlayerInput *in,
                       const PlayerInput *const hands[CRAFT_MAX_MASTS], float dt) {
    contact contacts[MAX_CONTACTS];
    // The sails: each set and braced by whoever is at its mast. Any set, with nobody
    // at the helm, the computer goes on keeping the craft straight and level while
    // the current drives it, as though a pilot sat there letting it be.
    PlayerInput keeping = {};
    bool sailing = false;
    float area = 0.0f, governed = 0.0f;   // ...and how much of the bow the set sails govern
    for (int m = 0; m < c->masts && m < CRAFT_MAX_MASTS; ++m) {
        craft_sail *sail = &c->sails[m];
        if (hands[m]) {
            sail->set += clamp1(hands[m]->sail) * CRAFT_SAIL_RATE * dt;
            if (sail->set < 0.0f) sail->set = 0.0f;
            if (sail->set > 1.0f) sail->set = 1.0f;
            sail->brace += clamp1(hands[m]->brace) * CRAFT_BRACE_RATE * dt;
            if (sail->brace < -CRAFT_BRACE_MAX) sail->brace = -CRAFT_BRACE_MAX;
            if (sail->brace > CRAFT_BRACE_MAX) sail->brace = CRAFT_BRACE_MAX;
        }
        if (sail->set > CRAFT_SAIL_SAILING && sail->area > 0.0f) sailing = true;
        area += sail->area;
        governed += sail->area * sail->set;
    }
    governed = area > 0.0f ? governed / area : 0.0f;
    // Docking: the autopilot flies her in, pilot or none, unless the pilot takes the
    // controls; touched down and resting on the dock, she is docked.
    PlayerInput autopilot = {};
    if (c->docking == DOCK_APPROACH || c->docking == DOCK_DESCEND) {
        if (c->docking == DOCK_DESCEND && c->resting) {
            c->docking = DOCK_DOCKED;
        } else if ((in && at_controls(in)) || !dock_autopilot(g, c, t, &autopilot)) {
            c->docking = DOCK_NONE;
            c->dock = CW_INVALID_ENTITY;
        } else {
            in = &autopilot;
        }
    }
    // Set sails keep her way: unmanned, her computer holds her on as she goes (a
    // craft at rest is not woken by them).
    if (!in && sailing && !c->resting) in = &keeping;
    bool landing = in ? in->landing : c->mode == CRAFT_LANDING;
    // Touched down in landing mode with nothing asked of it, a craft is as good as
    // unpiloted: its engines idle, and it settles and rests.
    bool idle = in == NULL || ((landing || c->docking == DOCK_DOCKED) && !asks_to_move(in));   // docked, she waits to be asked
    if (c->resting) {
        // Still until it is asked to move or the ground under it goes.
        if (idle) {
            v3 down = v3_scale(way_down(g, c, t->position), 0.05f);
            if (gather_contacts(g, id, c, v3_add(t->position, down), c->orientation, contacts) > 0) {
                for (int k = 0; k < CRAFT_GROUPS; ++k) c->throttle[k] = 0.0f;
                for (int m = 0; m < CRAFT_MAX_MASTS; ++m) c->sails[m].fill = 0.0f;
                if (in) c->mode = CRAFT_LANDING;
                return;
            }
        }
        c->resting = false;
        c->quiet_ticks = 0;
    }
    if (c->docking == DOCK_DOCKED) {   // woken, and out of her dock
        c->docking = DOCK_NONE;
        c->dock = CW_INVALID_ENTITY;
    }
    if (c->mass <= 0.0f) return;
    quat q = c->orientation;
    v3 com = v3_add(t->position, quat_rotate(q, c->center));
    v3 gravity = game_gravity(g, com);
    float pull = v3_length(gravity);
    v3 up_world = pull > 1e-3f ? v3_scale(gravity, -1.0f / pull) : quat_rotate(q, v3_make(0, 1, 0));
    // How much a way up there is to keep to: none in open space, all of it near a planet.
    float level_weight = smooth_between(CRAFT_ALIGN_GRAVITY * 0.5f, CRAFT_ALIGN_GRAVITY * 2.0f, pull);

    // The current. It drags the hull towards its flow: across the bow always, along
    // it as far as the sails are not set (set sails govern the bow). And each sail
    // drives the craft along its bow towards its sailing speed for that current and
    // its brace, pushing at the sail, high up, so turning the craft too.
    v3 bow = quat_rotate(q, v3_make(0, 0, -1));
    v3 drift = v3_make(0, 0, 0), sail_force = v3_make(0, 0, 0), sail_torque = v3_make(0, 0, 0);
    if (g->currents) {
        v3 flow = game_solar_current_at_tick(g, com, g->world.tick + 1);
        v3 slip = v3_sub(flow, c->velocity);
        float along = v3_dot(slip, bow);
        v3 across = v3_sub(slip, v3_scale(bow, along));
        drift = v3_scale(v3_add(v3_scale(across, v3_length(across)), v3_scale(bow, along * fabsf(along) * (1.0f - governed))),
                         c->hull_drag);
    }
    for (int m = 0; m < c->masts && m < CRAFT_MAX_MASTS; ++m) {
        craft_sail *sail = &c->sails[m];
        sail->fill = 0.0f;
        if (!g->currents || sail->set <= CRAFT_SAIL_SAILING || sail->area <= 0.0f) continue;
        v3 at = v3_add(t->position, quat_rotate(q, sail->center));
        float best, target, trim;
        sail_trim(quat_unrotate(q, game_solar_current_at_tick(g, at, g->world.tick + 1)), sail->brace, sail->sharpness, &best, &target,
                  &trim);
        float gap = target - v3_dot(c->velocity, bow);
        v3 push = v3_scale(bow, CRAFT_SAIL_PUSH * sail->area * sail->drive * sail->set * gap * fabsf(gap));
        sail_force = v3_add(sail_force, push);
        sail_torque = v3_add(sail_torque, quat_unrotate(q, v3_cross(v3_sub(at, com), push)));
        sail->fill = sail->set * trim;
    }

    // The flight computer: the controls in, thrust and torque out.
    v3 force = v3_make(0, 0, 0), torque = sail_torque, gyro_torque = v3_make(0, 0, 0);
    bool touched_down = false;
    if (in && landing) {
        float altitude = altitude_of(g, id, c, t->position);
        touched_down = altitude >= 0.0f && altitude <= CRAFT_TOUCHDOWN_HEIGHT && !asks_to_move(in);
    }
    if (in && !touched_down) {
        c->mode = landing ? CRAFT_LANDING : CRAFT_FLIGHT;
        c->assist = landing || !in->decoupled;
        // Thrust along the craft's axes; turning about them (nose up is +x, nose
        // right is -y, the starboard side down is -z).
        v3 throttle = v3_make(clamp1(in->strafe), clamp1(in->lift), -clamp1(in->forward));
        v3 turn = v3_make(clamp1(in->turn_pitch), -clamp1(in->turn_yaw), -clamp1(in->turn_roll));
        v3 wanted;
        if (landing) {
            // In a frame level with the ground: its bow laid flat, its way up
            // gravity's. Slow across the ground, and slower down as it comes near.
            v3 ahead = v3_sub(bow, v3_scale(up_world, v3_dot(bow, up_world)));
            if (v3_length(ahead) < 1e-3f) ahead = v3_sub(quat_rotate(q, v3_make(0, 1, 0)), v3_scale(up_world, v3_dot(quat_rotate(q, v3_make(0, 1, 0)), up_world)));
            ahead = v3_normalize(ahead);
            v3 right = v3_cross(ahead, up_world);
            float altitude = altitude_of(g, id, c, t->position);
            float sink = CRAFT_LANDING_SINK;
            if (altitude >= 0.0f) {
                sink = altitude * 0.5f;
                if (sink < CRAFT_TOUCHDOWN_SPEED) sink = CRAFT_TOUCHDOWN_SPEED;
                if (sink > CRAFT_LANDING_SINK) sink = CRAFT_LANDING_SINK;
            }
            v3 target = v3_add(v3_add(v3_scale(right, throttle.x * CRAFT_LANDING_SPEED), v3_scale(ahead, -throttle.z * CRAFT_LANDING_SPEED)),
                               v3_scale(up_world, throttle.y * (throttle.y >= 0.0f ? CRAFT_LANDING_CLIMB : sink)));
            v3 accel = v3_sub(v3_scale(v3_sub(target, c->velocity), 1.0f / c->response), gravity);
            accel = v3_sub(accel, v3_scale(v3_add(drift, sail_force), 1.0f / c->mass));   // held against the current, sail and all
            wanted = quat_unrotate(q, v3_scale(accel, c->mass));
        } else if (c->assist) {
            // A velocity to reach, in the craft's axes: the controls scale the top speeds.
            // Under sail, the bow is the sail's unless the pilot motors.
            v3 target = v3_make(0, 0, 0);
            for (int axis = 0; axis < 3; ++axis) {
                float v = axis_component(throttle, axis);
                float top = c->top_speed[axis * 2 + (v < 0.0f ? 1 : 0)];
                if (axis == 0) target.x = v * top;
                else if (axis == 1) target.y = v * top;
                else target.z = v * top;
            }
            if (sailing && throttle.z == 0.0f) target.z = quat_unrotate(q, c->velocity).z;
            v3 gap = v3_sub(quat_rotate(q, target), c->velocity);
            v3 accel = v3_sub(v3_scale(gap, 1.0f / c->response), gravity);   // gravity held off as well
            // ...and the current's drag, and the sail, except along the bow under sail:
            // that is the sail's to drive, and the current's to slow.
            v3 outside = v3_add(drift, sail_force);
            if (sailing && throttle.z == 0.0f) outside = v3_sub(outside, v3_scale(bow, v3_dot(outside, bow)));
            accel = v3_sub(accel, v3_scale(outside, 1.0f / c->mass));
            wanted = quat_unrotate(q, v3_scale(accel, c->mass));
        } else {
            wanted = v3_make(throttle.x * c->thrust[throttle.x < 0.0f ? CRAFT_GROUP_LEFT : CRAFT_GROUP_RIGHT],
                             throttle.y * c->thrust[throttle.y < 0.0f ? CRAFT_GROUP_DOWN : CRAFT_GROUP_UP],
                             throttle.z * c->thrust[throttle.z < 0.0f ? CRAFT_GROUP_FORWARD : CRAFT_GROUP_BACK]);
        }
        force = fire_thrusters(c, wanted, &torque);
        if (c->assist) {
            // Rates to reach. Near a planet the computer keeps the way up: level with
            // the ground when landing, where the stick turns it about the way up; in
            // flight the wings come level once the roll is let go, the less so the
            // more the nose points up or down.
            v3 target = mul(turn, c->top_rate);
            v3 up = quat_unrotate(q, up_world);   // the way up, in the craft's axes
            if (landing) {
                v3 level = v3_scale(level_spin(up), level_weight);
                target = v3_add(level, v3_scale(up, turn.y * c->top_rate.y));
            } else {
                if (turn.z == 0.0f && level_weight > 0.0f) {
                    float bank = atan2f(-up.x, up.y);
                    float flat = 1.0f - fabsf(v3_dot(quat_rotate(q, v3_make(0, 0, -1)), up_world));
                    target.z = CRAFT_ALIGN_RATE * bank * flat * level_weight;
                }
                // Under sail it sails trim: the pitch comes level too once the stick
                // lets it, against the sail's push high on the mast.
                if (sailing && turn.x == 0.0f && level_weight > 0.0f) target.x = level_spin(up).x * level_weight;
            }
            target = v3_make(clamp_abs(target.x, c->top_rate.x), clamp_abs(target.y, c->top_rate.y), clamp_abs(target.z, c->top_rate.z));
            // ...and the torque for them: what turns it, what keeps a spinning body
            // turning (the gyroscopic term), and what the thrusters' own torque would
            // add, all within what the gyros have.
            v3 needed = v3_scale(mul(c->inertia, v3_sub(target, c->spin)), 1.0f / c->turn_response);
            needed = v3_add(needed, v3_cross(c->spin, mul(c->inertia, c->spin)));
            needed = v3_sub(needed, torque);
            gyro_torque = v3_make(clamp_abs(needed.x, c->gyro.x), clamp_abs(needed.y, c->gyro.y), clamp_abs(needed.z, c->gyro.z));
        } else {
            gyro_torque = mul(turn, c->gyro);
        }
    } else {
        if (in) c->mode = CRAFT_LANDING;   // touched down: powered down, waiting to be asked
        for (int k = 0; k < CRAFT_GROUPS; ++k) c->throttle[k] = 0.0f;
    }
    torque = v3_add(torque, gyro_torque);

    // Newton and Euler, semi-implicitly: velocities first, then the pose with them.
    c->velocity = v3_add(c->velocity, v3_scale(v3_add(v3_scale(v3_add(v3_add(quat_rotate(q, force), sail_force), drift), 1.0f / c->mass), gravity), dt));
    v3 spin_accel = v3_sub(torque, v3_cross(c->spin, mul(c->inertia, c->spin)));
    spin_accel = v3_make(spin_accel.x / c->inertia.x, spin_accel.y / c->inertia.y, spin_accel.z / c->inertia.z);
    c->spin = v3_add(c->spin, v3_scale(spin_accel, dt));
    float speed = v3_length(c->velocity), rate = v3_length(c->spin);
    if (speed > CRAFT_MAX_SPEED) c->velocity = v3_scale(c->velocity, CRAFT_MAX_SPEED / speed);
    if (rate > CRAFT_MAX_SPIN) c->spin = v3_scale(c->spin, CRAFT_MAX_SPIN / rate);
    com = v3_add(com, v3_scale(c->velocity, dt));
    c->orientation = quat_integrate(q, c->spin, dt);
    t->position = v3_sub(com, quat_rotate(c->orientation, c->center));

    int count = gather_contacts(g, id, c, t->position, c->orientation, contacts);
    resolve_contacts(c, t, contacts, count);

    // Nearly still on the ground with nobody flying it (or its pilot having let it
    // settle) for long enough: at rest.
    if (count > 0 && (in == NULL || touched_down) && v3_length(c->velocity) < QUIET_SPEED && v3_length(c->spin) < QUIET_SPIN) {
        if (++c->quiet_ticks >= QUIET_TICKS) {
            c->resting = true;
            c->velocity = c->spin = v3_make(0, 0, 0);
        }
    } else {
        c->quiet_ticks = 0;
    }
}

// Flies every craft this game simulates: all of them on the host; on a client only
// the one its own player pilots (the rest arrive in snapshots, like other players).
void game_system_crafts(cw_world *w, cw_phase, float dt) {
    game *g = game_of(w);
    cw_query crafts = cw_query_begin(g->q_crafts);
    cw_entity_id id;
    while (cw_query_next(&crafts, &id)) {
        Craft *c = CW_GET(w, id, g->c_craft, Craft);
        const PlayerInput *in = pilot_input(g, id, c);
        if (g->role != CW_HOST && in == NULL) continue;
        const PlayerInput *hands[CRAFT_MAX_MASTS] = {};
        for (int m = 0; m < c->masts && m < CRAFT_MAX_MASTS; ++m) hands[m] = hand_input(g, id, c, m);
        craft_step(g, id, c, CW_GET(w, id, g->c_transform, Transform), in, hands, dt);
        // Its parts go with it now, not next tick: the players step after this.
        int first = 0, count = game_craft_part_range(g, id, &first);
        const Transform *ct = CW_GET(w, id, g->c_transform, Transform);
        for (int i = first; i < first + count; ++i) {
            cw_entity_id part = g->craft_parts[i].part;
            Transform *pt = CW_GET(w, part, g->c_transform, Transform);
            v3 offset = CW_GET(w, part, g->c_attachment, Attachment)->offset;
            pt->position = v3_add(ct->position, quat_rotate(c->orientation, offset));
            pt->previous = v3_add(ct->previous, quat_rotate(c->previous_orientation, offset));
        }
    }
}

// The host's part: players at a yard terminal calling their craft, and crafts gone
// too far to come back by themselves towed home.
void game_system_yard(cw_world *w, cw_phase, float) {
    game *g = game_of(w);
    cw_query players = cw_query_begin(g->q_players);
    cw_entity_id id;
    while (cw_query_next(&players, &id)) {
        if (!CW_GET(w, id, g->c_input, PlayerInput)->use || game_aboard(g, id) != CW_INVALID_ENTITY) continue;
        v3 at = CW_GET(w, id, g->c_transform, Transform)->position;
        for (int i = 0; i < g->place_count; ++i) {
            const game_place *p = &g->places[i];
            if (p->kind != PLACE_YARD) continue;
            if (fabsf(at.x - p->center.x) <= p->half.x && fabsf(at.y - p->center.y) <= p->half.y
                && fabsf(at.z - p->center.z) <= p->half.z) {
                const ShipOrder *order = cw_comp_has(w, id, g->c_ship_order) ? CW_GET(w, id, g->c_ship_order, ShipOrder) : NULL;
                game_call_craft(g, id, order ? &order->loadout : NULL);
                break;
            }
        }
    }
    cw_query crafts = cw_query_begin(g->q_crafts);
    while (cw_query_next(&crafts, &id)) {
        v3 at = CW_GET(w, id, g->c_transform, Transform)->position;
        if (v3_length(at) > CRAFT_RECOVERY_RANGE || at.y < CRAFT_RECOVERY_DEPTH) game_craft_to_berth(g, id);
    }
}
