#include "directory.h"
#include "accounts.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Encoding. Signed messages are: u16 body size, body, u64 SipHash(body).
// ---------------------------------------------------------------------------
static void secret_keys(const net_zone_table *zones, uint64_t *k0, uint64_t *k1) {
    uint64_t a = 0x9e3779b97f4a7c15ull, b = 0xd1b54a32d192ed03ull;   // distinct from the ticket keys
    for (const char *c = zones->secret; *c; ++c) {
        a = (a ^ (unsigned char)*c) * 0x100000001b3ull;
        b = (b ^ (unsigned char)*c) * 0x100000001b3ull;
    }
    *k0 = a;
    *k1 = b;
}

static void write_name(cw_writer *w, const char *name) {
    uint8_t length = (uint8_t)strnlen(name, NET_ZONE_NAME - 1);
    cw_writer_u8(w, length);
    cw_writer_bytes(w, name, length);
}
static bool read_name(cw_reader *r, char *name) {
    uint8_t length = 0;
    cw_reader_u8(r, &length);
    if (length >= NET_ZONE_NAME) return false;
    memset(name, 0, NET_ZONE_NAME);
    return cw_reader_bytes(r, name, length);
}
static void write_address(cw_writer *w, net_address a) {
    cw_writer_u32(w, a.ip);
    cw_writer_u16(w, a.port);
}
static bool read_address(cw_reader *r, net_address *a) {
    cw_reader_u32(r, &a->ip);
    cw_reader_u16(r, &a->port);
    return cw_reader_ok(r);
}
static void write_zone(cw_writer *w, const net_zone *zone) {
    write_name(w, zone->name);
    write_address(w, zone->address);
    cw_writer_u8(w, zone->instance_host ? 1 : 0);
    cw_writer_u8(w, (uint8_t)zone->place_count);
    for (int i = 0; i < zone->place_count; ++i) write_name(w, zone->places[i]);
    uint8_t region_length = (uint8_t)strnlen(zone->region, NET_REGION_NAME - 1);
    cw_writer_u8(w, region_length);
    cw_writer_bytes(w, zone->region, region_length);
    cw_writer_u16(w, zone->clients);
    cw_writer_u16(w, zone->capacity);
    cw_writer_u16(w, zone->target);
    cw_writer_u8(w, zone->draining ? 1 : 0);
}
static bool read_zone(cw_reader *r, net_zone *zone) {
    memset(zone, 0, sizeof(*zone));
    uint8_t instance_host = 0, places = 0, region_length = 0;
    if (!read_name(r, zone->name)) return false;
    read_address(r, &zone->address);
    cw_reader_u8(r, &instance_host);
    cw_reader_u8(r, &places);
    if (places > NET_ZONE_PLACES) return false;
    zone->instance_host = instance_host != 0;
    zone->place_count = places;
    for (int i = 0; i < places; ++i)
        if (!read_name(r, zone->places[i])) return false;
    cw_reader_u8(r, &region_length);
    if (region_length >= NET_REGION_NAME || !cw_reader_bytes(r, zone->region, region_length)) return false;
    cw_reader_u16(r, &zone->clients);
    cw_reader_u16(r, &zone->capacity);
    cw_reader_u16(r, &zone->target);
    uint8_t draining = 0;
    cw_reader_u8(r, &draining);
    zone->draining = draining != 0;
    return cw_reader_ok(r);
}

// Wraps a body written by `fill` with its size and signature.
static void write_signed(cw_writer *w, const unsigned char *body, size_t size, const net_zone_table *secret) {
    uint64_t k0, k1;
    secret_keys(secret, &k0, &k1);
    cw_writer_u16(w, (uint16_t)size);
    cw_writer_bytes(w, body, size);
    cw_writer_u64(w, net_siphash(body, size, k0, k1));
}
// Opens a signed body; with a secret the signature must match, without it is only skipped.
static bool read_signed(cw_reader *r, cw_reader *body, const net_zone_table *secret) {
    uint16_t size = 0;
    cw_reader_u16(r, &size);
    const unsigned char *data = (const unsigned char *)cw_reader_view(r, size);
    uint64_t signature = 0;
    if (!data || !cw_reader_u64(r, &signature)) return false;
    if (secret) {
        uint64_t k0, k1;
        secret_keys(secret, &k0, &k1);
        if (net_siphash(data, size, k0, k1) != signature) return false;
    }
    *body = cw_reader_make(data, size);
    return true;
}

