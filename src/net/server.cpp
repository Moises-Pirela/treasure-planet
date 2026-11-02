#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAPSHOT_EVERY (GAME_TICK_HZ / NET_SNAPSHOT_HZ)

server_config server_config_default(void) {
    server_config c = {};
    c.port = NET_DEFAULT_PORT;
    c.max_clients = 64;
    c.interest_radius = 0.0f;
    c.timeout = NET_TIMEOUT;
    c.zone = -1;
    return c;
}

static void log_client(const server *s, const server_client *c, const char *what) {
    if (!s->config.verbose) return;
    char address[32];
    net_address_format(c->connection.peer, address, sizeof(address));
    fprintf(stderr, "server: %s %s (player %lld, account %llx, %d online)\n", address, what, (long long)c->player,
            (unsigned long long)c->account, s->client_count);
}

// ---------------------------------------------------------------------------
// Id lists
// ---------------------------------------------------------------------------
static void ids_push(cw_id_list *list, cw_entity_id id) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->data = (cw_entity_id *)realloc(list->data, list->capacity * sizeof(cw_entity_id));
    }
    list->data[list->count++] = id;
}

// Lifecycle hooks on the game's sim: the world's host pointer is the server.
static void system_record_created(cw_world *w, cw_phase, float) {
    server *s = (server *)w->host;
    cw_id_view created = cw_world_created(w);
    for (size_t i = 0; i < created.count; ++i) ids_push(&s->spawned, created.data[i]);
}
static void system_record_destroyed(cw_world *w, cw_phase, float) {
    server *s = (server *)w->host;
    cw_id_view pending = cw_world_pending_destroy(w);
    for (size_t i = 0; i < pending.count; ++i) ids_push(&s->despawned, pending.data[i]);
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
        q->last_applied = next;
        return q->last;
    }
    ++q->starved;
    q->primed = false;
    PlayerInput repeat = q->last;
    repeat.jump = false;
    return repeat;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
// Sends entity records for `ids` in as many MSG_SPAWN messages as they need.
static void send_spawns(server *s, server_client *c, const cw_entity_id *ids, size_t count) {
    cw_writer w;
    size_t i = 0;
    while (i < count) {
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        cw_writer_u16(&w, 0);
        uint16_t in_message = 0;
        while (i < count) {
            if (!cw_world_is_valid(&s->zone.world, ids[i])) { ++i; continue; }
            size_t size = net_entity_size(&s->zone, ids[i]);
            if (w.size + size > NET_MAX_PAYLOAD) {
                if (in_message == 0) {
                    fprintf(stderr, "server: entity %lld does not fit a packet, skipped\n", (long long)ids[i]);
                    ++i;
                }
                break;
            }
            net_entity_write(&w, &s->zone, ids[i]);
            ++in_message;
            ++i;
        }
        if (in_message == 0) continue;
        s->scratch[0] = (unsigned char)(in_message & 0xff);
        s->scratch[1] = (unsigned char)(in_message >> 8);
        net_connection_send(&c->connection, MSG_SPAWN, true, s->scratch, w.size);
    }
}

static void send_despawns(server *s, server_client *c, const cw_entity_id *ids, size_t count) {
    const size_t per_message = (NET_MAX_PAYLOAD - 2) / 8;
    for (size_t i = 0; i < count; i += per_message) {
        size_t n = count - i < per_message ? count - i : per_message;
        cw_writer w;
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        cw_writer_u16(&w, (uint16_t)n);
        for (size_t k = 0; k < n; ++k) cw_writer_i64(&w, ids[i + k]);
        net_connection_send(&c->connection, MSG_DESPAWN, true, s->scratch, w.size);
    }
}

// The whole world, for a client that just joined.
static void send_baseline(server *s, server_client *c) {
    cw_world *w = &s->zone.world;
    size_t count = cw_world_entity_count(w);
    cw_entity_id *ids = (cw_entity_id *)malloc((count ? count : 1) * sizeof(cw_entity_id));
    size_t n = cw_query_collect_filter(w, cw_filter_all(cw_signature{}), ids, count);
    send_spawns(s, c, ids, n);
    free(ids);
    net_connection_send(&c->connection, MSG_BASELINE_END, true, NULL, 0);
}

static int compare_priority(const void *a, const void *b) {
    float x = ((const snapshot_entry *)a)->priority, y = ((const snapshot_entry *)b)->priority;
    return x < y ? -1 : x > y;
}

