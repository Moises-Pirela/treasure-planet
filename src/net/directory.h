// The zone directory: where servers say what they are and clients ask where to go.
//
// Each server registers every few seconds (its name, the places it owns or that it
// hosts instances, its load, the address it wants published); an entry that stops
// being refreshed expires. Servers fetch the table back with every registration and
// use it for hand-offs in place of a static zones file. Clients ask for the table to
// find the server owning a place, and for an *introduction*: the directory tells
// that server the client's address as seen from outside, and the server sends a few
// packets there, which opens the server's NAT (if any) for the client's packets the
// way the client's own first packet opened its own. Registrations, tables and
// introductions carry a SipHash of their body under the zone secret, so only a
// server holding it can register or be believed; client queries are unsigned.
//
// A client's query for a place carries its platform identity and proof; the
// directory verifies them (accounts.h) and answers with a *connect token*
// (zones.h): the identity and the chosen zone, signed, good for a minute. The
// zone admits the client on the token alone, so a shard never verifies a platform
// proof itself and nobody joins a shard the directory did not send them to. A
// client that lost its connection asks for a token for the zone it was on.
//
// All of it is a few datagrams on the same transport as everything else, in the
// clear on session 0 and repeated until answered.
#ifndef TP_DIRECTORY_H
#define TP_DIRECTORY_H

#include "social.h"
#include "zones.h"

#define DIRECTORY_EXPIRY 10.0        // seconds without a registration before an entry is dropped
#define DIRECTORY_REGISTER_EVERY 2.0 // seconds between a server's registrations
#define DIRECTORY_QUERY_EVERY 0.25   // seconds between a client's repeated queries
#define DIRECTORY_PUNCHES 3          // packets a server sends to open its NAT towards a client

enum directory_message_type {
    MSG_DIR_REGISTER = 32,   // server -> directory: net_zone entry, u16 clients, u16 capacity; signed
    MSG_DIR_QUERY,           // anyone -> directory: directory_query
    MSG_DIR_TABLE,           // directory -> asker: the live table, the chosen shard first; signed
    MSG_DIR_PUNCH,           // directory -> server: net_address of a client to open up for; signed
    MSG_DIR_DRAIN,           // orchestrator -> directory: zone name; signed. Nobody is sent there from now on
    MSG_DIR_TOKEN,           // directory -> asker: u8 refused (identity not accepted), else a net_connect_token
};
enum directory_query_flags { DIR_QUERY_PLACE = 1, DIR_QUERY_INTRODUCE = 2, DIR_QUERY_IDENTITY = 4, DIR_QUERY_ZONE = 8 };
#define DIRECTORY_QUERIES_PER_IP 20.0  // a second, from one address, with a burst of
#define DIRECTORY_QUERIES_BURST 100.0  // ...this many (a LAN party, a load test)

struct directory_entry {
    net_zone zone;               // as registered (address: what the server asked to publish)
    net_address observed;        // where its registrations come from
    uint16_t clients, capacity;
    // Players sent this way who have not shown up in its count yet: added to the
    // population when choosing, consumed as its reports rise, forgotten if they
    // never do.
    int pending;
    double pending_at;
    double last_seen;
    bool used, draining;
};
#define DIRECTORY_PENDING_EXPIRY 15.0

struct directory {
    net_socket *socket;
    net_zone_table secret;       // only the secret is used
    social people;               // the social service lives in this process too
    directory_entry entries[NET_MAX_ZONES];
    double expiry;
    bool verbose;
    net_limiter queries_by_address;
    uint32_t registrations, queries, introductions, rejected, assignments;
    uint32_t tokens_issued, identities_refused;
    uint32_t saturated;          // assignments made with every shard of the place at or past its target
    double last_saturation_notice;
    unsigned char scratch[NET_MAX_PACKET];
};

bool directory_init(directory *d, uint16_t port, const char *secret, bool verbose);
void directory_destroy(directory *d);
void directory_update(directory *d, double now);
uint16_t directory_port(const directory *d);
// The live entries as a zone table (with the secret), for the directory's own use.
void directory_table(const directory *d, net_zone_table *out);

// Encoding shared by both ends.
void directory_register_write(cw_writer *w, const net_zone *zone, uint16_t clients, uint16_t capacity,
                              const net_zone_table *secret);
bool directory_register_read(cw_reader *r, net_zone *zone, uint16_t *clients, uint16_t *capacity,
                             const net_zone_table *secret);
struct directory_query {
    char place[NET_ZONE_NAME];   // the place wanted ("" for the table only)
    bool introduce;              // or: introduce me to the server at...
    net_address introduce_to;
    char zone[NET_ZONE_NAME];    // or: a token for this zone, which I was on ("" otherwise)
    char region[NET_REGION_NAME];   // the asker's region, for the choice of shard
    uint64_t account;            // the asker, for their party's shard (unverified: it can only steer themselves)
    // Who the asker is, for a token: verified here. Without it no token is issued.
    bool has_identity;
    char platform[ACCOUNT_PLATFORM_CAPACITY];
    char name[ACCOUNT_NAME_CAPACITY];
    unsigned char proof[512];
    size_t proof_size;
};
void directory_query_write(cw_writer *w, const directory_query *q);
bool directory_query_read(cw_reader *r, directory_query *q);
// A token message: `token_size` 0 means refused. The client keeps the bytes opaque.
void directory_token_write(cw_writer *w, const net_connect_token *token, const net_zone_table *secret);
bool directory_token_read(cw_reader *r, unsigned char *token, size_t capacity, size_t *token_size);
void directory_table_write(cw_writer *w, const net_zone_table *table, const net_zone_table *secret);
// Unsigned readers accept any table (clients hold no secret); pass `secret` to insist.
bool directory_table_read(cw_reader *r, net_zone_table *table, const net_zone_table *secret);
void directory_punch_write(cw_writer *w, net_address client, const net_zone_table *secret);
bool directory_punch_read(cw_reader *r, net_address *client, const net_zone_table *secret);
void directory_drain_write(cw_writer *w, const char *zone, const net_zone_table *secret);
bool directory_drain_read(cw_reader *r, char zone[NET_ZONE_NAME], const net_zone_table *secret);

#endif
