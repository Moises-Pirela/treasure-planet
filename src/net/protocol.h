// The game's wire protocol on top of net.h: which messages exist and how the game's
// components and state are encoded. Both the server and the client include this and
// nothing else knows the byte layout.
//
// Every field is encoded explicitly, little-endian, through cw_writer and cw_reader,
// so a Windows client and a Linux server agree byte for byte and a change to a struct
// never silently changes the wire. Bump NET_PROTOCOL_VERSION when a message changes.
//
// Joining:
//   client  MSG_CONNECT ---------------------------> server     (unreliable, repeated)
//   client <--------------------------- MSG_WELCOME  server     (reliable: session, your entity)
//   client <--- MSG_SPAWN x N, MSG_BASELINE_END ---  server     (reliable: the whole world)
// Playing, every tick or snapshot:
//   client  MSG_INPUT -----------------------------> server     (unreliable, last few inputs)
//   client <------------------------- MSG_SNAPSHOT  server     (unreliable: who is where)
//   client <------------- MSG_SPAWN / MSG_DESPAWN   server     (reliable: joins, leaves)
//   client <--------------------- MSG_SHIP_STATE    server     (reliable: departures, arrivals)
// Either side may send MSG_DISCONNECT; silence for longer than the timeout is the same.
//
// Sharding: a zone server owns one region of the universe. MSG_REDIRECT tells a
// client which server owns the region it is entering; entity ids are 64-bit so each
// shard can be given its own range and a client can hold entities from several at once.
#ifndef TP_PROTOCOL_H
#define TP_PROTOCOL_H

#include "../game/game.h"
#include "net.h"

#define NET_SNAPSHOT_HZ 20
#define NET_INPUT_REDUNDANCY 3       // inputs per MSG_INPUT: the newest and the ones before it
#define NET_MAX_SNAPSHOT_ENTRIES 40  // per MSG_SNAPSHOT; larger sets span several messages
#define NET_TIMEOUT 5.0              // seconds of silence before a peer is gone
#define NET_KEEPALIVE 0.5            // seconds of nothing to say before saying so

enum net_message_type {
    MSG_CONNECT = 1,         // client -> server: u32 protocol version, u32 nonce
    MSG_WELCOME,             // server -> client: net_welcome
    MSG_REJECT,              // server -> client: u8 net_reject_reason
    MSG_SPAWN,               // server -> client: u16 count, then entity records (net_entity_write)
    MSG_DESPAWN,             // server -> client: u16 count, i64 ids
    MSG_BASELINE_END,        // server -> client: the world so far is complete
    MSG_SNAPSHOT,            // server -> client: net_snapshot_header, then entries
    MSG_INPUT,               // client -> server: net_input_message
    MSG_KEEPALIVE,           // either: empty
    MSG_DISCONNECT,          // either: empty
    MSG_REDIRECT,            // server -> client: u32 ip, u16 port of the shard to join (reserved)
    MSG_SHIP_STATE,          // server -> client: net_ship_state
};

enum net_reject_reason { REJECT_VERSION = 1, REJECT_FULL };

// The components that cross the wire, in their own numbering so the game's
// component ids (assigned at runtime) never leak into the protocol.
enum net_component {
    NET_C_TRANSFORM, NET_C_SOLID, NET_C_ATTACHMENT, NET_C_VISUAL, NET_C_PLAYER, NET_C_SHIP, NET_C_PLANET,
    NET_C_COUNT
};

struct net_welcome {
    uint32_t session;
    cw_entity_id player;
    uint32_t tick;
    uint8_t tick_hz, snapshot_hz;
};

struct net_snapshot_header {
    uint32_t tick;               // server tick the positions are from
    uint32_t last_input_seq;     // newest input the server had applied, for the client's RTT
    uint16_t count;
};
struct net_snapshot_entry {
    cw_entity_id id;
    v3 position;
    float yaw, pitch;
};

struct net_input_message {
    uint32_t seq;                // of the newest input; the ones before it count down
    uint32_t count;              // 1..NET_INPUT_REDUNDANCY, oldest first
    PlayerInput inputs[NET_INPUT_REDUNDANCY];
};

// A ship's timetable position, sent when it departs or arrives. Between these the client
// counts the timer on by itself: it only drives what players read, and the positions
// that matter arrive in snapshots.
struct net_ship_state {
    cw_entity_id id;
    int state;                   // ship_state
    int stop;                    // ship_stop
    float timer;
};

cw_component_id net_component_id(const game *g, int component);
// Payload only; the entity must carry the component.
bool net_component_write(cw_writer *w, game *g, cw_entity_id id, int component);
bool net_component_read(cw_reader *r, game *g, cw_entity_id id, int component);

// An entity record: i64 id, u8 count, then per component u8 net_component, u16 size,
// payload. Every replicated component the entity carries is included.
bool net_entity_write(cw_writer *w, game *g, cw_entity_id id);
// Creates the entity if it does not exist, then sets the components. Idempotent.
bool net_entity_read(cw_reader *r, game *g, cw_entity_id *id);
size_t net_entity_size(game *g, cw_entity_id id);

void net_welcome_write(cw_writer *w, const net_welcome *m);
bool net_welcome_read(cw_reader *r, net_welcome *m);
void net_snapshot_header_write(cw_writer *w, const net_snapshot_header *h);
bool net_snapshot_header_read(cw_reader *r, net_snapshot_header *h);
void net_snapshot_entry_write(cw_writer *w, const net_snapshot_entry *e);
bool net_snapshot_entry_read(cw_reader *r, net_snapshot_entry *e);
void net_input_write(cw_writer *w, const net_input_message *m);
bool net_input_read(cw_reader *r, net_input_message *m);
void net_ship_state_write(cw_writer *w, const net_ship_state *m);
bool net_ship_state_read(cw_reader *r, net_ship_state *m);

#endif
