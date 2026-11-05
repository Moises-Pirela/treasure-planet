// The multiplayer stack without a window: zone servers and clients in one process,
// talking over loopback UDP. Real sockets, real time, simulated loss.
#include "../src/net/client.h"
#include "../src/net/orchestrator.h"
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
// Transport: the four channels through loss and reordering
// ---------------------------------------------------------------------------
// Message type 1 is reliable ordered, 2 unreliable, 3 sequenced, 4 reliable
// (unordered); every payload is a u32 counter.
#define TRANSPORT_MESSAGES 300
struct receiver {
    int reliable_received, unreliable_received, out_of_order;
    uint32_t expected;
    int sequenced_received, sequenced_backwards;
    uint32_t sequenced_last;
    int unordered_received, unordered_out_of_order, unordered_duplicates;
    uint32_t unordered_last;
    unsigned char unordered_seen[TRANSPORT_MESSAGES];
    int unknown;
};
static void on_transport_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    receiver *r = (receiver *)user;
    uint32_t value = 0;
    cw_reader_u32(&payload, &value);
    switch (type) {
    case 1:
        if (value != r->expected) ++r->out_of_order;
        r->expected = value + 1;
        ++r->reliable_received;
        break;
    case 2:
        ++r->unreliable_received;
        break;
    case 3:
        if (r->sequenced_received && value <= r->sequenced_last) ++r->sequenced_backwards;
        r->sequenced_last = value;
        ++r->sequenced_received;
        break;
    case 4:
        if (value < TRANSPORT_MESSAGES) {
            if (r->unordered_seen[value]++) ++r->unordered_duplicates;
        }
        if (r->unordered_received && value < r->unordered_last) ++r->unordered_out_of_order;
        r->unordered_last = value;
        ++r->unordered_received;
        break;
    default:
        ++r->unknown;
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
// Pumps a socket, as at the instant `now`, until `condition` holds or `seconds` of
// real time have passed. A datagram sent on the loopback is not always there to be
// read the moment the send returns (macOS's loopback hands it over a little later
// than Linux's), so a test that counts what arrived waits for it to arrive; the
// connection still sees every packet at `now`.
// One datagram from a socket, waiting up to `seconds` for it; its size, or 0.
static int recv_within(net_socket *socket, unsigned char *packet, size_t capacity, double seconds) {
    double until = net_seconds() + seconds;
    for (;;) {
        int size = net_socket_recv(socket, NULL, packet, capacity);
        if (size > 0 || net_seconds() >= until) return size > 0 ? size : 0;
        net_sleep(0.001);
    }
}
#define PUMP_UNTIL(socket, c, now, seconds, condition) do { \
    double until_ = net_seconds() + (seconds); \
    for (;;) { \
        pump_socket(socket, c, now); \
        if ((condition) || net_seconds() >= until_) break; \
        net_sleep(0.001); \
    } \
} while (0)

static void test_channels_under_loss(void) {
    net_socket *a = net_socket_open(0), *b = net_socket_open(0);
    CHECK(a && b);
    net_socket_set_loss(a, 0.3f);
    net_socket_set_loss(b, 0.3f);
    net_socket_set_reorder(a, 0.3f);
    net_address loopback_a = net_address_make(0x7f000001u, net_socket_port(a));
    net_address loopback_b = net_address_make(0x7f000001u, net_socket_port(b));
    receiver ra = {}, rb = {};
    net_connection ca, cb;
    net_connection_init(&ca, a, loopback_b, 42, on_transport_message, &ra);
    net_connection_init(&cb, b, loopback_a, 42, on_transport_message, &rb);

    // Several windows' worth on every channel, so the backlog is exercised too.
    // Sent a few at a time so that the unreliable ones are spread over packets.
    const uint32_t total = TRANSPORT_MESSAGES;
    uint32_t sent = 0;
    double start = net_seconds();
    while ((rb.reliable_received < (int)total || rb.unordered_received < (int)total) && net_seconds() - start < 15.0) {
        double now = net_seconds();
        for (int k = 0; k < 3 && sent < total; ++k, ++sent) {
            unsigned char payload[4] = {(unsigned char)sent, (unsigned char)(sent >> 8), 0, 0};
            CHECK(net_connection_send(&ca, 1, NET_RELIABLE_ORDERED, payload, 4));
            CHECK(net_connection_send(&ca, 2, NET_UNRELIABLE, payload, 4));
            CHECK(net_connection_send(&ca, 3, NET_SEQUENCED, payload, 4));
            CHECK(net_connection_send(&ca, 4, NET_RELIABLE, payload, 4));
        }
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);   // b only carries acks
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        net_sleep(0.001);
    }
    CHECK(rb.reliable_received == (int)total && rb.out_of_order == 0);   // everything, in order
    CHECK(rb.unordered_received == (int)total && rb.unordered_duplicates == 0);   // everything, once
    CHECK(rb.unordered_out_of_order > 0);                          // ...as it came, not held back
    CHECK(rb.unreliable_received < (int)total);                    // some were lost, as they should be
    CHECK(rb.sequenced_received < (int)total && rb.sequenced_backwards == 0);   // never an older one
    CHECK(cb.messages_stale > 0);                                  // the reordering was caught
    CHECK(cb.packets_received > 0 && ca.messages_resent > 0 && rb.unknown == 0);
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
    net_socket_set_reorder(a, 0.0f);
    net_socket_set_loss(a, 0.0f);
    net_connection stranger;
    net_connection_init(&stranger, a, loopback_b, 7, NULL, NULL);
    net_connection_send(&stranger, 1, NET_UNRELIABLE, "x", 1);
    net_connection_flush(&stranger, net_seconds(), false);
    int before = rb.reliable_received + rb.unreliable_received;
    PUMP_UNTIL(b, &cb, net_seconds(), 2.0, cb.packets_rejected >= 1);
    CHECK(rb.reliable_received + rb.unreliable_received == before);
    CHECK(cb.packets_rejected == 1);
    net_connection_destroy(&stranger);

    net_connection_destroy(&ca);
    net_connection_destroy(&cb);
    net_socket_close(a);
    net_socket_close(b);
}

// Packet validation: nothing malformed is delivered, and nothing crashes. A valid
// packet in the clear and one encrypted are truncated, padded, and mutated at
// random; the counters say why each was refused.
static void test_packet_validation(void) {
    net_socket *a = net_socket_open(0), *b = net_socket_open(0);
    net_address to_a = net_address_make(0x7f000001u, net_socket_port(a));
    net_address to_b = net_address_make(0x7f000001u, net_socket_port(b));
    for (int encrypted = 0; encrypted < 2; ++encrypted) {
        receiver rb = {};
        net_connection ca, cb;
        net_connection_init(&ca, a, to_b, 5, NULL, NULL);
        net_connection_init(&cb, b, to_a, 5, on_transport_message, &rb);
        if (encrypted) {
            uint8_t key[32];
            for (int i = 0; i < 32; ++i) key[i] = (uint8_t)(i * 7);
            net_connection_set_keys(&ca, key, key);
            net_connection_set_keys(&cb, key, key);
        }
        // A packet with one message of every channel.
        unsigned char payload[4] = {1, 0, 0, 0};
        const int channel_of_type[5] = {0, NET_RELIABLE_ORDERED, NET_UNRELIABLE, NET_SEQUENCED, NET_RELIABLE};
        for (int t = 1; t <= 4; ++t) CHECK(net_connection_send(&ca, (uint8_t)t, channel_of_type[t], payload, 4));
        net_connection_flush(&ca, net_seconds(), false);
        unsigned char packet[NET_MAX_PACKET + 64];
        int size = recv_within(b, packet, sizeof(packet), 2.0);
        CHECK(size > 0);
        // Delivered once. Again: the reliable and sequenced messages are refused as
        // duplicates and stale, the unreliable one is delivered again; encrypted,
        // the whole packet is a replay.
        CHECK(net_connection_receive(&cb, packet, (size_t)size, net_seconds()));
        CHECK(rb.reliable_received == 1 && rb.unreliable_received == 1 && rb.sequenced_received == 1 && rb.unordered_received == 1);
        CHECK(net_connection_receive(&cb, packet, (size_t)size, net_seconds()) == !encrypted);
        if (encrypted) CHECK(cb.packets_replayed == 1);
        else CHECK(cb.messages_duplicate == 2 && cb.messages_stale == 1 && rb.unreliable_received == 2);
        CHECK(rb.reliable_received == 1 && rb.sequenced_received == 1 && rb.unordered_received == 1);
        cb.packets_replayed = 0;
        uint32_t seed = 12345u;
        int delivered_before = rb.reliable_received + rb.unreliable_received + rb.sequenced_received + rb.unordered_received;
        // Truncations: every prefix of the packet. In the clear a cut at a message
        // boundary is simply a shorter packet; under the tag nothing short passes.
        int truncations_accepted = 0;
        for (int n = 0; n < size; ++n) truncations_accepted += net_connection_receive(&cb, packet, (size_t)n, net_seconds());
        CHECK(encrypted ? truncations_accepted == 0 : truncations_accepted == NET_CHANNEL_COUNT);
        // Too long: more than a packet may be.
        CHECK(!net_connection_receive(&cb, packet, NET_MAX_PACKET + 1, net_seconds()));
        uint32_t attempts = (uint32_t)size + 1 + 2000 + 500 + 2;   // the two deliveries above included
        // Random mutations of one byte (not the header: that is a rejection, tested elsewhere).
        int mutations = 0;
        for (int i = 0; i < 2000; ++i) {
            unsigned char mutated[NET_MAX_PACKET];
            memcpy(mutated, packet, (size_t)size);
            seed = seed * 1664525u + 1013904223u;
            int at = NET_PACKET_HEADER + (int)(seed >> 8) % (size - NET_PACKET_HEADER);
            seed = seed * 1664525u + 1013904223u;
            unsigned char flip = (unsigned char)((seed >> 8) & 0xff);
            if (!flip) flip = 1;
            mutated[at] ^= flip;
            if (!net_connection_receive(&cb, mutated, (size_t)size, net_seconds())) ++mutations;
        }
        // Random garbage behind a good header.
        for (int i = 0; i < 500; ++i) {
            unsigned char garbage[NET_MAX_PACKET];
            memcpy(garbage, packet, NET_PACKET_HEADER);
            seed = seed * 1664525u + 1013904223u;
            int n = NET_PACKET_HEADER + (int)(seed >> 8) % (NET_MAX_PACKET - NET_PACKET_HEADER);
            for (int k = NET_PACKET_HEADER; k < n; ++k) {
                seed = seed * 1664525u + 1013904223u;
                garbage[k] = (unsigned char)(seed >> 8);
            }
            net_connection_receive(&cb, garbage, (size_t)n, net_seconds());
        }
        if (encrypted) {
            // Under the tag nothing gets through but the original.
            CHECK(mutations == 2000);
            CHECK(cb.packets_forged > 0 && cb.packets_forged + cb.packets_malformed + cb.packets_rejected + 2 == attempts);   // + the delivery and the replay
            CHECK(rb.reliable_received + rb.unreliable_received + rb.sequenced_received + rb.unordered_received == delivered_before);
            CHECK(cb.packets_received == 1);
        } else {
            // In the clear a mutation may land in a payload, which is delivered as
            // it is; one in the framing is refused with the whole packet.
            CHECK(cb.packets_malformed > 0 && cb.packets_forged == 0 && cb.packets_replayed == 0);
            CHECK(cb.packets_received + cb.packets_malformed + cb.packets_rejected == attempts);   // every one accounted for
        }
        net_connection_destroy(&ca);
        net_connection_destroy(&cb);
    }
    net_socket_close(a);
    net_socket_close(b);
}

// Inbound rate limiting: a peer sending more than its allowance is dropped unread.
static void test_inbound_limit(void) {
    net_socket *a = net_socket_open(0), *b = net_socket_open(0);
    net_address to_a = net_address_make(0x7f000001u, net_socket_port(a));
    net_address to_b = net_address_make(0x7f000001u, net_socket_port(b));
    receiver rb = {};
    net_connection ca, cb;
    net_connection_init(&ca, a, to_b, 9, NULL, NULL);
    net_connection_init(&cb, b, to_a, 9, on_transport_message, &rb);
    net_connection_set_inbound_limit(&cb, 50, 0);
    double now = net_seconds();
    for (int i = 0; i < 200; ++i) {
        unsigned char payload[4] = {(unsigned char)i, 0, 0, 0};
        net_connection_send(&ca, 2, NET_UNRELIABLE, payload, 4);
        net_connection_flush(&ca, now, false);
        pump_socket(b, &cb, now);   // the same instant: only the burst gets through
    }
    // Every one of the two hundred read, still at that instant, however late the
    // loopback hands the last of them over.
    PUMP_UNTIL(b, &cb, now, 2.0, rb.unreliable_received + (int)cb.packets_throttled >= 200);
    CHECK(rb.unreliable_received == 50);
    CHECK(cb.packets_throttled == 150);
    unsigned char payload[4] = {0, 0, 0, 0};
    net_connection_send(&ca, 2, NET_UNRELIABLE, payload, 4);
    net_connection_flush(&ca, now, false);
    PUMP_UNTIL(b, &cb, now + 0.1, 2.0, rb.unreliable_received >= 51);   // a tenth of a second later: five more are allowed
    CHECK(rb.unreliable_received == 51);
    net_connection_destroy(&ca);
    net_connection_destroy(&cb);
    net_socket_close(a);
    net_socket_close(b);
}

// Round trip and loss, from the packet acks: measured on a lossless loopback, then
// through 20% loss.
static void test_rtt_and_loss(void) {
    for (int lossy = 0; lossy < 2; ++lossy) {
        net_socket *a = net_socket_open(0), *b = net_socket_open(0);
        if (lossy) net_socket_set_loss(a, 0.2f);
        net_address to_a = net_address_make(0x7f000001u, net_socket_port(a));
        net_address to_b = net_address_make(0x7f000001u, net_socket_port(b));
        receiver rb = {};
        net_connection ca, cb;
        net_connection_init(&ca, a, to_b, 3, NULL, NULL);
        net_connection_init(&cb, b, to_a, 3, on_transport_message, &rb);
        double start = net_seconds();
        int sent = 0;
        while (sent < 400) {
            unsigned char payload[4] = {(unsigned char)sent, 0, 0, 0};
            net_connection_send(&ca, 2, NET_UNRELIABLE, payload, 4);
            net_connection_flush(&ca, net_seconds(), false);
            ++sent;
            net_sleep(0.001);
            pump_socket(b, &cb, net_seconds());
            net_connection_flush(&cb, net_seconds(), true);   // b answers every packet with an ack
            net_sleep(0.001);
            pump_socket(a, &ca, net_seconds());
        }
        CHECK(net_seconds() - start < 5.0);
        CHECK(ca.rtt_samples > 100 && ca.rtt > 0.0 && ca.rtt < 0.05 && ca.rtt_min <= ca.rtt);
        CHECK(ca.rto >= NET_RTO_MIN && ca.rto <= NET_RTO_MAX);
        if (lossy) {
            CHECK(ca.packets_lost > 30 && ca.packets_lost < 150);   // about a fifth, judged as the acks pass them by
            CHECK(ca.loss > 0.05 && ca.loss < 0.4);
        } else {
            CHECK(ca.packets_lost == 0 && ca.loss == 0.0);
        }
        net_connection_destroy(&ca);
        net_connection_destroy(&cb);
        net_socket_close(a);
        net_socket_close(b);
    }
}

// Fragmentation: messages far larger than a packet, on every channel, through loss
// and reordering. Reliable ones arrive whole, once, byte for byte.
struct big_receiver {
    int whole[NET_CHANNEL_COUNT], wrong[NET_CHANNEL_COUNT];
};
static uint32_t big_hash(const unsigned char *data, size_t size) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}
static void on_big_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    big_receiver *r = (big_receiver *)user;
    int channel = type - 1;
    if (channel < 0 || channel >= NET_CHANNEL_COUNT) return;
    size_t size = cw_reader_remaining(&payload);
    const unsigned char *data = (const unsigned char *)cw_reader_view(&payload, size);
    uint32_t expected = 0;
    bool ok = size == 40000 + (size_t)channel * 1000;
    if (ok) {
        memcpy(&expected, data, 4);
        ok = big_hash(data + 4, size - 4) == expected;
    }
    if (ok) ++r->whole[channel];
    else ++r->wrong[channel];
}
static void test_fragmentation(void) {
    net_socket *a = net_socket_open(0), *b = net_socket_open(0);
    net_socket_set_loss(a, 0.2f);
    net_socket_set_reorder(a, 0.3f);
    net_address to_a = net_address_make(0x7f000001u, net_socket_port(a));
    net_address to_b = net_address_make(0x7f000001u, net_socket_port(b));
    big_receiver rb = {};
    net_connection ca, cb;
    net_connection_init(&ca, a, to_b, 4, NULL, NULL);
    net_connection_init(&cb, b, to_a, 4, on_big_message, &rb);
    unsigned char *message = (unsigned char *)malloc(NET_MAX_MESSAGE);
    uint32_t seed = 99;
    for (int round = 0; round < 3; ++round)
        for (int channel = 0; channel < NET_CHANNEL_COUNT; ++channel) {
            size_t size = 40000 + (size_t)channel * 1000;
            for (size_t i = 4; i < size; ++i) {
                seed = seed * 1664525u + 1013904223u;
                message[i] = (unsigned char)(seed >> 24);
            }
            uint32_t h = big_hash(message + 4, size - 4);
            memcpy(message, &h, 4);
            CHECK(net_connection_send(&ca, (uint8_t)(channel + 1), channel, message, size));
        }
    CHECK(!net_connection_send(&ca, 1, NET_RELIABLE, message, NET_MAX_MESSAGE + 1));   // too large for anyone
    CHECK(ca.fragments_sent >= 12 * 36);
    double start = net_seconds();
    while ((rb.whole[NET_RELIABLE] < 3 || rb.whole[NET_RELIABLE_ORDERED] < 3) && net_seconds() - start < 20.0) {
        double now = net_seconds();
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        net_sleep(0.001);
    }
    CHECK(rb.whole[NET_RELIABLE] == 3 && rb.whole[NET_RELIABLE_ORDERED] == 3);
    CHECK(rb.whole[NET_UNRELIABLE] <= 3 && rb.whole[NET_SEQUENCED] <= 3);   // a lost piece loses the whole
    for (int k = 0; k < NET_CHANNEL_COUNT; ++k) CHECK(rb.wrong[k] == 0);
    CHECK((int)cb.messages_reassembled == rb.whole[0] + rb.whole[1] + rb.whole[2] + rb.whole[3]);
    CHECK(cb.fragments_received > 6 * 36);
    // Whatever was left half-done is let go of in time.
    double later = net_seconds() + NET_REASSEMBLY_TIMEOUT + 0.1;
    net_connection_flush(&cb, later, false);
    for (int i = 0; i < NET_REASSEMBLY_SLOTS; ++i) CHECK(cb.reassembly[i].data == NULL);
    free(message);
    net_connection_destroy(&ca);
    net_connection_destroy(&cb);
    net_socket_close(a);
    net_socket_close(b);
}

