// Crafts: ships players own and fly. A craft is a rigid body made of parts; its
// mass, inertia, thrust and torque are the sum of what its parts are, and it moves
// by Newton's laws under gravity, its thrusters and whatever it touches. The same
// code flies it on the server and, one tick ahead, on its pilot's client, so it
// must stay a pure function of the state and the pilot's inputs.
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

// ---------------------------------------------------------------------------
// Designs: what a craft is made of, as data. A part is a box in the craft's frame
// (the origin is the middle of the deck's top, the bow towards -z) with a mass and,
// for thrusters and gyros, what it can do. Everything about how a craft flies
// follows from these rows; adding a design is adding a table.
// ---------------------------------------------------------------------------
struct part_row {
    int kind;
    v3 offset, half, color;
    float mass;                  // kg
    v3 thrust;                   // thrusters: newtons, along one of the craft's axes
    float torque;                // gyros: N m
};

struct design_row {
    const char *name;
    const part_row *parts;
    int part_count;
    v3 cabin_center, cabin_half;
    v3 helm;
    float top_speed[CRAFT_GROUPS];   // right, left, up, down, back, forward
    v3 top_rate;                 // pitch, yaw, roll
};

static const v3 WOOD = {0.45f, 0.3f, 0.18f}, DARK_WOOD = {0.3f, 0.19f, 0.11f}, BRASS = {0.75f, 0.58f, 0.25f};
static const v3 IRON = {0.25f, 0.26f, 0.3f}, SAILCLOTH = {0.9f, 0.86f, 0.74f};

