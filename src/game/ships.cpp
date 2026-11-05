// Hulls, fittings and what they make of a ship (ships.h). Tables first, then the
// builder that turns a loadout into parts, then the sums that make a body of them.
#include "ships.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define COUNT_OF(a) (int)(sizeof(a) / sizeof((a)[0]))

static const v3 WOOD = {0.45f, 0.3f, 0.18f}, DARK_WOOD = {0.3f, 0.19f, 0.11f}, BRASS = {0.75f, 0.58f, 0.25f};
static const v3 IRON = {0.25f, 0.26f, 0.3f}, COPPER = {0.72f, 0.42f, 0.25f}, PALE_WOOD = {0.62f, 0.48f, 0.3f};
static const v3 SAILCLOTH = {0.9f, 0.86f, 0.74f}, SUNSILK = {0.98f, 0.88f, 0.55f}, STORM_CLOTH = {0.58f, 0.56f, 0.52f};
static const v3 GOLD = {0.85f, 0.65f, 0.22f}, LANTERN = {1.0f, 0.85f, 0.45f}, CREAM = {0.85f, 0.8f, 0.65f};

// ---------------------------------------------------------------------------
// Hulls. A sloop is the measure of everything: its mount is 1, and the catalogue's
// numbers are what a fitting is on it. Each class up is about twice the ship: it
// carries more, but its mass grows faster than its engines do and its length
// faster still, so it gathers way more slowly and turns far more slowly, and its
// flight computer, asking less of it, answers later.
// ---------------------------------------------------------------------------
static const hull_spec HULLS[HULL_COUNT] = {
    {"Sloop", "A one-man ship: light, quick on the helm, forgiving.", 1, BERTH_SMALL,
     2.0f, 5.0f, 0.8f, 0.7f, 4000.0f, 1.0f, {2, 2, 4, 2, 4, 1, 1, 1}, {-0.5f},
     {15.0f, 15.0f, 20.0f, 25.0f, 20.0f, 70.0f}, {0.9f, 0.8f, 1.4f}, 0.6f, 0.25f, 45.0f, 8000},
    {"Cutter", "For two: a longer deck and bigger fittings, a little slower to answer.", 2, BERTH_SMALL,
     2.5f, 7.0f, 0.9f, 0.75f, 8000.0f, 1.6f, {2, 2, 4, 2, 4, 1, 1, 1}, {-1.0f},
     {14.0f, 14.0f, 18.0f, 22.0f, 18.0f, 62.0f}, {0.7f, 0.6f, 1.1f}, 0.8f, 0.35f, 70.0f, 20000},
    {"Brigantine", "For four: two masts, and a long hull to turn.", 4, BERTH_LARGE,
     3.5f, 10.0f, 1.1f, 0.85f, 19000.0f, 2.6f, {2, 2, 6, 4, 6, 2, 2, 2}, {-3.5f, 3.5f},
     {12.0f, 12.0f, 16.0f, 20.0f, 16.0f, 55.0f}, {0.5f, 0.45f, 0.8f}, 1.0f, 0.5f, 115.0f, 60000},
    {"Galleon", "For eight: three masts and a great deal of ship. Hard to hurry, harder to turn.", 8, BERTH_LARGE,
     5.0f, 15.0f, 1.4f, 0.9f, 40000.0f, 2.2f, {4, 4, 12, 4, 8, 3, 3, 3}, {-7.0f, 0.0f, 7.0f},
     {10.0f, 10.0f, 14.0f, 18.0f, 14.0f, 48.0f}, {0.35f, 0.3f, 0.55f}, 1.4f, 0.8f, 205.0f, 150000},
};

