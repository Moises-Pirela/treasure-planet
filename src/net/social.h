// The social service: who is online and where, and the parties (fireteams) they
// form. Parties span zones, instances and servers, so they live here, above them.
//
// Clients never talk to it. Their zone server relays what they ask (reliably,
// encrypted) as signed datagrams, reports their presence (joins, leaves, the world
// they are in), and receives the party state to pass on whenever it changes for
// someone it hosts. The service runs inside the directory process; a server with no
// directory runs one of its own, in-process, so parties work alone too.
//
// A party has a leader and up to SOCIAL_PARTY_SIZE members, by account. Inviting is
// by display name (what a player sees of another); an invitation waits until it is
// accepted, declined or expires. Members who go offline stay in the party, and
// their presence comes back when they do. Members entering a dungeon share an
// instance, because the party id is the instance key.
//
// Accounts come from accounts.h: the platform identity a player presented, resolved
// by its provider. The record kept here (platform, name, last seen) is what a
// database would hold, and where a real Steam/console integration would sync
// friends and personas from.
#ifndef TP_SOCIAL_H
#define TP_SOCIAL_H

#include "../game/items.h"
#include "accounts.h"
#include "zones.h"

#define SOCIAL_PARTY_SIZE 5
#define SOCIAL_MAX_PLAYERS 1024
#define SOCIAL_MAX_PARTIES 256
#define SOCIAL_PRESENCE_EXPIRY 15.0  // seconds without a presence report before a player is offline
#define SOCIAL_PRESENCE_EVERY 5.0    // seconds between a server's presence reports
#define SOCIAL_INVITE_EXPIRY 60.0

enum social_message_type {
    MSG_SOC_PRESENCE = 48,   // server -> social: account, name, platform, server, world, kind; signed
    MSG_SOC_PARTY_OP,        // server -> social: account, op, target name; signed
    MSG_SOC_PARTY_STATE,     // social -> server: account, party_view; signed
    MSG_SOC_CHARACTER_GET,   // server -> social: account; signed
    MSG_SOC_CHARACTER,       // social -> server: account, character; signed
    MSG_SOC_CHARACTER_SET,   // server -> social: account, character; signed. Kept, and saved to disk
};

// A server's word on a player it hosts, under the connection's session id. An
// arrival moves the player to that connection; a refresh or a departure counts only
// from the connection the player is on, since the old one's last words about a
// player who moved on (to another server, or another world of the same one) may
// arrive after the new one's first.
enum presence_kind { PRESENCE_REFRESH, PRESENCE_ARRIVED, PRESENCE_LEFT };

enum party_op { PARTY_CREATE = 1, PARTY_INVITE, PARTY_ACCEPT, PARTY_DECLINE, PARTY_LEAVE, PARTY_KICK, PARTY_PROMOTE };
enum party_result {
    PARTY_OK = 0,
    PARTY_ERROR_UNKNOWN_PLAYER,  // no such name online
    PARTY_ERROR_FULL,
    PARTY_ERROR_NOT_LEADER,
    PARTY_ERROR_ALREADY_IN_ONE,  // the target (or you) already has a party
    PARTY_ERROR_NO_INVITE,
    PARTY_ERROR_NOT_IN_PARTY,
    PARTY_ERROR_NOT_A_MEMBER,
};
const char *party_result_string(int result);

// What a player sees of their party: what the UI draws.
struct party_member_view {
    uint64_t account;
    char name[ACCOUNT_NAME_CAPACITY];
    char world[NET_ZONE_NAME];   // "" for the universe, else the dungeon
    char server[NET_ZONE_NAME];  // the zone (shard) hosting them: where a fireteam is kept together
    bool online, leader;
};
struct party_view {
    uint64_t party;              // 0: none
    int count;
    party_member_view members[SOCIAL_PARTY_SIZE];
    uint64_t invite;             // a party asking us to join (0: none)
    char invited_by[ACCOUNT_NAME_CAPACITY];
    uint8_t last_result;         // party_result of the player's last request
};

