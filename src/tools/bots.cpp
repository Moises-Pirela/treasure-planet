// Headless players for load testing a zone server: each bot is a full network
// client with its own replica. By default they wander the hub and hop now and then;
// with --ride they walk down a pier, board a ship, ride it to its planet, step off,
// go through the dungeon there (an instance of their own), ride back, and repeat,
// which crosses zones and worlds when those belong to different servers.
//   treasure_bots [--connect host[:port] | --directory host[:port] [--place NAME] [--region NAME]]
//                 [--count N] [--seconds S] [--ride] [--party] [--light] [--quiet]
// --light bots send inputs and acknowledge snapshots but neither predict nor
// interpolate, so one process can drive hundreds of them: for loading a server.
// With --party the first bot invites the rest into a fireteam (five at most; the
// others accept whatever they are offered), and riding bots then take the same ship,
// so they reach the same dungeon and share its instance.
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
    bool delved;                 // been through the dungeon at this planet: time to ride home
    v3 path[GAME_MAX_PLACES];    // the dungeon's rooms, then its exit
    int path_count, path_index;
    char path_dungeon[GAME_NAME_CAPACITY];
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

// Inside a dungeon: room to room, then the exit. The layout is a function of the
// name, so a scratch build of it gives the route.
static void delve(bot *b, v3 at) {
    if (strcmp(b->path_dungeon, b->session.link->world_name) != 0) {
        game *scratch = (game *)malloc(sizeof(game));
        game_init(scratch, CW_HOST);
        game_build_dungeon(scratch, b->session.link->world_name, "", v3_make(0, 0, 0), 0);
        b->path_count = game_dungeon_path(scratch, b->path, GAME_MAX_PLACES - 1);
        int exit_place = game_find_place(scratch, "exit");
        if (exit_place >= 0) b->path[b->path_count++] = scratch->places[exit_place].center;
        game_destroy(scratch);
        free(scratch);
        b->path_index = 0;
        strncpy(b->path_dungeon, b->session.link->world_name, GAME_NAME_CAPACITY - 1);
    }
    if (b->path_index >= b->path_count) return;
    if (walk_towards(&b->input, at, b->path[b->path_index])) ++b->path_index;
}

// One decision for a riding bot, from what its replica shows.
static void ride(bot *b) {
    net_session *s = &b->session;
    game *g = s->world;
    cw_world *w = &g->world;
    cw_entity_id player = s->link->player;
    if (!cw_world_is_valid(w, player)) return;
    v3 at = CW_GET(w, player, g->c_transform, Transform)->position;
    b->input.forward = 0.0f;
    if (s->link->world_name[0]) {
        delve(b, at);
        b->delved = true;
        return;
    }

    cw_entity_id ships[8];
    size_t ship_count = cw_query_gather(g->q_ships, ships, 8);
    if (!ship_count) return;
    cw_entity_id ship_id = ships[(size_t)b->ship_index % ship_count];
    const Ship *ship = CW_GET(w, ship_id, g->c_ship, Ship);
    v3 ship_at = CW_GET(w, ship_id, g->c_transform, Transform)->position;
    bool docked = ship->state == SHIP_DOCKED;
    v3 pier_end = v3_make(ship->hub_dock.x, 0.0f, ship->hub_dock.z + ship->half.z + 18.0f);   // on the hub, facing the pier
    v3 pad_edge = v3_make(ship->planet_dock.x, ship->planet_dock.y, ship->planet_dock.z + ship->half.z + 8.0f);

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
    } else if (place >= 0 && g->places[place].kind == PLACE_ZONE && place == 0) {
        // At the hub: line up with the pier first, then straight down it onto the deck.
        b->delved = false;
        if (!docked || ship->stop != STOP_HUB) return;
        if (fabsf(at.x - pier_end.x) > 0.5f) walk_towards(&b->input, at, pier_end);
        else walk_towards(&b->input, at, ship_at);
    } else if (place >= 0) {
        // On a landing pad: the dungeon first, then straight onto the deck once the
        // ship is back.
        if (!b->delved) {
            char dungeon[GAME_NAME_CAPACITY];
            game_dungeon_name(g->places[place].kind == PLACE_ZONE ? g->places[place].name : g->places[place].destination,
                              dungeon, sizeof(dungeon));
            int door = game_find_place(g, dungeon);
            if (door >= 0) walk_towards(&b->input, at, g->places[door].center);
            else b->delved = true;
        } else if (docked && ship->stop == STOP_PLANET) {
            walk_towards(&b->input, at, ship_at);
        }
    }
}