// Congestion control, pacing, priorities. A capped connection through loss cuts
// its rate and raises it again when the loss stops; bulk leaves paced rather than
// in a burst; what is marked urgent goes first.
static void test_congestion_and_scheduling(void) {
    net_socket *a = net_socket_open(0), *b = net_socket_open(0);
    net_address to_a = net_address_make(0x7f000001u, net_socket_port(a));
    net_address to_b = net_address_make(0x7f000001u, net_socket_port(b));
    receiver rb = {};
    net_connection ca, cb;
    net_connection_init(&ca, a, to_b, 6, NULL, NULL);
    net_connection_init(&cb, b, to_a, 6, on_transport_message, &rb);
    net_connection_set_rate(&ca, 32768);
    CHECK(net_connection_rate(&ca) == 32768);

    // Pacing: forty packets' worth of bulk, queued at once, leaves two packets at a
    // time and the rest at the rate, not the budget's quarter second in one go.
    unsigned char bulk[1000];
    memset(bulk, 7, sizeof(bulk));
    for (int i = 0; i < 40; ++i) CHECK(net_connection_send_priority(&ca, 4, NET_RELIABLE, NET_PRIORITY_LOW, bulk, sizeof(bulk)));
    double now = net_seconds();
    net_connection_flush(&ca, now, false);
    CHECK(ca.packets_sent >= 1 && ca.packets_sent <= 3);
    uint32_t after_first = ca.packets_sent;
    net_connection_flush(&ca, now + 0.1, false);   // a tenth of a second later: about three more
    CHECK(ca.packets_sent - after_first >= 2 && ca.packets_sent - after_first <= 5);

    // Priority: with the bulk still queued, a normal message and then a high one
    // are what the receiver gets first.
    unsigned char urgent[4] = {1, 0, 0, 0};
    CHECK(net_connection_send_priority(&ca, 4, NET_RELIABLE, NET_PRIORITY_NORMAL, urgent, 4));
    urgent[0] = 2;
    CHECK(net_connection_send_priority(&ca, 4, NET_RELIABLE, NET_PRIORITY_HIGH, urgent, 4));
    urgent[0] = 3;
    CHECK(net_connection_send_priority(&ca, 1, NET_RELIABLE_ORDERED, NET_PRIORITY_LOW, urgent, 4));   // ordered, low: with the bulk
    pump_socket(b, &cb, net_seconds());
    int before = rb.unordered_received;
    double start = net_seconds();
    while (rb.unordered_received < before + 2 && net_seconds() - start < 5.0) {
        now = net_seconds();
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        net_sleep(0.001);
    }
    // The first two unordered messages received after the urgent ones were queued
    // were those: 2 (high) then 1 (normal), ahead of the bulk.
    CHECK(rb.unordered_received >= before + 2 && rb.unordered_last == 1);
    start = net_seconds();
    while (net_connection_unacked(&ca) && net_seconds() - start < 10.0) {
        now = net_seconds();
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        net_sleep(0.001);
    }
    CHECK(net_connection_unacked(&ca) == 0 && rb.reliable_received == 1);
    CHECK(ca.congestion_cuts == 0 && net_connection_rate(&ca) == 32768);

    // Loss: the rate is cut, more than once, never below the floor; then calm, and
    // it climbs back to the cap.
    net_socket_set_loss(a, 0.25f);
    start = net_seconds();
    uint32_t cut_to = 0;
    while (net_seconds() - start < 3.0) {
        now = net_seconds();
        net_connection_send(&ca, 2, NET_UNRELIABLE, urgent, 4);
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        if (net_connection_rate(&ca) < 32768) cut_to = net_connection_rate(&ca);
        net_sleep(0.001);
    }
    CHECK(ca.congestion_cuts >= 2 && cut_to > 0 && cut_to < 32768 && cut_to >= (uint32_t)(32768 * NET_CONGESTION_FLOOR));
    CHECK(ca.packets_lost > 20);
    net_socket_set_loss(a, 0.0f);
    start = net_seconds();
    while (net_connection_rate(&ca) < 32768 && net_seconds() - start < 15.0) {
        now = net_seconds();
        net_connection_send(&ca, 2, NET_UNRELIABLE, urgent, 4);
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        net_sleep(0.001);
    }
    CHECK(net_connection_rate(&ca) == 32768 && ca.congestion_raises > 0);
    // Delay does it too: a round trip well above its floor is congestion.
    ca.rtt = ca.rtt_min + NET_CONGESTION_DELAY * 2.0;
    ca.congestion_last_cut = 0.0;
    net_connection_flush(&ca, net_seconds(), false);
    CHECK(net_connection_rate(&ca) < 32768);
    net_connection_destroy(&ca);
    net_connection_destroy(&cb);
    net_socket_close(a);
    net_socket_close(b);
}

// ---------------------------------------------------------------------------
// Cryptography: the RFC test vectors, then an encrypted channel that refuses
// tampering and replay
// ---------------------------------------------------------------------------
static void from_hex(uint8_t *out, const char *hex) {
    for (size_t i = 0; hex[2 * i]; ++i) {
        unsigned value = 0;
        for (size_t k = 0; k < 2; ++k) {
            char c = hex[2 * i + k];
            value = value * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c - '0');
        }
        out[i] = (uint8_t)value;
    }
}
static bool equals_hex(const void *data, const char *hex) {
    uint8_t expected[128];
    from_hex(expected, hex);
    return memcmp(data, expected, strlen(hex) / 2) == 0;
}