// ---------------------------------------------------------------------------
// The catalogue. The first of each kind is its standard fitting. Thrusters' boxes
// are as they sit in their slots (a side jet is thin across, a downdraught flat).
// ---------------------------------------------------------------------------
static const ship_component COMPONENTS[] = {
    {"Nothing", "An empty slot: lighter, and that much less ship.", -1, 0, 0, 0, {}, {}, 0},
    {"Brass Drive", "The standard main engine: steady thrust for its weight.", SLOT_MAIN, 350.0f, 60000.0f, 0,
     {0.35f, 0.35f, 0.35f}, BRASS, 900},
    {"Iron Furnace", "Half as much thrust again, at twice the weight.", SLOT_MAIN, 700.0f, 95000.0f, 0,
     {0.45f, 0.45f, 0.45f}, IRON, 2400},
    {"Dragonfly", "A light engine: less push, and less ship to turn.", SLOT_MAIN, 140.0f, 38000.0f, 0,
     {0.28f, 0.28f, 0.3f}, COPPER, 1800},
    {"Brass Retro", "The standard retro thruster.", SLOT_RETRO, 150.0f, 45000.0f, 0, {0.25f, 0.25f, 0.25f}, BRASS, 400},
    {"Iron Retro", "Stops her quicker, and weighs on her bow.", SLOT_RETRO, 300.0f, 70000.0f, 0, {0.32f, 0.32f, 0.3f}, IRON, 900},
    {"Featherweight Retro", "Barely a brake, barely a weight.", SLOT_RETRO, 60.0f, 25000.0f, 0, {0.2f, 0.2f, 0.2f}, COPPER, 700},
    {"Brass Lifter", "The standard lift thruster.", SLOT_LIFT, 250.0f, 75000.0f, 0, {0.3f, 0.25f, 0.3f}, IRON, 700},
    {"Heavy Lifter", "Climbs like a lark, weighs like an anvil.", SLOT_LIFT, 480.0f, 115000.0f, 0, {0.36f, 0.3f, 0.36f}, DARK_WOOD, 1900},
    {"Light Lifter", "Just enough to hold her up, and not a kilogram more.", SLOT_LIFT, 110.0f, 45000.0f, 0,
     {0.25f, 0.2f, 0.25f}, COPPER, 1200},
    {"Downdraught", "The standard down thruster, on the rails.", SLOT_DOWN, 80.0f, 15000.0f, 0, {0.15f, 0.1f, 0.3f}, IRON, 250},
    {"Iron Downdraught", "Dives hard.", SLOT_DOWN, 140.0f, 25000.0f, 0, {0.18f, 0.12f, 0.36f}, DARK_WOOD, 600},
    {"Light Downdraught", "A nudge towards the ground.", SLOT_DOWN, 40.0f, 8000.0f, 0, {0.12f, 0.08f, 0.25f}, COPPER, 450},
    {"Side Jet", "The standard lateral thruster.", SLOT_LATERAL, 80.0f, 18000.0f, 0, {0.1f, 0.25f, 0.3f}, IRON, 250},
    {"Heavy Side Jet", "Slides her across smartly.", SLOT_LATERAL, 150.0f, 30000.0f, 0, {0.13f, 0.3f, 0.36f}, DARK_WOOD, 600},
    {"Light Side Jet", "Drifts her across.", SLOT_LATERAL, 45.0f, 10000.0f, 0, {0.08f, 0.2f, 0.25f}, COPPER, 450},
    {"Spinning Brass", "The standard gyroscope.", SLOT_GYRO, 500.0f, 260000.0f, 0, {0.4f, 0.3f, 0.4f}, IRON, 1500},
    {"Great Wheel", "Turns her with a will, and is a great weight to carry.", SLOT_GYRO, 1400.0f, 600000.0f, 0,
     {0.6f, 0.35f, 0.6f}, DARK_WOOD, 4200},
    {"Whirligig", "A light gyroscope for a light ship.", SLOT_GYRO, 200.0f, 140000.0f, 0, {0.3f, 0.25f, 0.3f}, COPPER, 2000},
    {"Pine Mast", "The standard mast.", SLOT_MAST, 300.0f, 6.0f, 0, {0.12f, 0, 0.12f}, DARK_WOOD, 600},
    {"Tall Oak", "Half as tall again: more sail, set higher, heeling her harder.", SLOT_MAST, 700.0f, 9.0f, 0,
     {0.16f, 0, 0.16f}, WOOD, 1800},
    {"Stub Mast", "A short spar: little sail, low and steady.", SLOT_MAST, 150.0f, 4.0f, 0, {0.1f, 0, 0.1f}, PALE_WOOD, 400},
    {"Canvas", "The standard sail: honest, forgiving of a brace a little off.", SLOT_SAIL, 4.2f, 1.0f, 1.0f, {}, SAILCLOTH, 300},
    {"Sunsilk", "Light and strong-drawing, and sulks unless braced just so.", SLOT_SAIL, 1.8f, 1.3f, 2.5f, {}, SUNSILK, 2500},
    {"Storm Cloth", "Heavy and dull, but draws near enough however it is braced.", SLOT_SAIL, 9.0f, 0.8f, 0.5f, {},
     STORM_CLOTH, 900},
};

// ---------------------------------------------------------------------------
// Looks: the paints, the cuts of sail and the shapes of hull, and their prices.
// ---------------------------------------------------------------------------
struct paint_row {
    const char *name;
    v3 color;
};
static const paint_row PAINTS[] = {
    {"As Built", {}},
    {"Natural Oak", {0.45f, 0.3f, 0.18f}}, {"Dark Walnut", {0.25f, 0.15f, 0.09f}}, {"Pale Ash", {0.72f, 0.62f, 0.45f}},
    {"Ship Red", {0.55f, 0.1f, 0.08f}},    {"Navy Blue", {0.1f, 0.16f, 0.35f}},    {"Sea Green", {0.12f, 0.38f, 0.3f}},
    {"Bone White", {0.85f, 0.82f, 0.74f}}, {"Pitch Black", {0.06f, 0.06f, 0.07f}}, {"Royal Purple", {0.3f, 0.12f, 0.4f}},
    {"Gold Leaf", {0.85f, 0.65f, 0.22f}},  {"Copper", {0.72f, 0.42f, 0.25f}},      {"Ochre", {0.7f, 0.5f, 0.15f}},
    {"Slate Grey", {0.35f, 0.37f, 0.42f}}, {"Sky Blue", {0.35f, 0.6f, 0.85f}},     {"Ember Orange", {0.9f, 0.4f, 0.1f}},
};
struct style_row {
    const char *name;
    const char *note;
    int price;
};
static const style_row CUTS[CUT_COUNT] = {
    {"Square", "A square sail on its yard: the sailor's sail.", 300},
    {"Lateen", "A triangle from the yard down to a point: rakish.", 900},
    {"Junk", "Battened panels, fanning wider towards the foot.", 1200},
    {"Solar", "A glowing panel with ribbed wings either side, as the old stories have them.", 5000},
};
static const style_row SHAPES[SHAPE_COUNT] = {
    {"Classic", "As she was drawn first: a plain prow and plain rails.", 1500},
    {"Clipper", "A long stepped prow and a bowsprit, and a stripe along her: built to look fast.", 3500},
    {"Ironclad", "Armour plate down both sides, and a ram under her prow.", 5000},
    {"Royal", "Gilded rails, a carved stern raised either side of the gangway with lanterns, and a figurehead.", 7000},
};
static const int PAINT_PRICES[4] = {400, 250, 300, 350};   // hull, trim, thrusters, sails

const char *ship_look_field_name(int field) {
    static const char *const NAMES[LOOK_FIELDS] = {"HULL PAINT", "TRIM PAINT", "THRUSTER PAINT", "SAIL PAINT", "SAIL CUT", "HULL SHAPE"};
    return field >= 0 && field < LOOK_FIELDS ? NAMES[field] : "?";
}

int ship_look_options(int field) {
    if (field >= LOOK_HULL_PAINT && field <= LOOK_SAIL_PAINT) return COUNT_OF(PAINTS);
    if (field == LOOK_SAIL_CUT) return CUT_COUNT;
    if (field == LOOK_HULL_SHAPE) return SHAPE_COUNT;
    return 0;
}

