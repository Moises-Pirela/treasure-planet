// Zones: which server owns which place, and the tickets players carry between them.
//
// The universe is split by place (see game_place): a zone server owns some places
// and every player standing in them. The table below is the same file on every
// server. When a player walks into a place another zone owns, their server issues a
// ticket (their state, signed with the shared secret) and redirects the client,
// which presents the ticket to the new zone. Servers never talk to each other;
// the signature is what lets the new zone trust what the client brought.
//
// Dungeons are instances: worlds created on demand for whoever holds the same
// instance key (solo: the player's account), hosted either by dedicated instance
// hosts from the table or, when none are listed, by the zone the door stands in.
//
// The file is plain text, one directive per line:
//   secret <word>                        the key tickets are signed with
//   zone <name> <host:port> <place>...   a server and the places it owns
//   instances <name> <host:port>         a server that only hosts dungeon instances
// A server started with --zone <name> listens on that entry's port and owns its
// places; places listed by no zone stay with whoever has them.
#ifndef TP_ZONES_H
#define TP_ZONES_H

#include "../tp_math.h"
#include "accounts.h"
#include "net.h"

#define NET_MAX_ZONES 16
#define NET_ZONE_PLACES 8
#define NET_ZONE_NAME 32
#define NET_SECRET_CAPACITY 64
#define NET_TICKET_SIZE 224          // encoded bytes at most, signature included
#define NET_TICKET_LIFETIME 30.0     // seconds of universe time a ticket stays valid
#define NET_TOKEN_SIZE 128           // encoded bytes at most, signature included
#define NET_TOKEN_LIFETIME 60.0      // seconds a connect token stays valid

#define NET_REGION_NAME 8            // "eu", "na-west", ...
struct net_zone {
    char name[NET_ZONE_NAME];
    net_address address;
    char places[NET_ZONE_PLACES][NET_ZONE_NAME];
    int place_count;
    bool instance_host;          // hosts dungeon instances only, no places
    // Shards: several zones may own the same place, each a full copy of it. Which
    // one a player goes to is chosen by region, then by filling shards up to their
    // target before opening another, so that no shard feels empty.
    char region[NET_REGION_NAME];
    uint16_t clients, capacity, target;   // as last registered with the directory
    bool draining;               // being emptied to stop: nobody is sent there
};
struct net_zone_table {
    net_zone zones[NET_MAX_ZONES];
    int count;
    char secret[NET_SECRET_CAPACITY];
};

void net_zones_init(net_zone_table *t);                  // empty, with the development secret
bool net_zones_load(net_zone_table *t, const char *path, char *error, size_t error_capacity);
int net_zones_add(net_zone_table *t, const char *name, net_address address);   // index, or -1 when full
bool net_zone_add_place(net_zone *zone, const char *place);
int net_zones_find(const net_zone_table *t, const char *name);
int net_zones_owner(const net_zone_table *t, const char *place);               // zone index, or -1
// The instance host for an instance key, spread over the hosts listed; -1 when none.
int net_zones_instance_host(const net_zone_table *t, uint64_t key);
// The shard of `place` a player should go to: one where `with` (the names of the
// servers their party members are on) has room, else one in their region, else
// any; among those, the fullest still below its target, else the emptiest. -1
// when no shard owns the place or all are full.
int net_zones_choose(const net_zone_table *t, const char *place, const char *region, const char *const *with,
                     int with_count);
void net_zones_set_secret(net_zone_table *t, const char *secret);

// What a player carries from one world to the next. With `dungeon` empty it is a
// move between zones and `position` is where to stand; with a dungeon named it
// opens (or joins) the instance of that dungeon for `key`, standing at its spawn,
// and `return_place`/`return_position` are where the exit leads.
struct net_ticket {
    uint64_t account;            // the player's identity across zones, as the issuing zone verified it
    char name[ACCOUNT_NAME_CAPACITY];
    char platform[ACCOUNT_PLATFORM_CAPACITY];
    v3 position, velocity;
    float yaw, pitch;
    double issued;               // universe time
    char dungeon[NET_ZONE_NAME];
    uint64_t key;
    char return_place[NET_ZONE_NAME];
    v3 return_position;
    uint64_t signature;          // filled by net_ticket_read; what a zone remembers to refuse a replay
};
void net_ticket_write(cw_writer *w, const net_ticket *ticket, const net_zone_table *zones);
// False when the signature does not match the table's secret (or the bytes are short).
bool net_ticket_read(cw_reader *r, net_ticket *ticket, const net_zone_table *zones);

// A connect token: the directory's word that a player may join a server, issued
// when it chose that server for them and good for NET_TOKEN_LIFETIME. It carries
// the identity the directory verified (so a shard never verifies a platform's
// proof itself), the zone it is for, and a nonce the server remembers so that a
// token is good once per address. A server behind a directory admits nobody
// without one, or a ticket; the directory is the only way in.
struct net_connect_token {
    uint64_t account;
    char name[ACCOUNT_NAME_CAPACITY];
    char platform[ACCOUNT_PLATFORM_CAPACITY];
    char zone[NET_ZONE_NAME];    // the server it admits to, by name
    double expires;              // wall-clock seconds (net_wall_seconds)
    uint64_t nonce;
    uint64_t signature;          // filled by net_token_read
};
void net_token_write(cw_writer *w, const net_connect_token *token, const net_zone_table *zones);
bool net_token_read(cw_reader *r, net_connect_token *token, const net_zone_table *zones);

// SipHash-2-4: a keyed 64-bit hash, which is what signs tickets and tokens.
uint64_t net_siphash(const void *data, size_t size, uint64_t k0, uint64_t k1);

#endif