void directory_register_write(cw_writer *w, const net_zone *zone, uint16_t clients, uint16_t capacity,
                              const net_zone_table *secret) {
    unsigned char body[512];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    write_zone(&b, zone);
    cw_writer_u16(&b, clients);
    cw_writer_u16(&b, capacity);
    write_signed(w, body, b.size, secret);
}
bool directory_register_read(cw_reader *r, net_zone *zone, uint16_t *clients, uint16_t *capacity,
                             const net_zone_table *secret) {
    cw_reader body;
    if (!read_signed(r, &body, secret) || !read_zone(&body, zone)) return false;
    cw_reader_u16(&body, clients);
    cw_reader_u16(&body, capacity);
    return cw_reader_ok(&body);
}

static void write_text(cw_writer *w, const char *text, size_t capacity) {
    uint8_t length = (uint8_t)strnlen(text, capacity - 1);
    cw_writer_u8(w, length);
    cw_writer_bytes(w, text, length);
}
static bool read_text(cw_reader *r, char *text, size_t capacity) {
    uint8_t length = 0;
    cw_reader_u8(r, &length);
    if (length >= capacity) return false;
    memset(text, 0, capacity);
    return cw_reader_bytes(r, text, length);
}

void directory_query_write(cw_writer *w, const directory_query *q) {
    uint8_t flags = (uint8_t)((q->place[0] ? DIR_QUERY_PLACE : 0) | (q->introduce ? DIR_QUERY_INTRODUCE : 0)
                              | (q->has_identity ? DIR_QUERY_IDENTITY : 0) | (q->zone[0] ? DIR_QUERY_ZONE : 0));
    cw_writer_u8(w, flags);
    if (flags & DIR_QUERY_PLACE) write_name(w, q->place);
    if (flags & DIR_QUERY_INTRODUCE) write_address(w, q->introduce_to);
    if (flags & DIR_QUERY_ZONE) write_name(w, q->zone);
    write_text(w, q->region, NET_REGION_NAME);
    cw_writer_u64(w, q->account);
    if (flags & DIR_QUERY_IDENTITY) {
        write_text(w, q->platform, ACCOUNT_PLATFORM_CAPACITY);
        write_text(w, q->name, ACCOUNT_NAME_CAPACITY);
        cw_writer_u16(w, (uint16_t)q->proof_size);
        cw_writer_bytes(w, q->proof, q->proof_size);
    }
}
bool directory_query_read(cw_reader *r, directory_query *q) {
    memset(q, 0, sizeof(*q));
    uint8_t flags = 0;
    cw_reader_u8(r, &flags);
    if ((flags & DIR_QUERY_PLACE) && !read_name(r, q->place)) return false;
    if (flags & DIR_QUERY_INTRODUCE) {
        if (!read_address(r, &q->introduce_to)) return false;
        q->introduce = true;
    }
    if ((flags & DIR_QUERY_ZONE) && !read_name(r, q->zone)) return false;
    if (!read_text(r, q->region, NET_REGION_NAME)) return false;
    cw_reader_u64(r, &q->account);
    if (flags & DIR_QUERY_IDENTITY) {
        uint16_t proof_size = 0;
        if (!read_text(r, q->platform, ACCOUNT_PLATFORM_CAPACITY) || !read_text(r, q->name, ACCOUNT_NAME_CAPACITY)) return false;
        cw_reader_u16(r, &proof_size);
        if (proof_size > sizeof(q->proof) || !cw_reader_bytes(r, q->proof, proof_size)) return false;
        q->proof_size = proof_size;
        q->has_identity = true;
    }
    return cw_reader_ok(r);
}

void directory_token_write(cw_writer *w, const net_connect_token *token, const net_zone_table *secret) {
    cw_writer_u8(w, token ? 0 : 1);
    if (token) net_token_write(w, token, secret);
}
bool directory_token_read(cw_reader *r, unsigned char *token, size_t capacity, size_t *token_size) {
    uint8_t refused = 0;
    cw_reader_u8(r, &refused);
    *token_size = 0;
    if (!cw_reader_ok(r)) return false;
    if (refused) return true;
    size_t size = cw_reader_remaining(r);
    if (size > capacity) return false;
    memcpy(token, cw_reader_view(r, size), size);
    *token_size = size;
    return true;
}