const char *ship_look_option_name(int field, int value) {
    if (value < 0 || value >= ship_look_options(field)) return "?";
    if (field == LOOK_SAIL_CUT) return CUTS[value].name;
    if (field == LOOK_HULL_SHAPE) return SHAPES[value].name;
    return PAINTS[value].name;
}

const char *ship_look_option_note(int field, int value) {
    if (value < 0 || value >= ship_look_options(field)) return "";
    if (field == LOOK_SAIL_CUT) return CUTS[value].note;
    if (field == LOOK_HULL_SHAPE) return SHAPES[value].note;
    return value == SHIP_PAINT_AS_BUILT ? "Each part in its own colour: the wood, the metal, the cloth." : "";
}

int ship_look_price(int field, int value) {
    if (value < 0 || value >= ship_look_options(field)) return 0;
    if (field == LOOK_SAIL_CUT) return CUTS[value].price;
    if (field == LOOK_HULL_SHAPE) return SHAPES[value].price;
    return PAINT_PRICES[field];
}

bool ship_paint_color(int paint, v3 *out) {
    if (paint <= SHIP_PAINT_AS_BUILT || paint >= COUNT_OF(PAINTS)) return false;
    *out = PAINTS[paint].color;
    return true;
}

bool ship_look_valid(const ship_look *look) {
    for (int f = 0; f < LOOK_FIELDS; ++f)
        if (look->field[f] >= ship_look_options(f)) return false;
    return true;
}

v3 ship_part_color(int paint_role, const ship_look *look, v3 base) {
    static const int FIELDS[] = {-1, LOOK_HULL_PAINT, LOOK_TRIM_PAINT, LOOK_THRUSTER_PAINT, LOOK_SAIL_PAINT};
    if (paint_role <= PAINT_FIXED || paint_role > PAINT_SAIL || !look) return base;
    v3 color;
    return ship_paint_color(look->field[FIELDS[paint_role]], &color) ? color : base;
}

const hull_spec *ship_hull(int hull) { return hull >= 0 && hull < HULL_COUNT ? &HULLS[hull] : NULL; }
int ship_hull_price(int hull) { return ship_hull(hull) ? HULLS[hull].price : 0; }
static_assert(sizeof(COMPONENTS) / sizeof(COMPONENTS[0]) <= SHIP_CATALOGUE_MAX, "a store counts every fitting");

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

int ship_hull_find(const char *name) {
    for (int i = 0; name && i < HULL_COUNT; ++i)
        if (same_name(name, HULLS[i].name)) return i;
    return -1;
}

const char *ship_mast_name(int masts, int mast) {
    static const char *const TWO[] = {"FOREMAST", "MAINMAST"}, *const THREE[] = {"FOREMAST", "MAINMAST", "MIZZEN"};
    if (masts <= 1 || mast < 0 || mast >= masts) return "MAST";
    return masts == 2 ? TWO[mast] : THREE[mast < 3 ? mast : 2];
}

int ship_component_count(void) { return COUNT_OF(COMPONENTS); }
int ship_component_price(int component) { return component > 0 && component < COUNT_OF(COMPONENTS) ? COMPONENTS[component].price : 0; }
const ship_component *ship_component_info(int component) {
    return component >= 0 && component < COUNT_OF(COMPONENTS) ? &COMPONENTS[component] : NULL;
}

int ship_component_find(const char *name) {
    for (int i = 0; name && i < COUNT_OF(COMPONENTS); ++i)
        if (same_name(name, COMPONENTS[i].name)) return i;
    return -1;
}

const char *ship_slot_name(int kind) {
    static const char *const NAMES[SLOT_KINDS] = {"main engine", "retro", "lift", "down", "lateral", "gyro", "mast", "sail"};
    return kind >= 0 && kind < SLOT_KINDS ? NAMES[kind] : "?";
}

bool ship_fits(int component, int slot_kind) {
    if (slot_kind < 0 || slot_kind >= SLOT_KINDS) return false;
    if (component == SHIP_NOTHING) return slot_kind != SLOT_MAST && slot_kind != SLOT_SAIL;
    const ship_component *c = ship_component_info(component);
    return c && c->slot == slot_kind;
}

// ---------------------------------------------------------------------------
// Slots: where a hull's fittings go, reckoned from its size.
// ---------------------------------------------------------------------------
// Evenly from -span to span, or the middle for one.
static float spread(int i, int n, float span) { return n <= 1 ? 0.0f : span * (-1.0f + 2.0f * (float)i / (float)(n - 1)); }
static float lerp1(float a, float b, float t) { return a + (b - a) * t; }
// A fitting's linear size on a mount: its mass and thrust go as the mount, its size as the cube root.
static float linear_of(const hull_spec *h) { return cbrtf(h->mount); }

static void add_slot(ship_slot *out, int *n, int capacity, int kind, v3 mount, v3 grow, v3 push, int mast) {
    if (*n >= capacity) return;
    ship_slot s = {kind, mount, grow, push, mast};
    out[(*n)++] = s;
}

// The x of a pair of fittings across the stern or the bow, the outermost first.
static float pair_x(const hull_spec *h, int pair, int pairs, float inner) {
    float outer = h->beam - 0.7f;
    return pairs <= 1 ? outer : lerp1(outer, inner < outer ? inner : outer, (float)pair / (float)(pairs - 1));
}

