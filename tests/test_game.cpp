// Plays the game without a window: walk onto a ship, fly to a planet, walk off.
#include "../src/game/game.h"
#include "../src/game/shipwright.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } \
} while (0)

static Transform *transform_of(game *g, cw_entity_id id) { return CW_GET(&g->world, id, g->c_transform, Transform); }
static Player *player_of(game *g, cw_entity_id id) { return CW_GET(&g->world, id, g->c_player, Player); }
static Ship *ship_of(game *g, cw_entity_id id) { return CW_GET(&g->world, id, g->c_ship, Ship); }

// Steers a player straight at a point on the ground for up to `seconds`. Aboard a
// craft the yaw is in the craft's frame, as a client would send it.
static bool walk_to(game *g, cw_entity_id player, float x, float z, float seconds) {
    PlayerInput *in = CW_GET(&g->world, player, g->c_input, PlayerInput);
    for (int i = 0; i < (int)(seconds * GAME_TICK_HZ); ++i) {
        v3 p = transform_of(g, player)->position;
        float dx = x - p.x, dz = z - p.z;
        if (dx * dx + dz * dz < 0.04f) {
            in->forward = 0.0f;
            return true;
        }
        v3 way = v3_make(dx, 0, dz);
        cw_entity_id craft = game_aboard(g, player);
        if (craft != CW_INVALID_ENTITY) way = quat_unrotate(CW_GET(&g->world, craft, g->c_craft, Craft)->orientation, way);
        in->yaw = atan2f(-way.x, -way.z);
        in->forward = 1.0f;
        game_tick(g);
    }
    in->forward = 0.0f;
    return false;
}
static void run_seconds(game *g, float seconds) {
    for (int i = 0; i < (int)(seconds * GAME_TICK_HZ); ++i) game_tick(g);
}

static void test_players_stand_and_walls_block(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    cw_entity_id p = game_spawn_player(g, v3_add(g->spawn, v3_make(0, 2, 0)), true);
    run_seconds(g, 1.0f);
    CHECK(fabsf(transform_of(g, p)->position.y) < 0.01f);    // landed on the hub
    CHECK(player_of(g, p)->grounded);

    // The terminal at z = 20..24 is a wall: walking into it stops at its face.
    walk_to(g, p, 0.0f, 30.0f, 4.0f);
    CHECK(transform_of(g, p)->position.z <= 20.0f - PLAYER_RADIUS + 0.01f);
    CHECK(transform_of(g, p)->position.z > 19.0f);

    // Jumping leaves the ground and comes back.
    CW_GET(&g->world, p, g->c_input, PlayerInput)->jump = true;
    game_tick(g);
    CW_GET(&g->world, p, g->c_input, PlayerInput)->jump = false;
    CHECK(!player_of(g, p)->grounded);
    run_seconds(g, 0.2f);
    CHECK(transform_of(g, p)->position.y > 0.5f);
    run_seconds(g, 1.0f);
    CHECK(player_of(g, p)->grounded);

    // Set down a hair inside the floor (as a teleport or a rounding might), a player
    // standing still is not thrown to the floor's far side.
    cw_entity_id r = game_spawn_player(g, v3_make(10.0f, -0.001f, 10.0f), true);
    run_seconds(g, 0.2f);
    CHECK(fabsf(transform_of(g, r)->position.x - 10.0f) < 0.001f && fabsf(transform_of(g, r)->position.z - 10.0f) < 0.001f);
    CHECK(fabsf(transform_of(g, r)->position.y) < 0.01f);

    // Walking off the edge of the world puts you back at the hub.
    cw_entity_id q = game_spawn_player(g, v3_make(-25, 0, 29), true);   // the south edge: no bridge there
    walk_to(g, q, -25.0f, 40.0f, 1.0f);
    run_seconds(g, 10.0f);
    CHECK(transform_of(g, q)->position.y > -1.0f);
    CHECK(v3_length(v3_sub(transform_of(g, q)->position, g->spawn)) < 1.0f);

    game_destroy(g);
    free(g);
}

// The whole loop, with two players boarding and one staying behind.
static void test_boarding_and_flight(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);

    cw_entity_id ship_id = game_find_ship(g, "Emerald Skiff");
    CHECK(ship_id != CW_INVALID_ENTITY);
    if (ship_id == CW_INVALID_ENTITY) return;
    const Ship *ship = ship_of(g, ship_id);
    v3 hub_dock = ship->hub_dock;

    // Alice and Bruno wait where the pier meets the hub; Carol stays in the middle.
    cw_entity_id alice = game_spawn_player(g, v3_make(hub_dock.x - 1.0f, 0, -28), true);
    cw_entity_id bruno = game_spawn_player(g, v3_make(hub_dock.x + 1.0f, 0, -28), true);
    cw_entity_id carol = game_spawn_player(g, g->spawn, true);
    run_seconds(g, 0.2f);
    CHECK(game_ship_at(g, transform_of(g, alice)->position) == CW_INVALID_ENTITY);

    // Boarding is walking: down the pier and through the stern gangway.
    CHECK(walk_to(g, alice, hub_dock.x, hub_dock.z, 6.0f));
    CHECK(game_ship_at(g, transform_of(g, alice)->position) == ship_id);
    CHECK(fabsf(transform_of(g, alice)->position.y - hub_dock.y) < 0.01f);
    CHECK(walk_to(g, bruno, hub_dock.x, hub_dock.z + 3.0f, 6.0f));
    CHECK(game_ship_at(g, transform_of(g, bruno)->position) == ship_id);
    v3 carol_start = transform_of(g, carol)->position;
    CHECK(ship->state == SHIP_DOCKED);

    // Left alone, the ship leaves on its timetable, carrying everyone aboard.
    int guard = 0;
    while (ship->state == SHIP_DOCKED && guard++ < 60 * 60) game_tick(g);
    CHECK(ship->state == SHIP_FLYING && ship->stop == STOP_PLANET);
    float highest = 0.0f;
    while (ship->state == SHIP_FLYING && guard++ < 60 * 120) {
        game_tick(g);
        v3 s = transform_of(g, ship_id)->position;
        v3 a = transform_of(g, alice)->position;
        if (s.y > highest) highest = s.y;
        // Standing on the deck the entire flight: no drift through or off it.
        CHECK(fabsf(a.y - s.y) < 0.05f);
        CHECK(game_ship_at(g, a) == ship_id);
        CHECK(player_of(g, alice)->grounded);
    }
    CHECK(ship->state == SHIP_DOCKED && ship->stop == STOP_PLANET);
    CHECK(highest >= ship->planet_dock.y + ship->cruise_height - 1.0f);
    CHECK(v3_length(v3_sub(transform_of(g, ship_id)->position, ship->planet_dock)) < 0.01f);
    CHECK(game_ship_at(g, transform_of(g, alice)->position) == ship_id);
    CHECK(game_ship_at(g, transform_of(g, bruno)->position) == ship_id);

    // Carol never boarded, so she is still at the hub.
    CHECK(v3_length(v3_sub(transform_of(g, carol)->position, carol_start)) < 0.01f);

    // Walking off onto the planet's pad, and staying there while the ship leaves.
    v3 dock = ship->planet_dock;
    CHECK(walk_to(g, alice, dock.x, dock.z + 20.0f, 10.0f));
    CHECK(game_ship_at(g, transform_of(g, alice)->position) == CW_INVALID_ENTITY);
    CHECK(fabsf(transform_of(g, alice)->position.y - dock.y) < 0.01f);
    guard = 0;
    while (ship->state == SHIP_DOCKED && guard++ < 60 * 60) game_tick(g);
    CHECK(ship->stop == STOP_HUB);
    run_seconds(g, 5.0f);
    CHECK(fabsf(transform_of(g, alice)->position.y - dock.y) < 0.01f);
    CHECK(game_ship_at(g, transform_of(g, bruno)->position) == ship_id);   // bruno rides back

    guard = 0;
    while (ship->state == SHIP_FLYING && guard++ < 60 * 60) game_tick(g);
    CHECK(ship->state == SHIP_DOCKED && ship->stop == STOP_HUB);
    CHECK(fabsf(transform_of(g, bruno)->position.y - hub_dock.y) < 0.05f);

    game_destroy(g);
    free(g);
}

// Each planet has its own distinct ship, and they do not interfere.
static void test_every_planet_has_a_ship(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    const char *names[] = {"Emerald Skiff", "Rust Hauler", "Blue Clipper"};
    cw_entity_id seen_planets[3];
    for (int i = 0; i < 3; ++i) {
        cw_entity_id id = game_find_ship(g, names[i]);
        CHECK(id != CW_INVALID_ENTITY);
        seen_planets[i] = ship_of(g, id)->planet;
        CHECK(cw_comp_has(&g->world, seen_planets[i], g->c_planet));
        for (int j = 0; j < i; ++j) CHECK(seen_planets[i] != seen_planets[j]);
    }
    game_destroy(g);
    free(g);
}

// A planet's land is ground: it is flat where the landing pad is set into it, it
// rises away from there, and a player dropped over it lands on it rather than
// through it. The monument stands on that same land.
static void test_planet_land(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);

    cw_entity_id planet = CW_INVALID_ENTITY;
    cw_component_id needed[] = {g->c_transform, g->c_planet};
    cw_query planets = cw_query_iter(&g->world, cw_mask(needed, 2));
    cw_entity_id id;
    while (cw_query_next(&planets, &id))
        if (!strcmp(CW_GET(&g->world, id, g->c_planet, Planet)->name, "Verdant")) planet = id;
    CHECK(planet != CW_INVALID_ENTITY);
    const Planet *p = CW_GET(&g->world, planet, g->c_planet, Planet);
    v3 centre = CW_GET(&g->world, planet, g->c_transform, Transform)->position;
    CHECK(p->land.radius > 0.0f && p->land.relief > 0.0f && p->land.pad_radius > 0.0f);

    // Flat where the pad is, and somewhere higher beyond it.
    CHECK(game_planet_height(&p->land, v3_make(0, 1, 0)) == 0.0f);
    float highest = 0.0f;
    for (int i = 0; i < 64; ++i) {
        float around = 2.0f * TP_PI * (float)i / 64.0f;
        v3 at = game_planet_point(&p->land, centre, v3_make(cosf(around), 0, sinf(around)), 120.0f);
        float height = v3_length(v3_sub(at, centre)) - p->land.radius;
        CHECK(height >= -0.01f && height <= p->land.relief + 0.01f);   // sea level to the highest peak
        if (height > highest) highest = height;
    }
    CHECK(highest > 1.0f);    // not a smooth ball: there is land out there

    // The ground under a point is the land: a player dropped over it lands on it.
    v3 spot = game_planet_point(&p->land, centre, v3_make(0.6f, 0, -0.8f), 110.0f);
    float ground = game_ground_y(g, v3_add(spot, v3_make(0, 40.0f, 0)));
    CHECK(fabsf(ground - spot.y) < 0.2f);
    cw_entity_id player = game_spawn_player(g, v3_add(spot, v3_make(0, 25.0f, 0)), true);
    for (int tick = 0; tick < 180; ++tick) game_tick(g);
    v3 at = CW_GET(&g->world, player, g->c_transform, Transform)->position;
    CHECK(CW_GET(&g->world, player, g->c_player, Player)->grounded);
    CHECK(fabsf(at.y - game_ground_y(g, at)) < 0.05f);
    CHECK(at.y > centre.y + p->land.radius - 40.0f);    // on the land, not fallen past the world

    // The monument is a landmark place, and it is not a way anywhere.
    int landmark = game_find_place(g, "Verdant Monument");
    CHECK(landmark >= 0 && g->places[landmark].kind == PLACE_LANDMARK);
    CHECK(game_place_at(g, g->places[landmark].center) != landmark);
    game_destroy(g);
    free(g);
}

// The whole game state survives a save: the persistence an MMO stands on.
static void test_save_and_restore(void) {
    game *a = (game *)malloc(sizeof(game));
    game_init(a, CW_HOST);
    game_build_universe(a);
    cw_entity_id player = game_spawn_player(a, a->spawn, true);
    run_seconds(a, 25.0f);     // the skiff is mid-flight by now

    cw_snapshot_save_config save = {};
    save.allow_raw = true;
    save.include_transient = true;
    cw_writer out;
    cw_writer_init(&out);
    cw_snapshot_error error;
    CHECK(cw_snapshot_save(&a->world, &save, &out, &error));

    game *b = (game *)malloc(sizeof(game));
    game_init(b, CW_HOST);
    cw_snapshot_load_config load = cw_snapshot_load_defaults();
    load.allow_raw = true;
    CHECK(cw_snapshot_load(&b->world, out.data, out.size, &load, &error));
    b->spawn = a->spawn;

    run_seconds(a, 10.0f);
    run_seconds(b, 10.0f);
    cw_entity_id ship = game_find_ship(a, "Emerald Skiff");
    CHECK(memcmp(transform_of(a, ship), transform_of(b, ship), sizeof(Transform)) == 0);
    CHECK(memcmp(transform_of(a, player), transform_of(b, player), sizeof(Transform)) == 0);

    cw_writer_destroy(&out);
    game_destroy(a);
    game_destroy(b);
    free(a);
    free(b);
}

// Every planet has a door on its pad, and behind it a dungeon that can be walked
// from the spawn to the exit, room by room.
static void test_dungeons(void) {
    game *u = (game *)malloc(sizeof(game));
    game_init(u, CW_HOST);
    game_build_universe(u);
    const char *planets[] = {"Verdant", "Ember", "Azure"};
    for (int i = 0; i < 3; ++i) {
        int planet = game_find_place(u, planets[i]);
        CHECK(planet >= 0 && u->places[planet].kind == PLACE_ZONE);
        char dungeon[GAME_NAME_CAPACITY];
        game_dungeon_name(planets[i], dungeon, sizeof(dungeon));
        int door = game_find_place(u, dungeon);
        CHECK(door >= 0 && u->places[door].kind == PLACE_ENTRANCE);
        CHECK(strcmp(u->places[door].destination, planets[i]) == 0);
        // The door stands on the pad, so the pad's place must not hide it...
        CHECK(game_place_at(u, u->places[door].center) == door);
        // ...and a player arriving at the planet can walk straight into it.
        cw_entity_id p = game_spawn_player(u, u->places[planet].spawn, true);
        run_seconds(u, 0.5f);
        CHECK(game_place_at(u, transform_of(u, p)->position) == planet);
        CHECK(walk_to(u, p, u->places[door].center.x, u->places[door].center.z, 8.0f));
        CHECK(game_place_at(u, transform_of(u, p)->position) == door);

        // The dungeon itself: same name, same layout, wherever it is built.
        game *d = (game *)malloc(sizeof(game));
        game_init(d, CW_HOST);
        game_build_dungeon(d, dungeon, planets[i], u->places[door].spawn, 7);
        game *d2 = (game *)malloc(sizeof(game));
        game_init(d2, CW_HOST);
        game_build_dungeon(d2, dungeon, planets[i], u->places[door].spawn, 7);
        CHECK(cw_world_entity_count(&d->world) == cw_world_entity_count(&d2->world));
        CHECK(cw_query_count(d->q_ships) == 0);
        int exit_place = game_find_place(d, "exit");
        CHECK(exit_place >= 0 && d->places[exit_place].kind == PLACE_EXIT);
        CHECK(strcmp(d->places[exit_place].destination, planets[i]) == 0);
        v3 rooms[GAME_MAX_PLACES];
        int room_count = game_dungeon_path(d, rooms, GAME_MAX_PLACES);
        CHECK(room_count >= 4);
        cw_entity_id q = game_spawn_player(d, d->spawn, true);
        run_seconds(d, 0.5f);
        CHECK(fabsf(transform_of(d, q)->position.y) < 0.01f);   // on the floor
        for (int k = 0; k < room_count; ++k) CHECK(walk_to(d, q, rooms[k].x, rooms[k].z, 10.0f));
        CHECK(walk_to(d, q, d->places[exit_place].center.x, d->places[exit_place].center.z, 6.0f));
        CHECK(game_place_at(d, transform_of(d, q)->position) == exit_place);
        game_destroy(d);
        game_destroy(d2);
        free(d);
        free(d2);
    }
    game_destroy(u);
    free(u);
}