void directory_table_write(cw_writer *w, const net_zone_table *table, const net_zone_table *secret) {
    unsigned char body[NET_MAX_PACKET];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    cw_writer_u8(&b, (uint8_t)table->count);
    for (int i = 0; i < table->count; ++i) write_zone(&b, &table->zones[i]);
    write_signed(w, body, b.size, secret);
}
bool directory_table_read(cw_reader *r, net_zone_table *table, const net_zone_table *secret) {
    cw_reader body;
    if (!read_signed(r, &body, secret)) return false;
    uint8_t count = 0;
    cw_reader_u8(&body, &count);
    if (count > NET_MAX_ZONES) return false;
    char keep[NET_SECRET_CAPACITY];
    memcpy(keep, table->secret, sizeof(keep));
    memset(table, 0, sizeof(*table));
    memcpy(table->secret, keep, sizeof(keep));
    for (int i = 0; i < count; ++i)
        if (!read_zone(&body, &table->zones[i])) return false;
    table->count = count;
    return true;
}

void directory_punch_write(cw_writer *w, net_address client, const net_zone_table *secret) {
    unsigned char body[8];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    write_address(&b, client);
    write_signed(w, body, b.size, secret);
}
bool directory_punch_read(cw_reader *r, net_address *client, const net_zone_table *secret) {
    cw_reader body;
    return read_signed(r, &body, secret) && read_address(&body, client);
}

void directory_drain_write(cw_writer *w, const char *zone, const net_zone_table *secret) {
    unsigned char body[40];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    write_name(&b, zone);
    write_signed(w, body, b.size, secret);
}
bool directory_drain_read(cw_reader *r, char zone[NET_ZONE_NAME], const net_zone_table *secret) {
    cw_reader body;
    return read_signed(r, &body, secret) && read_name(&body, zone);
}

// ---------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------
static void social_deliver(void *user, net_address to, uint8_t type, const void *payload, size_t size) {
    directory *d = (directory *)user;
    net_connection tmp;
    net_connection_init(&tmp, d->socket, to, 0, NULL, NULL);
    net_connection_send(&tmp, type, NET_UNRELIABLE, payload, size);
    net_connection_flush(&tmp, net_seconds(), false);
    net_connection_destroy(&tmp);
}

bool directory_init(directory *d, uint16_t port, const char *secret, bool verbose) {
    memset(d, 0, sizeof(*d));
    d->socket = net_socket_open(port);
    if (!d->socket) return false;
    net_zones_init(&d->secret);
    if (secret && *secret) net_zones_set_secret(&d->secret, secret);
    social_init(&d->people, &d->secret, social_deliver, d);
    d->expiry = DIRECTORY_EXPIRY;
    d->verbose = verbose;
    net_limiter_init(&d->queries_by_address, DIRECTORY_QUERIES_PER_IP, DIRECTORY_QUERIES_BURST);
    return true;
}

void directory_destroy(directory *d) {
    net_socket_close(d->socket);
    memset(d, 0, sizeof(*d));
}

uint16_t directory_port(const directory *d) { return net_socket_port(d->socket); }

void directory_table(const directory *d, net_zone_table *out) {
    *out = d->secret;
    out->count = 0;
    for (int i = 0; i < NET_MAX_ZONES; ++i) {
        const directory_entry *e = &d->entries[i];
        if (!e->used) continue;
        net_zone *z = &out->zones[out->count++];
        *z = e->zone;
        int population = (int)e->zone.clients + e->pending;
        z->clients = (uint16_t)(population > UINT16_MAX ? UINT16_MAX : population);
        z->draining = e->draining;
    }
}

static void send_plain(directory *d, net_address to, uint8_t type, const void *payload, size_t size, double now) {
    net_connection tmp;
    net_connection_init(&tmp, d->socket, to, 0, NULL, NULL);
    net_connection_send(&tmp, type, NET_UNRELIABLE, payload, size);
    net_connection_flush(&tmp, now, false);
    net_connection_destroy(&tmp);
}