int ship_slots(int hull, ship_slot *out, int capacity) {
    const hull_spec *h = ship_hull(hull);
    if (!h) return 0;
    float B = h->beam, L = h->length, D = h->depth, lin = linear_of(h);
    int n = 0;
    const v3 none = v3_make(0, 0, 0);
    int pairs = h->slots[SLOT_MAIN] / 2;
    for (int p = 0; p < pairs; ++p)
        for (int side = 0; side < 2; ++side) {
            float x = pair_x(h, p, pairs, 1.3f * lin) * (side ? -1.0f : 1.0f);
            add_slot(out, &n, capacity, SLOT_MAIN, v3_make(x, -0.75f * D, L), v3_make(0, 0, 1), v3_make(0, 0, -1), -1);
        }
    pairs = h->slots[SLOT_RETRO] / 2;
    for (int p = 0; p < pairs; ++p)
        for (int side = 0; side < 2; ++side) {
            // Clear of the prow, which reaches six tenths of the beam out from the keel line.
            float x = pair_x(h, p, pairs, 0.6f * B + 0.4f * lin) * (side ? -1.0f : 1.0f);
            add_slot(out, &n, capacity, SLOT_RETRO, v3_make(x, -0.75f * D, -(L + 0.05f)), v3_make(0, 0, -1), v3_make(0, 0, 1), -1);
        }
    int rows = h->slots[SLOT_LIFT] / 2;
    for (int r = 0; r < rows; ++r)
        for (int side = 0; side < 2; ++side)
            add_slot(out, &n, capacity, SLOT_LIFT, v3_make(0.5f * B * (side ? -1.0f : 1.0f), -D, spread(r, rows, 0.66f * L)),
                     v3_make(0, -1, 0), v3_make(0, 1, 0), -1);
    rows = h->slots[SLOT_DOWN] / 2;
    for (int r = 0; r < rows; ++r)
        for (int side = 0; side < 2; ++side)
            add_slot(out, &n, capacity, SLOT_DOWN, v3_make((B - 0.1f) * (side ? -1.0f : 1.0f), 1.0f, spread(r, rows, 0.5f * L)),
                     v3_make(0, 1, 0), v3_make(0, -1, 0), -1);
    int each = h->slots[SLOT_LATERAL] / 2;
    for (int side = 0; side < 2; ++side)   // the port side's jets push her to starboard, the starboard side's to port
        for (int i = 0; i < each; ++i) {
            float s = side ? 1.0f : -1.0f;
            add_slot(out, &n, capacity, SLOT_LATERAL, v3_make(s * B, -0.5f * D, spread(i, each, each <= 2 ? 0.5f * L : 0.6f * L)),
                     v3_make(s, 0, 0), v3_make(-s, 0, 0), -1);
        }
    for (int i = 0; i < h->slots[SLOT_GYRO]; ++i)
        add_slot(out, &n, capacity, SLOT_GYRO, v3_make(0, -0.625f * D, spread(i, h->slots[SLOT_GYRO], 0.35f * L)), none, none, -1);
    for (int m = 0; m < h->slots[SLOT_MAST] && m < SHIP_MAX_MASTS; ++m)
        add_slot(out, &n, capacity, SLOT_MAST, v3_make(0, 0, h->mast_z[m]), v3_make(0, 1, 0), none, m);
    for (int m = 0; m < h->slots[SLOT_SAIL] && m < SHIP_MAX_MASTS; ++m)
        add_slot(out, &n, capacity, SLOT_SAIL, v3_make(0, 0, h->mast_z[m]), none, none, m);
    return n;
}

void ship_slot_label(int hull, int index, char *out, size_t capacity) {
    static const char *const KINDS[SLOT_KINDS] = {"MAIN ENGINE", "RETRO", "LIFT", "DOWN THRUSTER", "SIDE JET", "GYRO", "MAST", "SAIL"};
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(hull, slots, SHIP_MAX_SLOTS);
    if (!out || capacity == 0) return;
    out[0] = 0;
    if (index < 0 || index >= n) return;
    const ship_slot *s = &slots[index];
    const hull_spec *h = ship_hull(hull);
    int masts = h->slots[SLOT_MAST];
    if (s->kind == SLOT_MAST) {
        snprintf(out, capacity, "%s", ship_mast_name(masts, s->mast));
        return;
    }
    if (s->kind == SLOT_SAIL) {
        if (masts <= 1) snprintf(out, capacity, "SAIL");
        else snprintf(out, capacity, "SAIL, %s", ship_mast_name(masts, s->mast));
        return;
    }
    // Which of its kind (counting from the first), and where: fore or aft when its
    // kind stands at more than one place along her, and which side.
    int number = 0;
    bool along = false;
    for (int i = 0; i < n; ++i) {
        if (slots[i].kind != s->kind) continue;
        if (i <= index) ++number;
        if (fabsf(slots[i].mount.z - s->mount.z) > 0.5f) along = true;
    }
    const char *where = !along ? "" : s->mount.z < -0.15f * h->length ? "FORE " : s->mount.z > 0.15f * h->length ? "AFT " : "MIDSHIPS ";
    const char *side = s->mount.x > 0.05f ? "STARBOARD" : s->mount.x < -0.05f ? "PORT" : "";
    snprintf(out, capacity, "%s %d%s%s%s", KINDS[s->kind], number, where[0] || side[0] ? ", " : "", where, side);
    // A trailing space when there is no side to follow "FORE ".
    size_t length = strlen(out);
    if (length && out[length - 1] == ' ') out[length - 1] = 0;
}

int ship_slot_count(int hull) {
    ship_slot slots[SHIP_MAX_SLOTS];
    return ship_slots(hull, slots, SHIP_MAX_SLOTS);
}

ship_loadout ship_default_loadout(int hull) {
    ship_loadout l = {};
    if (!ship_hull(hull)) hull = HULL_SLOOP;
    l.hull = (uint8_t)hull;
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(hull, slots, SHIP_MAX_SLOTS);
    for (int i = 0; i < n; ++i)
        for (int c = 1; c < COUNT_OF(COMPONENTS); ++c)
            if (COMPONENTS[c].slot == slots[i].kind) {
                l.fit[i] = (uint8_t)c;
                break;
            }
    return l;
}

