// Captains: players of the world's own, each the owner of a ship, who sail her from
// dock to dock about the hub and the planets. They fly as anyone flies: standing
// at the helm, their inputs set each tick as a client's would have been, the
// flight computer and the docking autopilot doing the rest. The host's alone, and
// a pure function of the world and their dice, so a world with captains ticks the
// same however it is batched.
//
//   boarding -> in port (a while) -> climbing -> cruising to a dock -> docking -> in port ...
#include "game_internal.h"
#include <stdio.h>
#include <string.h>

static const char *const NAMES[] = {"Captain Marlowe", "Captain Quill",  "Captain Brand", "Captain Halloway", "Captain Teague",
                                    "Captain Vance",   "Captain Morrow", "Captain Lark",  "Captain Ashby",    "Captain Crane"};
#define COUNT_OF(a) (int)(sizeof(a) / sizeof((a)[0]))

static const float CAPTAIN_CRUISE_HEIGHT = 40.0f;    // metres over a dock she makes for
static const float CAPTAIN_CLEARANCE = 20.0f;        // metres over the ground below which she climbs, whatever else
static const float CAPTAIN_GIVE_UP = 240.0f;         // seconds of cruising, or ninety of docking, before she is towed in

static uint32_t roll(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}
static float roll_between(uint32_t *state, float low, float high) {
    return low + (high - low) * (float)(roll(state) & 0xffff) / 65535.0f;
}

bool game_is_captain_account(game *g, uint64_t account) {
    cw_query captains = cw_query_begin(g->q_captains);
    cw_entity_id id;
    while (cw_query_next(&captains, &id))
        if (game_owner(g, id) == account) return true;
    return false;
}

static const hull_spec *hull_of(game *g, cw_entity_id craft) {
    return ship_hull(CW_GET(&g->world, craft, g->c_craft, Craft)->loadout.hull);
}

// A dock for a ship: free, one that takes her, away from where she is (more than
// `away` metres), picked by the dice among those there are.
static cw_entity_id pick_dock(game *g, cw_entity_id craft, int size, float away, uint32_t *dice) {
    cw_world *w = &g->world;
    v3 at = CW_GET(w, craft, g->c_transform, Transform)->position;
    cw_entity_id choices[32];
    int n = 0;
    cw_query docks = cw_query_begin(g->q_docks);
    cw_entity_id id;
    while (cw_query_next(&docks, &id) && n < 32) {
        if (CW_GET(w, id, g->c_dock, Dock)->size < size || !game_dock_free(g, id, craft)) continue;
        if (v3_length(v3_sub(CW_GET(w, id, g->c_transform, Transform)->position, at)) <= away) continue;
        choices[n++] = id;
    }
    return n ? choices[roll(dice) % (uint32_t)n] : CW_INVALID_ENTITY;
}