// A sloop: a 4 by 10 metre deck on skids, a mast and a sail for looks, stairs down
// from the stern gangway, two main engines astern, lift thrusters under the hull.
static const part_row SLOOP_PARTS[] = {
    {PART_HULL, {0, -0.4f, 0}, {2.0f, 0.4f, 5.0f}, WOOD, 4000.0f, {}, 0},                  // the deck
    {PART_HULL, {0, -0.3f, -5.6f}, {1.2f, 0.5f, 0.6f}, DARK_WOOD, 600.0f, {}, 0},          // the prow
    {PART_HULL, {1.6f, -1.15f, 0}, {0.15f, 0.35f, 4.2f}, IRON, 300.0f, {}, 0},             // skids
    {PART_HULL, {-1.6f, -1.15f, 0}, {0.15f, 0.35f, 4.2f}, IRON, 300.0f, {}, 0},
    {PART_HULL, {1.9f, 0.5f, 0}, {0.1f, 0.5f, 5.0f}, DARK_WOOD, 150.0f, {}, 0},            // rails
    {PART_HULL, {-1.9f, 0.5f, 0}, {0.1f, 0.5f, 5.0f}, DARK_WOOD, 150.0f, {}, 0},
    {PART_HULL, {0, 0.5f, -4.9f}, {1.8f, 0.5f, 0.1f}, DARK_WOOD, 80.0f, {}, 0},
    {PART_HULL, {1.5f, 0.5f, 4.9f}, {0.5f, 0.5f, 0.1f}, DARK_WOOD, 40.0f, {}, 0},          // beside the gangway
    {PART_HULL, {-1.5f, 0.5f, 4.9f}, {0.5f, 0.5f, 0.1f}, DARK_WOOD, 40.0f, {}, 0},
    {PART_HULL, {0, -0.9f, 5.225f}, {0.55f, 0.6f, 0.225f}, DARK_WOOD, 40.0f, {}, 0},       // stairs, 0.3 m a step
    {PART_HULL, {0, -1.05f, 5.675f}, {0.55f, 0.45f, 0.225f}, DARK_WOOD, 40.0f, {}, 0},
    {PART_HULL, {0, -1.2f, 6.125f}, {0.55f, 0.3f, 0.225f}, DARK_WOOD, 40.0f, {}, 0},
    {PART_HULL, {0, -1.35f, 6.575f}, {0.55f, 0.15f, 0.225f}, DARK_WOOD, 40.0f, {}, 0},
    {PART_HELM, {0, 0.45f, -3.7f}, {0.3f, 0.45f, 0.12f}, BRASS, 80.0f, {}, 0},          // below the pilot's eye line
    {PART_RIGGING, {0, 3.0f, -0.5f}, {0.12f, 3.0f, 0.12f}, DARK_WOOD, 300.0f, {}, 0},     // the mast
    {PART_RIGGING, {0, 3.8f, -0.64f}, {1.7f, 1.8f, 0.04f}, SAILCLOTH, 60.0f, {}, 0},      // the sail
    {PART_GYRO, {0, -0.5f, 0}, {0.4f, 0.3f, 0.4f}, IRON, 500.0f, {}, 260000.0f},
    {PART_THRUSTER, {1.3f, -0.6f, 5.35f}, {0.35f, 0.35f, 0.35f}, BRASS, 350.0f, {0, 0, -60000.0f}, 0},   // main engines
    {PART_THRUSTER, {-1.3f, -0.6f, 5.35f}, {0.35f, 0.35f, 0.35f}, BRASS, 350.0f, {0, 0, -60000.0f}, 0},
    {PART_THRUSTER, {1.3f, -0.6f, -5.3f}, {0.25f, 0.25f, 0.25f}, BRASS, 150.0f, {0, 0, 45000.0f}, 0},    // retro
    {PART_THRUSTER, {-1.3f, -0.6f, -5.3f}, {0.25f, 0.25f, 0.25f}, BRASS, 150.0f, {0, 0, 45000.0f}, 0},
    {PART_THRUSTER, {1.0f, -1.05f, 3.3f}, {0.3f, 0.25f, 0.3f}, IRON, 250.0f, {0, 75000.0f, 0}, 0},      // lift
    {PART_THRUSTER, {-1.0f, -1.05f, 3.3f}, {0.3f, 0.25f, 0.3f}, IRON, 250.0f, {0, 75000.0f, 0}, 0},
    {PART_THRUSTER, {1.0f, -1.05f, -3.3f}, {0.3f, 0.25f, 0.3f}, IRON, 250.0f, {0, 75000.0f, 0}, 0},
    {PART_THRUSTER, {-1.0f, -1.05f, -3.3f}, {0.3f, 0.25f, 0.3f}, IRON, 250.0f, {0, 75000.0f, 0}, 0},
    {PART_THRUSTER, {1.9f, 1.1f, 0}, {0.15f, 0.1f, 0.3f}, IRON, 80.0f, {0, -15000.0f, 0}, 0},          // down
    {PART_THRUSTER, {-1.9f, 1.1f, 0}, {0.15f, 0.1f, 0.3f}, IRON, 80.0f, {0, -15000.0f, 0}, 0},
    {PART_THRUSTER, {-2.1f, -0.4f, 2.5f}, {0.1f, 0.25f, 0.3f}, IRON, 80.0f, {18000.0f, 0, 0}, 0},      // to starboard
    {PART_THRUSTER, {-2.1f, -0.4f, -2.5f}, {0.1f, 0.25f, 0.3f}, IRON, 80.0f, {18000.0f, 0, 0}, 0},
    {PART_THRUSTER, {2.1f, -0.4f, 2.5f}, {0.1f, 0.25f, 0.3f}, IRON, 80.0f, {-18000.0f, 0, 0}, 0},      // to port
    {PART_THRUSTER, {2.1f, -0.4f, -2.5f}, {0.1f, 0.25f, 0.3f}, IRON, 80.0f, {-18000.0f, 0, 0}, 0},
};

#define COUNT_OF(a) (int)(sizeof(a) / sizeof((a)[0]))

static const design_row DESIGNS[] = {
    {"Sloop", SLOOP_PARTS, COUNT_OF(SLOOP_PARTS), {0, 4.0f, 0}, {2.0f, 4.2f, 5.0f}, {0, 0, -3.0f},
     {15.0f, 15.0f, 20.0f, 25.0f, 20.0f, 70.0f}, {0.9f, 0.8f, 1.4f}},
};

int game_craft_design_count(void) { return COUNT_OF(DESIGNS); }
const char *game_craft_design_name(int design) {
    return design >= 0 && design < COUNT_OF(DESIGNS) ? DESIGNS[design].name : "?";
}

// Contacts and the solver.
static const float RESTITUTION = 0.2f;       // of the speed a hard landing bounces back with
static const float BOUNCE_SPEED = 2.0f;      // m/s of impact below which nothing bounces
static const float FRICTION = 0.7f;
static const float CONTACT_SLOP = 0.005f;    // metres of overlap left alone, so resting contacts persist
static const float CORRECTION = 0.8f;        // of the remaining overlap pushed out per tick
static const int SOLVER_ROUNDS = 8;
static const int QUIET_TICKS = 30;           // ticks nearly still on the ground before it rests
static const float QUIET_SPEED = 0.2f, QUIET_SPIN = 0.1f;
#define MAX_CONTACTS 96

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
static float axis_component(v3 v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }
static v3 unit_axis(int k) { return v3_make(k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f); }

