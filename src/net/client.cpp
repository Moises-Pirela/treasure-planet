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
        if (gap > 2 * GAME_TICK_HZ) r->count = 0;               // back after a long absence: no sliding over
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
// or its previous one, so a deck and those on it interpolate together. Past the
// newest sample they walk on along their velocity for a while, then stand: a far
// player is refreshed every few seconds, and this is what keeps them moving.
static v3 replica_position(const net_client *c, const Replica *r, double tick, bool previous) {
    const net_sample *newest = sample_at(r, r->count - 1);
    if (r->count == 1 || tick >= (double)newest->tick) {
        float ahead = (float)(tick - (double)newest->tick) / (float)GAME_TICK_HZ;
        if (ahead > NET_EXTRAPOLATE) ahead = NET_EXTRAPOLATE;
        if (ahead < 0.0f) ahead = 0.0f;
        v3 velocity = newest->velocity;
        velocity.y = 0.0f;   // gravity is the server's business
        return v3_add(sample_world(c, newest, previous), v3_scale(velocity, ahead));
    }
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
    if (c->lightweight) {   // the server's word, and nothing else
        CW_GET(w, c->player, g->c_transform, Transform)->position = snapshot_position(e);
        w->tick = tick;
        return;
    }
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
    if (e->hits_taken != p->hits_taken) c->hit_flash = NET_HIT_FLASH;   // the server's word: we were tagged
    p->hits_taken = e->hits_taken;
    p->hits_made = e->hits_made;
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
        if (!h) continue;
        m.view_ticks[m.count] = h->view_tick;
        m.inputs[m.count++] = h->input;
    }
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    net_input_write(&w, &m);
    net_connection_send(&c->connection, MSG_INPUT, NET_SEQUENCED, c->scratch, w.size);
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
    net_input_quantize(&h->input);   // what the wire carries is what both sides simulate
    h->view_tick = (float)c->render_tick;
    *CW_GET(w, c->player, g->c_input, PlayerInput) = h->input;
    if (!c->lightweight) game_tick(g);
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
    if (c->has_offset && fabs(offset - c->tick_offset) > GAME_TICK_HZ) c->offset_count = c->offset_next = 0;   // a jump: start over
    c->offsets[c->offset_next] = offset;
    c->offset_next = (c->offset_next + 1) % NET_OFFSET_WINDOW;
    if (c->offset_count < NET_OFFSET_WINDOW) ++c->offset_count;
    c->tick_offset = c->offsets[0];
    for (int i = 1; i < c->offset_count; ++i)
        if (c->offsets[i] > c->tick_offset) c->tick_offset = c->offsets[i];
    c->has_offset = true;
    if (c->latest_tick) {
        float interval = (float)(tick - c->latest_tick) / (float)GAME_TICK_HZ;
        c->snapshot_interval = c->snapshot_interval > 0.0f ? c->snapshot_interval * 0.8f + interval * 0.2f : interval;
    }
    c->latest_tick = tick;

    // Our tick rate follows the server's: too many inputs queued there means our
    // clock runs fast, too few means slow.
    c->input_lead = snap->input_lead;
    float excess = (float)snap->input_lead - (float)c->input_target;
    c->rate = 1.0f - NET_RATE_GAIN * excess;
    if (c->rate < 1.0f - NET_RATE_LIMIT) c->rate = 1.0f - NET_RATE_LIMIT;
    if (c->rate > 1.0f + NET_RATE_LIMIT) c->rate = 1.0f + NET_RATE_LIMIT;
    c->clock.dt = 1.0f / ((float)GAME_TICK_HZ * c->rate);
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
        net_sample s = {tick, e->aboard, snapshot_position(e), snapshot_velocity(e), snapshot_yaw(e), snapshot_pitch(e)};
        Replica *replica = replica_of(c, e->id);
        replica_push(replica, &s);
        if (cw_comp_has(w, e->id, g->c_player)) {
            Player *p = CW_GET(w, e->id, g->c_player, Player);
            p->yaw = s.yaw;
            p->pitch = s.pitch;
            if (e->hits_taken != p->hits_taken) replica->flash = NET_HIT_FLASH;
            p->hits_taken = e->hits_taken;
            p->hits_made = e->hits_made;
        }
    }
}

