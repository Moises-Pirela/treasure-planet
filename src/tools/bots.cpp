// Headless players for load testing a zone server: each bot is a full network
// client with its own replica. By default they wander the hub and hop now and then;
// with --ride they walk down a pier, board a ship, ride it to its planet, step off,
// and ride back, which crosses zones when the places belong to different servers.
//   treasure_bots [--connect host[:port]] [--count N] [--seconds S] [--ride] [--quiet]
#include "../net/client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bot {
    net_session session;
    PlayerInput input;
    double next_turn;
    uint32_t seed;
    int ship_index;              // which ship this bot rides
    bool flown;                  // aboard since the ship left: step off at the next dock
};

static float bot_random(bot *b) {
    b->seed = b->seed * 1664525u + 1013904223u;
    return (float)(b->seed >> 8) / 16777216.0f;
}

// Steers towards a point on the ground; true when there.
static bool walk_towards(PlayerInput *in, v3 from, v3 to) {
    float dx = to.x - from.x, dz = to.z - from.z;
    if (dx * dx + dz * dz < 0.09f) {
        in->forward = 0.0f;
        return true;
    }
    in->yaw = atan2f(-dx, -dz);
    in->forward = 1.0f;
    return false;
}

// One decision for a riding bot, from what its replica shows.
static void ride(bot *b) {
    net_session *s = &b->session;
    game *g = s->world;
    cw_world *w = &g->world;
    cw_entity_id player = s->link->player;
    if (!cw_world_is_valid(w, player)) return;
    v3 at = CW_GET(w, player, g->c_transform, Transform)->position;

    cw_entity_id ships[8];
    size_t ship_count = cw_query_gather(g->q_ships, ships, 8);
    if (!ship_count) return;
    cw_entity_id ship_id = ships[(size_t)b->ship_index % ship_count];
    const Ship *ship = CW_GET(w, ship_id, g->c_ship, Ship);
    v3 ship_at = CW_GET(w, ship_id, g->c_transform, Transform)->position;
    bool docked = ship->state == SHIP_DOCKED;
    v3 pier_end = v3_make(ship->hub_dock.x, 0.0f, ship->hub_dock.z + ship->half.z + 18.0f);   // on the hub, facing the pier
    v3 pad_edge = v3_make(ship->planet_dock.x, ship->planet_dock.y, ship->planet_dock.z + ship->half.z + 8.0f);

    b->input.forward = 0.0f;
    if (b->flown) {
        // Disembarking after a flight: all the way to the pier or the pad before
        // deciding anything else. Our prediction runs ahead of the server, so a
        // decision taken right at a boundary would race the server's view of it.
        if (docked && walk_towards(&b->input, at, ship->stop == STOP_PLANET ? pad_edge : pier_end)) b->flown = false;
        return;
    }
    int place = game_place_at(g, at);
    if (game_ship_at(g, at) == ship_id) {
        if (!docked) b->flown = true;                 // under way: step off at the next dock
        else walk_towards(&b->input, at, ship_at);    // still on the gangway: all the way in
    } else if (place == 0) {
        // At the hub: line up with the pier first, then straight down it onto the deck.
        if (!docked || ship->stop != STOP_HUB) return;
        if (fabsf(at.x - pier_end.x) > 0.5f) walk_towards(&b->input, at, pier_end);
        else walk_towards(&b->input, at, ship_at);
    } else if (place > 0) {
        // On a landing pad: straight onto the deck once the ship is back.
        if (docked && ship->stop == STOP_PLANET) walk_towards(&b->input, at, ship_at);
    }
}