// Every player the client should hear about, nearest first, own player first.
static void build_snapshot(server *s, server_client *c) {
    game *g = &s->zone;
    cw_world *w = &g->world;
    snapshot *snap = &s->scratch_snapshot;
    snap->tick = (uint32_t)w->tick;
    snap->last_input_seq = c->inputs.last_applied;
    snap->count = 0;
    v3 own = CW_GET(w, c->player, g->c_transform, Transform)->position;
    float radius2 = s->config.interest_radius * s->config.interest_radius;

    cw_query players = cw_query_begin(g->q_all_players);
    cw_entity_id id;
    while (cw_query_next(&players, &id) && snap->count < snap->capacity) {
        const Transform *t = CW_GET(w, id, g->c_transform, Transform);
        const Player *p = CW_GET(w, id, g->c_player, Player);
        v3 d = v3_sub(t->position, own);
        float distance2 = v3_dot(d, d);
        if (id != c->player && radius2 > 0.0f && distance2 > radius2) continue;
        cw_entity_id aboard = game_ship_at(g, t->position);
        v3 position = t->position;
        if (aboard != CW_INVALID_ENTITY)
            position = v3_sub(position, CW_GET(w, aboard, g->c_transform, Transform)->position);
        snapshot_entry e = snapshot_entry_make(id, aboard, position, p->velocity, p->yaw, p->pitch, p->grounded);
        e.priority = id == c->player ? -1.0f : distance2;
        snap->entries[snap->count++] = e;
    }
    qsort(snap->entries, snap->count, sizeof(snapshot_entry), compare_priority);
}

static void send_snapshot(server *s, server_client *c) {
    build_snapshot(s, c);
    snapshot *snap = &s->scratch_snapshot;
    const snapshot *base = snapshot_ring_find(&c->sent, c->ack_tick);
    for (;;) {
        // Nearest `count` entries, sorted by id for coding. Too big: drop the farthest.
        snapshot_sort(snap);
        cw_writer w;
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        if (snapshot_encode(snap, base, &w)) {
            net_connection_send(&c->connection, MSG_SNAPSHOT, false, s->scratch, w.size);
            break;
        }
        if (snap->count <= 1) return;   // cannot even fit ourselves: nothing to send
        qsort(snap->entries, snap->count, sizeof(snapshot_entry), compare_priority);
        snap->count = (uint16_t)(snap->count - (snap->count / 4 > 0 ? snap->count / 4 : 1));
    }
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
    case MSG_DISCONNECT:
        c->state = CLIENT_LEAVING;
        break;
    default:
        break;   // MSG_KEEPALIVE, and anything a newer client might say
    }
}

struct handshake {
    const net_zone_table *zones;
    bool connect, bad_ticket;
    net_connect_message message;
};
static void on_handshake_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    handshake *h = (handshake *)user;
    if (type != MSG_CONNECT) return;
    h->connect = net_connect_read(&payload, &h->message, h->zones, &h->bad_ticket);
}

static server_client *find_client(server *s, net_address from) {
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_FREE && net_address_equal(c->connection.peer, from)) return c;
    }
    return NULL;
}