static void test_crypto(void) {
    uint8_t key[32], nonce[12], out[64], tag[16];
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;   // RFC 8439 2.3.2
    from_hex(nonce, "000000090000004a00000000");
    crypto_chacha20_block(key, 1, nonce, out);
    CHECK(equals_hex(out, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e"));
    from_hex(key, "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");   // 2.5.2
    const char *message = "Cryptographic Forum Research Group";
    crypto_poly1305(key, message, strlen(message), tag);
    CHECK(equals_hex(tag, "a8061dc1305136c6c22b8baf0c0127a9"));
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)(0x80 + i);   // 2.8.2
    from_hex(nonce, "070000004041424344454647");
    uint8_t aad[12];
    from_hex(aad, "50515253c0c1c2c3c4c5c6c7");
    const char *plain = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    uint8_t cipher[128], back[128];
    crypto_aead_encrypt(key, nonce, aad, 12, plain, strlen(plain), cipher, tag);
    CHECK(equals_hex(cipher, "d31a8d34648e60db7b86afbc53ef7ec2"));
    CHECK(equals_hex(tag, "1ae10b594f09e26a7e902ecbd0600691"));
    CHECK(crypto_aead_decrypt(key, nonce, aad, 12, cipher, strlen(plain), tag, back) && memcmp(back, plain, strlen(plain)) == 0);
    cipher[5] ^= 1;
    CHECK(!crypto_aead_decrypt(key, nonce, aad, 12, cipher, strlen(plain), tag, back));
    // RFC 7748 6.1: Alice and Bob.
    uint8_t a_secret[32], a_public[32], b_secret[32], b_public[32], shared_a[32], shared_b[32];
    from_hex(a_secret, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    from_hex(b_secret, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    crypto_x25519_base(a_public, a_secret);
    crypto_x25519_base(b_public, b_secret);
    CHECK(equals_hex(a_public, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK(equals_hex(b_public, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    crypto_x25519(shared_a, a_secret, b_public);
    crypto_x25519(shared_b, b_secret, a_public);
    CHECK(equals_hex(shared_a, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    CHECK(memcmp(shared_a, shared_b, 32) == 0);
    uint8_t k1[32], k2[32];
    CHECK(crypto_keypair(k1, k2) && memcmp(k1, k2, 32) != 0);

    // An encrypted channel between two connections.
    net_socket *a = net_socket_open(0), *b = net_socket_open(0);
    net_address to_a = net_address_make(0x7f000001u, net_socket_port(a)), to_b = net_address_make(0x7f000001u, net_socket_port(b));
    receiver ra = {}, rb = {};
    net_connection ca, cb;
    net_connection_init(&ca, a, to_b, 99, on_transport_message, &ra);
    net_connection_init(&cb, b, to_a, 99, on_transport_message, &rb);
    uint8_t ab[32], ba[32];
    crypto_session_keys(shared_a, a_public, b_public, ab, ba);
    net_connection_set_keys(&ca, ab, ba);
    net_connection_set_keys(&cb, ba, ab);
    for (uint32_t i = 0; i < 20; ++i) {
        unsigned char payload[4] = {(unsigned char)i, 0, 0, 0};
        net_connection_send(&ca, 1, NET_RELIABLE_ORDERED, payload, 4);
    }
    double start = net_seconds();
    while (rb.reliable_received < 20 && net_seconds() - start < 5.0) {
        double now = net_seconds();
        net_connection_flush(&ca, now, false);
        net_connection_flush(&cb, now, true);
        pump_socket(b, &cb, now);
        pump_socket(a, &ca, now);
        net_sleep(0.001);
    }
    CHECK(rb.reliable_received == 20 && rb.out_of_order == 0);
    // The bytes on the wire are not the message: nothing of the payload shows.
    unsigned char probe[4] = {0x5a, 0x5a, 0x5a, 0x5a};
    net_connection_send(&ca, 2, NET_UNRELIABLE, probe, 4);
    net_connection_flush(&ca, net_seconds(), false);
    unsigned char packet[NET_MAX_PACKET];
    int size = recv_within(b, packet, sizeof(packet), 2.0);
    CHECK(size > NET_PACKET_HEADER + 8 + CRYPTO_TAG_SIZE);
    bool leaked = false;
    for (int i = NET_PACKET_HEADER; i + 4 <= size; ++i) leaked = leaked || memcmp(packet + i, probe, 4) == 0;
    CHECK(!leaked);
    // Delivered once; replayed, refused; tampered with, refused.
    int before = rb.unreliable_received;
    CHECK(net_connection_receive(&cb, packet, (size_t)size, net_seconds()));
    CHECK(rb.unreliable_received == before + 1);
    CHECK(!net_connection_receive(&cb, packet, (size_t)size, net_seconds()));
    CHECK(cb.packets_replayed == 1);
    packet[size - 3] ^= 0x80;
    CHECK(!net_connection_receive(&cb, packet, (size_t)size, net_seconds()));
    CHECK(cb.packets_forged == 1);
    CHECK(rb.unreliable_received == before + 1);
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
                                                           -0.2f, i % 3 == 0, (uint16_t)i, 0);
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
    current->entries[current->count++] = snapshot_entry_make(99, CW_INVALID_ENTITY, v3_make(5, 5, 5), v3_make(0, 0, 0), 0, 0, true, 0, 0);
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

    // Crafts travel exactly: the bits of their floats, delta-coded. A craft sitting
    // still costs what an idle player does; one that moved, a few bytes a number.
    quat turned = quat_normalize(quat_make(0.01f, 0.7071f, -0.002f, 0.7071f));
    v3 where = v3_make(-1234.56789f, 87.654321f, 0.000123f), going = v3_make(12.3456f, -0.5f, 70.0f), spin = v3_make(0.01f, -0.3f, 0.0f);
    Craft flying = {}, parked = {};
    flying.velocity = parked.velocity = going;
    flying.orientation = parked.orientation = turned;
    flying.spin = parked.spin = spin;
    flying.pilot = 12;
    flying.assist = true;
    flying.throttle[CRAFT_GROUP_UP] = 0.6f;
    flying.masts = 2;
    flying.sails[0].set = 0.7318f;
    flying.sails[0].fill = 0.5f;
    flying.sails[0].brace = -0.61f;
    flying.sails[0].hand = 31;
    flying.sails[1].set = 0.25f;
    flying.sails[1].brace = 0.3f;
    flying.sails[1].hand = CW_INVALID_ENTITY;
    parked.masts = 1;
    parked.sails[0].hand = CW_INVALID_ENTITY;
    parked.pilot = CW_INVALID_ENTITY;
    parked.resting = true;
    parked.mode = CRAFT_LANDING;
    parked.quiet_ticks = 30;
    snapshot *craft_base = snapshot_ring_push(&ring, 4);
    craft_base->tick = 200;
    craft_base->entries[craft_base->count++] = snapshot_entry_craft(7, where, &flying);
    craft_base->entries[craft_base->count++] = snapshot_entry_craft(8, where, &parked);
    snapshot *craft_now = snapshot_ring_push(&ring, 4);
    craft_now->tick = 203;
    flying.orientation = quat_integrate(turned, spin, 0.05f);
    flying.throttle[CRAFT_GROUP_FORWARD] = 1.0f;   // the main engines light
    craft_now->entries[craft_now->count++] = snapshot_entry_craft(7, v3_add(where, v3_scale(going, 0.05f)), &flying);
    craft_now->entries[craft_now->count++] = craft_base->entries[1];
    cw_writer_init_fixed(&w, packet, sizeof(packet));
    CHECK(snapshot_encode(craft_now, NULL, &w));
    size_t full = w.size;
    cw_writer_init_fixed(&w, packet, sizeof(packet));
    CHECK(snapshot_encode(craft_now, craft_base, &w));
    CHECK(w.size < full / 2);
    CHECK(w.size < 14 + 2 + 2 + 7 * 4 + 17);   // the still one two bytes; the moving one's position, rotation, engines and sails
    r = cw_reader_make(packet, w.size);
    CHECK(snapshot_decode(&r, craft_base, &decoded));
    CHECK(decoded.count == 2);
    for (uint16_t i = 0; i < decoded.count && i < 2; ++i) {
        snapshot_entry a = decoded.entries[i], b = craft_now->entries[i];
        a.priority = b.priority = 0.0f;
        CHECK(memcmp(&a, &b, sizeof(a)) == 0);
        v3 p, v, sp;
        quat q;
        snapshot_craft_state(&a, &p, &v, &q, &sp);
        v3 p0, v0, sp0;
        quat q0;
        snapshot_craft_state(&b, &p0, &v0, &q0, &sp0);
        CHECK(memcmp(&p, &p0, sizeof(p)) == 0 && memcmp(&q, &q0, sizeof(q)) == 0 && memcmp(&sp, &sp0, sizeof(sp)) == 0);
    }
    CHECK(decoded.entries[0].pilot == 12 && decoded.entries[1].pilot == CW_INVALID_ENTITY);
    CHECK((decoded.entries[1].flags & (SNAP_CRAFT | SNAP_RESTING)) == (SNAP_CRAFT | SNAP_RESTING));
    Craft seen = {};
    v3 at;
    snapshot_craft_apply(&decoded.entries[0], &seen, &at);
    CHECK(seen.pilot == 12 && seen.assist && !seen.resting && seen.mode == CRAFT_FLIGHT);
    CHECK(seen.throttle[CRAFT_GROUP_FORWARD] == 1.0f && fabsf(seen.throttle[CRAFT_GROUP_UP] - 0.6f) < 0.003f);
    CHECK(seen.sails[0].set == 0.7318f && fabsf(seen.sails[0].fill - 0.5f) < 0.01f && seen.sails[0].brace == -0.61f && seen.sails[0].hand == 31);
    CHECK(seen.sails[1].set == 0.25f && seen.sails[1].brace == 0.3f && seen.sails[1].hand == CW_INVALID_ENTITY);
    CHECK(seen.sails[2].set == 0.0f && seen.sails[2].hand == CW_INVALID_ENTITY);   // no third mast
    snapshot_craft_apply(&decoded.entries[1], &seen, &at);
    CHECK(seen.resting && seen.mode == CRAFT_LANDING && seen.quiet_ticks == 30 && seen.throttle[CRAFT_GROUP_UP] == 0.0f);
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
#define RIG_MAX 8
struct rig {
    server *servers[RIG_MAX];
    int server_count;
    directory *dir;              // pumped along with the rest when present
    orchestrator *orch;
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
    config.linger_quit = 0.5;      // bodies linger briefly in tests
    config.linger_timeout = 12.0;  // the client notices a lost connection some seconds after the server
    config.craft_linger = 0.5;     // and crafts wait for their owners briefly
    rig_add_server(r, &config, server_loss);
}

static int rig_add_client_named(rig *r, server *s, float loss, const char *name) {
    int i = r->client_count++;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%u", (unsigned)server_port(s));
    net_identity identity = net_identity_local(name);
    CHECK(net_session_init(&r->sessions[i], address, 0.0f, name ? &identity : NULL));
    if (loss > 0.0f) net_socket_set_loss(r->sessions[i].link->socket, loss);
    r->used[i] = true;
    return i;
}
static int rig_add_client_to(rig *r, server *s, float loss) { return rig_add_client_named(r, s, loss, NULL); }
static int rig_add_client(rig *r, float loss) { return rig_add_client_to(r, r->servers[0], loss); }

static void rig_drop_client(rig *r, int i) {
    net_session_destroy(&r->sessions[i]);
    r->used[i] = false;
}

static void rig_destroy(rig *r) {
    for (int i = 0; i < r->client_count; ++i) if (r->used[i]) rig_drop_client(r, i);
    for (int i = 0; i < r->server_count; ++i) {
        if (!r->servers[i]) continue;
        server_destroy(r->servers[i]);
        free(r->servers[i]);
    }
    if (r->orch) {
        orchestrator_destroy(r->orch);
        free(r->orch);
    }
    if (r->dir) {
        directory_destroy(r->dir);
        free(r->dir);
    }
}

// One frame of everyone, the way the real programs run it.
static void rig_step(rig *r) {
    double now = net_seconds();
    if (r->dir) directory_update(r->dir, now);
    if (r->orch) orchestrator_update(r->orch, now);
    for (int i = 0; i < r->server_count; ++i)
        if (r->servers[i]) server_update(r->servers[i], now);
    for (int i = 0; i < r->client_count; ++i)
        if (r->used[i]) net_session_frame(&r->sessions[i], &r->inputs[i], now);
    net_sleep(0.001);
}
static void rig_run(rig *r, double seconds) {
    double start = net_seconds();
    while (net_seconds() - start < seconds) rig_step(r);
}
// Steps the rig until `condition` holds or `seconds` have passed: for what comes
// back over the network, which a slow or loaded machine takes longer over than a
// fast one, so that no test hangs on a fixed wait being long enough.
#define RIG_UNTIL(r, seconds, condition) do { \
    double until_ = net_seconds() + (seconds); \
    while (!(condition) && net_seconds() < until_) rig_step(r); \
} while (0)
static bool rig_wait_connected(rig *r, double seconds) {
    double start = net_seconds();
    for (;;) {
        bool all = true;
        for (int i = 0; i < r->client_count; ++i)
            if (r->used[i] && net_session_state(&r->sessions[i]) != NC_CONNECTED) all = false;
        if (all) { if (getenv("TEST_TIMING")) fprintf(stderr, "TIMING connect %.2f s of %.1f\n", net_seconds() - start, seconds); return true; }
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

    // A leaves: A's body lingers a moment (B still sees it), then the server
    // despawns it and B's replica loses the entity.
    cw_entity_id a_player = ca->player;
    rig_drop_client(&r, a);
    rig_run(&r, 0.2);
    CHECK(s->client_count == 1);
    CHECK(cw_world_is_valid(&s->zone.world, a_player) && cw_world_is_valid(&gb->world, a_player));
    CHECK(server_client_of_account(s, account_id_for("local:Alice")) == NULL || s->lingered == 1);
    rig_run(&r, 0.6);
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
    CHECK(rig_wait_connected(&r, 20.0));   // the world through a quarter of it lost each way, at the rate loss allows
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 1.0);   // the other's record, reliable, may take a few tries through the loss
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
    CHECK(!net_session_init(&bad, "no.such.host.invalid:1", 0.0f, NULL));
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
    hub_server->zones = planet_server->zones = zones;   // what a directory would keep fresh
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
    hub_server->zones = dead->zones = zones;
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
// universe time means the same thing on every zone. The clock is read before and
// after each update, and the tick must land between those two readings: exact,
// and not a race against how long a sleep or a check took to run.
static uint64_t update_between(server *s, uint64_t *before, uint64_t *after) {
    *before = server_target_tick(s);
    server_update(s, net_seconds());
    *after = server_target_tick(s);
    return s->zone.world.tick;
}
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
    uint64_t lo, hi;
    uint64_t tick = update_between(s, &lo, &hi);
    CHECK(tick >= lo && tick <= hi);
    CHECK(fabs(game_time_of_tick(&s->zone, tick) - (net_wall_seconds() - config.universe_epoch)) < 0.05);

    // A stall: time passes, no updates. Short ones are simulated in full.
    uint64_t before = s->zone.world.tick;
    net_sleep(0.3);
    tick = update_between(s, &lo, &hi);
    CHECK(tick >= lo && tick <= hi);
    CHECK(tick - before >= 17);
    CHECK(s->ticks_skipped == 0);

    // A long one (here: the clock saying we are 3 s behind) is caught up to a point
    // and the rest skipped.
    before = s->zone.world.tick;
    s->tick_base -= 3.0;
    tick = update_between(s, &lo, &hi);
    CHECK(tick >= lo && tick <= hi);
    CHECK(lo - before >= 180);
    CHECK(s->ticks_skipped >= lo - before - SERVER_MAX_CATCHUP && s->ticks_skipped <= hi - before - SERVER_MAX_CATCHUP);

    // A clock jump (10 s) moves universe time instead of the tick.
    before = s->zone.world.tick;
    s->tick_base -= 10.0;
    server_update(s, net_seconds());
    CHECK(s->rebases == 1);
    CHECK(s->zone.world.tick - before <= 2);
    CHECK(fabs(game_time_of_tick(&s->zone, s->zone.world.tick) - (net_wall_seconds() - config.universe_epoch)) < 0.05);
    server_destroy(s);
    free(s);
}

// Dungeons: each player who walks through a planet's door gets an instance of their
// own, on the same server when no instance host is listed; the exit brings them back.
static void test_solo_instances(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    s->config.instance_linger = 1.0;
    int a = rig_add_client(&r, 0.0f), b = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    uint64_t account_a = ca->account;
    CHECK(s->world_count == 1);

    // A steps through Verdant's door (set there by the server, in place of the trip).
    char dungeon[GAME_NAME_CAPACITY];
    game_dungeon_name("Verdant", dungeon, sizeof(dungeon));
    int door = game_find_place(&s->zone, dungeon);
    CHECK(door >= 0 && s->zone.places[door].kind == PLACE_ENTRANCE);
    cw_entity_id a_outside = ca->player;
    Transform *at = CW_GET(&s->zone.world, a_outside, s->zone.c_transform, Transform);
    at->position = at->previous = s->zone.places[door].center;
    double start = net_seconds();
    while (r.sessions[a].handoffs == 0 && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(r.sessions[a].handoffs == 1);
    rig_run(&r, 0.3);
    ca = link_of(&r, a);
    game *ga = world_of(&r, a);
    CHECK(ca->state == NC_CONNECTED);
    CHECK(ca->server.port == server_port(s));                 // the same server...
    CHECK(strcmp(ca->world_name, dungeon) == 0);              // ...another world
    CHECK(ca->account == account_a);
    CHECK(s->world_count == 2 && s->instances_opened == 1);
    server_world *instance = server_find_instance(s, dungeon, account_a);
    CHECK(instance && instance->client_count == 1);
    if (!instance) { rig_destroy(&r); return; }
    CHECK(cw_world_entity_count(&ga->world) == cw_world_entity_count(&instance->g->world));
    CHECK(cw_query_count(ga->q_ships) == 0);                  // no universe in here
    CHECK(cw_query_count(ga->q_all_players) == 1);            // and nobody else
    CHECK(game_find_place(ga, "exit") >= 0);
    CHECK(distance(position_in(ga, ca->player), instance->g->spawn) < 0.5f);
    // Outside, A is gone from the universe, and from B's view of it.
    CHECK(!cw_world_is_valid(&s->zone.world, a_outside));
    CHECK(!cw_world_is_valid(&world_of(&r, b)->world, a_outside));
    CHECK(s->zone_world->client_count == 1);

    // B goes through the same door and gets a dungeon of their own.
    at = CW_GET(&s->zone.world, cb->player, s->zone.c_transform, Transform);
    at->position = at->previous = s->zone.places[door].center;
    start = net_seconds();
    while (r.sessions[b].handoffs == 0 && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(r.sessions[b].handoffs == 1);
    rig_run(&r, 0.3);
    cb = link_of(&r, b);
    CHECK(s->world_count == 3 && s->instances_opened == 2);
    CHECK(strcmp(cb->world_name, dungeon) == 0);
    CHECK(cw_query_count(world_of(&r, b)->q_all_players) == 1);
    CHECK(cw_query_count(ga->q_all_players) == 1);
    CHECK(s->zone_world->client_count == 0);
    // A still plays: prediction holds in here too.
    r.inputs[a].forward = 1.0f;
    rig_run(&r, 0.5);
    r.inputs[a].forward = 0.0f;
    rig_run(&r, 0.3);
    CHECK(distance(position_in(ga, ca->player), instance->g->spawn) > 2.0f);
    CHECK(ca->last_error < 0.002f);

    // A reaches the exit (set there by the server) and is back on the pad, in front
    // of the door, in the universe; the empty instance closes a little later.
    int exit_place = game_find_place(instance->g, "exit");
    server_client *sc = server_client_of_account(s, account_a);
    CHECK(sc && sc->world == instance);
    at = CW_GET(&instance->g->world, sc->player, instance->g->c_transform, Transform);
    at->position = at->previous = instance->g->places[exit_place].center;
    start = net_seconds();
    while (r.sessions[a].handoffs == 1 && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(r.sessions[a].handoffs == 2);
    rig_run(&r, 0.3);
    ca = link_of(&r, a);
    ga = world_of(&r, a);
    CHECK(ca->world_name[0] == '\0');
    CHECK(cw_query_count(ga->q_ships) == 3);
    CHECK(distance(position_in(ga, ca->player), s->zone.places[door].spawn) < 0.5f);
    CHECK(game_place_at(&s->zone, position_in(&s->zone, ca->player)) == game_find_place(&s->zone, "Verdant"));
    CHECK(s->zone_world->client_count == 1);
    rig_run(&r, 1.5);
    CHECK(s->instances_closed == 1 && s->world_count == 2);
    CHECK(server_find_instance(s, dungeon, account_a) == NULL);
    CHECK(link_of(&r, b)->state == NC_CONNECTED);   // B's dungeon is unaffected
    CHECK(s->superseded == 0);                       // every old body left of its own accord
    CHECK(s->client_count == 2);
    rig_destroy(&r);
}

// A tight bandwidth cap: joining still works (the baseline is paced), snapshots
// shrink to the budget, and what arrives stays exact.
static void test_bandwidth_limit(void) {
    rig r;
    memset(&r, 0, sizeof(r));
    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = RIG_MAX;
    config.bandwidth = 3000;   // bytes per second per client
    server *s = rig_add_server(&r, &config, 0.0f);
    for (int i = 0; i < 4; ++i) rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 30.0));   // the world at 3 KB/s: some seconds
    if (failures) { rig_destroy(&r); return; }
    for (int i = 0; i < 4; ++i) r.inputs[i].forward = 1.0f;
    r.inputs[1].yaw = 1.0f;
    r.inputs[2].yaw = 2.0f;
    r.inputs[3].yaw = 3.0f;
    rig_run(&r, 0.5);
    uint64_t received[4];
    for (int i = 0; i < 4; ++i) received[i] = link_of(&r, i)->connection.bytes_received;
    rig_run(&r, 2.0);
    for (int i = 0; i < 4; ++i) {
        uint64_t bytes = link_of(&r, i)->connection.bytes_received - received[i];
        CHECK(bytes < (uint64_t)(2.0 * 3000 * 1.3));   // within the cap (plus the bucket's quarter second)
        CHECK(bytes > (uint64_t)(2.0 * 3000 * 0.3));   // and not starved
        CHECK(link_of(&r, i)->state == NC_CONNECTED);
        CHECK(link_of(&r, i)->last_error < 0.002f);
        CHECK(link_of(&r, i)->snapshots_received > 20);
    }
    // Everyone has moved, and every replica agrees with the server to the millimetre.
    for (int i = 0; i < 4; ++i) r.inputs[i].forward = 0.0f;
    rig_run(&r, 0.6);
    for (int i = 0; i < 4; ++i) {
        cw_entity_id me = link_of(&r, i)->player;
        CHECK(distance(position_in(&s->zone, me), s->zone.spawn) > 3.0f);
        for (int k = 0; k < 4; ++k)
            CHECK(distance(position_in(world_of(&r, k), me), position_in(&s->zone, me)) < 0.01f);
    }
    rig_destroy(&r);
}

// Lag compensation: B walks past A. A sees B where B was a little earlier (the
// interpolation delay and the trip), and a swing aimed there lands, because the
// server judges it against where B was at that moment. A swing aimed at where the
// server has B right now, which A could only know by cheating, misses.
static void test_lag_compensation(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    int a = rig_add_client(&r, 0.0f), b = rig_add_client(&r, 0.0f);
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    game *ga = world_of(&r, a);
    v3 post = v3_make(1.5f, 0.0f, 0.0f);   // A stands beside B's path, which runs up z through the origin
    Transform *ta = CW_GET(&s->zone.world, ca->player, s->zone.c_transform, Transform);
    ta->position = ta->previous = post;
    rig_run(&r, 0.3);
    CHECK(distance(position_in(ga, ca->player), post) < 0.01f);

    for (int pass = 0; pass < 2; ++pass) {
        bool cheating = pass == 0;
        Transform *tb = CW_GET(&s->zone.world, cb->player, s->zone.c_transform, Transform);
        tb->position = tb->previous = v3_make(0.0f, 0.0f, -6.0f);
        r.inputs[b].forward = 1.0f;
        r.inputs[b].yaw = 0.0f;   // yaw 0 walks towards -z...
        r.inputs[b].yaw = TP_PI;  // ...so face the other way
        rig_run(&r, 0.3);
        uint16_t hits_before = CW_GET(&s->zone.world, cb->player, s->zone.c_player, Player)->hits_taken;
        bool swung = false;
        float gap_at_swing = 0.0f;
        double start = net_seconds();
        while (!swung && net_seconds() - start < 3.0) {
            rig_step(&r);
            r.inputs[a].act = false;
            v3 seen = position_in(ga, cb->player);                   // where A draws B
            v3 truth = position_in(&s->zone, cb->player);            // where the server has B
            v3 target = cheating ? truth : seen;
            if (target.z < -0.25f || target.z > 0.25f) continue;
            v3 eye = v3_add(position_in(ga, ca->player), v3_make(0, PLAYER_EYE, 0));
            v3 d = v3_sub(v3_add(target, v3_make(0, PLAYER_EYE, 0)), eye);
            r.inputs[a].yaw = atan2f(-d.x, -d.z);
            r.inputs[a].pitch = asinf(d.y / v3_length(d));
            r.inputs[a].act = true;
            gap_at_swing = distance(seen, truth);
            swung = true;
        }
        CHECK(swung);
        rig_step(&r);
        r.inputs[a].act = false;
        rig_run(&r, 0.5);
        r.inputs[b].forward = 0.0f;
        uint16_t taken = CW_GET(&s->zone.world, cb->player, s->zone.c_player, Player)->hits_taken;
        CHECK(gap_at_swing > 0.4f);   // B really is drawn well behind where the server has him
        if (cheating) {
            CHECK(taken == hits_before);
        } else {
            CHECK(taken == hits_before + 1);
            CHECK(CW_GET(&s->zone.world, ca->player, s->zone.c_player, Player)->hits_made == 1);
            CHECK(CW_GET(&ga->world, cb->player, ga->c_player, Player)->hits_taken == 1);   // A sees it land...
            CHECK(CW_GET(&world_of(&r, b)->world, cb->player, world_of(&r, b)->c_player, Player)->hits_taken == 1);   // ...B feels it
        }
        rig_run(&r, 0.6);   // the cooldown, before the next pass
    }
    CHECK(s->swings == 2 && s->hits == 1);
    if (!(s->swings == 2 && s->hits == 1)) printf("  swings %u hits %u\n", s->swings, s->hits);
    rig_destroy(&r);
}

// The directory: servers register and learn of each other, a client finds the hub
// through it and is introduced (the server punches towards it), hand-offs use the
// table it keeps, a server that stops registering drops out, a wrong secret is refused.
static void test_directory(void) {
    rig r;
    memset(&r, 0, sizeof(r));
    r.dir = (directory *)malloc(sizeof(directory));
    CHECK(directory_init(r.dir, 0, "test-secret", false));
    r.dir->expiry = 3.0;   // shorter than the default, longer than the registration interval
    net_address dir_address = net_address_make(0x7f000001u, directory_port(r.dir));
    char dir_text[32];
    snprintf(dir_text, sizeof(dir_text), "127.0.0.1:%u", (unsigned)directory_port(r.dir));

    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = RIG_MAX;
    config.directory = dir_address;
    config.secret = "test-secret";
    strncpy(config.zone_name, "hub", NET_ZONE_NAME - 1);
    strncpy(config.owns[0], "hub", NET_ZONE_NAME - 1);
    config.owns_count = 1;
    server *hub = rig_add_server(&r, &config, 0.0f);
    strncpy(config.zone_name, "planets", NET_ZONE_NAME - 1);
    strncpy(config.owns[0], "Verdant", NET_ZONE_NAME - 1);
    strncpy(config.owns[1], "Ember", NET_ZONE_NAME - 1);
    strncpy(config.owns[2], "Azure", NET_ZONE_NAME - 1);
    config.owns_count = 3;
    server *planets = rig_add_server(&r, &config, 0.0f);
    config.secret = "wrong-secret";
    strncpy(config.zone_name, "impostor", NET_ZONE_NAME - 1);
    server *impostor = rig_add_server(&r, &config, 0.0f);
    if (failures) { rig_destroy(&r); return; }

    // Registration: each learns of the other, and the impostor of nobody.
    double start = net_seconds();
    while ((hub->zones.count < 2 || planets->zones.count < 2) && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(hub->zones.count == 2 && planets->zones.count == 2);
    CHECK(net_zones_owner(&hub->zones, "Verdant") == net_zones_find(&hub->zones, "planets"));
    CHECK(hub->zones.zones[net_zones_find(&hub->zones, "planets")].address.port == server_port(planets));
    CHECK(server_self(hub) >= 0 && hub->zones.zones[server_self(hub)].address.port == server_port(hub));
    CHECK(r.dir->rejected > 0 && impostor->tables_received == 0);
    CHECK(strcmp(hub->zones.secret, "test-secret") == 0);

    // A client asks the directory for the hub and lands on it, punched towards first.
    int a = r.client_count++;
    r.used[a] = true;
    CHECK(net_session_init_directory(&r.sessions[a], dir_text, "hub", 0.0f, NULL));
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    CHECK(link_of(&r, a)->server.port == server_port(hub));
    CHECK(hub->punches_sent >= 1);
    CHECK(link_of(&r, a)->punches_received >= 1);
    CHECK(r.dir->introductions >= 1);

    // A hand-off through the directory's table, with the new server introduced too.
    uint32_t punched_before = planets->punches_sent;
    int verdant = game_find_place(&hub->zone, "Verdant");
    Transform *at = CW_GET(&hub->zone.world, link_of(&r, a)->player, hub->zone.c_transform, Transform);
    at->position = at->previous = hub->zone.places[verdant].spawn;
    start = net_seconds();
    while (r.sessions[a].handoffs == 0 && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(r.sessions[a].handoffs == 1);
    rig_run(&r, 0.3);
    CHECK(link_of(&r, a)->server.port == server_port(planets));
    CHECK(planets->punches_sent > punched_before);
    CHECK(planets->client_count == 1 && hub->client_count == 0);

    // A party founded through the directory's social service.
    net_session_party_request(&r.sessions[a], PARTY_CREATE, NULL);
    start = net_seconds();
    while (net_session_party(&r.sessions[a])->party == 0 && net_seconds() - start < 3.0) rig_step(&r);
    CHECK(net_session_party(&r.sessions[a])->party != 0);
    CHECK(r.dir->people.ops == 1 && r.dir->people.presences > 0);

    // Moving to another server (here: a hand-off back to the hub, set up by the
    // planets server) tells the new server the party: it would be the instance key.
    {
        int hub_place = game_find_place(&planets->zone, "hub");
        Transform *t = CW_GET(&planets->zone.world, link_of(&r, a)->player, planets->zone.c_transform, Transform);
        t->position = t->previous = planets->zone.places[hub_place].spawn;
        start = net_seconds();
        while (r.sessions[a].handoffs == 1 && net_seconds() - start < 5.0) rig_step(&r);
        CHECK(r.sessions[a].handoffs == 2);
        rig_run(&r, 0.5);
        server_client *sc = server_client_of_account(hub, link_of(&r, a)->account);
        CHECK(sc && sc->party == net_session_party(&r.sessions[a])->party && sc->party != 0);
    }

    // The hub stops registering and drops out of the table the planets keep.
    server_destroy(hub);
    free(hub);
    r.servers[0] = NULL;
    start = net_seconds();
    while (net_zones_find(&planets->zones, "hub") >= 0 && net_seconds() - start < 8.0) rig_step(&r);
    CHECK(net_zones_find(&planets->zones, "hub") < 0);
    CHECK(net_zones_find(&planets->zones, "planets") >= 0);
    rig_destroy(&r);
}

// ---------------------------------------------------------------------------
// Parties
// ---------------------------------------------------------------------------
static int pushes_seen;
static void count_push(void *, net_address, uint8_t type, const void *, size_t) { if (type == MSG_SOC_PARTY_STATE) ++pushes_seen; }

static void test_social_service(void) {
    net_zone_table secret;
    net_zones_init(&secret);
    social *soc = (social *)malloc(sizeof(social));
    social_init(soc, &secret, count_push, NULL);
    net_address via = net_address_make(0x7f000001u, 1);
    double now = 100.0;
    social_presence(soc, via, 11, 1, "Alice", "local:alice", "hub", "", PRESENCE_ARRIVED, now);
    social_presence(soc, via, 12, 2, "Bob", "local:bob", "hub", "", PRESENCE_ARRIVED, now);
    social_presence(soc, via, 13, 3, "Carol", "local:carol", "planets", "Verdant Depths", PRESENCE_ARRIVED, now);
    // Carol moves to another connection; the old one's late words about her are ignored.
    net_address via2 = net_address_make(0x7f000001u, 2);
    social_presence(soc, via2, 14, 3, "Carol", "local:carol", "instances", "Verdant Depths", PRESENCE_ARRIVED, now);
    social_presence(soc, via, 13, 3, "Carol", "local:carol", "planets", "", PRESENCE_REFRESH, now);
    social_presence(soc, via, 13, 3, "Carol", "local:carol", "planets", "", PRESENCE_LEFT, now);
    CHECK(social_find(soc, 3)->online && social_find(soc, 3)->via.port == 2 && strcmp(social_find(soc, 3)->world, "Verdant Depths") == 0);
    party_view view;

    // Inviting while alone founds the party; the invitation waits with Bob.
    CHECK(social_party_request(soc, 1, PARTY_INVITE, "bob", now) == PARTY_OK);   // names are case-insensitive
    CHECK(social_view(soc, 1, &view) && view.party != 0 && view.count == 1 && view.members[0].leader);
    CHECK(social_view(soc, 2, &view) && view.party == 0 && view.invite != 0 && strcmp(view.invited_by, "Alice") == 0);
    CHECK(social_party_request(soc, 3, PARTY_ACCEPT, NULL, now) == PARTY_ERROR_NO_INVITE);
    CHECK(social_party_request(soc, 2, PARTY_ACCEPT, NULL, now) == PARTY_OK);
    CHECK(social_view(soc, 2, &view) && view.count == 2 && view.invite == 0);
    CHECK(social_view(soc, 1, &view) && view.count == 2 && view.members[0].account == 1 && view.members[1].account == 2);
    CHECK(strcmp(view.members[1].name, "Bob") == 0 && view.members[1].online);
    // Only the leader invites, kicks and promotes; a member of another party cannot be invited.
    CHECK(social_party_request(soc, 2, PARTY_INVITE, "carol", now) == PARTY_ERROR_NOT_LEADER);
    CHECK(social_party_request(soc, 1, PARTY_INVITE, "nobody", now) == PARTY_ERROR_UNKNOWN_PLAYER);
    CHECK(social_party_request(soc, 1, PARTY_INVITE, "carol", now) == PARTY_OK);
    CHECK(social_party_request(soc, 3, PARTY_ACCEPT, NULL, now) == PARTY_OK);
    CHECK(social_view(soc, 1, &view) && view.count == 3 && strcmp(view.members[2].world, "Verdant Depths") == 0);
    CHECK(social_party_request(soc, 1, PARTY_CREATE, NULL, now) == PARTY_ERROR_ALREADY_IN_ONE);
    CHECK(social_party_request(soc, 2, PARTY_KICK, "carol", now) == PARTY_ERROR_NOT_LEADER);
    CHECK(social_party_request(soc, 1, PARTY_PROMOTE, "bob", now) == PARTY_OK);
    CHECK(social_view(soc, 1, &view) && !view.members[0].leader && view.members[1].leader);
    CHECK(social_party_request(soc, 2, PARTY_KICK, "carol", now) == PARTY_OK);
    CHECK(social_view(soc, 3, &view) && view.party == 0);
    CHECK(social_view(soc, 2, &view) && view.count == 2);
    // Leaving: the leader's place passes on; the last one out closes the party.
    CHECK(social_party_request(soc, 2, PARTY_LEAVE, NULL, now) == PARTY_OK);
    CHECK(social_view(soc, 1, &view) && view.count == 1 && view.members[0].leader);
    CHECK(social_party_request(soc, 1, PARTY_LEAVE, NULL, now) == PARTY_OK);
    CHECK(social_view(soc, 1, &view) && view.party == 0);
    CHECK(social_party_request(soc, 1, PARTY_LEAVE, NULL, now) == PARTY_ERROR_NOT_IN_PARTY);
    // Presence expires; an invitation expires.
    CHECK(social_party_request(soc, 1, PARTY_INVITE, "bob", now) == PARTY_OK);
    social_update(soc, now + SOCIAL_INVITE_EXPIRY + 1.0);
    CHECK(social_view(soc, 2, &view) && view.invite == 0);
    CHECK(!social_find(soc, 2)->online);
    CHECK(pushes_seen > 10);
    // Characters saved before ships (version 1: gear alone) load, each with a sloop
    // and the purse; saved again, they keep their fleet.
    {
        const char *path = "treasure_test_v1.bin";
        cw_writer w;
        cw_writer_init(&w);
        cw_writer_bytes(&w, "TPSV", 4);
        cw_writer_u32(&w, 1);
        cw_writer_u32(&w, 1);
        cw_writer_u64(&w, 42);
        cw_writer_u8(&w, 4);
        cw_writer_bytes(&w, "Dora", 4);
        cw_writer_u8(&w, 0);
        character old = {};
        old.version = 9;
        old.equipment[SLOT_HEAD] = item_roll(5, 3);
        old.equipment[SLOT_HEAD].slot = SLOT_HEAD;
        cw_writer_u32(&w, old.version);
        for (int i = 0; i < SLOT_COUNT; ++i) item_write(&w, &old.equipment[i]);
        for (int i = 0; i < STASH_SIZE; ++i) item_write(&w, &old.stash[i]);
        cw_file_error error;
        CHECK(cw_file_write(path, w.data, w.size, &error));
        cw_writer_destroy(&w);
        social *loaded = (social *)malloc(sizeof(social));
        social_init(loaded, &secret, count_push, NULL);
        CHECK(social_load(loaded, path));
        character *dora = social_character(loaded, 42, NULL);
        CHECK(dora && dora->version == 9 && dora->equipment[SLOT_HEAD].id == old.equipment[SLOT_HEAD].id);
        CHECK(dora && fleet_owns(dora, HULL_SLOOP) && dora->ships.sailing == HULL_SLOOP && dora->bottomless);
        shipwright_order buy = {SHIPWRIGHT_BUY_HULL, HULL_BRIGANTINE, 0, 0};
        CHECK(dora && shipwright_apply(dora, &buy) == SHIPWRIGHT_DONE);
        social_set_save_path(loaded, path);
        CHECK(social_save(loaded));
        social *again = (social *)malloc(sizeof(social));
        social_init(again, &secret, count_push, NULL);
        CHECK(social_load(again, path));
        character *kept = social_character(again, 42, NULL);
        CHECK(kept && fleet_owns(kept, HULL_BRIGANTINE) && kept->spent == ship_hull_price(HULL_BRIGANTINE));
        free(loaded);
        free(again);
        remove(path);
    }
    // ...and so do those saved before ships had looks (version 2): as built.
    {
        const char *path = "treasure_test_v2.bin";
        cw_writer w;
        cw_writer_init(&w);
        cw_writer_bytes(&w, "TPSV", 4);
        cw_writer_u32(&w, 2);
        cw_writer_u32(&w, 1);
        cw_writer_u64(&w, 43);
        cw_writer_u8(&w, 3);
        cw_writer_bytes(&w, "Eve", 3);
        cw_writer_u8(&w, 0);
        character old = {};
        cw_writer_u32(&w, 4);
        for (int i = 0; i < SLOT_COUNT; ++i) item_write(&w, &old.equipment[i]);
        for (int i = 0; i < STASH_SIZE; ++i) item_write(&w, &old.stash[i]);
        cw_writer_i64(&w, 0);
        cw_writer_u8(&w, 1);
        cw_writer_i64(&w, 20000);
        cw_writer_u8(&w, (1u << HULL_SLOOP) | (1u << HULL_CUTTER));
        cw_writer_u8(&w, HULL_CUTTER);
        for (int h = HULL_SLOOP; h <= HULL_CUTTER; ++h) {   // the loadouts as they were: slots, and no look
            ship_loadout l = ship_default_loadout(h);
            int n = ship_slot_count(h);
            cw_writer_u8(&w, (uint8_t)h);
            cw_writer_u8(&w, (uint8_t)n);
            for (int i = 0; i < n; ++i) cw_writer_u8(&w, l.fit[i]);
        }
        cw_writer_u8(&w, (uint8_t)ship_component_count());
        for (int i = 0; i < ship_component_count(); ++i) cw_writer_u16(&w, i == ship_component_find("Iron Furnace") ? 2 : 0);
        cw_file_error error;
        CHECK(cw_file_write(path, w.data, w.size, &error));
        cw_writer_destroy(&w);
        social *loaded = (social *)malloc(sizeof(social));
        social_init(loaded, &secret, count_push, NULL);
        CHECK(social_load(loaded, path));
        character *eve = social_character(loaded, 43, NULL);
        CHECK(eve && eve->ships.sailing == HULL_CUTTER && fleet_owns(eve, HULL_SLOOP) && eve->spent == 20000);
        CHECK(eve && eve->ships.store[ship_component_find("Iron Furnace")] == 2);
        CHECK(eve && fleet_sailing(eve)->look.field[LOOK_HULL_SHAPE] == SHAPE_CLASSIC && fleet_sailing(eve)->look.field[LOOK_HULL_PAINT] == 0);
        free(loaded);
        remove(path);
    }
    free(soc);
}

// Two players on one server (its own social service): invite, accept, see each
// other's names, enter the dungeon together and share the instance, leave.
static void test_party(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.3);
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    game *ga = world_of(&r, a), *gb = world_of(&r, b);
    CHECK(strcmp(CW_GET(&gb->world, ca->player, gb->c_player, Player)->name, "Alice") == 0);   // names are replicated
    CHECK(strcmp(CW_GET(&ga->world, ca->player, ga->c_player, Player)->name, "Alice") == 0);
    CHECK(ca->account == account_id_for("local:Alice") && cb->account == account_id_for("local:Bob"));

    // Alice looks at Bob and invites whoever she is looking at.
    Transform *tb = CW_GET(&s->zone.world, cb->player, s->zone.c_transform, Transform);
    tb->position = tb->previous = v3_add(s->zone.spawn, v3_make(0, 0, -2.0f));   // two metres ahead of Alice (yaw 0 faces -z)
    rig_run(&r, 0.4);
    char looked_at[ACCOUNT_NAME_CAPACITY] = {0};
    CHECK(net_client_player_in_view(ca, 5.0f, looked_at) && strcmp(looked_at, "Bob") == 0);
    net_session_party_request(&r.sessions[a], PARTY_INVITE, looked_at);
    double start = net_seconds();
    while (net_session_party(&r.sessions[b])->invite == 0 && net_seconds() - start < 3.0) rig_step(&r);
    const party_view *vb = net_session_party(&r.sessions[b]);
    CHECK(vb->invite != 0 && strcmp(vb->invited_by, "Alice") == 0);
    CHECK(net_session_party(&r.sessions[a])->party != 0 && net_session_party(&r.sessions[a])->count == 1);
    net_session_party_request(&r.sessions[b], PARTY_ACCEPT, NULL);
    start = net_seconds();
    while (net_session_party(&r.sessions[a])->count < 2 && net_seconds() - start < 3.0) rig_step(&r);
    const party_view *va = net_session_party(&r.sessions[a]);
    vb = net_session_party(&r.sessions[b]);
    CHECK(va->count == 2 && vb->count == 2 && va->party == vb->party);
    CHECK(va->members[0].leader && strcmp(va->members[0].name, "Alice") == 0 && strcmp(va->members[1].name, "Bob") == 0);
    CHECK(vb->members[1].online && vb->invite == 0);
    uint64_t party_id = va->party;   // the views live in the links, which the hand-offs below replace
    CHECK(server_client_of_account(s, ca->account)->party == party_id);

    // Through Verdant's door, one after the other: the same instance.
    char dungeon[GAME_NAME_CAPACITY];
    game_dungeon_name("Verdant", dungeon, sizeof(dungeon));
    int door = game_find_place(&s->zone, dungeon);
    for (int k = 0; k < 2; ++k) {
        int who = k ? b : a;
        Transform *t = CW_GET(&s->zone.world, link_of(&r, who)->player, s->zone.c_transform, Transform);
        t->position = t->previous = s->zone.places[door].center;
        start = net_seconds();
        while (r.sessions[who].handoffs == 0 && net_seconds() - start < 5.0) rig_step(&r);
        CHECK(r.sessions[who].handoffs == 1);
    }
    rig_run(&r, 0.5);
    CHECK(s->world_count == 2 && s->instances_opened == 1);
    server_world *instance = server_find_instance(s, dungeon, party_id);
    CHECK(instance && instance->client_count == 2);
    CHECK(cw_query_count(world_of(&r, a)->q_all_players) == 2);   // they see each other inside
    CHECK(cw_query_count(world_of(&r, b)->q_all_players) == 2);
    va = net_session_party(&r.sessions[a]);
    CHECK(va->count == 2 && strcmp(va->members[1].world, dungeon) == 0);   // and the party shows where everyone is

    // Alice leaves the party: Bob leads what is left of it.
    net_session_party_request(&r.sessions[a], PARTY_LEAVE, NULL);
    start = net_seconds();
    while (net_session_party(&r.sessions[b])->count != 1 && net_seconds() - start < 3.0) rig_step(&r);
    vb = net_session_party(&r.sessions[b]);
    CHECK(vb->count == 1 && vb->members[0].leader && strcmp(vb->members[0].name, "Bob") == 0);
    CHECK(net_session_party(&r.sessions[a])->party == 0);
    rig_destroy(&r);
}

// ---------------------------------------------------------------------------
// Shards
// ---------------------------------------------------------------------------
static int add_shard(net_zone_table *t, const char *name, const char *region, uint16_t clients, uint16_t target,
                     uint16_t capacity, const char *place) {
    int i = net_zones_add(t, name, net_address_make(0x7f000001u, (uint16_t)(1000 + t->count)));
    net_zone_add_place(&t->zones[i], place);
    strncpy(t->zones[i].region, region, NET_REGION_NAME - 1);
    t->zones[i].clients = clients;
    t->zones[i].target = target;
    t->zones[i].capacity = capacity;
    return i;
}

static void test_shard_choice(void) {
    net_zone_table t;
    net_zones_init(&t);
    int hub1 = add_shard(&t, "hub-1", "eu", 2, 2, 4, "hub");     // at its target
    int hub2 = add_shard(&t, "hub-2", "eu", 0, 2, 4, "hub");
    int hub3 = add_shard(&t, "hub-3", "na", 1, 2, 4, "hub");
    int full = add_shard(&t, "hub-4", "eu", 4, 2, 4, "hub");     // no room at all
    add_shard(&t, "planets", "eu", 0, 0, 4, "Verdant");
    const char *none[1] = {""};
    // Region first, then the fullest shard still below its target.
    CHECK(net_zones_choose(&t, "hub", "eu", none, 0) == hub2);
    t.zones[hub1].clients = 1;
    CHECK(net_zones_choose(&t, "hub", "eu", none, 0) == hub1);
    CHECK(net_zones_choose(&t, "hub", "na", none, 0) == hub3);
    CHECK(net_zones_choose(&t, "hub", "sa", none, 0) == hub1);   // a region with no shard: anywhere
    // Everyone at or past target: the emptiest; a full shard never.
    t.zones[hub1].clients = 3;
    t.zones[hub2].clients = 2;
    CHECK(net_zones_choose(&t, "hub", "eu", none, 0) == hub2);
    t.zones[hub2].clients = 4;
    CHECK(net_zones_choose(&t, "hub", "eu", none, 0) == hub1);
    // The party's shard comes before all of that, when it has room.
    const char *with[2] = {"hub-3", "hub-3"};
    CHECK(net_zones_choose(&t, "hub", "eu", with, 2) == hub3);
    const char *with_full[1] = {"hub-4"};
    CHECK(net_zones_choose(&t, "hub", "eu", with_full, 1) == hub1);
    (void)full;
    CHECK(net_zones_choose(&t, "Ember", "eu", none, 0) == -1);
}

// Two hub shards and one in another region, through the directory: players fill
// the first to its target, then the next; another region gets its own; and a party
// member who comes back is sent where their party is, whatever the population says.
static void test_shards(void) {
    rig r;
    memset(&r, 0, sizeof(r));
    r.dir = (directory *)malloc(sizeof(directory));
    CHECK(directory_init(r.dir, 0, "test-secret", false));
    char dir_text[32];
    snprintf(dir_text, sizeof(dir_text), "127.0.0.1:%u", (unsigned)directory_port(r.dir));
    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = RIG_MAX;
    config.directory = net_address_make(0x7f000001u, directory_port(r.dir));
    config.secret = "test-secret";
    strncpy(config.owns[0], "hub", NET_ZONE_NAME - 1);
    config.owns_count = 1;
    config.target = 1;
    strncpy(config.region, "eu", NET_REGION_NAME - 1);
    strncpy(config.zone_name, "hub-1", NET_ZONE_NAME - 1);
    server *hub1 = rig_add_server(&r, &config, 0.0f);
    strncpy(config.zone_name, "hub-2", NET_ZONE_NAME - 1);
    server *hub2 = rig_add_server(&r, &config, 0.0f);
    strncpy(config.zone_name, "hub-na", NET_ZONE_NAME - 1);
    strncpy(config.region, "na", NET_REGION_NAME - 1);
    server *hub_na = rig_add_server(&r, &config, 0.0f);
    if (failures) { rig_destroy(&r); return; }
    double start = net_seconds();
    while (hub1->zones.count < 3 && net_seconds() - start < 5.0) rig_step(&r);
    CHECK(hub1->zones.count == 3);
    CHECK(hub1->zones.zones[net_zones_find(&hub1->zones, "hub-na")].region[0] == 'n');

    net_identity alice = net_identity_local("Alice"), bob = net_identity_local("Bob"), carol = net_identity_local("Carol");
    net_identity erin = net_identity_local("Erin");
    strncpy(alice.region, "eu", NET_REGION_NAME - 1);
    strncpy(bob.region, "eu", NET_REGION_NAME - 1);
    strncpy(carol.region, "na", NET_REGION_NAME - 1);
    strncpy(erin.region, "eu", NET_REGION_NAME - 1);
    int a = r.client_count++;
    r.used[a] = true;
    CHECK(net_session_init_directory(&r.sessions[a], dir_text, "hub", 0.0f, &alice));
    CHECK(rig_wait_connected(&r, 5.0));
    int b = r.client_count++;
    r.used[b] = true;
    CHECK(net_session_init_directory(&r.sessions[b], dir_text, "hub", 0.0f, &bob));
    CHECK(rig_wait_connected(&r, 5.0));
    int c = r.client_count++;
    r.used[c] = true;
    CHECK(net_session_init_directory(&r.sessions[c], dir_text, "hub", 0.0f, &carol));
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    CHECK(link_of(&r, a)->server.port == server_port(hub1));    // the first fills to its target of one
    CHECK(link_of(&r, b)->server.port == server_port(hub2));    // then the next opens
    CHECK(link_of(&r, c)->server.port == server_port(hub_na));  // another region, its own shard
    CHECK(r.dir->assignments == 3);

    // Bob founds a party with Erin, who then goes away; when Erin comes back she is
    // put with Bob on hub-2, though by population alone hub-1 would have had her.
    int e = r.client_count++;
    r.used[e] = true;
    CHECK(net_session_init_directory(&r.sessions[e], dir_text, "hub", 0.0f, &erin));
    CHECK(rig_wait_connected(&r, 5.0));
    uint16_t erin_first = link_of(&r, e)->server.port;
    net_session_party_request(&r.sessions[b], PARTY_INVITE, "Erin");
    start = net_seconds();
    while (net_session_party(&r.sessions[e])->invite == 0 && net_seconds() - start < 3.0) rig_step(&r);
    net_session_party_request(&r.sessions[e], PARTY_ACCEPT, NULL);
    start = net_seconds();
    while (net_session_party(&r.sessions[b])->count < 2 && net_seconds() - start < 3.0) rig_step(&r);
    CHECK(net_session_party(&r.sessions[b])->count == 2);
    CHECK(strcmp(net_session_party(&r.sessions[b])->members[0].server, "hub-2") == 0);
    rig_drop_client(&r, e);
    rig_run(&r, 2.5);   // the shards report their emptier selves
    int e2 = r.client_count++;
    r.used[e2] = true;
    CHECK(net_session_init_directory(&r.sessions[e2], dir_text, "hub", 0.0f, &erin));
    CHECK(rig_wait_connected(&r, 5.0));
    CHECK(link_of(&r, e2)->server.port == server_port(hub2));
    (void)erin_first;
    rig_destroy(&r);
}

// ---------------------------------------------------------------------------
// Loot: picked up by walking into it, worn for all to see, kept across a restart
// ---------------------------------------------------------------------------
static void test_loot(void) {
    const char *save = "treasure_test_characters.bin";
    remove(save);
    uint32_t worn_id = 0;
    for (int run = 0; run < 2; ++run) {
        rig r;
        memset(&r, 0, sizeof(r));
        server_config config = server_config_default();
        config.port = 0;
        config.max_clients = RIG_MAX;
        config.save_path = save;
        server *s = rig_add_server(&r, &config, 0.0f);
        int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
        CHECK(rig_wait_connected(&r, 5.0));
        if (failures) { rig_destroy(&r); return; }
        rig_run(&r, 0.3);
        net_client *ca = link_of(&r, a);
        game *ga = world_of(&r, a), *gb = world_of(&r, b);
        CHECK(ca->gear_known);
        if (run == 1) {
            // Back after the restart, wearing what was picked up last time.
            CHECK(ca->gear.equipment[0].id == worn_id || ca->gear.equipment[1].id == worn_id || ca->gear.equipment[2].id == worn_id
                  || ca->gear.equipment[3].id == worn_id || ca->gear.equipment[4].id == worn_id);
            CHECK(ca->gear.version >= 2);
            CHECK(fleet_owns(&ca->gear, HULL_CUTTER) && ca->gear.ships.sailing == HULL_CUTTER && ca->gear.spent == ship_hull_price(HULL_CUTTER));
            rig_destroy(&r);
            break;
        }
        CHECK(ca->gear.version == 0 && character_stash_count(&ca->gear) == 0);

        // A chest appears at Alice's feet; she takes it, wears it, Bob sees it on her.
        v3 feet = position_in(&s->zone, ca->player);
        item treasure = item_roll(777, 5);
        item lesser = item_roll(778, 1);
        lesser.slot = treasure.slot;
        lesser.power = (uint16_t)(treasure.power - 1);
        cw_entity_id chest = game_spawn_loot(&s->zone, v3_add(feet, v3_make(0.3f, 0.25f, 0)), &treasure);
        double start = net_seconds();
        while (ca->gear_updates < 2 && net_seconds() - start < 3.0) rig_step(&r);
        CHECK(s->pickups == 1);
        CHECK(ca->gear.equipment[treasure.slot].id == treasure.id && ca->pickup_flash > 0.0f && ca->last_pickup.id == treasure.id);
        rig_run(&r, 0.3);
        CHECK(!cw_world_is_valid(&s->zone.world, chest));   // swept by the tick after the pickup
        CHECK(!cw_world_is_valid(&ga->world, chest) && !cw_world_is_valid(&gb->world, chest));
        CHECK(CW_GET(&gb->world, ca->player, gb->c_equipment, Equipment)->items[treasure.slot].id == treasure.id);
        // ...as a partial record: the gear, not the whole player again.
        CHECK(link_of(&r, b)->updates_received >= 1 && server_client_of_account(s, ca->account)->updates_sent >= 1);
        worn_id = treasure.id;
        // A lesser piece for the same slot goes to the stash; wearing it from there swaps them.
        game_spawn_loot(&s->zone, v3_add(feet, v3_make(-0.3f, 0.25f, 0)), &lesser);
        start = net_seconds();
        while (ca->gear_updates < 3 && net_seconds() - start < 3.0) rig_step(&r);
        CHECK(ca->gear.equipment[treasure.slot].id == treasure.id && ca->gear.stash[0].id == lesser.id);
        net_session_equip(&r.sessions[a], 0);
        start = net_seconds();
        while (ca->gear_updates < 4 && net_seconds() - start < 3.0) rig_step(&r);
        CHECK(ca->gear.equipment[treasure.slot].id == lesser.id && ca->gear.stash[0].id == treasure.id);
        rig_run(&r, 0.3);
        CHECK(CW_GET(&gb->world, ca->player, gb->c_equipment, Equipment)->items[treasure.slot].id == lesser.id);
        net_session_equip(&r.sessions[a], 0);   // and back
        start = net_seconds();
        while (ca->gear_updates < 5 && net_seconds() - start < 3.0) rig_step(&r);
        CHECK(ca->gear.equipment[treasure.slot].id == treasure.id);
        // Ships are kept with the rest: a cutter bought at the shipwright's counter, and sailed.
        int counter = -1;
        for (int i = 0; i < s->zone.place_count; ++i)
            if (s->zone.places[i].kind == PLACE_SHIPWRIGHT) counter = i;
        Transform *there = CW_GET(&s->zone.world, ca->player, s->zone.c_transform, Transform);
        there->position = there->previous = s->zone.places[counter].spawn;
        rig_run(&r, 0.3);
        shipwright_order buy = {SHIPWRIGHT_BUY_HULL, HULL_CUTTER, 0, 0}, sail = {SHIPWRIGHT_SAIL, HULL_CUTTER, 0, 0};
        net_client_shipwright(ca, &buy);
        net_client_shipwright(ca, &sail);
        RIG_UNTIL(&r, 10.0, ca->gear.ships.sailing == HULL_CUTTER);
        CHECK(ca->gear.ships.sailing == HULL_CUTTER);
        // Chests in the dungeons are part of the world, and say what they hold.
        char dungeon[GAME_NAME_CAPACITY];
        game_dungeon_name("Verdant", dungeon, sizeof(dungeon));
        int door = game_find_place(&s->zone, dungeon);
        Transform *at = CW_GET(&s->zone.world, ca->player, s->zone.c_transform, Transform);
        at->position = at->previous = s->zone.places[door].center;
        start = net_seconds();
        while (r.sessions[a].handoffs == 0 && net_seconds() - start < 5.0) rig_step(&r);
        rig_run(&r, 0.4);
        ga = world_of(&r, a);
        CHECK(cw_query_count(ga->q_loot) >= 2);
        CHECK(link_of(&r, a)->gear_known && link_of(&r, a)->gear.equipment[treasure.slot].id == treasure.id);   // gear follows into the instance
        rig_destroy(&r);   // saves on the way out
    }
    remove(save);
}

// Relevance as a policy: players and loot within a radius of a client are sent to
// it, and forgotten (despawned there) once beyond it for a while; scenery always.
static void test_relevance(void) {
    rig r;
    memset(&r, 0, sizeof(r));
    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = RIG_MAX;
    config.relevance[RELEVANCE_PLAYER].radius = config.relevance[RELEVANCE_LOOT].radius = 30.0f;
    for (int i = 0; i < RELEVANCE_CLASS_COUNT; ++i) config.relevance[i].forget = 0.5;
    server *s = rig_add_server(&r, &config, 0.0f);
    int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.5);
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    game *ga = world_of(&r, a), *gb = world_of(&r, b);
    game *z = &s->zone;
    server_client *sa = server_client_of_account(s, ca->account), *sb = server_client_of_account(s, cb->account);
    size_t ships = cw_query_count(z->q_ships);
    CHECK(ships > 0 && cw_query_count(ga->q_ships) == ships && cw_query_count(gb->q_ships) == ships);
    CHECK(cw_world_is_valid(&ga->world, cb->player) && cw_world_is_valid(&gb->world, ca->player));

    // Loot near the two of them reaches both; loot on a far pad reaches neither.
    v3 here = position_in(z, ca->player);
    int far_place = game_find_place(z, "Verdant");
    CHECK(far_place >= 0);
    v3 far = z->places[far_place].spawn;
    item it = item_roll(4242, 3);
    cw_entity_id near_loot = game_spawn_loot(z, v3_add(here, v3_make(5.0f, 0.25f, 0)), &it);
    cw_entity_id far_loot = game_spawn_loot(z, v3_add(far, v3_make(5.0f, 0.25f, 0)), &it);
    rig_run(&r, 0.5);
    CHECK(cw_world_is_valid(&ga->world, near_loot) && cw_world_is_valid(&gb->world, near_loot));
    CHECK(!cw_world_is_valid(&ga->world, far_loot) && !cw_world_is_valid(&gb->world, far_loot));

    // Bob is moved to the far pad: within a second or so Alice and Bob are gone
    // from each other's worlds, the near loot from Bob's, and the far loot is in it.
    Transform *bt = CW_GET(&z->world, cb->player, z->c_transform, Transform);
    bt->position = bt->previous = far;
    rig_run(&r, 2.0);
    CHECK(!cw_world_is_valid(&ga->world, cb->player) && !cw_world_is_valid(&gb->world, ca->player));
    CHECK(cw_world_is_valid(&ga->world, near_loot) && !cw_world_is_valid(&gb->world, near_loot));
    CHECK(cw_world_is_valid(&gb->world, far_loot) && !cw_world_is_valid(&ga->world, far_loot));
    CHECK(sa->forgotten == 1 && sb->forgotten == 2);
    CHECK(cw_query_count(ga->q_ships) == ships && cw_query_count(gb->q_ships) == ships);   // scenery stays
    CHECK(cw_world_is_valid(&z->world, ca->player) && cw_world_is_valid(&z->world, cb->player));   // the server forgets nothing

    // And back: they meet again, with their records sent afresh (gear included).
    bt = CW_GET(&z->world, cb->player, z->c_transform, Transform);
    bt->position = bt->previous = v3_add(here, v3_make(2.0f, 0, 0));
    rig_run(&r, 2.0);
    CHECK(cw_world_is_valid(&ga->world, cb->player) && cw_world_is_valid(&gb->world, ca->player));
    CHECK(cw_comp_has(&ga->world, cb->player, ga->c_equipment) && cw_comp_has(&ga->world, cb->player, ga->c_owned));
    CHECK(cw_world_is_valid(&gb->world, near_loot) && !cw_world_is_valid(&gb->world, far_loot));
    CHECK(sb->forgotten == 3);
    rig_destroy(&r);
}

// ---------------------------------------------------------------------------
// The orchestrator: the plan on its own, then real shard processes
// ---------------------------------------------------------------------------
static net_zone *table_shard(net_zone_table *t, const char *name, uint16_t clients, uint16_t target) {
    int i = net_zones_find(t, name);
    if (i < 0) {
        i = net_zones_add(t, name, net_address_make(0x7f000001u, 1));
        net_zone_add_place(&t->zones[i], "hub");
        strncpy(t->zones[i].region, "eu", NET_REGION_NAME - 1);
        t->zones[i].capacity = 10;
    }
    t->zones[i].clients = clients;
    t->zones[i].target = target;
    return &t->zones[i];
}

static void test_orchestrator_plan(void) {
    orchestrator_config config = {};
    strncpy(config.places[0], "hub", NET_ZONE_NAME - 1);
    config.place_count = 1;
    strncpy(config.regions[0], "eu", NET_REGION_NAME - 1);
    config.region_count = 1;
    config.min = 1;
    config.max = 3;
    config.target = 2;
    config.empty_stop_after = 1.0;
    shard_process shards[4] = {};
    plan plans[8];
    net_zone_table table;
    net_zones_init(&table);
    double now = 100.0;
    // Nothing runs: start one. Started but not registered yet: wait.
    CHECK(orchestrator_plan(&table, &config, shards, 4, now, plans, 8) == 1 && plans[0].kind == PLAN_START);
    shards[0].used = true;
    strcpy(shards[0].name, "hub-eu-1");
    strcpy(shards[0].place, "hub");
    strcpy(shards[0].region, "eu");
    shards[0].started_at = now;
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 1.0, plans, 8) == 0);
    // Registered and below target: nothing. At target: another.
    table_shard(&table, "hub-eu-1", 1, 2);
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 2.0, plans, 8) == 0);
    table_shard(&table, "hub-eu-1", 2, 2);
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 3.0, plans, 8) == 1 && plans[0].kind == PLAN_START);
    shards[1].used = true;
    strcpy(shards[1].name, "hub-eu-2");
    strcpy(shards[1].place, "hub");
    strcpy(shards[1].region, "eu");
    shards[1].started_at = now + 3.0;
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 4.0, plans, 8) == 0);   // starting: wait
    // The second is up and empty. While the first is still at its target it stays;
    // once the first has room it is drained, after it has been empty long enough.
    table_shard(&table, "hub-eu-2", 0, 2);
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 5.0, plans, 8) == 0);
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 6.5, plans, 8) == 0);
    table_shard(&table, "hub-eu-1", 1, 2);
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 6.6, plans, 8) == 1 && plans[0].kind == PLAN_DRAIN && plans[0].shard == 1);
    shards[1].draining = true;
    table_shard(&table, "hub-eu-2", 0, 2)->draining = true;
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 7.0, plans, 8) == 1 && plans[0].kind == PLAN_STOP && plans[0].shard == 1);
    // A draining shard with players still on it waits; the first shard alone never drains.
    table_shard(&table, "hub-eu-2", 1, 2);
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 8.0, plans, 8) == 0);
    memset(&shards[1], 0, sizeof(shards[1]));
    net_zones_init(&table);
    table_shard(&table, "hub-eu-1", 0, 2);
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 20.0, plans, 8) == 0);
    // At the maximum, no more are started however full.
    for (int i = 0; i < 3; ++i) {
        shards[i].used = true;
        snprintf(shards[i].name, NET_ZONE_NAME, "hub-eu-%d", i + 1);
        strcpy(shards[i].place, "hub");
        strcpy(shards[i].region, "eu");
        shards[i].registered = true;
        shards[i].started_at = now;
        table_shard(&table, shards[i].name, 2, 2);
    }
    CHECK(orchestrator_plan(&table, &config, shards, 4, now + 30.0, plans, 8) == 0);
}

