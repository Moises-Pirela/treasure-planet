// The headless zone server. Runs anywhere tp_game runs: no window, no renderer.
//   treasure_server [--port N] [--max-clients N] [--interest METRES] [--quiet] [--trace]
//                   [--zones FILE --zone NAME] [--universe-epoch UNIX_SECONDS]
// Alone it owns the whole universe. With a zones file (zones.h) it owns the places
// listed for its name and hands players over to the other zones. Universe time,
// which ships run on, is the wall clock minus the epoch; every zone must be given
// the same epoch, and with none it is simply Unix time.
#include "../net/server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int) { stop_requested = 1; }

static int usage(void) {
    fputs("usage: treasure_server [--port N] [--max-clients N] [--interest METRES] [--quiet] [--trace]\n"
          "                       [--zones FILE --zone NAME] [--universe-epoch UNIX_SECONDS]\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    server_config config = server_config_default();
    config.verbose = true;
    const char *zones_path = NULL, *zone_name = NULL;
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--port") && more) config.port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && more) config.max_clients = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interest") && more) config.interest_radius = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--zones") && more) zones_path = argv[++i];
        else if (!strcmp(argv[i], "--zone") && more) zone_name = argv[++i];
        else if (!strcmp(argv[i], "--universe-epoch") && more) config.universe_epoch = atof(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) config.verbose = false;
        else if (!strcmp(argv[i], "--trace")) config.trace = true;
        else return usage();
    }
    if ((zones_path == NULL) != (zone_name == NULL)) return usage();
    if (!net_init()) {
        fputs("server: the network could not be initialised\n", stderr);
        return 1;
    }
    net_zone_table zones;
    net_zones_init(&zones);
    if (zones_path) {
        char error[256];
        if (!net_zones_load(&zones, zones_path, error, sizeof(error))) {
            fprintf(stderr, "server: %s\n", error);
            return 1;
        }
        config.zone = net_zones_find(&zones, zone_name);
        if (config.zone < 0) {
            fprintf(stderr, "server: %s lists no zone named %s\n", zones_path, zone_name);
            return 1;
        }
        config.zones = &zones;
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
    if (config.zones) {
        const net_zone *zone = &zones.zones[config.zone];
        fprintf(stderr, "server: zone %s owns", zone->name);
        for (int i = 0; i < zone->place_count; ++i) fprintf(stderr, " %s", zone->places[i]);
        fprintf(stderr, "; %d zones in %s\n", zones.count, zones_path);
    }

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
