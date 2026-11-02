// The multiplayer stack without a window: zone servers and clients in one process,
// talking over loopback UDP. Real sockets, real time, simulated loss.
#include "../src/net/client.h"
#include "../src/net/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } \
} while (0)

static float distance(v3 a, v3 b) { return v3_length(v3_sub(a, b)); }
static v3 position_in(game *g, cw_entity_id id) { return CW_GET(&g->world, id, g->c_transform, Transform)->position; }

// ---------------------------------------------------------------------------
// Transport: the reliable channel delivers everything, in order, through loss
// ---------------------------------------------------------------------------
struct receiver {
    int reliable_received, unreliable_received, out_of_order;
    uint32_t expected;
};
static void on_transport_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    receiver *r = (receiver *)user;
    uint32_t value = 0;
    cw_reader_u32(&payload, &value);
    if (type == 1) {
        if (value != r->expected) ++r->out_of_order;
        r->expected = value + 1;
        ++r->reliable_received;
    } else {
        ++r->unreliable_received;
    }
}

// Feeds everything waiting on the socket to the connection.
static void pump_socket(net_socket *socket, net_connection *c, double now) {
    unsigned char packet[NET_MAX_PACKET];
    for (;;) {
        int size = net_socket_recv(socket, NULL, packet, sizeof(packet));
        if (size <= 0) break;
        net_connection_receive(c, packet, (size_t)size, now);
    }
}

static void test_reliable_channel_under_loss(void) {
    net_socket *a = net_socket_open(0), *b = net_socket_open(0);
    CHECK(a && b);
    net_socket_set_loss(a, 0.3f);
    net_socket_set_loss(b, 0.3f);
    net_address loopback_a = net_address_make(0x7f000001u, net_socket_port(a));
    net_address loopback_b = net_address_make(0x7f000001u, net_socket_port(b));
    receiver ra = {}, rb = {};
    net_connection ca, cb;
    net_connection_init(&ca, a, loopback_b, 42, on_transport_message, &ra);
    net_connection_init(&cb, b, loopback_a, 42, on_transport_message, &rb);

    const uint32_t total = 300;   // several windows' worth, so the backlog is exercised too
    for (uint32_t i = 0; i < total; ++i) {
        unsigned char payload[4] = {(unsigned char)i, (unsigned char)(i >> 8), 0, 0};
        CHECK(net_connection_send(&ca, 1, true, payload, 4));
        CHECK(net_connection_send(&ca, 2, false, payload, 4));
    }
    double start = net_seconds();
    while (rb.reliable_received < (int)total && net_seconds() - start < 10.0) {
        double now = net_seconds();
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);   // b only carries acks
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        net_sleep(0.001);
    }
    CHECK(rb.reliable_received == (int)total);
    CHECK(rb.out_of_order == 0);
    CHECK(rb.unreliable_received < (int)total);      // some were lost, as they should be
    CHECK(cb.packets_received > 0 && ca.messages_resent > 0);
    start = net_seconds();
    while (net_connection_unacked(&ca) && net_seconds() - start < 5.0) {
        double now = net_seconds();
        net_connection_flush(&ca, now, true);
        net_connection_flush(&cb, now, true);
        pump_socket(a, &ca, now);
        pump_socket(b, &cb, now);
        net_sleep(0.001);
    }
    CHECK(net_connection_unacked(&ca) == 0);

    // A packet for another session is rejected, not delivered.
    net_connection stranger;
    net_connection_init(&stranger, a, loopback_b, 7, NULL, NULL);
    net_connection_send(&stranger, 1, false, "x", 1);
    net_connection_flush(&stranger, net_seconds(), false);
    net_sleep(0.01);
    int before = rb.reliable_received + rb.unreliable_received;
    pump_socket(b, &cb, net_seconds());
    CHECK(rb.reliable_received + rb.unreliable_received == before);
    CHECK(cb.packets_rejected == 1);
    net_connection_destroy(&stranger);

    net_connection_destroy(&ca);
    net_connection_destroy(&cb);
    net_socket_close(a);
    net_socket_close(b);
}