bool ship_loadout_valid(const ship_loadout *l) {
    if (!l || !ship_hull(l->hull) || !ship_look_valid(&l->look)) return false;
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(l->hull, slots, SHIP_MAX_SLOTS);
    for (int i = 0; i < SHIP_MAX_SLOTS; ++i) {
        if (i >= n) {
            if (l->fit[i] != SHIP_NOTHING) return false;
        } else if (!ship_fits(l->fit[i], slots[i].kind)) {
            return false;
        }
    }
    return true;
}

bool ship_loadout_equal(const ship_loadout *a, const ship_loadout *b) {
    return a->hull == b->hull && memcmp(a->fit, b->fit, sizeof(a->fit)) == 0 && memcmp(&a->look, &b->look, sizeof(a->look)) == 0;
}

bool ship_fit_all(ship_loadout *l, int slot_kind, int component) {
    if (!ship_fits(component, slot_kind)) return false;
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(l->hull, slots, SHIP_MAX_SLOTS);
    for (int i = 0; i < n; ++i)
        if (slots[i].kind == slot_kind) l->fit[i] = (uint8_t)component;
    return true;
}

// ---------------------------------------------------------------------------
// Building: the hull's structure, then each slot's fitting.
// ---------------------------------------------------------------------------
static void add_part(ship_design *d, int kind, v3 offset, v3 half, v3 color, float mass, v3 thrust, float torque, int mast,
                     int component, int paint) {
    if (d->part_count >= SHIP_MAX_PARTS) return;
    ship_part *p = &d->parts[d->part_count++];
    p->kind = kind;
    p->offset = offset;
    p->half = half;
    p->color = p->base = color;
    p->paint = paint;
    p->mass = mass;
    p->thrust = thrust;
    p->torque = torque;
    p->mast = mast;
    p->component = component;
}

static void add_structure(ship_design *d, v3 offset, v3 half, v3 color, float mass, int paint) {
    add_part(d, PART_HULL, offset, half, color, mass, v3_make(0, 0, 0), 0.0f, -1, SHIP_NOTHING, paint);
}
// Trim a shape adds: it weighs nothing in the reckoning.
static void add_trim(ship_design *d, v3 offset, v3 half, v3 color, int paint) { add_structure(d, offset, half, color, 0.0f, paint); }

// What a hull shape adds to her, all of it trim that weighs nothing: kept clear of
// the gangway and the stairs, and above the skids, so she boards and sits as any.
static void build_shape(ship_design *d, const hull_spec *h, int shape, v3 prow) {
    float B = h->beam, L = h->length, D = h->depth, lin = linear_of(h);
    float tip = L + 2.0f * prow.z;   // how far forward the prow reaches
    float prow_y = -0.375f * D;
    switch (shape) {
    case SHAPE_CLIPPER: {
        // The prow carried on in two narrowing steps, rising a little, and a
        // bowsprit beyond them; a stripe along each side.
        v3 second = v3_make(0.55f * prow.x, 0.8f * prow.y, 0.8f * prow.z), third = v3_make(0.25f * prow.x, 0.6f * prow.y, 0.7f * prow.z);
        add_trim(d, v3_make(0, prow_y + 0.1f * prow.y, -(tip + second.z)), second, DARK_WOOD, PAINT_TRIM);
        float end = tip + 2.0f * second.z;
        add_trim(d, v3_make(0, prow_y + 0.25f * prow.y, -(end + third.z)), third, DARK_WOOD, PAINT_TRIM);
        end += 2.0f * third.z;
        add_trim(d, v3_make(0, prow_y + prow.y + 0.08f * lin, -(end + 0.6f * lin)), v3_make(0.07f * lin, 0.07f * lin, 1.4f * lin), DARK_WOOD,
                 PAINT_FIXED);
        for (int side = -1; side <= 1; side += 2)
            add_trim(d, v3_make((B + 0.02f) * (float)side, -0.3f * D, 0), v3_make(0.02f, 0.08f, 0.95f * L), CREAM, PAINT_TRIM);
        break;
    }
    case SHAPE_IRONCLAD: {
        // Plate from under her deck to just below her rail tops, both sides, and a ram.
        float top = 0.9f, bottom = -D;
        for (int side = -1; side <= 1; side += 2)
            add_trim(d, v3_make((B + 0.05f) * (float)side, 0.5f * (top + bottom), 0), v3_make(0.05f, 0.5f * (top - bottom), 0.92f * L), IRON,
                     PAINT_TRIM);
        add_trim(d, v3_make(0, -0.8f * D, -(tip + 0.6f * lin)), v3_make(0.35f * prow.x, 0.25f * D, 0.6f * lin), IRON, PAINT_TRIM);
        add_trim(d, v3_make(0, prow_y + prow.y + 0.05f, -(L + prow.z)), v3_make(prow.x + 0.04f, 0.05f, prow.z + 0.04f), IRON, PAINT_TRIM);
        break;
    }
    case SHAPE_ROYAL: {
        // Gilt: caps on her rails, a stripe along each side; her stern carved and
        // raised either side of the gangway, with lanterns; a figurehead at her prow.
        for (int side = -1; side <= 1; side += 2) {
            add_trim(d, v3_make((B - 0.1f) * (float)side, 1.04f, 0), v3_make(0.13f, 0.04f, L), GOLD, PAINT_FIXED);
            add_trim(d, v3_make((B + 0.02f) * (float)side, -0.25f * D, 0), v3_make(0.02f, 0.06f, 0.95f * L), GOLD, PAINT_FIXED);
            add_trim(d, v3_make((1.0f + 0.5f * (B - 1.0f)) * (float)side, 1.5f, L - 0.1f), v3_make(0.5f * (B - 1.0f), 0.5f, 0.12f), DARK_WOOD,
                     PAINT_TRIM);
            add_trim(d, v3_make((B - 0.25f) * (float)side, 2.35f, L - 0.1f), v3_make(0.15f, 0.22f, 0.15f), LANTERN, PAINT_FIXED);
        }
        add_trim(d, v3_make(0, 1.04f, -(L - 0.1f)), v3_make(B - 0.2f, 0.04f, 0.13f), GOLD, PAINT_FIXED);
        add_trim(d, v3_make(0, prow_y + prow.y + 0.2f, -(tip + 0.25f)), v3_make(0.2f, 0.45f, 0.25f), GOLD, PAINT_FIXED);
        break;
    }
    default:
        break;
    }
}

