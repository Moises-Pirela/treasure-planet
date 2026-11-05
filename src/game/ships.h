// Ships as a player has them built: a hull of a size, and what is fitted to it.
//
//   loadout (hull + a fitting per slot)  ->  design (parts, layout, body, stats)  ->  a Craft
//
// A hull class sets the size of a ship: how many it is meant to crew, its deck, how
// much it weighs bare, the slots it has for fittings and where they are, and how
// its flight computer handles it. Four classes, from a one-man sloop to a galleon
// for eight; the bigger the hull, the more it carries and the harder it is to turn.
//
// A slot takes one fitting of its kind (an engine, a lift thruster, a gyro, a mast,
// a sail...), chosen from the catalogue. A fitting is a trade: a heavy engine pushes
// harder and weighs more, a tall mast carries more sail higher up, where its push
// heels the ship the more. The hull's mounts scale what is fitted to them: the same
// engine on a galleon is a bigger engine than on a sloop, so a fitting names a kind
// of thing, and the hull its size. Slots are per position, so a ship may be fitted
// lopsided, and will fly lopsided.
//
// Nothing here touches the world: a loadout is data, a design is a pure function
// of it, and the same physics the simulation flies a craft with (its mass, centre
// of mass, inertia, thrust and torque, summed over its parts: ship_body_of) gives
// the stats a shipyard shows. What the numbers are is what the ship does.
#ifndef TP_SHIPS_H
#define TP_SHIPS_H

#include "../ecs/clockwork.h"
#include "../tp_math.h"
#include <stddef.h>

// What a craft part is, beyond its box.
enum part_kind { PART_HULL, PART_THRUSTER, PART_GYRO, PART_HELM, PART_RIGGING, PART_CHART, PART_SAIL, PART_SAIL_STATION };
// The six thruster groups, by the way they push: +x, -x, +y, -y, +z, -z.
enum craft_group { CRAFT_GROUP_RIGHT, CRAFT_GROUP_LEFT, CRAFT_GROUP_UP, CRAFT_GROUP_DOWN, CRAFT_GROUP_BACK,
                   CRAFT_GROUP_FORWARD, CRAFT_GROUPS };

enum hull_class { HULL_SLOOP, HULL_CUTTER, HULL_BRIGANTINE, HULL_GALLEON, HULL_COUNT };
// Where a yard can keep a hull: a small berth takes a sloop or a cutter, a large one anything.
enum berth_size { BERTH_SMALL, BERTH_LARGE };
enum slot_kind {
    SLOT_MAIN,                   // main engines, astern, driving her ahead
    SLOT_RETRO,                  // at the bow, driving her astern
    SLOT_LIFT,                   // under the hull, lifting
    SLOT_DOWN,                   // on the rails, pressing down
    SLOT_LATERAL,                // on either side, pushing across
    SLOT_GYRO,                   // in the hull, turning her
    SLOT_MAST,                   // how tall each mast is, and so how much sail it carries
    SLOT_SAIL,                   // the cloth on each mast
    SLOT_KINDS
};

#define SHIP_MAX_SLOTS 48
#define SHIP_MAX_MASTS 3
#define SHIP_MAX_PARTS 112
#define SHIP_CATALOGUE_MAX 32    // fittings the catalogue may hold (a player's store counts each)
#define CRAFT_SAIL_PUSH 6.0f     // newtons per square metre of sail (drive 1) per (m/s)^2 short of its sailing speed
#define SHIP_NOTHING 0           // the fitting that is none: an empty slot (not for masts and sails)

struct hull_spec {
    const char *name;
    const char *note;            // a line for the shipyard
    int crew;                    // the complement she is meant for
    int berth;                   // berth_size
    float beam, length;          // half the deck's width and length, metres
    float depth;                 // the deck's thickness
    float skids;                 // how far the skids reach below it
    float structure;             // kg of deck; the prow, skids and rails are reckoned from it
    float mount;                 // fittings here are this many times a sloop's (thrust, torque, mass)
    int slots[SLOT_KINDS];       // how many of each kind (sails: one per mast, as masts)
    float mast_z[SHIP_MAX_MASTS];    // where each mast stands along her, bow first
    float top_speed[CRAFT_GROUPS];   // m/s the flight computer aims for, as fitted as standard
    v3 top_rate;                 // rad/s about pitch, yaw and roll, likewise
    float response;              // seconds the flight computer takes to close a velocity gap...
    float turn_response;         // ...and a rate gap: the bigger the hull, the slower it answers
    float hull_drag;             // N per (m/s)^2 of current its hull is dragged by
    int price;                   // doubloons, fitted as standard
};

