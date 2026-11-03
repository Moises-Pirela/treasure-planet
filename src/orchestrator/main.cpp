// The orchestrator process: keeps shard servers running for the places and regions
// it is told about, starting more as they fill and stopping extras as they empty.
//   treasure_orchestrator --directory HOST:PORT --secret WORD --server PATH
//                         --places hub,Verdant --regions eu,na [--min 1] [--max 8]
//                         [--target 800] [--capacity 1000] [--ports 27600-27699]
//                         [--empty-stop SECONDS] [--universe-epoch UNIX_SECONDS] [--quiet]
#include "../net/orchestrator.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int) { stop_requested = 1; }

static int split(const char *list, char out[][NET_ZONE_NAME], int capacity, size_t width) {
    int n = 0;
    for (const char *at = list; *at && n < capacity;) {
        const char *comma = strchr(at, ',');
        size_t length = comma ? (size_t)(comma - at) : strlen(at);
        if (length >= width) length = width - 1;
        memset(out[n], 0, NET_ZONE_NAME);
        memcpy(out[n], at, length);
        ++n;
        if (!comma) break;
        at = comma + 1;
    }
    return n;
}

int main(int argc, char **argv) {
    orchestrator_config config = {};
    config.min = 1;
    config.max = 8;
    config.target = 800;
    config.capacity = 1000;
    config.port_from = 27600;
    config.port_to = 27699;
    config.empty_stop_after = 60.0;
    config.verbose = true;
    const char *directory = NULL, *places = "hub", *regions = "eu";
    char region_list[ORCHESTRATOR_MAX_REGIONS][NET_ZONE_NAME];
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--directory") && more) directory = argv[++i];
        else if (!strcmp(argv[i], "--secret") && more) config.secret = argv[++i];
        else if (!strcmp(argv[i], "--server") && more) config.server_path = argv[++i];
        else if (!strcmp(argv[i], "--places") && more) places = argv[++i];
        else if (!strcmp(argv[i], "--regions") && more) regions = argv[++i];
        else if (!strcmp(argv[i], "--min") && more) config.min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && more) config.max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--target") && more) config.target = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capacity") && more) config.capacity = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ports") && more) {
            int from = 0, to = 0;
            if (sscanf(argv[++i], "%d-%d", &from, &to) != 2) return 2;
            config.port_from = (uint16_t)from;
            config.port_to = (uint16_t)to;
        } else if (!strcmp(argv[i], "--empty-stop") && more) config.empty_stop_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--universe-epoch") && more) config.universe_epoch = atof(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) config.verbose = false;
        else {
            fputs("usage: treasure_orchestrator --directory HOST:PORT --secret WORD --server PATH\n"
                  "                             --places A,B --regions R,S [--min N] [--max N] [--target N] [--capacity N]\n"
                  "                             [--ports FROM-TO] [--empty-stop SECONDS] [--universe-epoch UNIX_SECONDS] [--quiet]\n", stderr);
            return 2;
        }
    }
    if (!directory || !config.server_path) {
        fputs("orchestrator: --directory and --server are needed\n", stderr);
        return 2;
    }
    if (!net_init()) return 1;
    if (!net_address_parse(directory, NET_DIRECTORY_PORT, &config.directory)) {
        fprintf(stderr, "orchestrator: cannot resolve %s\n", directory);
        return 1;
    }
    config.place_count = split(places, config.places, ORCHESTRATOR_MAX_PLACES, NET_ZONE_NAME);
    config.region_count = split(regions, region_list, ORCHESTRATOR_MAX_REGIONS, NET_REGION_NAME);
    for (int i = 0; i < config.region_count; ++i) strncpy(config.regions[i], region_list[i], NET_REGION_NAME - 1);

    orchestrator *o = (orchestrator *)malloc(sizeof(orchestrator));
    if (!orchestrator_init(o, &config)) return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "orchestrator: %d places in %d regions, %d to %d shards each, target %u of %u\n", config.place_count,
            config.region_count, config.min, config.max, (unsigned)config.target, (unsigned)config.capacity);
    while (!stop_requested) {
        orchestrator_update(o, net_seconds());
        net_sleep(0.05);
    }
    fputs("orchestrator: stopping every shard\n", stderr);
    orchestrator_destroy(o);
    free(o);
    net_shutdown();
    return 0;
}
