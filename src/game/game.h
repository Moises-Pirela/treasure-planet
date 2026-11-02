// The simulation: everything the server owns. No renderer, no window and no notion
// of a "local" player here, so it runs headless (see tests/test_game.cpp).
//
//   per-player input + world  ->  fixed 60 Hz tick  ->  world
//
// All game state lives in the cw_world. Everything shares one coordinate space and
// ships really fly through it: nothing teleports and no level is loaded.
//
// The same code serves two roles. A CW_HOST game is authoritative and owns the truth.
// A CW_CLIENT game is a replica fed by the network that predicts what it can: ships,
// which are a pure function of universe time, and the one player it has an input
// for; every other player is interpolated from snapshots. The ECS skips
// CW_HOST_ONLY systems on a client, which is how the server keeps hooks to itself.
//
// Universe time is the clock every zone server and every client share: ships follow
// a timetable against it, so two servers that have never spoken agree where a ship
// is, and a client predicts a ship exactly. It is tick * dt plus a base set at start.
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
    float phase;                 // seconds into the timetable at universe time zero
    // Derived from universe time every tick, never integrated:
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

// A place is somewhere players stand still long enough to belong to: the hub, a
// planet's landing pad. Zone servers own places; a player who walks into a place
// another server owns is handed over. Ships in flight are between places.
#define GAME_MAX_PLACES 8
struct game_place {
    char name[GAME_NAME_CAPACITY];
    v3 center, half;             // the box a player must be inside to be "at" the place
    v3 spawn;                    // where a player arriving fresh stands
};

struct game {
    cw_world world;
    cw_sim sim;
    cw_sim_role role;
    cw_component_id c_transform, c_solid, c_attachment, c_visual, c_input, c_player, c_ship, c_planet;
    cw_cached_query *q_solids, *q_players, *q_ships, *q_attachments;
    cw_cached_query *q_all_players;   // every player, with or without an input
    v3 spawn;
    double universe_base;        // universe time at tick zero
    game_place places[GAME_MAX_PLACES];
    int place_count;
};

void game_init(game *g, cw_sim_role role);
void game_destroy(game *g);
// One fixed step: every system, then the entity lifecycle, then the tick advances.
void game_tick(game *g);
// One step of the movement systems only (ships, attachments, players), for replaying
// a client's inputs over a state the server sent. Advances the tick like game_tick.
void game_replay_tick(game *g);
// The universe time at which the positions of tick `tick` hold.
double game_time_of_tick(const game *g, uint64_t tick);
// Puts every ship and its parts where the timetable has them at the current tick,
// previous position included, carrying nobody: for a replica whose tick just moved.
void game_place_ships(game *g);

// Builds the hub, the planets and one ship per planet, and declares the places.
void game_build_universe(game *g);
// Players are capsules, coloured by their id so two players never look alike. The
// input component is what makes a player simulated: a replica leaves it off every
// player but its own.
cw_entity_id game_spawn_player(game *g, v3 position, bool with_input);
void game_despawn_player(game *g, cw_entity_id id);   // gone at the end of the next tick
v3 game_player_color(cw_entity_id id);

// Queries the client and the tests use. game_ship_at returns the ship whose volume
// holds the point, or CW_INVALID_ENTITY. game_place_at returns an index into
// places, or -1 between places.
cw_entity_id game_ship_at(game *g, v3 point);
cw_entity_id game_find_ship(game *g, const char *name);
int game_place_at(const game *g, v3 point);
int game_find_place(const game *g, const char *name);
v3 game_ship_position(const Ship *ship, float t01, bool to_planet);
// Where the timetable puts a ship at a universe time; fills the derived fields.
v3 game_ship_at_time(Ship *ship, double universe_time);

// Player size, in metres: position is the centre of the feet.
#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_EYE 1.65f

#endif