// Items are five numbers with a deterministic name; a character wears the best and
// stashes the rest; dungeons hold chests whose contents differ per run.
static void test_items(void) {
    item a = item_roll(12345, 1), b = item_roll(12345, 1), c = item_roll(54321, 3);
    CHECK(item_exists(&a) && memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(a.slot < SLOT_COUNT && a.rarity < RARITY_COUNT && a.power >= 10);
    CHECK(c.power >= 30);
    char name1[48], name2[48];
    item_name(&a, name1, sizeof(name1));
    item_name(&b, name2, sizeof(name2));
    CHECK(strcmp(name1, name2) == 0 && strlen(name1) > 4);
    int rarities[RARITY_COUNT] = {0};
    for (uint32_t seed = 1; seed <= 2000; ++seed) ++rarities[item_roll(seed, 1).rarity];
    CHECK(rarities[RARITY_COMMON] > 800 && rarities[RARITY_LEGENDARY] > 20 && rarities[RARITY_LEGENDARY] < 120);

    character ch = {};
    item cap = {1, SLOT_HEAD, RARITY_COMMON, 10, 1}, better = {2, SLOT_HEAD, RARITY_RARE, 25, 2}, worse = {3, SLOT_HEAD, RARITY_COMMON, 5, 3};
    CHECK(character_take(&ch, &cap) && ch.equipment[SLOT_HEAD].id == 1 && character_stash_count(&ch) == 0);
    CHECK(character_take(&ch, &better) && ch.equipment[SLOT_HEAD].id == 2 && ch.stash[0].id == 1);   // the old cap goes to the stash
    CHECK(character_take(&ch, &worse) && ch.equipment[SLOT_HEAD].id == 2 && ch.stash[1].id == 3);
    CHECK(ch.version == 3);
    CHECK(character_equip(&ch, 1) && ch.equipment[SLOT_HEAD].id == 3 && ch.stash[1].id == 2);   // wear the worse one on purpose
    CHECK(!character_equip(&ch, 5));
    for (int i = 0; i < STASH_SIZE + 2; ++i) {
        item filler = {(uint32_t)(100 + i), SLOT_TRINKET, RARITY_COMMON, 1, (uint32_t)i};
        character_take(&ch, &filler);
    }
    CHECK(character_stash_count(&ch) == STASH_SIZE);
    item one_more = {999, SLOT_TRINKET, RARITY_COMMON, 1, 9};
    CHECK(!character_take(&ch, &one_more));   // no room anywhere

    game *d1 = (game *)malloc(sizeof(game)), *d2 = (game *)malloc(sizeof(game));
    game_init(d1, CW_HOST);
    game_init(d2, CW_HOST);
    game_build_dungeon(d1, "Verdant Depths", "Verdant", v3_make(0, 0, 0), 11);
    game_build_dungeon(d2, "Verdant Depths", "Verdant", v3_make(0, 0, 0), 22);
    CHECK(cw_query_count(d1->q_loot) >= 2);
    CHECK(cw_query_count(d1->q_loot) == cw_query_count(d2->q_loot));   // the same chests...
    CHECK(cw_query_count(d1->q_solids) == cw_query_count(d2->q_solids));   // ...the same rooms...
    cw_entity_id chests1[16], chests2[16];
    size_t n = cw_query_gather(d1->q_loot, chests1, 16);
    cw_query_gather(d2->q_loot, chests2, 16);
    bool different = false;
    for (size_t i = 0; i < n; ++i)
        different = different || CW_GET(&d1->world, chests1[i], d1->c_loot, Loot)->it.id != CW_GET(&d2->world, chests2[i], d2->c_loot, Loot)->it.id;
    CHECK(different);                                                        // ...different loot
    v3 chest = CW_GET(&d1->world, chests1[0], d1->c_transform, Transform)->position;
    CHECK(game_loot_at(d1, v3_make(chest.x, 0.0f, chest.z)) == chests1[0]);
    CHECK(game_loot_at(d1, v3_make(chest.x + 3.0f, 0.0f, chest.z)) == CW_INVALID_ENTITY);
    game_destroy(d1);
    game_destroy(d2);
    free(d1);
    free(d2);
}

// ---------------------------------------------------------------------------
// Crafts
// ---------------------------------------------------------------------------
static Craft *craft_of(game *g, cw_entity_id id) { return CW_GET(&g->world, id, g->c_craft, Craft); }
static PlayerInput *input_of(game *g, cw_entity_id id) { return CW_GET(&g->world, id, g->c_input, PlayerInput); }
static void press_use(game *g, cw_entity_id player) {
    input_of(g, player)->use = true;
    game_tick(g);
    input_of(g, player)->use = false;
}
static v3 bow_of(game *g, cw_entity_id craft) { return quat_rotate(craft_of(g, craft)->orientation, v3_make(0, 0, -1)); }
static float lowest_point(game *g, cw_entity_id craft) {
    float lowest = 1e30f;
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&parts, &id)) {
        const Attachment *a = CW_GET(&g->world, id, g->c_attachment, Attachment);
        if (a->ship != craft) continue;
        v3 h = CW_GET(&g->world, id, g->c_solid, Solid)->half;
        for (int k = 0; k < 8; ++k) {
            v3 corner = v3_add(a->offset, v3_make(k & 1 ? h.x : -h.x, k & 2 ? h.y : -h.y, k & 4 ? h.z : -h.z));
            float y = game_craft_to_world(g, craft, corner).y;
            if (y < lowest) lowest = y;
        }
    }
    return lowest;
}
// Up a craft's stairs, from behind them, and onto its deck at their head.
static void board_craft(game *g, cw_entity_id player, cw_entity_id craft) {
    const Craft *c = CW_GET(&g->world, craft, g->c_craft, Craft);
    float stern = c->cabin_center.z + c->cabin_half.z, stairs = c->draft / 0.3f * 0.45f;   // as long as they are deep
    v3 foot = game_craft_to_world(g, craft, v3_make(0, 0, stern + stairs + 1.5f));
    CHECK(walk_to(g, player, foot.x, foot.z, 10.0f));
    v3 deck = game_craft_to_world(g, craft, v3_make(0, 0, stern - 1.5f));   // aft of any mast
    CHECK(walk_to(g, player, deck.x, deck.z, 6.0f));
}
// A player at the yard terminal calls a craft; they walk up its stairs and onto its deck.
static cw_entity_id call_and_board(game *g, cw_entity_id player) {
    int yard = -1;
    for (int i = 0; i < g->place_count; ++i)
        if (g->places[i].kind == PLACE_YARD) yard = i;
    CHECK(yard >= 0);
    if (yard < 0) return CW_INVALID_ENTITY;
    transform_of(g, player)->position = g->places[yard].spawn;
    run_seconds(g, 0.2f);
    press_use(g, player);
    cw_entity_id craft = game_craft_of(g, game_owner(g, player));
    CHECK(craft != CW_INVALID_ENTITY);
    if (craft == CW_INVALID_ENTITY) return craft;
    board_craft(g, player, craft);
    return craft;
}

// A craft is its parts: their masses and thrusters are what it weighs and pushes
// with. Called at the yard, it rests on its berth without sinking or sliding.
static void test_craft_from_parts(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    CHECK(g->berth_count >= 4);
    cw_entity_id player = game_spawn_player(g, g->spawn, true);
    game_set_owner(g, player, 42);
    cw_entity_id craft = call_and_board(g, player);
    if (craft == CW_INVALID_ENTITY) { game_destroy(g); free(g); return; }
    const Craft *c = craft_of(g, craft);
    CHECK(game_owner(g, craft) == 42);
    CHECK(c->berth == 0);

    float mass = 0.0f, thrust_forward = 0.0f, thrust_up = 0.0f;
    int parts = 0;
    cw_query query = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&query, &id)) {
        if (CW_GET(&g->world, id, g->c_attachment, Attachment)->ship != craft) continue;
        const Part *part = CW_GET(&g->world, id, g->c_part, Part);
        mass += part->mass;
        if (part->thrust.z < 0.0f) thrust_forward -= part->thrust.z;
        if (part->thrust.y > 0.0f) thrust_up += part->thrust.y;
        ++parts;
    }
    CHECK(parts > 20);
    CHECK(fabsf(c->mass - mass) < 0.5f);
    CHECK(fabsf(c->thrust[CRAFT_GROUP_FORWARD] - thrust_forward) < 1.0f);
    CHECK(fabsf(c->thrust[CRAFT_GROUP_UP] - thrust_up) < 1.0f);
    CHECK(c->thrust[CRAFT_GROUP_UP] > c->mass * 20.0f * 1.3f);          // it can lift itself, with some to spare
    CHECK(c->inertia.x > 0.0f && c->inertia.y > c->inertia.z);          // long and flat: yawing is the heaviest
    CHECK(c->gyro.x > 0.0f && c->radius > 5.0f && c->draft > 1.0f);
    CHECK(fabsf(c->center.x) < 0.01f);                                  // symmetric port to starboard
    // Its main engines sit below the centre of mass, so firing them pitches the bow
    // up: a torque about +x, which the gyros must hold.
    CHECK(c->thrust_torque[CRAFT_GROUP_FORWARD].x > 1000.0f);

    // At rest on its berth, and it stays there, the player having walked aboard.
    CHECK(c->resting);
    CHECK(game_aboard(g, player) == craft);
    v3 parked = transform_of(g, craft)->position;
    run_seconds(g, 2.0f);
    CHECK(v3_length(v3_sub(transform_of(g, craft)->position, parked)) < 0.001f);
    CHECK(fabsf(lowest_point(g, craft) - g->berths[0].floor.y) < 0.01f);

    // Calling it again while it is out tows it home.
    transform_of(g, craft)->position = v3_add(parked, v3_make(30, 5, 0));
    craft_of(g, craft)->resting = false;
    cw_entity_id other = game_spawn_player(g, g->spawn, true);
    game_set_owner(g, other, 43);
    CHECK(game_call_craft(g, player, NULL) == craft);
    CHECK(v3_length(v3_sub(transform_of(g, craft)->position, parked)) < 0.001f);
    CHECK(game_aboard(g, player) == craft);   // passengers go with it
    // Someone else's call gets a craft of their own at the next berth.
    cw_entity_id second = game_call_craft(g, other, NULL);
    CHECK(second != CW_INVALID_ENTITY && second != craft);
    CHECK(craft_of(g, second)->berth == 1 && game_owner(g, second) == 43);

    // Walking into its side from the yard, in the world's frame: its hull stops
    // them, in its own frame, and they are not aboard for having touched it.
    v3 beside = game_craft_to_world(g, second, v3_make(6.0f, 0, 0));
    beside.y = 0.0f;
    transform_of(g, other)->position = transform_of(g, other)->previous = beside;
    run_seconds(g, 0.3f);
    v3 middle = transform_of(g, second)->position;
    walk_to(g, other, middle.x, middle.z, 2.0f);
    v3 stopped = game_craft_from_world(g, second, transform_of(g, other)->position);
    CHECK(game_aboard(g, other) == CW_INVALID_ENTITY);
    CHECK(stopped.x > 2.0f && stopped.x < 2.7f);
    CHECK(fabsf(transform_of(g, other)->position.y) < 0.01f);

    // Fallen below everything there is, a craft is towed home.
    craft_of(g, second)->resting = false;
    transform_of(g, second)->position = v3_make(0, CRAFT_RECOVERY_DEPTH - 10.0f, 0);
    game_tick(g);
    CHECK(v3_length(v3_sub(transform_of(g, second)->position, middle)) < 0.001f);
    CHECK(craft_of(g, second)->resting);
    game_destroy(g);
    free(g);
}

// Boarding, the helm, and flying: lift off, hover, fly forward and stop, turn,
// decoupled flight, and a landing with nobody at the helm.
static void test_craft_flight(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    cw_entity_id player = game_spawn_player(g, g->spawn, true);
    game_set_owner(g, player, 7);
    cw_entity_id stranger = game_spawn_player(g, g->spawn, true);
    game_set_owner(g, stranger, 8);
    cw_entity_id craft = call_and_board(g, player);
    if (craft == CW_INVALID_ENTITY) { game_destroy(g); free(g); return; }
    Craft *c = craft_of(g, craft);

    // The stranger walks aboard too, but the helm is not theirs to take.
    transform_of(g, stranger)->position = transform_of(g, player)->previous;
    board_craft(g, stranger, craft);
    v3 deck = game_craft_to_world(g, craft, v3_make(1.0f, 0, -2.5f));
    CHECK(walk_to(g, stranger, deck.x, deck.z, 10.0f));
    CHECK(game_aboard(g, stranger) == craft);
    v3 helm = game_craft_to_world(g, craft, c->helm);
    CHECK(walk_to(g, stranger, helm.x, helm.z, 5.0f));
    press_use(g, stranger);
    CHECK(c->pilot == CW_INVALID_ENTITY);
    deck = game_craft_to_world(g, craft, v3_make(1.2f, 0, 2.0f));
    CHECK(walk_to(g, stranger, deck.x, deck.z, 5.0f));

    // Its owner takes it, round the mast.
    v3 beside_mast = game_craft_to_world(g, craft, v3_make(-0.8f, 0, -0.5f));
    CHECK(walk_to(g, player, beside_mast.x, beside_mast.z, 5.0f));
    CHECK(walk_to(g, player, helm.x, helm.z, 5.0f));
    press_use(g, player);
    CHECK(c->pilot == player);
    PlayerInput *in = input_of(g, player);

    // Lift off: up, and everyone aboard goes with it, standing where they stood.
    v3 start = transform_of(g, craft)->position;
    v3 stranger_local = CW_GET(&g->world, stranger, g->c_aboard, Aboard)->position;
    in->lift = 1.0f;
    run_seconds(g, 2.0f);
    in->lift = 0.0f;
    CHECK(!c->resting);
    CHECK(transform_of(g, craft)->position.y > start.y + 8.0f);
    CHECK(v3_length(v3_sub(CW_GET(&g->world, stranger, g->c_aboard, Aboard)->position, stranger_local)) < 0.05f);
    CHECK(v3_length(v3_sub(transform_of(g, player)->position, game_craft_to_world(g, craft, c->helm))) < 0.001f);
    CHECK(player_of(g, stranger)->grounded);

    // Let go, the flight computer holds it where it is against gravity.
    run_seconds(g, 3.0f);
    float hover = transform_of(g, craft)->position.y;
    run_seconds(g, 2.0f);
    CHECK(fabsf(transform_of(g, craft)->position.y - hover) < 0.3f);
    CHECK(v3_length(c->velocity) < 0.3f);
    CHECK(v3_length(c->spin) < 0.05f);
    CHECK(c->throttle[CRAFT_GROUP_UP] > 0.5f && c->throttle[CRAFT_GROUP_UP] < 0.9f);   // gravity, held off

    // Forward: along its bow, to its top speed; let go, and it stops.
    v3 bow = bow_of(g, craft);
    in->forward = 1.0f;
    run_seconds(g, 3.0f);
    CHECK(v3_dot(c->velocity, bow) > 25.0f);
    CHECK(fabsf(c->velocity.y) < 2.0f);
    in->forward = 0.0f;
    run_seconds(g, 5.0f);   // the retro thrusters are weaker than the main engines
    CHECK(v3_length(c->velocity) < 1.0f);
    CHECK(v3_length(v3_sub(CW_GET(&g->world, stranger, g->c_aboard, Aboard)->position, stranger_local)) < 0.05f);

    // Turning right swings the bow towards +x (it starts facing -z), the passengers with it.
    in->turn_yaw = 1.0f;
    run_seconds(g, 1.0f);
    in->turn_yaw = 0.0f;
    run_seconds(g, 1.0f);
    v3 turned = bow_of(g, craft);
    CHECK(turned.x > 0.4f);
    CHECK(fabsf(turned.y) < 0.1f);
    CHECK(v3_length(c->spin) < 0.05f);
    CHECK(game_aboard(g, stranger) == craft);
    v3 stranger_world = transform_of(g, stranger)->position;
    CHECK(v3_length(v3_sub(stranger_world, game_craft_to_world(g, craft, CW_GET(&g->world, stranger, g->c_aboard, Aboard)->position))) < 0.001f);
    // Pitching up raises the bow; the flight computer levels nothing by itself, so
    // down again by as much.
    in->turn_pitch = 1.0f;
    run_seconds(g, 0.5f);
    in->turn_pitch = 0.0f;
    run_seconds(g, 0.5f);
    CHECK(bow_of(g, craft).y > 0.2f);
    in->turn_pitch = -1.0f;
    run_seconds(g, 0.5f);
    in->turn_pitch = 0.0f;
    run_seconds(g, 0.5f);
    CHECK(fabsf(bow_of(g, craft).y) < 0.1f);

    // Decoupled, nothing holds it: a push forward is kept after letting go, and
    // gravity is not held off.
    in->decoupled = true;
    in->forward = 1.0f;
    run_seconds(g, 0.5f);
    in->forward = 0.0f;
    v3 coasting = c->velocity;
    float speed = v3_dot(coasting, bow_of(g, craft));
    CHECK(speed > 4.0f);
    run_seconds(g, 0.5f);
    // Across the ground nothing acts on it now (the solar current is off): it coasts.
    // (Its bow does not go on as it was: the main engines sit below the centre of
    // mass, so the push pitched it up, and decoupled, nothing stops the turn.)
    v3 change = v3_sub(c->velocity, coasting);
    change.y = 0.0f;
    CHECK(v3_length(change) < 0.05f);
    CHECK(c->velocity.y < coasting.y - 5.0f);   // and it falls
    CHECK(v3_length(c->spin) > 0.01f);
    in->decoupled = false;
    run_seconds(g, 3.0f);
    CHECK(v3_length(c->velocity) < 1.0f);

    // Fly back over the yard, and leave the helm: nothing holds it up any more, it
    // falls, lands on the yard and comes to rest there, its crew still aboard.
    v3 home = v3_add(g->berths[2].floor, v3_make(0, 8.0f, 0));
    for (int i = 0; i < 60 * 30; ++i) {
        v3 local = quat_unrotate(c->orientation, v3_sub(home, transform_of(g, craft)->position));
        float *axes[3] = {&in->strafe, &in->lift, &in->forward};
        float gaps[3] = {local.x, local.y, -local.z};
        for (int k = 0; k < 3; ++k) {
            float command = gaps[k] * 0.08f;
            *axes[k] = command > 1.0f ? 1.0f : command < -1.0f ? -1.0f : command;
        }
        game_tick(g);
        if (v3_length(local) < 0.5f && v3_length(c->velocity) < 0.3f) break;
    }
    in->forward = in->strafe = in->lift = 0.0f;
    CHECK(fabsf(transform_of(g, craft)->position.x - home.x) < 1.5f && fabsf(transform_of(g, craft)->position.z - home.z) < 1.5f);
    press_use(g, player);
    CHECK(c->pilot == CW_INVALID_ENTITY);
    int ticks = 0;
    while (!c->resting && ticks++ < 60 * 10) game_tick(g);
    CHECK(c->resting);
    CHECK(fabsf(lowest_point(g, craft) - g->berths[2].floor.y) < 0.05f);
    CHECK(game_aboard(g, player) == craft && game_aboard(g, stranger) == craft);
    CHECK(fabsf(bow_of(g, craft).y) < 0.05f);

    // Down the stairs and off: the world's frame again, where the craft left them.
    v3 gangway = game_craft_to_world(g, craft, v3_make(0, 0, 3.5f));
    CHECK(walk_to(g, stranger, gangway.x, gangway.z, 5.0f));
    v3 off = game_craft_to_world(g, craft, v3_make(0, 0, 9.0f));
    CHECK(walk_to(g, stranger, off.x, off.z, 10.0f));
    CHECK(game_aboard(g, stranger) == CW_INVALID_ENTITY);
    run_seconds(g, 0.5f);
    CHECK(player_of(g, stranger)->grounded);
    CHECK(fabsf(transform_of(g, stranger)->position.y) < 0.05f);   // on the yard
    game_destroy(g);
    free(g);
}

