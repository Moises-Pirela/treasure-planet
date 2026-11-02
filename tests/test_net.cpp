// The multiplayer stack without a window: a zone server and two clients in one
// process, talking over loopback UDP. Real sockets, real time, simulated loss.
#include "../src/net/client.h"
#include "../src/net/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } \
} while (0)

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
    // Once b's final ack gets through, a has nothing left to send.
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
// The stack: a server and clients pumped together in real time
// ---------------------------------------------------------------------------
struct rig {
    server *zone;
    game *games[4];
    net_client *clients[4];
    PlayerInput inputs[4];
    int client_count;
};

static void rig_init(rig *r, float server_loss) {
    memset(r, 0, sizeof(*r));
    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = 4;
    r->zone = (server *)malloc(sizeof(server));
    CHECK(server_init(r->zone, &config));
    if (server_loss > 0.0f) net_socket_set_loss(r->zone->socket, server_loss);
}

static int rig_add_client(rig *r, float loss) {
    int i = r->client_count++;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%u", (unsigned)server_port(r->zone));
    r->games[i] = (game *)malloc(sizeof(game));
    game_init(r->games[i], CW_CLIENT);
    r->clients[i] = (net_client *)malloc(sizeof(net_client));
    CHECK(net_client_init(r->clients[i], r->games[i], address, 0.0f));
    if (loss > 0.0f) net_socket_set_loss(r->clients[i]->socket, loss);
    return i;
}

static void rig_drop_client(rig *r, int i) {
    net_client_destroy(r->clients[i]);
    game_destroy(r->games[i]);
    free(r->clients[i]);
    free(r->games[i]);
    r->clients[i] = NULL;
    r->games[i] = NULL;
}

static void rig_destroy(rig *r) {
    for (int i = 0; i < r->client_count; ++i) if (r->clients[i]) rig_drop_client(r, i);
    server_destroy(r->zone);
    free(r->zone);
}

// One frame of everyone, the way the real programs run it.
static void rig_step(rig *r) {
    double now = net_seconds();
    server_update(r->zone, now);
    for (int i = 0; i < r->client_count; ++i) {
        net_client *c = r->clients[i];
        if (!c) continue;
        net_client_send_input(c, &r->inputs[i], now);
        net_client_update(c, now);
        net_client_resolve(c, now);
        game_client_frame(r->games[i]);
    }
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
            if (r->clients[i] && r->clients[i]->state != NC_CONNECTED) all = false;
        if (all) return true;
        if (net_seconds() - start > seconds) return false;
        rig_step(r);
    }
}

static v3 position_in(game *g, cw_entity_id id) { return CW_GET(&g->world, id, g->c_transform, Transform)->position; }
static float distance(v3 a, v3 b) { return v3_length(v3_sub(a, b)); }

