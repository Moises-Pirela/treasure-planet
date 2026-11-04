// The content of the world, as data: the hub, the planets, and the ship that serves
// each planet. Adding a planet is adding a row to PLANETS.
#include "game.h"
#include <stdio.h>
#include <string.h>

struct ship_part { v3 offset, half, color; };

static const ship_part SKIFF_PARTS[] = {
    {{0, 3.2f, -5.0f}, {1.5f, 0.15f, 1.0f}, {0.85f, 0.85f, 0.75f}},     // canopy
    {{1.2f, 1.6f, -5.0f}, {0.1f, 1.6f, 0.1f}, {0.4f, 0.4f, 0.4f}},
    {{-1.2f, 1.6f, -5.0f}, {0.1f, 1.6f, 0.1f}, {0.4f, 0.4f, 0.4f}},
};
static const ship_part HAULER_PARTS[] = {
    {{-3.5f, 1.0f, -4.0f}, {1.0f, 1.0f, 1.5f}, {0.55f, 0.35f, 0.2f}},   // cargo
    {{3.5f, 1.0f, -4.0f}, {1.0f, 1.0f, 1.5f}, {0.55f, 0.35f, 0.2f}},
    {{3.5f, 1.0f, 3.0f}, {1.0f, 1.0f, 1.5f}, {0.5f, 0.3f, 0.2f}},
    {{0, 2.5f, -11.5f}, {5.5f, 2.5f, 0.4f}, {0.7f, 0.4f, 0.15f}},       // bridge
};
static const ship_part CLIPPER_PARTS[] = {
    {{0, 6.0f, 2.0f}, {0.15f, 6.0f, 0.15f}, {0.35f, 0.25f, 0.2f}},      // masts
    {{0, 6.5f, -5.0f}, {0.15f, 6.5f, 0.15f}, {0.35f, 0.25f, 0.2f}},
    {{0, 8.0f, -5.0f}, {2.2f, 3.0f, 0.05f}, {0.85f, 0.9f, 0.95f}},      // sail
    {{0, 7.0f, 2.0f}, {2.0f, 2.5f, 0.05f}, {0.85f, 0.9f, 0.95f}},
};

struct planet_row {
    const char *planet, *ship;
    v3 color, ship_color;
    float radius;
    // Its land: how wide the continents are, how high they rise, where the sea
    // ends, how much of the height is mountains, and the seed it all comes from.
    float continents, relief, sea_level, mountains;
    uint32_t seed;
    v3 dock;                     // where its ship lands, relative to the origin
    float pier_x;                // the hub's pier for this ship
    float deck_half_x, deck_half_z;
    const ship_part *parts;
    int part_count;
    float flight_seconds, dock_seconds;
};

#define COUNT_OF(a) (int)(sizeof(a) / sizeof((a)[0]))

static const planet_row PLANETS[] = {
    {"Verdant", "Emerald Skiff", {0.25f, 0.55f, 0.3f}, {0.2f, 0.6f, 0.4f}, 260.0f,
     2.5f, 40.0f, 0.5f, 0.6f, 1,
     {-900, 120, -1400}, -20.0f, 2.5f, 7.0f, SKIFF_PARTS, COUNT_OF(SKIFF_PARTS), 30.0f, 20.0f},
    {"Ember", "Rust Hauler", {0.65f, 0.3f, 0.15f}, {0.75f, 0.45f, 0.2f}, 420.0f,
     2.5f, 55.0f, 0.42f, 0.8f, 2,
     {0, -200, -2200}, 0.0f, 5.5f, 12.0f, HAULER_PARTS, COUNT_OF(HAULER_PARTS), 40.0f, 25.0f},
    {"Azure", "Blue Clipper", {0.2f, 0.4f, 0.7f}, {0.3f, 0.45f, 0.8f}, 320.0f,
     2.0f, 45.0f, 0.55f, 0.5f, 3,
     {1100, 300, -1600}, 20.0f, 2.5f, 10.0f, CLIPPER_PARTS, COUNT_OF(CLIPPER_PARTS), 35.0f, 30.0f},
};