#ifdef TP_SERVER_PATH
static void test_orchestrator(void) {
    rig r;
    memset(&r, 0, sizeof(r));
    r.dir = (directory *)malloc(sizeof(directory));
    CHECK(directory_init(r.dir, 0, "test-secret", false));
    r.dir->expiry = 3.0;
    char dir_text[32];
    snprintf(dir_text, sizeof(dir_text), "127.0.0.1:%u", (unsigned)directory_port(r.dir));
    orchestrator_config config = {};
    config.directory = net_address_make(0x7f000001u, directory_port(r.dir));
    config.secret = "test-secret";
    config.server_path = TP_SERVER_PATH;
    strncpy(config.places[0], "hub", NET_ZONE_NAME - 1);
    config.place_count = 1;
    strncpy(config.regions[0], "eu", NET_REGION_NAME - 1);
    config.region_count = 1;
    config.min = 1;
    config.max = 2;
    config.target = 1;
    config.capacity = 4;
    config.port_from = 27700;
    config.port_to = 27710;
    config.empty_stop_after = 2.0;
    r.orch = (orchestrator *)malloc(sizeof(orchestrator));
    CHECK(orchestrator_init(r.orch, &config));

    // One shard appears by itself.
    double start = net_seconds();
    while (net_zones_find(&r.dir->secret, "hub-eu-1") < 0 && net_seconds() - start < 12.0) {
        rig_step(&r);
        net_zone_table table;
        directory_table(r.dir, &table);
        if (net_zones_find(&table, "hub-eu-1") >= 0) break;
    }
    net_zone_table table;
    directory_table(r.dir, &table);
    CHECK(net_zones_find(&table, "hub-eu-1") >= 0 && orchestrator_shard_count(r.orch) == 1);
    // A player fills it to its target; a second shard follows; the next player lands there.
    net_identity alice = net_identity_local("Alice"), bob = net_identity_local("Bob");
    strncpy(alice.region, "eu", NET_REGION_NAME - 1);
    strncpy(bob.region, "eu", NET_REGION_NAME - 1);
    int a = r.client_count++;
    r.used[a] = true;
    CHECK(net_session_init_directory(&r.sessions[a], dir_text, "hub", 0.0f, &alice));
    CHECK(rig_wait_connected(&r, 15.0));
    start = net_seconds();
    for (;;) {
        rig_step(&r);
        directory_table(r.dir, &table);
        if (net_zones_find(&table, "hub-eu-2") >= 0 || net_seconds() - start > 15.0) break;
    }
    CHECK(net_zones_find(&table, "hub-eu-2") >= 0 && orchestrator_shard_count(r.orch) == 2);
    int b = r.client_count++;
    r.used[b] = true;
    CHECK(net_session_init_directory(&r.sessions[b], dir_text, "hub", 0.0f, &bob));
    CHECK(rig_wait_connected(&r, 15.0));
    CHECK(link_of(&r, a)->server.port != link_of(&r, b)->server.port);
    // Both leave; the extra shard empties, drains, and is stopped; the first stays.
    rig_drop_client(&r, a);
    rig_drop_client(&r, b);
    start = net_seconds();
    while (orchestrator_shard_count(r.orch) > 1 && net_seconds() - start < 20.0) rig_step(&r);
    CHECK(orchestrator_shard_count(r.orch) == 1 && r.orch->drained == 1 && r.orch->stopped == 1);
    bool first_stays = false;
    for (int i = 0; i < ORCHESTRATOR_MAX_SHARDS; ++i) first_stays = first_stays || (r.orch->shards[i].used && strcmp(r.orch->shards[i].name, "hub-eu-1") == 0);
    CHECK(first_stays);
    start = net_seconds();
    for (;;) {
        rig_step(&r);
        directory_table(r.dir, &table);
        if (net_zones_find(&table, "hub-eu-2") < 0 || net_seconds() - start > 8.0) break;
    }
    CHECK(net_zones_find(&table, "hub-eu-2") < 0 && net_zones_find(&table, "hub-eu-1") >= 0);
    rig_destroy(&r);
}
#endif