// Gravity: the hub's down over the hub and the yard, nothing in open space, and
// each planet pulling towards its centre, weaker with height, gone by its reach.
static void test_gravity(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    v3 hub = game_gravity(g, v3_make(0, 5, 0)), yard = game_gravity(g, g->berths[7].floor);
    CHECK(fabsf(hub.y + 20.0f) < 0.001f && fabsf(hub.x) + fabsf(hub.z) < 0.001f);
    CHECK(fabsf(yard.y + 20.0f) < 0.001f);
    CHECK(v3_length(game_gravity(g, v3_make(3000, 0, 3000))) < 1e-6f);   // open space
    cw_entity_id verdant = CW_INVALID_ENTITY;
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (cw_query_next(&planets, &id))
        if (!strcmp(CW_GET(&g->world, id, g->c_planet, Planet)->name, "Verdant")) verdant = id;
    CHECK(verdant != CW_INVALID_ENTITY);
    if (verdant == CW_INVALID_ENTITY) { game_destroy(g); free(g); return; }
    v3 centre = transform_of(g, verdant)->position;
    float radius = CW_GET(&g->world, verdant, g->c_planet, Planet)->land.radius;
    v3 side = v3_normalize(v3_make(-1.0f, 0.3f, 0.5f));   // away from the other planets
    v3 low = game_gravity(g, v3_add(centre, v3_scale(side, radius))), high = game_gravity(g, v3_add(centre, v3_scale(side, radius * 1.5f)));
    CHECK(fabsf(v3_length(low) - 20.0f) < 0.01f);
    CHECK(v3_dot(v3_normalize(low), side) < -0.9999f);                   // towards the centre, wherever on it
    CHECK(fabsf(v3_length(high) - 20.0f / 2.25f) < 0.05f);               // the inverse square
    CHECK(v3_length(game_gravity(g, v3_add(centre, v3_scale(side, radius * (GAME_WELL_REACH + 0.01f))))) < 1e-6f);
    // No steps anywhere on the way out: the fades are smooth.
    float worst = 0.0f;
    for (float r = radius; r < radius * 4.0f; r += 0.5f) {
        float a = v3_length(game_gravity(g, v3_add(centre, v3_scale(side, r))));
        float b = v3_length(game_gravity(g, v3_add(centre, v3_scale(side, r + 0.5f))));
        if (fabsf(a - b) > worst) worst = fabsf(a - b);
    }
    CHECK(worst < 0.1f);
    game_destroy(g);
    free(g);
}

// How far a craft's up is from the way up gravity gives it, as the cosine.
static float level_with_gravity(game *g, cw_entity_id craft) {
    v3 up = quat_rotate(craft_of(g, craft)->orientation, v3_make(0, 1, 0));
    return -v3_dot(up, v3_normalize(game_gravity(g, transform_of(g, craft)->position)));
}

// Landing on a planet's side, well round from its pad: the computer turns the craft
// to the planet's way up, brings it down slowly at the end, and powers it down once
// it is down; asked to, it lifts off again.
static void test_craft_planet_landing(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    cw_entity_id verdant = CW_INVALID_ENTITY;
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    while (cw_query_next(&planets, &id))
        if (!strcmp(CW_GET(&g->world, id, g->c_planet, Planet)->name, "Verdant")) verdant = id;
    if (verdant == CW_INVALID_ENTITY) { CHECK(false); game_destroy(g); free(g); return; }
    const PlanetLand *land = &CW_GET(&g->world, verdant, g->c_planet, Planet)->land;
    v3 centre = transform_of(g, verdant)->position;
    // Open sea, a third of the way round: flat, and tilted well away from the world's up.
    v3 spot = v3_make(0, 0, 0), up = v3_make(0, 1, 0);
    for (float heading = 0.0f; heading < 6.28f && up.y > 0.95f; heading += 0.1f) {
        v3 point = game_planet_point(land, centre, v3_make(sinf(heading), 0, cosf(heading)), land->radius * 0.7f);
        v3 direction = v3_normalize(v3_sub(point, centre));
        if (game_planet_height(land, direction) == 0.0f) {
            spot = point;
            up = direction;
        }
    }
    CHECK(up.y < 0.95f);
    cw_entity_id craft = game_spawn_craft(g, NULL, v3_add(spot, v3_scale(up, 40.0f)), 0.0f, 0, -1);   // level with the world, not the planet
    Craft *c = craft_of(g, craft);
    c->resting = false;
    game_tick(g);
    cw_entity_id pilot = game_spawn_player(g, game_craft_to_world(g, craft, v3_make(0, 0.05f, -3.0f)), true);
    game_tick(g);
    press_use(g, pilot);
    CHECK(c->pilot == pilot);
    CHECK(level_with_gravity(g, craft) < 0.96f);
    PlayerInput *in = input_of(g, pilot);
    in->landing = true;
    run_seconds(g, 3.0f);
    CHECK(c->mode == CRAFT_LANDING && c->assist);
    CHECK(level_with_gravity(g, craft) > 0.999f);      // turned to the planet's way up
    float hovering = game_craft_altitude(g, craft);
    CHECK(hovering > 20.0f && hovering < 45.0f);
    // Down: fast while high, slowly at the end, and powered down once there.
    in->lift = -1.0f;
    float fastest = 0.0f, landing_speed = 0.0f;
    int ticks = 0;
    while (!c->resting && ticks++ < 60 * 30) {
        game_tick(g);
        float sinking = -game_craft_climb(g, craft), altitude = game_craft_altitude(g, craft);
        if (sinking > fastest) fastest = sinking;
        if (altitude < 0.5f && sinking > landing_speed) landing_speed = sinking;
    }
    CHECK(c->resting);
    CHECK(fastest > CRAFT_LANDING_SINK * 0.8f);
    CHECK(landing_speed < CRAFT_TOUCHDOWN_SPEED + 0.3f);
    CHECK(game_craft_altitude(g, craft) < CRAFT_TOUCHDOWN_HEIGHT);
    CHECK(level_with_gravity(g, craft) > 0.99f);   // about level: resting on the land where it came down, slope and all
    CHECK(c->pilot == pilot && game_aboard(g, pilot) == craft);
    for (int k = 0; k < CRAFT_GROUPS; ++k) CHECK(c->throttle[k] == 0.0f);
    v3 landed = transform_of(g, craft)->position;
    run_seconds(g, 2.0f);
    CHECK(v3_length(v3_sub(transform_of(g, craft)->position, landed)) < 0.001f);   // stays down, holding C or not
    in->lift = 0.0f;
    run_seconds(g, 0.5f);
    CHECK(c->resting);
    // Up again when asked.
    in->lift = 1.0f;
    run_seconds(g, 1.5f);
    CHECK(!c->resting && game_craft_altitude(g, craft) > 3.0f);
    CHECK(c->throttle[CRAFT_GROUP_UP] > 0.0f);
    game_destroy(g);
    free(g);
}

// Flying, the wings come level once the roll is let go, where there is gravity to
// say which way is level; in open space a roll stays where it was put.
static void test_craft_levelling(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    v3 places[2] = {v3_make(-60.0f, 60.0f, -60.0f), v3_make(3000.0f, 0.0f, 3000.0f)};
    for (int k = 0; k < 2; ++k) {
        cw_entity_id craft = game_spawn_craft(g, NULL, places[k], 0.0f, 0, -1);
        craft_of(g, craft)->resting = false;
        game_tick(g);
        cw_entity_id pilot = game_spawn_player(g, game_craft_to_world(g, craft, v3_make(0, 0.05f, -3.0f)), true);
        game_tick(g);
        press_use(g, pilot);
        CHECK(craft_of(g, craft)->pilot == pilot);
        PlayerInput *in = input_of(g, pilot);
        in->turn_roll = 1.0f;
        run_seconds(g, 0.5f);
        in->turn_roll = 0.0f;
        run_seconds(g, 0.3f);
        v3 right = quat_rotate(craft_of(g, craft)->orientation, v3_make(1, 0, 0));
        CHECK(right.y < -0.3f);   // banked right
        run_seconds(g, 3.0f);
        right = quat_rotate(craft_of(g, craft)->orientation, v3_make(1, 0, 0));
        if (k == 0) CHECK(fabsf(right.y) < 0.02f);   // level again
        else CHECK(right.y < -0.3f);                 // no way up to level to
        CHECK(v3_length(craft_of(g, craft)->spin) < 0.05f);
    }
    game_destroy(g);
    free(g);
}

// An owner puts players off their craft: behind its stern, on the ground when it
// is down, barred from coming back for a while; clearing the deck spares the owner.
static void test_craft_kick(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    cw_entity_id owner = game_spawn_player(g, g->spawn, true), bruno = game_spawn_player(g, g->spawn, true);
    cw_entity_id carol = game_spawn_player(g, g->spawn, true);
    game_set_owner(g, owner, 1);
    game_set_owner(g, bruno, 2);
    game_set_owner(g, carol, 3);
    cw_entity_id craft = call_and_board(g, owner);
    if (craft == CW_INVALID_ENTITY) { game_destroy(g); free(g); return; }
    Craft *c = craft_of(g, craft);
    cw_entity_id riders[2] = {bruno, carol};
    for (int i = 0; i < 2; ++i) {
        transform_of(g, riders[i])->position = transform_of(g, owner)->previous;
        board_craft(g, riders[i], craft);
        CHECK(game_aboard(g, riders[i]) == craft);
    }
    cw_entity_id crew[8];
    CHECK(game_craft_crew(g, craft, crew, 8) == 3);

    // Bruno is put off: behind the stern, on the yard, and the deck is closed to him.
    game_kick(g, craft, bruno);
    CHECK(game_aboard(g, bruno) == CW_INVALID_ENTITY);
    CHECK(!game_may_board(g, bruno, craft) && game_may_board(g, carol, craft));
    run_seconds(g, 0.5f);
    v3 behind = game_craft_from_world(g, craft, transform_of(g, bruno)->position);
    CHECK(behind.z > c->cabin_center.z + c->cabin_half.z + 2.0f);
    CHECK(fabsf(transform_of(g, bruno)->position.y) < 0.05f && player_of(g, bruno)->grounded);
    // He walks back up the stairs and cannot get on: the deck stops him at the gangway.
    v3 deck = game_craft_to_world(g, craft, v3_make(0, 0, 1.0f));
    bool ever_in = false;
    PlayerInput *in = input_of(g, bruno);
    for (int i = 0; i < 60 * 4; ++i) {
        v3 way = v3_sub(deck, transform_of(g, bruno)->position);
        in->yaw = atan2f(-way.x, -way.z);
        in->forward = 1.0f;
        game_tick(g);
        v3 local = game_craft_from_world(g, craft, transform_of(g, bruno)->position);
        if (game_aboard(g, bruno) != CW_INVALID_ENTITY
            || (fabsf(local.x - c->cabin_center.x) < c->cabin_half.x && fabsf(local.z - c->cabin_center.z) < c->cabin_half.z
                && fabsf(local.y - c->cabin_center.y) < c->cabin_half.y)) ever_in = true;
    }
    in->forward = 0.0f;
    CHECK(!ever_in);
    v3 stopped = game_craft_from_world(g, craft, transform_of(g, bruno)->position);
    CHECK(stopped.y > -0.35f && stopped.z < c->cabin_half.z + PLAYER_RADIUS + 0.05f);   // up the stairs, to the gangway and no further

    // Clearing the deck: everyone but the owner.
    CHECK(game_kick_all(g, craft, owner) == 1);
    CHECK(game_aboard(g, carol) == CW_INVALID_ENTITY && game_aboard(g, owner) == craft);
    CHECK(game_craft_crew(g, craft, crew, 8) == 1 && crew[0] == owner);

    // A minute later the bar lifts, and Bruno boards as anyone would.
    run_seconds(g, (float)CRAFT_BAR_SECONDS + 0.5f);
    CHECK(game_may_board(g, bruno, craft));
    transform_of(g, bruno)->position = transform_of(g, owner)->previous;   // somewhere clear of the craft
    transform_of(g, bruno)->position = g->places[0].spawn;
    for (int i = 0; i < g->place_count; ++i)
        if (g->places[i].kind == PLACE_YARD) transform_of(g, bruno)->position = g->places[i].spawn;
    board_craft(g, bruno, craft);
    CHECK(game_aboard(g, bruno) == craft);

    // Put off in flight, he goes over the side moving as the ship did, and falls.
    v3 helm = game_craft_to_world(g, craft, c->helm);
    v3 beside_mast = game_craft_to_world(g, craft, v3_make(-0.8f, 0, -0.5f));
    walk_to(g, owner, beside_mast.x, beside_mast.z, 5.0f);
    walk_to(g, owner, helm.x, helm.z, 5.0f);
    press_use(g, owner);
    CHECK(c->pilot == owner);
    input_of(g, owner)->lift = 1.0f;
    run_seconds(g, 1.5f);
    float rising = c->velocity.y;
    CHECK(rising > 5.0f);
    game_kick(g, craft, bruno);
    CHECK(game_aboard(g, bruno) == CW_INVALID_ENTITY);
    CHECK(fabsf(player_of(g, bruno)->velocity.y - rising) < 1.0f);
    float height = transform_of(g, bruno)->position.y;
    run_seconds(g, 3.0f);
    CHECK(transform_of(g, bruno)->position.y < height);
    CHECK(fabsf(transform_of(g, bruno)->position.y) < 0.05f);   // down on the yard
    CHECK(c->pilot == owner);                                    // the helm is the owner's still
    game_destroy(g);
    free(g);
}

