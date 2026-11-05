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
#include "ships.h"

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
    bool landing;                // landing mode (assisted whatever `decoupled` says)
    float sail;                  // -1..1: take the sail in, or hoist it (at the sails, held)
    float brace;                 // -1..1: turn the sail to port, or to starboard (at the sails, held)
    bool use;                    // an edge, like act: take or leave a helm, call a craft at a yard terminal
    bool dock;                   // an edge: at a helm, flying, dock at the free dock in reach, or call a docking off
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
// It is built from a loadout (ships.h): a hull of a size, and a fitting in each of
// its slots. It is a rigid body. Its parts are entities of their own (an Attachment
// to it, a Solid and a Visual, and a Part saying what the part does), boxes in its
// frame; its mass, centre of mass, inertia, thrust and torque all come from them
// (game_craft_refit, the same sums as ship_body_of), so a craft built differently
// flies differently. Its pose is the Transform's position (the origin of its frame:
// the middle of the deck's top) and `orientation`; its axes are a player's: +x to
// starboard, +y up, -z the bow.
//
// Thrusters push along the craft's axes, in six groups (CRAFT_GROUP_*). A group
// fires as one, and where its thrusters sit decides the torque it makes as well as
// the force: `thrust_torque` is that torque at full throttle, about the centre of
// mass, which a lopsided craft has to fight with its gyros. The flight computer
// (assist, unless the pilot's input is decoupled) turns the pilot's controls into
// target velocities and rates, and the thrust and torque that reach them, gravity
// included; decoupled, the controls are the thrust and torque themselves. How fast
// it closes a gap (`response`, `turn_response`) is the hull's: a galleon's computer
// asks less of it, and later, than a sloop's.
//
// Sails and the solar current (game_solar_current, which flows everywhere). The
// current drags on the hull towards its own flow (`hull_drag`, the hull's): across
// the bow always (a keel, the flight computer's thrusters, holds against that), and
// along the bow as far as the sails are not set. So a craft with its sails in and
// no engines working (decoupled, or nobody at the helm) drifts with the current,
// astern if it faces into it.
// Each mast carries a sail (a part, PART_SAIL), worked from that mast's sail station
// (PART_SAIL_STATION) by whoever stands there (its `hand`): how far it is set (`set`,
// 0 furled to 1 full) and how it is braced about the mast (`brace`). A sail drives
// the craft along its bow towards a sailing speed: the current's own running before
// it, a little less across it, and four tenths of it beating straight into it; each
// times how well the sail is trimmed, the cosine of its brace's error from the best
// brace for that current (about half the angle between the bow and the way the
// current flows), raised to its cloth's `sharpness`, so a sail braced badly draws
// little and one braced against the flow is aback and drives the craft astern. The
// drive is the difference from that speed, squared, times the sail's area, set and
// cloth's `drive`, acting at the sail, high up, so it turns the craft too. Under
// sail the flight computer leaves the bow to the sails (unless the pilot motors
// with W or S) and keeps everything else: height, heading, trim (level in pitch and
// roll, near a planet), no sideways slip. Left so with nobody at the helm, the craft
// sails on; with the sails in and nobody at the helm it powers down and drifts. In
// landing mode the computer holds the craft against it all.
//
// Near a planet the computer keeps the craft the right way up for it. In FLIGHT
// mode the wings come level with the local horizon whenever the pilot lets go of
// the roll; pitch and yaw are the pilot's. In LANDING mode (always assisted) the
// craft is held level with the ground under it: the stick turns it about the way
// up, the thrust keys move it in a frame level with the ground, at landing speeds,
// and its descent slows as the ground comes up. Touching down with nothing asked
// of it but to stay or go down, it powers down and comes to rest, and stays so
// until it is asked to move.
enum craft_mode { CRAFT_FLIGHT, CRAFT_LANDING };
#define CRAFT_MAX_MASTS SHIP_MAX_MASTS
struct craft_sail {
    float set;                   // how far it is set: 0 furled, 1 full
    float brace;                 // radians it is turned about the mast, to starboard positive
    float fill;                  // -1..1: how well it draws (aback when negative), for all to see
    cw_entity_id hand;           // at its sail station, or CW_INVALID_ENTITY
    v3 station;                  // where its hand stands, in the craft's frame
    // From the parts (game_craft_refit):
    float area;                  // m^2
    v3 center;                   // where the current pushes it, in the craft's frame
    float drive, sharpness;      // its cloth's
};
struct Craft {
    char name[GAME_NAME_CAPACITY];
    ship_loadout loadout;        // what it was built as: its hull and every slot's fitting
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
    int mode;                    // craft_mode, as the pilot last flew it
    int masts;                   // how many sails it has, one to a mast, bow first
    craft_sail sails[CRAFT_MAX_MASTS];
    float throttle[CRAFT_GROUPS];    // what each group fired at last tick, 0..1
    // What its parts make of it (game_craft_refit).
    float mass;                  // kg
    v3 center;                   // of mass, in its frame
    v3 inertia;                  // kg m^2 about the centre of mass, along its axes (principal moments)
    float thrust[CRAFT_GROUPS];  // newtons per group at full throttle
    v3 thrust_torque[CRAFT_GROUPS];
    v3 gyro;                     // N m its gyros can apply about each axis
    // What its flight computer aims for, and how quickly, from its design (ship_stats).
    float top_speed[CRAFT_GROUPS];   // m/s along each group's direction, assisted
    v3 top_rate;                 // rad/s about each axis, assisted
    float response;              // seconds to close a velocity gap
    float turn_response;         // ...and a rate gap
    float hull_drag;             // N per (m/s)^2 of current across its hull
    // Docking (docks, below): the dock it is making for or lies at, and how far along.
    cw_entity_id dock;           // or CW_INVALID_ENTITY
    int docking;                 // dock_state
};
struct Part {                    // what a craft part is, beyond its box
    int kind;                    // part_kind (ships.h)
    float mass;                  // kg
    v3 thrust;                   // thrusters: the force it pushes the craft with, in newtons, along one axis
    float torque;                // gyros: N m about every axis
    int mast;                    // sails, rigging, sail stations: which mast, bow first; else -1
    int component;               // the catalogue's fitting it is (ships.h), or SHIP_NOTHING for structure
    int paint;                   // paint_role: which of its craft's paints it takes (ships.h)
    v3 base;                     // its colour as built, under a paint of "as built" (its Visual has the paint)
};
// A dock: a place on a dock deck where a craft may be brought in and laid up, at
// the hub and by every planet's pad. Its Transform is the middle of its floor; a
// docked craft lies with its bow along `forward`. A pilot flying within
// CRAFT_DOCK_REACH of a free dock that takes their hull asks to dock
// (PlayerInput.dock): the craft's own autopilot (the flight computer in landing
// mode, given its inputs) brings it to a point CRAFT_DOCK_HOVER above the dock,
// turns it to the dock's heading, and lets it down onto it, where it powers down
// and rests, docked. Asking again while it is at work, or a touch on the controls,
// calls it off; leaving the helm does not (she docks herself), and asking her to
// move takes her out of the dock. The autopilot is a pure function of the craft and
// the dock, so a pilot's client predicts it, and it goes on with nobody at the helm.
struct Dock {
    char name[GAME_NAME_CAPACITY];
    int size;                    // berth_size: the hulls it takes
    v3 forward;                  // the way a docked craft's bow points: level, unit
};
enum dock_state { DOCK_NONE, DOCK_APPROACH, DOCK_DESCEND, DOCK_DOCKED };
#define CRAFT_DOCK_REACH 120.0f      // metres from a dock at which a pilot may ask to dock
#define CRAFT_DOCK_HOVER 12.0f       // metres over it the approach is made at
// A captain: a player of the world's own, not anyone's client, who owns a ship and
// sails her from dock to dock, about the hub and the planets, at the helm as any
// pilot is (captains.cpp). The host's alone; everyone else sees a player flying.
enum captain_state { CAPTAIN_BOARDING, CAPTAIN_IN_PORT, CAPTAIN_CLIMBING, CAPTAIN_CRUISING, CAPTAIN_DOCKING };
struct Captain {
    cw_entity_id craft;
    int state;                   // captain_state
    float timer;                 // seconds in port still, or in the state
    cw_entity_id destination;    // the dock she is bound for
    uint32_t seed;               // their own dice, so a world with captains stays a pure function of its ticks
    uint16_t voyages;            // docks reached
};
#define CAPTAIN_ACCOUNT_BASE 0xCA97A10000000000ull   // captains' accounts: this, plus which captain
// Who a craft's owner has put off it, and until when they may not come back aboard
// (by account, so a reconnection does not clear it). Replicated: every client
// predicts boarding the same way.
#define CRAFT_BARRED 8
#define CRAFT_BAR_SECONDS 60
struct Barred {
    uint64_t account[CRAFT_BARRED];
    uint64_t until[CRAFT_BARRED];  // the tick the bar lifts; 0: an empty slot
};
// A player standing in a craft moves in its frame: the craft's artificial gravity
// pulls towards its deck, its parts are the only solids, and its turning carries
// them. Their Player velocity, yaw and pitch are in that frame too.
struct Aboard {
    cw_entity_id craft;
    v3 position, previous;       // feet, in the craft's frame
};
#define CRAFT_HELM_REACH 1.2f        // metres from the helm at which it can be taken
#define CRAFT_MAX_SPEED 400.0f       // m/s: past this the hull would not hold
#define CRAFT_MAX_SPIN 6.0f          // rad/s
#define CRAFT_ALIGN_RATE 1.5f         // rad/s the computer turns back to level with, per radian off it
#define CRAFT_ALIGN_GRAVITY 2.0f      // m/s^2 of gravity from which a way up is worth keeping to
#define CRAFT_LANDING_SPEED 10.0f     // m/s across the ground, in landing mode
#define CRAFT_LANDING_CLIMB 8.0f      // m/s up
#define CRAFT_LANDING_SINK 8.0f       // m/s down, well clear of the ground...
#define CRAFT_TOUCHDOWN_SPEED 1.5f    // ...and at it: in between, half the height a second
#define CRAFT_TOUCHDOWN_HEIGHT 0.25f  // metres above the ground that count as down
#define CRAFT_ALTITUDE_RANGE 500.0f   // metres below a craft its altitude is measured to
#define CRAFT_SAIL_RATE 0.5f             // of full sail hoisted or taken in a second
#define CRAFT_SAIL_SAILING 0.01f         // set beyond which a craft is sailing
#define CRAFT_BRACE_MAX 1.4f             // radians the sail turns either way about the mast (80 degrees)
#define CRAFT_BRACE_RATE 0.7f            // ...at this many a second
#define CRAFT_STATION_REACH 1.2f         // metres from the sail station it can be taken
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
// A source of gravity, centred on its Transform: crafts (not walkers, yet: they
// feel the world's down everywhere) fall by the sum of every source (game_gravity).
// RADIAL pulls towards the centre with `strength` at `radius` (a planet's surface)
// and the inverse square of the distance beyond it; DOWN pulls along the world's -y
// with `strength` within `radius` (the hub's artificial gravity). Either fades to
// nothing by `reach`. Where no source reaches, space has no gravity at all.
enum gravity_kind { GRAVITY_DOWN, GRAVITY_RADIAL };
struct Gravity {
    int kind;                    // gravity_kind
    float strength;              // m/s^2
    float radius, reach;         // metres from the centre
};
#define GAME_WELL_REACH 2.5f         // a planet's gravity reaches this many radii from its centre

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
// What the yard builds a player: the loadout of the hull they sail, as their
// character has it (shipwright.h). Set by whoever keeps the character (a server, or
// a client playing alone); the host's alone, never replicated.
struct ShipOrder {
    ship_loadout loadout;
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
#define GAME_MAX_PLACES 24
// PLACE_LANDMARK is a named spot, not a way anywhere: the client lights it and
// points at it, the server ignores it. PLACE_YARD is a shipyard's terminal: a
// player using it there is given their craft at a free berth, or has it towed
// back to its berth (game_call_craft). PLACE_SHIPWRIGHT is the counter of the
// shipwright's stall: a player there may trade (shipwright.h); the client opens
// its screen there, and the server takes orders only from a player standing in
// it. None of the three is a way anywhere.
enum place_kind { PLACE_ZONE, PLACE_ENTRANCE, PLACE_EXIT, PLACE_LANDMARK, PLACE_YARD, PLACE_SHIPWRIGHT };
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
    int size;                    // berth_size: the hulls it takes (ships.h)
};