struct social_player {
    bool used;
    uint64_t account;
    char name[ACCOUNT_NAME_CAPACITY];
    char platform[ACCOUNT_PLATFORM_CAPACITY];
    char server[NET_ZONE_NAME];  // the zone hosting them, by name
    char world[NET_ZONE_NAME];
    net_address via;             // the server's address, where their party state is sent
    uint32_t session;            // the connection hosting them
    bool online;
    double last_seen;
    uint64_t party, invite;
    char invited_by[ACCOUNT_NAME_CAPACITY];
    double invited_at;
    uint8_t last_result;
    character gear;              // what they own: the persisted part of a player
};
struct social_party {
    bool used;
    uint64_t id, leader;
    uint64_t members[SOCIAL_PARTY_SIZE];
    int count;
};

// Sends a signed datagram to a server (the service's way out).
typedef void (*social_deliver_fn)(void *user, net_address to, uint8_t type, const void *payload, size_t size);

struct social {
    const net_zone_table *secret;
    social_player players[SOCIAL_MAX_PLAYERS];
    social_party parties[SOCIAL_MAX_PARTIES];
    uint64_t next_party;
    social_deliver_fn deliver;
    void *user;
    uint32_t presences, ops, pushes, rejected, character_sets;
    char save_path[256];         // where characters are kept between runs ("" keeps none)
    bool dirty;
    double last_saved;
};
#define SOCIAL_SAVE_EVERY 5.0        // seconds between writes while something changed
#define SOCIAL_SAVE_VERSION 3          // 3: ships have looks; 2: characters have a purse and ships; 1: gear alone. All load

void social_init(social *s, const net_zone_table *secret, social_deliver_fn deliver, void *user);
// A message from a server (already peeled out of its packet). `from` is where
// replies for the players it hosts go.
void social_handle(social *s, net_address from, uint8_t type, cw_reader payload, double now);
// Expires presence and invitations.
void social_update(social *s, double now);
// The state as a player would see it; false when the player is unknown.
bool social_view(const social *s, uint64_t account, party_view *out);
const social_player *social_find(const social *s, uint64_t account);
const social_player *social_find_name(const social *s, const char *name);

// Persistence: characters (and the accounts they belong to) in one file, rewritten
// atomically a few seconds after a change. Loading is for a fresh service.
void social_set_save_path(social *s, const char *path);
bool social_load(social *s, const char *path);
bool social_save(social *s);
// A player's character; creates an empty one for a newcomer.
character *social_character(social *s, uint64_t account, const char *name);

// Direct forms of the two requests, for the embedded service and tests.
void social_presence(social *s, net_address via, uint32_t session, uint64_t account, const char *name,
                     const char *platform, const char *server, const char *world, int kind, double now);
int social_party_request(social *s, uint64_t account, int op, const char *target_name, double now);

// Encoding. The server-facing messages are signed with the zone secret.
void social_presence_write(cw_writer *w, uint32_t session, uint64_t account, const char *name, const char *platform,
                           const char *server, const char *world, int kind, const net_zone_table *secret);
void social_party_op_write(cw_writer *w, uint64_t account, int op, const char *target_name, const net_zone_table *secret);
void social_party_state_write(cw_writer *w, uint64_t account, const party_view *view, const net_zone_table *secret);
bool social_party_state_read(cw_reader *r, uint64_t *account, party_view *view, const net_zone_table *secret);
// The view alone, as a server passes it to its client.
void party_view_write(cw_writer *w, const party_view *view);
bool party_view_read(cw_reader *r, party_view *view);
void social_character_get_write(cw_writer *w, uint64_t account, const net_zone_table *secret);
void social_character_write(cw_writer *w, uint64_t account, const character *c, const net_zone_table *secret);
bool social_character_read(cw_reader *r, uint64_t *account, character *c, const net_zone_table *secret);

#endif