static void send_connect(net_client *c, double now);
static void on_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    net_client *c = (net_client *)user;
    game *g = c->replica;
    cw_world *w = &g->world;
    switch (type) {
    case MSG_KEYS: {
        net_keys_message m;
        if (!net_keys_read(&payload, &m) || c->state != NC_CONNECTING || c->connection.encrypted) return;
        if (memcmp(m.client_public, c->public_key, CRYPTO_KEY_SIZE) != 0) return;   // an older attempt's
        uint8_t shared[CRYPTO_KEY_SIZE], to_server[CRYPTO_KEY_SIZE], to_client[CRYPTO_KEY_SIZE];
        crypto_x25519(shared, c->secret, m.server_public);
        crypto_session_keys(shared, c->public_key, m.server_public, to_server, to_client);
        c->connection.session = m.session;
        net_connection_set_keys(&c->connection, to_server, to_client);
        memset(shared, 0, sizeof(shared));
        memset(c->secret, 0, sizeof(c->secret));
        break;
    }
    case MSG_KEEPALIVE:
        if (c->state == NC_CONNECTING) ++c->punches_received;
        break;
    case MSG_CHALLENGE: {
        // The server wants proof that we receive at our address: the connect
        // again, with the challenge. At once, not at the next retry.
        uint32_t nonce = 0;
        uint64_t challenge = 0;
        cw_reader_u32(&payload, &nonce);
        cw_reader_u64(&payload, &challenge);
        if (!cw_reader_ok(&payload) || c->state != NC_CONNECTING || nonce != c->nonce || c->connection.encrypted) return;
        c->challenge = challenge;
        ++c->challenges_received;
        send_connect(c, net_seconds());
        break;
    }
    case MSG_WELCOME: {
        net_welcome m;
        if (!net_welcome_read(&payload, &m) || c->state != NC_CONNECTING) return;
        if (m.tick_hz != GAME_TICK_HZ) {
            fail(c, "the server ticks at a different rate");
            return;
        }
        c->player = m.player;
        c->account = m.account;
        c->input_target = m.input_target;
        memcpy(c->world_name, m.world, sizeof(c->world_name));
        memcpy(c->zone, m.zone, sizeof(c->zone));
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
             : reason == REJECT_TICKET ? "the server did not accept the ticket"
             : reason == REJECT_NO_WORLD ? "the server hosts instances only"
             : reason == REJECT_IDENTITY ? "the server did not accept who we said we are"
             : reason == REJECT_TOKEN ? "the server admits only those the directory sent" : "the server refused");
        break;
    }
    case MSG_SPAWN:
    case MSG_UPDATE: {
        // An update carries only the components that changed, of an entity we have;
        // a spawn the whole record, of one we may not.
        uint16_t count = 0;
        cw_reader_u16(&payload, &count);
        for (uint16_t i = 0; i < count; ++i) {
            // Our own player is predicted: a record about it (the server repeating a
            // spawn we already had, new gear) must not overwrite the prediction.
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
            if (!net_entity_read_masked(&payload, g, &id, type == MSG_SPAWN)) {
                fail(c, type == MSG_SPAWN ? "a spawn could not be decoded" : "an update could not be decoded");
                return;
            }
            if (own) {
                *CW_GET(w, id, g->c_transform, Transform) = t;
                *CW_GET(w, id, g->c_player, Player) = p;
            }
            if (type == MSG_UPDATE) ++c->updates_received;
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
        if (address.ip == 0) {   // "where you are": the same server, another world
            address.ip = c->server.ip;
            if (address.port == 0) address.port = c->server.port;
        }
        c->redirect_pending = true;
        c->redirect_address = address;
        memcpy(c->redirect_ticket, ticket, size);
        c->redirect_ticket_size = size;
        break;
    }
    case MSG_PARTY: {
        party_view view;
        if (!party_view_read(&payload, &view)) return;
        c->party = view;
        ++c->party_updates;
        break;
    }
    case MSG_CHARACTER: {
        character gear;
        if (!character_read(&payload, &gear)) return;
        // Reliable but unordered: an older copy overtaken by a newer one is ignored.
        if (c->gear_known && gear.version < c->gear.version) return;
        // Whatever is here now and was not before is what we just picked up.
        if (c->gear_known) {
            for (int i = 0; i < SLOT_COUNT + STASH_SIZE; ++i) {
                const item *it = i < SLOT_COUNT ? &gear.equipment[i] : &gear.stash[i - SLOT_COUNT];
                if (!item_exists(it)) continue;
                bool had = false;
                for (int k = 0; k < SLOT_COUNT + STASH_SIZE && !had; ++k) {
                    const item *old = k < SLOT_COUNT ? &c->gear.equipment[k] : &c->gear.stash[k - SLOT_COUNT];
                    had = old->id == it->id;
                }
                if (!had) {
                    c->last_pickup = *it;
                    c->pickup_flash = 3.0f;
                }
            }
        }
        c->gear = gear;
        c->gear_known = true;
        ++c->gear_updates;
        break;
    }
    case MSG_DISCONNECT:
        fail(c, "the server closed the connection");
        break;
    default:
        break;
    }
}

