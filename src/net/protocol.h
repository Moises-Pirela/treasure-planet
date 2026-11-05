// The game's wire protocol on top of net.h: which messages exist and how the game's
// components and state are encoded. Both the server and the client include this and
// nothing else knows the byte layout.
//
// Every field is encoded explicitly, little-endian, through cw_writer and cw_reader,
// so a Windows client and a Linux server agree byte for byte and a change to a struct
// never silently changes the wire. Bump NET_PROTOCOL_VERSION when a message changes.
//
// Joining:
//   client  MSG_CONNECT ---------------------------> server     (in the clear, padded, repeated: our public
//                                                               key, who we are, a ticket if handed over,
//                                                               a connect token if a directory sent us)
//   client <------------------------ MSG_CHALLENGE  server     (in the clear, stateless: a token that only
//                                                               whoever receives at our address can echo)
//   client  MSG_CONNECT + challenge ---------------> server     (the same again, with the challenge)
//   client <----------------------------- MSG_KEYS  server     (in the clear: the session, its public key;
//                                                               both sides now hold the packet keys)
//   client <--------------------------- MSG_WELCOME  server     (reliable, encrypted: your entity, the clock)
//   client <--- MSG_SPAWN x N, MSG_BASELINE_END ---  server     (reliable: the whole world)
// Playing, every tick or snapshot:
//   client  MSG_INPUT -----------------------------> server     (sequenced, one per tick, the last few repeated)
//   client <------------------------- MSG_SNAPSHOT  server     (sequenced: players, delta-coded; snapshot.h)
//   client <------------- MSG_SPAWN / MSG_DESPAWN   server     (reliable, ordered: what came into and went
//                                                               out of the client's interest; joins, leaves)
//   client <--------------------------- MSG_UPDATE  server     (reliable, ordered: the components of a known
//                                                               entity that changed, nothing else)
//   client <------------------------ MSG_CHARACTER  server     (reliable, unordered: the gear, purse and ships, versioned)
//   client  MSG_SHIPWRIGHT ------------------------> server     (reliable: an order at the shipwright's;
//   client <------------------ MSG_SHIPWRIGHT_RESULT server      answered, and MSG_CHARACTER if it was done)
//   client  MSG_KICK ------------------------------> server     (reliable: an owner putting a player off
//                                                               their craft; the change comes back as an UPDATE)
// Channels (net.h): the handshake is unreliable, everything with state is sequenced,
// everything that must arrive is reliable ordered unless it carries its own
// version, in which case it need not wait behind a baseline.
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
#include "../game/shipwright.h"
#include "accounts.h"
#include "net.h"
#include "social.h"
#include "snapshot.h"
#include "zones.h"

#define NET_SNAPSHOT_HZ 20
#define NET_CONNECT_PADDING 600      // bytes a MSG_CONNECT is padded to: larger than any answer to it, so
                                     // that a server cannot be used to amplify traffic towards a spoofed address
#define NET_INPUT_REDUNDANCY 3       // inputs per MSG_INPUT: the newest and the ones before it
#define NET_TIMEOUT 5.0              // seconds of silence before a peer is gone
#define NET_KEEPALIVE 0.5            // seconds of nothing to say before saying so

enum net_message_type {
    MSG_CONNECT = 1,         // client -> server: net_connect_message
    MSG_KEYS,                // server -> client: net_keys_message
    MSG_WELCOME,             // server -> client: net_welcome
    MSG_REJECT,              // server -> client: u8 net_reject_reason
    MSG_SPAWN,               // server -> client: u16 count, then entity records (net_entity_write)
    MSG_DESPAWN,             // server -> client: u16 count, i64 ids
    MSG_BASELINE_END,        // server -> client: the world so far is complete
    MSG_SNAPSHOT,            // server -> client: snapshot_encode
    MSG_INPUT,               // client -> server: net_input_message
    MSG_KEEPALIVE,           // either: empty
    MSG_DISCONNECT,          // either: u8 net_leave_reason (or empty: a quit)
    MSG_REDIRECT,            // server -> client: net_redirect
    MSG_HANDOFF_FAILED,      // client -> server: empty; the redirect did not work out, unfreeze me
    MSG_PARTY_REQUEST,       // client -> server: u8 party_op, target name; relayed to the social service
    MSG_PARTY,               // server -> client: party_view, whenever it changes
    MSG_CHARACTER,           // server -> client: character_write, on joining and after every change
    MSG_EQUIP,               // client -> server: u8 stash index to wear
    MSG_UPDATE,              // server -> client: u16 count, then partial entity records (net_entity_write_masked)
    MSG_CHALLENGE,           // server -> client: u32 nonce (the connect's), u64 challenge to repeat
    MSG_KICK,                // client -> server: u8 kick_op, i64 player; a craft owner putting someone off it
    MSG_SHIPWRIGHT,          // client -> server: shipwright_order_write; a player at the shipwright's trading
    MSG_SHIPWRIGHT_RESULT,   // server -> client: u8 shipwright_result, then the order it answers
};

// What MSG_KICK asks: one player off the sender's craft, or everyone aboard it but the sender.
enum kick_op { KICK_ONE = 0, KICK_ALL = 1 };

// Why a client leaves: a quit keeps its body in the world a short while (it may be
// back), a hand-off means it is already somewhere else, so nothing stays.
enum net_leave_reason { LEAVE_QUIT = 0, LEAVE_HANDOFF = 1 };

enum net_reject_reason { REJECT_VERSION = 1, REJECT_FULL, REJECT_TICKET, REJECT_NO_WORLD, REJECT_IDENTITY, REJECT_TOKEN };

