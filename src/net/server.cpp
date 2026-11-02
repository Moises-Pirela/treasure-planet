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
    return c;
}

static void log_client(const server *s, const server_client *c, const char *what) {
    if (!s->config.verbose) return;
    char address[32];
    net_address_format(c->connection.peer, address, sizeof(address));
    fprintf(stderr, "server: %s %s (player %lld, %d online)\n", address, what, (long long)c->player, s->client_count);
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

static void send_snapshot(server *s, server_client *c) {
    game *g = &s->zone;
    cw_world *w = &g->world;
    const Transform *own = cw_comp_has(w, c->player, g->c_transform) ? CW_GET(w, c->player, g->c_transform, Transform) : NULL;
    float radius2 = s->config.interest_radius * s->config.interest_radius;

    net_snapshot_header header = {};
    header.tick = (uint32_t)w->tick;
    header.last_input_seq = c->last_input_seq;

    cw_writer wr;
    cw_writer_init_fixed(&wr, s->scratch, NET_MAX_PAYLOAD);
    net_snapshot_header_write(&wr, &header);
    size_t header_size = wr.size;

    cw_query dynamic = cw_query_begin(g->q_dynamic);
    cw_entity_id id;
    while (cw_query_next(&dynamic, &id)) {
        const Transform *t = CW_GET(w, id, g->c_transform, Transform);
        const Player *p = cw_comp_has(w, id, g->c_player) ? CW_GET(w, id, g->c_player, Player) : NULL;
        if (p && radius2 > 0.0f && id != c->player && own) {
            v3 d = v3_sub(t->position, own->position);
            if (v3_dot(d, d) > radius2) continue;
        }
        net_snapshot_entry e = {};
        e.id = id;
        e.position = t->position;
        if (p) {
            e.yaw = p->yaw;
            e.pitch = p->pitch;
        }
        net_snapshot_entry_write(&wr, &e);
        if (++header.count == NET_MAX_SNAPSHOT_ENTRIES) {
            s->scratch[header_size - 2] = (unsigned char)(header.count & 0xff);
            s->scratch[header_size - 1] = (unsigned char)(header.count >> 8);
            net_connection_send(&c->connection, MSG_SNAPSHOT, false, s->scratch, wr.size);
            cw_writer_init_fixed(&wr, s->scratch, NET_MAX_PAYLOAD);
            header.count = 0;
            net_snapshot_header_write(&wr, &header);
        }
    }
    if (header.count || c->snapshots_sent == 0) {
        s->scratch[header_size - 2] = (unsigned char)(header.count & 0xff);
        s->scratch[header_size - 1] = (unsigned char)(header.count >> 8);
        net_connection_send(&c->connection, MSG_SNAPSHOT, false, s->scratch, wr.size);
    }
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
        if (c->has_input && net_sequence_compare((uint16_t)m.seq, (uint16_t)c->last_input_seq) <= 0) return;
        // Newest wins, except that a jump pressed in any input we had not seen yet
        // still counts: a lost packet must not swallow a key press.
        uint32_t unseen = c->has_input ? m.seq - c->last_input_seq : m.count;
        if (unseen > m.count) unseen = m.count;
        PlayerInput in = m.inputs[m.count - 1];
        for (uint32_t i = m.count - unseen; i < m.count; ++i) in.jump = in.jump || m.inputs[i].jump;
        c->input = in;
        c->last_input_seq = m.seq;
        c->has_input = true;
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
    bool connect;
    uint32_t version;
};
static void on_handshake_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    handshake *h = (handshake *)user;
    if (type != MSG_CONNECT) return;
    uint32_t nonce;
    cw_reader_u32(&payload, &h->version);
    cw_reader_u32(&payload, &nonce);
    h->connect = cw_reader_ok(&payload);
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

static void accept_client(server *s, net_address from, double now) {
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
    c->player = game_spawn_player(&s->zone, s->zone.spawn);
    c->joined_at = now;
    ++s->client_count;

    net_welcome welcome = {};
    welcome.session = session;
    welcome.player = c->player;
    welcome.tick = (uint32_t)s->zone.world.tick;
    welcome.tick_hz = GAME_TICK_HZ;
    welcome.snapshot_hz = NET_SNAPSHOT_HZ;
    cw_writer w;
    cw_writer_init_fixed(&w, s->scratch, NET_MAX_PAYLOAD);
    net_welcome_write(&w, &welcome);
    // The first reliable message on the connection. The client has no session yet and
    // adopts the one stamped on the packet that delivers this.
    net_connection_send(&c->connection, MSG_WELCOME, true, s->scratch, w.size);
    send_baseline(s, c);
    log_client(s, c, "joined");
}

static void drop_client(server *s, server_client *c, const char *why) {
    log_client(s, c, why);
    net_connection_send(&c->connection, MSG_DISCONNECT, false, NULL, 0);
    net_connection_flush(&c->connection, net_seconds(), false);
    game_despawn_player(&s->zone, c->player);
    net_connection_destroy(&c->connection);
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
        net_connection tmp;
        net_connection_init(&tmp, s->socket, from, 0, on_handshake_message, &h);
        net_connection_receive(&tmp, s->scratch, (size_t)size, now);
        net_connection_destroy(&tmp);
        if (!h.connect) continue;
        if (h.version != NET_PROTOCOL_VERSION) reject(s, from, REJECT_VERSION, now);
        else accept_client(s, from, now);
    }
}

