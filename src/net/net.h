// The transport: UDP sockets and connections. Knows nothing about the game; the
// protocol on top is in protocol.h.
//
// A connection is one peer on one socket. It carries four channels over the same
// datagrams, chosen per message:
//   - NET_UNRELIABLE: sent once, delivered if it arrives, in whatever order. For
//     things with no state (keepalives, the handshake's unconnected messages).
//   - NET_SEQUENCED: sent once, delivered only if newer than the last delivered;
//     an older one arriving late is dropped. For state that is superseded by the
//     next (snapshots, inputs).
//   - NET_RELIABLE: delivered exactly once, as soon as it arrives, retransmitted
//     until the peer acknowledges it. For things that must arrive but do not
//     depend on each other (a character's gear, which carries its own version).
//   - NET_RELIABLE_ORDERED: delivered exactly once and in order, held back until
//     everything before it has arrived. For anything that must arrive in sequence
//     (the handshake, spawns and despawns, updates, commands).
// Several messages share one packet, and a packet is never larger than
// NET_MAX_PACKET so it crosses the internet without IP fragmentation. A message
// larger than a packet is split into fragments (each a message of its own on the
// same channel) and put together again on the other side, up to NET_MAX_MESSAGE.
//
// Packet layout (little-endian, see cw_writer):
//   u32 magic ('TPN' + NET_PROTOCOL_VERSION)   u32 session
//   session 0 (the handshake, in the clear):  acks, messages
//   otherwise (encrypted):                    u64 counter, ChaCha20-Poly1305 of
//                                             (acks, messages), 16-byte tag
//   acks: u16 this packet's sequence; u16 the latest packet sequence received and
//         u32 bitfield of the 32 before it, u16 how long ago that latest one came
//         (1/16 ms); then for each reliable channel u16 latest message sequence
//         received and u64 bitfield of the 64 before it
//   a message: u8 channel (bits 7..6) and type (bits 5..0), u16 payload size,
//              [u16 sequence, unless NET_UNRELIABLE], payload
//   a fragment: a message of type NET_MSG_FRAGMENT whose payload is u16 message id,
//               u8 index, u8 count, u8 the real type, then the piece
// Every channel numbers its messages in its own 16-bit space; comparisons are
// modular, so a connection may carry any number of messages as long as fewer than
// NET_RELIABLE_WINDOW are in flight per reliable channel (the rest queue behind).
//
// What the packet acks give, besides the reliability: the round trip (the time
// from a packet's sending to the first ack naming it, less the time the peer held
// it: smoothed, with its variance, the way TCP does it; it sets the retransmission
// timeout), and the loss (a packet the peer's bitfield has passed by without
// naming is lost; a moving average of that is the loss rate). Both drive the
// congestion control: a connection sends at a rate between a floor and its cap,
// cut by a quarter when the round trip climbs well above its floor or the loss
// rate passes a few percent, and raised by a tenth of the cap for every second
// that is calm. What leaves is paced (bulk goes out smoothly at that rate rather
// than in bursts) and scheduled by priority: what is in the packet under
// construction already (snapshots, inputs), then reliable messages high before
// normal before low, the ordered channel in sequence, its head's priority being
// its own.
//
// What a packet must pass to be heard (net_connection_receive): the header (magic,
// version, this session), the inbound rate limit (packets and bytes per second,
// before any decryption, so a flood costs a comparison), the authentication tag
// under this session's keys, a counter larger than any seen or within the 64 below
// it and unused (no replay), and exact framing: every message header and payload
// within the packet and nothing left over. A packet that fails any of these is
// dropped whole: nothing in it is delivered, no ack in it is believed, and a
// counter tells which check it failed. The keys come from the handshake (crypto.h,
// protocol.h); until then, on session 0, packets are in the clear and carry only
// what the handshake needs.
#ifndef TP_NET_H
#define TP_NET_H

