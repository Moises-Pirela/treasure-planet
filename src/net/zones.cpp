#include "zones.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVELOPMENT_SECRET "treasure-planet-development"

void net_zones_init(net_zone_table *t) {
    memset(t, 0, sizeof(*t));
    net_zones_set_secret(t, DEVELOPMENT_SECRET);
}

void net_zones_set_secret(net_zone_table *t, const char *secret) {
    strncpy(t->secret, secret ? secret : "", NET_SECRET_CAPACITY - 1);
    t->secret[NET_SECRET_CAPACITY - 1] = '\0';
}

int net_zones_add(net_zone_table *t, const char *name, net_address address) {
    if (t->count == NET_MAX_ZONES) return -1;
    net_zone *z = &t->zones[t->count];
    memset(z, 0, sizeof(*z));
    strncpy(z->name, name, NET_ZONE_NAME - 1);
    z->address = address;
    return t->count++;
}

bool net_zone_add_place(net_zone *zone, const char *place) {
    if (zone->place_count == NET_ZONE_PLACES) return false;
    strncpy(zone->places[zone->place_count++], place, NET_ZONE_NAME - 1);
    return true;
}

int net_zones_find(const net_zone_table *t, const char *name) {
    for (int i = 0; i < t->count; ++i)
        if (strcmp(t->zones[i].name, name) == 0) return i;
    return -1;
}

int net_zones_owner(const net_zone_table *t, const char *place) {
    for (int i = 0; i < t->count; ++i)
        for (int k = 0; k < t->zones[i].place_count; ++k)
            if (strcmp(t->zones[i].places[k], place) == 0) return i;
    return -1;
}

// The next whitespace-delimited word of a line, NUL-terminated in place; NULL at the end.
static char *next_word(char **cursor) {
    char *c = *cursor;
    while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') ++c;
    if (!*c) {
        *cursor = c;
        return NULL;
    }
    char *word = c;
    while (*c && *c != ' ' && *c != '\t' && *c != '\r' && *c != '\n') ++c;
    if (*c) *c++ = '\0';
    *cursor = c;
    return word;
}

bool net_zones_load(net_zone_table *t, const char *path, char *error, size_t error_capacity) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(error, error_capacity, "cannot open %s", path);
        return false;
    }
    char line[512];
    int number = 0;
    while (fgets(line, sizeof(line), f)) {
        ++number;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *cursor = line;
        char *word = next_word(&cursor);
        if (!word) continue;
        if (strcmp(word, "secret") == 0) {
            char *secret = next_word(&cursor);
            if (!secret) {
                snprintf(error, error_capacity, "%s:%d: secret needs a value", path, number);
                fclose(f);
                return false;
            }
            net_zones_set_secret(t, secret);
        } else if (strcmp(word, "zone") == 0) {
            char *name = next_word(&cursor);
            char *address_text = name ? next_word(&cursor) : NULL;
            net_address address;
            if (!name || !address_text || !net_address_parse(address_text, NET_DEFAULT_PORT, &address)) {
                snprintf(error, error_capacity, "%s:%d: expected zone <name> <host:port> <place>...", path, number);
                fclose(f);
                return false;
            }
            int index = net_zones_add(t, name, address);
            if (index < 0) {
                snprintf(error, error_capacity, "%s:%d: more than %d zones", path, number, NET_MAX_ZONES);
                fclose(f);
                return false;
            }
            for (;;) {
                char *place = next_word(&cursor);
                if (!place) break;
                net_zone_add_place(&t->zones[index], place);
            }
        } else {
            snprintf(error, error_capacity, "%s:%d: unknown directive '%s'", path, number, word);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// SipHash-2-4 (Aumasson, Bernstein), the reference algorithm.
// ---------------------------------------------------------------------------
static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
#define SIPROUND \
    do { \
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32); \
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2; \
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0; \
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32); \
    } while (0)

uint64_t net_siphash(const void *data, size_t size, uint64_t k0, uint64_t k1) {
    const unsigned char *in = (const unsigned char *)data;
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    size_t blocks = size & ~(size_t)7;
    for (size_t i = 0; i < blocks; i += 8) {
        uint64_t m = 0;
        for (int k = 7; k >= 0; --k) m = (m << 8) | in[i + (size_t)k];
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t last = (uint64_t)(size & 0xff) << 56;
    for (size_t i = blocks; i < size; ++i) last |= (uint64_t)in[i] << (8 * (i - blocks));
    v3 ^= last;
    SIPROUND;
    SIPROUND;
    v0 ^= last;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

// Two 64-bit halves of a key from the secret: FNV-1a with two different offsets.
static void secret_keys(const net_zone_table *zones, uint64_t *k0, uint64_t *k1) {
    uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    for (const char *c = zones->secret; *c; ++c) {
        a = (a ^ (unsigned char)*c) * 0x100000001b3ull;
        b = (b ^ (unsigned char)*c) * 0x100000001b3ull;
    }
    *k0 = a;
    *k1 = b;
}

static void write_v3(cw_writer *w, v3 v) {
    cw_writer_f32(w, v.x);
    cw_writer_f32(w, v.y);
    cw_writer_f32(w, v.z);
}

void net_ticket_write(cw_writer *w, const net_ticket *t, const net_zone_table *zones) {
    unsigned char body[NET_TICKET_SIZE];
    cw_writer b;
    cw_writer_init_fixed(&b, body, sizeof(body));
    cw_writer_u64(&b, t->account);
    write_v3(&b, t->position);
    write_v3(&b, t->velocity);
    cw_writer_f32(&b, t->yaw);
    cw_writer_f32(&b, t->pitch);
    cw_writer_f64(&b, t->issued);
    uint64_t k0, k1;
    secret_keys(zones, &k0, &k1);
    cw_writer_bytes(w, body, b.size);
    cw_writer_u64(w, net_siphash(body, b.size, k0, k1));
}

bool net_ticket_read(cw_reader *r, net_ticket *t, const net_zone_table *zones) {
    const size_t body_size = 8 + 12 + 12 + 4 + 4 + 8;
    const unsigned char *body = (const unsigned char *)cw_reader_view(r, body_size);
    uint64_t signature = 0;
    if (!body || !cw_reader_u64(r, &signature)) return false;
    uint64_t k0, k1;
    secret_keys(zones, &k0, &k1);
    if (net_siphash(body, body_size, k0, k1) != signature) return false;
    cw_reader b = cw_reader_make(body, body_size);
    cw_reader_u64(&b, &t->account);
    cw_reader_f32(&b, &t->position.x);
    cw_reader_f32(&b, &t->position.y);
    cw_reader_f32(&b, &t->position.z);
    cw_reader_f32(&b, &t->velocity.x);
    cw_reader_f32(&b, &t->velocity.y);
    cw_reader_f32(&b, &t->velocity.z);
    cw_reader_f32(&b, &t->yaw);
    cw_reader_f32(&b, &t->pitch);
    cw_reader_f64(&b, &t->issued);
    t->signature = signature;
    return cw_reader_ok(&b);
}
