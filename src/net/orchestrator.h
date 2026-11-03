// The orchestrator: runs shard processes so that every place has the shards its
// population needs, in every region served.
//
// It watches the directory's table (every shard's region, population and target)
// and keeps, per place and region: at least `min` shards; one more whenever every
// shard there is at or past its target (one at a time, since a starting shard takes
// a moment to register); and when an extra shard has stood empty for a while, it
// tells the directory to stop sending players there (drain), waits for it to be
// empty, and stops the process. Shards it started that die are noticed by their
// absence and replaced. The policy is a pure function of the table and what it
// runs, so it is tested without processes; the processes are `treasure_server`
// with the right arguments.
#ifndef TP_ORCHESTRATOR_H
#define TP_ORCHESTRATOR_H

#include "directory.h"
#include "process.h"

#define ORCHESTRATOR_MAX_SHARDS 64
#define ORCHESTRATOR_MAX_PLACES 8
#define ORCHESTRATOR_MAX_REGIONS 8
#define ORCHESTRATOR_QUERY_EVERY 1.0   // seconds between looks at the table
#define ORCHESTRATOR_START_GRACE 8.0   // seconds a started shard is given to register before it counts as dead

struct orchestrator_config {
    net_address directory;
    const char *secret;
    const char *server_path;     // the treasure_server executable
    char places[ORCHESTRATOR_MAX_PLACES][NET_ZONE_NAME];
    int place_count;
    char regions[ORCHESTRATOR_MAX_REGIONS][NET_REGION_NAME];
    int region_count;
    int min;                     // shards per place and region at least
    int max;                     // ...and at most
    uint16_t target, capacity;   // per shard
    uint16_t port_from, port_to; // the ports shards listen on
    double empty_stop_after;     // seconds an extra shard may be empty before it goes
    double universe_epoch;       // passed on, so every shard agrees
    bool verbose;
};

struct shard_process {
    bool used;
    char name[NET_ZONE_NAME], place[NET_ZONE_NAME], region[NET_REGION_NAME];
    uint16_t port;
    process proc;
    double started_at, empty_since;
    bool registered, draining;
};

enum plan_kind { PLAN_START, PLAN_DRAIN, PLAN_STOP };
struct plan {
    int kind;
    char place[NET_ZONE_NAME], region[NET_REGION_NAME];
    int shard;                   // for drain and stop: index in the shards array
};

// What should happen next, from the table, the shards run so far and the time:
// fills `out` with at most `capacity` actions and returns how many.
int orchestrator_plan(const net_zone_table *table, const orchestrator_config *config, shard_process *shards,
                      int shard_count, double now, plan *out, int capacity);

struct orchestrator {
    orchestrator_config config;
    net_socket *socket;
    net_zone_table table;
    bool have_table;
    shard_process shards[ORCHESTRATOR_MAX_SHARDS];
    double last_query, last_table;
    uint32_t started, drained, stopped, replaced;
    unsigned char scratch[NET_MAX_PACKET];
};

bool orchestrator_init(orchestrator *o, const orchestrator_config *config);
void orchestrator_update(orchestrator *o, double now);
void orchestrator_destroy(orchestrator *o);   // stops every shard it runs
int orchestrator_shard_count(const orchestrator *o);

#endif