// The chase camera: behind and above the craft, looking ahead of it, and pulled in
// when something stands between it and the craft.
static void test_chase_view(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    v3 floor = v3_make(-20.0f, 0.0f, -10.0f);
    cw_entity_id craft = game_spawn_craft(g, NULL, floor, 0.0f, 0, -1);
    game_tick(g);
    const Craft *c = craft_of(g, craft);
    v3 origin = transform_of(g, craft)->position, eye, forward, up;
    game_chase_view(g, craft, origin, c->orientation, &eye, &forward, &up);
    v3 local = game_craft_from_world(g, craft, eye);
    CHECK(fabsf(local.z - c->radius * CRAFT_CHASE_DISTANCE) < 0.01f && local.y > 2.0f);
    CHECK(forward.z < -0.9f && up.y > 0.99f);   // looking out over its bow
    CHECK(fabsf(game_raycast(g, v3_make(0, 10, 0), v3_make(0, -1, 0), 50.0f) - 10.0f) < 0.01f);   // the hub's top
    CHECK(game_raycast(g, v3_make(0, 10, 0), v3_make(0, 1, 0), 50.0f) == 50.0f);                    // nothing above
    // A wall behind it: the camera comes in to this side of the wall.
    float wall_z = origin.z + c->radius * 1.2f;
    cw_entity_id wall = cw_world_create_entity(&g->world);
    Transform t = {v3_make(origin.x, 5.0f, wall_z + 0.5f), v3_make(origin.x, 5.0f, wall_z + 0.5f)};
    Solid solid = {v3_make(10.0f, 5.0f, 0.5f)};
    cw_comp_set(&g->world, wall, g->c_transform, &t, sizeof(t));
    cw_comp_set(&g->world, wall, g->c_solid, &solid, sizeof(solid));
    game_tick(g);
    game_chase_view(g, craft, origin, c->orientation, &eye, &forward, &up);
    CHECK(eye.z < wall_z && eye.z > origin.z);
    // Turned with the craft: rolled a quarter over, the camera's up is its up.
    quat rolled = quat_from_axis_angle(v3_make(0, 0, 1), 1.5707963f);
    game_chase_view(g, craft, v3_make(0, 200, 0), rolled, &eye, &forward, &up);
    CHECK(up.x < -0.99f);
    game_destroy(g);
    free(g);
}

// The solar current: flowing everywhere, within its bounds, in ways that differ
// across the system, smoothly, and drifting with time.
static void test_solar_current(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    g->currents = true;   // off by default, for now
    game_build_universe(g);
    double t = 1.7e9;   // universe time is large: the phases must not suffer for it
    float slowest = 1e9f, fastest = 0.0f, widest = 1.0f;
    v3 first = game_solar_current(g, v3_make(-3000, 0, -3000), t);
    for (float x = -3000.0f; x <= 3000.0f; x += 250.0f)
        for (float z = -3000.0f; z <= 3000.0f; z += 250.0f) {
            v3 flow = game_solar_current(g, v3_make(x, (x - z) * 0.1f, z), t);
            float speed = v3_length(flow);
            if (speed < slowest) slowest = speed;
            if (speed > fastest) fastest = speed;
            float agree = v3_dot(v3_normalize(flow), v3_normalize(first));
            if (agree < widest) widest = agree;
            CHECK(fabsf(flow.y) < speed * 0.2f);   // mostly across the system
            // Smooth: a ship's length away, nearly the same (a few percent of the most it flows).
            v3 near = game_solar_current(g, v3_make(x + 10.0f, (x - z) * 0.1f, z + 10.0f), t);
            CHECK(v3_length(v3_sub(near, flow)) < SOLAR_CURRENT_MAX * 0.05f);
        }
    CHECK(slowest >= SOLAR_CURRENT_MIN - 0.01f && fastest <= SOLAR_CURRENT_MAX + 0.01f);
    CHECK(fastest - slowest > 10.0f);   // strong here, weak there
    CHECK(widest < -0.5f);              // somewhere it flows the other way
    // Over time, the same place's flow drifts, a little in a second, much in minutes.
    v3 here = v3_make(120.0f, 30.0f, -800.0f);
    v3 now = game_solar_current(g, here, t), second = game_solar_current(g, here, t + 1.0), later = game_solar_current(g, here, t + 240.0);
    CHECK(v3_length(v3_sub(second, now)) < 1.0f);
    CHECK(v3_length(v3_sub(later, now)) > 3.0f);
    CHECK(memcmp(&now, &second, sizeof(v3)) != 0);
    game_destroy(g);
    free(g);
}

// A craft heading where the current flows (or into it), its helm held, and a hand
// at its sails.
static cw_entity_id sailing_craft(game *g, v3 at, bool into, cw_entity_id *pilot, cw_entity_id *hand) {
    v3 flow = game_solar_current_at_tick(g, at, g->world.tick);
    float heading = atan2f(-flow.x, -flow.z) + (into ? TP_PI : 0.0f);
    cw_entity_id craft = game_spawn_craft(g, NULL, at, heading, 0, -1);
    craft_of(g, craft)->resting = false;
    game_tick(g);
    *pilot = game_spawn_player(g, game_craft_to_world(g, craft, v3_make(0, 0.05f, -3.0f)), true);
    *hand = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(craft_of(g, craft)->sails[0].station, v3_make(0, 0.05f, 0))), true);
    game_tick(g);
    press_use(g, *pilot);
    press_use(g, *hand);
    CHECK(craft_of(g, craft)->pilot == *pilot && craft_of(g, craft)->sails[0].hand == *hand);
    return craft;
}

// Hoisting the sail, and bracing it to its best for the current, from the sail station.
static void set_sail(game *g, cw_entity_id craft, cw_entity_id hand) {
    PlayerInput *in = input_of(g, hand);
    in->sail = 1.0f;
    for (int i = 0; i < 60 * 4; ++i) {
        float best, speed;
        game_craft_sailing(g, craft, 0, &best, &speed);
        float error = best - craft_of(g, craft)->sails[0].brace;
        in->brace = error > 0.02f ? 1.0f : error < -0.02f ? -1.0f : 0.0f;
        game_tick(g);
    }
    in->sail = in->brace = 0.0f;
    CHECK(craft_of(g, craft)->sails[0].set == 1.0f);
}

// Sails: worked from the sail station, not the helm. Running before the current the
// craft is driven towards the current's speed along its heading, the computer
// keeping its height and killing its slip; left so, it sails on with nobody aboard
// at helm or sails. Braced well it makes headway against the current, slower; square,
// hardly any. With the sail in and no engines the current carries it astern; with
// the computer at work, it holds its place.
// With the currents off (as they are for now): no flow anywhere, nothing drifts,
// and a sail set and braced draws nothing and drives nothing.
static void test_currents_off(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    CHECK(!g->currents);
    v3 at = v3_make(-60.0f, 40.0f, -40.0f);
    CHECK(v3_length(game_solar_current(g, at, 1234.5)) == 0.0f);
    cw_entity_id craft = game_spawn_craft(g, NULL, at, 0.0f, 0, -1);
    craft_of(g, craft)->resting = false;
    game_tick(g);
    cw_entity_id hand = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(craft_of(g, craft)->sails[0].station, v3_make(0, 0.05f, 0))), true);
    game_tick(g);
    press_use(g, hand);
    cw_entity_id pilot = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(craft_of(g, craft)->helm, v3_make(0, 0.05f, 0))), true);
    game_tick(g);
    press_use(g, pilot);
    input_of(g, hand)->sail = 1.0f;
    run_seconds(g, 6.0f);
    const Craft *c = craft_of(g, craft);
    CHECK(c->sails[0].set == 1.0f && c->sails[0].fill == 0.0f);   // set, and drawing nothing
    CHECK(v3_length(c->velocity) < 0.05f);                          // held where it was
    float best = 1.0f, speed = 1.0f;
    game_craft_sailing(g, craft, 0, &best, &speed);
    CHECK(speed == 0.0f);
    game_destroy(g);
    free(g);
}

static void test_craft_sails(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    g->currents = true;   // off by default, for now
    game_build_universe(g);
    cw_entity_id pilot, hand;
    cw_entity_id craft = sailing_craft(g, v3_make(-60.0f, 40.0f, -40.0f), false, &pilot, &hand);
    Craft *c = craft_of(g, craft);
    CHECK(c->sails[0].set == 0.0f && c->sails[0].area > 10.0f && c->sails[0].center.y > 3.0f);
    // The helm does not work the sail.
    input_of(g, pilot)->sail = 1.0f;
    run_seconds(g, 1.0f);
    input_of(g, pilot)->sail = 0.0f;
    CHECK(c->sails[0].set == 0.0f);
    // The hand does, at its rate.
    input_of(g, hand)->sail = 1.0f;
    run_seconds(g, 1.0f);
    CHECK(fabsf(c->sails[0].set - CRAFT_SAIL_RATE) < 0.02f);
    input_of(g, hand)->sail = 0.0f;
    set_sail(g, craft, hand);
    float height = transform_of(g, craft)->position.y;
    run_seconds(g, 8.0f);
    v3 bow = bow_of(g, craft);
    float best, target;
    game_craft_sailing(g, craft, 0, &best, &target);
    float ahead = v3_dot(c->velocity, bow);
    CHECK(target > 10.0f && fabsf(best) < 0.2f);   // running before it: square is best
    CHECK(ahead > target * 0.6f && ahead < target + 0.5f);
    CHECK(c->sails[0].fill > 0.9f);
    CHECK(c->throttle[CRAFT_GROUP_FORWARD] < 0.05f && c->throttle[CRAFT_GROUP_BACK] < 0.05f);   // the sail's work
    CHECK(fabsf(transform_of(g, craft)->position.y - height) < 0.5f);
    CHECK(v3_length(v3_sub(c->velocity, v3_scale(bow, ahead))) < 0.5f);   // no slip
    // Left so, helm and sails both: it sails on.
    press_use(g, pilot);
    press_use(g, hand);
    CHECK(c->pilot == CW_INVALID_ENTITY && c->sails[0].hand == CW_INVALID_ENTITY);
    v3 from = transform_of(g, craft)->position;
    run_seconds(g, 3.0f);
    CHECK(v3_dot(v3_sub(transform_of(g, craft)->position, from), bow) > ahead * 2.5f);
    CHECK(fabsf(transform_of(g, craft)->position.y - height) < 0.5f);

    // Into the current, braced to its best: headway, slower than running. Square: little.
    float made[2];
    for (int k = 0; k < 2; ++k) {
        cw_entity_id into_pilot, into_hand;
        cw_entity_id into = sailing_craft(g, v3_make(150.0f + 200.0f * (float)k, 60.0f, 150.0f), true, &into_pilot, &into_hand);
        if (k == 0) set_sail(g, into, into_hand);
        else {
            input_of(g, into_hand)->sail = 1.0f;
            run_seconds(g, 2.5f);
            input_of(g, into_hand)->sail = 0.0f;
        }
        run_seconds(g, 10.0f);
        float flow = v3_length(game_solar_current_at_tick(g, transform_of(g, into)->position, g->world.tick));
        made[k] = v3_dot(craft_of(g, into)->velocity, bow_of(g, into));
        if (k == 0) {
            CHECK(made[k] > 0.2f * flow && made[k] < 0.45f * flow);   // headway, at four tenths of the current at most
            CHECK(fabsf(craft_of(g, into)->sails[0].brace) > 1.0f);           // braced hard round
        }
    }
    CHECK(made[1] < made[0] * 0.5f);

    // Sail in and nobody at the helm, out in open space: the current carries it astern.
    cw_entity_id drift_pilot, drift_hand;
    cw_entity_id adrift = sailing_craft(g, v3_make(3000.0f, 0.0f, 3000.0f), true, &drift_pilot, &drift_hand);
    press_use(g, drift_pilot);
    run_seconds(g, 10.0f);
    v3 flow = game_solar_current_at_tick(g, transform_of(g, adrift)->position, g->world.tick);
    CHECK(v3_dot(craft_of(g, adrift)->velocity, bow_of(g, adrift)) < -5.0f);
    CHECK(v3_dot(v3_normalize(craft_of(g, adrift)->velocity), v3_normalize(flow)) > 0.95f);   // with the flow
    // The owner back at the helm, the computer at work: it holds its place against it.
    press_use(g, drift_pilot);
    CHECK(craft_of(g, adrift)->pilot == drift_pilot);
    run_seconds(g, 8.0f);
    CHECK(v3_length(craft_of(g, adrift)->velocity) < 0.5f);
    // Put off the ship, the hand leaves the sails.
    game_kick(g, adrift, drift_hand);
    CHECK(craft_of(g, adrift)->sails[0].hand == CW_INVALID_ENTITY);
    game_destroy(g);
    free(g);
}

// The chart fits the whole universe, the hub and every planet whole, in its ball.
static void test_chart_fit(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    v3 centre;
    float extent;
    game_chart_fit(g, &centre, &extent);
    CHECK(v3_length(centre) < extent);   // the hub is in it
    cw_query planets = cw_query_begin(g->q_planets);
    cw_entity_id id;
    int seen = 0;
    while (cw_query_next(&planets, &id)) {
        float reach = v3_length(v3_sub(transform_of(g, id)->position, centre)) + CW_GET(&g->world, id, g->c_planet, Planet)->land.radius;
        CHECK(reach < extent && reach > extent * 0.5f);   // in, and not lost in the middle
        ++seen;
    }
    CHECK(seen == 3);
    cw_entity_id craft = game_spawn_craft(g, NULL, g->berths[0].floor, 0.0f, 0, 0);
    cw_entity_id charts[4];
    CHECK(game_craft_charts(g, craft, charts, 4) == 1);
    CHECK(CW_GET(&g->world, charts[0], g->c_part, Part)->kind == PART_CHART);
    game_destroy(g);
    free(g);
}

// ---------------------------------------------------------------------------
// Ships: hulls, fittings, and what they make of a craft
// ---------------------------------------------------------------------------
static int count_slots(const ship_slot *slots, int n, int kind) {
    int count = 0;
    for (int i = 0; i < n; ++i) count += slots[i].kind == kind;
    return count;
}
static int first_slot(int hull, int kind) {
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(hull, slots, SHIP_MAX_SLOTS);
    for (int i = 0; i < n; ++i)
        if (slots[i].kind == kind) return i;
    return -1;
}

