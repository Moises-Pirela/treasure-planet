#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Samples
// ---------------------------------------------------------------------------
// Note that CW_GET returns an entity's slot whether or not it carries the
// component (pools are sparse with stable addresses), so presence is cw_comp_has.
static Replica *replica_of(net_client *c, cw_entity_id id) {
    cw_world *w = &c->replica->world;
    if (!cw_comp_has(w, id, c->c_replica)) {
        Replica fresh = {};
        cw_comp_set(w, id, c->c_replica, &fresh, sizeof(fresh));
    }
    return CW_GET(w, id, c->c_replica, Replica);
}

static const net_sample *sample_at(const Replica *r, int i) {
    return &r->samples[(r->head + i) % NET_SAMPLE_HISTORY];
}

static void replica_push(Replica *r, const net_sample *s) {
    if (r->count) {
        int32_t gap = (int32_t)(s->tick - sample_at(r, r->count - 1)->tick);
        if (gap <= 0) return;                                   // old news
        if (gap > GAME_TICK_HZ) r->count = 0;                   // back after a long absence: no sliding over
    }
    if (r->count == NET_SAMPLE_HISTORY) {
        r->head = (r->head + 1) % NET_SAMPLE_HISTORY;
        --r->count;
    }
    r->samples[(r->head + r->count) % NET_SAMPLE_HISTORY] = *s;
    ++r->count;
    r->last_seen_tick = s->tick;
}