// The servers a player's party members are on, from the social service.
static int directory_party_servers(directory *d, uint64_t account, const char **out, int capacity) {
    const social_player *me = social_find(&d->people, account);
    if (!me || !me->party) return 0;
    int n = 0;
    for (int i = 0; i < SOCIAL_MAX_PLAYERS && n < capacity; ++i) {
        const social_player *p = &d->people.players[i];
        if (p->used && p->online && p->party == me->party && p->account != account) out[n++] = p->server;
    }
    return n;
}

// The live table; with `first`, that zone is moved to the front.
static void send_table(directory *d, net_address to, double now, const char *first) {
    net_zone_table table;
    directory_table(d, &table);
    if (first) {
        int i = net_zones_find(&table, first);
        if (i > 0) {
            net_zone z = table.zones[i];
            memmove(table.zones + 1, table.zones, (size_t)i * sizeof(net_zone));
            table.zones[0] = z;
        }
    }
    unsigned char payload[NET_MAX_PAYLOAD];
    cw_writer w;
    cw_writer_init_fixed(&w, payload, sizeof(payload));
    directory_table_write(&w, &table, &d->secret);
    if (cw_writer_ok(&w)) send_plain(d, to, MSG_DIR_TABLE, payload, w.size, now);
}

static directory_entry *find_entry(directory *d, const char *name) {
    for (int i = 0; i < NET_MAX_ZONES; ++i)
        if (d->entries[i].used && strcmp(d->entries[i].zone.name, name) == 0) return &d->entries[i];
    return NULL;
}

static directory_entry *find_entry_by_address(directory *d, net_address address) {
    for (int i = 0; i < NET_MAX_ZONES; ++i)
        if (d->entries[i].used && net_address_equal(d->entries[i].zone.address, address)) return &d->entries[i];
    return NULL;
}

struct directory_packet {
    directory *d;
    net_address from;
    double now;
};

