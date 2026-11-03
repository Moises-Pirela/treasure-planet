// Connections: packet framing and validation, the four channels, acknowledgements
// and retransmission, fragmentation, the round trip and the loss, congestion
// control, pacing and scheduling, the rate limits. See net.h for the packet layout.
#include "net.h"
#include <stdlib.h>
#include <string.h>

#define NET_MAGIC ((uint32_t)'T' | ((uint32_t)'P' << 8) | ((uint32_t)'N' << 16) | ((uint32_t)NET_PROTOCOL_VERSION << 24))

int net_sequence_compare(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }

// The messages region of `outgoing` starts here; what precedes it is framing space.
#define OUTGOING_START (NET_PACKET_HEADER + 8 + NET_ACKS_SIZE)

static bool is_reliable(int channel) { return channel == NET_RELIABLE || channel == NET_RELIABLE_ORDERED; }
// Reliable channels are numbered 0 and 1 in the per-channel arrays.
static int reliable_index(int channel) { return channel == NET_RELIABLE ? 0 : 1; }
static int channel_of(uint8_t header) { return header >> 6; }
static uint8_t type_of(uint8_t header) { return header & (NET_MESSAGE_TYPES - 1); }

static bool read_header(cw_reader *r, uint32_t *session) {
    uint32_t magic = 0;
    cw_reader_u32(r, &magic);
    cw_reader_u32(r, session);
    return cw_reader_ok(r) && magic == NET_MAGIC;
}

bool net_packet_peek(const void *packet, size_t size, uint32_t *session) {
    if (size > NET_MAX_PACKET) return false;
    cw_reader r = cw_reader_make(packet, size);
    uint32_t s = 0;
    if (!read_header(&r, &s)) return false;
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
    c->outgoing_size = OUTGOING_START;
    c->rto = NET_RTO_DEFAULT;
    c->last_received = c->last_sent = net_seconds();
}

void net_connection_set_keys(net_connection *c, const uint8_t send_key[CRYPTO_KEY_SIZE],
                             const uint8_t receive_key[CRYPTO_KEY_SIZE]) {
    memcpy(c->send_key, send_key, CRYPTO_KEY_SIZE);
    memcpy(c->receive_key, receive_key, CRYPTO_KEY_SIZE);
    c->encrypted = true;
    c->send_counter = 1;
    c->receive_highest = 0;
    c->receive_window = 0;
}

static void counter_nonce(uint64_t counter, uint8_t nonce[CRYPTO_NONCE_SIZE]) {
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = (uint8_t)(counter >> (8 * i));
}

void net_connection_destroy(net_connection *c) {
    for (int k = 0; k < 2; ++k) {
        for (size_t i = 0; i < c->senders[k].count; ++i) free(c->senders[k].queue[i].data);
        free(c->senders[k].queue);
    }
    for (size_t i = 0; i < NET_RELIABLE_WINDOW; ++i) free(c->slots[i].data);
    for (int i = 0; i < NET_REASSEMBLY_SLOTS; ++i) free(c->reassembly[i].data);
    memset(c, 0, sizeof(*c));
}

size_t net_connection_unacked(const net_connection *c) { return c->senders[0].count + c->senders[1].count; }

// ---------------------------------------------------------------------------
// Rate limits, congestion control, pacing
// ---------------------------------------------------------------------------
void net_connection_set_rate(net_connection *c, uint32_t bytes_per_second) {
    bool had = c->rate != 0;
    c->rate = bytes_per_second;
    if (!bytes_per_second) {
        c->congestion_rate = 0.0;
        return;
    }
    // A new cap keeps the rate where it is when that is still under it; a first
    // cap starts at the cap.
    if (!had || c->congestion_rate <= 0.0 || c->congestion_rate > bytes_per_second) c->congestion_rate = bytes_per_second;
    if (!had) {
        c->tokens = bytes_per_second * 0.25;
        c->tokens_at = net_seconds();
        c->pace_tokens = NET_PACING_BURST;
        c->pace_at = c->tokens_at;
        c->congestion_calm_since = c->tokens_at;
    }
}

uint32_t net_connection_rate(const net_connection *c) { return c->rate ? (uint32_t)c->congestion_rate : 0; }

static double refill(net_connection *c, double now) {
    if (!c->rate) return 1e18;
    double rate = c->congestion_rate;
    double burst = rate * 0.25 > 2.0 * NET_MAX_PACKET ? rate * 0.25 : 2.0 * NET_MAX_PACKET;
    c->tokens += (now - c->tokens_at) * rate;
    if (c->tokens > burst) c->tokens = burst;
    c->tokens_at = now;
    return c->tokens;
}