static v3 replica_position(const Replica *r, double tick) {
    const net_sample *newest = sample_at(r, r->count - 1);
    if (r->count == 1 || tick >= (double)newest->tick) return newest->position;
    const net_sample *oldest = sample_at(r, 0);
    if (tick <= (double)oldest->tick) return oldest->position;
    for (int i = r->count - 2; i >= 0; --i) {
        const net_sample *a = sample_at(r, i), *b = sample_at(r, i + 1);
        if ((double)a->tick <= tick) {
            float t = (float)((tick - (double)a->tick) / (double)(b->tick - a->tick));
            return v3_lerp(a->position, b->position, t);
        }
    }
    return newest->position;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
static void fail(net_client *c, const char *why) {
    c->state = NC_DISCONNECTED;
    c->failure = why;
}

static void on_snapshot(net_client *c, cw_reader *r, double now) {
    net_snapshot_header h;
    if (!net_snapshot_header_read(r, &h)) return;
    game *g = c->replica;
    cw_world *w = &g->world;

    if (!c->has_offset || (int32_t)(h.tick - c->latest_tick) > 0) {
        double offset = (double)h.tick - now * GAME_TICK_HZ;
        if (!c->has_offset || fabs(offset - c->tick_offset) > GAME_TICK_HZ) {
            c->tick_offset = offset;   // first, or a jump large enough to be a real change
            c->has_offset = true;
        } else if (offset > c->tick_offset) {
            c->tick_offset += (offset - c->tick_offset) * 0.5;    // we were behind: catch up
        } else {
            c->tick_offset += (offset - c->tick_offset) * 0.02;   // drift back slowly
        }
        c->latest_tick = h.tick;
    }
    if ((int32_t)(h.last_input_seq - c->acked_input_seq) > 0) {
        if (c->input_seq - h.last_input_seq < 64) {
            float sample = (float)(now - c->input_sent_at[h.last_input_seq % 64]);
            c->rtt = c->rtt > 0.0f ? c->rtt * 0.9f + sample * 0.1f : sample;
        }
        c->acked_input_seq = h.last_input_seq;
    }
    ++c->snapshots_received;

    for (uint16_t i = 0; i < h.count; ++i) {
        net_snapshot_entry e;
        if (!net_snapshot_entry_read(r, &e)) return;
        if (!cw_world_is_valid(w, e.id)) continue;   // its spawn is still on the way
        net_sample s = {h.tick, e.position, e.yaw, e.pitch};
        replica_push(replica_of(c, e.id), &s);
        if (cw_comp_has(w, e.id, g->c_player)) {
            Player *p = CW_GET(w, e.id, g->c_player, Player);
            p->yaw = e.yaw;
            p->pitch = e.pitch;
        }
    }
}

static void on_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    net_client *c = (net_client *)user;
    game *g = c->replica;
    cw_world *w = &g->world;
    switch (type) {
    case MSG_WELCOME: {
        net_welcome m;
        if (!net_welcome_read(&payload, &m) || c->state != NC_CONNECTING) return;
        if (m.tick_hz != GAME_TICK_HZ) {
            fail(c, "the server ticks at a different rate");
            return;
        }
        c->player = m.player;
        c->state = NC_LOADING;
        break;
    }
    case MSG_REJECT: {
        uint8_t reason = 0;
        cw_reader_u8(&payload, &reason);
        fail(c, reason == REJECT_VERSION ? "the server speaks another protocol version"
             : reason == REJECT_FULL ? "the server is full" : "the server refused");
        break;
    }
    case MSG_SPAWN: {
        uint16_t count = 0;
        cw_reader_u16(&payload, &count);
        for (uint16_t i = 0; i < count; ++i) {
            cw_entity_id id;
            if (!net_entity_read(&payload, g, &id)) {
                fail(c, "a spawn could not be decoded");
                return;
            }
        }
        break;
    }
    case MSG_DESPAWN: {
        uint16_t count = 0;
        cw_reader_u16(&payload, &count);
        for (uint16_t i = 0; i < count; ++i) {
            cw_entity_id id = CW_INVALID_ENTITY;
            cw_reader_i64(&payload, &id);
            if (cw_reader_ok(&payload)) cw_world_mark_destroy(w, id);
        }
        break;
    }
    case MSG_BASELINE_END:
        if (c->state == NC_LOADING) c->state = NC_CONNECTED;
        break;
    case MSG_SNAPSHOT:
        if (c->state == NC_LOADING || c->state == NC_CONNECTED) on_snapshot(c, &payload, net_seconds());
        break;
    case MSG_DISCONNECT:
        fail(c, "the server closed the connection");
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// The client
// ---------------------------------------------------------------------------
bool net_client_init(net_client *c, game *g, const char *address, float interp_delay) {
    memset(c, 0, sizeof(*c));
    if (!net_address_parse(address, NET_DEFAULT_PORT, &c->server)) return false;
    c->socket = net_socket_open(0);
    if (!c->socket) return false;
    c->replica = g;
    c->interp_delay = interp_delay > 0.0f ? interp_delay : NET_INTERP_DELAY;
    c->state = NC_CONNECTING;
    c->player = CW_INVALID_ENTITY;
    c->started = net_seconds();
    c->last_connect_sent = -1.0;
    c->nonce = (uint32_t)(c->started * 1e6) ^ 0xa5a5a5a5u;

    cw_world *w = &g->world;
    c->c_replica = cw_comp_block(w, 1);
    cw_comp_register(w, c->c_replica, sizeof(Replica), alignof(Replica), "Replica", NULL);
    cw_comp_set_transient(w, c->c_replica, true);
    net_connection_init(&c->connection, c->socket, c->server, 0, on_message, c);
    return true;
}

void net_client_destroy(net_client *c) {
    if (c->state != NC_DISCONNECTED) net_client_disconnect(c);
    net_connection_destroy(&c->connection);
    net_socket_close(c->socket);
    memset(c, 0, sizeof(*c));
}

void net_client_disconnect(net_client *c) {
    if (c->state == NC_DISCONNECTED) return;
    if (c->state != NC_CONNECTING) {
        net_connection_send(&c->connection, MSG_DISCONNECT, false, NULL, 0);
        net_connection_flush(&c->connection, net_seconds(), false);
    }
    fail(c, "disconnected");
}

static void send_connect(net_client *c, double now) {
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    cw_writer_u32(&w, NET_PROTOCOL_VERSION);
    cw_writer_u32(&w, c->nonce);
    net_connection_send(&c->connection, MSG_CONNECT, false, c->scratch, w.size);
    c->last_connect_sent = now;
}

void net_client_update(net_client *c, double now) {
    if (c->state == NC_DISCONNECTED) return;
    for (;;) {
        net_address from;
        int size = net_socket_recv(c->socket, &from, c->scratch, sizeof(c->scratch));
        if (size < 0) {
            fail(c, "the socket failed");
            return;
        }
        if (size == 0) break;
        if (!net_address_equal(from, c->server)) continue;
        if (c->state == NC_CONNECTING) {
            // No session yet: adopt whatever the server stamped on this packet, and
            // keep it only if it turned out to carry our welcome.
            uint32_t session;
            if (!net_packet_peek(c->scratch, (size_t)size, &session)) continue;
            c->connection.session = session;
            net_connection_receive(&c->connection, c->scratch, (size_t)size, now);
            if (c->state == NC_CONNECTING) c->connection.session = 0;
        } else {
            net_connection_receive(&c->connection, c->scratch, (size_t)size, now);
        }
        if (c->state == NC_DISCONNECTED) return;
    }

    if (c->state == NC_CONNECTING) {
        if (now - c->started > NET_CONNECT_TIMEOUT) {
            fail(c, "no answer from the server");
            return;
        }
        if (now - c->last_connect_sent >= NET_CONNECT_RETRY) send_connect(c, now);
    } else if (now - c->connection.last_received > NET_TIMEOUT) {
        fail(c, "the server stopped answering");
        return;
    }
    net_connection_flush(&c->connection, now, now - c->connection.last_sent > NET_KEEPALIVE);
}

void net_client_send_input(net_client *c, const PlayerInput *in, double now) {
    bool jumped = c->has_pending && c->pending.jump;
    c->pending = *in;
    c->pending.jump = in->jump || jumped;
    c->has_pending = true;
    if (c->state != NC_LOADING && c->state != NC_CONNECTED) return;
    if (now - c->last_input_sent < 1.0 / GAME_TICK_HZ) return;

    if (c->history_count == NET_INPUT_REDUNDANCY) {
        memmove(c->history, c->history + 1, (NET_INPUT_REDUNDANCY - 1) * sizeof(PlayerInput));
        --c->history_count;
    }
    c->history[c->history_count++] = c->pending;
    c->has_pending = false;
    ++c->input_seq;
    c->input_sent_at[c->input_seq % 64] = now;
    c->last_input_sent = now;

    net_input_message m = {};
    m.seq = c->input_seq;
    m.count = (uint32_t)c->history_count;
    memcpy(m.inputs, c->history, (size_t)c->history_count * sizeof(PlayerInput));
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    net_input_write(&w, &m);
    net_connection_send(&c->connection, MSG_INPUT, false, c->scratch, w.size);
}

double net_client_server_tick(const net_client *c, double now) {
    return c->has_offset ? now * GAME_TICK_HZ + c->tick_offset : 0.0;
}

void net_client_resolve(net_client *c, double now) {
    if (!c->has_offset) return;
    game *g = c->replica;
    cw_world *w = &g->world;
    double render_tick = net_client_server_tick(c, now) - (double)c->interp_delay * GAME_TICK_HZ;
    cw_component_id needed[] = {g->c_transform, c->c_replica};
    cw_query replicas = cw_query_iter(w, cw_mask(needed, 2));
    cw_entity_id id;
    while (cw_query_next(&replicas, &id)) {
        const Replica *r = CW_GET(w, id, c->c_replica, Replica);
        if (!r->count) continue;
        Transform *t = CW_GET(w, id, g->c_transform, Transform);
        t->position = t->previous = replica_position(r, render_tick);
    }
}

bool net_client_entity_visible(const net_client *c, cw_entity_id id) {
    cw_world *w = &c->replica->world;
    if (!cw_world_is_valid(w, id)) return false;
    if (!cw_comp_has(w, id, c->c_replica)) {
        // Never in a snapshot: static scenery is visible, a mover is still on its way.
        return !cw_comp_has(w, id, c->replica->c_player) && !cw_comp_has(w, id, c->replica->c_ship);
    }
    const Replica *r = CW_GET(w, id, c->c_replica, Replica);
    return r->count > 0 && c->latest_tick - r->last_seen_tick < GAME_TICK_HZ;
}