// ---------------------------------------------------------------------------
// The server
// ---------------------------------------------------------------------------
bool server_init(server *s, const server_config *config) {
    memset(s, 0, sizeof(*s));
    s->config = config ? *config : server_config_default();
    if (s->config.max_clients <= 0) s->config.max_clients = 1;
    s->socket = net_socket_open(s->config.port);
    if (!s->socket) return false;
    s->clients = (server_client *)calloc((size_t)s->config.max_clients, sizeof(server_client));
    s->next_session = (uint32_t)(net_seconds() * 1000.0) | 1u;

    game_init(&s->zone, CW_HOST);
    s->zone.world.host = s;
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
    s->ship_changes = cw_observer_create(&s->zone.world, s->zone.c_ship);

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
        }
    free(s->clients);
    free(s->spawned.data);
    free(s->despawned.data);
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

static void broadcast_ship_changes(server *s) {
    if (!cw_observer_pending(s->ship_changes, CW_CHANGE_MODIFIED)) return;
    cw_observer_observe(s->ship_changes);
    cw_world *w = &s->zone.world;
    cw_query changed = cw_observer_iter(s->ship_changes, CW_CHANGE_MODIFIED);
    cw_entity_id id;
    while (cw_query_next(&changed, &id)) {
        const Ship *ship = CW_GET(w, id, s->zone.c_ship, Ship);
        net_ship_state m = {id, ship->state, ship->stop, ship->timer};
        cw_writer wr;
        cw_writer_init_fixed(&wr, s->scratch, NET_MAX_PAYLOAD);
        net_ship_state_write(&wr, &m);
        for (int i = 0; i < s->config.max_clients; ++i)
            if (s->clients[i].state == CLIENT_ACTIVE)
                net_connection_send(&s->clients[i].connection, MSG_SHIP_STATE, true, s->scratch, wr.size);
    }
}

static void apply_inputs(server *s) {
    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_ACTIVE || !c->has_input) continue;
        PlayerInput *in = CW_GET(&s->zone.world, c->player, s->zone.c_input, PlayerInput);
        if (in) *in = c->input;
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
        broadcast_ship_changes(s);
        if (s->zone.world.tick % SNAPSHOT_EVERY == 0)
            for (int i = 0; i < s->config.max_clients; ++i)
                if (s->clients[i].state == CLIENT_ACTIVE) send_snapshot(s, &s->clients[i]);
    }

    for (int i = 0; i < s->config.max_clients; ++i) {
        server_client *c = &s->clients[i];
        if (c->state != CLIENT_FREE)
            net_connection_flush(&c->connection, now, now - c->connection.last_sent > NET_KEEPALIVE);
    }
}