#include "../ecs/clockwork.h"
#include "crypto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_PROTOCOL_VERSION 18
#define NET_MAX_PACKET 1200          // bytes, safely under every common MTU
#define NET_PACKET_HEADER 8          // magic, session: what every packet starts with, in the clear
#define NET_ACKS_SIZE (2 + 2 + 4 + 2 + 2 * (2 + 8))   // the packet acks, then one (latest, bitfield) per reliable channel
#define NET_PACKET_OVERHEAD (NET_PACKET_HEADER + 8 + NET_ACKS_SIZE + CRYPTO_TAG_SIZE)   // the most a packet adds to its messages
#define NET_MESSAGE_HEADER 3         // channel and type, size
#define NET_MAX_PAYLOAD (NET_MAX_PACKET - NET_PACKET_OVERHEAD - NET_MESSAGE_HEADER - 2)   // of one unfragmented message
#define NET_FRAGMENT_HEADER 5        // id, index, count, type
#define NET_FRAGMENT_PAYLOAD (NET_MAX_PAYLOAD - NET_FRAGMENT_HEADER)
#define NET_MAX_FRAGMENTS 64
#define NET_MAX_MESSAGE (NET_MAX_FRAGMENTS * NET_FRAGMENT_PAYLOAD)   // about 70 KB
#define NET_REASSEMBLY_SLOTS 8       // messages under reassembly at once per connection
#define NET_REASSEMBLY_TIMEOUT 3.0   // seconds a partial message waits for its rest
#define NET_MESSAGE_TYPES 64         // message types fit in six bits...
#define NET_MSG_FRAGMENT 63          // ...the last of which is the transport's own
#define NET_RELIABLE_WINDOW 64       // reliable messages in flight per channel per direction
#define NET_RELIABLE_BACKLOG 4096    // reliable messages a connection will queue before refusing more
#define NET_RTO_DEFAULT 0.15         // retransmission timeout before the round trip is known
#define NET_RTO_MIN 0.05             // ...and its bounds once it is
#define NET_RTO_MAX 1.0
#define NET_PACKET_HISTORY 256       // packets remembered for round trip and loss accounting
#define NET_PACING_BURST (2 * NET_MAX_PACKET)   // bytes that may leave at once beyond the paced rate
#define NET_CONGESTION_FLOOR 0.125   // the rate is never cut below this fraction of the cap
#define NET_CONGESTION_LOSS 0.03     // loss rate past which the rate is cut
#define NET_CONGESTION_DELAY 0.05    // seconds of round trip above its floor past which the rate is cut
#define NET_CONGESTION_CALM 1.0      // seconds without trouble before the rate is raised
#define NET_DEFAULT_PORT 27500
#define NET_DIRECTORY_PORT 27400

// ---------------------------------------------------------------------------
// Sockets: IPv4 UDP, non-blocking. net_init once per process (Winsock needs it).
// ---------------------------------------------------------------------------
struct net_address {
    uint32_t ip;                 // host byte order; 0x7f000001 is 127.0.0.1
    uint16_t port;
};
struct net_socket;

bool net_init(void);
void net_shutdown(void);
double net_seconds(void);                   // monotonic, for intervals
double net_wall_seconds(void);              // since the Unix epoch, sub-millisecond: the universe clock
void net_sleep(double seconds);

net_socket *net_socket_open(uint16_t port);         // 0 for any free port; NULL on failure
void net_socket_close(net_socket *socket);
uint16_t net_socket_port(const net_socket *socket);
bool net_socket_send(net_socket *socket, net_address to, const void *data, size_t size);
// Bytes received, 0 when nothing is waiting, -1 on a fatal socket error.
int net_socket_recv(net_socket *socket, net_address *from, void *data, size_t capacity);
// Testing aids: drop this fraction of outgoing packets (0 sends everything), and
// hold back this fraction so that they leave after the packet that follows them
// (reordering).
void net_socket_set_loss(net_socket *socket, float fraction);
void net_socket_set_reorder(net_socket *socket, float fraction);

bool net_address_equal(net_address a, net_address b);
// "host", "host:port", "1.2.3.4:port"; names are resolved. Missing port: default_port.
bool net_address_parse(const char *text, uint16_t default_port, net_address *out);
void net_address_format(net_address address, char *out, size_t capacity);
net_address net_address_make(uint32_t ip, uint16_t port);

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------
struct net_connection;
// Called for every delivered message. `payload` is valid during the call only.
typedef void (*net_message_fn)(void *user, net_connection *c, uint8_t type, cw_reader payload);

enum net_channel { NET_UNRELIABLE, NET_SEQUENCED, NET_RELIABLE, NET_RELIABLE_ORDERED, NET_CHANNEL_COUNT };
// What goes first when the budget is short. The ordered channel keeps its order
// whatever the priorities: its next message is as urgent as it is marked.
enum net_priority { NET_PRIORITY_HIGH, NET_PRIORITY_NORMAL, NET_PRIORITY_LOW, NET_PRIORITY_COUNT };

