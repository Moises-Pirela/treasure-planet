// The game's wire protocol on top of net.h: which messages exist and how the game's
// components and state are encoded. Both the server and the client include this and
// nothing else knows the byte layout.
//
// Every field is encoded explicitly, little-endian, through cw_writer and cw_reader,
// so a Windows client and a Linux server agree byte for byte and a change to a struct
// never silently changes the wire. Bump NET_PROTOCOL_VERSION when a message changes.
//
// Joining:
//   client  MSG_CONNECT ---------------------------> server     (unreliable, repeated; a ticket if handed over)
//   client <--------------------------- MSG_WELCOME  server     (reliable: session, your entity, the clock)
//   client <--- MSG_SPAWN x N, MSG_BASELINE_END ---  server     (reliable: the whole world)
// Playing, every tick or snapshot:
//   client  MSG_INPUT -----------------------------> server     (unreliable, one per tick, the last few repeated)
//   client <------------------------- MSG_SNAPSHOT  server     (unreliable: players, delta-coded; snapshot.h)
//   client <------------- MSG_SPAWN / MSG_DESPAWN   server     (reliable: joins, leaves)
// Changing zone:
//   client <------------------------- MSG_REDIRECT  server     (reliable: the other zone and a signed ticket;
//                                                               the player is frozen here from now on)
//   client  MSG_CONNECT + ticket -----------> other server
//   client  MSG_DISCONNECT ----------------------> server     (once the other zone is showing)
//   client  MSG_HANDOFF_FAILED ------------------> server     (instead, if the other zone would not have us)
// Either side may send MSG_DISCONNECT; silence for longer than the timeout is the same.
#ifndef TP_PROTOCOL_H
#define TP_PROTOCOL_H

#include "../game/game.h"
#include "net.h"
#include "snapshot.h"
#include "zones.h"

#define NET_SNAPSHOT_HZ 20
#define NET_INPUT_REDUNDANCY 3       // inputs per MSG_INPUT: the newest and the ones before it
#define NET_TIMEOUT 5.0              // seconds of silence before a peer is gone
#define NET_KEEPALIVE 0.5            // seconds of nothing to say before saying so

enum net_message_type {
    MSG_CONNECT = 1,         // client -> server: net_connect_message
    MSG_WELCOME,             // server -> client: net_welcome
    MSG_REJECT,              // server -> client: u8 net_reject_reason
    MSG_SPAWN,               // server -> client: u16 count, then entity records (net_entity_write)
    MSG_DESPAWN,             // server -> client: u16 count, i64 ids
    MSG_BASELINE_END,        // server -> client: the world so far is complete
    MSG_SNAPSHOT,            // server -> client: snapshot_encode
    MSG_INPUT,               // client -> server: net_input_message
    MSG_KEEPALIVE,           // either: empty
    MSG_DISCONNECT,          // either: empty
    MSG_REDIRECT,            // server -> client: net_redirect
    MSG_HANDOFF_FAILED,      // client -> server: empty; the redirect did not work out, unfreeze me
};

enum net_reject_reason { REJECT_VERSION = 1, REJECT_FULL, REJECT_TICKET };

// The components that cross the wire, in their own numbering so the game's
// component ids (assigned at runtime) never leak into the protocol.
enum net_component {
    NET_C_TRANSFORM, NET_C_SOLID, NET_C_ATTACHMENT, NET_C_VISUAL, NET_C_PLAYER, NET_C_SHIP, NET_C_PLANET,
    NET_C_COUNT
};

struct net_connect_message {
    uint32_t version, nonce;
    bool has_ticket;
    net_ticket ticket;
};

struct net_welcome {
    uint32_t session;
    cw_entity_id player;
    uint32_t tick;               // the server's tick as it sent this
    uint8_t tick_hz, snapshot_hz;
    double universe_base;        // universe time at the server's tick zero
    v3 spawn;                    // where players come back after falling off the world
    uint64_t account;
    uint8_t input_target;        // inputs the server likes to have queued: what the client steers for
    game_place places[GAME_MAX_PLACES];
    int place_count;
};

struct net_input_message {
    uint32_t seq;                // of the newest input; the ones before it count down
    uint32_t ack_tick;           // newest snapshot the client holds, the base for the next delta
    uint32_t count;              // 1..NET_INPUT_REDUNDANCY, oldest first
    PlayerInput inputs[NET_INPUT_REDUNDANCY];
};

struct net_redirect {
    net_address address;
    net_ticket ticket;
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

// Tickets are signed with the zone table's secret; a NULL table means none is carried.
void net_connect_write(cw_writer *w, const net_connect_message *m, const net_zone_table *zones);
// A ticket that fails its signature leaves has_ticket false and sets *bad_ticket.
bool net_connect_read(cw_reader *r, net_connect_message *m, const net_zone_table *zones, bool *bad_ticket);
void net_welcome_write(cw_writer *w, const net_welcome *m);
bool net_welcome_read(cw_reader *r, net_welcome *m);
void net_input_write(cw_writer *w, const net_input_message *m);
bool net_input_read(cw_reader *r, net_input_message *m);
void net_redirect_write(cw_writer *w, const net_redirect *m, const net_zone_table *zones);
// The ticket is opaque to the client: it is copied out for the next MSG_CONNECT.
bool net_redirect_read(cw_reader *r, net_address *address, unsigned char *ticket, size_t *ticket_size);

#endif
