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
    float forward, strafe;       // -1..1; at a helm, thrust along the craft's -z (its bow) and +x
    float yaw, pitch;            // the look, in the frame the player stands in (a craft's, aboard one)
    bool jump;
    bool act;                    // swing: tag whoever is within reach in front
    // At a helm (craft.cpp). Ignored on foot.
    float lift;                  // -1..1: thrust along the craft's +y
    float turn_pitch, turn_yaw, turn_roll;   // -1..1: nose up, nose right, right side down
    bool decoupled;              // flight assist off: the controls are thrust and torque, nothing holds them
    bool use;                    // an edge, like act: take or leave a helm, call a craft at a yard terminal
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

// A craft: a ship a player owns and flies, where a liner (Ship) keeps a timetable.
// It is a rigid body. Its parts are entities of their own (an Attachment to it, a
// Solid and a Visual, and a Part saying what the part does), boxes in its frame;
// its mass, centre of mass, inertia, thrust and torque all come from them
// (game_craft_refit), so a craft built differently flies differently. Its pose is
// the Transform's position (the origin of its frame: the middle of the deck's top)
// and `orientation`; its axes are a player's: +x to starboard, +y up, -z the bow.
//
// Thrusters push along the craft's axes, in six groups (CRAFT_GROUP_*). A group
// fires as one, and where its thrusters sit decides the torque it makes as well as
// the force: `thrust_torque` is that torque at full throttle, about the centre of
// mass, which a lopsided craft has to fight with its gyros. The flight computer
// (assist, unless the pilot's input is decoupled) turns the pilot's controls into
// target velocities and rates, and the thrust and torque that reach them, gravity
// included; decoupled, the controls are the thrust and torque themselves.
enum craft_group { CRAFT_GROUP_RIGHT, CRAFT_GROUP_LEFT, CRAFT_GROUP_UP, CRAFT_GROUP_DOWN, CRAFT_GROUP_BACK,
                   CRAFT_GROUP_FORWARD, CRAFT_GROUPS };   // +x, -x, +y, -y, +z, -z
struct Craft {
    char name[GAME_NAME_CAPACITY];
    int design;                  // its row in the designs table, for the client's look and the yard
    v3 cabin_center, cabin_half; // the space over the deck, in its frame: feet in it are aboard
    float radius;                // of a sphere about the origin holding every part
    float draft;                 // how far its lowest part reaches below the origin
    v3 helm;                     // where the pilot stands, in its frame
    cw_entity_id pilot;          // at the helm, or CW_INVALID_ENTITY
    int berth;                   // the yard berth it is kept at (game.berths), or -1
    // Motion. The velocity is the centre of mass's; spin is about it, in the craft's own axes.
    quat orientation, previous_orientation;
    v3 velocity, spin;
    bool resting;                // landed and still: not integrated until something wakes it
    int quiet_ticks;             // ticks in a row it has been nearly still on the ground
    bool assist;                 // as the pilot last flew it
    float throttle[CRAFT_GROUPS];    // what each group fired at last tick, 0..1
    // What its parts make of it (game_craft_refit).
    float mass;                  // kg
    v3 center;                   // of mass, in its frame
    v3 inertia;                  // kg m^2 about the centre of mass, along its axes (principal moments)
    float thrust[CRAFT_GROUPS];  // newtons per group at full throttle
    v3 thrust_torque[CRAFT_GROUPS];
    v3 gyro;                     // N m its gyros can apply about each axis
    // What its flight computer aims for, from the design.
    float top_speed[CRAFT_GROUPS];   // m/s along each group's direction, assisted
    v3 top_rate;                 // rad/s about each axis, assisted
};
enum part_kind { PART_HULL, PART_THRUSTER, PART_GYRO, PART_HELM, PART_RIGGING };
struct Part {                    // what a craft part is, beyond its box
    int kind;                    // part_kind
    float mass;                  // kg
    v3 thrust;                   // thrusters: the force it pushes the craft with, in newtons, along one axis
    float torque;                // gyros: N m about every axis
};
// A player standing in a craft moves in its frame: the craft's artificial gravity
// pulls towards its deck, its parts are the only solids, and its turning carries
// them. Their Player velocity, yaw and pitch are in that frame too.
struct Aboard {
    cw_entity_id craft;
    v3 position, previous;       // feet, in the craft's frame
};
#define CRAFT_HELM_REACH 1.2f        // metres from the helm at which it can be taken
#define CRAFT_RESPONSE 0.6f          // seconds the flight computer takes to close a velocity gap
#define CRAFT_TURN_RESPONSE 0.25f    // ...and a rate gap
#define CRAFT_MAX_SPEED 400.0f       // m/s: past this the hull would not hold
#define CRAFT_MAX_SPIN 6.0f          // rad/s
#define CRAFT_RECOVERY_RANGE 8000.0f // metres from the origin past which a craft is towed home...
#define CRAFT_RECOVERY_DEPTH -1500.0f // ...or below which, where it is under everything there is
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
struct Owned {                   // who an entity belongs to: the account whose player or craft it is
    uint64_t account;            // (later: whose loot). Only the owner's client may drive a player, and
};                               // only the owner may take a craft's helm; 0 is nobody's: anyone's
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
#define GAME_MAX_PLACES 16
// PLACE_LANDMARK is a named spot, not a way anywhere: the client lights it and
// points at it, the server ignores it. PLACE_YARD is a shipyard's terminal: a
// player using it there is given their craft at a free berth, or has it towed
// back to its berth (game_call_craft). Neither is a way anywhere.
enum place_kind { PLACE_ZONE, PLACE_ENTRANCE, PLACE_EXIT, PLACE_LANDMARK, PLACE_YARD };
struct game_place {
    char name[GAME_NAME_CAPACITY];
    int kind;                    // place_kind
    v3 center, half;             // the box a player must be inside to be "at" the place
    v3 spawn;
    char destination[GAME_NAME_CAPACITY];
};