// Every hull's slots are what its class says, its standard loadout fills them all,
// and a loadout goes over the wire and back; what cannot be built is refused.
static void test_ship_loadouts(void) {
    const int crews[HULL_COUNT] = {1, 2, 4, 8}, masts[HULL_COUNT] = {1, 1, 2, 3};
    for (int h = 0; h < HULL_COUNT; ++h) {
        const hull_spec *spec = ship_hull(h);
        CHECK(spec != NULL);
        if (!spec) continue;
        CHECK(spec->crew == crews[h] && ship_hull_find(spec->name) == h);
        CHECK(spec->berth == (h <= HULL_CUTTER ? BERTH_SMALL : BERTH_LARGE));
        ship_slot slots[SHIP_MAX_SLOTS];
        int n = ship_slots(h, slots, SHIP_MAX_SLOTS);
        CHECK(n == ship_slot_count(h) && n > 10 && n <= SHIP_MAX_SLOTS);
        for (int k = 0; k < SLOT_KINDS; ++k) CHECK(count_slots(slots, n, k) == spec->slots[k]);
        CHECK(count_slots(slots, n, SLOT_MAST) == masts[h] && count_slots(slots, n, SLOT_SAIL) == masts[h]);
        // Thrusters push the way their slots' kinds say, from where they say.
        for (int i = 0; i < n; ++i) {
            const ship_slot *slot = &slots[i];
            if (slot->kind == SLOT_MAIN) CHECK(slot->push.z < 0.0f && slot->mount.z > 0.0f);    // astern, driving her ahead
            if (slot->kind == SLOT_RETRO) CHECK(slot->push.z > 0.0f && slot->mount.z < 0.0f);
            if (slot->kind == SLOT_LIFT) CHECK(slot->push.y > 0.0f && slot->mount.y < 0.0f);
            if (slot->kind == SLOT_DOWN) CHECK(slot->push.y < 0.0f);
            if (slot->kind == SLOT_LATERAL) CHECK(slot->push.x * slot->mount.x < 0.0f);       // pushing away from its side
            if (slot->kind == SLOT_MAST || slot->kind == SLOT_SAIL) CHECK(slot->mast >= 0 && slot->mast < masts[h]);
        }
        ship_loadout l = ship_default_loadout(h);
        CHECK(l.hull == h && ship_loadout_valid(&l));
        for (int i = 0; i < n; ++i) CHECK(l.fit[i] != SHIP_NOTHING && ship_fits(l.fit[i], slots[i].kind));
        uint8_t buffer[SHIP_MAX_SLOTS + 16];
        cw_writer w;
        l.look.field[LOOK_HULL_PAINT] = 5;   // and a look of her own
        l.look.field[LOOK_SAIL_CUT] = CUT_SOLAR;
        l.look.field[LOOK_HULL_SHAPE] = (uint8_t)(h % SHAPE_COUNT);
        cw_writer_init_fixed(&w, buffer, sizeof(buffer));
        ship_loadout_write(&w, &l);
        CHECK(cw_writer_ok(&w) && w.size == (size_t)(2 + n + 1 + LOOK_FIELDS));
        cw_reader r = cw_reader_make(buffer, w.size);
        ship_loadout back;
        CHECK(ship_loadout_read(&r, &back) && ship_loadout_equal(&back, &l));
        buffer[w.size - 1] = SHAPE_COUNT;   // no such shape
        r = cw_reader_make(buffer, w.size);
        CHECK(!ship_loadout_read(&r, &back));
        buffer[w.size - 1] = l.look.field[LOOK_HULL_SHAPE];
        r = cw_reader_make(buffer, w.size - 1);   // cut short
        CHECK(!ship_loadout_read(&r, &back));
        buffer[2] = (uint8_t)ship_component_find("Canvas");   // a sail where an engine goes
        r = cw_reader_make(buffer, w.size);
        CHECK(!ship_loadout_read(&r, &back));
        buffer[2] = l.fit[0];
        buffer[0] = (uint8_t)((h + 2) % HULL_COUNT);   // another hull's slots (a different number of them) are not these
        r = cw_reader_make(buffer, w.size);
        CHECK(!ship_loadout_read(&r, &back));
    }
    CHECK(ship_hull(HULL_COUNT) == NULL && ship_hull_find("dinghy") == -1 && ship_hull_find("GALLEON") == HULL_GALLEON);
    CHECK(!strcmp(ship_mast_name(1, 0), "MAST") && !strcmp(ship_mast_name(2, 1), "MAINMAST") && !strcmp(ship_mast_name(3, 2), "MIZZEN"));
    // The catalogue: each fitting fits one kind of slot, and is found by its name.
    for (int c = 1; c < ship_component_count(); ++c) {
        const ship_component *info = ship_component_info(c);
        CHECK(info->slot >= 0 && info->slot < SLOT_KINDS && info->mass > 0.0f && info->power > 0.0f);
        CHECK(ship_component_find(info->name) == c);
        for (int k = 0; k < SLOT_KINDS; ++k) CHECK(ship_fits(c, k) == (k == info->slot));
    }
    CHECK(ship_component_info(ship_component_count()) == NULL);

    ship_loadout l = ship_default_loadout(HULL_SLOOP);
    int engine = first_slot(HULL_SLOOP, SLOT_MAIN), mast = first_slot(HULL_SLOOP, SLOT_MAST), sail = first_slot(HULL_SLOOP, SLOT_SAIL);
    ship_loadout odd = l;
    odd.fit[engine] = SHIP_NOTHING;
    CHECK(ship_loadout_valid(&odd));                   // an engine slot may stand empty...
    odd = l;
    odd.fit[mast] = SHIP_NOTHING;
    CHECK(!ship_loadout_valid(&odd));                  // ...a mast may not
    odd = l;
    odd.fit[sail] = SHIP_NOTHING;
    CHECK(!ship_loadout_valid(&odd));
    odd = l;
    odd.fit[engine] = (uint8_t)ship_component_find("Great Wheel");
    CHECK(!ship_loadout_valid(&odd));                  // a gyro is no engine
    odd = l;
    odd.fit[ship_slot_count(HULL_SLOOP)] = (uint8_t)ship_component_find("Brass Drive");
    CHECK(!ship_loadout_valid(&odd));                  // past the hull's slots
    odd = l;
    odd.fit[engine] = 250;
    CHECK(!ship_loadout_valid(&odd));
    odd = l;
    odd.hull = HULL_COUNT;
    CHECK(!ship_loadout_valid(&odd));
    ship_design *d = (ship_design *)malloc(sizeof(ship_design));
    CHECK(!ship_design_build(&odd, 20.0f, d));
    free(d);
    CHECK(ship_fit_all(&l, SLOT_MAIN, ship_component_find("Iron Furnace")) && l.fit[engine] == ship_component_find("Iron Furnace"));
    CHECK(!ship_fit_all(&l, SLOT_MAIN, ship_component_find("Canvas")));
    CHECK(ship_loadout_valid(&l));
}

static ship_stats stats_of(const ship_loadout *l) {
    ship_stats s = {};
    CHECK(game_ship_stats(l, &s));
    return s;
}
static ship_stats stats_with(int hull, int kind, const char *fitting) {
    ship_loadout l = ship_default_loadout(hull);
    CHECK(ship_fit_all(&l, kind, ship_component_find(fitting)));
    return stats_of(&l);
}

// The bigger the hull the more ship: heavier, slower to gather way, much slower to
// turn, and its flight computer answers later; every one of them can still lift
// itself. And every fitting is a trade that shows in the numbers.
static void test_ship_stats(void) {
    ship_stats hull[HULL_COUNT];
    for (int h = 0; h < HULL_COUNT; ++h) {
        ship_loadout l = ship_default_loadout(h);
        hull[h] = stats_of(&l);
        CHECK(hull[h].crew == ship_hull(h)->crew && hull[h].masts == ship_hull(h)->slots[SLOT_MAST]);
        CHECK(hull[h].lift_ratio > 1.3f && hull[h].lift_ratio < 1.7f);   // climbs, with some to spare
        CHECK(hull[h].sail_area > 10.0f && hull[h].sail_power > 0.0f && hull[h].heel > 0.0f);
        for (int k = 0; k < CRAFT_GROUPS; ++k) CHECK(hull[h].accel[k] > 0.0f && hull[h].top_speed[k] == ship_hull(h)->top_speed[k]);
        if (h == 0) continue;
        const ship_stats *a = &hull[h - 1], *b = &hull[h];
        CHECK(b->mass > a->mass * 1.5f);
        CHECK(b->accel[CRAFT_GROUP_FORWARD] < a->accel[CRAFT_GROUP_FORWARD]);
        CHECK(b->turn_accel.x < a->turn_accel.x && b->turn_accel.y < a->turn_accel.y && b->turn_accel.z < a->turn_accel.z);
        CHECK(b->top_rate.x < a->top_rate.x && b->top_rate.y < a->top_rate.y && b->top_rate.z < a->top_rate.z);
        CHECK(b->top_speed[CRAFT_GROUP_FORWARD] < a->top_speed[CRAFT_GROUP_FORWARD]);
        CHECK(b->response > a->response && b->turn_response > a->turn_response);
        CHECK(b->sail_area > a->sail_area && b->hull_drag > a->hull_drag);
    }
    CHECK(hull[HULL_GALLEON].turn_accel.y < hull[HULL_SLOOP].turn_accel.y * 0.2f);   // the galleon: a fifth of the sloop's turn, or less

    const ship_stats *sloop = &hull[HULL_SLOOP];
    // Engines: the furnace pushes harder and weighs more, so she turns slower; the
    // dragonfly the other way. The computer lets the stronger go faster.
    ship_stats furnace = stats_with(HULL_SLOOP, SLOT_MAIN, "Iron Furnace"), dragonfly = stats_with(HULL_SLOOP, SLOT_MAIN, "Dragonfly");
    CHECK(furnace.accel[CRAFT_GROUP_FORWARD] > sloop->accel[CRAFT_GROUP_FORWARD] * 1.3f && furnace.mass > sloop->mass);
    CHECK(furnace.top_speed[CRAFT_GROUP_FORWARD] > sloop->top_speed[CRAFT_GROUP_FORWARD]);
    CHECK(furnace.turn_accel.y < sloop->turn_accel.y && furnace.lift_ratio < sloop->lift_ratio);
    CHECK(dragonfly.accel[CRAFT_GROUP_FORWARD] < sloop->accel[CRAFT_GROUP_FORWARD] && dragonfly.mass < sloop->mass);
    CHECK(dragonfly.turn_accel.y > sloop->turn_accel.y && dragonfly.top_speed[CRAFT_GROUP_FORWARD] < sloop->top_speed[CRAFT_GROUP_FORWARD]);
    // Gyros: the great wheel turns her harder, and lets the computer turn her faster.
    ship_stats wheel = stats_with(HULL_SLOOP, SLOT_GYRO, "Great Wheel"), whirligig = stats_with(HULL_SLOOP, SLOT_GYRO, "Whirligig");
    CHECK(wheel.turn_accel.y > sloop->turn_accel.y * 1.5f && wheel.top_rate.y > sloop->top_rate.y && wheel.mass > sloop->mass);
    CHECK(whirligig.turn_accel.y < sloop->turn_accel.y && whirligig.top_rate.y < sloop->top_rate.y);
    // Lift: the heavy lifter climbs better despite its weight.
    ship_stats heavy = stats_with(HULL_SLOOP, SLOT_LIFT, "Heavy Lifter"), light = stats_with(HULL_SLOOP, SLOT_LIFT, "Light Lifter");
    CHECK(heavy.lift_ratio > sloop->lift_ratio && light.lift_ratio < sloop->lift_ratio && light.mass < sloop->mass);
    CHECK(heavy.top_speed[CRAFT_GROUP_UP] > sloop->top_speed[CRAFT_GROUP_UP]);
    // Masts: a tall one carries more sail, higher, so it heels her harder; a stub less of both.
    ship_stats oak = stats_with(HULL_SLOOP, SLOT_MAST, "Tall Oak"), stub = stats_with(HULL_SLOOP, SLOT_MAST, "Stub Mast");
    CHECK(oak.sail_area > sloop->sail_area * 1.3f && oak.sail_power > sloop->sail_power && oak.heel > sloop->heel);
    CHECK(stub.sail_area < sloop->sail_area && stub.heel < sloop->heel && stub.mass < sloop->mass);
    CHECK(oak.turn_accel.x < sloop->turn_accel.x);   // and the weight aloft makes her slower to pitch
    // Cloth: sunsilk is lighter and draws harder; storm cloth the other way.
    ship_stats silk = stats_with(HULL_SLOOP, SLOT_SAIL, "Sunsilk"), storm = stats_with(HULL_SLOOP, SLOT_SAIL, "Storm Cloth");
    CHECK(silk.sail_power > sloop->sail_power && silk.mass < sloop->mass && silk.sail_area == sloop->sail_area);
    CHECK(storm.sail_power < sloop->sail_power && storm.mass > sloop->mass);
    // Nothing where the retros go: no braking, and that much lighter.
    ship_loadout bare = ship_default_loadout(HULL_SLOOP);
    ship_fit_all(&bare, SLOT_RETRO, SHIP_NOTHING);
    ship_stats unbraked = stats_of(&bare);
    CHECK(unbraked.accel[CRAFT_GROUP_BACK] == 0.0f && unbraked.mass < sloop->mass);
    // The same fitting on a bigger hull's mount is a bigger thing.
    ship_design *small = (ship_design *)malloc(sizeof(ship_design)), *large = (ship_design *)malloc(sizeof(ship_design));
    ship_loadout a = ship_default_loadout(HULL_SLOOP), b = ship_default_loadout(HULL_GALLEON);
    CHECK(ship_design_build(&a, 20.0f, small) && ship_design_build(&b, 20.0f, large));
    float engine_small = 0.0f, engine_large = 0.0f;
    for (int i = 0; i < small->part_count; ++i)
        if (small->parts[i].component == ship_component_find("Brass Drive")) engine_small = -small->parts[i].thrust.z;
    for (int i = 0; i < large->part_count; ++i)
        if (large->parts[i].component == ship_component_find("Brass Drive")) engine_large = -large->parts[i].thrust.z;
    CHECK(engine_small == 60000.0f && engine_large > engine_small * 2.0f);
    CHECK(small->masts == 1 && large->masts == 3 && large->body.masts == 3);
    free(small);
    free(large);
}

// A loadout fitted lopsided flies lopsided: one heavy engine to starboard yaws her
// when it fires, where the standard's engines balance.
static void test_ship_lopsided(void) {
    ship_loadout l = ship_default_loadout(HULL_SLOOP);
    ship_design *d = (ship_design *)malloc(sizeof(ship_design)), *even = (ship_design *)malloc(sizeof(ship_design));
    CHECK(ship_design_build(&l, 20.0f, even));
    l.fit[first_slot(HULL_SLOOP, SLOT_MAIN)] = (uint8_t)ship_component_find("Iron Furnace");   // the first is to starboard
    CHECK(ship_design_build(&l, 20.0f, d));
    CHECK(fabsf(even->body.thrust_torque[CRAFT_GROUP_FORWARD].y) < 100.0f);
    CHECK(d->body.thrust_torque[CRAFT_GROUP_FORWARD].y > 10000.0f);   // pushing from starboard: her bow swings to port
    CHECK(d->body.center.x > even->body.center.x);
    free(d);
    free(even);
}

// A crew member at a helm, in the world of `g`.
static cw_entity_id take_helm(game *g, cw_entity_id craft) {
    cw_entity_id pilot = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(craft_of(g, craft)->helm, v3_make(0, 0.05f, 0))), true);
    game_tick(g);
    press_use(g, pilot);
    CHECK(craft_of(g, craft)->pilot == pilot);
    return pilot;
}