// The deck on its skids, its prow and rails, the stairs down from the gangway in
// the stern rail, the helm forward, and the star chart's table to port; then
// whatever the hull's shape adds.
static void build_structure(ship_design *d, const hull_spec *h, int shape) {
    float B = h->beam, L = h->length, D = h->depth, lin = linear_of(h), S = h->structure;
    v3 none = v3_make(0, 0, 0);
    add_structure(d, v3_make(0, -0.5f * D, 0), v3_make(B, 0.5f * D, L), WOOD, S, PAINT_HULL);
    // The prow: as dense as the deck, and a little more (it is solid timber).
    v3 prow = v3_make(0.6f * B, 0.625f * D, 0.6f * lin);
    add_structure(d, v3_make(0, -0.375f * D, -(L + prow.z)), prow, DARK_WOOD, 1.67f * S * (prow.x * prow.y * prow.z) / (B * 0.5f * D * L),
                  PAINT_TRIM);
    for (int side = -1; side <= 1; side += 2) {
        add_structure(d, v3_make(0.8f * B * (float)side, -(D + 0.5f * h->skids), 0), v3_make(0.15f * lin, 0.5f * h->skids, 0.84f * L),
                      IRON, 0.075f * S, PAINT_FIXED);
        add_structure(d, v3_make((B - 0.1f) * (float)side, 0.5f, 0), v3_make(0.1f, 0.5f, L), DARK_WOOD, 30.0f * L, PAINT_TRIM);
    }
    add_structure(d, v3_make(0, 0.5f, -(L - 0.1f)), v3_make(B - 0.2f, 0.5f, 0.1f), DARK_WOOD, 44.0f * (B - 0.2f), PAINT_TRIM);
    for (int side = -1; side <= 1; side += 2)   // beside the gangway, two metres wide
        add_structure(d, v3_make((1.0f + 0.5f * (B - 1.0f)) * (float)side, 0.5f, L - 0.1f), v3_make(0.5f * (B - 1.0f), 0.5f, 0.1f),
                      DARK_WOOD, 40.0f * (B - 1.0f), PAINT_TRIM);
    // Stairs from the deck to the ground the skids stand on, no step over 0.3 m.
    float draft = D + h->skids;
    int steps = (int)ceilf(draft / 0.3f - 0.01f) - 1;
    float rise = draft / (float)(steps + 1);
    for (int k = 0; k < steps; ++k) {
        float top = -rise * (float)(k + 1), bottom = -draft;
        add_structure(d, v3_make(0, 0.5f * (top + bottom), L + 0.225f + 0.45f * (float)k), v3_make(0.55f, 0.5f * (top - bottom), 0.225f),
                      DARK_WOOD, 40.0f, PAINT_TRIM);
    }
    // The helm's pedestal, below the pilot's eye line.
    add_part(d, PART_HELM, v3_make(0, 0.45f, -(L - 1.3f)), v3_make(0.3f, 0.45f, 0.12f), BRASS, 80.0f, none, 0.0f, -1, SHIP_NOTHING, PAINT_FIXED);
    d->helm = v3_make(0, 0, -(L - 2.0f));
    add_part(d, PART_CHART, v3_make(-(B - 0.75f), 0.45f, 0.32f * L), v3_make(0.45f, 0.45f, 0.6f), DARK_WOOD, 120.0f, none, 0.0f, -1,
             SHIP_NOTHING, PAINT_TRIM);
    d->cabin_center = v3_make(0, 4.0f, 0);
    d->cabin_half = v3_make(B, 4.2f, L);
    build_shape(d, h, shape, prow);
}

// A fitting's box centred out from its mount along the way it grows.
static v3 mounted(const ship_slot *s, v3 half) {
    return v3_add(s->mount, v3_make(s->out.x * half.x, s->out.y * half.y, s->out.z * half.z));
}

// Every part of a loadout: the structure, then each slot's fitting (masts before
// sails: a sail hangs from its mast's head).
static void build_parts(const ship_loadout *loadout, ship_design *d) {
    const hull_spec *h = ship_hull(loadout->hull);
    build_structure(d, h, loadout->look.field[LOOK_HULL_SHAPE]);
    float lin = linear_of(h);
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(loadout->hull, slots, SHIP_MAX_SLOTS);
    float mast_height[SHIP_MAX_MASTS] = {}, mast_thickness[SHIP_MAX_MASTS] = {};
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i) {
            const ship_slot *s = &slots[i];
            int fit = loadout->fit[i];
            const ship_component *c = ship_component_info(fit);
            if (fit == SHIP_NOTHING || !c || (s->kind == SLOT_SAIL) != (pass == 1)) continue;
            switch (s->kind) {
            case SLOT_MAST: {
                float height = c->power * lin, thick = c->half.x * lin;
                mast_height[s->mast] = height;
                mast_thickness[s->mast] = thick;
                add_part(d, PART_RIGGING, v3_add(s->mount, v3_make(0, 0.5f * height, 0)), v3_make(thick, 0.5f * height, thick), c->color,
                         c->mass * h->mount, v3_make(0, 0, 0), 0.0f, s->mast, fit, PAINT_FIXED);
                // Its pinrail, to starboard of it, where the sail is worked from.
                add_part(d, PART_SAIL_STATION, v3_make(h->beam - 0.45f, 0.45f, s->mount.z), v3_make(0.2f, 0.45f, 0.3f), BRASS, 60.0f,
                         v3_make(0, 0, 0), 0.0f, s->mast, SHIP_NOTHING, PAINT_FIXED);
                d->stations[s->mast] = v3_make(h->beam - 1.05f, 0, s->mount.z);
                if (s->mast + 1 > d->masts) d->masts = s->mast + 1;
                break;
            }
            case SLOT_SAIL: {
                // Nine tenths of the beam wide, the top two thirds of the mast high, just
                // ahead of it: above the heads of the crew.
                float height = mast_height[s->mast];
                if (height <= 0.0f) break;
                v3 half = v3_make(0.9f * h->beam, height / 3.0f, 0.04f);
                float area = 4.0f * half.x * half.y;
                add_part(d, PART_SAIL, v3_make(0, height - half.y, s->mount.z - mast_thickness[s->mast] - 0.02f), half, c->color,
                         c->mass * area, v3_make(0, 0, 0), 0.0f, s->mast, fit, PAINT_SAIL);
                break;
            }
            case SLOT_GYRO: {
                v3 half = v3_scale(c->half, lin);
                add_part(d, PART_GYRO, mounted(s, half), half, c->color, c->mass * h->mount, v3_make(0, 0, 0), c->power * h->mount, -1, fit,
                         PAINT_FIXED);
                break;
            }
            default: {   // a thruster
                v3 half = v3_scale(c->half, lin);
                add_part(d, PART_THRUSTER, mounted(s, half), half, c->color, c->mass * h->mount, v3_scale(s->push, c->power * h->mount), 0.0f,
                         -1, fit, PAINT_THRUSTER);
                break;
            }
            }
        }
}