static Craft *craft_get(game *g, cw_entity_id id) {
    return cw_comp_has(&g->world, id, g->c_craft) ? CW_GET(&g->world, id, g->c_craft, Craft) : NULL;
}

v3 game_gravity(const game *, v3) { return v3_make(0, -GAME_GRAVITY, 0); }

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
    if (part->kind != PART_THRUSTER) return -1;
    v3 f = part->thrust;
    int axis = fabsf(f.x) >= fabsf(f.y) && fabsf(f.x) >= fabsf(f.z) ? 0 : fabsf(f.y) >= fabsf(f.z) ? 1 : 2;
    float along = axis_component(f, axis);
    if (along == 0.0f) return -1;
    return axis * 2 + (along < 0.0f ? 1 : 0);
}

void game_craft_refit(game *g, cw_entity_id craft) {
    cw_world *w = &g->world;
    Craft *c = craft_get(g, craft);
    if (!c) return;
    c->mass = 0.0f;
    c->center = c->inertia = c->gyro = v3_make(0, 0, 0);
    c->radius = c->draft = 0.0f;
    for (int k = 0; k < CRAFT_GROUPS; ++k) {
        c->thrust[k] = 0.0f;
        c->thrust_torque[k] = v3_make(0, 0, 0);
    }
    // Two passes over the parts: the centre of mass first, then everything about it.
    v3 moment = v3_make(0, 0, 0);
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&parts, &id)) {
        const Attachment *a = CW_GET(w, id, g->c_attachment, Attachment);
        if (a->ship != craft || !cw_comp_has(w, id, g->c_part)) continue;
        const Part *part = CW_GET(w, id, g->c_part, Part);
        c->mass += part->mass;
        moment = v3_add(moment, v3_scale(a->offset, part->mass));
    }
    if (c->mass <= 0.0f) return;
    c->center = v3_scale(moment, 1.0f / c->mass);
    parts = cw_query_begin(g->q_attachments);
    while (cw_query_next(&parts, &id)) {
        const Attachment *a = CW_GET(w, id, g->c_attachment, Attachment);
        if (a->ship != craft || !cw_comp_has(w, id, g->c_part)) continue;
        const Part *part = CW_GET(w, id, g->c_part, Part);
        v3 h = cw_comp_has(w, id, g->c_solid) ? CW_GET(w, id, g->c_solid, Solid)->half : v3_make(0, 0, 0);
        v3 d = v3_sub(a->offset, c->center);
        float m = part->mass;
        // A solid box about its own centre (m/3 of the half extents squared), moved to
        // the craft's centre of mass. The products of inertia are left out: designs
        // are near enough symmetric that the principal axes are the craft's own.
        c->inertia.x += m / 3.0f * (h.y * h.y + h.z * h.z) + m * (d.y * d.y + d.z * d.z);
        c->inertia.y += m / 3.0f * (h.x * h.x + h.z * h.z) + m * (d.x * d.x + d.z * d.z);
        c->inertia.z += m / 3.0f * (h.x * h.x + h.y * h.y) + m * (d.x * d.x + d.y * d.y);
        float reach = v3_length(a->offset) + v3_length(h);
        if (reach > c->radius) c->radius = reach;
        if (h.y - a->offset.y > c->draft) c->draft = h.y - a->offset.y;
        int group = game_part_group(part);
        if (group >= 0) {
            // Only along its group's axis: what else it points along is lost.
            int axis = group / 2;
            float along = axis_component(part->thrust, axis);
            v3 pure = v3_scale(unit_axis(axis), along);
            c->thrust[group] += fabsf(along);
            c->thrust_torque[group] = v3_add(c->thrust_torque[group], v3_cross(d, pure));
        } else if (part->kind == PART_GYRO) {
            c->gyro = v3_add(c->gyro, v3_make(part->torque, part->torque, part->torque));
        }
    }
}

