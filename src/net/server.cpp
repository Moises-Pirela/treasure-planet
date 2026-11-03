#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAPSHOT_EVERY (GAME_TICK_HZ / NET_SNAPSHOT_HZ)

server_config server_config_default(void) {
    server_config c = {};
    c.port = NET_DEFAULT_PORT;
    c.max_clients = 64;
    for (int i = 0; i < RELEVANCE_CLASS_COUNT; ++i) c.relevance[i].forget = SERVER_FORGET;
    c.timeout = NET_TIMEOUT;
    c.zone = -1;
    c.instance_linger = SERVER_INSTANCE_LINGER;
    c.bandwidth = SERVER_BANDWIDTH;
    c.linger_quit = SERVER_LINGER_QUIT;
    c.linger_timeout = SERVER_LINGER_TIMEOUT;
    c.inbound_packets = SERVER_INBOUND_PACKETS;
    c.inbound_bytes = SERVER_INBOUND_BYTES;
    c.unconnected_per_ip = SERVER_UNCONNECTED_PER_IP;
    return c;
}

double server_time(const server *s) { return net_wall_seconds() - s->config.universe_epoch; }
const net_zone_table *server_zones(const server *s) { return &s->zones; }
int server_self(const server *s) { return net_zones_find(&s->zones, s->zone_name); }
static void social_send(server *s, uint8_t type, const unsigned char *payload, size_t size, double now);
static void handle_party_state(server *s, uint64_t account, const party_view *view);
static void handle_character(server *s, uint64_t account, const character *gear);
static void send_character(server *s, server_client *c);
static void wear_gear(server *s, server_client *c);
static void save_gear(server *s, server_client *c, double now);
static void ids_push(cw_id_list *list, cw_entity_id id);

static const char *world_name(const server_world *world) { return world->kind == WORLD_ZONE ? "the universe" : world->dungeon; }

static void log_client(const server *s, const server_client *c, const char *what) {
    if (!s->config.verbose) return;
    char address[32];
    net_address_format(c->connection.peer, address, sizeof(address));
    const Player *p = cw_world_is_valid(&c->world->g->world, c->player) ? CW_GET(&c->world->g->world, c->player, c->world->g->c_player, Player) : NULL;
    fprintf(stderr, "server: %s (%s) %s %s (player %lld, account %llx, %d online)\n", p ? p->name : "?", address, what,
            world_name(c->world), (long long)c->player, (unsigned long long)c->account, s->client_count);
}

// ---------------------------------------------------------------------------
// Worlds
// ---------------------------------------------------------------------------
// Queues a change to some of an entity's components for those who know the entity.
static void note_updated(server_world *world, cw_entity_id id, uint32_t components) {
    for (size_t i = 0; i < world->updated_count; ++i)
        if (world->updated[i].id == id) {
            world->updated[i].components |= components;
            return;
        }
    if (world->updated_count == world->updated_capacity) {
        world->updated_capacity = world->updated_capacity ? world->updated_capacity * 2 : 16;
        world->updated = (server_change *)realloc(world->updated, world->updated_capacity * sizeof(server_change));
    }
    world->updated[world->updated_count].id = id;
    world->updated[world->updated_count].components = components;
    ++world->updated_count;
}
static void ids_push(cw_id_list *list, cw_entity_id id) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->data = (cw_entity_id *)realloc(list->data, list->capacity * sizeof(cw_entity_id));
    }
    list->data[list->count++] = id;
}

// Lifecycle hooks on a game's sim: the world's host pointer is the server_world.
static void system_record_created(cw_world *w, cw_phase, float) {
    server_world *world = (server_world *)w->host;
    cw_id_view created = cw_world_created(w);
    for (size_t i = 0; i < created.count; ++i) ids_push(&world->spawned, created.data[i]);
}
static void system_record_destroyed(cw_world *w, cw_phase, float) {
    server_world *world = (server_world *)w->host;
    cw_id_view pending = cw_world_pending_destroy(w);
    for (size_t i = 0; i < pending.count; ++i) ids_push(&world->despawned, pending.data[i]);
}

// Wraps a game (built by the caller) as a world of this server and hooks it up.
static server_world *world_add(server *s, int kind, game *g) {
    server_world *world = (server_world *)calloc(1, sizeof(server_world));
    world->owner = s;
    world->kind = kind;
    world->g = g;
    world->opened_at = net_seconds();
    g->world.host = world;
    // The build filled the created list; settle it before the hooks see anything.
    game_tick(g);
    g->world.tick = s->tick;
    world->c_trail = cw_comp_block(&g->world, 1);
    cw_comp_register(&g->world, world->c_trail, sizeof(Trail), alignof(Trail), "Trail", NULL);
    cw_comp_set_transient(&g->world, world->c_trail, true);
    cw_system_desc d = {};
    d.name = "net_record_created";
    d.phases = CW_PHASE(CW_ENTITY_CREATED);
    d.fn = system_record_created;
    d.authority = CW_HOST_ONLY;
    cw_sim_register(&g->sim, d);
    d.name = "net_record_destroyed";
    d.phases = CW_PHASE(CW_ENTITY_DESTROYED);
    d.fn = system_record_destroyed;
    cw_sim_register(&g->sim, d);
    if (s->world_count == s->world_capacity) {
        s->world_capacity = s->world_capacity ? s->world_capacity * 2 : 8;
        s->worlds = (server_world **)realloc(s->worlds, (size_t)s->world_capacity * sizeof(server_world *));
    }
    s->worlds[s->world_count++] = world;
    return world;
}

server_world *server_find_instance(server *s, const char *dungeon, uint64_t key) {
    for (int i = 0; i < s->world_count; ++i) {
        server_world *world = s->worlds[i];
        if (world->kind == WORLD_INSTANCE && world->key == key && strcmp(world->dungeon, dungeon) == 0) return world;
    }
    return NULL;
}

static server_world *open_instance(server *s, const net_ticket *t) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    g->universe_base = s->tick_base;
    game_build_dungeon(g, t->dungeon, t->return_place, t->return_position, (uint32_t)t->key ^ (uint32_t)(server_time(s) * 10.0));
    server_world *world = world_add(s, WORLD_INSTANCE, g);
    strncpy(world->dungeon, t->dungeon, GAME_NAME_CAPACITY - 1);
    world->key = t->key;
    ++s->instances_opened;
    if (s->config.verbose)
        fprintf(stderr, "server: opened %s for %llx (%d worlds)\n", t->dungeon, (unsigned long long)t->key, s->world_count);
    return world;
}

static void close_world(server *s, server_world *world) {
    for (int i = 0; i < s->world_count; ++i)
        if (s->worlds[i] == world) {
            memmove(s->worlds + i, s->worlds + i + 1, (size_t)(s->world_count - i - 1) * sizeof(server_world *));
            --s->world_count;
            break;
        }
    if (world->kind == WORLD_INSTANCE) {
        ++s->instances_closed;
        if (s->config.verbose)
            fprintf(stderr, "server: closed %s for %llx (%d worlds)\n", world->dungeon, (unsigned long long)world->key,
                    s->world_count);
    }
    game_destroy(world->g);
    if (world->g != &s->zone) free(world->g);
    free(world->spawned.data);
    free(world->despawned.data);
    free(world->updated);
    free(world);
}

// ---------------------------------------------------------------------------
// Input queues
// ---------------------------------------------------------------------------
static void input_queue_receive(input_queue *q, const net_input_message *m) {
    if (!q->started) {
        q->started = true;
        q->last_applied = m->seq - m->count;   // everything in this message is new
        q->newest = q->last_applied;
    }
    for (uint32_t i = 0; i < m->count; ++i) {
        uint32_t seq = m->seq - (m->count - 1 - i);
        if ((int32_t)(seq - q->last_applied) <= 0) continue;              // already applied
        if (seq - q->last_applied > SERVER_INPUT_QUEUE) continue;         // too far ahead to hold
        q->inputs[seq % SERVER_INPUT_QUEUE] = m->inputs[i];
        q->view_ticks[seq % SERVER_INPUT_QUEUE] = m->view_ticks[i];
        q->present[seq % SERVER_INPUT_QUEUE] = true;
        if ((int32_t)(seq - q->newest) > 0) q->newest = seq;
    }
}

// The input for this tick: the next in sequence, or the last one again (without its
// jump) when the client's has not arrived. Consuming starts only once
// SERVER_INPUT_TARGET inputs are waiting, so jitter of that many ticks costs nothing;
// after a starvation the buffer is refilled the same way. A client far ahead is
// caught up with by dropping what it sent for ticks that are already behind us.
static PlayerInput input_queue_apply(input_queue *q) {
    if (!q->started) return q->last;
    if (q->newest - q->last_applied > SERVER_INPUT_MAX_AHEAD) {
        while (q->newest - q->last_applied > SERVER_INPUT_TARGET) {
            q->present[(q->last_applied + 1) % SERVER_INPUT_QUEUE] = false;
            ++q->last_applied;
            ++q->skipped;
        }
    }
    if (!q->primed) {
        if (q->newest - q->last_applied < SERVER_INPUT_TARGET) return q->last;
        q->primed = true;
    }
    uint32_t next = q->last_applied + 1;
    if (q->present[next % SERVER_INPUT_QUEUE]) {
        q->present[next % SERVER_INPUT_QUEUE] = false;
        q->last = q->inputs[next % SERVER_INPUT_QUEUE];
        q->last_view_tick = q->view_ticks[next % SERVER_INPUT_QUEUE];
        q->last_applied = next;
        return q->last;
    }
    ++q->starved;
    q->primed = false;
    PlayerInput repeat = q->last;
    repeat.jump = false;
    repeat.act = false;
    return repeat;
}

