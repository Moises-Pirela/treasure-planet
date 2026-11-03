#include "snapshot.h"
#include <stdlib.h>
#include <string.h>

// Per-entry change bits, after the id.
enum {
    CHANGE_ABOARD = 1, CHANGE_POSITION = 2, CHANGE_VELOCITY = 4, CHANGE_ANGLES = 8, CHANGE_FLAGS = 16,
    CHANGE_NEW = 32,             // not in the base: every field follows, coded against zero
    CHANGE_STATS = 64
};

void snapshot_reserve(snapshot *s, uint16_t count) {
    if (count <= s->capacity) return;
    uint16_t capacity = s->capacity ? s->capacity : 8;
    while (capacity < count) capacity = (uint16_t)(capacity * 2 > UINT16_MAX ? UINT16_MAX : capacity * 2);
    s->entries = (snapshot_entry *)realloc(s->entries, capacity * sizeof(snapshot_entry));
    s->capacity = capacity;
}

void snapshot_free(snapshot *s) {
    free(s->entries);
    memset(s, 0, sizeof(*s));
}

void snapshot_ring_init(snapshot_ring *ring) { memset(ring, 0, sizeof(*ring)); }

void snapshot_ring_destroy(snapshot_ring *ring) {
    for (int i = 0; i < SNAPSHOT_RING; ++i) snapshot_free(&ring->items[i]);
    memset(ring, 0, sizeof(*ring));
}

snapshot *snapshot_ring_find(snapshot_ring *ring, uint32_t tick) {
    if (tick == 0) return NULL;
    for (int i = 0; i < SNAPSHOT_RING; ++i)
        if (ring->items[i].tick == tick) return &ring->items[i];
    return NULL;
}

snapshot *snapshot_ring_push(snapshot_ring *ring, uint16_t count) {
    snapshot *s = &ring->items[ring->next];
    ring->next = (ring->next + 1) % SNAPSHOT_RING;
    s->tick = 0;
    s->last_input_seq = 0;
    s->count = 0;
    snapshot_reserve(s, count);
    return s;
}