cw_entity_id game_spawn_craft(game *g, int design, v3 floor, float heading, uint64_t owner, int berth) {
    cw_world *w = &g->world;
    if (design < 0 || design >= COUNT_OF(DESIGNS)) design = 0;
    const design_row *d = &DESIGNS[design];
    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {floor, floor};
    Craft c = {};
    strncpy(c.name, d->name, GAME_NAME_CAPACITY - 1);
    c.design = design;
    c.cabin_center = d->cabin_center;
    c.cabin_half = d->cabin_half;
    c.helm = d->helm;
    c.pilot = CW_INVALID_ENTITY;
    c.berth = berth;
    c.orientation = c.previous_orientation = quat_from_yaw(heading);
    c.assist = true;
    for (int k = 0; k < CRAFT_GROUPS; ++k) c.top_speed[k] = d->top_speed[k];
    c.top_rate = d->top_rate;
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_craft, &c, sizeof(c));
    if (owner) game_set_owner(g, id, owner);
    for (int i = 0; i < d->part_count; ++i) {
        const part_row *row = &d->parts[i];
        cw_entity_id part = cw_world_create_entity(w);
        Transform pt = {floor, floor};
        Solid solid = {row->half};
        Visual visual = {MESH_CUBE, row->half, row->color};
        Attachment a = {id, row->offset};
        Part p = {row->kind, row->mass, row->thrust, row->torque};
        cw_comp_set(w, part, g->c_transform, &pt, sizeof(pt));
        cw_comp_set(w, part, g->c_solid, &solid, sizeof(solid));
        cw_comp_set(w, part, g->c_visual, &visual, sizeof(visual));
        cw_comp_set(w, part, g->c_attachment, &a, sizeof(a));
        cw_comp_set(w, part, g->c_part, &p, sizeof(p));
    }
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

