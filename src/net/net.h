// The transport: UDP sockets and connections. Knows nothing about the game; the
// protocol on top is in protocol.h.
//
// A connection is one peer on one socket. It carries two kinds of message over the
// same datagrams:
//   - unreliable: sent once in the next packet, lost if the packet is lost. For
//     state that is superseded anyway (snapshots, inputs).
//   - reliable: delivered exactly once and in order, retransmitted until the peer
//     acknowledges it. For anything that must arrive (handshake, spawns, despawns).
// Several messages share one packet, and a packet is never larger than
// NET_MAX_PACKET so it crosses the internet without IP fragmentation.
//
// Packet layout (little-endian, see cw_writer):
//   u32 magic ('TPN' + NET_PROTOCOL_VERSION)   u32 session   u16 reliable ack
//   then messages: u8 type (bit 7 set: reliable), u16 payload size,
//                  [u16 reliable sequence], payload
// The reliable ack is cumulative: the last sequence this side delivered in order.
// Reliable sequences wrap at 16 bits; comparisons are modular, so a connection may
// carry any number of messages as long as fewer than 32768 are ever in flight.
#ifndef TP_NET_H
#define TP_NET_H

#include "../ecs/clockwork.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_PROTOCOL_VERSION 3
#define NET_MAX_PACKET 1200          // bytes, safely under every common MTU
#define NET_PACKET_HEADER 10         // magic, session, ack
#define NET_MESSAGE_HEADER 3         // type, size
#define NET_MAX_PAYLOAD (NET_MAX_PACKET - NET_PACKET_HEADER - NET_MESSAGE_HEADER - 2)
#define NET_RELIABLE_WINDOW 64       // reliable messages in flight per direction
#define NET_RELIABLE_RTO 0.15        // seconds before an unacknowledged message is resent
#define NET_DEFAULT_PORT 27500

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
// Testing aid: drop this fraction of outgoing packets (0 sends everything).
void net_socket_set_loss(net_socket *socket, float fraction);

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

struct net_reliable_message {
    unsigned char *data;         // sending: the whole encoded message; receiving: the payload
    size_t size;
    double sent_at;              // sending: last transmission, negative before the first
    uint8_t type;                // receiving: the message type waiting in this slot
};
struct net_connection {
    net_socket *socket;
    net_address peer;
    uint32_t session;            // stamped on every packet; the peer ignores mismatches
    net_message_fn on_message;
    void *user;

    // Outgoing packet under construction; unreliable messages go straight in.
    unsigned char outgoing[NET_MAX_PACKET];
    size_t outgoing_size;

    // Reliable send queue: message i has sequence send_base + i. The first
    // NET_RELIABLE_WINDOW are in flight; the rest wait for acks to make room.
    net_reliable_message *send_queue;
    size_t send_count, send_capacity;
    uint16_t send_base;
    // Reliable receive: out-of-order arrivals wait in slots until the gap closes.
    net_reliable_message receive_slots[NET_RELIABLE_WINDOW];
    uint16_t receive_expected;

    double last_received, last_sent;
    uint32_t packets_sent, packets_received, packets_rejected, messages_resent;
    uint64_t bytes_sent, bytes_received;
};

void net_connection_init(net_connection *c, net_socket *socket, net_address peer, uint32_t session,
                         net_message_fn on_message, void *user);
void net_connection_destroy(net_connection *c);
// Queues a message. Unreliable ones may leave in a packet straight away when the
// current one is full; reliable ones leave on the next flush. False when the
// payload is too large or (reliable) the queue is absurdly long.
bool net_connection_send(net_connection *c, uint8_t type, bool reliable, const void *payload, size_t size);
// Hands over a packet that arrived from this connection's peer. Validates the
// header, delivers what is deliverable, and records acks. Returns false for a
// packet that was not for this connection (bad magic, version or session).
bool net_connection_receive(net_connection *c, const void *packet, size_t size, double now);
// Sends what is pending: the packet under construction, retransmissions, and an
// empty packet when `force` is set (to carry an ack or keep the peer alive).
void net_connection_flush(net_connection *c, double now, bool force);
// Reliable messages queued but not yet acknowledged.
size_t net_connection_unacked(const net_connection *c);

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
