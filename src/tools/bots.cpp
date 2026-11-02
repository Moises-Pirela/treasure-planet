// Headless players for load testing a zone server: each bot is a full network
// client with its own replica, wandering the hub and hopping now and then.
//   treasure_bots [--connect host[:port]] [--count N] [--seconds S] [--quiet]
#include "../net/client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bot {
    game *replica;
    net_client *client;
    PlayerInput input;
    double next_turn;
    uint32_t seed;
};

static float bot_random(bot *b) {
    b->seed = b->seed * 1664525u + 1013904223u;
    return (float)(b->seed >> 8) / 16777216.0f;
}

int main(int argc, char **argv) {
    const char *address = "127.0.0.1";
    int count = 10;
    double seconds = 0.0;   // forever
    bool verbose = true;
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--connect") && more) address = argv[++i];
        else if (!strcmp(argv[i], "--count") && more) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) verbose = false;
        else {
            fputs("usage: treasure_bots [--connect host[:port]] [--count N] [--seconds S] [--quiet]\n", stderr);
            return 2;
        }
    }
    if (count <= 0) count = 1;
    if (!net_init()) return 1;

    bot *bots = (bot *)calloc((size_t)count, sizeof(bot));
    for (int i = 0; i < count; ++i) {
        bot *b = &bots[i];
        b->replica = (game *)malloc(sizeof(game));
        game_init(b->replica, CW_CLIENT);
        b->client = (net_client *)malloc(sizeof(net_client));
        if (!net_client_init(b->client, b->replica, address, 0.0f)) {
            fprintf(stderr, "bots: could not resolve %s\n", address);
            return 1;
        }
        b->seed = 0x1234567u + (uint32_t)i * 7919u;
        b->input.forward = 1.0f;
    }

    double start = net_seconds(), last_report = start;
    for (;;) {
        double now = net_seconds();
        if (seconds > 0.0 && now - start > seconds) break;
        int online = 0, gone = 0;
        float rtt_sum = 0.0f;
        for (int i = 0; i < count; ++i) {
            bot *b = &bots[i];
            net_client *c = b->client;
            if (c->state == NC_DISCONNECTED) { ++gone; continue; }
            if (now > b->next_turn) {
                b->input.yaw = bot_random(b) * 2.0f * TP_PI;
                b->input.jump = bot_random(b) < 0.2f;
                b->next_turn = now + 1.0 + 3.0 * (double)bot_random(b);
            }
            net_client_send_input(c, &b->input, now);
            net_client_update(c, now);
            if (c->state == NC_CONNECTED) {
                net_client_resolve(c, now);
                game_client_frame(b->replica);
                ++online;
                rtt_sum += c->rtt;
            }
        }
        if (verbose && now - last_report >= 5.0) {
            fprintf(stderr, "bots: %d online, %d gone, mean rtt %.1f ms, %zu entities in bot 0\n", online, gone,
                    online ? rtt_sum / (float)online * 1000.0f : 0.0f, cw_world_entity_count(&bots[0].replica->world));
            last_report = now;
        }
        if (gone == count) break;
        net_sleep(0.002);
    }

    int online = 0;
    for (int i = 0; i < count; ++i) {
        bot *b = &bots[i];
        if (b->client->state == NC_CONNECTED) ++online;
        else if (verbose && b->client->failure) fprintf(stderr, "bots: bot %d: %s\n", i, b->client->failure);
        net_client_destroy(b->client);
        game_destroy(b->replica);
        free(b->client);
        free(b->replica);
    }
    free(bots);
    net_shutdown();
    printf("bots: %d of %d online at the end\n", online, count);
    return online == count ? 0 : 1;
}