cw_entity_id game_call_craft(game *g, cw_entity_id player) {
    uint64_t owner = game_owner(g, player);
    cw_entity_id craft = game_craft_of(g, owner);
    if (craft != CW_INVALID_ENTITY) {
        if (CW_GET(&g->world, craft, g->c_craft, Craft)->berth >= 0) game_craft_to_berth(g, craft);
        return craft;
    }
    for (int b = 0; b < g->berth_count; ++b) {
        bool taken = false;
        cw_query crafts = cw_query_begin(g->q_crafts);
        cw_entity_id id;
        while (!taken && cw_query_next(&crafts, &id)) taken = CW_GET(&g->world, id, g->c_craft, Craft)->berth == b;
        if (!taken) return game_spawn_craft(g, 0, g->berths[b].floor, g->berths[b].heading, owner, b);
    }
    return CW_INVALID_ENTITY;
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

// The land's normal under a point, from the heights a little either way.
static v3 ground_normal(game *g, v3 p, float height) {
    const float step = 0.5f;
    float hx = game_ground_y(g, v3_add(p, v3_make(step, 0, 0)));
    float hz = game_ground_y(g, v3_add(p, v3_make(0, 0, step)));
    if (hx < -1e29f || hz < -1e29f) return v3_make(0, 1, 0);
    return v3_normalize(v3_make(-(hx - height) / step, 1.0f, -(hz - height) / step));
}

// Every contact of the craft posed at `position` and `q` with the land and the solids.
static int gather_contacts(game *g, cw_entity_id craft, const Craft *c, v3 position, quat q, contact *out) {
    cw_world *w = &g->world;
    int count = 0;
    // The land, only near enough a planet for it to matter.
    bool near_land = false;
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (!near_land && cw_query_next(&planets, &id)) {
        const Planet *planet = CW_GET(w, id, g->c_planet, Planet);
        if (planet->land.radius <= 0.0f) continue;
        float reach = planet->land.radius + planet->land.relief + c->radius + 1.0f;
        near_land = v3_length(v3_sub(position, CW_GET(w, id, g->c_transform, Transform)->position)) < reach;
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
    if (!near_land && solid_count == 0) return 0;

    int first = 0, parts = game_craft_part_range(g, craft, &first);
    for (int i = first; i < first + parts; ++i) {
        cw_entity_id part = g->craft_parts[i].part;
        if (!cw_comp_has(w, part, g->c_solid)) continue;
        obb box;
        box.center = v3_add(position, quat_rotate(q, CW_GET(w, part, g->c_attachment, Attachment)->offset));
        for (int k = 0; k < 3; ++k) box.axis[k] = quat_rotate(q, unit_axis(k));
        box.half = CW_GET(w, part, g->c_solid, Solid)->half;
        if (near_land) {
            for (int k = 0; k < 8; ++k) {
                v3 corner = obb_corner(&box, k);
                float ground = game_ground_y(g, corner);
                if (ground < -1e29f || corner.y >= ground) continue;
                v3 n = ground_normal(g, corner, ground);
                push_contact(out, &count, corner, n, (ground - corner.y) * n.y);
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
static void resolve_contacts(Craft *c, Transform *t, const contact *contacts, int count) {
    if (count == 0 || c->mass <= 0.0f) return;
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

static void craft_step(game *g, cw_entity_id id, Craft *c, Transform *t, const PlayerInput *in, float dt) {
    contact contacts[MAX_CONTACTS];
    if (c->resting) {
        // Still until the pilot flies it or the ground under it goes.
        if (in == NULL) {
            v3 down = v3_scale(v3_normalize(game_gravity(g, t->position)), 0.05f);
            if (gather_contacts(g, id, c, v3_add(t->position, down), c->orientation, contacts) > 0) return;
        }
        c->resting = false;
        c->quiet_ticks = 0;
    }
    if (c->mass <= 0.0f) return;
    quat q = c->orientation;
    v3 com = v3_add(t->position, quat_rotate(q, c->center));
    v3 gravity = game_gravity(g, com);

    // The flight computer: the controls in, thrust and torque out.
    v3 force = v3_make(0, 0, 0), torque = v3_make(0, 0, 0), gyro_torque = v3_make(0, 0, 0);
    if (in) {
        c->assist = !in->decoupled;
        // Thrust along the craft's axes; turning about them (nose up is +x, nose
        // right is -y, the starboard side down is -z).
        v3 throttle = v3_make(clamp1(in->strafe), clamp1(in->lift), -clamp1(in->forward));
        v3 turn = v3_make(clamp1(in->turn_pitch), -clamp1(in->turn_yaw), -clamp1(in->turn_roll));
        v3 wanted;
        if (c->assist) {
            // A velocity to reach, in the craft's axes: the controls scale the top speeds.
            v3 target = v3_make(0, 0, 0);
            for (int axis = 0; axis < 3; ++axis) {
                float v = axis_component(throttle, axis);
                float top = c->top_speed[axis * 2 + (v < 0.0f ? 1 : 0)];
                if (axis == 0) target.x = v * top;
                else if (axis == 1) target.y = v * top;
                else target.z = v * top;
            }
            v3 gap = v3_sub(quat_rotate(q, target), c->velocity);
            v3 accel = v3_sub(v3_scale(gap, 1.0f / CRAFT_RESPONSE), gravity);   // gravity held off as well
            wanted = quat_unrotate(q, v3_scale(accel, c->mass));
        } else {
            wanted = v3_make(throttle.x * c->thrust[throttle.x < 0.0f ? CRAFT_GROUP_LEFT : CRAFT_GROUP_RIGHT],
                             throttle.y * c->thrust[throttle.y < 0.0f ? CRAFT_GROUP_DOWN : CRAFT_GROUP_UP],
                             throttle.z * c->thrust[throttle.z < 0.0f ? CRAFT_GROUP_FORWARD : CRAFT_GROUP_BACK]);
        }
        force = fire_thrusters(c, wanted, &torque);
        if (c->assist) {
            // Rates to reach, and the torque for them: what turns it, what keeps a
            // spinning body turning (the gyroscopic term), and what the thrusters'
            // own torque would add, all within what the gyros have.
            v3 target = mul(turn, c->top_rate);
            v3 needed = v3_scale(mul(c->inertia, v3_sub(target, c->spin)), 1.0f / CRAFT_TURN_RESPONSE);
            needed = v3_add(needed, v3_cross(c->spin, mul(c->inertia, c->spin)));
            needed = v3_sub(needed, torque);
            gyro_torque = v3_make(clamp_abs(needed.x, c->gyro.x), clamp_abs(needed.y, c->gyro.y), clamp_abs(needed.z, c->gyro.z));
        } else {
            gyro_torque = mul(turn, c->gyro);
        }
    } else {
        for (int k = 0; k < CRAFT_GROUPS; ++k) c->throttle[k] = 0.0f;
    }
    torque = v3_add(torque, gyro_torque);

    // Newton and Euler, semi-implicitly: velocities first, then the pose with them.
    c->velocity = v3_add(c->velocity, v3_scale(v3_add(v3_scale(quat_rotate(q, force), 1.0f / c->mass), gravity), dt));
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

    // Nearly still on the ground with nobody flying it for long enough: at rest.
    if (count > 0 && in == NULL && v3_length(c->velocity) < QUIET_SPEED && v3_length(c->spin) < QUIET_SPIN) {
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
        craft_step(g, id, c, CW_GET(w, id, g->c_transform, Transform), in, dt);
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
                game_call_craft(g, id);
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
