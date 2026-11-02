// The simulation: everything the server owns. No renderer, no window and no notion
// of a "local" player here, so it runs headless (see tests/test_game.cpp).
//
//   per-player input + world  ->  fixed 60 Hz tick  ->  world
//
// All game state lives in the cw_world. Everything shares one coordinate space and
// ships really fly through it: nothing teleports and no level is loaded.
//
// The same code serves two roles. A CW_HOST game is authoritative: it runs every
// system and owns the truth. A CW_CLIENT game is a replica fed by the network: the
// ECS skips CW_HOST_ONLY systems for it, so it never simulates players or ships, and
// only derives what follows from replicated state (ship parts riding their hull).
#ifndef TP_GAME_H
#define TP_GAME_H

#include "../ecs/clockwork.h"
#include "../tp_math.h"

#define GAME_TICK_HZ 60
#define GAME_NAME_CAPACITY 32

// Components. Plain data, zero is a valid "unset" for every field.
struct Transform {
    v3 position, previous;       // previous: last tick's position, for render interpolation
};
struct Solid {                   // an axis-aligned box players collide with
    v3 half;
};
struct Attachment {              // moves with its ship at a fixed offset
    cw_entity_id ship;
    v3 offset;
};
struct Visual {
    int mesh;                    // visual_mesh
    v3 half;                     // half extents of the drawn mesh
    v3 color;
};
struct PlayerInput {             // what a client sends each tick
    float forward, strafe;       // -1..1
    float yaw, pitch;
    bool jump;
};
struct Player {
    v3 velocity;
    float yaw, pitch;
    bool grounded;
};
enum ship_state { SHIP_DOCKED, SHIP_FLYING };
enum ship_stop { STOP_HUB, STOP_PLANET };
struct Ship {
    char name[GAME_NAME_CAPACITY];
    v3 half;                     // the volume that travels with the ship
    v3 hub_dock, planet_dock;
    cw_entity_id planet;
    float cruise_height;         // height above the higher dock while cruising
    float flight_seconds, dock_seconds;
    float timer;                 // seconds in the current state
    int state;                   // ship_state
    int stop;                    // ship_stop: where it is docked, or flying to
};
struct Planet {
    char name[GAME_NAME_CAPACITY];
};

// MESH_CAPSULE is a player: PLAYER_RADIUS wide and PLAYER_HEIGHT tall with its feet
// at the origin, so it is drawn unscaled (Visual.half of 1) at the Transform.
enum visual_mesh { MESH_CUBE, MESH_SPHERE, MESH_CAPSULE, MESH_COUNT };

struct game {
    cw_world world;
    cw_sim sim;
    cw_sim_role role;
    cw_component_id c_transform, c_solid, c_attachment, c_visual, c_input, c_player, c_ship, c_planet;
    cw_cached_query *q_solids, *q_players, *q_ships, *q_attachments;
    cw_cached_query *q_dynamic;  // what moves on its own and so is worth a snapshot: players and ships
    v3 spawn;
};

void game_init(game *g, cw_sim_role role);
void game_destroy(game *g);
// One authoritative step (CW_HOST). On a client this runs only the derived systems.
void game_tick(game *g);
// A client's per-frame step: derives ship parts from their hulls and sweeps despawned
// entities. Nothing else moves; the network moved it already.
void game_client_frame(game *g);

// Builds the hub, the planets and one ship per planet.
void game_build_universe(game *g);
// Players are capsules, coloured by their id so two players never look alike.
cw_entity_id game_spawn_player(game *g, v3 position);
void game_despawn_player(game *g, cw_entity_id id);   // gone at the end of the next tick
v3 game_player_color(cw_entity_id id);

// Queries the client and the tests use. game_ship_at returns the ship whose volume
// holds the point, or CW_INVALID_ENTITY.
cw_entity_id game_ship_at(game *g, v3 point);
cw_entity_id game_find_ship(game *g, const char *name);
// Every player in this world. Not q_players, which holds those simulated here and so
// is empty on a client: inputs never leave the server.
size_t game_player_count(game *g);
v3 game_ship_position(const Ship *ship, float t01, bool to_planet);

// Player size, in metres: position is the centre of the feet.
#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_EYE 1.65f

#endif
