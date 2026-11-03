// The zone directory service: servers register with it, clients find zones through
// it, and it introduces clients to servers behind NAT. One small process.
//   treasure_directory [--port N] [--secret WORD] [--save FILE] [--quiet]
// --save keeps the players' characters (gear and stash) in FILE between runs.
#include "../net/directory.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int) { stop_requested = 1; }

int main(int argc, char **argv) {
    uint16_t port = NET_DIRECTORY_PORT;
    const char *secret = NULL, *save = NULL;
    bool verbose = true;
    for (int i = 1; i < argc; ++i) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--port") && more) port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--secret") && more) secret = argv[++i];
        else if (!strcmp(argv[i], "--save") && more) save = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) verbose = false;
        else {
            fputs("usage: treasure_directory [--port N] [--secret WORD] [--save FILE] [--quiet]\n", stderr);
            return 2;
        }
    }
    if (!net_init()) return 1;
    directory *d = (directory *)malloc(sizeof(directory));
    if (!directory_init(d, port, secret, verbose)) {
        fprintf(stderr, "directory: could not listen on UDP port %u\n", (unsigned)port);
        return 1;
    }
    if (save) {
        social_set_save_path(&d->people, save);
        if (social_load(&d->people, save)) fprintf(stderr, "directory: characters loaded from %s\n", save);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "directory: listening on UDP port %u\n", (unsigned)directory_port(d));
    double last_report = net_seconds();
    while (!stop_requested) {
        double now = net_seconds();
        directory_update(d, now);
        if (verbose && now - last_report > 60.0) {
            int live = 0;
            for (int i = 0; i < NET_MAX_ZONES; ++i) live += d->entries[i].used;
            fprintf(stderr, "directory: %d zones, %u registrations, %u queries, %u introductions\n", live,
                    d->registrations, d->queries, d->introductions);
            last_report = now;
        }
        net_sleep(0.005);
    }
    if (d->people.dirty) social_save(&d->people);
    directory_destroy(d);
    free(d);
    net_shutdown();
    return 0;
}