struct net_reliable_message {
    unsigned char *data;         // sending: the whole encoded message; receiving: the payload
    size_t size;
    double sent_at;              // sending: last transmission, negative before the first
    uint16_t sequence;
    uint8_t type;                // receiving: the message type waiting in this slot
    uint8_t priority;
    bool acked;                  // sending: the peer has it; waits only for the ones before it to go
};
// A packet sent, for the round trip and the loss.
struct net_packet_record {
    uint16_t sequence;
    double sent_at;
    bool sent, acked;
};
// A message arriving in pieces.
struct net_reassembly {
    unsigned char *data;
    uint16_t id;
    uint8_t count, received, type, channel;
    uint64_t have;               // bit per fragment
    size_t size;                 // bytes so far
    double started;
};
// One reliable channel's send side: a queue in sequence order. The first
// NET_RELIABLE_WINDOW are in flight; the rest wait for the head to be acknowledged.
struct net_reliable_sender {
    net_reliable_message *queue;
    size_t count, capacity;
    uint16_t next_sequence;
};
// What has been received on a reliable channel: the latest sequence and the 64
// before it. Sent as the ack, and what refuses a duplicate.
struct net_ack_state {
    uint16_t latest;
    uint64_t bits;
    bool any;
};
struct net_connection {
    net_socket *socket;
    net_address peer;
    uint32_t session;            // stamped on every packet; the peer ignores mismatches
    net_message_fn on_message;
    void *user;

    // Outgoing messages under construction (the packet framing is added on send);
    // unreliable and sequenced messages go straight in.
    unsigned char outgoing[NET_MAX_PACKET];
    size_t outgoing_size;

    // Encryption, once the handshake has produced the keys. Until then (session 0)
    // packets are in the clear.
    bool encrypted;
    uint8_t send_key[CRYPTO_KEY_SIZE], receive_key[CRYPTO_KEY_SIZE];
    uint64_t send_counter;       // the nonce of the next packet out
    uint64_t receive_highest;    // the largest counter seen, with a window of the 64 below it
    uint64_t receive_window;

    net_reliable_sender senders[2];          // NET_RELIABLE, NET_RELIABLE_ORDERED
    net_ack_state received[2];               // what to acknowledge on each, and the duplicate check
    bool ack_pending;                        // a reliable message came in since the last packet out
    net_reliable_message slots[NET_RELIABLE_WINDOW];   // ordered arrivals waiting for the gap before them
    uint16_t expected;                       // the next ordered sequence to deliver
    uint16_t sequenced_next, sequenced_last; // the sequenced channel: next to send, last delivered
    bool sequenced_any;
    uint16_t next_fragment_id;
    net_reassembly reassembly[NET_REASSEMBLY_SLOTS];

    // Packet accounting: every packet out is remembered until the peer has named
    // it (the round trip) or passed it by (lost).
    net_packet_record history[NET_PACKET_HISTORY];
    uint16_t packet_sequence;                // of the next packet out
    uint16_t packet_ack_latest;              // the latest packet sequence received from the peer...
    uint32_t packet_ack_bits;                // ...and the 32 before it
    bool packet_any;
    double packet_latest_at;                 // when that latest one came, for the delay we report
    uint16_t loss_edge;                      // packets up to here have been judged sent or lost
    bool loss_edge_set;
    double rtt, rtt_variance, rtt_min;       // smoothed round trip (seconds; 0 until measured), its variance, its floor
    double rtt_min_at;                       // when the floor was last lowered; it is let go of after a while
    double rto;                              // retransmission timeout, from the round trip
    double loss;                             // moving average of packets lost (0..1)
    uint32_t packets_lost, rtt_samples;

    // Congestion control: `congestion_rate` is what the budget and the pacer run
    // at, between NET_CONGESTION_FLOOR of `rate` and `rate` itself.
    double congestion_rate;
    double congestion_last_cut, congestion_calm_since;
    uint32_t congestion_cuts, congestion_raises;
    // Pacing: a second bucket at the same rate with a burst of two packets, so that
    // bulk leaves smoothly rather than the budget's quarter second at once.
    double pace_tokens, pace_at;

    double last_received, last_sent;
    uint64_t bytes_sent, bytes_received;
    uint32_t packets_sent, packets_received;
    uint32_t fragments_sent, fragments_received, messages_reassembled, reassemblies_abandoned;
    // Why packets were dropped, in the order they are checked.
    uint32_t packets_rejected;   // not ours: magic, version or session
    uint32_t packets_throttled;  // over the inbound rate limit
    uint32_t packets_forged;     // the authentication tag did not match
    uint32_t packets_replayed;   // a counter already seen
    uint32_t packets_malformed;  // framing that did not add up
    uint32_t messages_resent, messages_stale, messages_duplicate;