// ---------------------------------------------------------------------------
// Lag compensation
// ---------------------------------------------------------------------------
static void record_trails(server_world *world) {
    game *g = world->g;
    cw_world *w = &g->world;
    uint32_t tick = (uint32_t)w->tick;
    cw_query players = cw_query_begin(g->q_all_players);
    cw_entity_id id;
    while (cw_query_next(&players, &id)) {
        if (!cw_comp_has(w, id, world->c_trail)) {
            Trail fresh = {};
            cw_comp_set(w, id, world->c_trail, &fresh, sizeof(fresh));
        }
        Trail *trail = CW_GET(w, id, world->c_trail, Trail);
        const Transform *t = CW_GET(w, id, g->c_transform, Transform);
        cw_entity_id aboard = game_ship_at(g, t->position);
        int slot = (int)(tick % SERVER_TRAIL_TICKS);
        trail->positions[slot] = aboard != CW_INVALID_ENTITY
            ? v3_sub(t->position, CW_GET(w, aboard, g->c_transform, Transform)->position) : t->position;
        trail->aboard[slot] = aboard;
        trail->ticks[slot] = tick;
    }
}

// Where a player appeared to someone rendering them at `view_tick`: between the two
// recorded ticks around it, placed against the ship (where it is now, as the viewer
// drew it). Falls back to where they are when the trail does not reach.
static v3 trail_position(server_world *world, cw_entity_id id, double view_tick) {
    game *g = world->g;
    cw_world *w = &g->world;
    const Transform *t = CW_GET(w, id, g->c_transform, Transform);
    if (!cw_comp_has(w, id, world->c_trail)) return t->position;
    const Trail *trail = CW_GET(w, id, world->c_trail, Trail);
    uint32_t before = (uint32_t)floor(view_tick), after = before + 1;
    const int a = (int)(before % SERVER_TRAIL_TICKS), b = (int)(after % SERVER_TRAIL_TICKS);
    if (trail->ticks[a] != before || trail->ticks[b] != after) return t->position;
    float f = (float)(view_tick - (double)before);
    v3 pa = trail->positions[a], pb = trail->positions[b];
    if (trail->aboard[a] != CW_INVALID_ENTITY && cw_comp_has(w, trail->aboard[a], g->c_transform))
        pa = v3_add(pa, CW_GET(w, trail->aboard[a], g->c_transform, Transform)->position);
    if (trail->aboard[b] != CW_INVALID_ENTITY && cw_comp_has(w, trail->aboard[b], g->c_transform))
        pb = v3_add(pb, CW_GET(w, trail->aboard[b], g->c_transform, Transform)->position);
    return v3_lerp(pa, pb, f);
}

// A swing lands on the nearest other player it reaches, each judged where the
// swinger saw them.
static void resolve_swings(server *s) {
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_ACTIVE || !c->swinging) continue;
        c->swinging = false;
        server_world *world = c->world;
        game *g = world->g;
        cw_world *w = &g->world;
        Player *attacker = CW_GET(w, c->player, g->c_player, Player);
        if (attacker->swing_cooldown > 0.0f) continue;
        attacker->swing_cooldown = PLAYER_SWING_COOLDOWN;
        ++s->swings;
        v3 eye = v3_add(CW_GET(w, c->player, g->c_transform, Transform)->position, v3_make(0, PLAYER_EYE, 0));
        v3 direction = look_direction(attacker->yaw, attacker->pitch);
        cw_entity_id best = CW_INVALID_ENTITY;
        float best_distance = 1e30f;
        cw_query players = cw_query_begin(g->q_all_players);
        cw_entity_id id;
        while (cw_query_next(&players, &id)) {
            if (id == c->player) continue;
            v3 seen = trail_position(world, id, (double)c->swing_view_tick);
            if (!game_swing_hits(eye, direction, seen)) continue;
            float d = v3_length(v3_sub(seen, eye));
            if (d < best_distance) {
                best_distance = d;
                best = id;
            }
        }
        if (best == CW_INVALID_ENTITY) continue;
        ++CW_GET(w, best, g->c_player, Player)->hits_taken;
        ++attacker->hits_made;
        ++s->hits;
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
// Sends entity records for `ids` in as many MSG_SPAWN messages as they need, at
// the given priority (the baseline is bulk: low; what comes into view is normal).
// A record too large for a packet goes as a message of its own, fragmented.
static void set_known(server_client *c, cw_entity_id id, bool value);
static void send_spawns_priority(server *s, server_client *c, const cw_entity_id *ids, size_t count, int priority) {
    game *g = c->world->g;
    cw_writer w;
    size_t i = 0;
    while (i < count) {
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        cw_writer_u16(&w, 0);
        uint16_t in_message = 0;
        while (i < count) {
            if (!cw_world_is_valid(&g->world, ids[i])) { ++i; continue; }
            set_known(c, ids[i], true);
            size_t size = net_entity_size(g, ids[i]);
            if (w.size + size > NET_MAX_PAYLOAD) {
                if (in_message == 0) {
                    // Alone, and larger than a packet: the transport splits it.
                    if (2 + size > NET_MAX_MESSAGE) {
                        fprintf(stderr, "server: entity %lld does not fit a message, skipped\n", (long long)ids[i]);
                    } else {
                        cw_writer big;
                        cw_writer_init_fixed(&big, s->big, NET_MAX_MESSAGE);
                        cw_writer_u16(&big, 1);
                        net_entity_write(&big, g, ids[i]);
                        net_connection_send_priority(&c->connection, MSG_SPAWN, NET_RELIABLE_ORDERED, priority, s->big, big.size);
                    }
                    ++i;
                }
                break;
            }
            net_entity_write(&w, g, ids[i]);
            ++in_message;
            ++i;
        }
        if (in_message == 0) continue;
        s->scratch[0] = (unsigned char)(in_message & 0xff);
        s->scratch[1] = (unsigned char)(in_message >> 8);
        net_connection_send_priority(&c->connection, MSG_SPAWN, NET_RELIABLE_ORDERED, priority, s->scratch, w.size);
    }
}
static void send_spawns(server *s, server_client *c, const cw_entity_id *ids, size_t count) {
    send_spawns_priority(s, c, ids, count, NET_PRIORITY_NORMAL);
}

static void send_despawns(server *s, server_client *c, const cw_entity_id *ids, size_t count) {
    const size_t per_message = (NET_MAX_PAYLOAD - 2) / 8;
    for (size_t i = 0; i < count; i += per_message) {
        size_t n = count - i < per_message ? count - i : per_message;
        cw_writer w;
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        cw_writer_u16(&w, (uint16_t)n);
        for (size_t k = 0; k < n; ++k) cw_writer_i64(&w, ids[i + k]);
        net_connection_send(&c->connection, MSG_DESPAWN, NET_RELIABLE_ORDERED, s->scratch, w.size);
    }
}

// Partial records (the components in each update's mask) for the entities the
// client knows of, in as many MSG_UPDATE messages as they need.
static bool knows(const server_client *c, cw_entity_id id);
static void send_updates(server *s, server_client *c, const server_change *updates, size_t count) {
    game *g = c->world->g;
    cw_writer w;
    size_t i = 0;
    while (i < count) {
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        cw_writer_u16(&w, 0);
        uint16_t in_message = 0;
        while (i < count) {
            const server_change *u = &updates[i];
            if (!cw_world_is_valid(&g->world, u->id) || !knows(c, u->id)) { ++i; continue; }
            size_t size = net_entity_size_masked(g, u->id, u->components);
            if (w.size + size > NET_MAX_PAYLOAD) {
                if (in_message == 0) {
                    fprintf(stderr, "server: an update of entity %lld does not fit a packet, skipped\n", (long long)u->id);
                    ++i;
                }
                break;
            }
            net_entity_write_masked(&w, g, u->id, u->components);
            ++in_message;
            ++i;
        }
        if (in_message == 0) continue;
        s->scratch[0] = (unsigned char)(in_message & 0xff);
        s->scratch[1] = (unsigned char)(in_message >> 8);
        net_connection_send(&c->connection, MSG_UPDATE, NET_RELIABLE_ORDERED, s->scratch, w.size);
        c->updates_sent += in_message;
    }
}

// The class an entity's relevance is judged by (server_relevance_class).
static int relevance_class(const game *g, cw_entity_id id) {
    if (cw_comp_has(&g->world, id, g->c_player)) return RELEVANCE_PLAYER;
    if (cw_comp_has(&g->world, id, g->c_loot)) return RELEVANCE_LOOT;
    return RELEVANCE_SCENERY;
}
static float distance2_between(game *g, cw_entity_id a, cw_entity_id b) {
    v3 d = v3_sub(CW_GET(&g->world, a, g->c_transform, Transform)->position,
                  CW_GET(&g->world, b, g->c_transform, Transform)->position);
    return v3_dot(d, d);
}
// Whether an entity is within its class's radius of the client's player.
static bool relevant(const server *s, const server_client *c, cw_entity_id id) {
    game *g = c->world->g;
    const server_relevance *r = &s->config.relevance[relevance_class(g, id)];
    return r->radius <= 0.0f || id == c->player || distance2_between(g, id, c->player) <= r->radius * r->radius;
}

// The world for a client that just joined it: the scenery and the ships, the loot
// within reach, and its own player. Other players follow as they come into its
// interest, and so does the rest of the loot.
static void note_relevant(server_client *c, cw_entity_id id, uint32_t tick);
static void send_baseline(server *s, server_client *c) {
    game *g = c->world->g;
    cw_world *w = &g->world;
    size_t count = cw_world_entity_count(w);
    cw_entity_id *ids = (cw_entity_id *)malloc((count ? count : 1) * sizeof(cw_entity_id));
    size_t n = cw_query_collect_filter(w, cw_filter_make(cw_signature{}, cw_signature{}, cw_mask(&g->c_player, 1)), ids, count);
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!relevant(s, c, ids[i])) continue;
        if (relevance_class(g, ids[i]) == RELEVANCE_LOOT) note_relevant(c, ids[i], (uint32_t)w->tick);
        ids[kept++] = ids[i];
    }
    send_spawns_priority(s, c, ids, kept, NET_PRIORITY_LOW);   // bulk: anything that comes up meanwhile goes first
    send_spawns(s, c, &c->player, 1);
    free(ids);
    net_connection_send_priority(&c->connection, MSG_BASELINE_END, NET_RELIABLE_ORDERED, NET_PRIORITY_LOW, NULL, 0);
}