int main(int argc, char **argv) {
    const char *address = "127.0.0.1", *directory = NULL, *place = "hub", *region = "";
    int count = 10;
    double seconds = 0.0;   // forever
    bool verbose = true, riding = false, party = false, light = false;
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--connect") && more) address = argv[++i];
        else if (!strcmp(argv[i], "--directory") && more) directory = argv[++i];
        else if (!strcmp(argv[i], "--place") && more) place = argv[++i];
        else if (!strcmp(argv[i], "--region") && more) region = argv[++i];
        else if (!strcmp(argv[i], "--count") && more) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ride")) riding = true;
        else if (!strcmp(argv[i], "--party")) party = true;
        else if (!strcmp(argv[i], "--light")) light = true;
        else if (!strcmp(argv[i], "--quiet")) verbose = false;
        else {
            fputs("usage: treasure_bots [--connect host[:port] | --directory host[:port] [--place NAME]]\n"
                  "                     [--count N] [--seconds S] [--ride] [--party] [--light] [--quiet]\n", stderr);
            return 2;
        }
    }
    if (count <= 0) count = 1;
    if (!net_init()) return 1;

    bot *bots = (bot *)calloc((size_t)count, sizeof(bot));
    for (int i = 0; i < count; ++i) {
        bot *b = &bots[i];
        char name[ACCOUNT_NAME_CAPACITY];
        snprintf(name, sizeof(name), "b%03x-%d", (unsigned)(net_wall_seconds() * 1000.0) & 0xfffu, i % 100000);   // unique per run
        net_identity identity = net_identity_local(name);
        strncpy(identity.region, region, NET_REGION_NAME - 1);
        bool ok = directory ? net_session_init_directory(&b->session, directory, place, 0.0f, &identity)
                            : net_session_init(&b->session, address, 0.0f, &identity);
        if (!ok) {
            fprintf(stderr, "bots: %s: %s\n", directory ? directory : address, b->session.failure);
            return 1;
        }
        b->session.lightweight = light;   // a load generator: the server is what is under test
        b->seed = 0x1234567u + (uint32_t)i * 7919u;
        b->ship_index = party ? 0 : i;   // a fireteam travels together
        b->input.forward = riding ? 0.0f : 1.0f;
    }

    double start = net_seconds(), last_report = start, last_invite = 0.0;
    uint32_t frames = 0;
    double frame_time = 0.0;
    for (;;) {
        double now = net_seconds();
        if (seconds > 0.0 && now - start > seconds) break;
        ++frames;
        if (party && now - last_invite > 1.0 && net_session_state(&bots[0].session) == NC_CONNECTED) {
            // The first bot gathers the others, one invitation a second until the party is full.
            last_invite = now;
            const party_view *view = net_session_party(&bots[0].session);
            for (int i = 1; i < count && i < SOCIAL_PARTY_SIZE; ++i) {
                bool in = false;
                for (int k = 0; k < view->count; ++k) {
                    in = in || strcmp(view->members[k].name, bots[i].session.identity.name) == 0;
                }
                if (in) continue;
                net_session_party_request(&bots[0].session, PARTY_INVITE, bots[i].session.identity.name);
                break;
            }
        }
        int online = 0, gone = 0, handoffs = 0, handoffs_failed = 0;
        float rtt_sum = 0.0f, error_sum = 0.0f, handoff_seconds = 0.0f;
        double net_rtt_sum = 0.0, loss_sum = 0.0;
        for (int i = 0; i < count; ++i) {
            bot *b = &bots[i];
            if (net_session_state(&b->session) == NC_DISCONNECTED) { ++gone; continue; }
            if (party && net_session_party(&b->session)->invite) net_session_party_request(&b->session, PARTY_ACCEPT, NULL);
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
                net_rtt_sum += b->session.link->connection.rtt;
                loss_sum += b->session.link->connection.loss;
                error_sum += b->session.link->last_error;
            }
            handoffs += b->session.handoffs;
            handoffs_failed += b->session.handoffs_failed;
            if (b->session.last_handoff_seconds > handoff_seconds) handoff_seconds = b->session.last_handoff_seconds;
        }
        frame_time += net_seconds() - now;
        if (verbose && now - last_report >= 5.0) {
            fprintf(stderr, "bots: %d online, %d gone, %d hand-offs (%d failed, slowest %.0f ms), mean input rtt %.1f ms, "
                    "mean packet rtt %.1f ms, mean loss %.1f%%, mean misprediction %.1f mm, party of %d, %.1f ms per frame\n",
                    online, gone, handoffs, handoffs_failed, handoff_seconds * 1000.0f, online ? rtt_sum / (float)online * 1000.0f : 0.0f,
                    online ? net_rtt_sum / online * 1000.0 : 0.0, online ? loss_sum / online * 100.0 : 0.0,
                    online ? error_sum / (float)online * 1000.0f : 0.0f, net_session_party(&bots[0].session)->count,
                    frames ? frame_time / frames * 1000.0 : 0.0);
            frames = 0;
            frame_time = 0.0;
            if (!light && net_session_state(&bots[0].session) == NC_CONNECTED) {
                // What bot 0 sees of the crowd: how many players it holds, how many
                // were refreshed lately, and how stale the stalest is.
                game *g = bots[0].session.world;
                net_client *c = bots[0].session.link;
                int known = 0, fresh = 0;
                uint32_t oldest = 0;
                cw_query players = cw_query_begin(g->q_all_players);
                cw_entity_id id;
                while (cw_query_next(&players, &id)) {
                    if (id == c->player || !cw_comp_has(&g->world, id, c->c_replica)) continue;
                    const Replica *rep = CW_GET(&g->world, id, c->c_replica, Replica);
                    if (!rep->count) continue;
                    ++known;
                    uint32_t age = c->latest_tick - rep->last_seen_tick;
                    if (age < 5 * GAME_TICK_HZ) ++fresh;
                    if (age > oldest) oldest = age;
                }
                fprintf(stderr, "bots: bot 0 sees %d players, %d refreshed within 5 s, the stalest %.1f s ago, %llu B/s in\n", known, fresh,
                        (float)oldest / GAME_TICK_HZ, (unsigned long long)(c->connection.bytes_received / (uint64_t)(now - start + 1.0)));
            }
            if (riding && net_session_state(&bots[0].session) == NC_CONNECTED) {
                game *g = bots[0].session.world;
                cw_entity_id player = bots[0].session.link->player;
                v3 at = CW_GET(&g->world, player, g->c_transform, Transform)->position;
                cw_entity_id ships[8];
                size_t ship_count = cw_query_gather(g->q_ships, ships, 8);
                const Ship *ship = ship_count ? CW_GET(&g->world, ships[0], g->c_ship, Ship) : NULL;
                if (bots[0].session.link->world_name[0])
                    fprintf(stderr, "bots: bot 0 in %s at %.1f %.1f %.1f, waypoint %d of %d\n", bots[0].session.link->world_name,
                            at.x, at.y, at.z, bots[0].path_index, bots[0].path_count);
                else
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
        else if (verbose && b->session.failure) {
            const net_client *c = b->session.link;
            fprintf(stderr, "bots: bot %d: %s (%s, %u packets in, %u rejected, %u forged, %llu bytes out)\n", i,
                    b->session.failure, c && c->connection.encrypted ? "had keys" : "no keys", c ? c->connection.packets_received : 0,
                    c ? c->connection.packets_rejected : 0, c ? c->connection.packets_forged : 0,
                    c ? (unsigned long long)c->connection.bytes_sent : 0ull);
        }
        net_session_destroy(&b->session);
    }
    free(bots);
    net_shutdown();
    printf("bots: %d of %d online at the end\n", online, count);
    return online == count ? 0 : 1;
}