static const float HUB_HALF = 30.0f;
static const float PIER_LENGTH = 14.0f;
static const float PAD_HALF = 30.0f;
static const float PAD_FLAT = 45.0f;          // metres of land about the pole the pad is set into
static const float MONUMENT_ARC = 85.0f;      // how far from the pad the monument stands, along the land
static const float MONUMENT_HEIGHT = 52.0f;

static cw_entity_id spawn_block(game *g, v3 center, v3 half, v3 color) {
    cw_world *w = &g->world;
    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {center, center};
    Solid solid = {half};
    Visual visual = {MESH_CUBE, half, color};
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_solid, &solid, sizeof(solid));
    cw_comp_set(w, id, g->c_visual, &visual, sizeof(visual));
    return id;
}

static void spawn_part(game *g, cw_entity_id ship, v3 dock, v3 offset, v3 half, v3 color) {
    cw_world *w = &g->world;
    cw_entity_id id = spawn_block(g, v3_add(dock, offset), half, color);
    Attachment a = {ship, offset};
    cw_comp_set(w, id, g->c_attachment, &a, sizeof(a));
}

// Ships dock bow towards -z with the open stern against a pier or pad. The ship's
// position is the centre of the deck's top, level with what it docks against, so
// boarding is walking straight on.
static void spawn_ship(game *g, const planet_row *row, cw_entity_id planet, v3 hub_dock, v3 planet_dock) {
    cw_world *w = &g->world;
    float hx = row->deck_half_x, hz = row->deck_half_z;
    Ship ship = {};
    strncpy(ship.name, row->ship, GAME_NAME_CAPACITY - 1);
    ship.half = v3_make(hx, 16.0f, hz);
    ship.hub_dock = hub_dock;
    ship.planet_dock = planet_dock;
    ship.planet = planet;
    ship.cruise_height = 140.0f;
    ship.flight_seconds = row->flight_seconds;
    ship.dock_seconds = row->dock_seconds;
    ship.phase = 0.0f;           // every ship starts its timetable docked at the hub
    v3 at = game_ship_at_time(&ship, game_time_of_tick(g, g->world.tick));

    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {at, at};
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_ship, &ship, sizeof(ship));

    v3 hull = row->ship_color, rail = v3_scale(hull, 0.6f);
    const float rail_y = 0.55f, rail_half = 0.55f, thick = 0.1f;
    spawn_part(g, id, hub_dock, v3_make(0, -0.25f, 0), v3_make(hx, 0.25f, hz), hull);
    spawn_part(g, id, hub_dock, v3_make(hx - thick, rail_y, 0), v3_make(thick, rail_half, hz), rail);
    spawn_part(g, id, hub_dock, v3_make(-hx + thick, rail_y, 0), v3_make(thick, rail_half, hz), rail);
    spawn_part(g, id, hub_dock, v3_make(0, rail_y, -hz + thick), v3_make(hx, rail_half, thick), rail);
    // The stern stays open except for stubs beside a 2 m gangway.
    float stub = (hx - 1.0f) * 0.5f;
    if (stub > 0.0f) {
        spawn_part(g, id, hub_dock, v3_make(hx - stub, rail_y, hz - thick), v3_make(stub, rail_half, thick), rail);
        spawn_part(g, id, hub_dock, v3_make(-hx + stub, rail_y, hz - thick), v3_make(stub, rail_half, thick), rail);
    }
    for (int i = 0; i < row->part_count; ++i)
        spawn_part(g, id, hub_dock, row->parts[i].offset, row->parts[i].half, row->parts[i].color);
}

static void declare_place(game *g, const char *name, v3 center, v3 half, v3 spawn) {
    game_declare_place(g, name, PLACE_ZONE, center, half, spawn, NULL);
}