// The tick an entity was last relevant to this client (0: never).
static uint32_t last_relevant_of(server_client *c, cw_entity_id id) {
    return (size_t)id < c->last_relevant_capacity ? c->last_relevant[id] : 0;
}
static void note_relevant(server_client *c, cw_entity_id id, uint32_t tick) {
    if ((size_t)id >= c->last_relevant_capacity) {
        size_t capacity = c->last_relevant_capacity ? c->last_relevant_capacity : 256;
        while (capacity <= (size_t)id) capacity *= 2;
        c->last_relevant = (uint32_t *)realloc(c->last_relevant, capacity * sizeof(uint32_t));
        memset(c->last_relevant + c->last_relevant_capacity, 0, (capacity - c->last_relevant_capacity) * sizeof(uint32_t));
        c->last_relevant_capacity = capacity;
    }
    c->last_relevant[id] = tick;
}

// Whether the client has been sent an entity's record; players are sent theirs the
// first time they matter to it (spawn on interest), scenery with the baseline.
static bool knows(const server_client *c, cw_entity_id id) {
    size_t word = (size_t)id / 64;
    return word < c->known_words && (c->known[word] >> ((size_t)id % 64)) & 1;
}
static void set_known(server_client *c, cw_entity_id id, bool value) {
    size_t word = (size_t)id / 64;
    if (word >= c->known_words) {
        if (!value) return;
        size_t words = c->known_words ? c->known_words : 8;
        while (words <= word) words *= 2;
        c->known = (uint64_t *)realloc(c->known, words * sizeof(uint64_t));
        memset(c->known + c->known_words, 0, (words - c->known_words) * sizeof(uint64_t));
        c->known_words = words;
    }
    if (value) c->known[word] |= (uint64_t)1 << ((size_t)id % 64);
    else c->known[word] &= ~((uint64_t)1 << ((size_t)id % 64));
}