// ---------------------------------------------------------------------------
// Snapshots: quantised, delta-coded, and exact after a round trip
// ---------------------------------------------------------------------------
static void test_snapshot_codec(void) {
    uint64_t v = 0;
    unsigned char buffer[64];
    cw_writer w;
    cw_writer_init_fixed(&w, buffer, sizeof(buffer));
    net_write_uvarint(&w, 0);
    net_write_uvarint(&w, 127);
    net_write_uvarint(&w, 128);
    net_write_uvarint(&w, UINT64_MAX);
    net_write_svarint(&w, -1);
    net_write_svarint(&w, INT64_MIN);
    CHECK(w.size == 1 + 1 + 2 + 10 + 1 + 10);
    cw_reader r = cw_reader_make(buffer, w.size);
    int64_t sv = 0;
    CHECK(net_read_uvarint(&r, &v) && v == 0);
    CHECK(net_read_uvarint(&r, &v) && v == 127);
    CHECK(net_read_uvarint(&r, &v) && v == 128);
    CHECK(net_read_uvarint(&r, &v) && v == UINT64_MAX);
    CHECK(net_read_svarint(&r, &sv) && sv == -1);
    CHECK(net_read_svarint(&r, &sv) && sv == INT64_MIN);
    CHECK(cw_reader_at_end(&r));

    snapshot_ring ring;
    snapshot_ring_init(&ring);
    snapshot *base = snapshot_ring_push(&ring, 8);
    base->tick = 100;
    base->last_input_seq = 7;
    for (int i = 0; i < 6; ++i)
        base->entries[base->count++] = snapshot_entry_make(i * 3 + 1, i % 2 ? 50 : CW_INVALID_ENTITY,
                                                           v3_make(1.2345f * (float)i, -7.5f, 1000.0f + (float)i),
                                                           v3_make(6.0f, 0.0f, -0.01f * (float)i), 0.3f * (float)i,
                                                           -0.2f, i % 3 == 0);
    snapshot_sort(base);
    CHECK(fabsf(snapshot_position(&base->entries[1]).x - 1.2345f) < 0.0006f);   // a millimetre
    CHECK(fabsf(snapshot_yaw(&base->entries[2]) - 0.6f) < 0.0002f);
    CHECK(fabsf(snapshot_pitch(&base->entries[2]) + 0.2f) < 0.0002f);

    // Current: one player moved a little, one left, one arrived, the rest stood still.
    snapshot *current = snapshot_ring_push(&ring, 8);
    current->tick = 103;
    current->last_input_seq = 9;
    for (int i = 0; i < 6; ++i) {
        if (i == 2) continue;   // left
        snapshot_entry e = base->entries[i];
        if (i == 4) e.position[0] += 300;   // 30 cm
        current->entries[current->count++] = e;
    }
    current->entries[current->count++] = snapshot_entry_make(99, CW_INVALID_ENTITY, v3_make(5, 5, 5), v3_make(0, 0, 0), 0, 0, true);
    snapshot_sort(current);

    unsigned char packet[NET_MAX_PAYLOAD];
    cw_writer_init_fixed(&w, packet, sizeof(packet));
    CHECK(snapshot_encode(current, base, &w));
    size_t delta_size = w.size;
    CHECK(delta_size < 14 + 4 * 2 + 4 + 40);   // header, four idle players at two bytes, a nudge, a newcomer

    cw_writer_init_fixed(&w, packet, sizeof(packet));
    CHECK(snapshot_encode(current, NULL, &w));
    CHECK(w.size > delta_size);                 // a full snapshot costs more

    cw_writer_init_fixed(&w, packet, sizeof(packet));
    snapshot_encode(current, base, &w);
    snapshot decoded = {};
    r = cw_reader_make(packet, w.size);
    uint32_t tick, base_tick;
    CHECK(snapshot_peek(&r, &tick, &base_tick) && tick == 103 && base_tick == 100);
    CHECK(snapshot_decode(&r, base, &decoded));
    CHECK(decoded.tick == 103 && decoded.last_input_seq == 9 && decoded.count == current->count);
    for (uint16_t i = 0; i < decoded.count; ++i) {
        snapshot_entry a = decoded.entries[i], b = current->entries[i];
        a.priority = b.priority = 0.0f;
        CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    }
    // Decoding against the wrong base (or none) is refused rather than garbled.
    r = cw_reader_make(packet, w.size);
    CHECK(!snapshot_decode(&r, NULL, &decoded));
    snapshot_free(&decoded);
    snapshot_ring_destroy(&ring);
}