// The door to the planet's dungeon, at the far side of the pad: two posts, a lintel
// and a dark slab, with the entrance place inside the frame.
static void spawn_dungeon_door(game *g, const planet_row *row, v3 pad) {
    v3 door = v3_add(pad, v3_make(0, 1.0f, PAD_HALF - 6.0f));   // on the pad's surface
    v3 post_color = v3_scale(row->color, 0.5f), dark = v3_make(0.03f, 0.02f, 0.06f);
    spawn_block(g, v3_add(door, v3_make(-2.2f, 2.0f, 0)), v3_make(0.4f, 2.0f, 0.6f), post_color);
    spawn_block(g, v3_add(door, v3_make(2.2f, 2.0f, 0)), v3_make(0.4f, 2.0f, 0.6f), post_color);
    spawn_block(g, v3_add(door, v3_make(0, 4.4f, 0)), v3_make(2.6f, 0.4f, 0.6f), post_color);
    spawn_block(g, v3_add(door, v3_make(0, 2.0f, 1.2f)), v3_make(1.8f, 2.0f, 0.2f), dark);   // what you walk into
    char dungeon[GAME_NAME_CAPACITY];
    game_dungeon_name(row->planet, dungeon, sizeof(dungeon));
    game_declare_place(g, dungeon, PLACE_ENTRANCE, v3_add(door, v3_make(0, 2.0f, 0.3f)), v3_make(1.6f, 2.0f, 0.7f),
                       v3_add(door, v3_make(0, 0, -4.0f)), row->planet);
}

// The monument: a stepped spire on the land beyond the pad, in the direction the
// ship comes in from, so it stands ahead of the deck all the way down. The
// landmark place is what the client lights it by.
static void spawn_monument(game *g, const planet_row *row, const PlanetLand *land, v3 centre, v3 approach) {
    v3 base = game_planet_point(land, centre, approach, MONUMENT_ARC);
    v3 stone = v3_scale(row->color, 0.55f), facing = v3_scale(row->color, 1.5f);
    spawn_block(g, v3_add(base, v3_make(0, 2.0f, 0)), v3_make(11.0f, 3.0f, 11.0f), stone);      // the plinth
    float height = 5.0f;
    for (int step = 0; step < 4; ++step) {
        float half = 7.0f - 1.4f * (float)step, tall = MONUMENT_HEIGHT * 0.22f;
        spawn_block(g, v3_add(base, v3_make(0, height + tall, 0)), v3_make(half, tall, half),
                    step % 2 ? v3_scale(stone, 1.2f) : stone);
        height += tall * 2.0f;
    }
    spawn_block(g, v3_add(base, v3_make(0, height + 3.0f, 0)), v3_make(2.2f, 3.0f, 2.2f), facing);   // the crown
    // Two pylons flanking the way from the pad to it.
    v3 side = v3_normalize(v3_cross(v3_sub(base, centre), approach));
    for (int k = -1; k <= 1; k += 2) {
        v3 at = v3_add(base, v3_scale(side, 22.0f * (float)k));
        at.y = game_planet_surface_y(land, centre, at);
        spawn_block(g, v3_add(at, v3_make(0, 8.0f, 0)), v3_make(1.6f, 8.0f, 1.6f), stone);
    }
    char name[GAME_NAME_CAPACITY];
    snprintf(name, sizeof(name), "%s Monument", row->planet);
    game_declare_place(g, name, PLACE_LANDMARK, v3_add(base, v3_make(0, height + 3.0f, 0)),
                       v3_make(3.0f, 4.0f, 3.0f), base, row->planet);
}