struct game {
    cw_world world;
    cw_sim sim;
    cw_sim_role role;
    cw_component_id c_transform, c_solid, c_attachment, c_visual, c_input, c_player, c_ship, c_planet;
    cw_component_id c_equipment, c_loot, c_owned;
    cw_component_id c_craft, c_part, c_aboard, c_gravity, c_barred, c_ship_order, c_dock, c_captain;
    cw_cached_query *q_solids, *q_players, *q_ships, *q_attachments, *q_planets;
    cw_cached_query *q_all_players;   // every player, with or without an input
    cw_cached_query *q_loot;
    cw_cached_query *q_crafts, *q_gravity, *q_docks, *q_captains;
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
    ship_loadout yard_order;     // what a yard builds a player who has no craft (a standard sloop, unless told otherwise)
    bool currents;               // whether the solar current flows: off for now (no drift, and sails draw nothing).
                                 // A server says so to its clients (the welcome), so they predict alike
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
// The place of a kind whose box holds a point (a yard terminal, the shipwright's
// counter), or -1.
int game_place_of_kind_at(const game *g, v3 point, int kind);
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
// Spawning a craft builds it from a loadout (ships.h; NULL: a standard sloop, and
// an invalid one is refused) and refits it; it starts at rest with its lowest part
// on `floor`, its bow along `heading`. `owner` 0 lets anyone fly it.
cw_entity_id game_spawn_craft(game *g, const ship_loadout *loadout, v3 floor, float heading, uint64_t owner, int berth);
// A shipyard's reading of a loadout (ship_design_build's stats, against the gravity
// a planet's surface and the hub have); false for a loadout that cannot be built.
bool game_ship_stats(const ship_loadout *loadout, ship_stats *out);
// Works out a craft's mass, centre of mass, inertia, thrust and torque from its parts.
void game_craft_refit(game *g, cw_entity_id craft);
// The thruster group (craft_group) a part fires with, or -1 for a part that is no thruster.
int game_part_group(const Part *part);
// Removes a craft and its parts; whoever is aboard stays where they are, in the world's frame.
void game_despawn_craft(game *g, cw_entity_id craft);
// Puts a craft back at rest on its berth (a craft adrift, or called back), passengers and all.
void game_craft_to_berth(game *g, cw_entity_id craft);
// A player at a yard terminal calls their craft: given one at a free berth that
// takes its hull if they have none, else theirs is towed back to its berth. `order`
// is what to build (NULL: the yard's order, g->yard_order); a player whose craft is
// not what they order, and has nobody aboard, has it broken up and the order built
// instead. The host does this (the yard system); returns the craft, or
// CW_INVALID_ENTITY when no berth that takes it is free.
cw_entity_id game_call_craft(game *g, cw_entity_id player, const ship_loadout *order);
// The shipwright's work on a player's ship, once they have changed what it is to be
// (or come to look at it): built at a free berth if they have none; with nobody
// aboard, broken up and built anew if it is not `order`, and brought round to its
// berth if it is; with crew aboard, left alone (the refit then waits for a call at
// the terminal with nobody aboard). Returns their craft, whichever it is, or
// CW_INVALID_ENTITY when there is none and no berth for one. The host's.
cw_entity_id game_refit_craft(game *g, cw_entity_id player, const ship_loadout *order);
// What the yard builds a player from now on (ShipOrder).
void game_set_ship_order(game *g, cw_entity_id player, const ship_loadout *loadout);
// The berth a craft of a hull would be given: the first free one that takes it,
// small hulls preferring small berths; -1 when there is none.
int game_free_berth(game *g, int hull);
cw_entity_id game_craft_of(game *g, uint64_t owner);   // the craft an account owns, if any
bool game_can_pilot(game *g, cw_entity_id player, cw_entity_id craft);
// Whether a player may work a craft's sails: anyone aboard it may (its crew); the
// owner puts off anyone who should not.
bool game_can_work_sails(game *g, cw_entity_id player, cw_entity_id craft);
// How one of a craft's sails would draw where it is now: the best brace for the
// current there (radians, to starboard positive), and the speed that sail drives it
// towards as it is braced (negative: astern).
void game_craft_sailing(game *g, cw_entity_id craft, int mast, float *best_brace, float *sailing_speed);
// The mast whose sails a player is at, or -1.
int game_craft_station_of(const Craft *c, cw_entity_id player);
// All the sail a craft has set, and how well it draws, as one: each sail's weighed by its area.
void game_craft_sail_totals(const Craft *c, float *set, float *fill);
// Whether a player may come aboard a craft: not while its owner has them barred.
// A barred player meets the space over its deck as a wall.
bool game_may_board(game *g, cw_entity_id player, cw_entity_id craft);
// The owner's word: the player is put off the craft behind its stern (onto the
// ground at the foot of its stairs when it is down, into the air when it is not),
// moving as that point of it was, and barred from it for CRAFT_BAR_SECONDS. A pilot
// put off leaves the helm. The host does this; a client only hears of it.
void game_kick(game *g, cw_entity_id craft, cw_entity_id player);
// Everyone aboard a craft but `keep` (its owner, usually), put off at once; the count.
int game_kick_all(game *g, cw_entity_id craft, cw_entity_id keep);
// Players aboard a craft, into `out` (at most `capacity`); returns how many there are.
int game_craft_crew(game *g, cw_entity_id craft, cw_entity_id *out, int capacity);
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
// A star chart: the universe in a ball. Where its middle is and how far the planets
// and the hub reach from it, for drawing it all at a scale that fits.
void game_chart_fit(game *g, v3 *centre, float *extent);
// The chart tables of a craft (parts of PART_CHART), at most `capacity`; the count.
int game_craft_charts(game *g, cw_entity_id craft, cw_entity_id *out, int capacity);
#define CRAFT_CHART_REACH 1.6f       // metres from a chart table it can be studied from
#define CRAFT_CHART_RADIUS 0.4f      // of the ball the universe is shown in, over the table
// How far along `direction` (unit) from `from` the first solid or land is, up to
// `range` (returned when nothing is met). A craft's parts are not in the way.
float game_raycast(game *g, v3 from, v3 direction, float range);
// Docks. A dock's name, floor (its Transform), heading (in look_direction's sense)
// and berth_size. Whether one is free (no craft but `except` making for or lying at
// it, nor sitting on it), the nearest free dock within reach of a craft that takes
// its hull (of its own size before a larger; CW_INVALID_ENTITY: none), and setting a craft to docking at one (false
// when it may not: too far, taken, too small).
cw_entity_id game_spawn_dock(game *g, const char *name, v3 floor, float heading, int size);
bool game_dock_free(game *g, cw_entity_id dock, cw_entity_id except);
cw_entity_id game_dock_in_reach(game *g, cw_entity_id craft);
// Whether a craft is flying, as docking asks: not resting, and more than a couple
// of metres over whatever is under it (a craft hovering on the ground is not).
bool game_craft_airborne(game *g, cw_entity_id craft);
bool game_craft_start_docking(game *g, cw_entity_id craft, cw_entity_id dock);
// Lays a craft up at a dock at once, at rest, docked (a captain's ship, to begin with).
void game_craft_lay_up(game *g, cw_entity_id craft, cw_entity_id dock);
// Captains (captains.cpp): `count` of them, each with a ship of their own laid up
// at a dock, their looks and hulls rolled from `seed`. The host's.
void game_spawn_captains(game *g, int count, uint32_t seed);
bool game_is_captain_account(game *g, uint64_t account);
// A chase camera for a craft posed at `origin` and `orientation`: behind and above
// it by its size, looking out ahead of its bow, its up the craft's, and pulled in
// towards it when a solid or the land is between the two.
#define CRAFT_CHASE_DISTANCE 2.3f    // radii behind
#define CRAFT_CHASE_HEIGHT 1.4f      // radii above: over the masthead, so the way ahead shows
void game_chase_view(game *g, cw_entity_id craft, v3 origin, quat orientation, v3 *eye, v3 *forward, v3 *up);
// The solar current at a point at a universe time: the flow a sail feels, m/s. It
// flows everywhere, never slower than SOLAR_CURRENT_MIN, mostly across the plane
// of the system; its way and its strength change over a kilometre or two and
// drift over minutes. A pure function, so everyone computes the same flow. Where
// the game's currents are off (game.currents, the default for now) it is still
// and zero everywhere: nothing drifts, and sails draw nothing.
#define SOLAR_CURRENT_MIN 12.0f
#define SOLAR_CURRENT_MAX 35.0f
v3 game_solar_current(const game *g, v3 position, double time);
// ...at a tick's time: where a tick ends, as the ships and crafts move to.
v3 game_solar_current_at_tick(const game *g, v3 position, uint64_t tick);
// The pull of gravity on a craft at a point, m/s^2: every Gravity source's, summed.
v3 game_gravity(game *g, v3 position);
// Metres from a craft's lowest point down to the land or a solid under it, along
// gravity (along its own down where there is none); -1 when nothing is within
// CRAFT_ALTITUDE_RANGE.
float game_craft_altitude(game *g, cw_entity_id craft);
// Its velocity split the way a pilot reads it: along the local way up (the climb)
// and the rest; the way up is gravity's, or the world's +y where there is none.
float game_craft_climb(game *g, cw_entity_id craft);

// Player size, in metres: position is the centre of the feet.
#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_EYE 1.65f

#endif
