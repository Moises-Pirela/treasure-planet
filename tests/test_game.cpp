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

// Steers a player straight at a point on the ground for up to `seconds`.
static bool walk_to(game *g, cw_entity_id player, float x, float z, float seconds) {
    PlayerInput *in = CW_GET(&g->world, player, g->c_input, PlayerInput);
    for (int i = 0; i < (int)(seconds * GAME_TICK_HZ); ++i) {
        v3 p = transform_of(g, player)->position;
        float dx = x - p.x, dz = z - p.z;
        if (dx * dx + dz * dz < 0.04f) {
            in->forward = 0.0f;
            return true;
        }
        in->yaw = atan2f(-dx, -dz);
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

// The simulation is a pure function of its inputs and tick count: two games fed
// the same scripted inputs end bit-identical, however the ticks are batched, and
// nothing in a tick reads a clock. This is what lets a server be authoritative, a
// client predict, and every zone agree about the ships.
static uint64_t hash_world(game *g) {
    uint64_t h = 0xcbf29ce484222325ull;
    cw_component_id ids[] = {g->c_transform, g->c_player, g->c_ship};
    cw_query all = cw_query_iter(&g->world, cw_mask(&g->c_transform, 1));
    cw_entity_id id;
    while (cw_query_next(&all, &id)) {
        for (int k = 0; k < 3; ++k) {
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
    // a: 2400 ticks one at a time; b: the same in batches of uneven size.
    int tick = 0;
    for (; tick < 2400; ++tick) {
        script_input(CW_GET(&a->world, pa, a->c_input, PlayerInput), tick);
        script_input(CW_GET(&a->world, qa, a->c_input, PlayerInput), tick + 1000);
        game_tick(a);
    }
    tick = 0;
    for (int batch = 1; tick < 2400; batch = batch % 37 + 1) {
        for (int i = 0; i < batch && tick < 2400; ++i, ++tick) {
            script_input(CW_GET(&b->world, pb, b->c_input, PlayerInput), tick);
            script_input(CW_GET(&b->world, qb, b->c_input, PlayerInput), tick + 1000);
            game_tick(b);
        }
    }
    CHECK(a->world.tick == b->world.tick);
    CHECK(hash_world(a) == hash_world(b));
    CHECK(memcmp(transform_of(a, pa), transform_of(b, pb), sizeof(Transform)) == 0);
    CHECK(memcmp(transform_of(a, qa), transform_of(b, qb), sizeof(Transform)) == 0);
    CHECK(ship_of(a, skiff)->state == SHIP_FLYING || ship_of(a, skiff)->stop == STOP_PLANET);   // time moved: 40 s
    CHECK(fabs(game_time_of_tick(a, a->world.tick) - (12.0 + 40.0)) < 1e-9);
    game_destroy(a);
    game_destroy(b);
    free(a);
    free(b);
}

int main(void) {
    test_determinism();
    test_items();
    test_dungeons();
    test_players_stand_and_walls_block();
    test_boarding_and_flight();
    test_every_planet_has_a_ship();
    test_save_and_restore();
    if (failures) {
        printf("test_game: %d failed\n", failures);
        return 1;
    }
    puts("test_game: ok");
    return 0;
}