static void test_two_clients_see_each_other(void) {
    rig r;
    rig_init(&r, 0.0f);
    int a = rig_add_client(&r, 0.0f), b = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    server *s = r.zone;
    net_client *ca = r.clients[a], *cb = r.clients[b];
    game *ga = r.games[a], *gb = r.games[b];
    CHECK(s->client_count == 2);
    CHECK(ca->player != cb->player);

    // Both replicas hold the whole world: every entity the server has, and nothing else.
    rig_run(&r, 0.3);
    size_t server_entities = cw_world_entity_count(&s->zone.world);
    CHECK(cw_world_entity_count(&ga->world) == server_entities);
    CHECK(cw_world_entity_count(&gb->world) == server_entities);
    CHECK(game_find_ship(ga, "Emerald Skiff") == game_find_ship(&s->zone, "Emerald Skiff"));
    CHECK(cw_query_count(ga->q_solids) == cw_query_count(s->zone.q_solids));
    CHECK(game_player_count(&s->zone) == 2);
    CHECK(game_player_count(ga) == 2);      // what the window title shows
    CHECK(game_player_count(gb) == 2);

    // Each sees the other as a capsule, coloured by the other's id.
    for (int k = 0; k < 2; ++k) {
        game *viewer = k ? gb : ga;
        cw_entity_id other = k ? ca->player : cb->player;
        CHECK(cw_world_is_valid(&viewer->world, other));
        const Visual *v = CW_GET(&viewer->world, other, viewer->c_visual, Visual);
        CHECK(v && v->mesh == MESH_CAPSULE);
        v3 expected_color = game_player_color(other);
        if (v) CHECK(memcmp(&v->color, &expected_color, sizeof(v3)) == 0);
        CHECK(cw_comp_has(&viewer->world, other, viewer->c_player));
        CHECK(net_client_entity_visible(k ? cb : ca, other));
        CHECK(!cw_comp_has(&viewer->world, other, viewer->c_input));   // inputs never leave the server
    }
    // The server moved nothing by itself: both stand at the spawn.
    CHECK(distance(position_in(gb, ca->player), s->zone.spawn) < 0.01f);

    // A walks forward for a second; the server moves A, and B watches it happen.
    r.inputs[a].forward = 1.0f;
    v3 a_before_on_b = position_in(gb, ca->player);
    rig_run(&r, 1.0);
    r.inputs[a].forward = 0.0f;
    v3 a_on_server = position_in(&s->zone, ca->player);
    CHECK(distance(a_on_server, s->zone.spawn) > 3.0f);
    CHECK(distance(position_in(gb, ca->player), a_before_on_b) > 2.0f);
    CHECK(distance(position_in(gb, cb->player), s->zone.spawn) < 0.01f);    // B never moved
    // Once A stops and the interpolation delay passes, every view agrees.
    rig_run(&r, 0.5);
    a_on_server = position_in(&s->zone, ca->player);
    CHECK(distance(position_in(gb, ca->player), a_on_server) < 0.05f);
    CHECK(distance(position_in(ga, ca->player), a_on_server) < 0.05f);
    CHECK(ca->rtt > 0.0f && ca->rtt < 0.2f);
    CHECK(cb->snapshots_received > 20);

    // Ships fly on the server and the replicas follow, parts included.
    cw_entity_id skiff = game_find_ship(&s->zone, "Emerald Skiff");
    CHECK(distance(position_in(gb, skiff), position_in(&s->zone, skiff)) < 0.5f);
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
    CHECK(game_player_count(gb) == 1);

    // A third player joins later and appears on B without B reconnecting.
    int c = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    rig_run(&r, 0.3);
    CHECK(game_player_count(gb) == 2);
    CHECK(cw_world_is_valid(&gb->world, r.clients[c]->player));
    CHECK(CW_GET(&gb->world, r.clients[c]->player, gb->c_visual, Visual)->mesh == MESH_CAPSULE);
    CHECK(cw_world_entity_count(&gb->world) == cw_world_entity_count(&s->zone.world));
    CHECK(cw_world_entity_count(&r.games[c]->world) == cw_world_entity_count(&s->zone.world));
    rig_destroy(&r);
}

