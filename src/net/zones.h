// Zones: which server owns which place, and the tickets players carry between them.
//
// The universe is split by place (see game_place): a zone server owns some places
// and every player standing in them. The table below is the same file on every
// server. When a player walks into a place another zone owns, their server issues a
// ticket (their state, signed with the shared secret) and redirects the client,
// which presents the ticket to the new zone. Servers never talk to each other;
// the signature is what lets the new zone trust what the client brought.
//
// The file is plain text, one directive per line:
//   secret <word>                        the key tickets are signed with
//   zone <name> <host:port> <place>...   a server and the places it owns
// A server started with --zone <name> listens on that zone's port and owns its
// places; places listed by no zone stay with whoever has them.
#ifndef TP_ZONES_H
#define TP_ZONES_H

#include "../tp_math.h"
#include "net.h"

#define NET_MAX_ZONES 16
#define NET_ZONE_PLACES 8
#define NET_ZONE_NAME 32
#define NET_SECRET_CAPACITY 64
#define NET_TICKET_SIZE 64           // encoded bytes, signature included
#define NET_TICKET_LIFETIME 30.0     // seconds of universe time a ticket stays valid

struct net_zone {
    char name[NET_ZONE_NAME];
    net_address address;
    char places[NET_ZONE_PLACES][NET_ZONE_NAME];
    int place_count;
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
void net_zones_set_secret(net_zone_table *t, const char *secret);

// What a player carries from one zone to the next.
struct net_ticket {
    uint64_t account;            // the player's identity across zones
    v3 position, velocity;
    float yaw, pitch;
    double issued;               // universe time
    uint64_t signature;          // filled by net_ticket_read; what a zone remembers to refuse a replay
};
void net_ticket_write(cw_writer *w, const net_ticket *ticket, const net_zone_table *zones);
// False when the signature does not match the table's secret (or the bytes are short).
bool net_ticket_read(cw_reader *r, net_ticket *ticket, const net_zone_table *zones);

// SipHash-2-4: a keyed 64-bit hash, which is what signs tickets.
uint64_t net_siphash(const void *data, size_t size, uint64_t k0, uint64_t k1);

#endif