// ---------------------------------------------------------------------------
// The connection lifecycle: a lost connection leaves the body where it was for a
// while; coming back reattaches to it; a quit lingers briefly; a hand-off does not.
// ---------------------------------------------------------------------------
static void test_reconnection(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.3);
    net_client *ca = link_of(&r, a);
    game *gb = world_of(&r, b);
    cw_entity_id body = ca->player;
    uint64_t account = ca->account;
    CHECK(game_owner(&s->zone, body) == account);                              // the body is hers
    CHECK(CW_GET(&gb->world, body, gb->c_owned, Owned)->account == account);   // and everyone can tell
    // Alice walks off somewhere, wearing something.
    item cap = item_roll(4242, 2);
    game_spawn_loot(&s->zone, v3_add(position_in(&s->zone, body), v3_make(0.3f, 0.25f, 0)), &cap);
    r.inputs[a].forward = 1.0f;
    rig_run(&r, 1.0);
    r.inputs[a].forward = 0.0f;
    rig_run(&r, 0.3);
    v3 where = position_in(&s->zone, body);
    CHECK(distance(where, s->zone.spawn) > 3.0f && ca->gear.equipment[cap.slot].id == cap.id);

    // Her connection dies (every packet lost, and her client is not even running).
    net_socket_set_loss(ca->socket, 1.0f);
    r.used[a] = false;
    double start = net_seconds();
    while (server_client_of_account(s, account)->state != CLIENT_LINGERING && net_seconds() - start < 10.0) rig_step(&r);
    CHECK(server_client_of_account(s, account)->state == CLIENT_LINGERING && s->lingered == 1);
    CHECK(s->client_count == 1);
    CHECK(cw_world_is_valid(&s->zone.world, body) && distance(position_in(&s->zone, body), where) < 0.01f);   // the body waits where she was
    CHECK(cw_world_is_valid(&gb->world, body));                                                               // Bob still sees it
    CHECK(s->zone_world->client_count == 2);

    // Her client notices, and comes back: the same body, where it was, with the cap.
    r.used[a] = true;
    start = net_seconds();
    while (r.sessions[a].reconnects == 0 && net_seconds() - start < 10.0) rig_step(&r);
    CHECK(r.sessions[a].reconnects == 1);
    start = net_seconds();
    while ((net_session_state(&r.sessions[a]) != NC_CONNECTED || link_of(&r, a) == ca) && net_seconds() - start < 10.0) rig_step(&r);
    rig_run(&r, 0.5);
    ca = link_of(&r, a);
    CHECK(ca->state == NC_CONNECTED && ca->player == body);
    CHECK(s->reattached == 1 && s->client_count == 2 && s->finalized == 0);
    CHECK(cw_world_is_valid(&world_of(&r, a)->world, body));
    if (!cw_world_is_valid(&world_of(&r, a)->world, body)) { rig_destroy(&r); return; }
    CHECK(distance(position_in(world_of(&r, a), body), where) < 0.05f);
    CHECK(ca->gear_known && ca->gear.equipment[cap.slot].id == cap.id);
    CHECK(cw_world_entity_count(&world_of(&r, a)->world) > 10);   // the world came back with her
    // And she can walk on from there; Bob sees the same entity move.
    r.inputs[a].forward = 1.0f;
    rig_run(&r, 0.5);
    r.inputs[a].forward = 0.0f;
    rig_run(&r, 0.4);
    CHECK(distance(position_in(&s->zone, body), where) > 2.0f);
    CHECK(distance(position_in(gb, body), position_in(&s->zone, body)) < 0.05f);

    // A lost connection that the server has not noticed yet: coming back still
    // reattaches (the dead connection is closed).
    net_client *old = link_of(&r, a);
    net_socket_set_loss(old->socket, 1.0f);
    r.sessions[a].reconnects = 0;
    old->connection.last_received = net_seconds() - NET_TIMEOUT - 1.0;   // the client gives up at once
    start = net_seconds();
    while (s->reattached < 2 && net_seconds() - start < 10.0) rig_step(&r);
    CHECK(s->reattached == 2 && link_of(&r, a)->player == body);
    rig_run(&r, 0.3);
    CHECK(s->client_count == 2 && s->superseded == 0);

    // A quit keeps the body for the short linger, then it is gone for good.
    rig_drop_client(&r, a);
    rig_run(&r, 0.2);
    CHECK(cw_world_is_valid(&s->zone.world, body) && server_client_of_account(s, account)->state == CLIENT_LINGERING);
    rig_run(&r, 0.6);
    CHECK(!cw_world_is_valid(&s->zone.world, body) && server_client_of_account(s, account) == NULL);
    CHECK(s->finalized == 1);
    rig_destroy(&r);
}