static void on_message(void *user, net_connection *, uint8_t type, cw_reader payload) {
    directory_packet *p = (directory_packet *)user;
    directory *d = p->d;
    switch (type) {
    case MSG_DIR_REGISTER: {
        net_zone zone;
        uint16_t clients, capacity;
        if (!directory_register_read(&payload, &zone, &clients, &capacity, &d->secret)) {
            ++d->rejected;
            return;
        }
        if (zone.address.ip == 0) zone.address = p->from;   // publish what we see
        directory_entry *e = find_entry(d, zone.name);
        if (!e) {
            for (int i = 0; i < NET_MAX_ZONES && !e; ++i)
                if (!d->entries[i].used) e = &d->entries[i];
            if (!e) return;
            e->used = true;
            e->draining = false;
            e->pending = 0;
            if (d->verbose) {
                char address[32];
                net_address_format(zone.address, address, sizeof(address));
                fprintf(stderr, "directory: %s registered at %s%s\n", zone.name, address, zone.instance_host ? " (instances)" : "");
            }
        }
        int growth = (int)clients - (int)e->clients;
        if (growth > 0) e->pending = e->pending > growth ? e->pending - growth : 0;   // they arrived
        if (p->now - e->pending_at > DIRECTORY_PENDING_EXPIRY) e->pending = 0;      // or never will
        e->zone = zone;
        e->observed = p->from;
        e->clients = clients;
        e->capacity = capacity;
        e->last_seen = p->now;
        ++d->registrations;
        send_table(d, p->from, p->now, NULL);
        break;
    }
    case MSG_DIR_QUERY: {
        directory_query q;
        if (!directory_query_read(&payload, &q)) return;
        if (!net_limiter_allow(&d->queries_by_address, p->from, p->now)) return;   // a few a second each
        ++d->queries;
        // Who is asking, when it matters (a token is wanted): the platform's word.
        account_identity identity = {};
        bool vouched = false;
        if (q.has_identity) {
            vouched = accounts_resolve(q.platform, (const char *)q.proof, q.proof_size, q.name, &identity);
            if (!vouched) ++d->identities_refused;
            else q.account = identity.account;   // their party's shard is theirs, not whoever they claim
        }
        // An introduction: to the shard chosen for the place asked for (the player's
        // party's shard, their region's, the fullest with room: zones.h), or to the
        // server at the address given (a redirect's destination). The table goes
        // back with the chosen shard first, so the asker need not choose again.
        directory_entry *e = NULL;
        if (q.introduce) e = find_entry_by_address(d, q.introduce_to);
        else if (q.place[0]) {
            net_zone_table table;
            directory_table(d, &table);   // populations include the players on their way
            const char *with[SOCIAL_PARTY_SIZE];
            int with_count = directory_party_servers(d, q.account, with, SOCIAL_PARTY_SIZE);
            int chosen = net_zones_choose(&table, q.place, q.region, with, with_count);
            if (chosen >= 0) {
                e = find_entry(d, table.zones[chosen].name);
                if (e) {
                    ++e->pending;
                    e->pending_at = p->now;
                }
                ++d->assignments;
                // Every shard of the place at or past its target: the moment to start
                // another. An orchestrator would watch for this; here it is a log line.
                const net_zone *z = &table.zones[chosen];
                if (z->target && z->clients >= z->target) {
                    ++d->saturated;
                    if (d->verbose && p->now - d->last_saturation_notice > 10.0) {
                        d->last_saturation_notice = p->now;
                        fprintf(stderr, "directory: every shard of %s%s%s is at its target; time to start another\n", q.place,
                                q.region[0] ? " for " : "", q.region);
                    }
                }
            }
        }
        if (!e && q.zone[0]) e = find_entry(d, q.zone);   // a token for where they were, after a lost connection
        send_table(d, p->from, p->now, e ? e->zone.name : NULL);
        // The token: the identity and the zone, signed. Without a verified identity,
        // a refusal, so the client knows not to try.
        if (q.has_identity && (q.place[0] || q.zone[0])) {
            unsigned char answer[NET_TOKEN_SIZE + 16];
            cw_writer w;
            cw_writer_init_fixed(&w, answer, sizeof(answer));
            if (vouched && e) {
                net_connect_token token = {};
                token.account = identity.account;
                memcpy(token.name, identity.name, sizeof(token.name));
                memcpy(token.platform, identity.platform, sizeof(token.platform));
                memcpy(token.zone, e->zone.name, sizeof(token.zone));
                token.expires = net_wall_seconds() + NET_TOKEN_LIFETIME;
                crypto_random(&token.nonce, sizeof(token.nonce));
                directory_token_write(&w, &token, &d->secret);
                ++d->tokens_issued;
            } else {
                directory_token_write(&w, NULL, &d->secret);
            }
            send_plain(d, p->from, MSG_DIR_TOKEN, answer, w.size, p->now);
        }
        if (e) {
            unsigned char punch[32];
            cw_writer w;
            cw_writer_init_fixed(&w, punch, sizeof(punch));
            directory_punch_write(&w, p->from, &d->secret);
            send_plain(d, e->observed, MSG_DIR_PUNCH, punch, w.size, p->now);
            ++d->introductions;
        }
        break;
    }
    case MSG_DIR_DRAIN: {
        char zone[NET_ZONE_NAME];
        if (!directory_drain_read(&payload, zone, &d->secret)) {
            ++d->rejected;
            return;
        }
        directory_entry *e = find_entry(d, zone);
        if (e && !e->draining) {
            e->draining = true;
            if (d->verbose) fprintf(stderr, "directory: %s is draining\n", zone);
        }
        break;
    }
    case MSG_SOC_PRESENCE:
    case MSG_SOC_PARTY_OP:
        social_handle(&d->people, p->from, type, payload, p->now);
        break;
    default:
        break;
    }
}

void directory_update(directory *d, double now) {
    social_update(&d->people, now);
    for (;;) {
        net_address from;
        int size = net_socket_recv(d->socket, &from, d->scratch, sizeof(d->scratch));
        if (size <= 0) break;
        uint32_t session;
        if (!net_packet_peek(d->scratch, (size_t)size, &session) || session != 0) continue;
        directory_packet p = {d, from, now};
        net_connection tmp;
        net_connection_init(&tmp, d->socket, from, 0, on_message, &p);
        net_connection_receive(&tmp, d->scratch, (size_t)size, now);
        net_connection_destroy(&tmp);
    }
    for (int i = 0; i < NET_MAX_ZONES; ++i) {
        directory_entry *e = &d->entries[i];
        if (e->used && now - e->last_seen > d->expiry) {
            if (d->verbose) fprintf(stderr, "directory: %s expired\n", e->zone.name);
            e->used = false;
        }
    }
}
