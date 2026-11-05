// The headless zone server. Runs anywhere tp_game runs: no window, no renderer.
//   treasure_server [--port N] [--max-clients N] [--interest METRES [--forget SECONDS]] [--bandwidth KB/S] [--total-bandwidth KB/S] [--per-ip N] [--quiet] [--trace]
//                   [--zones FILE --zone NAME]
//                   [--directory HOST:PORT --zone NAME [--owns PLACE,...] [--instances] [--public HOST:PORT] [--secret WORD]
//                    [--region NAME] [--target N]]
//                   [--universe-epoch UNIX_SECONDS] [--save FILE] [--currents] [--captains N]
// Alone it owns the whole universe. With a zones file (zones.h) it owns the places
// listed for its name and hands players over to the other zones; with a directory
// (directory.h) it registers what it owns there and learns the others from it.
// Universe time, which ships run on, is the wall clock minus the epoch; every zone
// must be given the same epoch, and with none it is simply Unix time.
#include "../net/server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int) { stop_requested = 1; }

static int usage(void) {
    fputs("usage: treasure_server [--port N] [--max-clients N] [--interest METRES [--forget SECONDS]] [--bandwidth KB/S] [--total-bandwidth KB/S] [--per-ip N] [--quiet] [--trace]\n"
          "                       [--zones FILE --zone NAME]\n"
          "                       [--directory HOST:PORT --zone NAME [--owns PLACE,...] [--instances] [--public HOST:PORT] [--secret WORD]\n"
          "                        [--region NAME] [--target N]]\n"
          "                       [--universe-epoch UNIX_SECONDS] [--save FILE] [--currents] [--captains N]\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    server_config config = server_config_default();
    config.verbose = true;
    config.captains = 4;
    const char *zones_path = NULL, *zone_name = NULL, *directory = NULL, *owns = NULL, *public_address = NULL;
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--port") && more) config.port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--directory") && more) directory = argv[++i];
        else if (!strcmp(argv[i], "--owns") && more) owns = argv[++i];
        else if (!strcmp(argv[i], "--instances")) config.instance_host = true;
        else if (!strcmp(argv[i], "--public") && more) public_address = argv[++i];
        else if (!strcmp(argv[i], "--secret") && more) config.secret = argv[++i];
        else if (!strcmp(argv[i], "--region") && more) strncpy(config.region, argv[++i], NET_REGION_NAME - 1);
        else if (!strcmp(argv[i], "--target") && more) config.target = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && more) config.max_clients = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interest") && more) {   // players and loot alike; scenery is always sent
            float radius = (float)atof(argv[++i]);
            config.relevance[RELEVANCE_PLAYER].radius = config.relevance[RELEVANCE_LOOT].radius = radius;
        } else if (!strcmp(argv[i], "--per-ip") && more) {   // handshakes a second from one address; 0: no limit (load tests)
            double per_ip = atof(argv[++i]);
            config.unconnected_per_ip = per_ip > 0.0 ? per_ip : -1.0;
        } else if (!strcmp(argv[i], "--forget") && more) {
            double forget = atof(argv[++i]);
            for (int k = 0; k < RELEVANCE_CLASS_COUNT; ++k) config.relevance[k].forget = forget;
        }
        else if (!strcmp(argv[i], "--bandwidth") && more) config.bandwidth = (uint32_t)(atof(argv[++i]) * 1024.0);
        else if (!strcmp(argv[i], "--total-bandwidth") && more) config.total_bandwidth = (uint32_t)(atof(argv[++i]) * 1024.0);
        else if (!strcmp(argv[i], "--zones") && more) zones_path = argv[++i];
        else if (!strcmp(argv[i], "--zone") && more) zone_name = argv[++i];
        else if (!strcmp(argv[i], "--universe-epoch") && more) config.universe_epoch = atof(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) config.verbose = false;
        else if (!strcmp(argv[i], "--trace")) config.trace = true;
        else if (!strcmp(argv[i], "--save") && more) config.save_path = argv[++i];
        else if (!strcmp(argv[i], "--currents")) config.currents = true;   // the solar current flows (off for now)
        else if (!strcmp(argv[i], "--captains") && more) config.captains = atoi(argv[++i]);   // ships of the world's own (default 4)
        else return usage();
    }
    if (zones_path && !zone_name) return usage();
    if (directory && (zones_path || !zone_name)) return usage();
    if (!net_init()) {
        fputs("server: the network could not be initialised\n", stderr);
        return 1;
    }
    net_zone_table zones;
    net_zones_init(&zones);
    if (directory) {
        if (!net_address_parse(directory, NET_DIRECTORY_PORT, &config.directory)) {
            fprintf(stderr, "server: cannot resolve the directory %s\n", directory);
            return 1;
        }
        if (public_address && !net_address_parse(public_address, config.port, &config.public_address)) {
            fprintf(stderr, "server: cannot resolve %s\n", public_address);
            return 1;
        }
        strncpy(config.zone_name, zone_name, NET_ZONE_NAME - 1);
        if (owns) {
            char list[256];
            strncpy(list, owns, sizeof(list) - 1);
            list[sizeof(list) - 1] = '\0';
            for (char *word = list; *word && config.owns_count < NET_ZONE_PLACES;) {
                char *comma = strchr(word, ',');
                if (comma) *comma = '\0';
                strncpy(config.owns[config.owns_count++], word, NET_ZONE_NAME - 1);
                if (!comma) break;
                word = comma + 1;
            }
        }
    } else if (zones_path) {
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
    } else if (directory) {
        fprintf(stderr, "server: zone %s%s, registering with the directory at %s\n", zone_name,
                config.instance_host ? " (instances)" : "", directory);
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
        net_sleep(0.004);   // ticks are 16 ms apart; a few hundred updates a second is plenty
    }
    fputs("server: shutting down\n", stderr);
    server_destroy(s);
    free(s);
    net_shutdown();
    return 0;
}