// Each hull, built: its craft is the design (the refit sums its parts as the
// shipyard did), it sits on a berth of its size and stays there, a player climbs
// its stairs to its deck, and every one of its helm and sail stations can be
// taken. Then flown, the same pilot's hands on each: the bigger it is the slower
// it gathers way and the slower it comes round.
static void test_hulls_built_and_flown(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    ship_design *d = (ship_design *)malloc(sizeof(ship_design));
    float turned[HULL_COUNT], speed[HULL_COUNT], climbed[HULL_COUNT];
    for (int h = 0; h < HULL_COUNT; ++h) {
        ship_loadout l = ship_default_loadout(h);
        CHECK(ship_design_build(&l, 20.0f, d));
        cw_entity_id owner = game_spawn_player(g, g->spawn, true);
        game_set_owner(g, owner, 500 + (uint64_t)h);
        cw_entity_id craft = game_call_craft(g, owner, &l);
        CHECK(craft != CW_INVALID_ENTITY);
        if (craft == CW_INVALID_ENTITY) continue;
        Craft *c = craft_of(g, craft);
        CHECK(ship_loadout_equal(&c->loadout, &l) && !strcmp(c->name, ship_hull(h)->name));
        CHECK(g->berths[c->berth].size == ship_hull(h)->berth);
        CHECK(fabsf(c->mass - d->body.mass) < d->body.mass * 1e-5f);
        CHECK(v3_length(v3_sub(c->center, d->body.center)) < 1e-3f);
        CHECK(fabsf(c->inertia.y - d->body.inertia.y) < d->body.inertia.y * 1e-4f);
        CHECK(fabsf(c->thrust[CRAFT_GROUP_UP] - d->body.thrust[CRAFT_GROUP_UP]) < 1.0f && c->masts == d->masts);
        CHECK(c->response == d->stats.response && c->top_rate.y == d->stats.top_rate.y && c->hull_drag == d->stats.hull_drag);
        for (int m = 0; m < c->masts; ++m) CHECK(c->sails[m].area > 10.0f && fabsf(c->sails[m].area - d->body.sails[m].area) < 0.01f);
        // On its berth, still, on its skids.
        v3 parked = transform_of(g, craft)->position;
        run_seconds(g, 1.0f);
        CHECK(v3_length(v3_sub(transform_of(g, craft)->position, parked)) < 0.001f);
        CHECK(fabsf(lowest_point(g, craft) - g->berths[c->berth].floor.y) < 0.01f);
        // Up the stairs, to the helm, and to each mast's sails.
        transform_of(g, owner)->position = transform_of(g, owner)->previous = game_craft_to_world(g, craft, v3_make(0, -c->draft, c->cabin_half.z + 10.0f));
        board_craft(g, owner, craft);
        CHECK(game_aboard(g, owner) == craft);
        for (int m = 0; m < c->masts; ++m) {
            cw_entity_id hand = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(c->sails[m].station, v3_make(0, 0.05f, 0))), true);
            game_tick(g);
            press_use(g, hand);
            CHECK(game_craft_station_of(craft_of(g, craft), hand) == m);
            game_despawn_player(g, hand);
        }

        // Flown, out over the void in the hub's gravity: lift, then the stick hard over, then the throttle.
        v3 sky = v3_make(100.0f, 120.0f, 150.0f);
        cw_entity_id flown = game_spawn_craft(g, &l, sky, 0.0f, 0, -1);
        craft_of(g, flown)->resting = false;
        game_tick(g);
        cw_entity_id pilot = take_helm(g, flown);
        PlayerInput *in = input_of(g, pilot);
        in->lift = 1.0f;
        run_seconds(g, 2.0f);
        climbed[h] = transform_of(g, flown)->position.y - sky.y;
        in->lift = 0.0f;
        run_seconds(g, 2.0f);
        v3 bow = bow_of(g, flown);
        in->turn_yaw = 1.0f;
        run_seconds(g, 1.5f);
        in->turn_yaw = 0.0f;
        turned[h] = acosf(fminf(1.0f, v3_dot(bow, bow_of(g, flown))));
        run_seconds(g, 3.0f);
        in->forward = 1.0f;
        float before = v3_dot(craft_of(g, flown)->velocity, bow_of(g, flown));
        run_seconds(g, 2.0f);
        speed[h] = v3_dot(craft_of(g, flown)->velocity, bow_of(g, flown)) - before;
        in->forward = 0.0f;
        CHECK(climbed[h] > 3.0f);
        game_despawn_craft(g, flown);
        game_despawn_player(g, pilot);
        game_tick(g);
    }
    for (int h = 1; h < HULL_COUNT; ++h) {
        CHECK(turned[h] < turned[h - 1]);
        CHECK(speed[h] < speed[h - 1]);
    }
    CHECK(turned[HULL_GALLEON] < turned[HULL_SLOOP] * 0.5f);
    free(d);
    game_destroy(g);
    free(g);
}

// A ship of two masts: a hand at each, each working their own; one sail set draws
// her along, both set draw her harder; a hand put off leaves their mast alone.
static void test_masts(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    g->currents = true;   // off by default, for now
    game_build_universe(g);
    ship_loadout brig = ship_default_loadout(HULL_BRIGANTINE);
    float made[2];
    for (int both = 0; both < 2; ++both) {
        v3 at = v3_make(-60.0f + 400.0f * (float)both, 40.0f, -40.0f);
        v3 flow = game_solar_current_at_tick(g, at, g->world.tick);
        cw_entity_id craft = game_spawn_craft(g, &brig, at, atan2f(-flow.x, -flow.z), 0, -1);
        craft_of(g, craft)->resting = false;
        game_tick(g);
        Craft *c = craft_of(g, craft);
        CHECK(c->masts == 2 && c->sails[0].station.z < c->sails[1].station.z);   // the foremast first
        take_helm(g, craft);
        cw_entity_id hands[2];
        for (int m = 0; m < 2; ++m) {
            hands[m] = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(c->sails[m].station, v3_make(0, 0.05f, 0))), true);
            game_tick(g);
            press_use(g, hands[m]);
            CHECK(game_craft_station_of(c, hands[m]) == m);
        }
        // Someone else at the foremast's pinrail finds it taken.
        cw_entity_id third = game_spawn_player(g, game_craft_to_world(g, craft, v3_add(c->sails[0].station, v3_make(0.3f, 0.05f, 0.3f))), true);
        game_tick(g);
        press_use(g, third);
        CHECK(game_craft_station_of(c, third) == -1 && c->sails[0].hand == hands[0]);
        // The foremast's hand hoists: the mainmast's sail stays in.
        input_of(g, hands[0])->sail = 1.0f;
        if (both) input_of(g, hands[1])->sail = 1.0f;
        run_seconds(g, 2.5f);
        input_of(g, hands[0])->sail = input_of(g, hands[1])->sail = 0.0f;
        CHECK(c->sails[0].set == 1.0f && c->sails[1].set == (both ? 1.0f : 0.0f));
        float set, fill;
        game_craft_sail_totals(c, &set, &fill);
        CHECK(both ? set == 1.0f : set > 0.3f && set < 0.7f);
        run_seconds(g, 4.0f);
        CHECK(c->sails[0].fill > 0.8f && (both ? c->sails[1].fill > 0.8f : c->sails[1].fill == 0.0f));
        made[both] = v3_dot(c->velocity, bow_of(g, craft));
        // Put off the ship, the mainmast's hand leaves it; the foremast's is still at hers.
        game_kick(g, craft, hands[1]);
        CHECK(c->sails[1].hand == CW_INVALID_ENTITY && c->sails[0].hand == hands[0]);
    }
    CHECK(made[0] > 3.0f && made[1] > made[0] * 1.2f);
    game_destroy(g);
    free(g);
}

// The yard builds what is ordered, on a berth its size; rebuilds to a new order a
// craft nobody is aboard, and will not one with crew; and when every berth that
// takes a hull is full, it has none to give.
static void test_yard_hulls(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    int small = 0, large = 0;
    for (int b = 0; b < g->berth_count; ++b) (g->berths[b].size == BERTH_SMALL ? small : large)++;
    CHECK(small == 8 && large == 3);
    ship_loadout galleon = ship_default_loadout(HULL_GALLEON), cutter = ship_default_loadout(HULL_CUTTER);
    ship_loadout sloop = ship_default_loadout(HULL_SLOOP);
    cw_entity_id players[5];
    for (int i = 0; i < 5; ++i) {
        players[i] = game_spawn_player(g, g->spawn, true);
        game_set_owner(g, players[i], 900 + (uint64_t)i);
    }
    cw_entity_id big = game_call_craft(g, players[0], &galleon);
    CHECK(big != CW_INVALID_ENTITY && g->berths[craft_of(g, big)->berth].size == BERTH_LARGE);
    cw_entity_id little = game_call_craft(g, players[1], &cutter);
    CHECK(little != CW_INVALID_ENTITY && g->berths[craft_of(g, little)->berth].size == BERTH_SMALL);
    CHECK(game_call_craft(g, players[1], &cutter) == little);   // the same order: the same craft
    // Nobody aboard the galleon: its owner's new order breaks it up and builds a sloop, on a small berth.
    int freed = craft_of(g, big)->berth;
    cw_entity_id rebuilt = game_call_craft(g, players[0], &sloop);
    CHECK(rebuilt != CW_INVALID_ENTITY && rebuilt != big && craft_of(g, rebuilt)->loadout.hull == HULL_SLOOP);
    CHECK(g->berths[craft_of(g, rebuilt)->berth].size == BERTH_SMALL);
    game_tick(g);
    CHECK(!cw_world_is_valid(&g->world, big) && game_free_berth(g, HULL_GALLEON) == freed);
    // Someone aboard the cutter: no rebuilding under them; it is towed home as it is.
    transform_of(g, players[2])->position = game_craft_to_world(g, little, v3_make(0, 0.05f, 0));
    run_seconds(g, 0.2f);
    CHECK(game_aboard(g, players[2]) == little);
    CHECK(game_call_craft(g, players[1], &galleon) == little && craft_of(g, little)->loadout.hull == HULL_CUTTER);
    // Three large berths: a fourth big ship has nowhere to go.
    for (int i = 2; i < 5; ++i) {
        ship_loadout brig = ship_default_loadout(HULL_BRIGANTINE);
        cw_entity_id built = game_call_craft(g, players[i], &brig);
        CHECK((i < 5 && built != CW_INVALID_ENTITY) || built == CW_INVALID_ENTITY);
    }
    cw_entity_id late = game_spawn_player(g, g->spawn, true);
    game_set_owner(g, late, 999);
    CHECK(game_free_berth(g, HULL_GALLEON) == -1 && game_call_craft(g, late, &galleon) == CW_INVALID_ENTITY);
    CHECK(game_free_berth(g, HULL_SLOOP) >= 0);   // small ones are left
    // The yard's own order, for a call with none.
    g->yard_order = cutter;
    CHECK(craft_of(g, game_call_craft(g, late, NULL))->loadout.hull == HULL_CUTTER);
    game_destroy(g);
    free(g);
}

// ---------------------------------------------------------------------------
// The shipwright: the books, and the stall in the yard
// ---------------------------------------------------------------------------
static shipwright_order order_of(int op, int hull, int slot, int component) {
    shipwright_order o = {};
    o.op = (uint8_t)op;
    o.hull = (uint8_t)hull;
    o.slot = (uint8_t)slot;
    o.component = (uint8_t)component;
    return o;
}

// Everyone starts with a sloop and a bottomless purse; a purse with a bottom pays
// and runs out; fittings come from store before they are bought, and what comes out
// goes into store; a hull comes fitted as standard; orders that make no sense, or
// no change, change nothing.
static void test_shipwright_books(void) {
    character c = {};
    character_fit_out(&c);
    CHECK(fleet_owns(&c, HULL_SLOOP) && !fleet_owns(&c, HULL_CUTTER) && c.ships.sailing == HULL_SLOOP && c.bottomless);
    ship_loadout sloop = ship_default_loadout(HULL_SLOOP);
    CHECK(ship_loadout_equal(fleet_sailing(&c), &sloop));

    // Bottomless: anything, paid for in the tally and not in the purse.
    int furnace = ship_component_find("Iron Furnace"), brass = ship_component_find("Brass Drive");
    uint32_t version = c.version;
    int64_t cost = 0;
    shipwright_order fit_all = order_of(SHIPWRIGHT_FIT_ALL, 0, SLOT_MAIN, furnace);
    CHECK(shipwright_quote(&c, &fit_all, &cost) == SHIPWRIGHT_DONE && cost == 2 * ship_component_price(furnace));
    CHECK(shipwright_apply(&c, &fit_all) == SHIPWRIGHT_DONE);
    CHECK(c.version == version + 1 && c.doubloons == 0 && c.spent == 2 * ship_component_price(furnace));
    CHECK(c.ships.store[brass] == 2 && fleet_sailing(&c)->fit[0] == furnace && fleet_sailing(&c)->fit[1] == furnace);
    CHECK(shipwright_apply(&c, &fit_all) == SHIPWRIGHT_UNCHANGED && c.version == version + 1);
    // Back to brass on one side: out of store, for nothing.
    int64_t spent = c.spent;
    shipwright_order one = order_of(SHIPWRIGHT_FIT, 0, 1, brass);
    CHECK(shipwright_quote(&c, &one, &cost) == SHIPWRIGHT_DONE && cost == 0);
    CHECK(shipwright_apply(&c, &one) == SHIPWRIGHT_DONE && c.spent == spent);
    CHECK(c.ships.store[brass] == 1 && c.ships.store[furnace] == 1 && fleet_sailing(&c)->fit[1] == brass);
    // Taking one out leaves the slot empty; a mast cannot be.
    int retro = ship_component_find("Brass Retro");
    shipwright_order out = order_of(SHIPWRIGHT_FIT, 0, 2, SHIP_NOTHING);
    CHECK(shipwright_apply(&c, &out) == SHIPWRIGHT_DONE);
    CHECK(fleet_sailing(&c)->fit[2] == SHIP_NOTHING && c.ships.store[retro] == 1);
    int mast_slot = ship_slot_count(HULL_SLOOP) - 2;
    shipwright_order no_mast = order_of(SHIPWRIGHT_FIT, 0, mast_slot, SHIP_NOTHING);
    CHECK(shipwright_apply(&c, &no_mast) == SHIPWRIGHT_INVALID);
    shipwright_order wrong = order_of(SHIPWRIGHT_FIT, 0, 0, ship_component_find("Canvas"));
    CHECK(shipwright_apply(&c, &wrong) == SHIPWRIGHT_INVALID);
    shipwright_order past = order_of(SHIPWRIGHT_FIT, 0, ship_slot_count(HULL_SLOOP), brass);
    CHECK(shipwright_apply(&c, &past) == SHIPWRIGHT_INVALID);
    // Selling from store pays half.
    int64_t purse = c.doubloons;
    shipwright_order sell = order_of(SHIPWRIGHT_SELL, 0, 0, furnace);
    CHECK(shipwright_apply(&c, &sell) == SHIPWRIGHT_DONE && c.doubloons == purse + ship_component_price(furnace) / 2);
    CHECK(c.ships.store[furnace] == 0 && shipwright_apply(&c, &sell) == SHIPWRIGHT_NOT_IN_STORE);
    // Hulls: bought fitted as standard, sailed only when owned.
    shipwright_order sail = order_of(SHIPWRIGHT_SAIL, HULL_GALLEON, 0, 0), buy = order_of(SHIPWRIGHT_BUY_HULL, HULL_GALLEON, 0, 0);
    CHECK(shipwright_apply(&c, &sail) == SHIPWRIGHT_NOT_OWNED);
    CHECK(shipwright_apply(&c, &buy) == SHIPWRIGHT_DONE && fleet_owns(&c, HULL_GALLEON) && c.ships.sailing == HULL_SLOOP);
    CHECK(shipwright_apply(&c, &buy) == SHIPWRIGHT_OWNED);
    CHECK(shipwright_apply(&c, &sail) == SHIPWRIGHT_DONE && c.ships.sailing == HULL_GALLEON);
    ship_loadout galleon = ship_default_loadout(HULL_GALLEON);
    CHECK(ship_loadout_equal(fleet_sailing(&c), &galleon));
    // The sloop keeps its own fittings while she is laid up; fittings go to the sailed hull.
    CHECK(c.ships.loadouts[HULL_SLOOP].fit[0] == furnace);
    shipwright_order wheels = order_of(SHIPWRIGHT_FIT_ALL, 0, SLOT_GYRO, ship_component_find("Great Wheel"));
    CHECK(shipwright_apply(&c, &wheels) == SHIPWRIGHT_DONE && c.ships.store[ship_component_find("Spinning Brass")] == 3);
    CHECK(c.ships.loadouts[HULL_SLOOP].fit[ship_slot_count(HULL_SLOOP) - 3] == ship_component_find("Spinning Brass"));

    // A purse with a bottom: what it cannot pay for is refused, and changes nothing.
    character poor = {};
    character_fit_out(&poor);
    poor.bottomless = false;
    poor.doubloons = 1000;
    shipwright_order cutter = order_of(SHIPWRIGHT_BUY_HULL, HULL_CUTTER, 0, 0);
    character before = poor;
    CHECK(shipwright_apply(&poor, &cutter) == SHIPWRIGHT_CANNOT_AFFORD && memcmp(&before, &poor, sizeof(poor)) == 0);
    shipwright_order lifter = order_of(SHIPWRIGHT_FIT, 0, 4, ship_component_find("Light Lifter"));   // 1,200 a piece
    CHECK(shipwright_apply(&poor, &lifter) == SHIPWRIGHT_CANNOT_AFFORD);
    shipwright_order retro_fit = order_of(SHIPWRIGHT_FIT, 0, 2, ship_component_find("Iron Retro"));   // 900
    CHECK(shipwright_apply(&poor, &retro_fit) == SHIPWRIGHT_DONE && poor.doubloons == 100 && poor.spent == 900);
    // The old retro's sale pays for part of the next.
    shipwright_order sell_retro = order_of(SHIPWRIGHT_SELL, 0, 0, retro);
    CHECK(shipwright_apply(&poor, &sell_retro) == SHIPWRIGHT_DONE && poor.doubloons == 300);

    // A character that was never fitted out, or is muddled, is made whole.
    character muddled = {};
    muddled.ships.owned = 1u << HULL_CUTTER;
    muddled.ships.sailing = HULL_GALLEON;
    muddled.ships.store[0] = 7;
    character_fit_out(&muddled);
    CHECK(muddled.ships.sailing == HULL_CUTTER && muddled.ships.store[0] == 0 && ship_loadout_valid(fleet_sailing(&muddled)));
    // Orders over the wire.
    uint8_t buffer[8];
    cw_writer w;
    cw_writer_init_fixed(&w, buffer, sizeof(buffer));
    shipwright_order_write(&w, &wheels);
    shipwright_order back;
    cw_reader r = cw_reader_make(buffer, w.size);
    CHECK(shipwright_order_read(&r, &back) && memcmp(&back, &wheels, sizeof(back)) == 0);
    buffer[0] = SHIPWRIGHT_OPS;
    r = cw_reader_make(buffer, w.size);
    CHECK(!shipwright_order_read(&r, &back));
}