void game_spawn_captains(game *g, int count, uint32_t seed) {
    cw_world *w = &g->world;
    // Mostly small ships: there are twice as many small docks as large.
    static const int HULLS[] = {HULL_SLOOP, HULL_SLOOP, HULL_CUTTER, HULL_CUTTER, HULL_BRIGANTINE, HULL_GALLEON};
    for (int i = 0; i < count; ++i) {
        uint32_t dice = seed * 2654435761u + (uint32_t)i * 40503u + 1u;
        roll(&dice);
        ship_loadout l = ship_default_loadout(HULLS[roll(&dice) % (uint32_t)COUNT_OF(HULLS)]);
        // A look of her own: most paint, some as built; any cut, any shape.
        for (int f = LOOK_HULL_PAINT; f <= LOOK_SAIL_PAINT; ++f)
            l.look.field[f] = (uint8_t)(roll(&dice) % 4 == 0 ? 0 : 1 + roll(&dice) % (uint32_t)(ship_look_options(f) - 1));
        l.look.field[LOOK_SAIL_CUT] = (uint8_t)(roll(&dice) % CUT_COUNT);
        l.look.field[LOOK_HULL_SHAPE] = (uint8_t)(roll(&dice) % SHAPE_COUNT);
        // A free dock that takes her, to start from.
        cw_entity_id dock = CW_INVALID_ENTITY;
        cw_entity_id choices[32];
        int n = 0;
        cw_query docks = cw_query_begin(g->q_docks);
        cw_entity_id id;
        while (cw_query_next(&docks, &id) && n < 32)
            if (CW_GET(w, id, g->c_dock, Dock)->size >= ship_hull(l.hull)->berth && game_dock_free(g, id, CW_INVALID_ENTITY)) choices[n++] = id;
        if (n == 0) continue;   // every dock that would take her is taken
        dock = choices[roll(&dice) % (uint32_t)n];
        uint64_t account = CAPTAIN_ACCOUNT_BASE + (uint64_t)i;
        v3 floor = CW_GET(w, dock, g->c_transform, Transform)->position;
        cw_entity_id craft = game_spawn_craft(g, &l, floor, 0.0f, account, -1);
        if (craft == CW_INVALID_ENTITY) continue;
        game_craft_lay_up(g, craft, dock);
        // The captain, at her helm.
        const Craft *c = CW_GET(w, craft, g->c_craft, Craft);
        cw_entity_id captain = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(c->helm, v3_make(0, 0.05f, 0))), true);
        game_set_owner(g, captain, account);
        Player *p = CW_GET(w, captain, g->c_player, Player);
        snprintf(p->name, sizeof(p->name), "%s", NAMES[i % COUNT_OF(NAMES)]);
        Captain cap = {};
        cap.craft = craft;
        cap.state = CAPTAIN_BOARDING;
        cap.destination = CW_INVALID_ENTITY;
        cap.seed = dice;
        cw_comp_set(w, captain, g->c_captain, &cap, sizeof(cap));
    }
}

// Steering for a point: the stick towards it, the throttle as far as the distance
// warrants (easing off as she comes near), and up, whatever else, when the ground
// comes too close below.
static void steer_for(game *g, cw_entity_id craft, v3 goal, PlayerInput *in) {
    const Craft *c = CW_GET(&g->world, craft, g->c_craft, Craft);
    v3 at = CW_GET(&g->world, craft, g->c_transform, Transform)->position;
    v3 to = v3_sub(goal, at);
    float distance = v3_length(to);
    if (distance < 1e-3f) return;
    v3 local = quat_unrotate(c->orientation, v3_scale(to, 1.0f / distance));
    float yaw = atan2f(local.x, -local.z), pitch = atan2f(local.y, sqrtf(local.x * local.x + local.z * local.z));
    in->turn_yaw = yaw * 1.5f > 1.0f ? 1.0f : yaw * 1.5f < -1.0f ? -1.0f : yaw * 1.5f;
    in->turn_pitch = pitch * 1.5f > 1.0f ? 1.0f : pitch * 1.5f < -1.0f ? -1.0f : pitch * 1.5f;
    bool aligned = fabsf(yaw) < 0.5f && fabsf(pitch) < 0.5f;
    float throttle = (distance - 60.0f) / 300.0f;
    throttle = throttle < 0.12f ? 0.12f : throttle > 1.0f ? 1.0f : throttle;
    in->forward = aligned ? throttle : 0.1f;
    float altitude = game_craft_altitude(g, craft);
    if (altitude >= 0.0f && altitude < CAPTAIN_CLEARANCE) {
        in->lift = 1.0f;
        if (in->turn_pitch < 0.3f) in->turn_pitch = 0.3f;
    }
}