// A ship's countdown and state keep up on clients, though snapshots only carry positions.
static void test_ship_state_reaches_clients(void) {
    rig r;
    rig_init(&r, 0.0f);
    int a = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    game *zone = &r.zone->zone;
    cw_entity_id id = game_find_ship(zone, "Emerald Skiff");
    Ship *truth = CW_GET(&zone->world, id, zone->c_ship, Ship);
    const Ship *seen = CW_GET(&r.games[a]->world, id, r.games[a]->c_ship, Ship);
    const float close_enough = 0.25f;

    // Docked: no messages, and the client counts by itself.
    float before = seen->timer;
    rig_run(&r, 1.0);
    CHECK(seen->timer > before + 0.8f);
    CHECK(fabsf(seen->timer - truth->timer) < close_enough);

    // Departure: skip most of the wait, then watch it arrive.
    truth->timer = truth->dock_seconds - 0.1f;
    rig_run(&r, 0.5);
    CHECK(truth->state == SHIP_FLYING);
    CHECK(seen->state == SHIP_FLYING && seen->stop == STOP_PLANET);
    CHECK(fabsf(seen->timer - truth->timer) < close_enough);

    // Someone joining mid-flight gets it from the baseline.
    int b = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    rig_run(&r, 0.2);
    const Ship *late = CW_GET(&r.games[b]->world, id, r.games[b]->c_ship, Ship);
    CHECK(late->state == SHIP_FLYING && late->stop == STOP_PLANET);
    CHECK(fabsf(late->timer - truth->timer) < close_enough);

    // Arrival, and both restart their count from the server's.
    truth->timer = truth->flight_seconds - 0.1f;
    rig_run(&r, 0.5);
    CHECK(truth->state == SHIP_DOCKED);
    CHECK(seen->state == SHIP_DOCKED && seen->stop == STOP_PLANET);
    CHECK(late->state == SHIP_DOCKED && late->stop == STOP_PLANET);
    CHECK(fabsf(seen->timer - truth->timer) < close_enough);
    CHECK(fabsf(late->timer - truth->timer) < close_enough);

    // A client whose message is late stops at zero left, and never counts past it.
    Ship *frozen = CW_GET(&r.games[a]->world, id, r.games[a]->c_ship, Ship);
    frozen->timer = frozen->dock_seconds - 0.05f;
    rig_run(&r, 0.3);
    CHECK(frozen->timer == frozen->dock_seconds);
    rig_destroy(&r);
}

// Joining and staying joined through a lossy link: the baseline is reliable, the
// snapshots keep coming, and nothing arrives twice.
static void test_join_and_play_under_loss(void) {
    rig r;
    rig_init(&r, 0.25f);
    int a = rig_add_client(&r, 0.25f);
    int b = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 8.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.5);
    server *s = r.zone;
    CHECK(cw_world_entity_count(&r.games[a]->world) == cw_world_entity_count(&s->zone.world));
    CHECK(cw_world_entity_count(&r.games[b]->world) == cw_world_entity_count(&s->zone.world));
    CHECK(r.clients[a]->connection.messages_resent > 0 || s->clients[0].connection.messages_resent > 0);

    r.inputs[a].forward = 1.0f;
    r.inputs[a].jump = true;
    rig_run(&r, 1.0);
    r.inputs[a].forward = 0.0f;
    r.inputs[a].jump = false;
    rig_run(&r, 1.0);
    v3 on_server = position_in(&s->zone, r.clients[a]->player);
    CHECK(distance(on_server, s->zone.spawn) > 3.0f);
    CHECK(distance(position_in(r.games[b], r.clients[a]->player), on_server) < 0.05f);
    CHECK(r.clients[a]->state == NC_CONNECTED && r.clients[b]->state == NC_CONNECTED);
    rig_destroy(&r);
}

// A server that is not there, a full server and a protocol mismatch all end cleanly.
static void test_rejections(void) {
    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_CLIENT);
    net_client *c = (net_client *)malloc(sizeof(net_client));
    CHECK(!net_client_init(c, g, "no.such.host.invalid:1", 0.0f));
    net_client_destroy(c);   // safe on a failed init
    game_destroy(g);

    rig r;
    rig_init(&r, 0.0f);
    r.zone->config.max_clients = 1;   // one seat
    int a = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    int b = rig_add_client(&r, 0.0f);
    double start = net_seconds();
    while (r.clients[b]->state != NC_DISCONNECTED && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(r.clients[b]->state == NC_DISCONNECTED);
    CHECK(r.clients[b]->failure && strstr(r.clients[b]->failure, "full"));
    CHECK(r.clients[a]->state == NC_CONNECTED);
    rig_destroy(&r);
}

int main(void) {
    if (!net_init()) {
        puts("test_net: the network could not be initialised");
        return 1;
    }
    test_reliable_channel_under_loss();
    test_two_clients_see_each_other();
    test_ship_state_reaches_clients();
    test_join_and_play_under_loss();
    test_rejections();
    net_shutdown();
    if (failures) {
        printf("test_net: %d failed\n", failures);
        return 1;
    }
    puts("test_net: ok");
    return 0;
}