// The stall in the yard: a counter to stand at, and the shipwright's work on the
// ship, rebuilt when nobody is aboard it and left alone when someone is; the yard's
// terminal builds what the player's order says.
static void test_shipwright_yard(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    int counter = -1;
    for (int i = 0; i < g->place_count; ++i)
        if (g->places[i].kind == PLACE_SHIPWRIGHT) counter = i;
    CHECK(counter >= 0);
    if (counter < 0) { game_destroy(g); free(g); return; }
    cw_entity_id player = game_spawn_player(g, g->places[counter].spawn, true);
    game_set_owner(g, player, 77);
    run_seconds(g, 0.5f);
    CHECK(game_place_of_kind_at(g, transform_of(g, player)->position, PLACE_SHIPWRIGHT) == counter);   // stands there, held up
    CHECK(game_place_of_kind_at(g, g->spawn, PLACE_SHIPWRIGHT) == -1);
    // Walking to the counter from the bridge: nothing is in the way.
    transform_of(g, player)->position = transform_of(g, player)->previous = v3_make(-40.0f, 0.0f, 0.0f);
    run_seconds(g, 0.3f);
    CHECK(walk_to(g, player, -47.5f, 0.0f, 8.0f));   // off the bridge
    CHECK(walk_to(g, player, g->places[counter].spawn.x, g->places[counter].spawn.z, 8.0f));
    CHECK(game_place_of_kind_at(g, transform_of(g, player)->position, PLACE_SHIPWRIGHT) == counter);

    character books = {};
    character_fit_out(&books);
    shipwright_order buy = order_of(SHIPWRIGHT_BUY_HULL, HULL_BRIGANTINE, 0, 0), sail = order_of(SHIPWRIGHT_SAIL, HULL_BRIGANTINE, 0, 0);
    CHECK(shipwright_apply(&books, &buy) == SHIPWRIGHT_DONE && shipwright_apply(&books, &sail) == SHIPWRIGHT_DONE);
    game_set_ship_order(g, player, fleet_sailing(&books));
    // Nothing built yet: the shipwright has her built, on a berth her size.
    cw_entity_id craft = game_refit_craft(g, player, fleet_sailing(&books));
    CHECK(craft != CW_INVALID_ENTITY && craft_of(g, craft)->loadout.hull == HULL_BRIGANTINE);
    CHECK(g->berths[craft_of(g, craft)->berth].size == BERTH_LARGE);
    // Refitted with nobody aboard: broken up and built anew, as ordered.
    shipwright_order heavy = order_of(SHIPWRIGHT_FIT_ALL, 0, SLOT_LIFT, ship_component_find("Heavy Lifter"));
    CHECK(shipwright_apply(&books, &heavy) == SHIPWRIGHT_DONE);
    cw_entity_id refitted = game_refit_craft(g, player, fleet_sailing(&books));
    CHECK(refitted != craft && ship_loadout_equal(&craft_of(g, refitted)->loadout, fleet_sailing(&books)));
    game_tick(g);
    CHECK(!cw_world_is_valid(&g->world, craft));
    // Someone aboard: her refit waits, and the yard's terminal will not rebuild her under them.
    cw_entity_id rider = game_spawn_player(g, game_craft_to_world(g, refitted, v3_make(0, 0.05f, 0)), true);
    run_seconds(g, 0.2f);
    CHECK(game_aboard(g, rider) == refitted);
    shipwright_order light = order_of(SHIPWRIGHT_FIT_ALL, 0, SLOT_LIFT, ship_component_find("Light Lifter"));
    CHECK(shipwright_apply(&books, &light) == SHIPWRIGHT_DONE);
    game_set_ship_order(g, player, fleet_sailing(&books));
    CHECK(game_refit_craft(g, player, fleet_sailing(&books)) == refitted);
    CHECK(!ship_loadout_equal(&craft_of(g, refitted)->loadout, fleet_sailing(&books)));
    // Once the rider has gone, a call at the terminal builds what was ordered.
    game_despawn_player(g, rider);
    run_seconds(g, 0.1f);
    int yard = -1;
    for (int i = 0; i < g->place_count; ++i)
        if (g->places[i].kind == PLACE_YARD) yard = i;
    transform_of(g, player)->position = transform_of(g, player)->previous = g->places[yard].spawn;
    run_seconds(g, 0.2f);
    press_use(g, player);
    cw_entity_id called = game_craft_of(g, 77);
    CHECK(called != refitted && called != CW_INVALID_ENTITY && ship_loadout_equal(&craft_of(g, called)->loadout, fleet_sailing(&books)));
    game_destroy(g);
    free(g);
}

// A ship's look: every choice named and priced; paint on the parts that take it
// and none on those that do not; each shape adding trim and nothing to how she
// flies; each shape still boarded up her stairs and sat on her skids; the look
// bought at the shipwright's a field at a time.
static void test_ship_looks(void) {
    for (int f = 0; f < LOOK_FIELDS; ++f) {
        CHECK(ship_look_options(f) >= 4 && strcmp(ship_look_field_name(f), "?") != 0);
        for (int v = 0; v < ship_look_options(f); ++v) CHECK(ship_look_price(f, v) > 0 && strcmp(ship_look_option_name(f, v), "?") != 0);
        CHECK(strcmp(ship_look_option_name(f, ship_look_options(f)), "?") == 0);
    }
    for (int v = 0; v < SHAPE_COUNT; ++v) CHECK(ship_look_option_note(LOOK_HULL_SHAPE, v)[0] != 0);
    v3 navy;
    CHECK(!ship_paint_color(SHIP_PAINT_AS_BUILT, &navy) && ship_paint_color(5, &navy));
    ship_design *plain = (ship_design *)malloc(sizeof(ship_design)), *d = (ship_design *)malloc(sizeof(ship_design));
    for (int h = 0; h < HULL_COUNT; ++h) {
        ship_loadout l = ship_default_loadout(h);
        CHECK(ship_design_build(&l, 20.0f, plain));
        for (int shape = 0; shape < SHAPE_COUNT; ++shape) {
            ship_loadout styled = l;
            styled.look.field[LOOK_HULL_SHAPE] = (uint8_t)shape;
            CHECK(ship_design_build(&styled, 20.0f, d));
            // Nothing of her flying is changed: the same mass, balance, inertia, thrust and stats.
            CHECK(d->body.mass == plain->body.mass && memcmp(&d->body.center, &plain->body.center, sizeof(v3)) == 0);
            CHECK(memcmp(&d->body.inertia, &plain->body.inertia, sizeof(v3)) == 0 && d->body.draft == plain->body.draft);
            CHECK(memcmp(d->body.thrust, plain->body.thrust, sizeof(d->body.thrust)) == 0 && d->stats.top_rate.y == plain->stats.top_rate.y);
            CHECK(d->body.radius >= plain->body.radius);
            CHECK(shape == SHAPE_CLASSIC ? d->part_count == plain->part_count : d->part_count > plain->part_count);
            for (int i = plain->part_count; i < d->part_count; ++i) CHECK(d->parts[i - 0].mass >= 0.0f);
        }
    }
    // Paint: the deck takes the hull's, the thrusters theirs, the sail its own; the
    // skids, the masts and the helm keep their colours; as built, each its own.
    ship_loadout l = ship_default_loadout(HULL_SLOOP);
    CHECK(ship_design_build(&l, 20.0f, plain));
    l.look.field[LOOK_HULL_PAINT] = 5;
    l.look.field[LOOK_THRUSTER_PAINT] = 15;
    l.look.field[LOOK_SAIL_PAINT] = 4;
    CHECK(ship_design_build(&l, 20.0f, d));
    v3 ember, red;
    ship_paint_color(15, &ember);
    ship_paint_color(4, &red);
    int painted = 0;
    for (int i = 0; i < d->part_count; ++i) {
        const ship_part *p = &d->parts[i], *q = &plain->parts[i];
        CHECK(memcmp(&p->base, &q->color, sizeof(v3)) == 0);   // as built is what the plain one wears
        if (p->paint == PAINT_HULL) CHECK(memcmp(&p->color, &navy, sizeof(v3)) == 0);
        if (p->paint == PAINT_THRUSTER) CHECK(memcmp(&p->color, &ember, sizeof(v3)) == 0);
        if (p->paint == PAINT_SAIL) CHECK(memcmp(&p->color, &red, sizeof(v3)) == 0);
        if (p->paint == PAINT_TRIM || p->paint == PAINT_FIXED) CHECK(memcmp(&p->color, &q->color, sizeof(v3)) == 0);
        if (p->kind == PART_THRUSTER) CHECK(p->paint == PAINT_THRUSTER);
        if (p->kind == PART_RIGGING || p->kind == PART_HELM) CHECK(p->paint == PAINT_FIXED);
        painted += p->paint != PAINT_FIXED && memcmp(&p->color, &q->color, sizeof(v3)) != 0;
    }
    CHECK(painted > 10);
    CHECK(ship_design_build(&l, 20.0f, d) && d->body.mass == plain->body.mass);
    free(plain);
    free(d);

    // Every shape sits on its skids at a berth and is boarded up its stairs.
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    for (int shape = 0; shape < SHAPE_COUNT; ++shape) {
        ship_loadout styled = ship_default_loadout(HULL_CUTTER);
        styled.look.field[LOOK_HULL_SHAPE] = (uint8_t)shape;
        cw_entity_id owner = game_spawn_player(g, g->spawn, true);
        game_set_owner(g, owner, 300 + (uint64_t)shape);
        cw_entity_id craft = game_call_craft(g, owner, &styled);
        CHECK(craft != CW_INVALID_ENTITY);
        if (craft == CW_INVALID_ENTITY) continue;
        run_seconds(g, 0.5f);
        CHECK(fabsf(lowest_point(g, craft) - g->berths[craft_of(g, craft)->berth].floor.y) < 0.01f);
        transform_of(g, owner)->position = transform_of(g, owner)->previous = game_craft_to_world(g, craft, v3_make(0, -craft_of(g, craft)->draft, 20.0f));
        board_craft(g, owner, craft);
        CHECK(game_aboard(g, owner) == craft);
        v3 beside = game_craft_to_world(g, craft, v3_make(0.8f, 0, -3.0f));   // round the mast
        CHECK(walk_to(g, owner, beside.x, beside.z, 10.0f));
        v3 helm = game_craft_to_world(g, craft, craft_of(g, craft)->helm);
        CHECK(walk_to(g, owner, helm.x, helm.z, 10.0f));
        press_use(g, owner);
        CHECK(craft_of(g, craft)->pilot == owner);
    }
    game_destroy(g);
    free(g);

    // The look at the shipwright's: a field at a time, on the hull she sails.
    character c = {};
    character_fit_out(&c);
    shipwright_order paint = order_of(SHIPWRIGHT_STYLE, 0, LOOK_HULL_PAINT, 6), shape = order_of(SHIPWRIGHT_STYLE, 0, LOOK_HULL_SHAPE, SHAPE_ROYAL);
    int64_t cost = 0;
    CHECK(shipwright_quote(&c, &paint, &cost) == SHIPWRIGHT_DONE && cost == ship_look_price(LOOK_HULL_PAINT, 6));
    CHECK(shipwright_apply(&c, &paint) == SHIPWRIGHT_DONE && fleet_sailing(&c)->look.field[LOOK_HULL_PAINT] == 6);
    CHECK(shipwright_apply(&c, &paint) == SHIPWRIGHT_UNCHANGED);
    CHECK(shipwright_apply(&c, &shape) == SHIPWRIGHT_DONE && c.spent == ship_look_price(LOOK_HULL_PAINT, 6) + ship_look_price(LOOK_HULL_SHAPE, SHAPE_ROYAL));
    shipwright_order bad_field = order_of(SHIPWRIGHT_STYLE, 0, LOOK_FIELDS, 0), bad_value = order_of(SHIPWRIGHT_STYLE, 0, LOOK_SAIL_CUT, CUT_COUNT);
    CHECK(shipwright_apply(&c, &bad_field) == SHIPWRIGHT_INVALID && shipwright_apply(&c, &bad_value) == SHIPWRIGHT_INVALID);
    shipwright_order buy = order_of(SHIPWRIGHT_BUY_HULL, HULL_CUTTER, 0, 0), sail = order_of(SHIPWRIGHT_SAIL, HULL_CUTTER, 0, 0);
    CHECK(shipwright_apply(&c, &buy) == SHIPWRIGHT_DONE && shipwright_apply(&c, &sail) == SHIPWRIGHT_DONE);
    CHECK(fleet_sailing(&c)->look.field[LOOK_HULL_PAINT] == SHIP_PAINT_AS_BUILT);   // a new hull comes as built
    CHECK(c.ships.loadouts[HULL_SLOOP].look.field[LOOK_HULL_SHAPE] == SHAPE_ROYAL);   // the sloop keeps hers
}

// ---------------------------------------------------------------------------
// Docks, sails that keep her way, and captains
// ---------------------------------------------------------------------------
static cw_entity_id find_dock(game *g, const char *name) {
    cw_query docks = cw_query_begin(g->q_docks);
    cw_entity_id id;
    while (cw_query_next(&docks, &id))
        if (!strcmp(CW_GET(&g->world, id, g->c_dock, Dock)->name, name)) return id;
    return CW_INVALID_ENTITY;
}
// A craft in the air at `at`, its bow along `heading`, and a pilot at its helm.
static cw_entity_id flying_craft(game *g, int hull, v3 at, float heading, cw_entity_id *pilot) {
    ship_loadout l = ship_default_loadout(hull);
    cw_entity_id craft = game_spawn_craft(g, &l, at, heading, 0, -1);
    craft_of(g, craft)->resting = false;
    game_tick(g);
    *pilot = take_helm(g, craft);
    return craft;
}
static void press_dock(game *g, cw_entity_id player) {
    input_of(g, player)->dock = true;
    game_tick(g);
    input_of(g, player)->dock = false;
}
// Runs until a craft is docked, or the time is up; whether it was.
static bool until_docked(game *g, cw_entity_id craft, float seconds) {
    for (int i = 0; i < (int)(seconds * GAME_TICK_HZ); ++i) {
        game_tick(g);
        if (craft_of(g, craft)->docking == DOCK_DOCKED) return true;
    }
    return false;
}