static double pace_refill(net_connection *c, double now) {
    if (!c->rate) return 1e18;
    c->pace_tokens += (now - c->pace_at) * c->congestion_rate;
    if (c->pace_tokens > NET_PACING_BURST) c->pace_tokens = NET_PACING_BURST;
    c->pace_at = now;
    return c->pace_tokens;
}

size_t net_connection_allowance(const net_connection *c, double now) {
    if (!c->rate) return (size_t)-1;
    double tokens = c->tokens + (now - c->tokens_at) * c->congestion_rate;
    return tokens <= 0.0 ? 0 : (size_t)tokens;
}

// Cuts the rate when the path is in trouble (loss, or the round trip well above
// its floor), at most once per round trip; raises it by a tenth of the cap after
// a calm second. Nothing is judged until the round trip has been measured.
static void update_congestion(net_connection *c, double now) {
    if (!c->rate || !c->rtt_samples) return;
    bool trouble = c->loss > NET_CONGESTION_LOSS || c->rtt > c->rtt_min + NET_CONGESTION_DELAY;
    if (trouble) {
        double interval = c->rtt > 0.1 ? c->rtt : 0.1;
        if (now - c->congestion_last_cut > interval) {
            double floor = c->rate * NET_CONGESTION_FLOOR;
            double cut = c->congestion_rate * 0.75;
            c->congestion_rate = cut > floor ? cut : floor;
            c->congestion_last_cut = now;
            c->loss *= 0.5;   // the cut is the answer to this loss; the next one is news
            ++c->congestion_cuts;
        }
        c->congestion_calm_since = now;
    } else if (c->congestion_rate < c->rate && now - c->congestion_calm_since > NET_CONGESTION_CALM) {
        double raised = c->congestion_rate + 0.1 * c->rate;
        c->congestion_rate = raised < c->rate ? raised : c->rate;
        c->congestion_calm_since = now;
        ++c->congestion_raises;
    }
}

void net_connection_set_inbound_limit(net_connection *c, uint32_t packets_per_second, uint32_t bytes_per_second) {
    c->inbound_packets = packets_per_second;
    c->inbound_bytes = bytes_per_second;
    c->inbound_packet_tokens = packets_per_second;
    c->inbound_byte_tokens = bytes_per_second;
    c->inbound_at = net_seconds();
}

// Whether a packet of `size` bytes is within what the peer may send; a second's
// worth may arrive at once.
static bool inbound_allows(net_connection *c, size_t size, double now) {
    if (!c->inbound_packets && !c->inbound_bytes) return true;
    double elapsed = now - c->inbound_at;
    if (elapsed < 0.0) elapsed = 0.0;
    c->inbound_at = now;
    c->inbound_packet_tokens += elapsed * c->inbound_packets;
    if (c->inbound_packet_tokens > c->inbound_packets) c->inbound_packet_tokens = c->inbound_packets;
    c->inbound_byte_tokens += elapsed * c->inbound_bytes;
    if (c->inbound_byte_tokens > c->inbound_bytes) c->inbound_byte_tokens = c->inbound_bytes;
    if ((c->inbound_packets && c->inbound_packet_tokens < 1.0) || (c->inbound_bytes && c->inbound_byte_tokens < (double)size))
        return false;
    c->inbound_packet_tokens -= 1.0;
    c->inbound_byte_tokens -= (double)size;
    return true;
}

// ---------------------------------------------------------------------------
// Round trip and loss, from the packet acks
// ---------------------------------------------------------------------------
static void rtt_sample(net_connection *c, double sample, double now) {
    if (sample < 0.0) sample = 0.0;
    if (!c->rtt_samples) {
        c->rtt = sample;
        c->rtt_variance = sample * 0.5;
        c->rtt_min = sample;
        c->rtt_min_at = now;
    } else {
        double deviation = c->rtt - sample;
        if (deviation < 0.0) deviation = -deviation;
        c->rtt_variance = 0.75 * c->rtt_variance + 0.25 * deviation;
        c->rtt = 0.875 * c->rtt + 0.125 * sample;
        // The floor is the least seen lately: a path that got slower for good is
        // not congestion for ever.
        if (sample <= c->rtt_min || now - c->rtt_min_at > 10.0) {
            c->rtt_min = sample < c->rtt ? sample : c->rtt;
            c->rtt_min_at = now;
        }
    }
    ++c->rtt_samples;
    double rto = c->rtt + 4.0 * c->rtt_variance;
    c->rto = rto < NET_RTO_MIN ? NET_RTO_MIN : rto > NET_RTO_MAX ? NET_RTO_MAX : rto;
}