// ---------------------------------------------------------------------------
// Tickets: signed by one zone, verified by another, forgeries refused
// ---------------------------------------------------------------------------
static void test_tickets(void) {
    // The SipHash-2-4 reference vector: key 00..0f, message 00..0e.
    unsigned char key_bytes[16], message[15];
    for (int i = 0; i < 16; ++i) key_bytes[i] = (unsigned char)i;
    for (int i = 0; i < 15; ++i) message[i] = (unsigned char)i;
    uint64_t k0 = 0, k1 = 0;
    for (int i = 7; i >= 0; --i) {
        k0 = (k0 << 8) | key_bytes[i];
        k1 = (k1 << 8) | key_bytes[8 + i];
    }
    CHECK(net_siphash(message, sizeof(message), k0, k1) == UINT64_C(0xa129ca6149be45e5));

    net_zone_table zones;
    net_zones_init(&zones);
    net_ticket t = {};
    t.account = 0x1234567890abcdefull;
    t.position = v3_make(1, 2, 3);
    t.velocity = v3_make(-4, 5, -6);
    t.yaw = 0.5f;
    t.pitch = -0.25f;
    t.issued = 1234.5;
    unsigned char buffer[NET_TICKET_SIZE];
    cw_writer w;
    cw_writer_init_fixed(&w, buffer, sizeof(buffer));
    net_ticket_write(&w, &t, &zones);
    CHECK(w.size <= NET_TICKET_SIZE);
    net_ticket back = {};
    cw_reader r = cw_reader_make(buffer, w.size);
    CHECK(net_ticket_read(&r, &back, &zones));
    CHECK(back.signature != 0);
    t.signature = back.signature;
    CHECK(memcmp(&back, &t, sizeof(t)) == 0);

    unsigned char forged[NET_TICKET_SIZE];
    memcpy(forged, buffer, w.size);
    forged[8] ^= 1;   // a different position
    r = cw_reader_make(forged, w.size);
    CHECK(!net_ticket_read(&r, &back, &zones));
    net_zone_table other;
    net_zones_init(&other);
    net_zones_set_secret(&other, "another-secret");
    r = cw_reader_make(buffer, w.size);
    CHECK(!net_ticket_read(&r, &back, &other));
}

// ---------------------------------------------------------------------------
// The stack: servers and clients pumped together in real time
// ---------------------------------------------------------------------------
#define RIG_MAX 4
struct rig {
    server *servers[RIG_MAX];
    int server_count;
    net_session sessions[RIG_MAX];
    PlayerInput inputs[RIG_MAX];
    bool used[RIG_MAX];
    int client_count;
};

static server *rig_add_server(rig *r, const server_config *config, float loss) {
    server *s = (server *)malloc(sizeof(server));
    CHECK(server_init(s, config));
    if (loss > 0.0f) net_socket_set_loss(s->socket, loss);
    r->servers[r->server_count++] = s;
    return s;
}

static void rig_init(rig *r, float server_loss) {
    memset(r, 0, sizeof(*r));
    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = RIG_MAX;
    rig_add_server(r, &config, server_loss);
}

static int rig_add_client_to(rig *r, server *s, float loss) {
    int i = r->client_count++;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%u", (unsigned)server_port(s));
    CHECK(net_session_init(&r->sessions[i], address, 0.0f));
    if (loss > 0.0f) net_socket_set_loss(r->sessions[i].link->socket, loss);
    r->used[i] = true;
    return i;
}
static int rig_add_client(rig *r, float loss) { return rig_add_client_to(r, r->servers[0], loss); }

static void rig_drop_client(rig *r, int i) {
    net_session_destroy(&r->sessions[i]);
    r->used[i] = false;
}

static void rig_destroy(rig *r) {
    for (int i = 0; i < r->client_count; ++i) if (r->used[i]) rig_drop_client(r, i);
    for (int i = 0; i < r->server_count; ++i) {
        server_destroy(r->servers[i]);
        free(r->servers[i]);
    }
}

// One frame of everyone, the way the real programs run it.
static void rig_step(rig *r) {
    double now = net_seconds();
    for (int i = 0; i < r->server_count; ++i) server_update(r->servers[i], now);
    for (int i = 0; i < r->client_count; ++i)
        if (r->used[i]) net_session_frame(&r->sessions[i], &r->inputs[i], now);
    net_sleep(0.001);
}
static void rig_run(rig *r, double seconds) {
    double start = net_seconds();
    while (net_seconds() - start < seconds) rig_step(r);
}
static bool rig_wait_connected(rig *r, double seconds) {
    double start = net_seconds();
    for (;;) {
        bool all = true;
        for (int i = 0; i < r->client_count; ++i)
            if (r->used[i] && net_session_state(&r->sessions[i]) != NC_CONNECTED) all = false;
        if (all) return true;
        if (net_seconds() - start > seconds) return false;
        rig_step(r);
    }
}
static net_client *link_of(rig *r, int i) { return r->sessions[i].link; }
static game *world_of(rig *r, int i) { return r->sessions[i].world; }