struct ship_component {
    const char *name;
    const char *note;
    int slot;                    // the slot_kind it fits (SHIP_NOTHING fits all but masts and sails)
    float mass;                  // kg on a sloop's mount; sails: kg per square metre of cloth
    float power;                 // thrusters: N; gyros: N m; masts: metres tall on a sloop; sails: drive (canvas 1)
    float sharpness;             // sails: how closely it must be braced to draw (canvas 1; higher, less forgiving)
    v3 half;                     // its box on a sloop's mount; masts: the spar's half thickness in x and z
    v3 color;
    int price;                   // doubloons, whatever the hull: the yard builds it to the mount's size
};

// How a ship looks, beside what she is: paint for her hull, her trim (prow, rails,
// stairs, and the plates and stripes a shape adds), her thrusters and her sails;
// the cut of her sails; and the shape of her hull. None of it changes how she
// flies: a shape's extra timber and iron is trim, and weighs nothing in the
// reckoning (a game's allowance), and a sail draws by its cloth, whatever its cut.
enum look_field { LOOK_HULL_PAINT, LOOK_TRIM_PAINT, LOOK_THRUSTER_PAINT, LOOK_SAIL_PAINT, LOOK_SAIL_CUT, LOOK_HULL_SHAPE, LOOK_FIELDS };
enum sail_cut {
    CUT_SQUARE,                  // a square sail, hung from its yard
    CUT_LATEEN,                  // a triangle from the length of the yard down to a point
    CUT_JUNK,                    // battened panels, fanning wider towards the foot
    CUT_SOLAR,                   // a solar sail: a glowing panel with ribbed wings either side
    CUT_COUNT
};
enum hull_shape {
    SHAPE_CLASSIC,               // as she was drawn first
    SHAPE_CLIPPER,               // a long, stepped, pointed prow and a bowsprit; a stripe along her
    SHAPE_IRONCLAD,              // armour plate down both sides, and a ram under her prow
    SHAPE_ROYAL,                 // gilded rail caps, a raised and carved stern with lanterns, a figurehead
    SHAPE_COUNT
};
// Which paint a part takes: none (it keeps its own colour), or one of the look's.
enum paint_role { PAINT_FIXED, PAINT_HULL, PAINT_TRIM, PAINT_THRUSTER, PAINT_SAIL };
#define SHIP_PAINT_AS_BUILT 0    // the paint that is none: each part its own colour
struct ship_look {
    uint8_t field[LOOK_FIELDS];  // a paint (ship_paint_*), a sail_cut, a hull_shape; zero is as built
};

// Everything a player chooses about a ship. `fit[i]` is the catalogue index of what
// is in slot i of the hull's slots (ship_slots); the rest stay SHIP_NOTHING.
struct ship_loadout {
    uint8_t hull;                // hull_class
    uint8_t fit[SHIP_MAX_SLOTS];
    ship_look look;
};

struct ship_slot {
    int kind;                    // slot_kind
    v3 mount;                    // where a fitting sits, in the ship's frame...
    v3 out;                      // ...and which way from there its box grows (unit, or zero: centred)
    v3 push;                     // thrusters: the way they push the ship (unit)
    int mast;                    // masts and sails: which, counted from the bow; else -1
};

// A part as a design lists it: what becomes a part entity of a craft.
struct ship_part {
    int kind;                    // part_kind
    v3 offset, half, color;      // colour: as painted (ship_part_color)
    int paint;                   // paint_role
    v3 base;                     // its colour as built, which a paint of "as built" leaves it
    float mass;                  // kg
    v3 thrust;                   // thrusters: N along one of the ship's axes
    float torque;                // gyros: N m about every axis
    int mast;                    // sails, rigging, sail stations: which mast; else -1
    int component;               // the catalogue index it was fitted from, or SHIP_NOTHING for structure
};

// What the parts make of a ship: the numbers it flies by.
struct ship_sail_body {
    float area;                  // m^2
    v3 center;                   // where the current pushes it, in the ship's frame
    float drive, sharpness;      // from its cloth
};
struct ship_body {
    float mass;                  // kg
    v3 center;                   // of mass, in the ship's frame
    v3 inertia;                  // kg m^2 about the centre of mass, along its axes
    float radius;                // of a sphere about the origin holding every part
    float draft;                 // how far its lowest part reaches below the origin
    float thrust[CRAFT_GROUPS];  // N per group at full throttle
    v3 thrust_torque[CRAFT_GROUPS];
    v3 gyro;                     // N m about each axis
    int masts;
    ship_sail_body sails[SHIP_MAX_MASTS];
};