static void loss_sample(net_connection *c, bool lost) {
    c->loss += ((lost ? 1.0 : 0.0) - c->loss) / 32.0;
    if (lost) ++c->packets_lost;
}

// The peer's word on our packets: the latest it has and the 32 before it. The
// latest gives a round trip sample (once); what the bitfield has passed by without
// naming is lost.
static void process_packet_acks(net_connection *c, uint16_t latest, uint32_t bits, double delay, double now) {
    net_packet_record *r = &c->history[latest % NET_PACKET_HISTORY];
    if (r->sent && r->sequence == latest) {
        if (!r->acked) rtt_sample(c, now - r->sent_at - delay, now);
        r->acked = true;
    }
    for (int i = 0; i < 32; ++i) {
        if (!(bits >> i & 1)) continue;
        uint16_t sequence = (uint16_t)(latest - 1 - i);
        net_packet_record *b = &c->history[sequence % NET_PACKET_HISTORY];
        if (b->sent && b->sequence == sequence) b->acked = true;
    }
    // Everything older than the bitfield is judged: acked, or lost.
    uint16_t edge = (uint16_t)(latest - 33);
    if (!c->loss_edge_set) {
        c->loss_edge = edge;
        c->loss_edge_set = true;
        return;
    }
    int behind = net_sequence_compare(edge, c->loss_edge);
    if (behind <= 0) return;
    if (behind > NET_PACKET_HISTORY - 33) behind = NET_PACKET_HISTORY - 33;   // a gap too long to judge: skip it
    for (uint16_t s = (uint16_t)(edge - behind + 1);; ++s) {
        net_packet_record *b = &c->history[s % NET_PACKET_HISTORY];
        if (b->sent && b->sequence == s) loss_sample(c, !b->acked);
        if (s == edge) break;
    }
    c->loss_edge = edge;
}