// Ballast in the hull, low down: as much as three hundredths of the deck's weight,
// placed where it brings the centre of mass of the hull, fitted as standard, over
// the middle of the deck, so that the standard lift holds her level. Fitted
// otherwise, she may not be: that is hers to fight with her gyros.
static v3 trim_ballast(const hull_spec *h, const ship_design *standard, float mass) {
    v3 moment = v3_make(0, 0, 0);
    for (int i = 0; i < standard->part_count; ++i) moment = v3_add(moment, v3_scale(standard->parts[i].offset, standard->parts[i].mass));
    float x = -moment.x / mass, z = -moment.z / mass;
    float reach_x = h->beam - 0.5f, reach_z = 0.8f * h->length;
    x = x < -reach_x ? -reach_x : x > reach_x ? reach_x : x;
    z = z < -reach_z ? -reach_z : z > reach_z ? reach_z : z;
    return v3_make(x, -0.75f * h->depth, z);
}

static void add_ballast(ship_design *d, v3 at, float mass) {
    add_structure(d, at, v3_make(0.3f, 0.15f, 0.3f), IRON, mass, PAINT_FIXED);
}

bool ship_design_build(const ship_loadout *loadout, float gravity, ship_design *d) {
    memset(d, 0, sizeof(*d));
    if (!ship_loadout_valid(loadout)) return false;
    const hull_spec *h = ship_hull(loadout->hull);
    d->loadout = *loadout;
    // The standard fit first, for its ballast and its body: what this one is measured against.
    ship_design standard;
    memset(&standard, 0, sizeof(standard));
    ship_loadout plain = ship_default_loadout(loadout->hull);
    build_parts(&plain, &standard);
    float ballast_mass = 0.03f * h->structure;
    v3 ballast = trim_ballast(h, &standard, ballast_mass);
    add_ballast(&standard, ballast, ballast_mass);
    ship_body_of(standard.parts, standard.part_count, &standard.body);
    build_parts(loadout, d);
    add_ballast(d, ballast, ballast_mass);
    for (int i = 0; i < d->part_count; ++i) d->parts[i].color = ship_part_color(d->parts[i].paint, &loadout->look, d->parts[i].base);
    ship_body_of(d->parts, d->part_count, &d->body);

    // The stats: the body read as a shipyard would, and the flight computer's aims
    // set by how this ship's thrust and turning compare with the hull's standard.
    ship_stats *st = &d->stats;
    const ship_body *b = &d->body, *reference = &standard.body;
    st->hull = loadout->hull;
    st->crew = h->crew;
    st->masts = b->masts;
    st->mass = b->mass;
    st->response = h->response;
    st->turn_response = h->turn_response;
    st->hull_drag = h->hull_drag;
    if (b->mass <= 0.0f || reference->mass <= 0.0f) return false;
    for (int k = 0; k < CRAFT_GROUPS; ++k) {
        st->accel[k] = b->thrust[k] / b->mass;
        float plain_accel = reference->thrust[k] / reference->mass;
        float ratio = plain_accel > 0.0f ? sqrtf(st->accel[k] / plain_accel) : 1.0f;
        st->top_speed[k] = h->top_speed[k] * (ratio < 0.6f ? 0.6f : ratio > 1.4f ? 1.4f : ratio);
    }
    st->lift_ratio = gravity > 0.0f ? b->thrust[CRAFT_GROUP_UP] / (b->mass * gravity) : 0.0f;
    st->turn_accel = v3_make(b->gyro.x / b->inertia.x, b->gyro.y / b->inertia.y, b->gyro.z / b->inertia.z);
    const float turn[3] = {st->turn_accel.x, st->turn_accel.y, st->turn_accel.z};
    const float plain_turn[3] = {reference->gyro.x / reference->inertia.x, reference->gyro.y / reference->inertia.y,
                                 reference->gyro.z / reference->inertia.z};
    const float top[3] = {h->top_rate.x, h->top_rate.y, h->top_rate.z};
    float rate[3];
    for (int k = 0; k < 3; ++k) {
        float ratio = plain_turn[k] > 0.0f ? sqrtf(turn[k] / plain_turn[k]) : 1.0f;
        rate[k] = top[k] * (ratio < 0.6f ? 0.6f : ratio > 1.4f ? 1.4f : ratio);
    }
    st->top_rate = v3_make(rate[0], rate[1], rate[2]);
    float heel = 0.0f;
    for (int m = 0; m < b->masts; ++m) {
        const ship_sail_body *s = &b->sails[m];
        float push = CRAFT_SAIL_PUSH * s->area * s->drive;
        st->sail_area += s->area;
        st->sail_power += push;
        // Pushing along the bow (-z) at a height over the centre of mass: it pitches her nose down.
        heel += push * 100.0f * (s->center.y - b->center.y);
    }
    st->sail_accel = st->sail_power * 100.0f / b->mass;
    st->heel = b->inertia.x > 0.0f ? heel / b->inertia.x : 0.0f;
    return true;
}

