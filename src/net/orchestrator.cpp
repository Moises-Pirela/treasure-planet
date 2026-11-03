#include "orchestrator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------
static const net_zone *zone_named(const net_zone_table *table, const char *name) {
    int i = net_zones_find(table, name);
    return i >= 0 ? &table->zones[i] : NULL;
}

int orchestrator_plan(const net_zone_table *table, const orchestrator_config *config, shard_process *shards,
                      int shard_count, double now, plan *out, int capacity) {
    int n = 0;
    for (int pi = 0; pi < config->place_count && n < capacity; ++pi) {
        for (int ri = 0; ri < config->region_count && n < capacity; ++ri) {
            const char *place = config->places[pi], *region = config->regions[ri];
            // What serves this place in this region: shards we run, starting or
            // registered, plus any in the table we did not start (someone else's).
            int serving = 0, at_target = 0, starting = 0;
            for (int i = 0; i < shard_count; ++i) {
                shard_process *sp = &shards[i];
                if (!sp->used || strcmp(sp->place, place) != 0 || strcmp(sp->region, region) != 0) continue;
                const net_zone *z = zone_named(table, sp->name);
                if (z) sp->registered = true;
                if (sp->draining) continue;
                ++serving;
                if (!z) {
                    if (now - sp->started_at < ORCHESTRATOR_START_GRACE || !sp->registered) ++starting;
                    continue;
                }
                if (config->target && z->clients >= config->target) ++at_target;
            }
            for (int i = 0; i < table->count; ++i) {
                const net_zone *z = &table->zones[i];
                if (z->instance_host || z->draining || strcmp(z->region, region) != 0) continue;
                bool owns = false, ours = false;
                for (int k = 0; k < z->place_count; ++k) owns = owns || strcmp(z->places[k], place) == 0;
                for (int k = 0; k < shard_count; ++k) ours = ours || (shards[k].used && strcmp(shards[k].name, z->name) == 0);
                if (!owns || ours) continue;
                ++serving;
                if (z->target && z->clients >= z->target) ++at_target;
            }
            bool want_more = serving < config->min || (serving > 0 && at_target == serving && starting == 0);
            if (want_more && serving < config->max && starting == 0) {
                out[n].kind = PLAN_START;
                strncpy(out[n].place, place, NET_ZONE_NAME - 1);
                strncpy(out[n].region, region, NET_REGION_NAME - 1);
                out[n].shard = -1;
                ++n;
                continue;
            }
            // Too many: draining ones that are empty stop; of the rest, the youngest,
            // if it has been empty long enough and what remains has room, drains. Only
            // the youngest is ever drained, so the original shards persist.
            int youngest = -1;
            for (int i = 0; i < shard_count && n < capacity; ++i) {
                shard_process *sp = &shards[i];
                if (!sp->used || strcmp(sp->place, place) != 0 || strcmp(sp->region, region) != 0) continue;
                const net_zone *z = zone_named(table, sp->name);
                bool empty = !z || z->clients == 0;
                if (sp->draining) {
                    if (empty) {
                        out[n].kind = PLAN_STOP;
                        out[n].shard = i;
                        strncpy(out[n].place, place, NET_ZONE_NAME - 1);
                        strncpy(out[n].region, region, NET_REGION_NAME - 1);
                        ++n;
                    }
                    continue;
                }
                if (!empty || !sp->registered) sp->empty_since = 0.0;
                else if (sp->empty_since == 0.0) sp->empty_since = now;
                if (youngest < 0 || sp->started_at > shards[youngest].started_at) youngest = i;
            }
            if (youngest >= 0 && n < capacity && serving > config->min && at_target < serving - 1) {
                shard_process *sp = &shards[youngest];
                if (sp->empty_since != 0.0 && now - sp->empty_since >= config->empty_stop_after) {
                    out[n].kind = PLAN_DRAIN;
                    out[n].shard = youngest;
                    strncpy(out[n].place, place, NET_ZONE_NAME - 1);
                    strncpy(out[n].region, region, NET_REGION_NAME - 1);
                    ++n;
                }
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Running shards
// ---------------------------------------------------------------------------
bool orchestrator_init(orchestrator *o, const orchestrator_config *config) {
    memset(o, 0, sizeof(*o));
    o->config = *config;
    if (o->config.min < 1) o->config.min = 1;
    if (o->config.max < o->config.min) o->config.max = o->config.min;
    if (o->config.empty_stop_after <= 0.0) o->config.empty_stop_after = 60.0;
    o->socket = net_socket_open(0);
    if (!o->socket) return false;
    net_zones_init(&o->table);
    if (config->secret && *config->secret) net_zones_set_secret(&o->table, config->secret);
    o->last_query = -1e9;
    return true;
}

int orchestrator_shard_count(const orchestrator *o) {
    int n = 0;
    for (int i = 0; i < ORCHESTRATOR_MAX_SHARDS; ++i) n += o->shards[i].used;
    return n;
}

static void send_plain(orchestrator *o, uint8_t type, const void *payload, size_t size, double now) {
    net_connection tmp;
    net_connection_init(&tmp, o->socket, o->config.directory, 0, NULL, NULL);
    net_connection_send(&tmp, type, NET_UNRELIABLE, payload, size);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

static uint16_t free_port(const orchestrator *o) {
    for (uint16_t port = o->config.port_from; port <= o->config.port_to && port != 0; ++port) {
        bool taken = false;
        for (int i = 0; i < ORCHESTRATOR_MAX_SHARDS; ++i) taken = taken || (o->shards[i].used && o->shards[i].port == port);
        if (!taken) return port;
    }
    return 0;
}

static void start_shard(orchestrator *o, const char *place, const char *region, double now) {
    shard_process *sp = NULL;
    for (int i = 0; i < ORCHESTRATOR_MAX_SHARDS && !sp; ++i)
        if (!o->shards[i].used) sp = &o->shards[i];
    uint16_t port = free_port(o);
    if (!sp || !port) {
        if (o->config.verbose) fputs("orchestrator: no room for another shard\n", stderr);
        return;
    }
    // The smallest number not in use for this place and region.
    int number = 1;
    for (bool taken = true; taken; ++number) {
        taken = false;
        for (int i = 0; i < ORCHESTRATOR_MAX_SHARDS; ++i) {
            char candidate[NET_ZONE_NAME];
            snprintf(candidate, sizeof(candidate), "%s-%s-%d", place, region, number);
            taken = taken || (o->shards[i].used && strcmp(o->shards[i].name, candidate) == 0);
        }
        if (!taken) --number;
    }
    memset(sp, 0, sizeof(*sp));
    snprintf(sp->name, sizeof(sp->name), "%s-%s-%d", place, region, number);
    strncpy(sp->place, place, NET_ZONE_NAME - 1);
    strncpy(sp->region, region, NET_REGION_NAME - 1);
    sp->port = port;

    char directory[32], port_text[8], target[8], capacity[8], epoch[32];
    net_address_format(o->config.directory, directory, sizeof(directory));
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    snprintf(target, sizeof(target), "%u", (unsigned)o->config.target);
    snprintf(capacity, sizeof(capacity), "%u", (unsigned)o->config.capacity);
    snprintf(epoch, sizeof(epoch), "%.3f", o->config.universe_epoch);
    const char *argv[32];
    int n = 0;
    argv[n++] = o->config.server_path;
    argv[n++] = "--directory"; argv[n++] = directory;
    argv[n++] = "--secret"; argv[n++] = o->config.secret ? o->config.secret : "";
    argv[n++] = "--zone"; argv[n++] = sp->name;
    argv[n++] = "--owns"; argv[n++] = sp->place;
    argv[n++] = "--region"; argv[n++] = sp->region;
    argv[n++] = "--target"; argv[n++] = target;
    argv[n++] = "--max-clients"; argv[n++] = capacity;
    argv[n++] = "--port"; argv[n++] = port_text;
    argv[n++] = "--universe-epoch"; argv[n++] = epoch;
    if (!o->config.verbose) argv[n++] = "--quiet";
    argv[n] = NULL;
    if (!process_start(&sp->proc, argv)) {
        if (o->config.verbose) fprintf(stderr, "orchestrator: could not start %s\n", o->config.server_path);
        return;
    }
    sp->used = true;
    sp->started_at = now;
    ++o->started;
    if (o->config.verbose) fprintf(stderr, "orchestrator: started %s on port %u\n", sp->name, (unsigned)port);
}

static void drain_shard(orchestrator *o, shard_process *sp, double now) {
    cw_writer w;
    cw_writer_init_fixed(&w, o->scratch, sizeof(o->scratch));
    directory_drain_write(&w, sp->name, &o->table);
    send_plain(o, MSG_DIR_DRAIN, o->scratch, w.size, now);
    sp->draining = true;
    ++o->drained;
    if (o->config.verbose) fprintf(stderr, "orchestrator: draining %s\n", sp->name);
}

static void stop_shard(orchestrator *o, shard_process *sp) {
    process_terminate(&sp->proc);
    ++o->stopped;
    if (o->config.verbose) fprintf(stderr, "orchestrator: stopped %s\n", sp->name);
    memset(sp, 0, sizeof(*sp));
}

struct table_answer {
    orchestrator *o;
    bool got;
};
static void on_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    table_answer *a = (table_answer *)user;
    if (type != MSG_DIR_TABLE) return;
    net_zone_table table = a->o->table;
    if (directory_table_read(&payload, &table, &a->o->table)) {
        a->o->table = table;
        a->got = true;
    }
}

void orchestrator_update(orchestrator *o, double now) {
    // Shards that died are forgotten (and replaced by the next plan).
    for (int i = 0; i < ORCHESTRATOR_MAX_SHARDS; ++i) {
        shard_process *sp = &o->shards[i];
        if (sp->used && !process_running(&sp->proc)) {
            if (o->config.verbose) fprintf(stderr, "orchestrator: %s has died\n", sp->name);
            process_forget(&sp->proc);
            memset(sp, 0, sizeof(*sp));
            ++o->replaced;
        }
    }
    // The table, asked for once a second.
    if (now - o->last_query >= ORCHESTRATOR_QUERY_EVERY) {
        o->last_query = now;
        directory_query q = {};
        cw_writer w;
        cw_writer_init_fixed(&w, o->scratch, sizeof(o->scratch));
        directory_query_write(&w, &q);
        send_plain(o, MSG_DIR_QUERY, o->scratch, w.size, now);
    }
    for (;;) {
        net_address from;
        int size = net_socket_recv(o->socket, &from, o->scratch, sizeof(o->scratch));
        if (size <= 0) break;
        if (!net_address_equal(from, o->config.directory)) continue;
        table_answer a = {o, false};
        net_connection tmp;
        net_connection_init(&tmp, o->socket, from, 0, on_message, &a);
        net_connection_receive(&tmp, o->scratch, (size_t)size, now);
        net_connection_destroy(&tmp);
        if (a.got) {
            o->have_table = true;
            o->last_table = now;
        }
    }
    if (!o->have_table || now - o->last_table > 3.0 * DIRECTORY_EXPIRY) return;   // no directory: do nothing rash
    plan plans[16];
    int n = orchestrator_plan(&o->table, &o->config, o->shards, ORCHESTRATOR_MAX_SHARDS, now, plans, 16);
    for (int i = 0; i < n; ++i) {
        switch (plans[i].kind) {
        case PLAN_START: start_shard(o, plans[i].place, plans[i].region, now); break;
        case PLAN_DRAIN: drain_shard(o, &o->shards[plans[i].shard], now); break;
        case PLAN_STOP: stop_shard(o, &o->shards[plans[i].shard]); break;
        default: break;
        }
    }
}

void orchestrator_destroy(orchestrator *o) {
    for (int i = 0; i < ORCHESTRATOR_MAX_SHARDS; ++i)
        if (o->shards[i].used) process_terminate(&o->shards[i].proc);
    net_socket_close(o->socket);
    memset(o, 0, sizeof(*o));
}