static void reject(server *s, net_address to, uint8_t reason, double now) {
    net_connection tmp;
    net_connection_init(&tmp, s->socket, to, 0, NULL, NULL);
    net_connection_send(&tmp, MSG_REJECT, false, &reason, 1);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

v3 server_spawn(const server *s) {
    if (s->config.zones && s->config.zone >= 0) {
        const net_zone *zone = &s->config.zones->zones[s->config.zone];
        for (int i = 0; i < zone->place_count; ++i) {
            int place = game_find_place(&s->zone, zone->places[i]);
            if (place >= 0) return s->zone.places[place].spawn;
        }
    }
    return s->zone.spawn;
}

static void accept_client(server *s, net_address from, const net_connect_message *m, double now) {
    server_client *c = NULL;
    for (int i = 0; i < s->config.max_clients && !c; ++i)
        if (s->clients[i].state == CLIENT_FREE) c = &s->clients[i];
    if (!c) {
        reject(s, from, REJECT_FULL, now);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->state = CLIENT_ACTIVE;
    uint32_t session = s->next_session++;
    if (session == 0) session = s->next_session++;   // 0 means "no session yet"
    net_connection_init(&c->connection, s->socket, from, session, on_client_message, c);
    snapshot_ring_init(&c->sent);
    c->joined_at = now;

    if (m->has_ticket) {
        // Handed over from another zone: carry on from where they were.
        c->account = m->ticket.account;
        c->player = game_spawn_player(&s->zone, m->ticket.position, true);
        Player *p = CW_GET(&s->zone.world, c->player, s->zone.c_player, Player);
        p->velocity = m->ticket.velocity;
        p->yaw = m->ticket.yaw;
        p->pitch = m->ticket.pitch;
    } else {
        c->account = ((uint64_t)(uint32_t)(net_wall_seconds() * 1000.0) << 32) ^ ((uint64_t)session << 8) ^ m->nonce;
        c->player = game_spawn_player(&s->zone, server_spawn(s), true);
    }
    ++s->client_count;

    net_welcome welcome = {};
    welcome.session = session;
    welcome.player = c->player;
    welcome.tick = (uint32_t)s->zone.world.tick;
    welcome.tick_hz = GAME_TICK_HZ;
    welcome.snapshot_hz = NET_SNAPSHOT_HZ;
    welcome.universe_base = s->zone.universe_base;
    welcome.spawn = s->zone.spawn;
    welcome.account = c->account;
    welcome.place_count = s->zone.place_count;
    memcpy(welcome.places, s->zone.places, sizeof(welcome.places));
    cw_writer w;
    cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
    net_welcome_write(&w, &welcome);
    // The first reliable message on the connection. The client has no session yet and
    // adopts the one stamped on the packet that delivers this.
    net_connection_send(&c->connection, MSG_WELCOME, true, s->scratch, w.size);
    send_baseline(s, c);
    log_client(s, c, m->has_ticket ? "arrived" : "joined");
}

static void drop_client(server *s, server_client *c, const char *why) {
    log_client(s, c, why);
    net_connection_send(&c->connection, MSG_DISCONNECT, false, NULL, 0);
    net_connection_flush(&c->connection, net_seconds(), false);
    game_despawn_player(&s->zone, c->player);
    net_connection_destroy(&c->connection);
    snapshot_ring_destroy(&c->sent);
    c->state = CLIENT_FREE;
    --s->client_count;
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
            // Session 0 from a known address is a repeated MSG_CONNECT that crossed
            // our welcome; the reliable welcome will get there.
            if (session != 0) net_connection_receive(&c->connection, s->scratch, (size_t)size, now);
            continue;
        }
        if (session != 0) continue;   // a stale session: a client we already dropped
        handshake h = {};
        h.zones = s->config.zones;
        net_connection tmp;
        net_connection_init(&tmp, s->socket, from, 0, on_handshake_message, &h);
        net_connection_receive(&tmp, s->scratch, (size_t)size, now);
        net_connection_destroy(&tmp);
        if (!h.connect) continue;
        if (h.message.version != NET_PROTOCOL_VERSION) reject(s, from, REJECT_VERSION, now);
        else if (h.bad_ticket && s->config.zones) reject(s, from, REJECT_TICKET, now);
        else if (h.message.has_ticket
                 && fabs(game_time_of_tick(&s->zone, s->zone.world.tick) - h.message.ticket.issued) > NET_TICKET_LIFETIME)
            reject(s, from, REJECT_TICKET, now);
        else accept_client(s, from, &h.message, now);
    }
}

// ---------------------------------------------------------------------------
// Hand-offs
// ---------------------------------------------------------------------------
static void check_handoffs(server *s, double now) {
    const net_zone_table *zones = s->config.zones;
    if (!zones) return;
    game *g = &s->zone;
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_ACTIVE || now - c->redirected_at < SERVER_HANDOFF_RETRY) continue;
        const Transform *t = CW_GET(&g->world, c->player, g->c_transform, Transform);
        int place = game_place_at(g, t->position);
        if (place < 0) continue;
        int owner = net_zones_owner(zones, g->places[place].name);
        if (owner < 0 || owner == s->config.zone) continue;

        const Player *p = CW_GET(&g->world, c->player, g->c_player, Player);
        net_redirect m = {};
        m.address = zones->zones[owner].address;
        m.ticket.account = c->account;
        m.ticket.position = t->position;
        m.ticket.velocity = p->velocity;
        m.ticket.yaw = p->yaw;
        m.ticket.pitch = p->pitch;
        m.ticket.issued = game_time_of_tick(g, g->world.tick);
        cw_writer w;
        cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
        net_redirect_write(&w, &m, zones);
        net_connection_send(&c->connection, MSG_REDIRECT, true, s->scratch, w.size);
        c->redirected_at = now;
        if (s->config.verbose)
            fprintf(stderr, "server: player %lld entered %s, handing over to zone %s\n", (long long)c->player,
                    g->places[place].name, zones->zones[owner].name);
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
    if (s->config.zones && (s->config.zone < 0 || s->config.zone >= s->config.zones->count)) return false;
    if (s->config.zones) s->config.port = s->config.zones->zones[s->config.zone].address.port;
    s->socket = net_socket_open(s->config.port);
    if (!s->socket) return false;
    s->clients = (server_client *)calloc((size_t)s->config.max_clients, sizeof(server_client));
    s->next_session = (uint32_t)(net_seconds() * 1000.0) | 1u;
    snapshot_reserve(&s->scratch_snapshot, (uint16_t)s->config.max_clients);

    game_init(&s->zone, CW_HOST);
    s->zone.world.host = s;
    s->zone.universe_base = net_wall_seconds() - s->config.universe_epoch;   // universe time now, at tick zero
    game_build_universe(&s->zone);
    cw_system_desc d = {};
    d.name = "net_record_created";
    d.phases = CW_PHASE(CW_ENTITY_CREATED);
    d.fn = system_record_created;
    d.authority = CW_HOST_ONLY;
    cw_sim_register(&s->zone.sim, d);
    d.name = "net_record_destroyed";
    d.phases = CW_PHASE(CW_ENTITY_DESTROYED);
    d.fn = system_record_destroyed;
    cw_sim_register(&s->zone.sim, d);
    game_tick(&s->zone);   // settles the universe's own created list before anyone joins
    s->spawned.count = 0;

    s->clock = cw_clock_make(GAME_TICK_HZ);
    return true;
}