static void spawn_planet(game *g, const planet_row *row) {
    cw_world *w = &g->world;
    // The landing pad butts against the ship's stern, level with its deck.
    v3 pad = v3_make(row->dock.x, row->dock.y - 1.0f, row->dock.z + row->deck_half_z + PAD_HALF);
    spawn_block(g, pad, v3_make(PAD_HALF, 1.0f, PAD_HALF), v3_scale(row->color, 1.3f));
    // Standing anywhere over the pad is being at the planet; the docked ship's deck
    // is not, so a passenger who stays aboard rides back.
    declare_place(g, row->planet, v3_add(pad, v3_make(0, 20.0f, 0)), v3_make(PAD_HALF, 21.0f, PAD_HALF),
                  v3_add(pad, v3_make(0, 1.0f, 10.0f)));
    spawn_dungeon_door(g, row, pad);
    spawn_block(g, v3_add(pad, v3_make(10, 2.0f, 8)), v3_make(2, 1, 2), v3_scale(row->color, 0.8f));
    spawn_block(g, v3_add(pad, v3_make(-14, 2.5f, 15)), v3_make(3, 1.5f, 2), v3_scale(row->color, 0.7f));

    cw_entity_id id = cw_world_create_entity(w);
    v3 center = v3_make(pad.x, pad.y - row->radius, pad.z);
    Transform t = {center, center};
    Planet planet = {};
    strncpy(planet.name, row->planet, GAME_NAME_CAPACITY - 1);
    planet.land.radius = row->radius;
    planet.land.continents = row->continents;
    planet.land.relief = row->relief;
    planet.land.sea_level = row->sea_level;
    planet.land.mountains = row->mountains;
    planet.land.shore = 0.04f;
    planet.land.pad_radius = PAD_FLAT;
    planet.land.seed = row->seed;
    Visual visual = {MESH_SPHERE, v3_make(row->radius, row->radius, row->radius), row->color};
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_planet, &planet, sizeof(planet));
    cw_comp_set(w, id, g->c_visual, &visual, sizeof(visual));

    v3 hub_dock = v3_make(row->pier_x, 0.0f, -HUB_HALF - PIER_LENGTH - row->deck_half_z);
    // The way in, from the hub's dock to this one: the monument stands beyond the
    // pad along it, so a passenger coming down has it ahead of them.
    spawn_monument(g, row, &planet.land, center, v3_normalize(v3_sub(row->dock, hub_dock)));
    spawn_ship(g, row, id, hub_dock, row->dock);
    v3 pier = v3_make(row->pier_x, -0.5f, -HUB_HALF - PIER_LENGTH * 0.5f);
    spawn_block(g, pier, v3_make(2.0f, 0.5f, PIER_LENGTH * 0.5f), v3_make(0.45f, 0.45f, 0.5f));
}

void game_build_universe(game *g) {
    g->spawn = v3_make(0.0f, 0.0f, 12.0f);
    // The hub and its piers; the ships docked at their ends are not part of it.
    declare_place(g, "hub", v3_make(0, 20.0f, -PIER_LENGTH * 0.5f),
                  v3_make(HUB_HALF + 5.0f, 21.0f, HUB_HALF + PIER_LENGTH * 0.5f), g->spawn);
    v3 stone = v3_make(0.5f, 0.5f, 0.55f);
    spawn_block(g, v3_make(0, -1.0f, 0), v3_make(HUB_HALF, 1.0f, HUB_HALF), stone);
    spawn_block(g, v3_make(0, 3.0f, 22.0f), v3_make(8, 3, 2), v3_scale(stone, 0.7f));      // a terminal
    spawn_block(g, v3_make(-22, 1.0f, 5.0f), v3_make(2, 1, 2), v3_make(0.4f, 0.3f, 0.25f));  // crates
    spawn_block(g, v3_make(22, 1.5f, -5.0f), v3_make(2, 1.5f, 3), v3_make(0.3f, 0.35f, 0.4f));
    for (int i = 0; i < COUNT_OF(PLANETS); ++i) spawn_planet(g, &PLANETS[i]);
}