static void test_two_clients_see_each_other(void) {
    rig r;
    rig_init(&r, 0.0f);
    int a = rig_add_client(&r, 0.0f), b = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    server *s = r.servers[0];
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    game *ga = world_of(&r, a), *gb = world_of(&r, b);
    CHECK(s->client_count == 2);
    CHECK(ca->player != cb->player);
    CHECK(ca->account != cb->account);

    // Both replicas hold the whole world: every entity the server has, and nothing else.
    rig_run(&r, 0.3);
    size_t server_entities = cw_world_entity_count(&s->zone.world);
    CHECK(cw_world_entity_count(&ga->world) == server_entities);
    CHECK(cw_world_entity_count(&gb->world) == server_entities);
    CHECK(game_find_ship(ga, "Emerald Skiff") == game_find_ship(&s->zone, "Emerald Skiff"));
    CHECK(cw_query_count(ga->q_solids) == cw_query_count(s->zone.q_solids));
    CHECK(ga->place_count == s->zone.place_count);

    // Each sees the other as a capsule, coloured by the other's id, and only has an
    // input for itself: that is what makes its own player the predicted one.
    for (int k = 0; k < 2; ++k) {
        game *viewer = k ? gb : ga;
        net_client *link = k ? cb : ca;
        cw_entity_id other = k ? ca->player : cb->player;
        CHECK(cw_world_is_valid(&viewer->world, other));
        const Visual *v = CW_GET(&viewer->world, other, viewer->c_visual, Visual);
        CHECK(v && v->mesh == MESH_CAPSULE);
        v3 expected_color = game_player_color(other);
        if (v) CHECK(memcmp(&v->color, &expected_color, sizeof(v3)) == 0);
        CHECK(cw_comp_has(&viewer->world, other, viewer->c_player));
        CHECK(!cw_comp_has(&viewer->world, other, viewer->c_input));
        CHECK(cw_comp_has(&viewer->world, link->player, viewer->c_input));
        CHECK(cw_query_count(viewer->q_players) == 1);
        CHECK(net_client_entity_visible(link, other));
    }
    CHECK(distance(position_in(gb, ca->player), s->zone.spawn) < 0.01f);

    // A walks forward for a second. A's own view moves at once (prediction), the
    // server follows a round trip later, and B watches it happen. Throughout, A's
    // prediction agrees with what the server then computes.
    r.inputs[a].forward = 1.0f;
    v3 a_before_on_b = position_in(gb, ca->player);
    float worst_error = 0.0f;
    double start = net_seconds();
    while (net_seconds() - start < 1.0) {
        rig_step(&r);
        if (ca->last_error > worst_error) worst_error = ca->last_error;
    }
    v3 a_predicted = position_in(ga, ca->player), a_on_server = position_in(&s->zone, ca->player);
    CHECK(distance(a_on_server, s->zone.spawn) > 3.0f);
    CHECK(distance(a_predicted, s->zone.spawn) > distance(a_on_server, s->zone.spawn));   // A is ahead of the server
    CHECK(distance(a_predicted, a_on_server) < 0.6f);                                     // by about a round trip
    // ...and never mispredicted, unless an input reached the server late: then the
    // server repeated the previous one and the client was told so.
    uint32_t starved = server_client_of(s, ca->player)->inputs.starved;
    CHECK(worst_error < 0.002f || starved > 0);
    CHECK(ca->resyncs <= 2 + starved);   // the first snapshot anchors the clock; each starvation moves it
    CHECK(distance(position_in(gb, ca->player), a_before_on_b) > 2.0f);
    CHECK(distance(position_in(gb, cb->player), s->zone.spawn) < 0.01f);    // B never moved
    CHECK(s->clients[0].inputs.starved + s->clients[1].inputs.starved < 20);
    // The clients' tick rates follow the server's: its input queues sit at the target.
    for (int k = 0; k < 2; ++k) {
        const input_queue *q = &server_client_of(s, k ? cb->player : ca->player)->inputs;
        int lead = (int)(q->newest - q->last_applied);
        CHECK(lead >= SERVER_INPUT_TARGET - 1 && lead <= SERVER_INPUT_TARGET + 3);
        CHECK(q->skipped == 0);
    }
    CHECK(ca->rate > 0.9f && ca->rate < 1.1f);
    r.inputs[a].forward = 0.0f;
    // Once A stops and the interpolation delay passes, every view agrees exactly.
    // Standing still, a snapshot is a couple of bytes per player: B hears almost
    // nothing, and A's inputs are the bulk of what crosses the wire.
    uint64_t b_received = cb->connection.bytes_received, a_sent = ca->connection.bytes_sent;
    rig_run(&r, 0.5);
    CHECK(cb->connection.bytes_received - b_received < 1500);   // ~10 snapshots of two idle players
    CHECK(ca->connection.bytes_sent - a_sent < 4000);           // ~30 inputs
    a_on_server = position_in(&s->zone, ca->player);
    CHECK(distance(position_in(gb, ca->player), a_on_server) < 0.005f);
    CHECK(distance(position_in(ga, ca->player), a_on_server) < 0.001f);
    CHECK(v3_length(net_client_view_offset(ca)) < 0.001f);
    CHECK(ca->rtt > 0.0f && ca->rtt < 0.2f);
    CHECK(cb->snapshots_received > 20 && cb->snapshots_dropped == 0);

    // Ships follow the same timetable everywhere: B's are exactly where universe time
    // puts them at B's tick, which runs a little ahead of the server's. Parts follow.
    cw_entity_id skiff = game_find_ship(&s->zone, "Emerald Skiff");
    CHECK(gb->universe_base == s->zone.universe_base);
    int64_t lead = (int64_t)(gb->world.tick - s->zone.world.tick);
    CHECK(lead >= 0 && lead <= 12);
    Ship timetable = *CW_GET(&gb->world, skiff, gb->c_ship, Ship);
    CHECK(distance(position_in(gb, skiff), game_ship_at_time(&timetable, game_time_of_tick(gb, gb->world.tick))) < 0.001f);
    timetable = *CW_GET(&s->zone.world, skiff, s->zone.c_ship, Ship);
    CHECK(distance(position_in(&s->zone, skiff), game_ship_at_time(&timetable, game_time_of_tick(&s->zone, s->zone.world.tick))) < 0.001f);
    cw_query parts = cw_query_begin(gb->q_attachments);
    cw_entity_id part;
    int checked = 0;
    while (cw_query_next(&parts, &part)) {
        const Attachment *at = CW_GET(&gb->world, part, gb->c_attachment, Attachment);
        if (at->ship != skiff) continue;
        v3 expected = v3_add(position_in(gb, skiff), at->offset);
        CHECK(distance(position_in(gb, part), expected) < 0.001f);
        ++checked;
    }
    CHECK(checked > 5);

    // A leaves: the server despawns A and B's replica loses the entity.
    cw_entity_id a_player = ca->player;
    rig_drop_client(&r, a);
    rig_run(&r, 0.5);
    CHECK(s->client_count == 1);
    CHECK(!cw_world_is_valid(&s->zone.world, a_player));
    CHECK(!cw_world_is_valid(&gb->world, a_player));
    CHECK(cw_world_is_valid(&gb->world, cb->player));
    CHECK(cb->state == NC_CONNECTED);

    // A third player joins later and appears on B without B reconnecting.
    int c = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    rig_run(&r, 0.3);
    CHECK(cw_world_is_valid(&gb->world, link_of(&r, c)->player));
    CHECK(CW_GET(&gb->world, link_of(&r, c)->player, gb->c_visual, Visual)->mesh == MESH_CAPSULE);
    CHECK(cw_world_entity_count(&gb->world) == cw_world_entity_count(&s->zone.world));
    CHECK(cw_world_entity_count(&world_of(&r, c)->world) == cw_world_entity_count(&s->zone.world));
    rig_destroy(&r);
}