    // Outbound rate limit: a token bucket refilled at `rate` bytes per second,
    // holding up to a quarter second's worth. Unreliable and sequenced messages
    // that do not fit are dropped, and reliable traffic due to go has first claim
    // on the budget, so that a stream of records is never starved by snapshots.
    // 0 is unlimited.
    uint32_t rate;
    double tokens, tokens_at;
    uint32_t messages_dropped;
    // Inbound rate limit: packets and bytes per second this peer may send us,
    // with a second's burst; the rest are dropped unread. 0 is unlimited.
    uint32_t inbound_packets, inbound_bytes;
    double inbound_packet_tokens, inbound_byte_tokens, inbound_at;
};

void net_connection_init(net_connection *c, net_socket *socket, net_address peer, uint32_t session,
                         net_message_fn on_message, void *user);
void net_connection_destroy(net_connection *c);
// From here on packets are encrypted with `send_key` and expected under `receive_key`.
void net_connection_set_keys(net_connection *c, const uint8_t send_key[CRYPTO_KEY_SIZE],
                             const uint8_t receive_key[CRYPTO_KEY_SIZE]);
// Queues a message on a channel, at normal priority. Unreliable and sequenced ones
// may leave in a packet straight away when the current one is full; reliable ones
// leave on the next flush, high priority first. A message larger than a packet is
// fragmented (up to NET_MAX_MESSAGE). False when the type is not ours to use, the
// payload is too large, the outbound budget has no room for an unreliable message,
// or a reliable queue is absurdly long.
bool net_connection_send(net_connection *c, uint8_t type, int channel, const void *payload, size_t size);
bool net_connection_send_priority(net_connection *c, uint8_t type, int channel, int priority, const void *payload, size_t size);
// Hands over a packet that arrived from this connection's peer. Validates it (see
// the top of this file), delivers what is deliverable, and records acks. Returns
// false for a packet that was dropped, with the reason counted.
bool net_connection_receive(net_connection *c, const void *packet, size_t size, double now);
// Sends what is pending: the packet under construction, retransmissions, and an
// empty packet when `force` is set (to carry an ack or keep the peer alive).
void net_connection_flush(net_connection *c, double now, bool force);
// Reliable messages queued (on either channel) but not yet acknowledged.
size_t net_connection_unacked(const net_connection *c);
// Caps what this connection sends, in bytes per second (0: no cap). The congestion
// control works below the cap; a new cap keeps the current rate where it can.
void net_connection_set_rate(net_connection *c, uint32_t bytes_per_second);
// What the connection is sending at right now: the cap, or less under congestion.
uint32_t net_connection_rate(const net_connection *c);
// Bytes the rate limit would let out right now.
size_t net_connection_allowance(const net_connection *c, double now);
// Caps what this connection will read from its peer (0: no cap).
void net_connection_set_inbound_limit(net_connection *c, uint32_t packets_per_second, uint32_t bytes_per_second);

// ---------------------------------------------------------------------------
// Rate limiting by address, for traffic that has no connection yet (a server's
// handshakes, a directory's queries): a fixed table of token buckets indexed by a
// hash of the address (the ip only: ports are free to forge). An address that
// collides with another's slot takes it over, so a flood from one address can
// at worst throttle a few others, never fill memory. `burst` is the bucket's size.
// ---------------------------------------------------------------------------
#define NET_LIMITER_SLOTS 4096
struct net_limiter_slot {
    uint32_t ip;
    double tokens, at;
};
struct net_limiter {
    net_limiter_slot slots[NET_LIMITER_SLOTS];
    double rate, burst;
    uint32_t refused;
};
void net_limiter_init(net_limiter *l, double per_second, double burst);
bool net_limiter_allow(net_limiter *l, net_address from, double now);

// Reads the session field of a raw packet without a connection, for demultiplexing
// on a listening socket. False when the header is not ours.
bool net_packet_peek(const void *packet, size_t size, uint32_t *session);
// Modular 16-bit comparison: positive when a is newer than b.
int net_sequence_compare(uint16_t a, uint16_t b);

// Variable-length integers (LEB128): small values in one byte, up to ten for the
// largest. Signed values are zigzag-mapped first so small negatives stay small.
void net_write_uvarint(cw_writer *w, uint64_t value);
bool net_read_uvarint(cw_reader *r, uint64_t *value);
void net_write_svarint(cw_writer *w, int64_t value);
bool net_read_svarint(cw_reader *r, int64_t *value);

#endif
