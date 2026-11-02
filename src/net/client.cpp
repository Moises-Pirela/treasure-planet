#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Samples of other players
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

// A sample's position in world space, given where its ship is now (or was).
static v3 sample_world(const net_client *c, const net_sample *s, bool previous) {
    if (s->aboard == CW_INVALID_ENTITY) return s->position;
    game *g = c->replica;
    if (!cw_comp_has(&g->world, s->aboard, g->c_transform)) return s->position;
    const Transform *ship = CW_GET(&g->world, s->aboard, g->c_transform, Transform);
    return v3_add(previous ? ship->previous : ship->position, s->position);
}

// Where the samples put the player at `tick`, placed against the ship's position
// or its previous one, so a deck and those on it interpolate together.
static v3 replica_position(const net_client *c, const Replica *r, double tick, bool previous) {
    const net_sample *newest = sample_at(r, r->count - 1);
    if (r->count == 1 || tick >= (double)newest->tick) return sample_world(c, newest, previous);
    const net_sample *oldest = sample_at(r, 0);
    if (tick <= (double)oldest->tick) return sample_world(c, oldest, previous);
    for (int i = r->count - 2; i >= 0; --i) {
        const net_sample *a = sample_at(r, i), *b = sample_at(r, i + 1);
        if ((double)a->tick <= tick) {
            float t = (float)((tick - (double)a->tick) / (double)(b->tick - a->tick));
            return v3_lerp(sample_world(c, a, previous), sample_world(c, b, previous), t);
        }
    }
    return sample_world(c, newest, previous);
}

// ---------------------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------------------
static void fail(net_client *c, const char *why) {
    c->state = NC_DISCONNECTED;
    c->failure = why;
}

static predicted_input *history_of(net_client *c, uint32_t seq) {
    predicted_input *h = &c->history[seq % NET_PREDICTION_HISTORY];
    return h->seq == seq ? h : NULL;
}

// The server's word on our player as of tick `tick`, with input `applied` in it.
static void reconcile(net_client *c, const snapshot_entry *e, uint32_t tick, uint32_t applied) {
    game *g = c->replica;
    cw_world *w = &g->world;
    if (!cw_world_is_valid(w, c->player) || !c->clock_started) return;
    Transform *t = CW_GET(w, c->player, g->c_transform, Transform);
    Player *p = CW_GET(w, c->player, g->c_player, Player);

    // The server's position in world space: ships are where the timetable had them
    // at that tick, which we can compute without having been there.
    v3 server_position = snapshot_position(e);
    if (e->aboard != CW_INVALID_ENTITY && cw_comp_has(w, e->aboard, g->c_ship)) {
        Ship ship = *CW_GET(w, e->aboard, g->c_ship, Ship);
        server_position = v3_add(game_ship_at_time(&ship, game_time_of_tick(g, tick)), server_position);
    }

    v3 before = t->position;
    uint64_t tick_before = w->tick;
    PlayerInput current = *CW_GET(w, c->player, g->c_input, PlayerInput);

    t->position = t->previous = server_position;
    p->velocity = snapshot_velocity(e);
    p->grounded = (e->flags & SNAP_GROUNDED) != 0;
    w->tick = tick;
    game_place_ships(g);   // the ships too: the replay's first step carries us by their motion from here
    if (applied != 0 && (int32_t)(c->input_seq - applied) >= 0 && c->input_seq - applied < NET_PREDICTION_HISTORY
        && history_of(c, applied)) {
        // Replay what the server has not seen yet, one tick per input, as it will.
        for (uint32_t seq = applied + 1; (int32_t)(c->input_seq - seq) >= 0; ++seq) {
            predicted_input *h = history_of(c, seq);
            if (!h) break;
            *CW_GET(w, c->player, g->c_input, PlayerInput) = h->input;
            game_replay_tick(g);
            h->position = t->position;
        }
    } else {
        // Nothing to replay against (no input applied yet, or too long ago): the
        // server's state stands and we run on from its tick plus our lead.
        w->tick = tick + NET_INITIAL_LEAD;
        game_place_ships(g);
    }
    *CW_GET(w, c->player, g->c_input, PlayerInput) = current;

    v3 delta = v3_sub(t->position, before);
    float error = v3_length(delta);
    c->last_error = error;
    if (error > 0.0f) {
        // The simulation is corrected now; the picture catches up gradually.
        c->correction = error < NET_CORRECTION_SNAP ? v3_sub(c->correction, delta) : v3_make(0, 0, 0);
        t->previous = v3_add(t->previous, delta);
    }
    if (w->tick != tick_before) {
        // The tick counter moved: the ships' previous positions are from another
        // time, so hold them still for one frame rather than sweep them across.
        ++c->resyncs;
        game_place_ships(g);
    }
}