// Reorders candidates so the `k` best (smallest) priorities come first; O(n).
typedef struct { cw_entity_id id; float priority; } candidate;
static void select_candidates(candidate *e, int n, int k) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = e[(lo + hi) / 2].priority;
        int i = lo, j = hi;
        while (i <= j) {
            while (e[i].priority < pivot) ++i;
            while (e[j].priority > pivot) --j;
            if (i <= j) {
                candidate t = e[i];
                e[i] = e[j];
                e[j] = t;
                ++i;
                --j;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
}
static void select_best(snapshot_entry *e, int n, int k) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = e[(lo + hi) / 2].priority;
        int i = lo, j = hi;
        while (i <= j) {
            while (e[i].priority < pivot) ++i;
            while (e[j].priority > pivot) --j;
            if (i <= j) {
                snapshot_entry t = e[i];
                e[i] = e[j];
                e[j] = t;
                ++i;
                --j;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
}

// The `wanted` players in the client's world it should hear about first: its own,
// then by distance, discounted by how long since the client last heard of them, so
// that the far ones still come round every few seconds. Only those are built.
static void build_snapshot(server *s, server_client *c, int wanted) {
    game *g = c->world->g;
    cw_world *w = &g->world;
    server_world *world = c->world;
    snapshot *snap = &s->scratch_snapshot;
    snap->tick = (uint32_t)w->tick;
    snap->last_input_seq = c->inputs.last_applied;
    int lead = c->inputs.started ? (int)(c->inputs.newest - c->inputs.last_applied) : 0;
    snap->input_lead = (int8_t)(lead > 127 ? 127 : lead < -127 ? -127 : lead);
    snap->count = 0;
    v3 own = CW_GET(w, c->player, g->c_transform, Transform)->position;
    int slot = (int)(snap->tick % SERVER_TRAIL_TICKS);

    // Choosing is the one pass over every player in the world, so a choice is kept
    // for a few snapshots: the same people, at their newer positions.
    bool reselect = c->selection_count == 0 || c->selection_count > wanted
        || snap->tick - c->selection_tick >= (uint32_t)(SERVER_RESELECT_EVERY * s->snapshot_every);
    if (reselect) {
        size_t players = cw_query_count(g->q_all_players);
        if (players > s->candidate_capacity) {
            s->candidate_capacity = players * 2;
            s->candidates = realloc(s->candidates, s->candidate_capacity * sizeof(candidate));
        }
        candidate *candidates = (candidate *)s->candidates;
        int n = 0;
        // The same pass over the world is the relevance pass: players and loot that
        // have been beyond their radius (and a margin) for long enough are dropped
        // from the client, which is told; loot that came within reach is sent.
        const server_relevance *relevance = s->config.relevance;
        float radius2 = relevance[RELEVANCE_PLAYER].radius * relevance[RELEVANCE_PLAYER].radius;
        float margin2 = SERVER_FORGET_MARGIN * SERVER_FORGET_MARGIN;
        uint32_t forget_ticks = (uint32_t)(relevance[RELEVANCE_PLAYER].forget * GAME_TICK_HZ);
        cw_entity_id forget[64], arrive[64];
        size_t forgets = 0, arrivals = 0;
        cw_query query = cw_query_begin(g->q_all_players);
        cw_entity_id id;
        while (cw_query_next(&query, &id)) {
            v3 d = v3_sub(CW_GET(w, id, g->c_transform, Transform)->position, own);
            float distance2 = v3_dot(d, d);
            if (id != c->player && radius2 > 0.0f && distance2 > radius2) {
                if (distance2 > radius2 * margin2 && knows(c, id) && snap->tick - last_relevant_of(c, id) > forget_ticks) {
                    set_known(c, id, false);
                    ++c->forgotten;
                    forget[forgets++] = id;
                    if (forgets == 64) { send_despawns(s, c, forget, forgets); forgets = 0; }
                }
                continue;
            }
            uint32_t last = last_relevant_of(c, id);
            float age = last ? (float)(snap->tick - last) / (float)SNAPSHOT_EVERY : 60.0f;   // in snapshots missed
            candidates[n].id = id;
            candidates[n].priority = id == c->player ? -1.0f : distance2 / ((1.0f + age) * (1.0f + age));   // the long unseen come round
            ++n;
        }
        if (relevance[RELEVANCE_LOOT].radius > 0.0f) {
            radius2 = relevance[RELEVANCE_LOOT].radius * relevance[RELEVANCE_LOOT].radius;
            forget_ticks = (uint32_t)(relevance[RELEVANCE_LOOT].forget * GAME_TICK_HZ);
            query = cw_query_begin(g->q_loot);
            while (cw_query_next(&query, &id)) {
                v3 d = v3_sub(CW_GET(w, id, g->c_transform, Transform)->position, own);
                float distance2 = v3_dot(d, d);
                if (distance2 <= radius2) {
                    note_relevant(c, id, snap->tick);
                    if (knows(c, id)) continue;
                    arrive[arrivals++] = id;
                    if (arrivals == 64) { send_spawns(s, c, arrive, arrivals); arrivals = 0; }
                } else if (distance2 > radius2 * margin2 && knows(c, id) && snap->tick - last_relevant_of(c, id) > forget_ticks) {
                    set_known(c, id, false);
                    ++c->forgotten;
                    forget[forgets++] = id;
                    if (forgets == 64) { send_despawns(s, c, forget, forgets); forgets = 0; }
                }
            }
        }
        if (forgets) send_despawns(s, c, forget, forgets);
        if (arrivals) send_spawns(s, c, arrive, arrivals);
        if (wanted > n) wanted = n;
        if (wanted > snap->capacity) wanted = snap->capacity;
        select_candidates(candidates, n, wanted);
        if (wanted > c->selection_capacity) {
            c->selection_capacity = wanted * 2;
            c->selection = (cw_entity_id *)realloc(c->selection, (size_t)c->selection_capacity * sizeof(cw_entity_id));
        }
        for (int i = 0; i < wanted; ++i) c->selection[i] = candidates[i].id;
        c->selection_count = wanted;
        c->selection_tick = snap->tick;
    }
    for (int i = 0; i < c->selection_count; ++i) {
        cw_entity_id id = c->selection[i];
        if (!cw_world_is_valid(w, id) || !cw_comp_has(w, id, g->c_player)) continue;   // gone since chosen
        const Transform *t = CW_GET(w, id, g->c_transform, Transform);
        const Player *p = CW_GET(w, id, g->c_player, Player);
        // The trail recorded this tick already knows the ship, and the relative position.
        const Trail *trail = cw_comp_has(w, id, world->c_trail) ? CW_GET(w, id, world->c_trail, Trail) : NULL;
        cw_entity_id aboard = trail && trail->ticks[slot] == snap->tick ? trail->aboard[slot] : game_ship_at(g, t->position);
        v3 position = t->position;
        if (aboard != CW_INVALID_ENTITY)
            position = v3_sub(position, CW_GET(w, aboard, g->c_transform, Transform)->position);
        snapshot_entry e = snapshot_entry_make(id, aboard, position, p->velocity, p->yaw, p->pitch, p->grounded,
                                               p->hits_made, p->hits_taken);
        e.priority = id == c->player ? -1.0f : (float)i;   // the order they were chosen in
        snap->entries[snap->count++] = e;
    }
}

static void send_snapshot(server *s, server_client *c) {
    // A snapshot's share of the client's bandwidth (what the connection is sending
    // at now: the cap, or less under congestion), less the packet and message
    // headers; reliable traffic in flight (a baseline, spawns) takes from it too.
    size_t budget = net_connection_rate(&c->connection) / NET_SNAPSHOT_HZ;
    if (net_connection_unacked(&c->connection)) budget /= 2;
    if (budget > NET_PACKET_OVERHEAD + NET_MESSAGE_HEADER) budget -= NET_PACKET_OVERHEAD + NET_MESSAGE_HEADER;
    if (budget < SERVER_SNAPSHOT_MIN) budget = SERVER_SNAPSHOT_MIN;
    if (budget > NET_MAX_PAYLOAD) budget = NET_MAX_PAYLOAD;
    // How many might fit: a moving player delta-codes to about ten bytes. That many
    // are built, best first, and sorted by id for coding; if they still do not fit,
    // the worst of them go, until it does.
    int count = (int)((budget - 14) / 10);
    if (count < 1) count = 1;
    build_snapshot(s, c, count);
    snapshot *snap = &s->scratch_snapshot;
    const snapshot *base = snapshot_ring_find(&c->sent, c->ack_tick);
    count = snap->count;
    for (;;) {
        snap->count = (uint16_t)count;
        snapshot_sort(snap);
        cw_writer w;
        cw_writer_init_fixed(&w, s->scratch, budget);
        if (snapshot_encode(snap, base, &w)) {
            net_connection_send(&c->connection, MSG_SNAPSHOT, NET_SEQUENCED, s->scratch, w.size);
            break;
        }
        if (count <= 1) return;   // cannot even fit ourselves: nothing to send
        // The id order scrambled the priority order: pick the best of what is left again.
        select_best(snap->entries, count, count - (count / 4 > 0 ? count / 4 : 1));
        count -= count / 4 > 0 ? count / 4 : 1;
    }
    if (snap->count < c->selection_count) {   // the budget took fewer: keep the set that fit
        for (int i = 0; i < snap->count; ++i) c->selection[i] = snap->entries[i].id;
        c->selection_count = snap->count;
    }
    // Players the client has never been told about come with their record first.
    cw_entity_id unknown[64];
    int unknown_count = 0;
    for (int i = 0; i < snap->count; ++i) {
        note_relevant(c, snap->entries[i].id, snap->tick);
        if (!knows(c, snap->entries[i].id) && unknown_count < 64) unknown[unknown_count++] = snap->entries[i].id;
    }
    if (unknown_count) send_spawns(s, c, unknown, (size_t)unknown_count);
    snapshot *kept = snapshot_ring_push(&c->sent, snap->count);
    kept->tick = snap->tick;
    kept->last_input_seq = snap->last_input_seq;
    kept->count = snap->count;
    memcpy(kept->entries, snap->entries, snap->count * sizeof(snapshot_entry));
    ++c->snapshots_sent;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
static void on_client_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    server_client *c = (server_client *)user;
    switch (type) {
    case MSG_INPUT: {
        net_input_message m;
        if (!net_input_read(&payload, &m)) return;
        input_queue_receive(&c->inputs, &m);
        if ((int32_t)(m.ack_tick - c->ack_tick) > 0) c->ack_tick = m.ack_tick;
        break;
    }
    case MSG_DISCONNECT: {
        uint8_t reason = LEAVE_QUIT;
        cw_reader_u8(&payload, &reason);
        c->leave_reason = reason;
        c->state = CLIENT_LEAVING;
        break;
    }
    case MSG_EQUIP: {
        uint8_t index = 0;
        cw_reader_u8(&payload, &index);
        server *s = c->world->owner;
        if (c->gear_known && character_equip(&c->gear, index)) {
            wear_gear(s, c);
            save_gear(s, c, net_seconds());
        }
        break;
    }
    case MSG_PARTY_REQUEST: {
        uint8_t op = 0;
        char target[ACCOUNT_NAME_CAPACITY] = {0};
        uint8_t length = 0;
        cw_reader_u8(&payload, &op);
        cw_reader_u8(&payload, &length);
        if (length >= sizeof(target) || !cw_reader_bytes(&payload, target, length)) return;
        server *s = c->world->owner;
        unsigned char relay[128];
        cw_writer w;
        cw_writer_init_fixed(&w, relay, sizeof(relay));
        social_party_op_write(&w, c->account, op, target, &s->zones);
        social_send(s, MSG_SOC_PARTY_OP, relay, w.size, net_seconds());
        break;
    }
    case MSG_HANDOFF_FAILED:
        if (c->state == CLIENT_HANDING_OFF) {
            c->state = CLIENT_ACTIVE;   // unfrozen; check_handoffs tries again after the retry delay
            c->redirected_at = net_seconds();
            ++c->handoffs_failed;
        }
        break;
    default:
        break;   // MSG_KEEPALIVE, and anything a newer client might say
    }
}

struct handshake {
    server *s;
    net_address from;
    double now;
    bool connect, bad_credential;
    net_connect_message message;
};
static void on_handshake_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    handshake *h = (handshake *)user;
    server *s = h->s;
    switch (type) {
    case MSG_CONNECT:
        h->connect = net_connect_read(&payload, &h->message, server_zones(s), &h->bad_credential);
        break;
    case MSG_DIR_TABLE: {
        // The directory's view of the world replaces ours (its reply to our registration).
        if (!s->config.directory.ip || !net_address_equal(h->from, s->config.directory)) return;
        net_zone_table table = s->zones;
        if (!directory_table_read(&payload, &table, &s->zones)) return;
        if (net_zones_find(&table, s->zone_name) < 0) return;   // not yet including us: keep ours
        s->zones = table;
        ++s->tables_received;
        break;
    }
    case MSG_SOC_PARTY_STATE: {
        if (!s->config.directory.ip || !net_address_equal(h->from, s->config.directory)) return;
        uint64_t account;
        party_view view;
        if (social_party_state_read(&payload, &account, &view, &s->zones)) handle_party_state(s, account, &view);
        break;
    }
    case MSG_SOC_CHARACTER: {
        if (!s->config.directory.ip || !net_address_equal(h->from, s->config.directory)) return;
        uint64_t account;
        character gear;
        if (social_character_read(&payload, &account, &gear, &s->zones)) handle_character(s, account, &gear);
        break;
    }
    case MSG_DIR_PUNCH: {
        // Someone is about to connect from the outside: a few packets their way open
        // our NAT for theirs. Harmless otherwise.
        net_address client;
        if (!net_address_equal(h->from, s->config.directory) || !directory_punch_read(&payload, &client, &s->zones)) return;
        net_connection tmp;
        net_connection_init(&tmp, s->socket, client, 0, NULL, NULL);
        for (int i = 0; i < DIRECTORY_PUNCHES; ++i) {
            net_connection_send(&tmp, MSG_KEEPALIVE, NET_UNRELIABLE, NULL, 0);
            net_connection_flush(&tmp, h->now, false);
        }
        net_connection_destroy(&tmp);
        ++s->punches_sent;
        break;
    }
    default:
        break;
    }
}

// The address a redirect names for this very server: ip 0 means "where you are".
static net_address self_address(void) { return net_address_make(0, 0); }

// ---------------------------------------------------------------------------
// The social service: through the directory, or the one we run ourselves.
// ---------------------------------------------------------------------------
static void handle_party_state(server *s, uint64_t account, const party_view *view) {
    server_client *c = server_client_of_account(s, account);
    if (!c || c->state == CLIENT_FREE || c->state == CLIENT_LINGERING) return;
    c->party = view->party;
    c->fireteam = *view;
    cw_writer w;
    cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
    party_view_write(&w, view);
    net_connection_send(&c->connection, MSG_PARTY, NET_RELIABLE_ORDERED, s->scratch, w.size);
    ++s->party_states_relayed;
}

// The player's gear as the backend has it: worn on the entity (for everyone), sent
// to the client (for its inventory), and from now on theirs to change here.
static void send_character(server *s, server_client *c) {
    cw_writer w;
    cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
    character_write(&w, &c->gear);
    net_connection_send(&c->connection, MSG_CHARACTER, NET_RELIABLE, s->scratch, w.size);
}
static void wear_gear(server *s, server_client *c) {
    game *g = c->world->g;
    Equipment *e = CW_GET(&g->world, c->player, g->c_equipment, Equipment);
    memcpy(e->items, c->gear.equipment, sizeof(e->items));
    note_updated(c->world, c->player, 1u << NET_C_EQUIPMENT);
    send_character(s, c);
}
static void handle_character(server *s, uint64_t account, const character *gear) {
    server_client *c = server_client_of_account(s, account);
    if (!c || c->state == CLIENT_FREE || c->state == CLIENT_LINGERING || c->gear_known) return;
    c->gear = *gear;
    c->gear_known = true;
    wear_gear(s, c);
}
// After a change here, the backend's copy.
static void save_gear(server *s, server_client *c, double now) {
    unsigned char payload[NET_MAX_PAYLOAD];
    cw_writer w;
    cw_writer_init_fixed(&w, payload, sizeof(payload));
    social_character_write(&w, c->account, &c->gear, &s->zones);
    social_send(s, MSG_SOC_CHARACTER_SET, payload, w.size, now);
}

// What the embedded service sends "to a server": that is us.
static void local_social_deliver(void *user, net_address, uint8_t type, const void *payload, size_t size) {
    server *s = (server *)user;
    cw_reader r = cw_reader_make(payload, size);
    uint64_t account;
    if (type == MSG_SOC_PARTY_STATE) {
        party_view view;
        if (social_party_state_read(&r, &account, &view, &s->zones)) handle_party_state(s, account, &view);
    } else if (type == MSG_SOC_CHARACTER) {
        character gear;
        if (social_character_read(&r, &account, &gear, &s->zones)) handle_character(s, account, &gear);
    }
}

static void social_send(server *s, uint8_t type, const unsigned char *payload, size_t size, double now) {
    if (s->local_social) {
        social_handle(s->local_social, self_address(), type, cw_reader_make(payload, size), now);
        return;
    }
    net_connection tmp;
    net_connection_init(&tmp, s->socket, s->config.directory, 0, NULL, NULL);
    net_connection_send(&tmp, type, NET_UNRELIABLE, payload, size);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

static void report_presence(server *s, server_client *c, int kind, double now) {
    game *g = c->world->g;
    const Player *p = CW_GET(&g->world, c->player, g->c_player, Player);
    unsigned char payload[300];
    cw_writer w;
    cw_writer_init_fixed(&w, payload, sizeof(payload));
    social_presence_write(&w, c->connection.session, c->account, p->name, c->platform, s->zone_name,
                          c->world->kind == WORLD_INSTANCE ? c->world->dungeon : "", kind, &s->zones);
    social_send(s, MSG_SOC_PRESENCE, payload, w.size, now);
    c->last_presence = now;
}

// Tells the directory what we are, every DIRECTORY_REGISTER_EVERY seconds.
static void register_with_directory(server *s, double now) {
    if (!s->config.directory.ip || now - s->last_registered < DIRECTORY_REGISTER_EVERY) return;
    s->last_registered = now;
    int self = server_self(s);
    if (self < 0) return;
    net_zone me = s->zones.zones[self];
    me.address = s->config.public_address;   // ip 0: the directory publishes what it sees
    me.clients = (uint16_t)s->client_count;
    me.capacity = (uint16_t)s->config.max_clients;
    if (me.address.ip && me.address.port == 0) me.address.port = net_socket_port(s->socket);
    cw_writer w;
    cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
    directory_register_write(&w, &me, (uint16_t)s->client_count, (uint16_t)s->config.max_clients, &s->zones);
    net_connection tmp;
    net_connection_init(&tmp, s->socket, s->config.directory, 0, NULL, NULL);
    net_connection_send(&tmp, MSG_DIR_REGISTER, NET_UNRELIABLE, s->scratch, w.size);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

// Clients by address: an open-addressed hash with linear probing, twice the size
// of the client table so it never fills; freed slots are tombstoned (-2).
static size_t address_slot(const server *s, net_address a) {
    uint64_t h = ((uint64_t)a.ip << 16 | a.port) * 0x9e3779b97f4a7c15ull;
    return (size_t)(h >> 32) % s->hash_capacity;
}
static void hash_insert(server *s, server_client *c) {
    size_t i = address_slot(s, c->connection.peer);
    while (s->client_by_address[i] >= 0) i = (i + 1) % s->hash_capacity;
    s->client_by_address[i] = (int)(c - s->clients);
}
static void hash_remove(server *s, const server_client *c) {
    size_t i = address_slot(s, c->connection.peer);
    for (;;) {
        int at = s->client_by_address[i];
        if (at == -1) return;
        if (at == (int)(c - s->clients)) {
            s->client_by_address[i] = -2;
            return;
        }
        i = (i + 1) % s->hash_capacity;
    }
}
static server_client *find_client(server *s, net_address from) {
    size_t i = address_slot(s, from);
    for (size_t probes = 0; probes < s->hash_capacity; ++probes) {
        int at = s->client_by_address[i];
        if (at == -1) return NULL;
        if (at >= 0 && s->clients[at].state != CLIENT_FREE && net_address_equal(s->clients[at].connection.peer, from))
            return &s->clients[at];
        i = (i + 1) % s->hash_capacity;
    }
    return NULL;
}

static void reject(server *s, net_address to, uint8_t reason, double now) {
    net_connection tmp;
    net_connection_init(&tmp, s->socket, to, 0, NULL, NULL);
    net_connection_send(&tmp, MSG_REJECT, NET_UNRELIABLE, &reason, 1);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

// The handshake's answer, in the clear: the session and our public key. Repeated
// whenever the client's MSG_CONNECT comes again, since the first may have been lost.
static void send_keys(server *s, server_client *c, double now) {
    net_keys_message m;
    m.session = c->connection.session;
    memcpy(m.server_public, c->server_public, CRYPTO_KEY_SIZE);
    memcpy(m.client_public, c->client_public, CRYPTO_KEY_SIZE);
    cw_writer w;
    cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
    net_keys_write(&w, &m);
    net_connection tmp;
    net_connection_init(&tmp, s->socket, c->connection.peer, 0, NULL, NULL);
    net_connection_send(&tmp, MSG_KEYS, NET_UNRELIABLE, s->scratch, w.size);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

v3 server_spawn(const server *s) {
    int self = server_self(s);
    if (self >= 0) {
        const net_zone *zone = &s->zones.zones[self];
        for (int i = 0; i < zone->place_count; ++i) {
            int place = game_find_place(&s->zone, zone->places[i]);
            if (place >= 0) return s->zone.places[place].spawn;
        }
    }
    return s->zone.spawn;
}

static void detach_client(server *s, server_client *c, int why, double now);
static void close_connection(server *s, server_client *c);

// The challenge for a connect from `from`: whoever can read at that address can
// repeat it, nobody else can make it. Keyed by the time slot so that it expires.
static uint64_t challenge_for(const server *s, net_address from, const net_connect_message *m, uint64_t slot) {
    unsigned char body[4 + 2 + CRYPTO_KEY_SIZE + 4 + 8];
    cw_writer w;
    cw_writer_init_fixed(&w, body, sizeof(body));
    cw_writer_u32(&w, from.ip);
    cw_writer_u16(&w, from.port);
    cw_writer_bytes(&w, m->public_key, CRYPTO_KEY_SIZE);
    cw_writer_u32(&w, m->nonce);
    cw_writer_u64(&w, slot);
    uint64_t h = net_siphash(body, w.size, s->challenge_key[0], s->challenge_key[1]);
    return h ? h : 1;   // 0 means "none" on the wire
}
static bool challenge_valid(const server *s, net_address from, const net_connect_message *m) {
    if (!m->challenge) return false;
    uint64_t slot = (uint64_t)(net_seconds() / SERVER_CHALLENGE_ROTATION);
    return m->challenge == challenge_for(s, from, m, slot) || (slot && m->challenge == challenge_for(s, from, m, slot - 1));
}
static void send_challenge(server *s, net_address to, const net_connect_message *m, double now) {
    unsigned char payload[12];
    cw_writer w;
    cw_writer_init_fixed(&w, payload, sizeof(payload));
    cw_writer_u32(&w, m->nonce);
    cw_writer_u64(&w, challenge_for(s, to, m, (uint64_t)(net_seconds() / SERVER_CHALLENGE_ROTATION)));
    net_connection tmp;
    net_connection_init(&tmp, s->socket, to, 0, NULL, NULL);
    net_connection_send(&tmp, MSG_CHALLENGE, NET_UNRELIABLE, payload, w.size);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
    ++s->challenges_sent;
}

// A token is good once, or again from the same address. The memory is a ring.
static bool token_seen(server *s, const net_connect_token *t, net_address from) {
    for (int i = 0; i < SERVER_TOKEN_MEMORY; ++i)
        if (s->tokens[i].nonce == t->nonce) return !net_address_equal(s->tokens[i].from, from);
    s->tokens[s->next_token].nonce = t->nonce;
    s->tokens[s->next_token].from = from;
    s->next_token = (s->next_token + 1) % SERVER_TOKEN_MEMORY;
    return false;
}

// A ticket is good once. The memory is a ring, long enough to outlive a ticket.
static bool ticket_seen(server *s, const net_ticket *t) {
    for (int i = 0; i < SERVER_TICKET_MEMORY; ++i)
        if (s->tickets[i].signature == t->signature && s->tickets[i].issued == t->issued) return true;
    s->tickets[s->next_ticket].signature = t->signature;
    s->tickets[s->next_ticket].issued = t->issued;
    s->next_ticket = (s->next_ticket + 1) % SERVER_TICKET_MEMORY;
    return false;
}

static void accept_client(server *s, net_address from, const net_connect_message *m, double now) {
    // Who is this? The directory's word (a token: it verified them), else the
    // platform's (accounts.h). A token must be for this server and still good; a
    // ticket must be theirs. Where tokens are required, a ticket will do instead:
    // it was issued by a server that admitted them.
    account_identity identity = {};
    if (m->has_ticket) {
        if (ticket_seen(s, &m->ticket)) {
            ++s->credentials_refused;
            reject(s, from, REJECT_TICKET, now);
            return;
        }
        identity.account = m->ticket.account;
        memcpy(identity.name, m->ticket.name, sizeof(identity.name));
        memcpy(identity.platform, m->ticket.platform, sizeof(identity.platform));
        identity.provider = "ticket";
    } else if (m->has_token) {
        if (strcmp(m->token.zone, s->zone_name) != 0 || net_wall_seconds() > m->token.expires || token_seen(s, &m->token, from)) {
            ++s->credentials_refused;
            reject(s, from, REJECT_TICKET, now);
            return;
        }
        identity.account = m->token.account;
        memcpy(identity.name, m->token.name, sizeof(identity.name));
        memcpy(identity.platform, m->token.platform, sizeof(identity.platform));
        identity.provider = "directory";
    } else if (s->config.require_token) {
        ++s->tokens_refused;
        reject(s, from, REJECT_TOKEN, now);
        return;
    } else if (!accounts_resolve(m->platform, (const char *)m->proof, m->proof_size, m->name, &identity)) {
        reject(s, from, REJECT_IDENTITY, now);
        return;
    }
    // Which world, and where in it. A dungeon ticket opens or joins an instance; any
    // other arrival is for the universe, which an instance host does not have.
    bool dungeon = m->has_ticket && m->ticket.dungeon[0];
    if (!dungeon && !s->zone_world) {
        reject(s, from, REJECT_NO_WORLD, now);
        return;
    }
    // The same account already here: a body lingering for them, or a connection of
    // theirs that died without our noticing. Without a ticket they are coming back
    // to it, and it is theirs again where they left it; with one they arrive from
    // elsewhere with newer state, and the old body goes.
    server_client *previous = NULL;
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *o = &s->clients[i];
        if (o->state != CLIENT_FREE && o->state != CLIENT_HANDING_OFF && o->account == identity.account) previous = o;
    }
    server_client *c = NULL;
    bool reattach = previous && !m->has_ticket;
    if (previous && !reattach) {
        detach_client(s, previous, SERVER_LEAVE_REPLACED, now);
        ++s->superseded;
    }
    if (reattach) {
        c = previous;
        if (c->state != CLIENT_LINGERING) {
            log_client(s, c, "reconnected over a dead connection in");
            close_connection(s, c);
            c->state = CLIENT_LINGERING;
        }
        // Back in the slot's sense: the rest of the client is set up afresh below.
        ++s->client_count;
        ++s->reattached;
    } else {
        for (int i = 0; i < s->config.max_clients && !c; ++i)
            if (s->clients[i].state == CLIENT_FREE) c = &s->clients[i];
        if (!c) {
            reject(s, from, REJECT_FULL, now);
            return;
        }
    }
    server_world *world = reattach ? c->world : s->zone_world;
    v3 position = m->has_ticket ? m->ticket.position : server_spawn(s);
    if (dungeon) {
        world = server_find_instance(s, m->ticket.dungeon, m->ticket.key);
        if (!world) world = open_instance(s, &m->ticket);
        position = world->g->spawn;
    }
    // What survives a reattachment: the body, the gear, the party, the world.
    cw_entity_id body = reattach ? c->player : CW_INVALID_ENTITY;
    character gear = c->gear;
    bool gear_known = reattach && c->gear_known;
    uint64_t party = reattach ? c->party : 0;
    party_view fireteam = c->fireteam;
    memset(c, 0, sizeof(*c));
    c->state = CLIENT_ACTIVE;
    uint32_t session = s->next_session++;
    if (session == 0) session = s->next_session++;   // 0 means "no session yet"
    net_connection_init(&c->connection, s->socket, from, session, on_client_message, c);
    net_connection_set_rate(&c->connection, s->config.bandwidth);
    net_connection_set_inbound_limit(&c->connection, s->config.inbound_packets, s->config.inbound_bytes);
    hash_insert(s, c);
    // Our half of the key exchange, rotated every few seconds (see server.h).
    uint8_t shared[CRYPTO_KEY_SIZE], to_server[CRYPTO_KEY_SIZE], to_client[CRYPTO_KEY_SIZE];
    if (now - s->key_made_at > SERVER_KEY_ROTATION) {
        if (!crypto_keypair(s->key_secret, s->key_public)) {
            reject(s, from, REJECT_NO_WORLD, now);   // no randomness: nothing safe to do
            hash_remove(s, c);
            c->state = CLIENT_FREE;
            return;
        }
        s->key_made_at = now;
    }
    memcpy(c->server_public, s->key_public, CRYPTO_KEY_SIZE);
    memcpy(c->client_public, m->public_key, CRYPTO_KEY_SIZE);
    crypto_x25519(shared, s->key_secret, c->client_public);
    crypto_session_keys(shared, c->client_public, c->server_public, to_server, to_client);
    net_connection_set_keys(&c->connection, to_client, to_server);
    memset(shared, 0, sizeof(shared));
    send_keys(s, c, now);
    snapshot_ring_init(&c->sent);
    c->joined_at = now;
    c->world = world;
    game *g = world->g;
    if (reattach) {
        c->player = body;
        c->gear = gear;
        c->gear_known = gear_known;
        c->party = party;
        c->fireteam = fireteam;
    } else {
        ++world->client_count;
        world->empty_since = 0.0;
        c->player = game_spawn_player(g, position, true);
        game_set_owner(g, c->player, identity.account);
        Player *p = CW_GET(&g->world, c->player, g->c_player, Player);
        strncpy(p->name, identity.name, GAME_NAME_CAPACITY - 1);
        if (m->has_ticket) {
            p->yaw = m->ticket.yaw;
            p->pitch = m->ticket.pitch;
            if (!m->ticket.dungeon[0]) p->velocity = m->ticket.velocity;   // a zone crossing carries on mid-stride
        }
        ++s->client_count;
    }
    c->account = identity.account;
    memcpy(c->platform, identity.platform, sizeof(c->platform));
    memcpy(c->region, m->region, NET_REGION_NAME);

    net_welcome welcome = {};
    welcome.session = session;
    welcome.player = c->player;
    welcome.tick = (uint32_t)g->world.tick;
    welcome.tick_hz = GAME_TICK_HZ;
    welcome.snapshot_hz = NET_SNAPSHOT_HZ;
    welcome.universe_base = g->universe_base;
    welcome.spawn = g->spawn;
    welcome.account = c->account;
    welcome.input_target = SERVER_INPUT_TARGET;
    if (world->kind == WORLD_INSTANCE) strncpy(welcome.world, world->dungeon, GAME_NAME_CAPACITY - 1);
    memcpy(welcome.zone, s->zone_name, sizeof(welcome.zone));
    welcome.place_count = g->place_count;
    memcpy(welcome.places, g->places, sizeof(welcome.places));
    cw_writer w;
    cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
    net_welcome_write(&w, &welcome);
    // The first reliable message on the connection. The client has no session yet and
    // adopts the one stamped on the packet that delivers this.
    net_connection_send(&c->connection, MSG_WELCOME, NET_RELIABLE_ORDERED, s->scratch, w.size);
    send_baseline(s, c);
    report_presence(s, c, PRESENCE_ARRIVED, now);
    if (c->gear_known) {
        send_character(s, c);
    } else {
        unsigned char payload[64];
        cw_writer gw;
        cw_writer_init_fixed(&gw, payload, sizeof(payload));
        social_character_get_write(&gw, c->account, &s->zones);
        social_send(s, MSG_SOC_CHARACTER_GET, payload, gw.size, now);
    }
    ++s->accepts_since_trace;
    log_client(s, c, reattach ? "came back to" : m->has_ticket ? "arrived in" : "joined");
}

// Lets go of the client's connection and what served it. The body is not touched.
static void close_connection(server *s, server_client *c) {
    if (c->state != CLIENT_LINGERING) {
        hash_remove(s, c);
        net_connection_destroy(&c->connection);
        --s->client_count;
    }
    snapshot_ring_destroy(&c->sent);
    free(c->last_relevant);
    c->last_relevant = NULL;
    c->last_relevant_capacity = 0;
    free(c->known);
    c->known = NULL;
    c->known_words = 0;
    free(c->selection);
    c->selection = NULL;
    c->selection_count = c->selection_capacity = 0;
    memset(&c->inputs, 0, sizeof(c->inputs));
}

// The body goes and the slot is free; the player is offline.
static void finalize_client(server *s, server_client *c, const char *why) {
    log_client(s, c, why);
    report_presence(s, c, PRESENCE_LEFT, net_seconds());
    if (c->state != CLIENT_LINGERING) {
        net_connection_send(&c->connection, MSG_DISCONNECT, NET_UNRELIABLE, NULL, 0);
        net_connection_flush(&c->connection, net_seconds(), false);
    }
    game_despawn_player(c->world->g, c->player);
    if (--c->world->client_count == 0) c->world->empty_since = net_seconds();
    close_connection(s, c);
    c->state = CLIENT_FREE;
    ++s->finalized;
}

// The client is gone; whether the body stays depends on why. A quit or a lost
// connection leaves it lingering, for the player to come back to; a hand-off or a
// replacement means they are elsewhere already.
static void detach_client(server *s, server_client *c, int why, double now) {
    double linger = why == SERVER_LEAVE_QUIT ? s->config.linger_quit : why == SERVER_LEAVE_TIMEOUT ? s->config.linger_timeout : 0.0;
    if (linger <= 0.0) {
        finalize_client(s, c, why == SERVER_LEAVE_HANDOFF ? "left" : why == SERVER_LEAVE_REPLACED ? "superseded in" : "left for good");
        return;
    }
    log_client(s, c, why == SERVER_LEAVE_TIMEOUT ? "lost; body lingers in" : "quit; body lingers in");
    close_connection(s, c);
    c->state = CLIENT_LINGERING;
    c->linger_until = now + linger;
    // Idle while nobody drives it: physics carries on, the swing arm does not.
    game *g = c->world->g;
    PlayerInput *in = CW_GET(&g->world, c->player, g->c_input, PlayerInput);
    PlayerInput idle = {};
    idle.yaw = in->yaw;
    idle.pitch = in->pitch;
    *in = idle;
    ++s->lingered;
}



static void receive_packets(server *s, double now) {
    for (;;) {
        net_address from;
        int size = net_socket_recv(s->socket, &from, s->scratch, sizeof(s->scratch));
        if (size <= 0) break;
        uint32_t session;
        if (!net_packet_peek(s->scratch, (size_t)size, &session)) continue;
        server_client *c = find_client(s, from);
        if (c) {
            // Session 0 from a known address is a repeated MSG_CONNECT: our keys did
            // not reach them, so they go again.
            if (session != 0) net_connection_receive(&c->connection, s->scratch, (size_t)size, now);
            else send_keys(s, c, now);
            continue;
        }
        if (session != 0) continue;   // a stale session: a client we already dropped
        // Nobody we know: the handshake, or the directory. Each address gets a few
        // of these a second; a flood from one is dropped here, unread. The directory
        // is exempt: it speaks for every player joining, and signs what it says.
        bool from_directory = s->config.directory.ip && net_address_equal(from, s->config.directory);
        if (!from_directory && !net_limiter_allow(&s->unconnected, from, now)) continue;
        handshake h = {};
        h.s = s;
        h.from = from;
        h.now = now;
        net_connection tmp;
        net_connection_init(&tmp, s->socket, from, 0, on_handshake_message, &h);
        net_connection_receive(&tmp, s->scratch, (size_t)size, now);
        net_connection_destroy(&tmp);
        if (!h.connect) continue;
        if (h.message.version != NET_PROTOCOL_VERSION) {
            reject(s, from, REJECT_VERSION, now);
            continue;
        }
        // Before anything is allocated or computed for them: the challenge. A
        // connect without the right one gets a challenge back, and nothing else.
        if (!challenge_valid(s, from, &h.message)) {
            if (h.message.challenge) ++s->challenges_failed;
            send_challenge(s, from, &h.message, now);
            continue;
        }
        ++s->challenges_passed;
        if (now - s->accepts_second_started >= 1.0) {
            s->accepts_second_started = now;
            s->accepts_this_second = 0;
        }
        if (s->accepts_this_second >= SERVER_ACCEPTS_PER_SECOND) continue;   // they will ask again shortly
        ++s->accepts_this_second;
        if (h.bad_credential) {
            ++s->credentials_refused;
            reject(s, from, REJECT_TICKET, now);
        } else if (h.message.has_ticket && fabs(server_time(s) - h.message.ticket.issued) > NET_TICKET_LIFETIME) {
            ++s->credentials_refused;
            reject(s, from, REJECT_TICKET, now);
        } else accept_client(s, from, &h.message, now);
    }
}

// ---------------------------------------------------------------------------
// Hand-offs
// ---------------------------------------------------------------------------
// The shard of a place for this client: the one its party is on when that has
// room, else by region and population (zones.h). A player already on a shard of
// the place stays, so crossing within it never bounces them elsewhere.
static int choose_shard(server *s, server_client *c, const char *place) {
    int self = server_self(s);
    if (self >= 0)
        for (int k = 0; k < s->zones.zones[self].place_count; ++k)
            if (strcmp(s->zones.zones[self].places[k], place) == 0) return self;
    const char *with[SOCIAL_PARTY_SIZE];
    int with_count = 0;
    for (int i = 0; i < c->fireteam.count; ++i) {
        const party_member_view *m = &c->fireteam.members[i];
        if (m->online && m->account != c->account && m->server[0]) with[with_count++] = m->server;
    }
    return net_zones_choose(&s->zones, place, c->region, with, with_count);
}

// The server that should take this player next, and the ticket to bring, for the
// place they stand in; false when the place leads nowhere (or nowhere else).
static bool handoff_for(server *s, server_client *c, const game_place *place, net_address *address, net_ticket *ticket) {
    // Alone (listed nowhere, nobody else listed) every place is ours and instances are ours to host.
    const net_zone_table *zones = s->zones.count > 1 || server_self(s) < 0 ? &s->zones : NULL;
    int self = server_self(s);
    game *g = c->world->g;
    const Transform *t = CW_GET(&g->world, c->player, g->c_transform, Transform);
    const Player *p = CW_GET(&g->world, c->player, g->c_player, Player);
    memset(ticket, 0, sizeof(*ticket));
    ticket->account = c->account;
    memcpy(ticket->name, p->name, sizeof(ticket->name));
    memcpy(ticket->platform, c->platform, sizeof(ticket->platform));
    ticket->position = t->position;
    ticket->velocity = p->velocity;
    ticket->yaw = p->yaw;
    ticket->pitch = p->pitch;
    ticket->issued = server_time(s);
    switch (place->kind) {
    case PLACE_ZONE: {
        if (!zones) return false;
        int owner = choose_shard(s, c, place->name);
        if (owner < 0 || owner == self) return false;
        *address = zones->zones[owner].address;
        return true;
    }
    case PLACE_ENTRANCE: {
        // The instance key: the party's id, or alone, the player's own account.
        strncpy(ticket->dungeon, place->name, NET_ZONE_NAME - 1);
        ticket->key = c->party ? c->party : c->account;   // a party shares an instance
        strncpy(ticket->return_place, place->destination, NET_ZONE_NAME - 1);
        ticket->return_position = place->spawn;
        int host = zones ? net_zones_instance_host(zones, ticket->key) : -1;
        *address = host >= 0 ? zones->zones[host].address : self_address();
        return true;
    }
    case PLACE_EXIT: {
        ticket->position = place->spawn;
        int owner = zones ? choose_shard(s, c, place->destination) : -1;
        if (zones && owner < 0) return false;   // nobody owns where this leads
        *address = owner >= 0 && owner != self ? zones->zones[owner].address : self_address();
        return true;
    }
    default:
        return false;
    }
}

static void check_handoffs(server *s, double now) {
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_ACTIVE || now - c->redirected_at < SERVER_HANDOFF_RETRY) continue;
        game *g = c->world->g;
        const Transform *t = CW_GET(&g->world, c->player, g->c_transform, Transform);
        int place = game_place_at(g, t->position);
        if (place < 0) continue;
        net_redirect m = {};
        if (!handoff_for(s, c, &g->places[place], &m.address, &m.ticket)) continue;
        cw_writer w;
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        net_redirect_write(&w, &m, server_zones(s));
        net_connection_send(&c->connection, MSG_REDIRECT, NET_RELIABLE_ORDERED, s->scratch, w.size);
        c->redirected_at = c->handoff_started = now;
        c->state = CLIENT_HANDING_OFF;
        if (s->config.verbose) {
            char address[32];
            net_address_format(m.address, address, sizeof(address));
            fprintf(stderr, "server: player %lld entered %s, handing over to %s%s\n", (long long)c->player,
                    g->places[place].name, m.address.ip ? address : "this server", m.ticket.dungeon[0] ? " (an instance)" : "");
        }
    }
}

// ---------------------------------------------------------------------------
// The server
// ---------------------------------------------------------------------------
bool server_init(server *s, const server_config *config) {
    memset(s, 0, sizeof(*s));
    s->config = config ? *config : server_config_default();
    if (s->config.max_clients <= 0) s->config.max_clients = 1;
    if (s->config.max_clients > 4096) s->config.max_clients = 4096;
    if (s->config.instance_linger <= 0.0) s->config.instance_linger = SERVER_INSTANCE_LINGER;
    if (s->config.bandwidth == 0) s->config.bandwidth = SERVER_BANDWIDTH;
    if (s->config.linger_quit < 0.0) s->config.linger_quit = 0.0;
    if (s->config.linger_timeout < 0.0) s->config.linger_timeout = 0.0;
    // The zone table to start from.
    net_zones_init(&s->zones);
    if (s->config.zones) {
        if (s->config.zone < 0 || s->config.zone >= s->config.zones->count) return false;
        s->zones = *s->config.zones;
        strncpy(s->zone_name, s->zones.zones[s->config.zone].name, NET_ZONE_NAME - 1);
        s->config.port = s->zones.zones[s->config.zone].address.port;
    } else {
        // Ourselves, under the given name (or a default): the directory, if any, fills in the rest.
        strncpy(s->zone_name, s->config.zone_name[0] ? s->config.zone_name : "zone", NET_ZONE_NAME - 1);
        int self = net_zones_add(&s->zones, s->zone_name, s->config.public_address);
        for (int i = 0; i < s->config.owns_count; ++i) net_zone_add_place(&s->zones.zones[self], s->config.owns[i]);
        s->zones.zones[self].instance_host = s->config.instance_host;
        memcpy(s->zones.zones[self].region, s->config.region, NET_REGION_NAME);
        s->zones.zones[self].target = s->config.target;
        s->zones.zones[self].capacity = (uint16_t)s->config.max_clients;
    }
    if (s->config.secret && *s->config.secret) net_zones_set_secret(&s->zones, s->config.secret);
    s->socket = net_socket_open(s->config.port);
    if (!s->socket) return false;
    if (s->zones.count == 1 && s->zones.zones[0].address.port == 0) s->zones.zones[0].address.port = net_socket_port(s->socket);
    s->clients = (server_client *)calloc((size_t)s->config.max_clients, sizeof(server_client));
    s->tokens = (used_token *)calloc(SERVER_TOKEN_MEMORY, sizeof(used_token));
    s->big = (unsigned char *)malloc(NET_MAX_MESSAGE);
    if (!crypto_random(s->challenge_key, sizeof(s->challenge_key))) return false;
    if (s->config.unconnected_per_ip >= 0.0)
        net_limiter_init(&s->unconnected, s->config.unconnected_per_ip > 0.0 ? s->config.unconnected_per_ip : SERVER_UNCONNECTED_PER_IP,
                         SERVER_UNCONNECTED_BURST);
    if (s->config.directory.ip) s->config.require_token = true;
    s->hash_capacity = (size_t)s->config.max_clients * 2 + 1;
    s->client_by_address = (int *)malloc(s->hash_capacity * sizeof(int));
    for (size_t i = 0; i < s->hash_capacity; ++i) s->client_by_address[i] = -1;
    s->next_session = (uint32_t)(net_seconds() * 1000.0) | 1u;
    s->key_made_at = -1e9;
    s->snapshot_every = SNAPSHOT_EVERY;
    s->load_measured_at = net_seconds();
    snapshot_reserve(&s->scratch_snapshot, (uint16_t)s->config.max_clients);
    s->tick_base = server_time(s);   // universe time now, at tick zero

    if (!s->config.directory.ip) {
        s->local_social = (social *)calloc(1, sizeof(social));
        social_init(s->local_social, &s->zones, local_social_deliver, s);
        if (s->config.save_path) {
            social_set_save_path(s->local_social, s->config.save_path);
            social_load(s->local_social, s->config.save_path);
        }
    }

    game_init(&s->zone, CW_HOST);
    s->zone.universe_base = s->tick_base;
    int self = server_self(s);
    bool instance_host = self >= 0 && s->zones.zones[self].instance_host;
    if (!instance_host) {
        game_build_universe(&s->zone);
        s->zone_world = world_add(s, WORLD_ZONE, &s->zone);
    }
    return true;
}

uint64_t server_target_tick(const server *s) {
    double elapsed = server_time(s) - s->tick_base;
    return elapsed <= 0.0 ? 0 : (uint64_t)(elapsed * GAME_TICK_HZ);
}

void server_destroy(server *s) {
    double now = net_seconds();
    for (int i = 0; i < s->config.max_clients; ++i)
        if (s->clients[i].state != CLIENT_FREE && s->clients[i].state != CLIENT_LINGERING) {
            net_connection_send(&s->clients[i].connection, MSG_DISCONNECT, NET_UNRELIABLE, NULL, 0);
            net_connection_flush(&s->clients[i].connection, now, false);
            net_connection_destroy(&s->clients[i].connection);
            snapshot_ring_destroy(&s->clients[i].sent);
        }
    for (int i = 0; i < s->config.max_clients; ++i) {
        free(s->clients[i].last_relevant);
        free(s->clients[i].known);
        free(s->clients[i].selection);
    }
    free(s->clients);
    free(s->client_by_address);
    free(s->candidates);
    free(s->tokens);
    free(s->big);
    while (s->world_count) close_world(s, s->worlds[s->world_count - 1]);
    if (!s->zone_world) game_destroy(&s->zone);   // never wrapped in a world, so not closed above
    free(s->worlds);
    if (s->local_social && s->local_social->dirty) social_save(s->local_social);
    free(s->local_social);
    snapshot_free(&s->scratch_snapshot);
    net_socket_close(s->socket);
    memset(s, 0, sizeof(*s));
}

uint16_t server_port(const server *s) { return net_socket_port(s->socket); }

server_client *server_client_of(server *s, cw_entity_id player) {
    for (int i = 0; i < s->config.max_clients; ++i)
        if (s->clients[i].state != CLIENT_FREE && s->clients[i].player == player) return &s->clients[i];
    return NULL;
}

server_client *server_client_of_account(server *s, uint64_t account) {
    for (int i = 0; i < s->config.max_clients; ++i)
        if (s->clients[i].state != CLIENT_FREE && s->clients[i].account == account) return &s->clients[i];
    return NULL;
}

static void apply_inputs(server *s) {
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_ACTIVE && c->state != CLIENT_HANDING_OFF) continue;
        game *g = c->world->g;
        PlayerInput *in = CW_GET(&g->world, c->player, g->c_input, PlayerInput);
        if (!in) continue;
        *in = input_queue_apply(&c->inputs);
        c->swinging = in->act && c->state == CLIENT_ACTIVE;
        c->swing_view_tick = c->inputs.last_view_tick;
        if (c->state == CLIENT_HANDING_OFF) {
            // Frozen: the ticket described this player standing here, and the other
            // world will pick up from exactly that. Looking around still works.
            in->forward = in->strafe = 0.0f;
            in->jump = false;
        }
    }
}