// Joining and staying joined through a lossy link: the baseline is reliable, the
// snapshots keep coming (coded against whatever did arrive), prediction holds.
static void test_join_and_play_under_loss(void) {
    rig r;
    rig_init(&r, 0.25f);
    int a = rig_add_client(&r, 0.25f);
    int b = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 8.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.5);
    server *s = r.servers[0];
    net_client *ca = link_of(&r, a);
    CHECK(cw_world_entity_count(&world_of(&r, a)->world) == cw_world_entity_count(&s->zone.world));
    CHECK(cw_world_entity_count(&world_of(&r, b)->world) == cw_world_entity_count(&s->zone.world));
    CHECK(ca->connection.messages_resent > 0 || s->clients[0].connection.messages_resent > 0);

    r.inputs[a].forward = 1.0f;
    r.inputs[a].jump = true;
    rig_run(&r, 1.0);
    r.inputs[a].forward = 0.0f;
    r.inputs[a].jump = false;
    rig_run(&r, 1.0);
    v3 on_server = position_in(&s->zone, ca->player);
    CHECK(distance(on_server, s->zone.spawn) > 3.0f);
    CHECK(distance(position_in(world_of(&r, a), ca->player), on_server) < 0.01f);
    CHECK(distance(position_in(world_of(&r, b), ca->player), on_server) < 0.01f);
    CHECK(ca->snapshots_received > 20);
    CHECK(ca->state == NC_CONNECTED && link_of(&r, b)->state == NC_CONNECTED);
    rig_destroy(&r);
}

