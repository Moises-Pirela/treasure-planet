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
#include "items.h"

#define GAME_TICK_HZ 60
#define GAME_NAME_CAPACITY 32
#define GAME_GRID_CELL 16.0f     // metres; a solid is listed in every cell it touches

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
    bool act;                    // swing: tag whoever is within reach in front
};
struct Player {
    v3 velocity;
    float yaw, pitch;
    bool grounded;
    float swing_cooldown;        // seconds until the next swing counts
    uint16_t hits_made, hits_taken;
    char name[GAME_NAME_CAPACITY];   // the display name, as the account service has it
};
#define PLAYER_REACH 3.0f        // metres a swing carries, from the eyes
#define PLAYER_SWING_COOLDOWN 0.5f
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
// A planet's land: the field the renderer displaces its sphere by, and the ground
// the simulation stands players on, so that what is walked on is what is seen.
// Continents where noise of `continents` frequency passes `sea_level`, ridged
// mountains on them up to `relief` metres, and a flat of `pad_radius` metres about
// the top (the landing pad's).
struct PlanetLand {
    float radius;
    float continents, relief, sea_level, mountains, shore, pad_radius;
    uint32_t seed;
};
struct Planet {
    char name[GAME_NAME_CAPACITY];
    PlanetLand land;
};

// Metres the land rises above the sphere in a direction (unit) from the centre.
float game_planet_height(const PlanetLand *land, v3 direction);
// The height of the land under a point, on the side of the planet the point is on;
// -1e30 where the point is past the planet's limb. `centre` is the planet's.
float game_planet_surface_y(const PlanetLand *land, v3 centre, v3 position);
// A point on the land `arc` metres from the top of the planet along `heading`
// (a horizontal direction), for placing what stands on it.
v3 game_planet_point(const PlanetLand *land, v3 centre, v3 heading, float arc);
struct Equipment {               // what a player wears, for everyone to see
    item items[SLOT_COUNT];
};
struct Loot {                    // an item lying in the world, taken by walking into it
    item it;
};
struct Owned {                   // who an entity belongs to: the account whose player it is (later:
    uint64_t account;            // whose loot, whose ship). Only the owner's client may drive it, and it
};                               // outlives the owner's connection for a while
#define LOOT_REACH 0.9f          // metres from a player's feet at which loot is taken

// MESH_CAPSULE is a player: PLAYER_RADIUS wide and PLAYER_HEIGHT tall with its feet
// at the origin, so it is drawn unscaled (Visual.half of 1) at the Transform.
enum visual_mesh { MESH_CUBE, MESH_SPHERE, MESH_CAPSULE, MESH_COUNT };

// A place is a box a player can be "at", and walking into one may move them to
// another world:
//   PLACE_ZONE      the hub, a planet's landing pad. Zone servers own these, and a
//                   player who walks into one another server owns is handed over.
//                   `spawn` is where a player arriving fresh stands.
//   PLACE_ENTRANCE  a dungeon door. `name` is the dungeon, `destination` the zone
//                   place it stands in, `spawn` the spot in front of it a player
//                   comes back to.
//   PLACE_EXIT      the way out of a dungeon. `destination` and `spawn` are the
//                   place and position to return to.
// Ships in flight are between places.
#define GAME_MAX_PLACES 12
// PLACE_LANDMARK is a named spot, not a way anywhere: the client lights it and
// points at it, the server ignores it.
enum place_kind { PLACE_ZONE, PLACE_ENTRANCE, PLACE_EXIT, PLACE_LANDMARK };
struct game_place {
    char name[GAME_NAME_CAPACITY];
    int kind;                    // place_kind
    v3 center, half;             // the box a player must be inside to be "at" the place
    v3 spawn;
    char destination[GAME_NAME_CAPACITY];
};

struct game {
    cw_world world;
    cw_sim sim;
    cw_sim_role role;
    cw_component_id c_transform, c_solid, c_attachment, c_visual, c_input, c_player, c_ship, c_planet;
    cw_component_id c_equipment, c_loot, c_owned;
    cw_cached_query *q_solids, *q_players, *q_ships, *q_attachments, *q_planets;
    cw_cached_query *q_all_players;   // every player, with or without an input
    cw_cached_query *q_loot;
    // Broadphase for the solids: rebuilt every tick, so that a player's collision
    // test touches the few solids around it rather than every one in the world.
    struct { int32_t key; cw_entity_id id; } *grid;
    int grid_count, grid_capacity;
    v3 spawn;
    double universe_base;        // universe time at tick zero
    game_place places[GAME_MAX_PLACES];
    int place_count;
    char dungeon[GAME_NAME_CAPACITY];   // set when this world is a dungeon
    v3 dungeon_path[GAME_MAX_PLACES];   // room centres in order
    int dungeon_rooms;
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

// Builds the hub, the planets and one ship per planet, and declares the places: each
// planet has a dungeon whose door stands on its landing pad.
void game_build_universe(game *g);
// Builds a dungeon instead: rooms and corridors generated from the name, with the
// spawn in the first room and an exit in the last that returns to `return_place`
// at `return_position`, and a few chests of loot rolled from `loot_seed` (different
// every run). A world holds one or the other, never both.
void game_build_dungeon(game *g, const char *name, const char *return_place, v3 return_position, uint32_t loot_seed);
// Loot lying within reach of a player standing at `feet`, or CW_INVALID_ENTITY.
cw_entity_id game_loot_at(game *g, v3 feet);
cw_entity_id game_spawn_loot(game *g, v3 position, const item *it);
void game_set_owner(game *g, cw_entity_id id, uint64_t account);
uint64_t game_owner(game *g, cw_entity_id id);   // 0 when unowned
// The dungeon of a zone place, as named on its door ("Verdant" -> "Verdant Depths").
void game_dungeon_name(const char *place, char *out, size_t capacity);
// The rooms of a dungeon in walking order (for bots and tests); returns the count.
int game_dungeon_path(const game *g, v3 *out, int capacity);
void game_declare_place(game *g, const char *name, int kind, v3 center, v3 half, v3 spawn, const char *destination);
// Players are capsules, coloured by their id so two players never look alike. The
// input component is what makes a player simulated: a replica leaves it off every
// player but its own.
cw_entity_id game_spawn_player(game *g, v3 position, bool with_input);
void game_despawn_player(game *g, cw_entity_id id);   // gone at the end of the next tick
v3 game_player_color(cw_entity_id id);

// Queries the client and the tests use. game_ship_at returns the ship whose volume
// holds the point, or CW_INVALID_ENTITY. game_place_at returns an index into
// places, or -1 between places.
// The highest planet surface under a point, or -1e30 where no planet is under it.
float game_ground_y(game *g, v3 position);
cw_entity_id game_ship_at(game *g, v3 point);
cw_entity_id game_find_ship(game *g, const char *name);
int game_place_at(const game *g, v3 point);
int game_find_place(const game *g, const char *name);
v3 game_ship_position(const Ship *ship, float t01, bool to_planet);
// Whether a swing from `eye` along `direction` (unit) reaches a player standing at
// `feet`: a ray of PLAYER_REACH against their capsule.
bool game_swing_hits(v3 eye, v3 direction, v3 feet);
// Where the timetable puts a ship at a universe time; fills the derived fields.
v3 game_ship_at_time(Ship *ship, double universe_time);

// Player size, in metres: position is the centre of the feet.
#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_EYE 1.65f

#endif