// The components that cross the wire, in their own numbering so the game's
// component ids (assigned at runtime) never leak into the protocol.
enum net_component {
    NET_C_TRANSFORM, NET_C_SOLID, NET_C_ATTACHMENT, NET_C_VISUAL, NET_C_PLAYER, NET_C_SHIP, NET_C_PLANET,
    NET_C_EQUIPMENT, NET_C_LOOT, NET_C_OWNED, NET_C_CRAFT, NET_C_PART, NET_C_GRAVITY, NET_C_BARRED, NET_C_DOCK,
    NET_C_COUNT
};

#define NET_PROOF_CAPACITY 512       // a platform's proof of identity (a Steam ticket is ~240 bytes)
struct net_connect_message {
    uint32_t version, nonce;
    uint8_t public_key[CRYPTO_KEY_SIZE];   // the client's ephemeral X25519 key
    char platform[ACCOUNT_PLATFORM_CAPACITY];   // who the client says it is (accounts.h)
    char name[ACCOUNT_NAME_CAPACITY];
    char region[NET_REGION_NAME];   // where the client is, for the choice of shard on hand-offs
    unsigned char proof[NET_PROOF_CAPACITY];
    size_t proof_size;
    uint64_t challenge;          // what the server last sent us in MSG_CHALLENGE; 0 before that
    bool has_ticket;
    net_ticket ticket;
    bool has_token;
    net_connect_token token;
};
struct net_keys_message {
    uint32_t session;
    uint8_t server_public[CRYPTO_KEY_SIZE];
    uint8_t client_public[CRYPTO_KEY_SIZE];   // echoed, so the client knows it is for this attempt
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
    bool currents;               // whether the solar current flows in this world (game.currents)
    char world[GAME_NAME_CAPACITY];   // the dungeon this world is, or "" for the universe
    char zone[NET_ZONE_NAME];    // the server's name, for a connect token to come back with
    game_place places[GAME_MAX_PLACES];
    int place_count;
};

struct net_input_message {
    uint32_t seq;                // of the newest input; the ones before it count down
    uint32_t ack_tick;           // newest snapshot the client holds, the base for the next delta
    uint32_t count;              // 1..NET_INPUT_REDUNDANCY, oldest first
    PlayerInput inputs[NET_INPUT_REDUNDANCY];
    float view_ticks[NET_INPUT_REDUNDANCY];   // the server tick the client was rendering others at
};
// Inputs travel quantised (eighths of a percent, 16-bit angles); the client applies
// this before predicting so that both sides simulate the same numbers. A helm's
// controls (lift, the three turns, the sail and its brace) follow only when one of them is not zero, so
// walking costs what it always did.
void net_input_quantize(PlayerInput *in);

struct net_redirect {
    net_address address;
    net_ticket ticket;
};

cw_component_id net_component_id(const game *g, int component);
// Payload only; the entity must carry the component.
bool net_component_write(cw_writer *w, game *g, cw_entity_id id, int component);
bool net_component_read(cw_reader *r, game *g, cw_entity_id id, int component);

// An entity record: i64 id, u8 count, then per component u8 net_component, u16 size,
// payload. Every replicated component the entity carries is included; the masked
// form includes only those whose bit (1 << net_component) is set, which is how a
// change to one component (gear) travels without the rest of the record.
#define NET_C_ALL ((1u << NET_C_COUNT) - 1)
bool net_entity_write(cw_writer *w, game *g, cw_entity_id id);
bool net_entity_write_masked(cw_writer *w, game *g, cw_entity_id id, uint32_t components);
// Creates the entity if it does not exist, then sets the components. Idempotent.
// With `create` false a record of an entity the reader does not have is skipped
// (an update that overtook a despawn).
bool net_entity_read(cw_reader *r, game *g, cw_entity_id *id);
bool net_entity_read_masked(cw_reader *r, game *g, cw_entity_id *id, bool create);
size_t net_entity_size(game *g, cw_entity_id id);
size_t net_entity_size_masked(game *g, cw_entity_id id, uint32_t components);

// The client writes what it was given: a ticket (from a redirect) and a token (from
// the directory) are opaque bytes to it, signed by those who hold the secret. The
// message is padded to NET_CONNECT_PADDING.
void net_connect_write(cw_writer *w, const net_connect_message *m, const unsigned char *ticket, size_t ticket_size,
                       const unsigned char *token, size_t token_size);
// A ticket or token that fails its signature sets *bad_credential and is left out.
bool net_connect_read(cw_reader *r, net_connect_message *m, const net_zone_table *zones, bool *bad_credential);
void net_keys_write(cw_writer *w, const net_keys_message *m);
bool net_keys_read(cw_reader *r, net_keys_message *m);
void item_write(cw_writer *w, const item *it);
bool item_read(cw_reader *r, item *it);
void character_write(cw_writer *w, const character *c);
bool character_read(cw_reader *r, character *c);
// Only what a character was before it had a purse and ships: the gear (old saves).
bool character_read_gear(cw_reader *r, character *c);
// A character as written at an older format: 1 the gear alone, 2 with ships whose
// loadouts had no look, 3 (CHARACTER_FORMAT) as now.
#define CHARACTER_FORMAT 3
bool character_read_format(cw_reader *r, character *c, int format);
void net_welcome_write(cw_writer *w, const net_welcome *m);
bool net_welcome_read(cw_reader *r, net_welcome *m);
void net_input_write(cw_writer *w, const net_input_message *m);
bool net_input_read(cw_reader *r, net_input_message *m);
void net_redirect_write(cw_writer *w, const net_redirect *m, const net_zone_table *zones);
// The ticket is opaque to the client: it is copied out for the next MSG_CONNECT.
bool net_redirect_read(cw_reader *r, net_address *address, unsigned char *ticket, size_t *ticket_size);

#endif