void game_system_captains(cw_world *w, cw_phase, float dt) {
    game *g = game_of(w);
    cw_query captains = cw_query_begin(g->q_captains);
    cw_entity_id id;
    while (cw_query_next(&captains, &id)) {
        Captain *cap = CW_GET(w, id, g->c_captain, Captain);
        if (!cw_comp_has(w, id, g->c_input)) continue;
        PlayerInput *in = CW_GET(w, id, g->c_input, PlayerInput);
        *in = {};
        if (!cw_world_is_valid(w, cap->craft) || !cw_comp_has(w, cap->craft, g->c_craft)) continue;   // her ship is gone
        Craft *c = CW_GET(w, cap->craft, g->c_craft, Craft);
        const hull_spec *hull = hull_of(g, cap->craft);
        cap->timer += dt;
        switch (cap->state) {
        case CAPTAIN_BOARDING:
            if (c->pilot == id) {
                cap->state = CAPTAIN_IN_PORT;
                cap->timer = -roll_between(&cap->seed, 10.0f, 40.0f);   // counts up to zero, in port
            } else if (game_aboard(g, id) == cap->craft) {
                in->use = (w->tick & 1) == 0;   // a press, then a release
            } else if (cap->timer > 3.0f) {
                // Not aboard yet (her ship was rebuilt, or moved): put her at the helm.
                Transform *t = CW_GET(w, id, g->c_transform, Transform);
                t->position = t->previous = game_craft_to_world(g, cap->craft, v3_add(c->helm, v3_make(0, 0.05f, 0)));
                cap->timer = 0.0f;
            }
            break;
        case CAPTAIN_IN_PORT:
            if (c->pilot != id) {
                cap->state = CAPTAIN_BOARDING;
                cap->timer = 0.0f;
                break;
            }
            if (cap->timer < 0.0f) break;
            cap->destination = pick_dock(g, cap->craft, hull->berth, 300.0f, &cap->seed);
            if (cap->destination == CW_INVALID_ENTITY) {
                cap->timer = -10.0f;   // nowhere to go: wait, and look again
                break;
            }
            cap->state = CAPTAIN_CLIMBING;
            cap->timer = 0.0f;
            break;
        case CAPTAIN_CLIMBING: {
            in->lift = 1.0f;   // flight mode: asking to move, she wakes and leaves her dock
            float altitude = game_craft_altitude(g, cap->craft);
            if (cap->timer > 2.0f && (altitude < 0.0f || altitude > CAPTAIN_CRUISE_HEIGHT * 0.75f)) {
                cap->state = CAPTAIN_CRUISING;
                cap->timer = 0.0f;
            }
            break;
        }
        case CAPTAIN_CRUISING: {
            if (!game_dock_free(g, cap->destination, cap->craft)) {   // taken while she was on her way
                cw_entity_id other = pick_dock(g, cap->craft, hull->berth, 0.0f, &cap->seed);
                if (other != CW_INVALID_ENTITY) cap->destination = other;
            }
            v3 floor = CW_GET(w, cap->destination, g->c_transform, Transform)->position;
            v3 pull = game_gravity(g, floor);
            v3 up = v3_length(pull) > 1e-3f ? v3_normalize(v3_scale(pull, -1.0f)) : v3_make(0, 1, 0);
            steer_for(g, cap->craft, v3_add(floor, v3_scale(up, CAPTAIN_CRUISE_HEIGHT)), in);
            v3 at = CW_GET(w, cap->craft, g->c_transform, Transform)->position;
            // Near enough, and a dock in reach: the dock key, and the autopilot brings her in.
            if (v3_length(v3_sub(floor, at)) < CRAFT_DOCK_REACH * 0.8f && game_dock_in_reach(g, cap->craft) != CW_INVALID_ENTITY
                && (w->tick & 1) == 0) {
                *in = {};
                in->dock = true;
                cap->state = CAPTAIN_DOCKING;
                cap->timer = 0.0f;
            } else if (cap->timer > CAPTAIN_GIVE_UP) {
                game_craft_lay_up(g, cap->craft, cap->destination);   // lost: towed in
                cap->state = CAPTAIN_IN_PORT;
                cap->timer = -roll_between(&cap->seed, 10.0f, 40.0f);
            }
            break;
        }
        case CAPTAIN_DOCKING:
            if (c->docking == DOCK_DOCKED) {
                ++cap->voyages;
                cap->state = CAPTAIN_IN_PORT;
                cap->timer = -roll_between(&cap->seed, 15.0f, 50.0f);
            } else if (c->docking == DOCK_NONE && cap->timer > 0.5f) {
                cap->state = CAPTAIN_CRUISING;   // it would not have her: on again
                cap->timer = 0.0f;
            } else if (cap->timer > 90.0f && c->dock != CW_INVALID_ENTITY) {
                game_craft_lay_up(g, cap->craft, c->dock);
            }
            break;
        default:
            break;
        }
    }
}