// ---------------------------------------------------------------------------
// The body: mass, centre of mass, inertia, thrust and torque, summed over parts.
// ---------------------------------------------------------------------------
static float axis_component(v3 v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }
static v3 unit_axis(int k) { return v3_make(k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f); }

int ship_thrust_group(v3 f) {
    int axis = fabsf(f.x) >= fabsf(f.y) && fabsf(f.x) >= fabsf(f.z) ? 0 : fabsf(f.y) >= fabsf(f.z) ? 1 : 2;
    float along = axis_component(f, axis);
    if (along == 0.0f) return -1;
    return axis * 2 + (along < 0.0f ? 1 : 0);
}

void ship_body_of(const ship_part *parts, int count, ship_body *b) {
    memset(b, 0, sizeof(*b));
    // Two passes: the centre of mass first, then everything about it.
    v3 moment = v3_make(0, 0, 0);
    for (int i = 0; i < count; ++i) {
        b->mass += parts[i].mass;
        moment = v3_add(moment, v3_scale(parts[i].offset, parts[i].mass));
    }
    if (b->mass <= 0.0f) return;
    b->center = v3_scale(moment, 1.0f / b->mass);
    for (int i = 0; i < count; ++i) {
        const ship_part *part = &parts[i];
        v3 h = part->half, d = v3_sub(part->offset, b->center);
        float m = part->mass;
        // A solid box about its own centre (m/3 of the half extents squared), moved to
        // the centre of mass. The products of inertia are left out: hulls are near
        // enough symmetric that the principal axes are the ship's own.
        b->inertia.x += m / 3.0f * (h.y * h.y + h.z * h.z) + m * (d.y * d.y + d.z * d.z);
        b->inertia.y += m / 3.0f * (h.x * h.x + h.z * h.z) + m * (d.x * d.x + d.z * d.z);
        b->inertia.z += m / 3.0f * (h.x * h.x + h.y * h.y) + m * (d.x * d.x + d.y * d.y);
        float reach = v3_length(part->offset) + v3_length(h);
        if (reach > b->radius) b->radius = reach;
        if (h.y - part->offset.y > b->draft) b->draft = h.y - part->offset.y;
        int group = part->kind == PART_THRUSTER ? ship_thrust_group(part->thrust) : -1;
        if (group >= 0) {
            // Only along its group's axis: what else it points along is lost.
            int axis = group / 2;
            float along = axis_component(part->thrust, axis);
            b->thrust[group] += fabsf(along);
            b->thrust_torque[group] = v3_add(b->thrust_torque[group], v3_cross(d, v3_scale(unit_axis(axis), along)));
        } else if (part->kind == PART_GYRO) {
            b->gyro = v3_add(b->gyro, v3_make(part->torque, part->torque, part->torque));
        } else if (part->kind == PART_SAIL && part->mast >= 0 && part->mast < SHIP_MAX_MASTS) {
            // Its face, square to the bow: its breadth by its height. A mast's push acts
            // at the middle of all the sail on it.
            ship_sail_body *s = &b->sails[part->mast];
            float area = 4.0f * h.x * h.y;
            const ship_component *cloth = ship_component_info(part->component);
            float drive = cloth && cloth->slot == SLOT_SAIL ? cloth->power : 1.0f;
            float sharpness = cloth && cloth->slot == SLOT_SAIL ? cloth->sharpness : 1.0f;
            float total = s->area + area;
            if (total <= 0.0f) continue;
            s->center = v3_scale(v3_add(v3_scale(s->center, s->area), v3_scale(part->offset, area)), 1.0f / total);
            s->drive = (s->drive * s->area + drive * area) / total;
            s->sharpness = (s->sharpness * s->area + sharpness * area) / total;
            s->area = total;
            if (part->mast + 1 > b->masts) b->masts = part->mast + 1;
        }
    }
}

// ---------------------------------------------------------------------------
// The wire
// ---------------------------------------------------------------------------
void ship_loadout_write(cw_writer *w, const ship_loadout *l) {
    int n = ship_slot_count(l->hull);
    cw_writer_u8(w, l->hull);
    cw_writer_u8(w, (uint8_t)n);
    for (int i = 0; i < n; ++i) cw_writer_u8(w, l->fit[i]);
    cw_writer_u8(w, LOOK_FIELDS);
    for (int f = 0; f < LOOK_FIELDS; ++f) cw_writer_u8(w, l->look.field[f]);
}

static bool read_slots(cw_reader *r, ship_loadout *l) {
    memset(l, 0, sizeof(*l));
    uint8_t count = 0;
    cw_reader_u8(r, &l->hull);
    cw_reader_u8(r, &count);
    if (!cw_reader_ok(r) || count > SHIP_MAX_SLOTS) return false;
    for (int i = 0; i < count; ++i) cw_reader_u8(r, &l->fit[i]);
    return cw_reader_ok(r) && count == ship_slot_count(l->hull);
}

bool ship_loadout_read(cw_reader *r, ship_loadout *l) {
    if (!read_slots(r, l)) return false;
    uint8_t fields = 0;
    cw_reader_u8(r, &fields);
    if (!cw_reader_ok(r) || fields > LOOK_FIELDS) return false;   // a newer writer's extra fields would need skipping
    for (int f = 0; f < fields; ++f) cw_reader_u8(r, &l->look.field[f]);
    return cw_reader_ok(r) && ship_loadout_valid(l);
}

bool ship_loadout_read_plain(cw_reader *r, ship_loadout *l) { return read_slots(r, l) && ship_loadout_valid(l); }
