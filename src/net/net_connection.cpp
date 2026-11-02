// Connections: packet framing, the unreliable channel and the ordered reliable
// channel. See net.h for the packet layout.
#include "net.h"
#include <stdlib.h>
#include <string.h>

#define NET_MAGIC ((uint32_t)'T' | ((uint32_t)'P' << 8) | ((uint32_t)'N' << 16) | ((uint32_t)NET_PROTOCOL_VERSION << 24))
#define NET_RELIABLE_FLAG 0x80u
#define NET_RELIABLE_BACKLOG 4096    // reliable messages a connection will queue before refusing

int net_sequence_compare(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }

static void write_header(unsigned char *packet, uint32_t session, uint16_t ack) {
    cw_writer w;
    cw_writer_init_fixed(&w, packet, NET_PACKET_HEADER);
    cw_writer_u32(&w, NET_MAGIC);
    cw_writer_u32(&w, session);
    cw_writer_u16(&w, ack);
}

static bool read_header(cw_reader *r, uint32_t *session, uint16_t *ack) {
    uint32_t magic = 0;
    cw_reader_u32(r, &magic);
    cw_reader_u32(r, session);
    cw_reader_u16(r, ack);
    return cw_reader_ok(r) && magic == NET_MAGIC;
}

bool net_packet_peek(const void *packet, size_t size, uint32_t *session) {
    cw_reader r = cw_reader_make(packet, size);
    uint32_t s = 0;
    uint16_t ack;
    if (!read_header(&r, &s, &ack)) return false;
    if (session) *session = s;
    return true;
}

void net_connection_init(net_connection *c, net_socket *socket, net_address peer, uint32_t session,
                         net_message_fn on_message, void *user) {
    memset(c, 0, sizeof(*c));
    c->socket = socket;
    c->peer = peer;
    c->session = session;
    c->on_message = on_message;
    c->user = user;
    c->outgoing_size = NET_PACKET_HEADER;
    c->last_received = c->last_sent = net_seconds();
}

void net_connection_destroy(net_connection *c) {
    for (size_t i = 0; i < c->send_count; ++i) free(c->send_queue[i].data);
    free(c->send_queue);
    for (size_t i = 0; i < NET_RELIABLE_WINDOW; ++i) free(c->receive_slots[i].data);
    memset(c, 0, sizeof(*c));
}

size_t net_connection_unacked(const net_connection *c) { return c->send_count; }

static void send_outgoing(net_connection *c, double now) {
    write_header(c->outgoing, c->session, (uint16_t)(c->receive_expected - 1u));
    net_socket_send(c->socket, c->peer, c->outgoing, c->outgoing_size);
    c->outgoing_size = NET_PACKET_HEADER;
    c->last_sent = now;
    ++c->packets_sent;
}

// Appends an encoded message to the packet under construction, sending the current
// packet first when the message does not fit.
static void append_message(net_connection *c, const unsigned char *message, size_t size, double now) {
    if (c->outgoing_size + size > NET_MAX_PACKET) send_outgoing(c, now);
    memcpy(c->outgoing + c->outgoing_size, message, size);
    c->outgoing_size += size;
}

bool net_connection_send(net_connection *c, uint8_t type, bool reliable, const void *payload, size_t size) {
    if ((type & NET_RELIABLE_FLAG) || size > NET_MAX_PAYLOAD) return false;
    if (!reliable) {
        unsigned char message[NET_MAX_PACKET];
        cw_writer w;
        cw_writer_init_fixed(&w, message, sizeof(message));
        cw_writer_u8(&w, type);
        cw_writer_u16(&w, (uint16_t)size);
        cw_writer_bytes(&w, payload, size);
        append_message(c, message, w.size, net_seconds());
        return true;
    }
    if (c->send_count >= NET_RELIABLE_BACKLOG) return false;
    if (c->send_count == c->send_capacity) {
        c->send_capacity = c->send_capacity ? c->send_capacity * 2 : 64;
        c->send_queue = (net_reliable_message *)realloc(c->send_queue, c->send_capacity * sizeof(*c->send_queue));
    }
    net_reliable_message *m = &c->send_queue[c->send_count];
    m->size = NET_MESSAGE_HEADER + 2 + size;
    m->data = (unsigned char *)malloc(m->size);
    m->sent_at = -1.0;
    cw_writer w;
    cw_writer_init_fixed(&w, m->data, m->size);
    cw_writer_u8(&w, (uint8_t)(type | NET_RELIABLE_FLAG));
    cw_writer_u16(&w, (uint16_t)size);
    cw_writer_u16(&w, (uint16_t)(c->send_base + c->send_count));
    cw_writer_bytes(&w, payload, size);
    ++c->send_count;
    return true;
}

