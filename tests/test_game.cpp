// Plays the game without a window: walk onto a ship, fly to a planet, walk off.
#include "../src/game/game.h"
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

    // Walking off the edge of the world puts you back at the hub.
    cw_entity_id q = game_spawn_player(g, v3_make(29, 0, 0), true);
    walk_to(g, q, 40.0f, 0.0f, 1.0f);
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
// Up a craft's stairs, from behind them, and onto its deck.
static void board_craft(game *g, cw_entity_id player, cw_entity_id craft) {
    v3 foot = game_craft_to_world(g, craft, v3_make(0, 0, 8.0f));
    CHECK(walk_to(g, player, foot.x, foot.z, 10.0f));
    v3 deck = game_craft_to_world(g, craft, v3_make(0, 0, 1.0f));
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
    CHECK(game_call_craft(g, player) == craft);
    CHECK(v3_length(v3_sub(transform_of(g, craft)->position, parked)) < 0.001f);
    CHECK(game_aboard(g, player) == craft);   // passengers go with it
    // Someone else's call gets a craft of their own at the next berth.
    cw_entity_id second = game_call_craft(g, other);
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
    // No drag in space: across the ground it goes on as it was. (Its bow does not:
    // the main engines sit below the centre of mass, so the push pitched it up, and
    // decoupled, nothing stops the turn.)
    CHECK(fabsf(c->velocity.x - coasting.x) < 0.05f && fabsf(c->velocity.z - coasting.z) < 0.05f);
    CHECK(c->velocity.y < coasting.y - 5.0f);   // and it falls
    CHECK(v3_length(c->spin) > 0.01f);
    in->decoupled = false;
    run_seconds(g, 3.0f);
    CHECK(v3_length(c->velocity) < 1.0f);

    // Fly back over the yard, and leave the helm: nothing holds it up any more, it
    // falls, lands on the yard and comes to rest there, its crew still aboard.
    v3 home = v3_add(g->berths[3].floor, v3_make(0, 8.0f, 0));
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
    CHECK(fabsf(lowest_point(g, craft) - g->berths[3].floor.y) < 0.05f);
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
}
static void test_determinism(void) {
    game *a = (game *)malloc(sizeof(game)), *b = (game *)malloc(sizeof(game));
    game_init(a, CW_HOST);
    game_init(b, CW_HOST);
    a->universe_base = b->universe_base = 12.0;   // the skiff leaves eight seconds in
    game_build_universe(a);
    game_build_universe(b);
    cw_entity_id skiff = game_find_ship(a, "Emerald Skiff");
    v3 deck = ship_of(a, skiff)->hub_dock;
    cw_entity_id pa = game_spawn_player(a, deck, true), pb = game_spawn_player(b, deck, true);   // aboard from the start
    cw_entity_id qa = game_spawn_player(a, a->spawn, true), qb = game_spawn_player(b, b->spawn, true);
    CHECK(pa == pb && qa == qb);
    // And a craft out over the void with its pilot already at the helm's spot.
    v3 high = v3_make(400.0f, 80.0f, 300.0f);
    cw_entity_id ca = game_spawn_craft(a, 0, high, 0.3f, 0, -1), cb = game_spawn_craft(b, 0, high, 0.3f, 0, -1);
    cw_entity_id ra = game_spawn_player(a, game_craft_to_world(a, ca, v3_make(0, 0.05f, -3.0f)), true);
    cw_entity_id rb = game_spawn_player(b, game_craft_to_world(b, cb, v3_make(0, 0.05f, -3.0f)), true);
    CHECK(ca == cb && ra == rb);
    // a: 2400 ticks one at a time; b: the same in batches of uneven size.
    int tick = 0;
    for (; tick < 2400; ++tick) {
        script_input(CW_GET(&a->world, pa, a->c_input, PlayerInput), tick);
        script_input(CW_GET(&a->world, qa, a->c_input, PlayerInput), tick + 1000);
        script_flight(CW_GET(&a->world, ra, a->c_input, PlayerInput), tick);
        game_tick(a);
    }
    tick = 0;
    for (int batch = 1; tick < 2400; batch = batch % 37 + 1) {
        for (int i = 0; i < batch && tick < 2400; ++i, ++tick) {
            script_input(CW_GET(&b->world, pb, b->c_input, PlayerInput), tick);
            script_input(CW_GET(&b->world, qb, b->c_input, PlayerInput), tick + 1000);
            script_flight(CW_GET(&b->world, rb, b->c_input, PlayerInput), tick);
            game_tick(b);
        }
    }
    CHECK(a->world.tick == b->world.tick);
    CHECK(hash_world(a) == hash_world(b));
    CHECK(memcmp(transform_of(a, pa), transform_of(b, pb), sizeof(Transform)) == 0);
    CHECK(memcmp(transform_of(a, qa), transform_of(b, qb), sizeof(Transform)) == 0);
    // The craft flew (and its pilot with it) and came out the same to the bit.
    CHECK(craft_of(a, ca)->pilot == ra);
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