// Docks: twelve of them, three to a deck at the hub and by each planet. A pilot
// flying near one asks with the dock key and the autopilot brings her in: over it,
// turned to its heading, down onto it, at rest, docked; the use key then leaves the
// helm, and asking her to move takes her out. Asking again, or a touch on the
// controls, calls a docking off; leaving the helm does not (she docks herself); a
// taken dock, or one too small, is not offered; landed, there is no docking. On a
// planet's deck, the same.
static void test_docking(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    CHECK(cw_query_count(g->q_docks) == 12);
    cw_entity_id small = find_dock(g, "Hub Dock 2"), large = find_dock(g, "Hub Dock 1"), other = find_dock(g, "Hub Dock 3");
    CHECK(small != CW_INVALID_ENTITY && large != CW_INVALID_ENTITY && other != CW_INVALID_ENTITY);
    CHECK(CW_GET(&g->world, large, g->c_dock, Dock)->size == BERTH_LARGE && CW_GET(&g->world, small, g->c_dock, Dock)->size == BERTH_SMALL);
    CHECK(find_dock(g, "Verdant Dock 1") != CW_INVALID_ENTITY && find_dock(g, "Azure Dock 3") != CW_INVALID_ENTITY);

    // A sloop, coming in from the south-east at thirty metres.
    cw_entity_id pilot;
    cw_entity_id craft = flying_craft(g, HULL_SLOOP, v3_make(80.0f, 30.0f, -40.0f), 1.0f, &pilot);
    CHECK(game_dock_in_reach(g, craft) == small);   // the nearest free one that takes her
    press_dock(g, pilot);
    Craft *c = craft_of(g, craft);
    CHECK(c->docking == DOCK_APPROACH && c->dock == small && c->pilot == pilot);
    CHECK(until_docked(g, craft, 40.0f));
    v3 floor = transform_of(g, small)->position, at = transform_of(g, craft)->position;
    CHECK(c->resting && fabsf(at.x - floor.x) < 0.4f && fabsf(at.z - floor.z) < 0.4f && fabsf(lowest_point(g, craft) - floor.y) < 0.05f);
    CHECK(v3_dot(bow_of(g, craft), CW_GET(&g->world, small, g->c_dock, Dock)->forward) > 0.998f);
    CHECK(!game_dock_free(g, small, CW_INVALID_ENTITY));
    // Docked: the use key leaves the helm, and she stays.
    press_use(g, pilot);
    CHECK(c->pilot == CW_INVALID_ENTITY && c->docking == DOCK_DOCKED);
    run_seconds(g, 1.0f);
    CHECK(c->resting && c->docking == DOCK_DOCKED);
    // At the helm again, lift: out of the dock.
    press_use(g, pilot);
    CHECK(c->pilot == pilot);
    input_of(g, pilot)->lift = 1.0f;
    run_seconds(g, 0.5f);
    CHECK(c->docking == DOCK_NONE && c->dock == CW_INVALID_ENTITY && !c->resting);
    CHECK(!game_dock_free(g, small, CW_INVALID_ENTITY));   // still over it
    run_seconds(g, 2.0f);
    input_of(g, pilot)->lift = 0.0f;
    CHECK(game_dock_free(g, small, CW_INVALID_ENTITY));    // clear of it

    // Another sloop takes the dock first; ours is offered the other small one, and
    // a touch on the controls calls her docking off.
    cw_entity_id other_pilot;
    cw_entity_id rival = flying_craft(g, HULL_SLOOP, v3_make(70.0f, 25.0f, -30.0f), 0.0f, &other_pilot);
    CHECK(game_craft_start_docking(g, rival, small));
    CHECK(game_dock_in_reach(g, craft) == other && !game_craft_start_docking(g, craft, small));
    press_dock(g, pilot);
    CHECK(c->docking == DOCK_APPROACH && c->dock == other);
    press_dock(g, pilot);   // asked again: called off
    CHECK(c->docking == DOCK_NONE && c->pilot == pilot);
    press_dock(g, pilot);
    run_seconds(g, 1.0f);
    input_of(g, pilot)->forward = 1.0f;   // a touch on the controls: called off too
    game_tick(g);
    input_of(g, pilot)->forward = 0.0f;
    CHECK(c->docking == DOCK_NONE && c->pilot == pilot);
    // A galleon is offered only the large dock; the small ones do not take her.
    cw_entity_id big_pilot;
    cw_entity_id big = flying_craft(g, HULL_GALLEON, v3_make(60.0f, 40.0f, 60.0f), 0.0f, &big_pilot);
    CHECK(game_dock_in_reach(g, big) == large && !game_craft_start_docking(g, big, other));
    // The rival's pilot leaves the helm: she docks herself, unmanned.
    press_use(g, other_pilot);
    CHECK(craft_of(g, rival)->pilot == CW_INVALID_ENTITY && craft_of(g, rival)->docking == DOCK_APPROACH);
    CHECK(until_docked(g, rival, 40.0f));

    // Landed near a dock, there is no docking: only in flight.
    cw_entity_id lander_pilot;
    cw_entity_id lander = flying_craft(g, HULL_SLOOP, v3_make(10.0f, 3.0f, -15.0f), 0.0f, &lander_pilot);
    input_of(g, lander_pilot)->landing = true;
    input_of(g, lander_pilot)->lift = -1.0f;
    run_seconds(g, 3.0f);
    input_of(g, lander_pilot)->lift = 0.0f;
    run_seconds(g, 1.0f);
    CHECK(craft_of(g, lander)->resting && !game_craft_airborne(g, lander));
    press_dock(g, lander_pilot);
    CHECK(craft_of(g, lander)->pilot == lander_pilot && craft_of(g, lander)->docking == DOCK_NONE);

    // By a planet, where the way down leans off the deck's: a brigantine docks.
    cw_entity_id far_pilot;
    cw_entity_id brig = flying_craft(g, HULL_BRIGANTINE, v3_make(-820.0f, 150.0f, -1300.0f), 0.0f, &far_pilot);
    cw_entity_id verdant = game_dock_in_reach(g, brig);
    CHECK(verdant == find_dock(g, "Verdant Dock 1"));
    press_dock(g, far_pilot);
    CHECK(until_docked(g, brig, 60.0f));
    CHECK(craft_of(g, brig)->dock == verdant && craft_of(g, brig)->resting);
    game_destroy(g);
    free(g);
}

// Set sails keep her way: flown ahead and let go, with her sails set she holds her
// speed and her height, pilot or none; with her sails in and nobody at the helm,
// her engines stop and she falls.
static void test_sails_keep_way(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    float kept[2], fell[2];
    for (int set = 0; set < 2; ++set) {
        cw_entity_id pilot;
        cw_entity_id craft = flying_craft(g, HULL_SLOOP, v3_make(120.0f + 200.0f * (float)set, 120.0f, 150.0f), 0.0f, &pilot);
        Craft *c = craft_of(g, craft);
        c->sails[0].set = set ? 1.0f : 0.0f;
        input_of(g, pilot)->forward = 1.0f;
        run_seconds(g, 2.0f);
        input_of(g, pilot)->forward = 0.0f;
        float speed = v3_dot(c->velocity, bow_of(g, craft)), height = transform_of(g, craft)->position.y;
        CHECK(speed > 10.0f);
        // Let go of the throttle: under sail she keeps it, else the computer stops her.
        run_seconds(g, 2.0f);
        float let_go = v3_dot(c->velocity, bow_of(g, craft));
        if (set) CHECK(fabsf(let_go - speed) < speed * 0.05f);
        else CHECK(let_go < speed * 0.3f);
        // Off the helm altogether.
        input_of(g, pilot)->forward = 1.0f;
        run_seconds(g, 2.0f);
        input_of(g, pilot)->forward = 0.0f;
        speed = v3_dot(c->velocity, bow_of(g, craft));
        height = transform_of(g, craft)->position.y;
        press_use(g, pilot);
        CHECK(c->pilot == CW_INVALID_ENTITY);
        run_seconds(g, 4.0f);
        kept[set] = v3_dot(c->velocity, bow_of(g, craft)) / speed;
        fell[set] = height - transform_of(g, craft)->position.y;
    }
    CHECK(kept[1] > 0.95f && kept[1] < 1.05f && fabsf(fell[1]) < 0.5f);   // under sail: on she goes
    CHECK(fell[0] > 20.0f);                                                 // sails in: her engines stop, and down she goes
    game_destroy(g);
    free(g);
}

// Captains: each aboard her own ship, at the helm, docked; a few minutes on, each
// has flown to another dock and docked there, and none of them is anyone's client.
static void test_captains(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    game_spawn_captains(g, 3, 11);
    CHECK(cw_query_count(g->q_captains) == 3);
    cw_entity_id captains[3];
    int n = 0;
    cw_query query = cw_query_begin(g->q_captains);
    cw_entity_id id;
    while (cw_query_next(&query, &id) && n < 3) captains[n++] = id;
    for (int i = 0; i < n; ++i) {
        const Captain *cap = CW_GET(&g->world, captains[i], g->c_captain, Captain);
        const Craft *c = craft_of(g, cap->craft);
        CHECK(c->docking == DOCK_DOCKED && c->resting && game_owner(g, cap->craft) == game_owner(g, captains[i]));
        CHECK(game_is_captain_account(g, game_owner(g, captains[i])) && !game_is_captain_account(g, 12345));
        CHECK(strncmp(player_of(g, captains[i])->name, "Captain ", 8) == 0);
    }
    cw_entity_id first_dock[3];
    for (int i = 0; i < n; ++i) first_dock[i] = craft_of(g, CW_GET(&g->world, captains[i], g->c_captain, Captain)->craft)->dock;
    run_seconds(g, 3.0f);
    for (int i = 0; i < n; ++i) {
        const Captain *cap = CW_GET(&g->world, captains[i], g->c_captain, Captain);
        CHECK(craft_of(g, cap->craft)->pilot == captains[i] && game_aboard(g, captains[i]) == cap->craft);
    }
    bool flew[3] = {}, away[3] = {};
    for (int s = 0; s < 180 * GAME_TICK_HZ; ++s) {
        game_tick(g);
        for (int i = 0; i < n; ++i) {
            const Captain *cap = CW_GET(&g->world, captains[i], g->c_captain, Captain);
            const Craft *c = craft_of(g, cap->craft);
            if (c->docking == DOCK_NONE && transform_of(g, cap->craft)->position.y > transform_of(g, first_dock[i])->position.y + 20.0f) flew[i] = true;
            if (cap->voyages > 0 && c->docking == DOCK_DOCKED && c->dock != first_dock[i]) away[i] = true;
        }
    }
    for (int i = 0; i < n; ++i) CHECK(flew[i] && away[i]);
    game_destroy(g);
    free(g);
}

// The simulation is a pure function of its inputs and tick count: two games fed
// the same scripted inputs end bit-identical, however the ticks are batched, and
// nothing in a tick reads a clock. This is what lets a server be authoritative, a
// client predict, and every zone agree about the ships.
static uint64_t hash_world(game *g) {
    uint64_t h = 0xcbf29ce484222325ull;
    cw_component_id ids[] = {g->c_transform, g->c_player, g->c_ship, g->c_craft, g->c_aboard};
    cw_query all = cw_query_iter(&g->world, cw_mask(&g->c_transform, 1));
    cw_entity_id id;
    while (cw_query_next(&all, &id)) {
        for (int k = 0; k < 5; ++k) {
            if (!cw_comp_has(&g->world, id, ids[k])) continue;
            size_t size = g->world.pools[ids[k]].elem_size;
            const unsigned char *bytes = (const unsigned char *)cw_comp_ptr(&g->world, id, ids[k]);
            for (size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 0x100000001b3ull;
        }
    }
    return h;
}
static void script_input(PlayerInput *in, int tick) {
    in->forward = tick % 200 < 120 ? 1.0f : 0.0f;
    in->strafe = tick % 300 > 250 ? 1.0f : 0.0f;
    in->yaw = (float)tick * 0.01f;
    in->jump = tick % 97 == 0;
}
// A pilot's: take the helm, then climb, turn, roll, fly on, decoupled a while.
static void script_flight(PlayerInput *in, int tick) {
    in->use = tick == 3;
    in->lift = tick < 300 ? 0.8f : tick % 400 < 100 ? -0.5f : 0.0f;
    in->forward = tick > 200 && tick % 500 < 300 ? 1.0f : 0.0f;
    in->turn_yaw = sinf((float)tick * 0.013f);
    in->turn_pitch = tick % 700 < 50 ? 0.6f : 0.0f;
    in->turn_roll = tick % 900 > 850 ? -1.0f : 0.0f;
    in->decoupled = tick % 1100 > 1000;
    in->landing = tick % 1300 > 1150;
}
// A sail hand's: take the sails, then hoist, brace this way and that, take in.
static void script_sails(PlayerInput *in, int tick) {
    in->use = tick == 5;
    in->sail = tick % 700 < 200 ? 1.0f : tick % 900 > 820 ? -1.0f : 0.0f;
    in->brace = sinf((float)tick * 0.007f);
}
static void test_determinism(void) {
    game *a = (game *)malloc(sizeof(game)), *b = (game *)malloc(sizeof(game));
    game_init(a, CW_HOST);
    game_init(b, CW_HOST);
    a->universe_base = b->universe_base = 12.0;   // the skiff leaves eight seconds in
    game_build_universe(a);
    game_build_universe(b);
    game_spawn_captains(a, 2, 5);   // and captains, flying by their own dice
    game_spawn_captains(b, 2, 5);
    cw_entity_id skiff = game_find_ship(a, "Emerald Skiff");
    v3 deck = ship_of(a, skiff)->hub_dock;
    cw_entity_id pa = game_spawn_player(a, deck, true), pb = game_spawn_player(b, deck, true);   // aboard from the start
    cw_entity_id qa = game_spawn_player(a, a->spawn, true), qb = game_spawn_player(b, b->spawn, true);
    CHECK(pa == pb && qa == qb);
    // And a craft out over the void with its pilot already at the helm's spot.
    v3 high = v3_make(300.0f, 80.0f, 200.0f);   // where the hub's gravity fades out
    cw_entity_id ca = game_spawn_craft(a, NULL, high, 0.3f, 0, -1), cb = game_spawn_craft(b, NULL, high, 0.3f, 0, -1);
    cw_entity_id ra = game_spawn_player(a, game_craft_to_world(a, ca, v3_make(0, 0.05f, -3.0f)), true);
    cw_entity_id rb = game_spawn_player(b, game_craft_to_world(b, cb, v3_make(0, 0.05f, -3.0f)), true);
    v3 station = v3_add(craft_of(a, ca)->sails[0].station, v3_make(0, 0.05f, 0));
    cw_entity_id ha = game_spawn_player(a, game_craft_to_world(a, ca, station), true);
    cw_entity_id hb = game_spawn_player(b, game_craft_to_world(b, cb, station), true);
    CHECK(ca == cb && ra == rb && ha == hb);
    // a: 2400 ticks one at a time; b: the same in batches of uneven size.
    int tick = 0;
    for (; tick < 2400; ++tick) {
        script_input(CW_GET(&a->world, pa, a->c_input, PlayerInput), tick);
        script_input(CW_GET(&a->world, qa, a->c_input, PlayerInput), tick + 1000);
        script_flight(CW_GET(&a->world, ra, a->c_input, PlayerInput), tick);
        script_sails(CW_GET(&a->world, ha, a->c_input, PlayerInput), tick);
        game_tick(a);
    }
    tick = 0;
    for (int batch = 1; tick < 2400; batch = batch % 37 + 1) {
        for (int i = 0; i < batch && tick < 2400; ++i, ++tick) {
            script_input(CW_GET(&b->world, pb, b->c_input, PlayerInput), tick);
            script_input(CW_GET(&b->world, qb, b->c_input, PlayerInput), tick + 1000);
            script_flight(CW_GET(&b->world, rb, b->c_input, PlayerInput), tick);
            script_sails(CW_GET(&b->world, hb, b->c_input, PlayerInput), tick);
            game_tick(b);
        }
    }
    CHECK(a->world.tick == b->world.tick);
    CHECK(hash_world(a) == hash_world(b));
    CHECK(memcmp(transform_of(a, pa), transform_of(b, pb), sizeof(Transform)) == 0);
    CHECK(memcmp(transform_of(a, qa), transform_of(b, qb), sizeof(Transform)) == 0);
    // The craft flew (and its pilot with it) and came out the same to the bit.
    CHECK(craft_of(a, ca)->pilot == ra && craft_of(a, ca)->sails[0].hand == ha);
    CHECK(v3_length(v3_sub(transform_of(a, ca)->position, high)) > 50.0f);
    CHECK(memcmp(transform_of(a, ca), transform_of(b, cb), sizeof(Transform)) == 0);
    CHECK(memcmp(&craft_of(a, ca)->orientation, &craft_of(b, cb)->orientation, sizeof(quat)) == 0);
    CHECK(ship_of(a, skiff)->state == SHIP_FLYING || ship_of(a, skiff)->stop == STOP_PLANET);   // time moved: 40 s
    CHECK(fabs(game_time_of_tick(a, a->world.tick) - (12.0 + 40.0)) < 1e-9);
    game_destroy(a);
    game_destroy(b);
    free(a);
    free(b);
}

int main(void) {
    test_craft_from_parts();
    test_craft_flight();
    test_gravity();
    test_craft_planet_landing();
    test_craft_levelling();
    test_craft_kick();
    test_chase_view();
    test_solar_current();
    test_craft_sails();
    test_currents_off();
    test_chart_fit();
    test_ship_loadouts();
    test_ship_stats();
    test_ship_lopsided();
    test_hulls_built_and_flown();
    test_masts();
    test_yard_hulls();
    test_shipwright_books();
    test_shipwright_yard();
    test_ship_looks();
    test_docking();
    test_sails_keep_way();
    test_captains();
    test_determinism();
    test_items();
    test_dungeons();
    test_players_stand_and_walls_block();
    test_boarding_and_flight();
    test_every_planet_has_a_ship();
    test_planet_land();
    test_save_and_restore();
    if (failures) {
        printf("test_game: %d failed\n", failures);
        return 1;
    }
    puts("test_game: ok");
    return 0;
}