// A server that is not there, a full server and a protocol mismatch all end cleanly.
static void test_rejections(void) {
    net_session bad;
    CHECK(!net_session_init(&bad, "no.such.host.invalid:1", 0.0f));
    net_session_destroy(&bad);   // safe on a failed init

    rig r;
    rig_init(&r, 0.0f);
    r.servers[0]->config.max_clients = 1;   // one seat
    int a = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    int b = rig_add_client(&r, 0.0f);
    double start = net_seconds();
    while (net_session_state(&r.sessions[b]) != NC_DISCONNECTED && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(net_session_state(&r.sessions[b]) == NC_DISCONNECTED);
    CHECK(r.sessions[b].failure && strstr(r.sessions[b].failure, "full"));
    CHECK(net_session_state(&r.sessions[a]) == NC_CONNECTED);
    rig_destroy(&r);
}

// ---------------------------------------------------------------------------
// Zones: two servers, a hand-off, a forged ticket
// ---------------------------------------------------------------------------
static void test_zones(void) {
    // Two zones: the hub, and every planet. Addresses are filled in once the
    // sockets are open, since the tests take whatever ports are free.
    net_zone_table zones;
    net_zones_init(&zones);
    int hub = net_zones_add(&zones, "hub", net_address_make(0x7f000001u, 0));
    int planets = net_zones_add(&zones, "planets", net_address_make(0x7f000001u, 0));
    net_zone_add_place(&zones.zones[hub], "hub");
    net_zone_add_place(&zones.zones[planets], "Verdant");
    net_zone_add_place(&zones.zones[planets], "Ember");
    net_zone_add_place(&zones.zones[planets], "Azure");

    rig r;
    memset(&r, 0, sizeof(r));
    server_config config = server_config_default();
    config.max_clients = RIG_MAX;
    config.zones = &zones;
    config.universe_epoch = net_wall_seconds() - 25.0;   // the same clock for both, with the skiff mid-flight
    config.zone = hub;
    server *hub_server = rig_add_server(&r, &config, 0.0f);
    zones.zones[hub].address.port = server_port(hub_server);
    config.zone = planets;
    server *planet_server = rig_add_server(&r, &config, 0.0f);
    zones.zones[planets].address.port = server_port(planet_server);
    if (failures) { rig_destroy(&r); return; }

    // Ships agree between zones without the zones ever talking.
    rig_run(&r, 0.5);
    cw_entity_id skiff = game_find_ship(&hub_server->zone, "Emerald Skiff");
    CHECK(skiff == game_find_ship(&planet_server->zone, "Emerald Skiff"));
    const Ship *skiff_on_hub = CW_GET(&hub_server->zone.world, skiff, hub_server->zone.c_ship, Ship);
    CHECK(skiff_on_hub->state == SHIP_FLYING);
    CHECK(distance(position_in(&hub_server->zone, skiff), position_in(&planet_server->zone, skiff)) < 2.0f);

    // A player on the hub zone, and a bystander already on the planet zone.
    int a = rig_add_client_to(&r, hub_server, 0.0f);
    int b = rig_add_client_to(&r, planet_server, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    CHECK(hub_server->client_count == 1 && planet_server->client_count == 1);
    CHECK(game_place_at(&hub_server->zone, position_in(&hub_server->zone, link_of(&r, a)->player)) == 0);
    int verdant = game_find_place(&planet_server->zone, "Verdant");
    CHECK(verdant > 0);
    CHECK(distance(position_in(&planet_server->zone, link_of(&r, b)->player), planet_server->zone.places[verdant].spawn) < 0.01f);
    uint64_t account = link_of(&r, a)->account;
    cw_entity_id a_on_hub = link_of(&r, a)->player;

    // A is set down on Verdant's pad by the hub server (in place of a thirty-second
    // flight): the hub hands A over, A's session moves zones, B sees A arrive.
    v3 pad = planet_server->zone.places[verdant].spawn;
    Transform *at = CW_GET(&hub_server->zone.world, a_on_hub, hub_server->zone.c_transform, Transform);
    at->position = at->previous = v3_add(pad, v3_make(0, 0.5f, 0));
    double start = net_seconds();
    while (r.sessions[a].handoffs == 0 && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(r.sessions[a].handoffs == 1);
    rig_run(&r, 0.5);
    net_client *ca = link_of(&r, a);
    CHECK(ca->state == NC_CONNECTED);
    CHECK(ca->server.port == server_port(planet_server));
    CHECK(ca->account == account);                                      // the same player
    CHECK(hub_server->client_count == 0 && planet_server->client_count == 2);
    CHECK(!cw_world_is_valid(&hub_server->zone.world, a_on_hub));
    CHECK(distance(position_in(&planet_server->zone, ca->player), pad) < 1.0f);   // standing where they were set down
    CHECK(distance(position_in(world_of(&r, a), ca->player), pad) < 1.0f);
    game *gb = world_of(&r, b);
    CHECK(cw_world_is_valid(&gb->world, ca->player));
    CHECK(CW_GET(&gb->world, ca->player, gb->c_visual, Visual)->mesh == MESH_CAPSULE);
    CHECK(distance(position_in(gb, ca->player), pad) < 1.0f);
    // A keeps playing in the new zone: walk, and B sees it.
    r.inputs[a].forward = 1.0f;
    rig_run(&r, 1.0);
    r.inputs[a].forward = 0.0f;
    rig_run(&r, 0.5);
    CHECK(distance(position_in(&planet_server->zone, ca->player), pad) > 3.0f);
    CHECK(distance(position_in(gb, ca->player), position_in(&planet_server->zone, ca->player)) < 0.01f);
    CHECK(ca->last_error < 0.002f);

    // The ticket A arrived with is good once: presenting it again is refused.
    {
        game *again_world = (game *)malloc(sizeof(game));
        game_init(again_world, CW_CLIENT);
        net_client *again = (net_client *)malloc(sizeof(net_client));
        CHECK(net_client_init(again, again_world, zones.zones[planets].address, ca->ticket, ca->ticket_size, 0.0f));
        PlayerInput idle = {};
        start = net_seconds();
        while (again->state != NC_DISCONNECTED && net_seconds() - start < 5.0) {
            rig_step(&r);
            net_client_frame(again, &idle, net_seconds());
        }
        CHECK(again->state == NC_DISCONNECTED);
        CHECK(again->failure && strstr(again->failure, "ticket"));
        CHECK(planet_server->client_count == 2);
        net_client_destroy(again);
        game_destroy(again_world);
        free(again);
        free(again_world);
    }

    // A ticket that was not signed by a zone is refused.
    game *forger_world = (game *)malloc(sizeof(game));
    game_init(forger_world, CW_CLIENT);
    net_client *forger = (net_client *)malloc(sizeof(net_client));
    unsigned char forged[NET_TICKET_SIZE];
    memset(forged, 0x42, sizeof(forged));
    CHECK(net_client_init(forger, forger_world, zones.zones[planets].address, forged, sizeof(forged), 0.0f));
    PlayerInput idle = {};
    start = net_seconds();
    while (forger->state != NC_DISCONNECTED && net_seconds() - start < 5.0) {
        rig_step(&r);
        net_client_frame(forger, &idle, net_seconds());
    }
    CHECK(forger->state == NC_DISCONNECTED);
    CHECK(forger->failure && strstr(forger->failure, "ticket"));
    CHECK(planet_server->client_count == 2);
    net_client_destroy(forger);
    game_destroy(forger_world);
    free(forger);
    free(forger_world);
    rig_destroy(&r);
}

// A hand-off to a zone that will not answer: the player is frozen meanwhile, nothing
// is lost, and both sides thaw and carry on in the old zone.
static void test_handoff_failure(void) {
    net_zone_table zones;
    net_zones_init(&zones);
    int hub = net_zones_add(&zones, "hub", net_address_make(0x7f000001u, 0));
    int planets = net_zones_add(&zones, "planets", net_address_make(0x7f000001u, 0));
    net_zone_add_place(&zones.zones[hub], "hub");
    net_zone_add_place(&zones.zones[planets], "Verdant");

    rig r;
    memset(&r, 0, sizeof(r));
    server_config config = server_config_default();
    config.max_clients = RIG_MAX;
    config.zones = &zones;
    config.zone = hub;
    server *hub_server = rig_add_server(&r, &config, 0.0f);
    zones.zones[hub].address.port = server_port(hub_server);
    config.zone = planets;
    server *dead = rig_add_server(&r, &config, 1.0f);   // there, but every packet it sends is lost
    zones.zones[planets].address.port = server_port(dead);
    int a = rig_add_client_to(&r, hub_server, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    net_client *ca = link_of(&r, a);
    server_client *sc = server_client_of(hub_server, ca->player);
    int verdant = game_find_place(&hub_server->zone, "Verdant");
    v3 pad = hub_server->zone.places[verdant].spawn;
    Transform *at = CW_GET(&hub_server->zone.world, ca->player, hub_server->zone.c_transform, Transform);
    at->position = at->previous = v3_add(pad, v3_make(0, 0.5f, 0));
    r.inputs[a].forward = 1.0f;   // trying to walk the whole time

    // Frozen: redirected, and not moving despite the input, on the server or in the prediction.
    rig_run(&r, 1.0);
    CHECK(sc->state == CLIENT_HANDING_OFF);
    CHECK(ca->redirect_pending && r.sessions[a].next_link != NULL);
    CHECK(distance(position_in(&hub_server->zone, ca->player), pad) < 0.05f);
    CHECK(distance(position_in(world_of(&r, a), ca->player), pad) < 0.05f);
    CHECK(ca->last_error < 0.002f);

    // The attempt times out; the client says so; everyone thaws. Moved off the pad
    // by the server (so the hub does not just try again), the player walks on.
    double start = net_seconds();
    while (r.sessions[a].handoffs_failed == 0 && net_seconds() - start < NET_CONNECT_TIMEOUT + 2.0) rig_step(&r);
    CHECK(r.sessions[a].handoffs_failed == 1);
    CHECK(!ca->redirect_pending && r.sessions[a].next_link == NULL);
    at->position = at->previous = hub_server->zone.spawn;
    rig_run(&r, 0.6);
    CHECK(sc->handoffs_failed >= 1);
    CHECK(sc->state == CLIENT_ACTIVE);
    CHECK(distance(position_in(&hub_server->zone, ca->player), hub_server->zone.spawn) > 2.0f);
    CHECK(ca->state == NC_CONNECTED && hub_server->client_count == 1);
    rig_destroy(&r);
}

// The server's tick follows the wall clock, through stalls and clock jumps, so
// universe time means the same thing on every zone.
static void test_server_clock(void) {
    server_config config = server_config_default();
    config.port = 0;
    server *s = (server *)malloc(sizeof(server));
    CHECK(server_init(s, &config));
    double start = net_seconds();
    while (net_seconds() - start < 1.0) {
        server_update(s, net_seconds());
        net_sleep(0.001);
    }
    uint64_t target = server_target_tick(s);
    CHECK(s->zone.world.tick <= target && target - s->zone.world.tick <= 1);
    CHECK(fabs(game_time_of_tick(&s->zone, s->zone.world.tick) - (net_wall_seconds() - config.universe_epoch)) < 0.05);

    // A stall: time passes, no updates. Short ones are simulated in full.
    net_sleep(0.3);
    uint64_t before = s->zone.world.tick;
    server_update(s, net_seconds());
    CHECK(s->zone.world.tick - before >= 17 && s->zone.world.tick - before <= 19);
    CHECK(s->ticks_skipped == 0);

    // A long one (here: the clock saying we are 3 s behind) is caught up to a point
    // and the rest skipped.
    s->zone.universe_base -= 3.0;
    server_update(s, net_seconds());
    CHECK(s->ticks_skipped >= 180 - SERVER_MAX_CATCHUP && s->ticks_skipped <= 180 - SERVER_MAX_CATCHUP + 3);
    target = server_target_tick(s);
    CHECK(target - s->zone.world.tick <= 3);

    // A clock jump (10 s) moves universe time instead of the tick.
    uint64_t tick = s->zone.world.tick;
    s->zone.universe_base -= 10.0;
    server_update(s, net_seconds());
    CHECK(s->rebases == 1);
    CHECK(s->zone.world.tick - tick <= 2);
    CHECK(fabs(game_time_of_tick(&s->zone, s->zone.world.tick) - (net_wall_seconds() - config.universe_epoch)) < 0.05);
    server_destroy(s);
    free(s);
}

int main(void) {
    if (!net_init()) {
        puts("test_net: the network could not be initialised");
        return 1;
    }
    test_reliable_channel_under_loss();
    test_snapshot_codec();
    test_tickets();
    test_two_clients_see_each_other();
    test_join_and_play_under_loss();
    test_rejections();
    test_zones();
    test_handoff_failure();
    test_server_clock();
    net_shutdown();
    if (failures) {
        printf("test_net: %d failed\n", failures);
        return 1;
    }
    puts("test_net: ok");
    return 0;
}
