// The headless zone server. Runs anywhere tp_game runs: no window, no renderer.
//   treasure_server [--port N] [--max-clients N] [--interest METRES] [--quiet]
#include "../net/server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int) { stop_requested = 1; }

int main(int argc, char **argv) {
    server_config config = server_config_default();
    config.verbose = true;
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--port") && more) config.port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && more) config.max_clients = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interest") && more) config.interest_radius = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) config.verbose = false;
        else {
            fputs("usage: treasure_server [--port N] [--max-clients N] [--interest METRES] [--quiet]\n", stderr);
            return 2;
        }
    }
    if (!net_init()) {
        fputs("server: the network could not be initialised\n", stderr);
        return 1;
    }
    server *s = (server *)malloc(sizeof(server));
    if (!server_init(s, &config)) {
        fprintf(stderr, "server: could not listen on UDP port %u\n", (unsigned)config.port);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "server: listening on UDP port %u, %d players max, %d Hz ticks, %d Hz snapshots\n",
            (unsigned)server_port(s), config.max_clients, GAME_TICK_HZ, NET_SNAPSHOT_HZ);

    double last_report = net_seconds();
    while (!stop_requested) {
        double now = net_seconds();
        server_update(s, now);
        if (config.verbose && now - last_report > 60.0) {
            fprintf(stderr, "server: tick %llu, %d online, %zu entities\n", (unsigned long long)s->zone.world.tick,
                    s->client_count, cw_world_entity_count(&s->zone.world));
            last_report = now;
        }
        net_sleep(0.001);
    }
    fputs("server: shutting down\n", stderr);
    server_destroy(s);
    free(s);
    net_shutdown();
    return 0;
}