// ---------------------------------------------------------------------------
// The handshake's defences: the challenge, no amplification, the per-address limit
// ---------------------------------------------------------------------------
struct raw_answer {
    int challenges, keys, rejects, other;
    uint64_t challenge;
    size_t bytes;
};
static void on_raw_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    raw_answer *a = (raw_answer *)user;
    if (type == MSG_CHALLENGE) {
        uint32_t nonce = 0;
        cw_reader_u32(&payload, &nonce);
        cw_reader_u64(&payload, &a->challenge);
        ++a->challenges;
    } else if (type == MSG_KEYS) ++a->keys;
    else if (type == MSG_REJECT) ++a->rejects;
    else ++a->other;
}
// Sends one MSG_CONNECT from a raw socket, as a client would; returns the bytes sent.
static size_t raw_connect(net_socket *socket, net_address to, const net_connect_message *m) {
    unsigned char payload[NET_MAX_PAYLOAD];
    cw_writer w;
    cw_writer_init_fixed(&w, payload, sizeof(payload));
    net_connect_write(&w, m, NULL, 0, NULL, 0);
    net_connection tmp;
    net_connection_init(&tmp, socket, to, 0, NULL, NULL);
    net_connection_send(&tmp, MSG_CONNECT, NET_UNRELIABLE, payload, w.size);
    net_connection_flush(&tmp, net_seconds(), false);
    size_t bytes = tmp.bytes_sent;
    net_connection_destroy(&tmp);
    return bytes;
}
static void raw_receive(net_socket *socket, raw_answer *a) {
    unsigned char packet[NET_MAX_PACKET];
    for (;;) {
        net_address from;
        int size = net_socket_recv(socket, &from, packet, sizeof(packet));
        if (size <= 0) break;
        a->bytes += (size_t)size;
        net_connection tmp;
        net_connection_init(&tmp, socket, from, 0, on_raw_message, a);
        net_connection_receive(&tmp, packet, (size_t)size, net_seconds());
        net_connection_destroy(&tmp);
    }
}

// Steps the rig and reads a raw socket's answers until `condition` holds, or up to
// `seconds` of real time.
#define RAW_UNTIL(r, socket, answer, seconds, condition) do { \
    double until_ = net_seconds() + (seconds); \
    for (;;) { \
        rig_step(r); \
        raw_receive(socket, answer); \
        if ((condition) || net_seconds() >= until_) break; \
    } \
} while (0)

static void test_handshake_defences(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    net_address to = net_address_make(0x7f000001u, server_port(s));
    int a = rig_add_client(&r, 0.0f);   // a real client, connected before the trouble starts
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    CHECK(link_of(&r, a)->challenges_received == 1 && s->challenges_sent == 1 && s->challenges_passed == 1);

    net_socket *raw = net_socket_open(0);
    net_connect_message m = {};
    m.version = NET_PROTOCOL_VERSION;
    m.nonce = 77;
    crypto_random(m.public_key, sizeof(m.public_key));
    strcpy(m.platform, "local:mallory");
    strcpy(m.name, "Mallory");
    // A connect without a challenge gets one back: smaller than the connect, and
    // nothing is allocated or computed for the sender.
    raw_answer answer = {};
    size_t sent = raw_connect(raw, to, &m);
    RAW_UNTIL(&r, raw, &answer, 2.0, answer.challenges >= 1);
    CHECK(answer.challenges == 1 && answer.keys == 0 && answer.rejects == 0);
    CHECK(answer.bytes < sent && sent >= NET_CONNECT_PADDING);
    CHECK(s->challenges_sent == 2 && s->client_count == 1);
    // A wrong challenge: the same, counted.
    m.challenge = answer.challenge ^ 1;
    raw_connect(raw, to, &m);
    RAW_UNTIL(&r, raw, &answer, 2.0, answer.challenges >= 2);
    CHECK(answer.challenges == 2 && answer.keys == 0 && s->challenges_failed == 1 && s->client_count == 1);
    // A challenge for one address is no good from another.
    net_socket *other = net_socket_open(0);
    m.challenge = answer.challenge;
    raw_connect(other, to, &m);
    raw_answer other_answer = {};
    RAW_UNTIL(&r, other, &other_answer, 2.0, other_answer.challenges >= 1);
    CHECK(other_answer.challenges == 1 && other_answer.keys == 0 && s->challenges_failed == 2 && s->client_count == 1);
    net_socket_close(other);
    // The right one is admitted: the keys come back, and a slot is taken.
    raw_connect(raw, to, &m);
    RAW_UNTIL(&r, raw, &answer, 2.0, answer.keys >= 1);
    CHECK(answer.keys == 1 && s->challenges_passed == 2 && s->client_count == 2);

    // A flood of connects from one address: most are dropped unread, the challenges
    // sent back are few, and the client already connected does not notice.
    uint32_t challenges_before = s->challenges_sent;
    size_t flood_bytes = 0;
    net_socket *flooder = net_socket_open(0);
    for (int i = 0; i < 300; ++i) {
        m.nonce = (uint32_t)i;
        m.challenge = 0;
        flood_bytes += raw_connect(flooder, to, &m);
        if (i % 50 == 0) rig_step(&r);
    }
    rig_run(&r, 0.1);
    raw_answer flood_answer = {};
    raw_receive(flooder, &flood_answer);
    CHECK(s->unconnected.refused > 200);
    CHECK(s->challenges_sent - challenges_before <= (uint32_t)(SERVER_UNCONNECTED_BURST + 2));
    CHECK(flood_answer.bytes < flood_bytes / 4);
    CHECK(link_of(&r, a)->state == NC_CONNECTED && link_of(&r, a)->connection.packets_received > 0);
    rig_run(&r, 0.3);
    CHECK(link_of(&r, a)->state == NC_CONNECTED && s->client_count == 2);
    net_socket_close(flooder);
    net_socket_close(raw);
    rig_destroy(&r);
}

// ---------------------------------------------------------------------------
// Connect tokens: behind a directory, nobody gets in without its word
// ---------------------------------------------------------------------------
static int rig_add_raw_client(rig *r, server *s, const unsigned char *token, size_t token_size, const net_identity *identity) {
    int i = r->client_count++;
    net_session *ss = &r->sessions[i];
    memset(ss, 0, sizeof(*ss));
    ss->identity = identity ? *identity : net_identity_local(NULL);
    if (token_size) memcpy(ss->token, token, token_size);
    ss->token_size = token_size;
    net_address address = net_address_make(0x7f000001u, server_port(s));
    ss->world = (game *)malloc(sizeof(game));
    game_init(ss->world, CW_CLIENT);
    ss->link = (net_client *)malloc(sizeof(net_client));
    net_client_options o = {};
    o.server = address;
    o.token = token_size ? ss->token : NULL;
    o.token_size = token_size;
    o.identity = &ss->identity;
    CHECK(net_client_init_options(ss->link, ss->world, &o));
    r->used[i] = true;
    return i;
}
static size_t make_token(unsigned char *out, const char *secret, const char *zone, const net_identity *who, double expires) {
    net_zone_table t;
    net_zones_init(&t);
    net_zones_set_secret(&t, secret);
    net_connect_token token = {};
    token.account = account_id_for(who->platform);
    memcpy(token.name, who->name, sizeof(token.name));
    memcpy(token.platform, who->platform, sizeof(token.platform));
    strncpy(token.zone, zone, NET_ZONE_NAME - 1);
    token.expires = expires;
    crypto_random(&token.nonce, sizeof(token.nonce));
    cw_writer w;
    cw_writer_init_fixed(&w, out, NET_TOKEN_SIZE);
    net_token_write(&w, &token, &t);
    return w.size;
}
static bool session_failed_with(rig *r, int i, const char *text, double seconds) {
    double start = net_seconds();
    while (net_session_state(&r->sessions[i]) != NC_DISCONNECTED && net_seconds() - start < seconds) rig_step(r);
    const char *failure = r->sessions[i].link ? r->sessions[i].link->failure : r->sessions[i].failure;
    return failure && strcmp(failure, text) == 0;
}

static void test_connect_tokens(void) {
    rig r;
    memset(&r, 0, sizeof(r));
    r.dir = (directory *)malloc(sizeof(directory));
    CHECK(directory_init(r.dir, 0, "test-secret", false));
    char dir_text[32];
    snprintf(dir_text, sizeof(dir_text), "127.0.0.1:%u", (unsigned)directory_port(r.dir));
    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = RIG_MAX;
    config.directory = net_address_make(0x7f000001u, directory_port(r.dir));
    config.secret = "test-secret";
    config.linger_timeout = 12.0;
    strncpy(config.zone_name, "hub", NET_ZONE_NAME - 1);
    strncpy(config.owns[0], "hub", NET_ZONE_NAME - 1);
    config.owns_count = 1;
    server *hub = rig_add_server(&r, &config, 0.0f);
    if (failures) { rig_destroy(&r); return; }
    CHECK(hub->config.require_token);
    double start = net_seconds();
    while (hub->zones.count < 1 && net_seconds() - start < 5.0) rig_step(&r);

    // Straight to the server, no token: refused, told why.
    net_identity mallory = net_identity_local("Mallory");
    int m = rig_add_raw_client(&r, hub, NULL, 0, &mallory);
    CHECK(session_failed_with(&r, m, "the server admits only those the directory sent", 5.0));
    CHECK(hub->tokens_refused == 1 && hub->client_count == 0);
    rig_drop_client(&r, m);
    // A token signed with the wrong secret, an expired one, one for another zone: refused.
    unsigned char token[NET_TOKEN_SIZE];
    size_t size = make_token(token, "wrong-secret", "hub", &mallory, net_wall_seconds() + 60.0);
    m = rig_add_raw_client(&r, hub, token, size, &mallory);
    CHECK(session_failed_with(&r, m, "the server did not accept the ticket", 5.0) && hub->credentials_refused == 1);
    rig_drop_client(&r, m);
    size = make_token(token, "test-secret", "hub", &mallory, net_wall_seconds() - 1.0);
    m = rig_add_raw_client(&r, hub, token, size, &mallory);
    CHECK(session_failed_with(&r, m, "the server did not accept the ticket", 5.0) && hub->credentials_refused == 2);
    rig_drop_client(&r, m);
    size = make_token(token, "test-secret", "planets", &mallory, net_wall_seconds() + 60.0);
    m = rig_add_raw_client(&r, hub, token, size, &mallory);
    CHECK(session_failed_with(&r, m, "the server did not accept the ticket", 5.0) && hub->credentials_refused == 3);
    rig_drop_client(&r, m);
    CHECK(hub->client_count == 0);
    // A good one admits, once: the same token from another socket is a replay.
    size = make_token(token, "test-secret", "hub", &mallory, net_wall_seconds() + 60.0);
    m = rig_add_raw_client(&r, hub, token, size, &mallory);
    CHECK(rig_wait_connected(&r, 5.0));
    CHECK(hub->client_count == 1 && link_of(&r, m)->account == account_id_for(mallory.platform));
    int m2 = rig_add_raw_client(&r, hub, token, size, &mallory);
    CHECK(session_failed_with(&r, m2, "the server did not accept the ticket", 5.0) && hub->credentials_refused == 4);
    rig_drop_client(&r, m2);
    rig_drop_client(&r, m);
    rig_run(&r, 0.3);

    // The proper way: the directory verifies who we are, issues the token, and the
    // zone takes its word. Then the connection is lost; the session asks the
    // directory for a fresh token for the zone it was on and reattaches.
    net_identity alice = net_identity_local("Alice");
    int a = r.client_count++;
    r.used[a] = true;
    CHECK(net_session_init_directory(&r.sessions[a], dir_text, "hub", 0.0f, &alice));
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    CHECK(r.dir->tokens_issued == 1 && r.sessions[a].token_size > 0 && link_of(&r, a)->token_size > 0);
    server_client *sa = server_client_of_account(hub, link_of(&r, a)->account);
    CHECK(sa && strcmp(sa->platform, alice.platform) == 0);
    cw_entity_id body = link_of(&r, a)->player;
    net_client *old = link_of(&r, a);
    net_socket_set_loss(old->socket, 1.0f);
    old->connection.last_received = net_seconds() - NET_TIMEOUT - 1.0;
    start = net_seconds();
    while (hub->reattached < 1 && net_seconds() - start < 10.0) rig_step(&r);
    rig_run(&r, 0.3);
    CHECK(hub->reattached == 1 && r.sessions[a].reconnects == 1 && link_of(&r, a)->player == body);
    CHECK(r.dir->tokens_issued == 2 && net_session_state(&r.sessions[a]) == NC_CONNECTED);
    // Someone the directory will not vouch for gets no token, and knows it.
    net_identity nobody = {};
    strcpy(nobody.platform, "nowhere:x");
    strcpy(nobody.name, "Nobody");
    int n = r.client_count++;
    r.used[n] = true;
    CHECK(net_session_init_directory(&r.sessions[n], dir_text, "hub", 0.0f, &nobody));
    CHECK(session_failed_with(&r, n, "the directory did not accept who we said we are", 5.0));
    CHECK(r.dir->identities_refused >= 1);
    rig_destroy(&r);
}