// Records a packet sequence from the peer for our acks to it.
static void note_packet_received(net_connection *c, uint16_t sequence, double now) {
    if (!c->packet_any) {
        c->packet_any = true;
        c->packet_ack_latest = sequence;
        c->packet_ack_bits = 0;
        c->packet_latest_at = now;
        return;
    }
    int ahead = net_sequence_compare(sequence, c->packet_ack_latest);
    if (ahead > 0) {
        c->packet_ack_bits = ahead >= 32 ? 0 : (c->packet_ack_bits << ahead) | ((uint32_t)1 << (ahead - 1));
        c->packet_ack_latest = sequence;
        c->packet_latest_at = now;
    } else if (ahead < 0 && -ahead <= 32) {
        c->packet_ack_bits |= (uint32_t)1 << (-ahead - 1);
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
// Frames and sends the messages gathered so far: in the clear with the acks in
// front, or under the session keys with the counter as nonce and the header as aad.
static void send_outgoing(net_connection *c, double now) {
    unsigned char packet[NET_MAX_PACKET];
    cw_writer w;
    cw_writer_init_fixed(&w, packet, sizeof(packet));
    cw_writer_u32(&w, NET_MAGIC);
    cw_writer_u32(&w, c->session);
    unsigned char plain[NET_MAX_PACKET];
    cw_writer p;
    cw_writer_init_fixed(&p, plain, sizeof(plain));
    uint16_t sequence = c->packet_sequence++;
    net_packet_record *r = &c->history[sequence % NET_PACKET_HISTORY];
    r->sequence = sequence;
    r->sent_at = now;
    r->sent = true;
    r->acked = false;
    cw_writer_u16(&p, sequence);
    double held = c->packet_any ? (now - c->packet_latest_at) * 16000.0 : 0.0;   // 1/16 ms
    cw_writer_u16(&p, c->packet_any ? c->packet_ack_latest : (uint16_t)0xffff);
    cw_writer_u32(&p, c->packet_any ? c->packet_ack_bits : 0);
    cw_writer_u16(&p, (uint16_t)(held < 0.0 ? 0.0 : held > 65535.0 ? 65535.0 : held));
    for (int k = 0; k < 2; ++k) {
        // Nothing received yet acknowledges "the one before the first": nothing.
        cw_writer_u16(&p, c->received[k].any ? c->received[k].latest : (uint16_t)0xffff);
        cw_writer_u64(&p, c->received[k].any ? c->received[k].bits : 0);
    }
    cw_writer_bytes(&p, c->outgoing + OUTGOING_START, c->outgoing_size - OUTGOING_START);
    if (!c->encrypted) {
        cw_writer_bytes(&w, plain, p.size);
    } else {
        uint64_t counter = c->send_counter++;
        cw_writer_u64(&w, counter);
        uint8_t nonce[CRYPTO_NONCE_SIZE], tag[CRYPTO_TAG_SIZE];
        counter_nonce(counter, nonce);
        unsigned char *cipher = (unsigned char *)cw_writer_claim(&w, p.size);
        crypto_aead_encrypt(c->send_key, nonce, packet, NET_PACKET_HEADER + 8, plain, p.size, cipher, tag);
        cw_writer_bytes(&w, tag, sizeof(tag));
    }
    net_socket_send(c->socket, c->peer, packet, w.size);
    c->bytes_sent += w.size;
    if (c->rate) {
        refill(c, now);
        c->tokens -= (double)w.size;   // may dip below zero: the next sends wait for it
        pace_refill(c, now);
        c->pace_tokens -= (double)w.size;
    }
    c->outgoing_size = OUTGOING_START;
    c->last_sent = now;
    c->ack_pending = false;
    ++c->packets_sent;
}

// Appends an encoded message to the packet under construction, sending the current
// packet first when the message does not fit.
static void append_message(net_connection *c, const unsigned char *message, size_t size, double now) {
    if (c->outgoing_size + size + CRYPTO_TAG_SIZE > NET_MAX_PACKET) send_outgoing(c, now);
    memcpy(c->outgoing + c->outgoing_size, message, size);
    c->outgoing_size += size;
}

static size_t encode_message(unsigned char *out, uint8_t type, int channel, uint16_t sequence, const void *payload, size_t size) {
    cw_writer w;
    cw_writer_init_fixed(&w, out, NET_MAX_PACKET);
    cw_writer_u8(&w, (uint8_t)((channel << 6) | type));
    cw_writer_u16(&w, (uint16_t)size);
    if (channel != NET_UNRELIABLE) cw_writer_u16(&w, sequence);
    cw_writer_bytes(&w, payload, size);
    return w.size;
}

// Bytes of reliable messages due to go out: never sent, or sent and unacknowledged
// for a retransmission timeout. Unreliable traffic yields the budget to them.
static bool due(const net_connection *c, const net_reliable_message *m, double now) {
    return !m->acked && (m->sent_at < 0.0 || now - m->sent_at >= c->rto);
}
static double reliable_pending(const net_connection *c, double now) {
    double pending = 0.0;
    for (int k = 0; k < 2; ++k) {
        const net_reliable_sender *s = &c->senders[k];
        size_t in_flight = s->count < NET_RELIABLE_WINDOW ? s->count : NET_RELIABLE_WINDOW;
        for (size_t i = 0; i < in_flight; ++i)
            if (due(c, &s->queue[i], now)) pending += (double)s->queue[i].size;
    }
    return pending;
}

// One message, no larger than a packet's payload. `budgeted` is false for the
// fragments of a message whose whole was budgeted already.
static bool send_one(net_connection *c, uint8_t type, int channel, int priority, const void *payload, size_t size, bool budgeted) {
    if (!is_reliable(channel)) {
        double now = net_seconds();
        if (budgeted && c->rate && refill(c, now) < (double)(c->outgoing_size + size) + reliable_pending(c, now)) {
            ++c->messages_dropped;   // over budget: it would be stale by the time it could go
            return false;
        }
        unsigned char message[NET_MAX_PACKET];
        uint16_t sequence = channel == NET_SEQUENCED ? c->sequenced_next++ : 0;
        size_t encoded = encode_message(message, type, channel, sequence, payload, size);
        append_message(c, message, encoded, now);
        return true;
    }
    net_reliable_sender *s = &c->senders[reliable_index(channel)];
    if (s->count >= NET_RELIABLE_BACKLOG) return false;
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 64;
        s->queue = (net_reliable_message *)realloc(s->queue, s->capacity * sizeof(*s->queue));
    }
    net_reliable_message *m = &s->queue[s->count++];
    memset(m, 0, sizeof(*m));
    m->sequence = s->next_sequence++;
    m->type = type;
    m->priority = (uint8_t)priority;
    m->sent_at = -1.0;
    m->data = (unsigned char *)malloc(NET_MESSAGE_HEADER + 2 + size);
    m->size = encode_message(m->data, type, channel, m->sequence, payload, size);
    return true;
}

bool net_connection_send(net_connection *c, uint8_t type, int channel, const void *payload, size_t size) {
    return net_connection_send_priority(c, type, channel, NET_PRIORITY_NORMAL, payload, size);
}

bool net_connection_send_priority(net_connection *c, uint8_t type, int channel, int priority, const void *payload, size_t size) {
    if (type >= NET_MSG_FRAGMENT || channel < 0 || channel >= NET_CHANNEL_COUNT || priority < 0 || priority >= NET_PRIORITY_COUNT)
        return false;
    if (size <= NET_MAX_PAYLOAD) return send_one(c, type, channel, priority, payload, size, true);
    if (size > NET_MAX_MESSAGE) return false;
    // Fragments: each a message of its own on the channel, with enough in front of
    // it to put the whole back together. The budget is asked once, for all of them;
    // a reliable queue must have room for all of them too.
    size_t count = (size + NET_FRAGMENT_PAYLOAD - 1) / NET_FRAGMENT_PAYLOAD;
    if (!is_reliable(channel)) {
        double now = net_seconds();
        size_t framed = size + count * (NET_MESSAGE_HEADER + 2 + NET_FRAGMENT_HEADER);
        if (c->rate && refill(c, now) < (double)(c->outgoing_size + framed) + reliable_pending(c, now)) {
            ++c->messages_dropped;
            return false;
        }
    } else if (c->senders[reliable_index(channel)].count + count > NET_RELIABLE_BACKLOG) {
        return false;
    }
    uint16_t id = c->next_fragment_id++;
    const unsigned char *bytes = (const unsigned char *)payload;
    for (size_t i = 0; i < count; ++i) {
        unsigned char piece[NET_MAX_PAYLOAD];
        cw_writer w;
        cw_writer_init_fixed(&w, piece, sizeof(piece));
        cw_writer_u16(&w, id);
        cw_writer_u8(&w, (uint8_t)i);
        cw_writer_u8(&w, (uint8_t)count);
        cw_writer_u8(&w, type);
        size_t offset = i * NET_FRAGMENT_PAYLOAD;
        size_t piece_size = size - offset < NET_FRAGMENT_PAYLOAD ? size - offset : NET_FRAGMENT_PAYLOAD;
        cw_writer_bytes(&w, bytes + offset, piece_size);
        send_one(c, NET_MSG_FRAGMENT, channel, priority, piece, w.size, false);
        ++c->fragments_sent;
    }
    return true;
}

// Sends one reliable message if the allowance (the lesser of the budget and the
// pace, less what is in the packet under construction already) covers it; false
// means stop scheduling. Reliable traffic never goes over budget: it has first
// claim on the budget instead, so it is never starved either.
static bool schedule(net_connection *c, net_reliable_message *m, double *allowance, double now) {
    if (c->rate && *allowance < (double)m->size) return false;
    if (m->sent_at >= 0.0) ++c->messages_resent;
    append_message(c, m->data, m->size, now);
    *allowance -= (double)m->size;
    m->sent_at = now;
    return true;
}

static void abandon_reassembly(net_connection *c, net_reassembly *r) {
    free(r->data);
    memset(r, 0, sizeof(*r));
    ++c->reassemblies_abandoned;
}

void net_connection_flush(net_connection *c, double now, bool force) {
    update_congestion(c, now);
    for (int i = 0; i < NET_REASSEMBLY_SLOTS; ++i)
        if (c->reassembly[i].data && now - c->reassembly[i].started > NET_REASSEMBLY_TIMEOUT) abandon_reassembly(c, &c->reassembly[i]);
    // Reliable messages go out as the budget and the pace allow (unreliable traffic
    // has already left them their share), by priority: the ordered channel in
    // sequence, as urgent as its next message; the unordered one high first. An
    // empty packet (an ack, a keepalive) is always allowed.
    double budget = refill(c, now), pace = pace_refill(c, now);
    double allowance = (budget < pace ? budget : pace) - (double)(c->outgoing_size - OUTGOING_START);
    net_reliable_sender *ordered = &c->senders[1], *unordered = &c->senders[0];
    size_t ordered_in_flight = ordered->count < NET_RELIABLE_WINDOW ? ordered->count : NET_RELIABLE_WINDOW;
    size_t unordered_in_flight = unordered->count < NET_RELIABLE_WINDOW ? unordered->count : NET_RELIABLE_WINDOW;
    int ordered_priority = -1;
    for (size_t i = 0; i < ordered_in_flight && ordered_priority < 0; ++i)
        if (due(c, &ordered->queue[i], now)) ordered_priority = ordered->queue[i].priority;
    bool room = true;
    for (int priority = 0; priority < NET_PRIORITY_COUNT && room; ++priority) {
        if (ordered_priority == priority)
            for (size_t i = 0; i < ordered_in_flight && room; ++i)
                if (due(c, &ordered->queue[i], now)) room = schedule(c, &ordered->queue[i], &allowance, now);
        for (size_t i = 0; i < unordered_in_flight && room; ++i) {
            net_reliable_message *m = &unordered->queue[i];
            if (m->priority == priority && due(c, m, now)) room = schedule(c, m, &allowance, now);
        }
    }
    // An ack owed goes at once, in its own packet if need be: the peer would
    // otherwise resend, which costs far more than the empty packet.
    if (c->outgoing_size > OUTGOING_START || force || c->ack_pending) send_outgoing(c, now);
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
// Marks what the peer says it has and pops the acknowledged head of the queue.
// Only messages actually sent can be acknowledged: an ack ahead of those is
// corrupt or stale and is ignored.
static void acknowledge(net_reliable_sender *s, uint16_t latest, uint64_t bits) {
    for (size_t i = 0; i < s->count && i < NET_RELIABLE_WINDOW; ++i) {
        net_reliable_message *m = &s->queue[i];
        if (m->acked || m->sent_at < 0.0) continue;
        int behind = net_sequence_compare(latest, m->sequence);
        if (behind == 0 || (behind > 0 && behind <= 64 && (bits >> (behind - 1) & 1))) m->acked = true;
    }
    size_t popped = 0;
    while (popped < s->count && s->queue[popped].acked) free(s->queue[popped++].data);
    if (popped) {
        memmove(s->queue, s->queue + popped, (s->count - popped) * sizeof(*s->queue));
        s->count -= popped;
    }
}

// Records a sequence as received; false when it already was (or is too old to tell).
static bool note_received(net_ack_state *a, uint16_t sequence) {
    if (!a->any) {
        a->any = true;
        a->latest = sequence;
        a->bits = 0;
        return true;
    }
    int ahead = net_sequence_compare(sequence, a->latest);
    if (ahead > 0) {
        a->bits = ahead >= 64 ? 0 : (a->bits << ahead) | ((uint64_t)1 << (ahead - 1));
        a->latest = sequence;
        return true;
    }
    if (ahead == 0) return false;
    int behind = -ahead;
    if (behind > 64) return false;
    uint64_t bit = (uint64_t)1 << (behind - 1);
    if (a->bits & bit) return false;
    a->bits |= bit;
    return true;
}

// A piece of a larger message: kept until the rest has come, then delivered whole.
// Pieces are of a fixed size but the last, so each knows its place.
static void reassemble(net_connection *c, int channel, const unsigned char *payload, size_t size, double now) {
    cw_reader r = cw_reader_make(payload, size);
    uint16_t id = 0;
    uint8_t index = 0, count = 0, type = 0;
    cw_reader_u16(&r, &id);
    cw_reader_u8(&r, &index);
    cw_reader_u8(&r, &count);
    cw_reader_u8(&r, &type);
    size_t piece = cw_reader_remaining(&r);
    if (!cw_reader_ok(&r) || count < 2 || count > NET_MAX_FRAGMENTS || index >= count || type >= NET_MSG_FRAGMENT
        || piece == 0 || piece > NET_FRAGMENT_PAYLOAD || (index + 1 < count && piece != NET_FRAGMENT_PAYLOAD))
        return;
    net_reassembly *slot = NULL;
    for (int i = 0; i < NET_REASSEMBLY_SLOTS && !slot; ++i)
        if (c->reassembly[i].data && c->reassembly[i].id == id && c->reassembly[i].channel == channel) slot = &c->reassembly[i];
    if (!slot) {
        // A free slot, else the one that has waited longest gives way: an
        // unreliable one first, since a reliable one is sure to be completed.
        for (int i = 0; i < NET_REASSEMBLY_SLOTS && !slot; ++i)
            if (!c->reassembly[i].data) slot = &c->reassembly[i];
        if (!slot) {
            for (int i = 0; i < NET_REASSEMBLY_SLOTS; ++i) {
                net_reassembly *m = &c->reassembly[i];
                bool m_reliable = is_reliable(m->channel), slot_reliable = slot && is_reliable(slot->channel);
                if (!slot || (slot_reliable && !m_reliable) || (slot_reliable == m_reliable && m->started < slot->started)) slot = m;
            }
            abandon_reassembly(c, slot);
        }
        slot->data = (unsigned char *)malloc((size_t)count * NET_FRAGMENT_PAYLOAD);
        slot->id = id;
        slot->count = count;
        slot->type = type;
        slot->channel = (uint8_t)channel;
        slot->started = now;
    }
    if (slot->count != count || slot->type != type || (slot->have >> index & 1)) return;   // not of a piece with it, or again
    memcpy(slot->data + (size_t)index * NET_FRAGMENT_PAYLOAD, cw_reader_view(&r, piece), piece);
    slot->have |= (uint64_t)1 << index;
    ++slot->received;
    ++c->fragments_received;
    if (index + 1 == count) slot->size = (size_t)index * NET_FRAGMENT_PAYLOAD + piece;
    if (slot->received < count) return;
    unsigned char *data = slot->data;
    size_t whole = slot->size;
    uint8_t whole_type = slot->type;
    memset(slot, 0, sizeof(*slot));
    ++c->messages_reassembled;
    if (c->on_message) c->on_message(c->user, c, whole_type, cw_reader_make(data, whole));
    free(data);
}

static void deliver(net_connection *c, int channel, uint8_t type, const unsigned char *payload, size_t size, double now) {
    if (type == NET_MSG_FRAGMENT) reassemble(c, channel, payload, size, now);
    else if (c->on_message) c->on_message(c->user, c, type, cw_reader_make(payload, size));
}

// Accepts a counter once: larger than any seen, or within the window below the
// largest and not yet marked.
static bool replay_check(net_connection *c, uint64_t counter) {
    if (counter == 0) return false;
    if (counter > c->receive_highest) {
        uint64_t shift = counter - c->receive_highest;
        c->receive_window = shift >= 64 ? 0 : c->receive_window << shift;
        c->receive_window |= 1;
        c->receive_highest = counter;
        return true;
    }
    uint64_t behind = c->receive_highest - counter;
    if (behind >= 64) return false;
    uint64_t bit = (uint64_t)1 << behind;
    if (c->receive_window & bit) return false;
    c->receive_window |= bit;
    return true;
}

// One message's framing, as both the validation pass and the delivery pass read it.
typedef struct {
    uint8_t header;
    uint16_t sequence;
    const unsigned char *payload;
    size_t size;
} framed_message;
static bool read_message(cw_reader *r, framed_message *m) {
    uint16_t length = 0;
    cw_reader_u8(r, &m->header);
    cw_reader_u16(r, &length);
    m->sequence = 0;
    if (channel_of(m->header) != NET_UNRELIABLE) cw_reader_u16(r, &m->sequence);
    if (!cw_reader_ok(r) || length > NET_MAX_PAYLOAD) return false;
    m->payload = (const unsigned char *)cw_reader_view(r, length);
    m->size = length;
    return m->payload != NULL;
}

bool net_connection_receive(net_connection *c, const void *packet, size_t size, double now) {
    cw_reader r = cw_reader_make(packet, size);
    uint32_t session = 0;
    if (size > NET_MAX_PACKET || !read_header(&r, &session) || session != c->session) {
        ++c->packets_rejected;
        return false;
    }
    if (!inbound_allows(c, size, now)) {
        ++c->packets_throttled;
        return false;
    }
    unsigned char plain[NET_MAX_PACKET];
    if (c->encrypted) {
        uint64_t counter = 0;
        cw_reader_u64(&r, &counter);
        size_t cipher_size = cw_reader_remaining(&r);
        if (!cw_reader_ok(&r) || cipher_size < NET_ACKS_SIZE + CRYPTO_TAG_SIZE) {
            ++c->packets_malformed;
            return false;
        }
        cipher_size -= CRYPTO_TAG_SIZE;
        const unsigned char *cipher = (const unsigned char *)cw_reader_view(&r, cipher_size);
        const uint8_t *tag = (const uint8_t *)cw_reader_view(&r, CRYPTO_TAG_SIZE);
        uint8_t nonce[CRYPTO_NONCE_SIZE];
        counter_nonce(counter, nonce);
        if (!crypto_aead_decrypt(c->receive_key, nonce, packet, NET_PACKET_HEADER + 8, cipher, cipher_size, tag, plain)) {
            ++c->packets_forged;
            return false;
        }
        // Checked after the tag: a forged counter must not move the window.
        if (!replay_check(c, counter)) {
            ++c->packets_replayed;
            return false;
        }
        r = cw_reader_make(plain, cipher_size);
    }
    uint16_t packet_sequence = 0, packet_ack = 0, held = 0;
    uint32_t packet_bits = 0;
    cw_reader_u16(&r, &packet_sequence);
    cw_reader_u16(&r, &packet_ack);
    cw_reader_u32(&r, &packet_bits);
    cw_reader_u16(&r, &held);
    uint16_t ack_latest[2];
    uint64_t ack_bits[2];
    for (int k = 0; k < 2; ++k) {
        cw_reader_u16(&r, &ack_latest[k]);
        cw_reader_u64(&r, &ack_bits[k]);
    }
    if (!cw_reader_ok(&r)) {
        ++c->packets_malformed;
        return false;
    }
    // Framing first, all of it, so that a packet is delivered whole or not at all.
    cw_reader check = r;
    while (cw_reader_remaining(&check)) {
        framed_message m;
        if (!read_message(&check, &m)) {
            ++c->packets_malformed;
            return false;
        }
    }
    c->last_received = now;
    ++c->packets_received;
    c->bytes_received += size;
    note_packet_received(c, packet_sequence, now);
    process_packet_acks(c, packet_ack, packet_bits, (double)held / 16000.0, now);
    for (int k = 0; k < 2; ++k) acknowledge(&c->senders[k], ack_latest[k], ack_bits[k]);

    while (cw_reader_remaining(&r)) {
        framed_message m;
        read_message(&r, &m);
        int channel = channel_of(m.header);
        uint8_t type = type_of(m.header);
        switch (channel) {
        case NET_UNRELIABLE:
            deliver(c, channel, type, m.payload, m.size, now);
            break;
        case NET_SEQUENCED:
            if (c->sequenced_any && net_sequence_compare(m.sequence, c->sequenced_last) <= 0) {
                ++c->messages_stale;
                break;
            }
            c->sequenced_any = true;
            c->sequenced_last = m.sequence;
            deliver(c, channel, type, m.payload, m.size, now);
            break;
        case NET_RELIABLE:
            c->ack_pending = true;
            if (!note_received(&c->received[0], m.sequence)) {
                ++c->messages_duplicate;
                break;
            }
            deliver(c, channel, type, m.payload, m.size, now);
            break;
        case NET_RELIABLE_ORDERED: {
            c->ack_pending = true;
            int distance = net_sequence_compare(m.sequence, c->expected);
            if (distance < 0) {   // delivered already
                ++c->messages_duplicate;
                break;
            }
            if (distance >= NET_RELIABLE_WINDOW) break;   // beyond the window: the peer is ahead of itself
            if (!note_received(&c->received[1], m.sequence)) {
                ++c->messages_duplicate;
                break;
            }
            net_reliable_message *slot = &c->slots[m.sequence % NET_RELIABLE_WINDOW];
            slot->data = (unsigned char *)malloc(m.size ? m.size : 1);
            memcpy(slot->data, m.payload, m.size);
            slot->size = m.size;
            slot->type = type;
            // Deliver everything that is now contiguous, in order.
            for (;;) {
                net_reliable_message *next = &c->slots[c->expected % NET_RELIABLE_WINDOW];
                if (!next->data) break;
                unsigned char *data = next->data;
                size_t data_size = next->size;
                uint8_t next_type = next->type;
                next->data = NULL;
                next->size = 0;
                ++c->expected;
                deliver(c, channel, next_type, data, data_size, now);
                free(data);
            }
            break;
        }
<<<<<<< Updated upstream
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
        slot->type = (uint8_t)(type & ~NET_RELIABLE_FLAG);

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
=======
>>>>>>> Stashed changes
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rate limiting by address
// ---------------------------------------------------------------------------
void net_limiter_init(net_limiter *l, double per_second, double burst) {
    memset(l, 0, sizeof(*l));
    l->rate = per_second;
    l->burst = burst;
}

bool net_limiter_allow(net_limiter *l, net_address from, double now) {
    if (l->rate <= 0.0) return true;
    uint32_t h = from.ip * 2654435761u;
    net_limiter_slot *slot = &l->slots[(h >> 16) % NET_LIMITER_SLOTS];
    if (slot->ip != from.ip) {   // a new address, or one that takes the slot over
        slot->ip = from.ip;
        slot->tokens = l->burst;
        slot->at = now;
    }
    double elapsed = now - slot->at;
    if (elapsed > 0.0) slot->tokens += elapsed * l->rate;
    if (slot->tokens > l->burst) slot->tokens = l->burst;
    slot->at = now;
    if (slot->tokens < 1.0) {
        ++l->refused;
        return false;
    }
    slot->tokens -= 1.0;
    return true;
}

// ---------------------------------------------------------------------------
// Varints
// ---------------------------------------------------------------------------
void net_write_uvarint(cw_writer *w, uint64_t value) {
    while (value >= 0x80) {
        cw_writer_u8(w, (uint8_t)(value | 0x80));
        value >>= 7;
    }
    cw_writer_u8(w, (uint8_t)value);
}

bool net_read_uvarint(cw_reader *r, uint64_t *value) {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        uint8_t byte = 0;
        if (!cw_reader_u8(r, &byte)) return false;
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return true;
        }
    }
    r->error = true;
    return false;
}

void net_write_svarint(cw_writer *w, int64_t value) {
    net_write_uvarint(w, ((uint64_t)value << 1) ^ (uint64_t)(value >> 63));
}

bool net_read_svarint(cw_reader *r, int64_t *value) {
    uint64_t raw;
    if (!net_read_uvarint(r, &raw)) return false;
    *value = (int64_t)(raw >> 1) ^ -(int64_t)(raw & 1);
    return true;
}