void server_update(server *s, double now) {
    register_with_directory(s, now);
    if (s->local_social) social_update(s->local_social, now);
    double t0 = net_seconds();
    receive_packets(s, now);
    double t1 = net_seconds();
    s->time_receive += t1 - t0;
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_FREE && c->state != CLIENT_LINGERING && now - c->last_presence > SOCIAL_PRESENCE_EVERY)
            report_presence(s, c, PRESENCE_REFRESH, now);
    }

    // Leavers and the silent go first, so the tick below despawns them. A frozen
    // player whose hand-off went nowhere thaws.
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state == CLIENT_LINGERING) {
            if (now >= c->linger_until) finalize_client(s, c, "gone from");
        } else if (c->state == CLIENT_LEAVING) {
            detach_client(s, c, c->leave_reason == LEAVE_HANDOFF ? SERVER_LEAVE_HANDOFF : SERVER_LEAVE_QUIT, now);
        } else if (c->state != CLIENT_FREE && now - c->connection.last_received > s->config.timeout) {
            detach_client(s, c, c->state == CLIENT_HANDING_OFF ? SERVER_LEAVE_HANDOFF : SERVER_LEAVE_TIMEOUT, now);
        } else if (c->state == CLIENT_HANDING_OFF && now - c->handoff_started > SERVER_HANDOFF_TIMEOUT) {
            c->state = CLIENT_ACTIVE;
            ++c->handoffs_failed;
            if (s->config.verbose) fprintf(stderr, "server: player %lld never left; unfrozen\n", (long long)c->player);
        }
    }
    // Instances nobody has come back to.
    for (int i = s->world_count - 1; i >= 0; --i) {
        server_world *world = s->worlds[i];
        if (world->kind == WORLD_INSTANCE && world->client_count == 0 && now - world->empty_since > s->config.instance_linger)
            close_world(s, world);
    }

    // The wall clock says which tick we should be at. Normally that is the next one
    // or two. After a stall we catch up, to a point: beyond SERVER_MAX_CATCHUP the
    // skipped time is skipped (ships are functions of time and land where they
    // should; players simply did not move). A large jump either way means the clock
    // itself moved, and universe time moves with it.
    uint64_t target = server_target_tick(s);
    double behind = ((double)target - (double)s->tick) / GAME_TICK_HZ;
    if (behind < -SERVER_REBASE_SECONDS || behind > SERVER_REBASE_SECONDS) {
        s->tick_base += behind;   // universe time at tick zero moves so that target == tick
        for (int i = 0; i < s->world_count; ++i) s->worlds[i]->g->universe_base = s->tick_base;
        target = s->tick;
        ++s->rebases;
        if (s->config.verbose) fprintf(stderr, "server: the clock jumped %.1f s; universe time follows\n", behind);
    } else if (target > s->tick + SERVER_MAX_CATCHUP) {
        uint64_t skipped = target - s->tick - SERVER_MAX_CATCHUP;
        s->tick += skipped;
        for (int i = 0; i < s->world_count; ++i) s->worlds[i]->g->world.tick += skipped;
        s->ticks_skipped += (uint32_t)skipped;
        if (s->config.verbose)
            fprintf(stderr, "server: %.2f s behind the clock; skipped %llu ticks\n", behind, (unsigned long long)skipped);
    }
    while (s->tick < target) {
        ++s->tick;
        double a = net_seconds();
        apply_inputs(s);
        for (int i = 0; i < s->world_count; ++i) game_tick(s->worlds[i]->g);
        resolve_swings(s);
        for (int i = 0; i < s->world_count; ++i) record_trails(s->worlds[i]);
        // Loot underfoot is taken: worn if better, stashed otherwise, saved either way.
        for (int i = 0; i < s->config.max_clients; ++i) {
            server_client *c = &s->clients[i];
            if (c->state != CLIENT_ACTIVE || !c->gear_known) continue;
            game *g = c->world->g;
            cw_entity_id loot = game_loot_at(g, CW_GET(&g->world, c->player, g->c_transform, Transform)->position);
            if (loot == CW_INVALID_ENTITY) continue;
            if (!character_take(&c->gear, &CW_GET(&g->world, loot, g->c_loot, Loot)->it)) continue;   // no room
            cw_world_mark_destroy(&g->world, loot);
            wear_gear(s, c);
            save_gear(s, c, now);
            ++s->pickups;
        }
        s->time_ticks += net_seconds() - a;

        for (int i = 0; i < s->config.max_clients; ++i) {
            server_client *c = &s->clients[i];
            if (c->state != CLIENT_ACTIVE && c->state != CLIENT_HANDING_OFF) continue;
            server_world *world = c->world;
            game *g = world->g;
            // Despawns of what the client knows of; spawns of scenery. Players are
            // spawned on interest, from the snapshots.
            cw_entity_id batch[64];
            size_t n = 0;
            for (size_t k = 0; k < world->despawned.count; ++k) {
                cw_entity_id id = world->despawned.data[k];
                if (!knows(c, id)) continue;
                set_known(c, id, false);
                batch[n++] = id;
                if (n == 64) { send_despawns(s, c, batch, n); n = 0; }
            }
            if (n) send_despawns(s, c, batch, n);
            n = 0;
            for (size_t k = 0; k < world->spawned.count; ++k) {
                cw_entity_id id = world->spawned.data[k];
                int cls = relevance_class(g, id);
                if (cls == RELEVANCE_PLAYER && id != c->player) continue;
                if (cls == RELEVANCE_LOOT) {
                    if (!relevant(s, c, id)) continue;   // the relevance pass sends it when the client comes near
                    note_relevant(c, id, (uint32_t)g->world.tick);
                }
                batch[n++] = id;
                if (n == 64) { send_spawns(s, c, batch, n); n = 0; }
            }
            if (n) send_spawns(s, c, batch, n);
            // The components that changed (gear), to those who know the entity.
            send_updates(s, c, world->updated, world->updated_count);
        }
        for (int i = 0; i < s->world_count; ++i)
            s->worlds[i]->spawned.count = s->worlds[i]->despawned.count = s->worlds[i]->updated_count = 0;

        // Snapshots, staggered: each client gets one every snapshot_every ticks, a
        // share of them every tick, so that building them is spread evenly (a
        // thousand at once would keep the socket unread long enough to overflow)
        // and so is what leaves.
        double b = net_seconds();
        if (s->tick % (uint64_t)s->snapshot_every == 0) check_handoffs(s, now);
        for (int i = 0; i < s->config.max_clients; ++i)
            if ((s->clients[i].state == CLIENT_ACTIVE || s->clients[i].state == CLIENT_HANDING_OFF)
                && (s->tick + (uint64_t)i) % (uint64_t)s->snapshot_every == 0)
                send_snapshot(s, &s->clients[i]);
        s->time_snapshots += net_seconds() - b;
    }

    double t2 = net_seconds();
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_FREE && c->state != CLIENT_LINGERING)
            net_connection_flush(&c->connection, now, now - c->connection.last_sent > NET_KEEPALIVE);
    }
    s->time_flush += net_seconds() - t2;

    // Once a second: the snapshot rate follows the load (halving down to 5 Hz when
    // snapshots eat the budget, doubling back when they do not), and the timing log.
    double after = net_seconds();
    if (after - s->load_measured_at >= 1.0) {
        double elapsed = after - s->load_measured_at;
        // The server's total budget, shared out: each client's cap is its share of
        // it or the per-client cap, whichever is less.
        if (s->config.total_bandwidth) {
            uint32_t share = s->config.total_bandwidth / (uint32_t)(s->client_count > 0 ? s->client_count : 1);
            uint32_t cap = share < s->config.bandwidth ? share : s->config.bandwidth;
            for (int i = 0; i < s->config.max_clients; ++i)
                if (s->clients[i].state == CLIENT_ACTIVE || s->clients[i].state == CLIENT_HANDING_OFF)
                    net_connection_set_rate(&s->clients[i].connection, cap);
        }
        double load = s->time_snapshots / elapsed;
        if (load > SERVER_SNAPSHOT_LOAD_UP && s->snapshot_every < 4 * SNAPSHOT_EVERY) s->snapshot_every *= 2;
        else if (load < SERVER_SNAPSHOT_LOAD_DOWN && s->snapshot_every > SNAPSHOT_EVERY) s->snapshot_every /= 2;
        if (s->config.trace)
            fprintf(stderr, "timing: %d clients, %u accepts: receive %.0f ms, ticks %.0f ms, snapshots %.0f ms, flush %.0f ms per second; snapshots every %d ticks\n",
                    s->client_count, s->accepts_since_trace, s->time_receive * 1000.0 / elapsed, s->time_ticks * 1000.0 / elapsed,
                    s->time_snapshots * 1000.0 / elapsed, s->time_flush * 1000.0 / elapsed, s->snapshot_every);
        s->time_receive = s->time_ticks = s->time_snapshots = s->time_flush = 0.0;
        s->accepts_since_trace = 0;
        s->load_measured_at = after;
    }
    if (s->config.trace && now - s->last_trace >= 1.0) {
        s->last_trace = now;
        for (int i = 0; i < s->config.max_clients; ++i) {
            server_client *c = &s->clients[i];
            if (c->state == CLIENT_FREE) continue;
            game *g = c->world->g;
            v3 at = CW_GET(&g->world, c->player, g->c_transform, Transform)->position;
            int place = game_place_at(g, at);
            fprintf(stderr, "trace: tick %llu player %lld in %s at %.2f %.2f %.2f, %s, aboard %lld, input %u applied %u starved %u skipped %u\n",
                    (unsigned long long)g->world.tick, (long long)c->player, world_name(c->world), at.x, at.y, at.z,
                    place >= 0 ? g->places[place].name : "between places", (long long)game_ship_at(g, at),
                    c->inputs.newest, c->inputs.last_applied, c->inputs.starved, c->inputs.skipped);
        }
    }
}
