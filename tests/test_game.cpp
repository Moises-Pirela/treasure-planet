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

int main(void) {
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