static void acknowledge(net_connection *c, uint16_t ack) {
    // Pops every message up to `ack`, but only ones that were actually sent: an
    // ack ahead of those is corrupt or stale and is ignored.
    while (c->send_count && c->send_queue[0].sent_at >= 0.0 && net_sequence_compare(ack, c->send_base) >= 0) {
        free(c->send_queue[0].data);
        memmove(c->send_queue, c->send_queue + 1, (c->send_count - 1) * sizeof(*c->send_queue));
        --c->send_count;
        ++c->send_base;
    }
}

static void deliver(net_connection *c, uint8_t type, const unsigned char *payload, size_t size) {
    if (c->on_message) c->on_message(c->user, c, type, cw_reader_make(payload, size));
}

bool net_connection_receive(net_connection *c, const void *packet, size_t size, double now) {
    cw_reader r = cw_reader_make(packet, size);
    uint32_t session = 0;
    uint16_t ack = 0;
    if (!read_header(&r, &session, &ack) || session != c->session) {
        ++c->packets_rejected;
        return false;
    }
    c->last_received = now;
    ++c->packets_received;
    acknowledge(c, ack);

    while (cw_reader_remaining(&r) >= NET_MESSAGE_HEADER) {
        uint8_t type = 0;
        uint16_t length = 0;
        cw_reader_u8(&r, &type);
        cw_reader_u16(&r, &length);
        if (!(type & NET_RELIABLE_FLAG)) {
            const unsigned char *payload = (const unsigned char *)cw_reader_view(&r, length);
            if (!payload) break;   // truncated: the rest of the packet is unusable
            deliver(c, type, payload, length);
            continue;
        }
        uint16_t sequence = 0;
        cw_reader_u16(&r, &sequence);
        const unsigned char *payload = (const unsigned char *)cw_reader_view(&r, length);
        if (!payload) break;
        int distance = net_sequence_compare(sequence, c->receive_expected);
        if (distance < 0 || distance >= NET_RELIABLE_WINDOW) continue;   // duplicate, or beyond the window
        net_reliable_message *slot = &c->receive_slots[sequence % NET_RELIABLE_WINDOW];
        if (slot->data) continue;                                        // already holding this one
        slot->data = (unsigned char *)malloc(length ? length : 1);
        memcpy(slot->data, payload, length);
        slot->size = length;
        slot->type = (uint8_t)(type & (uint8_t)~NET_RELIABLE_FLAG);

        // Deliver everything that is now contiguous, in order.
        for (;;) {
            net_reliable_message *next = &c->receive_slots[c->receive_expected % NET_RELIABLE_WINDOW];
            if (!next->data) break;
            unsigned char *data = next->data;
            size_t data_size = next->size;
            uint8_t next_type = next->type;
            next->data = NULL;
            next->size = 0;
            ++c->receive_expected;
            deliver(c, next_type, data, data_size);
            free(data);
        }
    }
    return true;
}

void net_connection_flush(net_connection *c, double now, bool force) {
    size_t in_flight = c->send_count < NET_RELIABLE_WINDOW ? c->send_count : NET_RELIABLE_WINDOW;
    for (size_t i = 0; i < in_flight; ++i) {
        net_reliable_message *m = &c->send_queue[i];
        if (m->sent_at >= 0.0 && now - m->sent_at < NET_RELIABLE_RTO) continue;
        if (m->sent_at >= 0.0) ++c->messages_resent;
        append_message(c, m->data, m->size, now);
        m->sent_at = now;
    }
    if (c->outgoing_size > NET_PACKET_HEADER || force) send_outgoing(c, now);
}