// TEST_ONLY=name in the environment runs just that test; TEST_TIMING=1 prints how
// long each rig's clients took to load the world (rig_wait_connected).
// ---------------------------------------------------------------------------
// Crafts: flown by their owner's client a tick ahead of the server and agreeing
// with it exactly, seen by another client, refused to anyone else at the helm
// ---------------------------------------------------------------------------
// Puts a client's body somewhere on the server; its client hears of it in the next
// snapshot and takes the server's word, as after any large misprediction.
static void place_on_server(server *s, cw_entity_id player, v3 at) {
    Transform *t = CW_GET(&s->zone.world, player, s->zone.c_transform, Transform);
    t->position = t->previous = at;
    CW_GET(&s->zone.world, player, s->zone.c_player, Player)->velocity = v3_make(0, 0, 0);
    if (cw_comp_has(&s->zone.world, player, s->zone.c_aboard)) cw_comp_remove(&s->zone.world, player, s->zone.c_aboard);
}
static void press_use(rig *r, int i) {
    r->inputs[i].use = true;
    rig_step(r);
    r->inputs[i].use = false;
}

// What may part a client's prediction from the server's flight: inputs of the
// player's the server did not apply as sent (ticks it had none, those that came too
// late), and ticks the server skipped, stalled past its catch-up (a machine too
// slow or too loaded to keep the clock). Without any of these it must be exact.
static uint32_t missed_inputs(server *s, cw_entity_id player) {
    const server_client *c = server_client_of(s, player);
    return c ? c->inputs.starved + c->inputs.skipped + s->ticks_skipped : 0;
}

static void test_crafts_online(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.3);
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    game *ga = world_of(&r, a), *gb = world_of(&r, b), *gs = &s->zone;

    // Alice's craft, parked on the hub; both clients are sent it, parts and all.
    cw_entity_id craft = game_spawn_craft(gs, NULL, v3_make(12.0f, 0.0f, -6.0f), 0.0f, ca->account, -1);
    rig_run(&r, 0.3);
    CHECK(cw_world_is_valid(&ga->world, craft) && cw_world_is_valid(&gb->world, craft));
    CHECK(cw_world_entity_count(&ga->world) == cw_world_entity_count(&gs->world));
    if (!cw_world_is_valid(&ga->world, craft) || !cw_world_is_valid(&gb->world, craft)) { rig_destroy(&r); return; }
    CHECK(game_owner(ga, craft) == ca->account);
    const Craft *on_server = CW_GET(&gs->world, craft, gs->c_craft, Craft);
    const Craft *on_a = CW_GET(&ga->world, craft, ga->c_craft, Craft), *on_b = CW_GET(&gb->world, craft, gb->c_craft, Craft);
    CHECK(on_a->mass == on_server->mass && on_a->thrust[CRAFT_GROUP_UP] == on_server->thrust[CRAFT_GROUP_UP]);
    v3 helm = game_craft_to_world(gs, craft, on_server->helm);

    // Bob at the helm first: it is not his to take, on the server or in his prediction.
    place_on_server(s, cb->player, v3_add(helm, v3_make(0, 0.05f, 0)));
    rig_run(&r, 0.3);
    CHECK(game_aboard(gs, cb->player) == craft);
    CHECK(game_aboard(gb, cb->player) == craft);
    press_use(&r, b);
    rig_run(&r, 0.3);
    CHECK(on_server->pilot == CW_INVALID_ENTITY && on_b->pilot == CW_INVALID_ENTITY);
    place_on_server(s, cb->player, game_craft_to_world(gs, craft, v3_make(1.2f, 0.05f, 2.0f)));
    place_on_server(s, ca->player, v3_add(helm, v3_make(0, 0.05f, 0)));
    rig_run(&r, 0.3);
    CHECK(game_aboard(gs, ca->player) == craft && game_aboard(ga, ca->player) == craft);
    press_use(&r, a);
    rig_run(&r, 0.3);
    CHECK(on_server->pilot == ca->player && on_a->pilot == ca->player && on_b->pilot == ca->player);

    // Alice flies: up, round, forward, and stops. Her client flies the craft a round
    // trip ahead of the server and, replaying from the server's exact state, agrees
    // with it every time; Bob rides along, predicting himself in its frame.
    struct { float lift, yaw, forward, seconds; } legs[] = {{1.0f, 0, 0, 1.0f}, {0.2f, 0.8f, 0, 1.0f}, {0, 0, 1.0f, 1.0f}, {0, 0, 0, 4.0f}};
    float worst_craft = 0.0f, worst_bob = 0.0f;
    double start = 0.0;
    float engines_on_b[CRAFT_GROUPS] = {};   // the most Bob saw each thruster group fire at
    uint32_t starved = 0;
    for (size_t leg = 0; leg < sizeof(legs) / sizeof(legs[0]); ++leg) {
        r.inputs[a].lift = legs[leg].lift;
        r.inputs[a].turn_yaw = legs[leg].yaw;
        r.inputs[a].forward = legs[leg].forward;
        start = net_seconds();
        while (net_seconds() - start < legs[leg].seconds) {
            rig_step(&r);
            if (ca->craft_error > worst_craft) worst_craft = ca->craft_error;
            if (cb->last_error > worst_bob) worst_bob = cb->last_error;
            for (int k = 0; k < CRAFT_GROUPS; ++k)
                if (on_b->throttle[k] > engines_on_b[k]) engines_on_b[k] = on_b->throttle[k];
        }
    }
    // Bob saw her engines fire: the lift on the way up, the mains on the way forward.
    CHECK(engines_on_b[CRAFT_GROUP_UP] > 0.5f);
    CHECK(engines_on_b[CRAFT_GROUP_FORWARD] > 0.9f);
    CHECK(on_b->mode == CRAFT_FLIGHT && on_b->assist);
    starved = missed_inputs(s, ca->player);
    v3 parked = v3_make(12.0f, 0.0f, -6.0f);
    v3 there = position_in(gs, craft);
    CHECK(there.y > parked.y + 3.0f);
    CHECK(distance(there, parked) > 8.0f);
    CHECK(worst_craft < 0.001f || starved > 0);
    CHECK(worst_bob < 0.002f);
    CHECK(game_aboard(gs, cb->player) == craft && game_aboard(gb, cb->player) == craft);
    CHECK(game_aboard(gs, ca->player) == craft && on_server->pilot == ca->player);
    // Hovering, every view agrees: Alice's (ahead, but nothing is moving), Bob's
    // interpolated craft, and where each sees the other standing on it.
    CHECK(v3_length(on_server->velocity) < 0.1f);
    CHECK(distance(position_in(ga, craft), there) < 0.05f);
    CHECK(distance(position_in(gb, craft), there) < 0.05f);
    v3 bob_on_a = position_in(ga, cb->player), bob_on_server = position_in(gs, cb->player);
    CHECK(distance(bob_on_a, bob_on_server) < 0.05f);
    CHECK(distance(position_in(gb, ca->player), game_craft_to_world(gs, craft, on_server->helm)) < 0.05f);
    CHECK(fabsf(quat_dot(on_b->orientation, on_server->orientation)) > 0.9999f);
    CHECK(fabsf(quat_dot(on_server->orientation, quat_identity())) < 0.99f);   // it did turn

    // Alice puts it in landing mode and brings it down onto the hub: it settles and
    // powers down with her still at the helm. Her prediction holds to the bit all the
    // way, and Bob sees the mode, the engines dying away and the craft at rest.
    r.inputs[a].landing = true;
    r.inputs[a].lift = -1.0f;
    start = net_seconds();
    float worst_landing = 0.0f;
    while (net_seconds() - start < 8.0 && !(on_server->resting && on_b->resting)) {
        rig_step(&r);
        if (ca->craft_error > worst_landing) worst_landing = ca->craft_error;
    }
    rig_run(&r, 0.3);
    CHECK(worst_landing < 0.001f || missed_inputs(s, ca->player) > starved);
    CHECK(on_server->resting && on_server->mode == CRAFT_LANDING && on_server->pilot == ca->player);
    CHECK(on_a->resting && on_a->mode == CRAFT_LANDING);
    CHECK(on_b->resting && on_b->mode == CRAFT_LANDING && on_b->pilot == ca->player);
    for (int k = 0; k < CRAFT_GROUPS; ++k) CHECK(on_b->throttle[k] == 0.0f && on_server->throttle[k] == 0.0f);
    CHECK(distance(position_in(gb, craft), position_in(gs, craft)) < 0.01f);
    CHECK(position_in(gs, craft).y < 2.0f);   // on the hub's floor: its draft is a metre and a half
    r.inputs[a].lift = 0.0f;
    r.inputs[a].landing = false;
    rig_run(&r, 0.3);   // in flight mode on the ground, the computer holds it up: awake

    // Alice lets go of the helm: its engines stop, and it drops to the hub. Her
    // client stops predicting it and draws it from snapshots again, from fresh ones:
    // never back where it was when she took it.
    press_use(&r, a);
    float farthest = 0.0f;
    start = net_seconds();
    while (net_seconds() - start < 2.5) {
        rig_step(&r);
        float off = distance(position_in(ga, craft), position_in(gs, craft));
        if (off > farthest) farthest = off;
    }
    CHECK(on_server->pilot == CW_INVALID_ENTITY && on_a->pilot == CW_INVALID_ENTITY);
    CHECK(farthest < 3.0f);                                   // a fall seen a moment late, no more
    CHECK(on_server->resting);                                // landed on the hub, and still
    CHECK(distance(position_in(ga, craft), position_in(gs, craft)) < 0.01f);
    CHECK(game_aboard(gs, ca->player) == craft && game_aboard(gs, cb->player) == craft);

    // Bob cannot put the owner off her own craft; the owner can put him off. He is
    // set down behind the stern on the server and in his own prediction, hears that
    // he is barred, and walking back up the stairs he is stopped at the gangway,
    // his prediction agreeing with the server as he is.
    net_session_kick(&r.sessions[b], ca->player);
    rig_run(&r, 0.3);
    CHECK(s->kicks_refused == 1 && game_aboard(gs, ca->player) == craft);
    net_session_kick(&r.sessions[a], cb->player);
    rig_run(&r, 0.5);
    CHECK(s->kicks == 1);
    CHECK(game_aboard(gs, cb->player) == CW_INVALID_ENTITY && game_aboard(gb, cb->player) == CW_INVALID_ENTITY);
    CHECK(!game_may_board(gs, cb->player, craft) && !game_may_board(gb, cb->player, craft) && !game_may_board(ga, cb->player, craft));
    CHECK(distance(position_in(gb, cb->player), position_in(gs, cb->player)) < 0.6f);   // a round trip ahead of the server
    v3 gangway = game_craft_to_world(gs, craft, v3_make(0, 0, 1.0f));
    float worst_outside = 0.0f;
    start = net_seconds();
    while (net_seconds() - start < 3.0) {
        v3 way = v3_sub(gangway, position_in(gb, cb->player));
        r.inputs[b].yaw = atan2f(-way.x, -way.z);
        r.inputs[b].forward = 1.0f;
        rig_step(&r);
        CHECK(game_aboard(gs, cb->player) == CW_INVALID_ENTITY && game_aboard(gb, cb->player) == CW_INVALID_ENTITY);
        if (net_seconds() - start > 0.5 && cb->last_error > worst_outside) worst_outside = cb->last_error;
    }
    r.inputs[b].forward = 0.0f;
    rig_run(&r, 0.3);
    CHECK(worst_outside < 0.01f);
    // Wherever he was set down (behind the stern, or beside the craft where the hub
    // ends behind it), he is on his feet by it, and outside the space over its deck.
    v3 reached = game_craft_from_world(gs, craft, position_in(gs, cb->player));
    const Craft *shape = CW_GET(&gs->world, craft, gs->c_craft, Craft);
    CHECK(v3_length(reached) < shape->radius + 3.0f);
    CHECK(CW_GET(&gs->world, cb->player, gs->c_player, Player)->grounded);
    CHECK(fabsf(reached.x) >= shape->cabin_half.x || fabsf(reached.z) >= shape->cabin_half.z || reached.y < -0.1f);

    // Alice leaves: her craft waits for her a while (here half a second; her body
    // lingers as long again) and is then put away.
    rig_drop_client(&r, a);
    rig_run(&r, 0.3);
    CHECK(cw_world_is_valid(&gs->world, craft));
    rig_run(&r, 2.5);
    CHECK(!cw_world_is_valid(&gs->world, craft));
    CHECK(!cw_world_is_valid(&gb->world, craft));
    CHECK(game_aboard(gs, cb->player) == CW_INVALID_ENTITY);
    CHECK(game_aboard(gb, cb->player) == CW_INVALID_ENTITY);
    CHECK(cb->state == NC_CONNECTED);
    rig_destroy(&r);
}

// Sailing online: Alice at the helm, Bob at the sails. Bob hoists and braces the
// sail and everyone sees it set and turned; Alice's prediction of her craft, which
// cannot know Bob's hands, is exact again as soon as they rest; and left with nobody
// at helm or sails, the craft sails on.
static void test_sails_online(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    s->zone.currents = true;   // as --currents does: the clients hear of it in the welcome
    int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.3);
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    game *ga = world_of(&r, a), *gb = world_of(&r, b), *gs = &s->zone;
    CHECK(ga->currents && gb->currents);
    // High over the hub, its bow where the current flows.
    v3 at = v3_make(0.0f, 60.0f, 0.0f);
    v3 flow = game_solar_current_at_tick(gs, at, gs->world.tick);
    cw_entity_id craft = game_spawn_craft(gs, NULL, at, atan2f(-flow.x, -flow.z), ca->account, -1);
    CW_GET(&gs->world, craft, gs->c_craft, Craft)->resting = false;
    rig_run(&r, 0.3);
    const Craft *on_server = CW_GET(&gs->world, craft, gs->c_craft, Craft);
    place_on_server(s, ca->player, game_craft_to_world(gs, craft, v3_make(0, 0.05f, -3.0f)));
    place_on_server(s, cb->player, game_craft_to_world(gs, craft, v3_add(on_server->sails[0].station, v3_make(0, 0.05f, 0))));
    rig_run(&r, 0.3);
    press_use(&r, a);
    press_use(&r, b);
    rig_run(&r, 0.5);
    CHECK(on_server->pilot == ca->player && on_server->sails[0].hand == cb->player);
    if (!cw_world_is_valid(&gb->world, craft) || !cw_world_is_valid(&ga->world, craft)) { rig_destroy(&r); return; }
    const Craft *on_a = CW_GET(&ga->world, craft, ga->c_craft, Craft), *on_b = CW_GET(&gb->world, craft, gb->c_craft, Craft);
    CHECK(on_a->sails[0].hand == cb->player && on_b->sails[0].hand == cb->player);

    // Bob hoists, and braces a little to starboard.
    r.inputs[b].sail = 1.0f;
    r.inputs[b].brace = 1.0f;
    rig_run(&r, 0.5);
    r.inputs[b].brace = 0.0f;
    rig_run(&r, 2.0);
    r.inputs[b].sail = 0.0f;
    rig_run(&r, 0.5);
    CHECK(on_server->sails[0].set == 1.0f && on_a->sails[0].set == 1.0f && on_b->sails[0].set == 1.0f);
    CHECK(on_server->sails[0].brace > 0.2f && on_a->sails[0].brace == on_server->sails[0].brace && on_b->sails[0].brace == on_server->sails[0].brace);
    CHECK(on_server->sails[0].fill > 0.5f);
    uint32_t starved = missed_inputs(s, ca->player);
    float worst = 0.0f;
    double start = net_seconds();
    while (net_seconds() - start < 1.5) {
        rig_step(&r);
        if (ca->craft_error > worst) worst = ca->craft_error;
    }
    CHECK(worst < 0.001f || missed_inputs(s, ca->player) > starved);
    // Both step away: it sails on, straight and level.
    press_use(&r, a);
    press_use(&r, b);
    rig_run(&r, 0.5);
    CHECK(on_server->pilot == CW_INVALID_ENTITY && on_server->sails[0].hand == CW_INVALID_ENTITY && on_server->sails[0].set == 1.0f);
    CHECK(on_b->sails[0].hand == CW_INVALID_ENTITY);
    v3 sailing = position_in(gs, craft);
    float height = sailing.y;
    rig_run(&r, 2.0);
    v3 bow = quat_rotate(on_server->orientation, v3_make(0, 0, -1));
    CHECK(v3_dot(v3_sub(position_in(gs, craft), sailing), bow) > 10.0f);
    CHECK(fabsf(position_in(gs, craft).y - height) < 0.5f);
    CHECK(distance(position_in(gb, craft), position_in(gs, craft)) < 5.0f);
    CHECK(game_aboard(gs, cb->player) == craft && game_aboard(gb, cb->player) == craft);
    rig_destroy(&r);
}