// A shipyard's reading of a loadout, all from its body: what it weighs, what it can
// push and turn with, and what its flight computer makes of that.
struct ship_stats {
    int hull, crew, masts;
    float mass;                  // kg
    float accel[CRAFT_GROUPS];   // m/s^2 each thruster group gives at full throttle
    float lift_ratio;            // lift over weight at the gravity given: under 1 she cannot rise
    v3 turn_accel;               // rad/s^2 her gyros turn her by, about pitch, yaw and roll
    float top_speed[CRAFT_GROUPS];   // m/s the computer aims for: the hull's, for how her thrust compares with standard
    v3 top_rate;                 // rad/s, likewise for her turning
    float response, turn_response;   // seconds, the hull's
    float sail_area;             // m^2, every mast
    float sail_power;            // N per (m/s)^2 short of her sailing speed, all sail set and drawing
    float sail_accel;            // m/s^2 that gives her 10 m/s short of it
    float heel;                  // rad/s^2 the same push pitches her by, before her gyros answer it
    float hull_drag;             // N per (m/s)^2 the current drags her hull by
};

// A design: a loadout built. Its parts, where people stand on it, and its body and stats.
struct ship_design {
    ship_loadout loadout;
    ship_part parts[SHIP_MAX_PARTS];
    int part_count;
    v3 cabin_center, cabin_half; // the space over the deck: feet in it are aboard
    v3 helm;                     // where the pilot stands
    int masts;
    v3 stations[SHIP_MAX_MASTS]; // where each mast's sail hand stands
    ship_body body;
    ship_stats stats;
};

const hull_spec *ship_hull(int hull);                    // NULL past the last
int ship_hull_price(int hull);                           // doubloons; 0 for no hull
int ship_component_price(int component);                 // doubloons; 0 for nothing
int ship_hull_find(const char *name);                    // case aside; -1 when there is none
const char *ship_mast_name(int masts, int mast);         // "MAST"; "FOREMAST", "MAINMAST", "MIZZEN"
int ship_component_count(void);
const ship_component *ship_component_info(int component);   // NULL past the last
int ship_component_find(const char *name);               // case aside; -1 when there is none
const char *ship_slot_name(int kind);
// The hull's slots, in the order a loadout's `fit` follows; returns how many (at most `capacity`).
int ship_slots(int hull, ship_slot *out, int capacity);
int ship_slot_count(int hull);
// What the shipwright calls a slot: "LIFT 3, FORE PORT", "SAIL, MAINMAST".
void ship_slot_label(int hull, int index, char *out, size_t capacity);
// Whether a component may go in a slot of a kind.
bool ship_fits(int component, int slot_kind);
// Every slot with its kind's standard fitting (the first in the catalogue for it).
ship_loadout ship_default_loadout(int hull);
// Whether a loadout is one that can be built: a hull there is, every slot it has
// holding something that fits it (masts and sails never empty), the rest empty.
bool ship_loadout_valid(const ship_loadout *loadout);
bool ship_loadout_equal(const ship_loadout *a, const ship_loadout *b);
// Fits `component` to every slot of a kind (the shipyard's "all the same"); false when it does not fit.
bool ship_fit_all(ship_loadout *loadout, int slot_kind, int component);

// Builds a design from a loadout; an invalid loadout is refused. `gravity` is the
// pull the stats quote the lift against, m/s^2.
bool ship_design_build(const ship_loadout *loadout, float gravity, ship_design *out);
// The body a set of parts makes: the same sums a craft's refit does.
void ship_body_of(const ship_part *parts, int count, ship_body *out);
// The thruster group a thrust falls in: the axis it mostly pushes along, and which way; -1 for none.
int ship_thrust_group(v3 thrust);

// The look: how many choices a field has, what they are called, what the
// shipwright asks for one, and what a paint is.
const char *ship_look_field_name(int field);             // "HULL PAINT"
int ship_look_options(int field);
const char *ship_look_option_name(int field, int value); // "Navy Blue", "Lateen", "Clipper"
const char *ship_look_option_note(int field, int value); // a line for the shipwright ("" for paints)
int ship_look_price(int field, int value);               // doubloons
bool ship_paint_color(int paint, v3 *out);               // false for as built, and past the last
bool ship_look_valid(const ship_look *look);
// A part's colour under a look: its role's paint, or its colour as built.
v3 ship_part_color(int paint_role, const ship_look *look, v3 base);

// On the wire: the hull, how many slots follow, a byte each; then how many look
// fields follow, a byte each.
void ship_loadout_write(cw_writer *w, const ship_loadout *loadout);
bool ship_loadout_read(cw_reader *r, ship_loadout *loadout);   // false for one that is not valid
// ...as it was before ships had looks (saved characters of that time): slots alone.
bool ship_loadout_read_plain(cw_reader *r, ship_loadout *loadout);

#endif