static int32_t quantize(float value, float scale) {
    float scaled = value * scale;
    return (int32_t)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

snapshot_entry snapshot_entry_make(cw_entity_id id, cw_entity_id aboard, v3 position, v3 velocity, float yaw,
                                   float pitch, bool grounded, uint16_t hits_made, uint16_t hits_taken) {
    snapshot_entry e = {};
    e.id = id;
    e.aboard = aboard;
    e.hits_made = hits_made;
    e.hits_taken = hits_taken;
    e.position[0] = quantize(position.x, SNAPSHOT_POSITION_SCALE);
    e.position[1] = quantize(position.y, SNAPSHOT_POSITION_SCALE);
    e.position[2] = quantize(position.z, SNAPSHOT_POSITION_SCALE);
    e.velocity[0] = quantize(velocity.x, SNAPSHOT_VELOCITY_SCALE);
    e.velocity[1] = quantize(velocity.y, SNAPSHOT_VELOCITY_SCALE);
    e.velocity[2] = quantize(velocity.z, SNAPSHOT_VELOCITY_SCALE);
    float turns = yaw / (2.0f * TP_PI);
    turns -= floorf(turns);
    e.yaw = (uint16_t)(int)(turns * 65536.0f + 0.5f);
    int32_t p = quantize(pitch, SNAPSHOT_PITCH_SCALE);
    e.pitch = (int16_t)(p > 32767 ? 32767 : p < -32767 ? -32767 : p);
    e.flags = grounded ? SNAP_GROUNDED : 0;
    return e;
}

v3 snapshot_position(const snapshot_entry *e) {
    return v3_make((float)e->position[0] / SNAPSHOT_POSITION_SCALE, (float)e->position[1] / SNAPSHOT_POSITION_SCALE,
                   (float)e->position[2] / SNAPSHOT_POSITION_SCALE);
}
v3 snapshot_velocity(const snapshot_entry *e) {
    return v3_make((float)e->velocity[0] / SNAPSHOT_VELOCITY_SCALE, (float)e->velocity[1] / SNAPSHOT_VELOCITY_SCALE,
                   (float)e->velocity[2] / SNAPSHOT_VELOCITY_SCALE);
}
float snapshot_yaw(const snapshot_entry *e) { return (float)e->yaw / SNAPSHOT_YAW_SCALE; }
float snapshot_pitch(const snapshot_entry *e) { return (float)e->pitch / SNAPSHOT_PITCH_SCALE; }

static int compare_entries(const void *a, const void *b) {
    cw_entity_id x = ((const snapshot_entry *)a)->id, y = ((const snapshot_entry *)b)->id;
    return x < y ? -1 : x > y;
}
void snapshot_sort(snapshot *s) { if (s->count > 1) qsort(s->entries, s->count, sizeof(snapshot_entry), compare_entries); }

const snapshot_entry *snapshot_find(const snapshot *s, cw_entity_id id) {
    if (!s) return NULL;
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s->entries[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < s->count && s->entries[lo].id == id ? &s->entries[lo] : NULL;
}

static void write_id(cw_writer *w, cw_entity_id id) { net_write_uvarint(w, (uint64_t)(id + 1)); }   // -1 becomes 0
static bool read_id(cw_reader *r, cw_entity_id *id) {
    uint64_t raw;
    if (!net_read_uvarint(r, &raw) || raw > (uint64_t)INT64_MAX) return false;
    *id = (cw_entity_id)raw - 1;
    return true;
}

bool snapshot_encode(const snapshot *s, const snapshot *base, cw_writer *w) {
    static const snapshot_entry zero = {};
    cw_writer_u32(w, s->tick);
    cw_writer_u32(w, base ? base->tick : 0);
    cw_writer_u32(w, s->last_input_seq);
    cw_writer_i8(w, s->input_lead);
    cw_writer_u16(w, s->count);
    for (uint16_t i = 0; i < s->count; ++i) {
        const snapshot_entry *e = &s->entries[i];
        const snapshot_entry *b = snapshot_find(base, e->id);
        uint8_t change = 0;
        if (!b) {
            b = &zero;
            change = CHANGE_NEW | CHANGE_ABOARD | CHANGE_POSITION | CHANGE_VELOCITY | CHANGE_ANGLES | CHANGE_FLAGS | CHANGE_STATS;
        } else {
            if (e->aboard != b->aboard) change |= CHANGE_ABOARD;
            if (memcmp(e->position, b->position, sizeof(e->position)) != 0) change |= CHANGE_POSITION;
            if (memcmp(e->velocity, b->velocity, sizeof(e->velocity)) != 0) change |= CHANGE_VELOCITY;
            if (e->yaw != b->yaw || e->pitch != b->pitch) change |= CHANGE_ANGLES;
            if (e->flags != b->flags) change |= CHANGE_FLAGS;
            if (e->hits_made != b->hits_made || e->hits_taken != b->hits_taken) change |= CHANGE_STATS;
        }
        write_id(w, e->id);
        cw_writer_u8(w, change);
        if (change & CHANGE_ABOARD) write_id(w, e->aboard);
        if (change & CHANGE_POSITION)
            for (int k = 0; k < 3; ++k) net_write_svarint(w, (int64_t)e->position[k] - b->position[k]);
        if (change & CHANGE_VELOCITY)
            for (int k = 0; k < 3; ++k) net_write_svarint(w, (int64_t)e->velocity[k] - b->velocity[k]);
        if (change & CHANGE_ANGLES) {
            net_write_svarint(w, (int16_t)(uint16_t)(e->yaw - b->yaw));   // shortest way round
            net_write_svarint(w, (int64_t)e->pitch - b->pitch);
        }
        if (change & CHANGE_FLAGS) cw_writer_u8(w, e->flags);
        if (change & CHANGE_STATS) {
            net_write_uvarint(w, e->hits_made);
            net_write_uvarint(w, e->hits_taken);
        }
    }
    return cw_writer_ok(w);
}

bool snapshot_peek(cw_reader *r, uint32_t *tick, uint32_t *base_tick) {
    cw_reader copy = *r;
    cw_reader_u32(&copy, tick);
    cw_reader_u32(&copy, base_tick);
    return cw_reader_ok(&copy);
}

bool snapshot_decode(cw_reader *r, const snapshot *base, snapshot *out) {
    static const snapshot_entry zero = {};
    uint32_t base_tick = 0;
    uint16_t count = 0;
    cw_reader_u32(r, &out->tick);
    cw_reader_u32(r, &base_tick);
    cw_reader_u32(r, &out->last_input_seq);
    cw_reader_i8(r, &out->input_lead);
    cw_reader_u16(r, &count);
    if (!cw_reader_ok(r)) return false;
    if ((base == NULL) != (base_tick == 0) || (base && base->tick != base_tick)) return false;
    snapshot_reserve(out, count);
    out->count = 0;
    for (uint16_t i = 0; i < count; ++i) {
        snapshot_entry *e = &out->entries[i];
        cw_entity_id id = CW_INVALID_ENTITY;
        uint8_t change = 0;
        if (!read_id(r, &id)) return false;
        cw_reader_u8(r, &change);
        const snapshot_entry *b = (change & CHANGE_NEW) ? NULL : snapshot_find(base, id);
        if (!b) {
            if (!(change & CHANGE_NEW)) return false;   // the base lacks an entry the sender coded against
            b = &zero;
        }
        *e = *b;   // unchanged fields carry over; the rest are patched below
        e->id = id;
        if (change & CHANGE_ABOARD && !read_id(r, &e->aboard)) return false;
        if (change & CHANGE_POSITION)
            for (int k = 0; k < 3; ++k) {
                int64_t d = 0;
                if (!net_read_svarint(r, &d)) return false;
                e->position[k] = (int32_t)(b->position[k] + d);
            }
        if (change & CHANGE_VELOCITY)
            for (int k = 0; k < 3; ++k) {
                int64_t d = 0;
                if (!net_read_svarint(r, &d)) return false;
                e->velocity[k] = (int32_t)(b->velocity[k] + d);
            }
        if (change & CHANGE_ANGLES) {
            int64_t dyaw = 0, dpitch = 0;
            if (!net_read_svarint(r, &dyaw) || !net_read_svarint(r, &dpitch)) return false;
            e->yaw = (uint16_t)(b->yaw + (uint16_t)dyaw);
            e->pitch = (int16_t)(b->pitch + dpitch);
        }
        if (change & CHANGE_FLAGS) cw_reader_u8(r, &e->flags);
        if (change & CHANGE_STATS) {
            uint64_t made = 0, taken = 0;
            if (!net_read_uvarint(r, &made) || !net_read_uvarint(r, &taken)) return false;
            e->hits_made = (uint16_t)made;
            e->hits_taken = (uint16_t)taken;
        }
        if (!cw_reader_ok(r)) return false;
        ++out->count;
    }
    return true;
}