void net_client_party_request(net_client *c, int op, const char *target_name) {
    if (c->state != NC_CONNECTED) return;
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    cw_writer_u8(&w, (uint8_t)op);
    uint8_t length = (uint8_t)(target_name ? strnlen(target_name, ACCOUNT_NAME_CAPACITY - 1) : 0);
    cw_writer_u8(&w, length);
    cw_writer_bytes(&w, target_name ? target_name : "", length);
    net_connection_send(&c->connection, MSG_PARTY_REQUEST, NET_RELIABLE_ORDERED, c->scratch, w.size);
}

void net_client_equip(net_client *c, int index) {
    if (c->state != NC_CONNECTED || index < 0 || index >= STASH_SIZE) return;
    uint8_t payload = (uint8_t)index;
    net_connection_send(&c->connection, MSG_EQUIP, NET_RELIABLE_ORDERED, &payload, 1);
}

bool net_client_loot_in_view(const net_client *c, float reach, item *out) {
    game *g = c->replica;
    cw_world *w = &g->world;
    if (!cw_world_is_valid(w, c->player)) return false;
    v3 eye = v3_add(CW_GET(w, c->player, g->c_transform, Transform)->position, v3_make(0, PLAYER_EYE, 0));
    const Player *self = CW_GET(w, c->player, g->c_player, Player);
    v3 direction = look_direction(self->yaw, self->pitch);
    float best = reach;
    bool found = false;
    cw_query loot = cw_query_begin(g->q_loot);
    cw_entity_id id;
    while (cw_query_next(&loot, &id)) {
        v3 at = CW_GET(w, id, g->c_transform, Transform)->position;
        v3 to = v3_sub(at, eye);
        float along = v3_dot(to, direction);
        if (along < 0.0f || along > reach) continue;
        float off = v3_length(v3_sub(to, v3_scale(direction, along)));
        if (off < 0.6f && along < best) {
            best = along;
            *out = CW_GET(w, id, g->c_loot, Loot)->it;
            found = true;
        }
    }
    return found;
}