int main(int argc, char **argv) {
    const char *address = "127.0.0.1";
    int count = 10;
    double seconds = 0.0;   // forever
    bool verbose = true, riding = false;
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--connect") && more) address = argv[++i];
        else if (!strcmp(argv[i], "--count") && more) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ride")) riding = true;
        else if (!strcmp(argv[i], "--quiet")) verbose = false;
        else {
            fputs("usage: treasure_bots [--connect host[:port]] [--count N] [--seconds S] [--ride] [--quiet]\n", stderr);
            return 2;
        }
    }
    if (count <= 0) count = 1;
    if (!net_init()) return 1;

    bot *bots = (bot *)calloc((size_t)count, sizeof(bot));
    for (int i = 0; i < count; ++i) {
        bot *b = &bots[i];
        if (!net_session_init(&b->session, address, 0.0f)) {
            fprintf(stderr, "bots: %s: %s\n", address, b->session.failure);
            return 1;
        }
        b->seed = 0x1234567u + (uint32_t)i * 7919u;
        b->ship_index = i;
        b->input.forward = riding ? 0.0f : 1.0f;
    }

    double start = net_seconds(), last_report = start;
    for (;;) {
        double now = net_seconds();
        if (seconds > 0.0 && now - start > seconds) break;
        int online = 0, gone = 0, handoffs = 0, handoffs_failed = 0;
        float rtt_sum = 0.0f, error_sum = 0.0f, handoff_seconds = 0.0f;
        for (int i = 0; i < count; ++i) {
            bot *b = &bots[i];
            if (net_session_state(&b->session) == NC_DISCONNECTED) { ++gone; continue; }
            if (riding) {
                if (net_session_state(&b->session) == NC_CONNECTED) ride(b);
            } else if (now > b->next_turn) {
                b->input.yaw = bot_random(b) * 2.0f * TP_PI;
                b->input.jump = bot_random(b) < 0.2f;
                b->next_turn = now + 1.0 + 3.0 * (double)bot_random(b);
            }
            net_session_frame(&b->session, &b->input, now);
            if (net_session_state(&b->session) == NC_CONNECTED) {
                ++online;
                rtt_sum += b->session.link->rtt;
                error_sum += b->session.link->last_error;
            }
            handoffs += b->session.handoffs;
            handoffs_failed += b->session.handoffs_failed;
            if (b->session.last_handoff_seconds > handoff_seconds) handoff_seconds = b->session.last_handoff_seconds;
        }
        if (verbose && now - last_report >= 5.0) {
            fprintf(stderr, "bots: %d online, %d gone, %d hand-offs (%d failed, slowest %.0f ms), mean rtt %.1f ms, "
                    "mean misprediction %.1f mm\n", online, gone, handoffs, handoffs_failed, handoff_seconds * 1000.0f,
                    online ? rtt_sum / (float)online * 1000.0f : 0.0f, online ? error_sum / (float)online * 1000.0f : 0.0f);
            if (riding && net_session_state(&bots[0].session) == NC_CONNECTED) {
                game *g = bots[0].session.world;
                cw_entity_id player = bots[0].session.link->player;
                v3 at = CW_GET(&g->world, player, g->c_transform, Transform)->position;
                cw_entity_id ships[8];
                size_t ship_count = cw_query_gather(g->q_ships, ships, 8);
                const Ship *ship = ship_count ? CW_GET(&g->world, ships[0], g->c_ship, Ship) : NULL;
                fprintf(stderr, "bots: bot 0 at %.1f %.1f %.1f, place %d, aboard %lld; %s is %s the %s (%.0f s in)\n", at.x,
                        at.y, at.z, game_place_at(g, at), (long long)game_ship_at(g, at), ship ? ship->name : "?",
                        ship && ship->state == SHIP_DOCKED ? "docked at" : "flying to",
                        ship && ship->stop == STOP_PLANET ? "planet" : "hub", ship ? ship->timer : 0.0f);
            }
            last_report = now;
        }
        if (gone == count) break;
        net_sleep(0.002);
    }

    int online = 0;
    for (int i = 0; i < count; ++i) {
        bot *b = &bots[i];
        if (net_session_state(&b->session) == NC_CONNECTED) ++online;
        else if (verbose && b->session.failure) fprintf(stderr, "bots: bot %d: %s\n", i, b->session.failure);
        net_session_destroy(&b->session);
    }
    free(bots);
    net_shutdown();
    printf("bots: %d of %d online at the end\n", online, count);
    return online == count ? 0 : 1;
}