void server_destroy(server *s) {
    double now = net_seconds();
    for (int i = 0; i < s->config.max_clients; ++i)
        if (s->clients[i].state != CLIENT_FREE) {
            net_connection_send(&s->clients[i].connection, MSG_DISCONNECT, false, NULL, 0);
            net_connection_flush(&s->clients[i].connection, now, false);
            net_connection_destroy(&s->clients[i].connection);
            snapshot_ring_destroy(&s->clients[i].sent);
        }
    free(s->clients);
    free(s->spawned.data);
    free(s->despawned.data);
    snapshot_free(&s->scratch_snapshot);
    game_destroy(&s->zone);
    net_socket_close(s->socket);
    memset(s, 0, sizeof(*s));
}

uint16_t server_port(const server *s) { return net_socket_port(s->socket); }

server_client *server_client_of(server *s, cw_entity_id player) {
    for (int i = 0; i < s->config.max_clients; ++i)
        if (s->clients[i].state != CLIENT_FREE && s->clients[i].player == player) return &s->clients[i];
    return NULL;
}

static void apply_inputs(server *s) {
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_ACTIVE) continue;
        PlayerInput *in = CW_GET(&s->zone.world, c->player, s->zone.c_input, PlayerInput);
        if (in) *in = input_queue_apply(&c->inputs);
    }
}

void server_update(server *s, double now) {
    receive_packets(s, now);

    // Leavers and the silent go first, so the tick below despawns them.
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state == CLIENT_LEAVING) drop_client(s, c, "left");
        else if (c->state == CLIENT_ACTIVE && now - c->connection.last_received > s->config.timeout)
            drop_client(s, c, "timed out");
    }

    if (!s->clock_started) {
        s->clock_started = true;
        s->last_update = now;
    }
    int steps = cw_clock_advance(&s->clock, (float)(now - s->last_update));
    s->last_update = now;
    for (int step = 0; step < steps; ++step) {
        apply_inputs(s);
        game_tick(&s->zone);

        if (s->spawned.count || s->despawned.count) {
            for (int i = 0; i < s->config.max_clients; ++i) {
                server_client *c = &s->clients[i];
                if (c->state != CLIENT_ACTIVE) continue;
                if (s->despawned.count) send_despawns(s, c, s->despawned.data, s->despawned.count);
                if (s->spawned.count) send_spawns(s, c, s->spawned.data, s->spawned.count);
            }
            s->spawned.count = s->despawned.count = 0;
        }
        if (s->zone.world.tick % SNAPSHOT_EVERY == 0) {
            check_handoffs(s, now);
            for (int i = 0; i < s->config.max_clients; ++i)
                if (s->clients[i].state == CLIENT_ACTIVE) send_snapshot(s, &s->clients[i]);
        }
    }

    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_FREE)
            net_connection_flush(&c->connection, now, now - c->connection.last_sent > NET_KEEPALIVE);
    }

    if (s->config.trace && now - s->last_trace >= 1.0) {
        s->last_trace = now;
        game *g = &s->zone;
        for (int i = 0; i < s->config.max_clients; ++i) {
            server_client *c = &s->clients[i];
            if (c->state != CLIENT_ACTIVE) continue;
            v3 at = CW_GET(&g->world, c->player, g->c_transform, Transform)->position;
            int place = game_place_at(g, at);
            fprintf(stderr, "trace: tick %llu player %lld at %.2f %.2f %.2f, %s, aboard %lld, input %u applied %u starved %u skipped %u\n",
                    (unsigned long long)g->world.tick, (long long)c->player, at.x, at.y, at.z,
                    place >= 0 ? g->places[place].name : "between places", (long long)game_ship_at(g, at),
                    c->inputs.newest, c->inputs.last_applied, c->inputs.starved, c->inputs.skipped);
        }
    }
}