bool net_client_player_in_view(const net_client *c, float reach, char name[ACCOUNT_NAME_CAPACITY]) {
    game *g = c->replica;
    cw_world *w = &g->world;
    if (!cw_world_is_valid(w, c->player)) return false;
    const Transform *me = CW_GET(w, c->player, g->c_transform, Transform);
    const Player *self = CW_GET(w, c->player, g->c_player, Player);
    v3 eye = v3_add(me->position, v3_make(0, PLAYER_EYE, 0));
    v3 direction = look_direction(self->yaw, self->pitch);
    cw_entity_id best = CW_INVALID_ENTITY;
    float best_distance = reach;
    cw_query players = cw_query_begin(g->q_all_players);
    cw_entity_id id;
    while (cw_query_next(&players, &id)) {
        if (id == c->player || !net_client_entity_visible(c, id)) continue;
        v3 feet = CW_GET(w, id, g->c_transform, Transform)->position;
        float distance = v3_length(v3_sub(feet, me->position));
        if (distance < best_distance && game_swing_hits(eye, direction, feet)) {
            best_distance = distance;
            best = id;
        }
    }
    if (best == CW_INVALID_ENTITY) return false;
    strncpy(name, CW_GET(w, best, g->c_player, Player)->name, ACCOUNT_NAME_CAPACITY - 1);
    name[ACCOUNT_NAME_CAPACITY - 1] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// The client
// ---------------------------------------------------------------------------
bool net_client_init(net_client *c, game *g, net_address server, const unsigned char *ticket, size_t ticket_size,
                     float interp_delay) {
    net_client_options o = {};
    o.server = server;
    o.ticket = ticket;
    o.ticket_size = ticket_size;
    o.interp_delay = interp_delay;
    return net_client_init_options(c, g, &o);
}

net_identity net_identity_local(const char *name) {
    net_identity id = {};
    char clean[ACCOUNT_NAME_CAPACITY];
    if (!account_clean_name(name, clean)) {
        uint16_t tag = 0;
        crypto_random(&tag, sizeof(tag));   // two sessions made up in the same instant must not be one account
        snprintf(clean, sizeof(clean), "player%04x", (unsigned)tag);
    }
    snprintf(id.platform, sizeof(id.platform), "local:%s", clean);
    memcpy(id.name, clean, sizeof(id.name));
    return id;
}

bool net_client_init_options(net_client *c, game *g, const net_client_options *o) {
    memset(c, 0, sizeof(*c));
    c->server = o->server;
    c->identity = o->identity ? *o->identity : net_identity_local(NULL);
    c->socket = o->socket ? o->socket : net_socket_open(0);
    if (!c->socket) return false;
    c->introducer = o->introducer;
    c->lightweight = o->lightweight;
    const unsigned char *ticket = o->ticket;
    size_t ticket_size = o->ticket_size;
    float interp_delay = o->interp_delay;
    c->replica = g;
    c->interp_delay = interp_delay > 0.0f ? interp_delay : NET_INTERP_DELAY;
    c->state = NC_CONNECTING;
    c->player = CW_INVALID_ENTITY;
    c->started = net_seconds();
    c->last_connect_sent = -1.0;
    c->nonce = (uint32_t)(c->started * 1e6) ^ 0xa5a5a5a5u;
    if (o->token && o->token_size && o->token_size <= NET_TOKEN_SIZE) {
        memcpy(c->token, o->token, o->token_size);
        c->token_size = o->token_size;
    }
    if (ticket && ticket_size && ticket_size <= NET_TICKET_SIZE) {
        memcpy(c->ticket, ticket, ticket_size);
        c->ticket_size = ticket_size;
    }
    c->clock = cw_clock_make(GAME_TICK_HZ);
    c->rate = 1.0f;
    snapshot_ring_init(&c->received);
    if (!crypto_keypair(c->secret, c->public_key)) {
        net_socket_close(c->socket);
        memset(c, 0, sizeof(*c));
        return false;
    }

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
    if (c->state != NC_DISCONNECTED) net_client_disconnect(c, LEAVE_QUIT);
    net_connection_destroy(&c->connection);
    snapshot_ring_destroy(&c->received);
    net_socket_close(c->socket);
    memset(c, 0, sizeof(*c));
}

void net_client_disconnect(net_client *c, int reason) {
    if (c->state == NC_DISCONNECTED) return;
    if (c->state != NC_CONNECTING) {
        // Unreliable by nature (we are leaving), so said three times: a lost notice
        // would leave a frozen ghost on the server until its timeout.
        uint8_t why = (uint8_t)reason;
        for (int i = 0; i < 3; ++i) {
            net_connection_send(&c->connection, MSG_DISCONNECT, NET_UNRELIABLE, &why, 1);
            net_connection_flush(&c->connection, net_seconds(), false);
        }
    }
    fail(c, "disconnected");
}

void net_client_handoff_failed(net_client *c) {
    if (!c->redirect_pending) return;
    c->redirect_pending = false;
    if (c->state == NC_CONNECTED) net_connection_send(&c->connection, MSG_HANDOFF_FAILED, NET_RELIABLE, NULL, 0);
}

// Asks the directory to have the server send us a packet, so that a NAT in front
// of the server lets ours through.
static void ask_introduction(net_client *c, double now) {
    if (!c->introducer.ip || c->connection.encrypted || now - c->last_introduction_asked < DIRECTORY_QUERY_EVERY) return;
    c->last_introduction_asked = now;
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    directory_query q = {};
    q.introduce = true;
    q.introduce_to = c->server;
    memcpy(q.region, c->identity.region, sizeof(q.region));
    q.account = account_id_for(c->identity.platform);
    directory_query_write(&w, &q);
    net_connection tmp;
    net_connection_init(&tmp, c->socket, c->introducer, 0, NULL, NULL);
    net_connection_send(&tmp, MSG_DIR_QUERY, NET_UNRELIABLE, c->scratch, w.size);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

static void send_connect(net_client *c, double now) {
    // Sent in the clear on session 0 until MSG_KEYS arrives; after that the
    // connection is encrypted and repeating this would be pointless. The ticket
    // and the token are opaque here: signed by those who hold the secret, copied
    // through rather than re-encoded.
    if (c->connection.encrypted) return;
    net_connect_message m = {};
    m.version = NET_PROTOCOL_VERSION;
    m.nonce = c->nonce;
    memcpy(m.public_key, c->public_key, CRYPTO_KEY_SIZE);
    memcpy(m.platform, c->identity.platform, sizeof(m.platform));
    memcpy(m.name, c->identity.name, sizeof(m.name));
    memcpy(m.region, c->identity.region, sizeof(m.region));
    memcpy(m.proof, c->identity.proof, c->identity.proof_size);
    m.proof_size = c->identity.proof_size;
    m.challenge = c->challenge;
    cw_writer w;
    cw_writer_init_fixed(&w, c->scratch, sizeof(c->scratch));
    net_connect_write(&w, &m, c->ticket, c->ticket_size, c->token, c->token_size);
    net_connection_send(&c->connection, MSG_CONNECT, NET_UNRELIABLE, c->scratch, w.size);
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
        uint32_t session;
        if (!net_packet_peek(c->scratch, (size_t)size, &session)) continue;
        if (session == 0 && c->connection.session != 0) {
            // The handshake in the clear (keys, a rejection, a NAT punch) once we
            // already hold a session: read it on the side, without touching ours.
            net_connection clear;
            net_connection_init(&clear, c->socket, c->server, 0, on_message, c);
            net_connection_receive(&clear, c->scratch, (size_t)size, now);
            net_connection_destroy(&clear);
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
        ask_introduction(c, now);
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
        // Drawn between two samples: the delay is the longer of the configured one
        // and one and a half snapshot intervals, whatever rate the server manages.
        float delay = c->interp_delay > 1.5f * c->snapshot_interval ? c->interp_delay : 1.5f * c->snapshot_interval;
        if (c->has_offset) c->render_tick = now * GAME_TICK_HZ + c->tick_offset - (double)delay * GAME_TICK_HZ;
        int steps = cw_clock_advance(&c->clock, frame_dt);
        PlayerInput applied = *in;
        if (c->redirect_pending) {
            // Frozen for the hand-off, like the server has us: the other zone picks
            // up exactly where the ticket says, and nothing rubber-bands meanwhile.
            applied.forward = applied.strafe = 0.0f;
            applied.jump = false;
            applied.act = false;
        }
        // A swing is an edge, seen in one frame; ticks are rarer than frames, so it
        // is kept until the next tick and applied to that one alone.
        if (applied.act) c->act_latched = true;
        for (int i = 0; i < steps; ++i) {
            applied.act = c->act_latched;
            c->act_latched = false;
            predict_tick(c, &applied, now);
        }

        // The correction fades by half every NET_CORRECTION_HALF_LIFE; flashes run down.
        float keep = powf(0.5f, frame_dt / NET_CORRECTION_HALF_LIFE);
        c->correction = v3_scale(c->correction, keep);
        if (v3_length(c->correction) < 0.0005f) c->correction = v3_make(0, 0, 0);
        if (c->hit_flash > 0.0f) c->hit_flash -= frame_dt;
        if (c->pickup_flash > 0.0f) c->pickup_flash -= frame_dt;

        // The other players, between the two samples around the render time.
        if (c->has_offset && !c->lightweight) {
            game *g = c->replica;
            cw_world *w = &g->world;
            double render_tick = c->render_tick;
            cw_component_id needed[] = {g->c_transform, c->c_replica};
            cw_query replicas = cw_query_iter(w, cw_mask(needed, 2));
            cw_entity_id id;
            while (cw_query_next(&replicas, &id)) {
                Replica *r = CW_GET(w, id, c->c_replica, Replica);
                if (r->flash > 0.0f) r->flash -= frame_dt;
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
    return r->count > 0 && c->latest_tick - r->last_seen_tick < NET_UNSEEN_HIDE;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
static bool session_open(net_session *s, game **world, net_client **link, net_address address,
                         const unsigned char *ticket, size_t ticket_size, net_socket *socket) {
    *world = (game *)malloc(sizeof(game));
    game_init(*world, CW_CLIENT);
    *link = (net_client *)malloc(sizeof(net_client));
    net_client_options o = {};
    o.server = address;
    o.ticket = ticket;
    o.ticket_size = ticket_size;
    o.token = s->token_size ? s->token : NULL;   // a ticket takes precedence at the server; both may travel
    o.token_size = s->token_size;
    o.interp_delay = s->interp_delay;
    o.socket = socket;
    o.introducer = s->directory;
    o.identity = &s->identity;
    o.lightweight = s->lightweight;
    if (!net_client_init_options(*link, *world, &o)) {
        game_destroy(*world);
        free(*world);
        free(*link);
        *world = NULL;
        *link = NULL;
        return false;
    }
    return true;
}

// A lost connection is worth trying again; a refusal or a deliberate close is not.
static bool lost(const net_client *c) {
    return c->failure && (strcmp(c->failure, "the server stopped answering") == 0 || strcmp(c->failure, "the socket failed") == 0);
}

static void session_close(game **world, net_client **link, int reason) {
    if (*link) {
        net_client_disconnect(*link, reason);
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

bool net_session_init(net_session *s, const char *address, float interp_delay, const net_identity *identity) {
    memset(s, 0, sizeof(*s));
    s->interp_delay = interp_delay;
    s->identity = identity ? *identity : net_identity_local(NULL);
    net_address server;
    if (!net_address_parse(address, NET_DEFAULT_PORT, &server)) {
        s->failure = "the address could not be resolved";
        return false;
    }
    if (!session_open(s, &s->world, &s->link, server, NULL, 0, NULL)) {
        s->failure = "no socket could be opened";
        return false;
    }
    return true;
}

bool net_session_init_directory(net_session *s, const char *address, const char *place, float interp_delay,
                                const net_identity *identity) {
    memset(s, 0, sizeof(*s));
    s->interp_delay = interp_delay;
    s->identity = identity ? *identity : net_identity_local(NULL);
    if (!net_address_parse(address, NET_DIRECTORY_PORT, &s->directory)) {
        s->failure = "the directory's address could not be resolved";
        return false;
    }
    strncpy(s->place, place && *place ? place : "hub", NET_ZONE_NAME - 1);
    s->resolver = net_socket_open(0);
    if (!s->resolver) {
        s->failure = "no socket could be opened";
        return false;
    }
    s->resolve_started = net_seconds();
    s->last_query = -1.0;
    return true;
}

void net_session_destroy(net_session *s) {
    session_close(&s->next_world, &s->next_link, LEAVE_QUIT);
    session_close(&s->world, &s->link, LEAVE_QUIT);
    net_socket_close(s->resolver);
    memset(s, 0, sizeof(*s));
}

struct resolve_answer {
    net_session *s;
    bool got_table;
};
static void on_resolve_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    resolve_answer *a = (resolve_answer *)user;
    net_session *s = a->s;
    if (type == MSG_DIR_TOKEN) {
        size_t size = 0;
        if (!directory_token_read(&payload, s->token, sizeof(s->token), &size)) return;
        s->token_size = size;
        s->token_at = net_seconds();
        s->token_refused = size == 0;
        return;
    }
    if (type != MSG_DIR_TABLE) return;
    net_zones_init(&s->table);
    if (directory_table_read(&payload, &s->table, NULL)) a->got_table = true;   // unsigned: we hold no secret
}

// Asks the directory until it answers, then connects to the owner of the place
// from the same socket, which the directory has just had that server punch towards.
static void session_resolve(net_session *s, double now) {
    if (now - s->resolve_started > NET_CONNECT_TIMEOUT) {
        net_socket_close(s->resolver);
        s->resolver = NULL;
        if (s->reconnect_zone[0]) {   // one try fewer; the next frame asks again, or gives up
            s->reconnect_zone[0] = 0;
            ++s->reconnect_failures;
        } else {
            s->failure = "the directory did not answer";
        }
        return;
    }
    if (now - s->last_query >= DIRECTORY_QUERY_EVERY) {
        s->last_query = now;
        unsigned char payload[96];
        cw_writer w;
        cw_writer_init_fixed(&w, payload, sizeof(payload));
        directory_query q = {};
        if (s->reconnect_zone[0]) memcpy(q.zone, s->reconnect_zone, sizeof(q.zone));   // a token for where we were
        else memcpy(q.place, s->place, sizeof(q.place));
        memcpy(q.region, s->identity.region, sizeof(q.region));
        q.account = account_id_for(s->identity.platform);
        // Who we are, for the token: the directory verifies it and the zone takes its word.
        q.has_identity = true;
        memcpy(q.platform, s->identity.platform, sizeof(q.platform));
        memcpy(q.name, s->identity.name, sizeof(q.name));
        memcpy(q.proof, s->identity.proof, s->identity.proof_size);
        q.proof_size = s->identity.proof_size;
        s->token_size = 0;   // whatever comes back is for this query
        directory_query_write(&w, &q);
        net_connection tmp;
        net_connection_init(&tmp, s->resolver, s->directory, 0, NULL, NULL);
        net_connection_send(&tmp, MSG_DIR_QUERY, NET_UNRELIABLE, payload, w.size);
        net_connection_flush(&tmp, now, false);
        net_connection_destroy(&tmp);
    }
    unsigned char packet[NET_MAX_PACKET];
    for (;;) {
        net_address from;
        int size = net_socket_recv(s->resolver, &from, packet, sizeof(packet));
        if (size <= 0) break;
        if (!net_address_equal(from, s->directory)) continue;
        resolve_answer answer = {s, false};
        net_connection tmp;
        net_connection_init(&tmp, s->resolver, from, 0, on_resolve_message, &answer);
        net_connection_receive(&tmp, packet, (size_t)size, now);
        net_connection_destroy(&tmp);
        if (s->token_refused) {
            s->failure = "the directory did not accept who we said we are";
            net_socket_close(s->resolver);
            s->resolver = NULL;
            return;
        }
        // Both halves of the answer (the table, then the token) must be in.
        if (!s->token_size || !s->table.count) continue;
        // The directory put the shard it chose for us first; after a lost
        // connection it is the zone we were on, at the address we had.
        net_address to;
        if (s->reconnect_zone[0]) {
            if (net_zones_find(&s->table, s->reconnect_zone) < 0) continue;   // gone: keep asking until the timeout
            to = s->reconnect_to;
        } else {
            int owner = net_zones_owner(&s->table, s->place);
            if (owner < 0) continue;   // not yet: a zone may still be registering
            to = s->table.zones[owner].address;
        }
        net_socket *socket = s->resolver;
        s->resolver = NULL;
        bool reconnecting = s->reconnect_zone[0] != 0;
        s->reconnect_zone[0] = 0;
        if (!session_open(s, reconnecting ? &s->next_world : &s->world, reconnecting ? &s->next_link : &s->link, to, NULL, 0, socket)) {
            s->failure = "no socket could be opened";
            net_socket_close(socket);
        }
        return;
    }
}

// Starts resolving a fresh token for the zone the lost link was on.
static bool session_reresolve(net_session *s, double now) {
    s->resolver = net_socket_open(0);
    if (!s->resolver) return false;
    memcpy(s->reconnect_zone, s->link->zone, sizeof(s->reconnect_zone));
    s->reconnect_to = s->link->server;
    s->resolve_started = now;
    s->last_query = -1.0;
    return true;
}

net_client_state net_session_state(const net_session *s) {
    if (s->resolver) return NC_CONNECTING;
    if (!s->link) return NC_DISCONNECTED;
    // While the next zone is being joined the session is alive whatever became of
    // the current link (the old world stays on show, frozen, until the switch).
    if (s->link->state == NC_DISCONNECTED && s->next_link) return NC_CONNECTED;
    return s->link->state;
}

void net_session_frame(net_session *s, const PlayerInput *in, double now) {
    if (s->resolver) {
        session_resolve(s, now);
        return;
    }
    if (!s->link) return;
    net_client_frame(s->link, in, now);
    if (s->link->state == NC_DISCONNECTED && !s->next_link) {
        // Lost the server: back to it, where our body is waiting, a few times over.
        // Through a directory that takes a fresh token for that zone (ours may be
        // stale, or for the zone before a hand-off), resolved first.
        if (lost(s->link) && s->link->state == NC_DISCONNECTED && s->reconnect_failures < NET_RECONNECT_ATTEMPTS
            && (s->link->player != CW_INVALID_ENTITY || s->reconnects)) {
            bool ok = s->directory.ip && s->link->zone[0] ? session_reresolve(s, now)
                                                          : session_open(s, &s->next_world, &s->next_link, s->link->server, NULL, 0, NULL);
            if (ok) {
                ++s->reconnects;
                return;
            }
        }
        s->failure = s->link->failure;
        return;
    }

    if (s->next_link) {
        net_client_frame(s->next_link, in, now);
        if (s->next_link->state == NC_DISCONNECTED) {
            // The new zone would not have us; stay here, unfrozen. The old zone asks
            // again later. (After a lost connection: one try fewer.)
            bool was_reconnect = !s->link->redirect_pending;
            session_close(&s->next_world, &s->next_link, LEAVE_QUIT);
            if (was_reconnect) ++s->reconnect_failures;
            else {
                net_client_handoff_failed(s->link);
                ++s->handoffs_failed;
            }
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
            if (!s->next_link->party_updates) s->next_link->party = s->link->party;   // until the new zone is told
            if (!s->next_link->gear_known) {
                s->next_link->gear = s->link->gear;
                s->next_link->gear_known = s->link->gear_known;
            }
            bool handoff = s->link->redirect_pending;
            session_close(&s->world, &s->link, handoff ? LEAVE_HANDOFF : LEAVE_QUIT);
            s->world = new_world;
            s->link = s->next_link;
            s->next_world = NULL;
            s->next_link = NULL;
            if (handoff) {
                ++s->handoffs;
                s->last_handoff_seconds = (float)(now - s->handoff_started);
            } else {
                s->reconnect_failures = 0;
            }
        }
    } else if (s->link->redirect_pending) {
        s->handoff_started = now;
        if (!session_open(s, &s->next_world, &s->next_link, s->link->redirect_address, s->link->redirect_ticket,
                          s->link->redirect_ticket_size, NULL))
            net_client_handoff_failed(s->link);
    }
}

void net_session_party_request(net_session *s, int op, const char *target_name) {
    if (s->link) net_client_party_request(s->link, op, target_name);
}

const party_view *net_session_party(const net_session *s) {
    static const party_view none = {};
    return s->link ? &s->link->party : &none;
}

void net_session_equip(net_session *s, int stash_index) {
    if (s->link) net_client_equip(s->link, stash_index);
}