// The parts of a craft a world holds: all of them, once its records have arrived.
static int craft_parts_in(game *g, cw_entity_id craft) {
    int n = 0;
    cw_query query = cw_query_begin(g->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&query, &id)) n += CW_GET(&g->world, id, g->c_attachment, Attachment)->ship == craft;
    return n;
}
// Whether a client holds a craft whole: its record and every part the server has.
static bool holds_craft(game *client, game *server, cw_entity_id craft) {
    return cw_world_is_valid(&client->world, craft) && cw_comp_has(&client->world, craft, client->c_craft)
        && craft_parts_in(client, craft) == craft_parts_in(server, craft);
}

// A galleon online: bought at the shipwright's counter (and not from anywhere
// else), kept by the backend, refitted with great wheels and rebuilt; the clients
// are sent it whole (its loadout, its three masts, each part's mast), a hand works
// the mizzen alone, and its owner flies it from the helm, her prediction agreeing
// with the server as a sloop's does.
static void test_ships_online(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.3);
    net_client *ca = link_of(&r, a), *cb = link_of(&r, b);
    game *ga = world_of(&r, a), *gb = world_of(&r, b), *gs = &s->zone;
    // Alice's character arrives: a sloop, and the purse everyone has for now.
    RIG_UNTIL(&r, 10.0, ca->gear_known);
    CHECK(ca->gear_known && ca->gear.bottomless && fleet_owns(&ca->gear, HULL_SLOOP) && !fleet_owns(&ca->gear, HULL_GALLEON));
    int counter = -1;
    for (int i = 0; i < gs->place_count; ++i)
        if (gs->places[i].kind == PLACE_SHIPWRIGHT) counter = i;
    CHECK(counter >= 0);
    if (counter < 0) { rig_destroy(&r); return; }
    // An order from the middle of the hub is refused: the shipwright is in the yard.
    shipwright_order buy = {SHIPWRIGHT_BUY_HULL, HULL_GALLEON, 0, 0}, sail = {SHIPWRIGHT_SAIL, HULL_GALLEON, 0, 0};
    net_client_shipwright(ca, &buy);
    RIG_UNTIL(&r, 10.0, ca->shipwright_answers >= 1);
    CHECK(ca->shipwright_answers == 1 && ca->shipwright_result == SHIPWRIGHT_INVALID && s->shipwright_refused == 1);
    CHECK(!fleet_owns(&ca->gear, HULL_GALLEON));
    // At the counter she buys a galleon and sails her: the yard builds her, on a large berth.
    place_on_server(s, ca->player, gs->places[counter].spawn);
    rig_run(&r, 0.3);
    net_client_shipwright(ca, &buy);
    net_client_shipwright(ca, &sail);
    RIG_UNTIL(&r, 10.0, ca->shipwright_answers >= 3 && ca->gear.ships.sailing == HULL_GALLEON);
    CHECK(ca->shipwright_answers == 3 && ca->shipwright_result == SHIPWRIGHT_DONE && s->shipwright_orders == 2);
    CHECK(fleet_owns(&ca->gear, HULL_GALLEON) && ca->gear.ships.sailing == HULL_GALLEON && ca->gear.spent == ship_hull_price(HULL_GALLEON));
    const character *kept = social_character(s->local_social, ca->account, NULL);   // and the backend keeps it
    CHECK(kept && fleet_owns(kept, HULL_GALLEON) && kept->ships.sailing == HULL_GALLEON);
    // A fitting bought: her gyros, every one; she is rebuilt with them.
    cw_entity_id first = game_craft_of(gs, ca->account);
    shipwright_order wheels = {SHIPWRIGHT_FIT_ALL, 0, SLOT_GYRO, (uint8_t)ship_component_find("Great Wheel")};
    net_client_shipwright(ca, &wheels);
    RIG_UNTIL(&r, 10.0, ca->shipwright_answers >= 4 && ca->gear.ships.store[ship_component_find("Spinning Brass")] == 3);
    CHECK(ca->shipwright_result == SHIPWRIGHT_DONE && ca->gear.ships.store[ship_component_find("Spinning Brass")] == 3);
    CHECK(game_craft_of(gs, ca->account) != first);
    // Her look: navy paint and solar sails, bought a field at a time; she is rebuilt
    // so, and every client sees her so (the loadout's look, and the paint on her parts).
    shipwright_order navy = {SHIPWRIGHT_STYLE, 0, LOOK_HULL_PAINT, 5}, solar = {SHIPWRIGHT_STYLE, 0, LOOK_SAIL_CUT, CUT_SOLAR};
    net_client_shipwright(ca, &navy);
    net_client_shipwright(ca, &solar);
    RIG_UNTIL(&r, 10.0, ca->shipwright_answers >= 6 && fleet_sailing(&ca->gear)->look.field[LOOK_SAIL_CUT] == CUT_SOLAR);
    CHECK(ca->shipwright_result == SHIPWRIGHT_DONE && fleet_sailing(&ca->gear)->look.field[LOOK_HULL_PAINT] == 5);
    cw_entity_id craft = game_craft_of(gs, ca->account);
    CHECK(craft != CW_INVALID_ENTITY);
    // Both clients are sent her whole: her record and every one of her parts.
    RIG_UNTIL(&r, 15.0, craft != CW_INVALID_ENTITY && holds_craft(ga, gs, craft) && holds_craft(gb, gs, craft));
    if (craft == CW_INVALID_ENTITY || !cw_world_is_valid(&ga->world, craft) || !cw_world_is_valid(&gb->world, craft)) {
        CHECK(false);
        rig_destroy(&r);
        return;
    }
    const Craft *on_server = CW_GET(&gs->world, craft, gs->c_craft, Craft);
    const Craft *on_a = CW_GET(&ga->world, craft, ga->c_craft, Craft), *on_b = CW_GET(&gb->world, craft, gb->c_craft, Craft);
    CHECK(on_server->loadout.hull == HULL_GALLEON && gs->berths[on_server->berth].size == BERTH_LARGE);
    CHECK(ship_loadout_equal(&on_a->loadout, &on_server->loadout) && ship_loadout_equal(&on_b->loadout, &on_server->loadout));
    CHECK(on_a->masts == 3 && on_a->mass == on_server->mass && on_a->inertia.y == on_server->inertia.y);
    CHECK(on_a->response == on_server->response && on_a->turn_response == on_server->turn_response && on_a->hull_drag == on_server->hull_drag);
    for (int m = 0; m < 3; ++m)
        CHECK(on_a->sails[m].area == on_server->sails[m].area && v3_length(v3_sub(on_a->sails[m].station, on_server->sails[m].station)) == 0.0f);
    // Every part arrived, with the mast it belongs to.
    int sails[3] = {}, parts = 0;
    cw_query query = cw_query_begin(ga->q_attachments);
    cw_entity_id id;
    while (cw_query_next(&query, &id)) {
        if (CW_GET(&ga->world, id, ga->c_attachment, Attachment)->ship != craft) continue;
        const Part *part = CW_GET(&ga->world, id, ga->c_part, Part);
        ++parts;
        if (part->kind == PART_SAIL && part->mast >= 0 && part->mast < 3) ++sails[part->mast];
    }
    CHECK(parts > 60 && sails[0] == 1 && sails[1] == 1 && sails[2] == 1);
    CHECK(on_a->loadout.look.field[LOOK_HULL_PAINT] == 5 && on_b->loadout.look.field[LOOK_SAIL_CUT] == CUT_SOLAR);
    v3 navy_color;
    ship_paint_color(5, &navy_color);
    int navy_parts = 0;
    query = cw_query_begin(gb->q_attachments);
    while (cw_query_next(&query, &id)) {
        if (CW_GET(&gb->world, id, gb->c_attachment, Attachment)->ship != craft || !cw_comp_has(&gb->world, id, gb->c_part)) continue;
        const Part *part = CW_GET(&gb->world, id, gb->c_part, Part);
        if (part->paint != PAINT_HULL) continue;
        ++navy_parts;
        CHECK(memcmp(&CW_GET(&gb->world, id, gb->c_visual, Visual)->color, &navy_color, sizeof(v3)) == 0);
    }
    CHECK(navy_parts >= 1);

    // Alice to the helm, Bob to the mizzen.
    place_on_server(s, ca->player, game_craft_to_world(gs, craft, v3_add(on_server->helm, v3_make(0, 0.05f, 0))));
    place_on_server(s, cb->player, game_craft_to_world(gs, craft, v3_add(on_server->sails[2].station, v3_make(0, 0.05f, 0))));
    rig_run(&r, 0.3);
    press_use(&r, a);
    press_use(&r, b);
    RIG_UNTIL(&r, 10.0, on_server->pilot == ca->player && on_a->sails[2].hand == cb->player && on_b->sails[2].hand == cb->player);
    CHECK(on_server->pilot == ca->player && on_server->sails[2].hand == cb->player && on_server->sails[0].hand == CW_INVALID_ENTITY);
    CHECK(on_a->sails[2].hand == cb->player && on_b->sails[2].hand == cb->player);
    r.inputs[b].sail = 1.0f;
    r.inputs[a].lift = 1.0f;
    RIG_UNTIL(&r, 15.0, on_server->sails[2].set == 1.0f && game_craft_altitude(gs, craft) > 5.0f);
    r.inputs[b].sail = 0.0f;
    r.inputs[a].lift = 0.0f;
    RIG_UNTIL(&r, 10.0, on_a->sails[2].set == 1.0f && on_b->sails[2].set == 1.0f);
    CHECK(on_server->sails[2].set == 1.0f && on_server->sails[0].set == 0.0f && on_server->sails[1].set == 0.0f);
    CHECK(on_a->sails[2].set == 1.0f && on_b->sails[2].set == 1.0f && on_b->sails[0].set == 0.0f);
    CHECK(game_craft_altitude(gs, craft) > 5.0f);
    // Alice brings her round: her prediction agrees with the server's flight.
    uint32_t starved = missed_inputs(s, ca->player);
    r.inputs[a].turn_yaw = 1.0f;
    float worst = 0.0f;
    double start = net_seconds();
    while (net_seconds() - start < 1.5) {
        rig_step(&r);
        if (ca->craft_error > worst) worst = ca->craft_error;
    }
    r.inputs[a].turn_yaw = 0.0f;
    CHECK(worst < 0.001f || missed_inputs(s, ca->player) > starved);
    CHECK(v3_length(on_server->spin) > 0.05f);
    rig_destroy(&r);
}

// Docking online: the docks reach every client with their names; Alice asks at the
// helm and her craft's autopilot brings it in, her prediction agreeing with the
// server as it flies, and Bob sees it docking and docked.
static void test_docking_online(void) {
    rig r;
    rig_init(&r, 0.0f);
    server *s = r.servers[0];
    int a = rig_add_client_named(&r, s, 0.0f, "Alice"), b = rig_add_client_named(&r, s, 0.0f, "Bob");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    rig_run(&r, 0.3);
    net_client *ca = link_of(&r, a);
    game *ga = world_of(&r, a), *gb = world_of(&r, b), *gs = &s->zone;
    CHECK(cw_query_count(ga->q_docks) == 12 && cw_query_count(gb->q_docks) == 12);
    bool named = false;
    cw_query docks = cw_query_begin(gb->q_docks);
    cw_entity_id id;
    while (cw_query_next(&docks, &id)) named = named || !strcmp(CW_GET(&gb->world, id, gb->c_dock, Dock)->name, "Hub Dock 2");
    CHECK(named);
    // Her sail set, she holds where she is with nobody at the helm, however long
    // Alice takes to get there.
    cw_entity_id craft = game_spawn_craft(gs, NULL, v3_make(80.0f, 30.0f, -40.0f), 1.0f, ca->account, -1);
    Craft *spawned = CW_GET(&gs->world, craft, gs->c_craft, Craft);
    spawned->resting = false;
    spawned->sails[0].set = 1.0f;
    RIG_UNTIL(&r, 10.0, holds_craft(ga, gs, craft));
    const Craft *on_server = CW_GET(&gs->world, craft, gs->c_craft, Craft);
    place_on_server(s, ca->player, game_craft_to_world(gs, craft, v3_add(on_server->helm, v3_make(0, 0.05f, 0))));
    RIG_UNTIL(&r, 10.0, game_aboard(gs, ca->player) == craft && game_aboard(ga, ca->player) == craft);
    press_use(&r, a);
    RIG_UNTIL(&r, 10.0, on_server->pilot == ca->player && CW_GET(&ga->world, craft, ga->c_craft, Craft)->pilot == ca->player);
    CHECK(on_server->pilot == ca->player);
    uint32_t missed = missed_inputs(s, ca->player);
    r.inputs[a].dock = true;
    rig_step(&r);
    r.inputs[a].dock = false;
    float worst = 0.0f;
    bool seen_docking = false;
    double start = net_seconds();
    while (net_seconds() - start < 90.0 && on_server->docking != DOCK_DOCKED) {
        rig_step(&r);
        if (ca->craft_error > worst) worst = ca->craft_error;
        if (cw_world_is_valid(&gb->world, craft) && CW_GET(&gb->world, craft, gb->c_craft, Craft)->docking == DOCK_APPROACH) seen_docking = true;
    }
    RIG_UNTIL(&r, 10.0, CW_GET(&gb->world, craft, gb->c_craft, Craft)->docking == DOCK_DOCKED
                       && CW_GET(&ga->world, craft, ga->c_craft, Craft)->docking == DOCK_DOCKED);
    CHECK(on_server->docking == DOCK_DOCKED && on_server->resting);
    CHECK(worst < 0.001f || missed_inputs(s, ca->player) > missed);
    CHECK(seen_docking && CW_GET(&gb->world, craft, gb->c_craft, Craft)->docking == DOCK_DOCKED);
    CHECK(CW_GET(&ga->world, craft, ga->c_craft, Craft)->docking == DOCK_DOCKED);
    rig_destroy(&r);
}

// Players a client sees named as captains are.
static int captains_seen(game *g) {
    int named = 0;
    cw_query players = cw_query_begin(g->q_all_players);
    cw_entity_id id;
    while (cw_query_next(&players, &id)) named += strncmp(CW_GET(&g->world, id, g->c_player, Player)->name, "Captain ", 8) == 0;
    return named;
}

// Captains on a server: clients see them (players named so, at the helms of ships
// of their own) and their ships leave their docks; the server keeps their ships,
// whose owners are none of its clients.
static void test_captains_online(void) {
    rig r;
    memset(&r, 0, sizeof(r));
    server_config config = server_config_default();
    config.port = 0;
    config.max_clients = RIG_MAX;
    config.craft_linger = 0.5;
    config.captains = 3;
    server *s = rig_add_server(&r, &config, 0.0f);
    int a = rig_add_client_named(&r, s, 0.0f, "Alice");
    CHECK(rig_wait_connected(&r, 5.0));
    if (failures) { rig_destroy(&r); return; }
    game *ga = world_of(&r, a), *gs = &s->zone;
    CHECK(cw_query_count(gs->q_captains) == 3 && cw_query_count(gs->q_crafts) == 3);
    RIG_UNTIL(&r, 15.0, captains_seen(ga) == 3 && cw_query_count(ga->q_crafts) == 3);
    CHECK(captains_seen(ga) == 3);
    CHECK(cw_query_count(ga->q_crafts) == 3);
    // Their time in port cut short (it is ten to forty seconds, by their dice): the
    // first to leave is seen leaving, and the sweep, long past, has left their ships be.
    bool left = false;
    cw_entity_id id;
    double start = net_seconds();
    while (net_seconds() - start < 60.0 && !left) {
        rig_step(&r);
        cw_query captains = cw_query_begin(gs->q_captains);
        while (cw_query_next(&captains, &id)) {
            Captain *cap = CW_GET(&gs->world, id, gs->c_captain, Captain);
            if (cap->state == CAPTAIN_IN_PORT && cap->timer < -1.0f) cap->timer = -1.0f;
        }
        cw_query crafts = cw_query_begin(ga->q_crafts);
        while (cw_query_next(&crafts, &id))
            if (!CW_GET(&ga->world, id, ga->c_craft, Craft)->resting && CW_GET(&ga->world, id, ga->c_craft, Craft)->docking == DOCK_NONE) left = true;
    }
    CHECK(left);
    rig_run(&r, 1.5);   // a sweep or two more
    CHECK(cw_query_count(gs->q_crafts) == 3);
    rig_destroy(&r);
}

#define RUN(name) do { const char *only = getenv("TEST_ONLY"); if (!only || !strcmp(only, #name)) name(); } while (0)

int main(void) {
    if (!net_init()) {
        puts("test_net: the network could not be initialised");
        return 1;
    }
    RUN(test_channels_under_loss);
    RUN(test_packet_validation);
    RUN(test_inbound_limit);
    RUN(test_rtt_and_loss);
    RUN(test_fragmentation);
    RUN(test_congestion_and_scheduling);
    RUN(test_crypto);
    RUN(test_snapshot_codec);
    RUN(test_tickets);
    RUN(test_two_clients_see_each_other);
    RUN(test_join_and_play_under_loss);
    RUN(test_rejections);
    RUN(test_zones);
    RUN(test_handoff_failure);
    RUN(test_server_clock);
    RUN(test_solo_instances);
    RUN(test_bandwidth_limit);
    RUN(test_lag_compensation);
    RUN(test_crafts_online);
    RUN(test_sails_online);
    RUN(test_ships_online);
    RUN(test_docking_online);
    RUN(test_captains_online);
    RUN(test_directory);
    RUN(test_social_service);
    RUN(test_party);
    RUN(test_shard_choice);
    RUN(test_shards);
    RUN(test_loot);
    RUN(test_relevance);
    RUN(test_reconnection);
    RUN(test_handshake_defences);
    RUN(test_connect_tokens);
    RUN(test_orchestrator_plan);
#ifdef TP_SERVER_PATH
    RUN(test_orchestrator);
#endif
    net_shutdown();
    if (failures) {
        printf("test_net: %d failed\n", failures);
        return 1;
    }
    puts("test_net: ok");
    return 0;
}