static void send_input(net_client *c, double now) {
    net_input_message m = {};
    m.seq = c->input_seq;
    m.ack_tick = c->latest_tick;
    for (uint32_t back = NET_INPUT_REDUNDANCY; back > 0; --back) {
        uint32_t seq = c->input_seq + 1 - back;
        predicted_input *h = seq >= 1 ? history_of(c, seq) : NULL;
        if (h) m.inputs[m.count++] = h->input;
    }
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    net_input_write(&w, &m);
    net_connection_send(&c->connection, MSG_INPUT, false, c->scratch, w.size);
    c->input_sent_at[c->input_seq % 64] = now;
}

// One predicted tick: number the input, apply it, step, remember where we ended up.
static void predict_tick(net_client *c, const PlayerInput *in, double now) {
    game *g = c->replica;
    cw_world *w = &g->world;
    ++c->input_seq;
    predicted_input *h = &c->history[c->input_seq % NET_PREDICTION_HISTORY];
    h->seq = c->input_seq;
    h->input = *in;
    *CW_GET(w, c->player, g->c_input, PlayerInput) = *in;
    game_tick(g);
    h->position = CW_GET(w, c->player, g->c_transform, Transform)->position;
    send_input(c, now);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
static void on_snapshot(net_client *c, cw_reader *r, double now) {
    uint32_t tick = 0, base_tick = 0;
    if (!snapshot_peek(r, &tick, &base_tick)) return;
    if (c->has_offset && (int32_t)(tick - c->latest_tick) <= 0) return;   // arrived out of order
    const snapshot *base = base_tick ? snapshot_ring_find(&c->received, base_tick) : NULL;
    if (base_tick && !base) {
        ++c->snapshots_dropped;   // coded against one we never got; the next will use what we acked
        return;
    }
    snapshot *snap = snapshot_ring_push(&c->received, 0);
    if (!snapshot_decode(r, base, snap)) {
        snap->tick = 0;
        ++c->snapshots_dropped;
        return;
    }
    game *g = c->replica;
    cw_world *w = &g->world;

    // The server's clock as seen from here, for placing the interpolation window.
    double offset = (double)tick - now * GAME_TICK_HZ;
    if (!c->has_offset || fabs(offset - c->tick_offset) > GAME_TICK_HZ) {
        c->tick_offset = offset;
        c->has_offset = true;
    } else if (offset > c->tick_offset) {
        c->tick_offset += (offset - c->tick_offset) * 0.5;    // we were behind: catch up
    } else {
        c->tick_offset += (offset - c->tick_offset) * 0.02;   // drift back slowly
    }
    c->latest_tick = tick;
    if ((int32_t)(snap->last_input_seq - c->acked_input_seq) > 0) {
        if (c->input_seq - snap->last_input_seq < 64) {
            float sample = (float)(now - c->input_sent_at[snap->last_input_seq % 64]);
            c->rtt = c->rtt > 0.0f ? c->rtt * 0.9f + sample * 0.1f : sample;
        }
        c->acked_input_seq = snap->last_input_seq;
    }
    ++c->snapshots_received;

    for (uint16_t i = 0; i < snap->count; ++i) {
        const snapshot_entry *e = &snap->entries[i];
        if (!cw_world_is_valid(w, e->id)) continue;   // its spawn is still on the way
        if (e->id == c->player) {
            reconcile(c, e, tick, snap->last_input_seq);
            continue;
        }
        net_sample s = {tick, e->aboard, snapshot_position(e), snapshot_yaw(e), snapshot_pitch(e)};
        replica_push(replica_of(c, e->id), &s);
        if (cw_comp_has(w, e->id, g->c_player)) {
            Player *p = CW_GET(w, e->id, g->c_player, Player);
            p->yaw = s.yaw;
            p->pitch = s.pitch;
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
        c->account = m.account;
        g->universe_base = m.universe_base;
        g->spawn = m.spawn;
        g->place_count = m.place_count;
        memcpy(g->places, m.places, sizeof(g->places));
        w->tick = m.tick + NET_INITIAL_LEAD;
        c->state = NC_LOADING;
        break;
    }
    case MSG_REJECT: {
        uint8_t reason = 0;
        cw_reader_u8(&payload, &reason);
        fail(c, reason == REJECT_VERSION ? "the server speaks another protocol version"
             : reason == REJECT_FULL ? "the server is full"
             : reason == REJECT_TICKET ? "the server did not accept the ticket" : "the server refused");
        break;
    }
    case MSG_SPAWN: {
        uint16_t count = 0;
        cw_reader_u16(&payload, &count);
        for (uint16_t i = 0; i < count; ++i) {
            // Our own player is predicted: a record about it (the server repeating a
            // spawn we already had) must not overwrite the prediction.
            cw_reader peek = payload;
            cw_entity_id id = CW_INVALID_ENTITY;
            cw_reader_i64(&peek, &id);
            bool own = id == c->player && cw_world_is_valid(w, id) && c->clock_started;
            Transform t = {};
            Player p = {};
            if (own) {
                t = *CW_GET(w, id, g->c_transform, Transform);
                p = *CW_GET(w, id, g->c_player, Player);
            }
            if (!net_entity_read(&payload, g, &id)) {
                fail(c, "a spawn could not be decoded");
                return;
            }
            if (own) {
                *CW_GET(w, id, g->c_transform, Transform) = t;
                *CW_GET(w, id, g->c_player, Player) = p;
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
            if (cw_reader_ok(&payload) && id != c->player) cw_world_mark_destroy(w, id);
        }
        break;
    }
    case MSG_BASELINE_END:
        if (c->state == NC_LOADING) {
            if (!cw_world_is_valid(w, c->player)) {
                fail(c, "the server never sent our player");
                return;
            }
            // Having an input is what makes our player the one the systems move.
            PlayerInput in = {};
            cw_comp_set(w, c->player, g->c_input, &in, sizeof(in));
            game_place_ships(g);   // the baseline had them at the server's tick, not ours
            c->state = NC_CONNECTED;
        }
        break;
    case MSG_SNAPSHOT:
        if (c->state == NC_LOADING || c->state == NC_CONNECTED) on_snapshot(c, &payload, net_seconds());
        break;
    case MSG_REDIRECT: {
        net_address address;
        unsigned char ticket[NET_TICKET_SIZE];
        size_t size = 0;
        if (!net_redirect_read(&payload, &address, ticket, &size)) return;
        c->redirect_pending = true;
        c->redirect_address = address;
        memcpy(c->redirect_ticket, ticket, size);
        c->redirect_ticket_size = size;
        break;
    }
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
bool net_client_init(net_client *c, game *g, net_address server, const unsigned char *ticket, size_t ticket_size,
                     float interp_delay) {
    memset(c, 0, sizeof(*c));
    c->server = server;
    c->socket = net_socket_open(0);
    if (!c->socket) return false;
    c->replica = g;
    c->interp_delay = interp_delay > 0.0f ? interp_delay : NET_INTERP_DELAY;
    c->state = NC_CONNECTING;
    c->player = CW_INVALID_ENTITY;
    c->started = net_seconds();
    c->last_connect_sent = -1.0;
    c->nonce = (uint32_t)(c->started * 1e6) ^ 0xa5a5a5a5u;
    if (ticket && ticket_size && ticket_size <= NET_TICKET_SIZE) {
        memcpy(c->ticket, ticket, ticket_size);
        c->ticket_size = ticket_size;
    }
    c->clock = cw_clock_make(GAME_TICK_HZ);
    snapshot_ring_init(&c->received);

    cw_world *w = &g->world;
    c->c_replica = cw_comp_block(w, 1);
    cw_comp_register(w, c->c_replica, sizeof(Replica), alignof(Replica), "Replica", NULL);
    cw_comp_set_transient(w, c->c_replica, true);
    net_connection_init(&c->connection, c->socket, c->server, 0, on_message, c);
    return true;
}

bool net_client_connect(net_client *c, game *g, const char *address, float interp_delay) {
    net_address server;
    if (!net_address_parse(address, NET_DEFAULT_PORT, &server)) {
        memset(c, 0, sizeof(*c));
        return false;
    }
    return net_client_init(c, g, server, NULL, 0, interp_delay);
}

void net_client_destroy(net_client *c) {
    if (c->state != NC_DISCONNECTED) net_client_disconnect(c);
    net_connection_destroy(&c->connection);
    snapshot_ring_destroy(&c->received);
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
    net_connect_message m = {};
    m.version = NET_PROTOCOL_VERSION;
    m.nonce = c->nonce;
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    // The ticket is opaque here: it was signed by the zone that issued it, so it is
    // copied through rather than re-encoded.
    cw_writer_u32(&w, m.version);
    cw_writer_u32(&w, m.nonce);
    cw_writer_u16(&w, (uint16_t)c->ticket_size);
    cw_writer_bytes(&w, c->ticket, c->ticket_size);
    net_connection_send(&c->connection, MSG_CONNECT, false, c->scratch, w.size);
    c->last_connect_sent = now;
}

static void receive(net_client *c, double now) {
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
}

void net_client_frame(net_client *c, const PlayerInput *in, double now) {
    if (c->state == NC_DISCONNECTED) return;
    receive(c, now);
    if (c->state == NC_DISCONNECTED) return;

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

    if (c->state == NC_CONNECTED) {
        if (!c->clock_started) {
            c->clock_started = true;
            c->last_frame = now;
        }
        float frame_dt = (float)(now - c->last_frame);
        c->last_frame = now;
        int steps = cw_clock_advance(&c->clock, frame_dt);
        for (int i = 0; i < steps; ++i) predict_tick(c, in, now);

        // The correction fades by half every NET_CORRECTION_HALF_LIFE.
        float keep = powf(0.5f, frame_dt / NET_CORRECTION_HALF_LIFE);
        c->correction = v3_scale(c->correction, keep);
        if (v3_length(c->correction) < 0.0005f) c->correction = v3_make(0, 0, 0);

        // The other players, between the two samples around the render time.
        if (c->has_offset) {
            game *g = c->replica;
            cw_world *w = &g->world;
            double render_tick = now * GAME_TICK_HZ + c->tick_offset - (double)c->interp_delay * GAME_TICK_HZ;
            cw_component_id needed[] = {g->c_transform, c->c_replica};
            cw_query replicas = cw_query_iter(w, cw_mask(needed, 2));
            cw_entity_id id;
            while (cw_query_next(&replicas, &id)) {
                const Replica *r = CW_GET(w, id, c->c_replica, Replica);
                if (!r->count || id == c->player) continue;
                Transform *t = CW_GET(w, id, g->c_transform, Transform);
                t->position = replica_position(c, r, render_tick, false);
                t->previous = replica_position(c, r, render_tick, true);
            }
        }
    }
    net_connection_flush(&c->connection, now, now - c->connection.last_sent > NET_KEEPALIVE);
}

float net_client_alpha(const net_client *c) { return c->clock_started ? cw_clock_alpha(&c->clock) : 1.0f; }
v3 net_client_view_offset(const net_client *c) { return c->correction; }

bool net_client_entity_visible(const net_client *c, cw_entity_id id) {
    cw_world *w = &c->replica->world;
    if (!cw_world_is_valid(w, id)) return false;
    if (id == c->player) return true;
    if (!cw_comp_has(w, id, c->c_replica)) {
        // Never in a snapshot: scenery and ships are visible, a player is still on the way.
        return !cw_comp_has(w, id, c->replica->c_player);
    }
    const Replica *r = CW_GET(w, id, c->c_replica, Replica);
    return r->count > 0 && c->latest_tick - r->last_seen_tick < GAME_TICK_HZ;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
static bool session_open(net_session *s, game **world, net_client **link, net_address address,
                         const unsigned char *ticket, size_t ticket_size) {
    *world = (game *)malloc(sizeof(game));
    game_init(*world, CW_CLIENT);
    *link = (net_client *)malloc(sizeof(net_client));
    if (!net_client_init(*link, *world, address, ticket, ticket_size, s->interp_delay)) {
        game_destroy(*world);
        free(*world);
        free(*link);
        *world = NULL;
        *link = NULL;
        return false;
    }
    return true;
}

static void session_close(game **world, net_client **link) {
    if (*link) {
        net_client_destroy(*link);
        free(*link);
        *link = NULL;
    }
    if (*world) {
        game_destroy(*world);
        free(*world);
        *world = NULL;
    }
}

bool net_session_init(net_session *s, const char *address, float interp_delay) {
    memset(s, 0, sizeof(*s));
    s->interp_delay = interp_delay;
    net_address server;
    if (!net_address_parse(address, NET_DEFAULT_PORT, &server)) {
        s->failure = "the address could not be resolved";
        return false;
    }
    if (!session_open(s, &s->world, &s->link, server, NULL, 0)) {
        s->failure = "no socket could be opened";
        return false;
    }
    return true;
}

void net_session_destroy(net_session *s) {
    session_close(&s->next_world, &s->next_link);
    session_close(&s->world, &s->link);
    memset(s, 0, sizeof(*s));
}

net_client_state net_session_state(const net_session *s) { return s->link ? s->link->state : NC_DISCONNECTED; }

void net_session_frame(net_session *s, const PlayerInput *in, double now) {
    if (!s->link) return;
    net_client_frame(s->link, in, now);
    if (s->link->state == NC_DISCONNECTED && !s->next_link) {
        s->failure = s->link->failure;
        return;
    }

    if (s->next_link) {
        net_client_frame(s->next_link, in, now);
        if (s->next_link->state == NC_DISCONNECTED) {
            // The new zone would not have us; stay here. The old zone asks again later.
            session_close(&s->next_world, &s->next_link);
            s->link->redirect_pending = false;
        } else if (s->next_link->state == NC_CONNECTED && s->next_link->snapshots_received > 0) {
            // The new zone is ready. Bridge the two predictions with the view offset
            // so the switch does not show, then let the old zone go.
            game *old_world = s->world, *new_world = s->next_world;
            v3 old_position = cw_world_is_valid(&old_world->world, s->link->player)
                ? v3_add(CW_GET(&old_world->world, s->link->player, old_world->c_transform, Transform)->position,
                         s->link->correction)
                : v3_make(0, 0, 0);
            v3 new_position = CW_GET(&new_world->world, s->next_link->player, new_world->c_transform, Transform)->position;
            v3 delta = v3_sub(old_position, new_position);
            s->next_link->correction = v3_length(delta) < NET_CORRECTION_SNAP ? delta : v3_make(0, 0, 0);
            session_close(&s->world, &s->link);
            s->world = new_world;
            s->link = s->next_link;
            s->next_world = NULL;
            s->next_link = NULL;
            ++s->handoffs;
        }
    } else if (s->link->redirect_pending) {
        if (!session_open(s, &s->next_world, &s->next_link, s->link->redirect_address, s->link->redirect_ticket,
                          s->link->redirect_ticket_size))
            s->link->redirect_pending = false;
    }
}