// Where a yard keeps a craft: the point its frame's origin sits over, and its heading.
#define GAME_MAX_BERTHS 16
struct game_berth {
    v3 floor;                    // the top of the floor under the berth's middle
    float heading;               // the bow's, in look_direction's sense
};

struct game {
    cw_world world;
    cw_sim sim;
    cw_sim_role role;
    cw_component_id c_transform, c_solid, c_attachment, c_visual, c_input, c_player, c_ship, c_planet;
    cw_component_id c_equipment, c_loot, c_owned;
    cw_component_id c_craft, c_part, c_aboard;
    cw_cached_query *q_solids, *q_players, *q_ships, *q_attachments, *q_planets;
    cw_cached_query *q_all_players;   // every player, with or without an input
    cw_cached_query *q_loot;
    cw_cached_query *q_crafts;
    // Broadphase for the solids: rebuilt every tick, so that a player's collision
    // test touches the few solids around it rather than every one in the world.
    // A craft's parts are not in it: they turn with the craft, so they are tested
    // in its frame instead, from the list of each craft's parts beside it.
    struct { int32_t key; cw_entity_id id; } *grid;
    int grid_count, grid_capacity;
    struct { cw_entity_id craft, part; } *craft_parts;   // sorted by craft
    int craft_part_count, craft_part_capacity;
    game_berth berths[GAME_MAX_BERTHS];
    int berth_count;
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

// ---------------------------------------------------------------------------
// Crafts (craft.cpp)
// ---------------------------------------------------------------------------
// Designs are data, like planets: a row of parts each, in craft.cpp. Spawning one
// builds the craft and its parts and refits it; it starts at rest with its lowest
// part on `floor`, its bow along `heading`. `owner` 0 lets anyone fly it.
int game_craft_design_count(void);
const char *game_craft_design_name(int design);
cw_entity_id game_spawn_craft(game *g, int design, v3 floor, float heading, uint64_t owner, int berth);
// Works out a craft's mass, centre of mass, inertia, thrust and torque from its parts.
void game_craft_refit(game *g, cw_entity_id craft);
// The thruster group (craft_group) a part fires with, or -1 for a part that is no thruster.
int game_part_group(const Part *part);
// Removes a craft and its parts; whoever is aboard stays where they are, in the world's frame.
void game_despawn_craft(game *g, cw_entity_id craft);
// Puts a craft back at rest on its berth (a craft adrift, or called back), passengers and all.
void game_craft_to_berth(game *g, cw_entity_id craft);
// A player at a yard terminal calls their craft: given one at a free berth if they
// have none, else theirs is towed back to its berth. The host does this (the yard
// system); returns the craft, or CW_INVALID_ENTITY when every berth is taken.
cw_entity_id game_call_craft(game *g, cw_entity_id player);
cw_entity_id game_craft_of(game *g, uint64_t owner);   // the craft an account owns, if any
bool game_can_pilot(game *g, cw_entity_id player, cw_entity_id craft);
// The craft a player is aboard, or CW_INVALID_ENTITY (also when that craft is gone).
cw_entity_id game_aboard(game *g, cw_entity_id player);
// A craft's pose blended between last tick's and this tick's, for drawing.
void game_craft_pose(game *g, cw_entity_id craft, float alpha, v3 *position, quat *orientation);
// A point and a direction in a craft's frame, in the world's, and back (its pose now).
v3 game_craft_to_world(game *g, cw_entity_id craft, v3 local);
v3 game_craft_from_world(game *g, cw_entity_id craft, v3 world);
// The velocity of a point of a craft (in the world), through its motion and its spin.
v3 game_craft_point_velocity(game *g, cw_entity_id craft, v3 world_point);
// Where a player stands in the frame that carries them: a craft (in its own frame),
// a liner (relative to it, unrotated), or nothing (the world). `frame` is that craft
// or liner, or CW_INVALID_ENTITY. The inverse puts such a point back in the world.
v3 game_player_frame(game *g, cw_entity_id player, cw_entity_id *frame);
v3 game_frame_to_world(game *g, cw_entity_id frame, v3 point);
// Where a player's eyes are and where they look, in the world, whatever carries them.
void game_player_view(game *g, cw_entity_id player, v3 *eye, v3 *direction);
// The pull of gravity at a point, m/s^2. Down the world's -y for now, everywhere.
v3 game_gravity(const game *g, v3 position);

// Player size, in metres: position is the centre of the feet.
#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_EYE 1.65f

#endif
