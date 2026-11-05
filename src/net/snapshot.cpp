#include "snapshot.h"
#include <stdlib.h>
#include <string.h>

// Per-entry change bits, after the id.
enum {
    CHANGE_ABOARD = 1, CHANGE_POSITION = 2, CHANGE_VELOCITY = 4, CHANGE_ANGLES = 8, CHANGE_FLAGS = 16,
    CHANGE_NEW = 32,             // not in the base: every field follows, coded against zero
    CHANGE_STATS = 64,
    CHANGE_ROTATION = 128,       // a craft's orientation and spin
    CHANGE_PILOT = 256,
    CHANGE_ENGINES = 512,        // a craft's throttles and its count towards rest
    CHANGE_ALL = 1023
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

static int32_t bits_of(float f) {
    int32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}
static float float_of(int32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

snapshot_entry snapshot_entry_craft(cw_entity_id id, v3 position, const Craft *craft) {
    snapshot_entry e = {};
    e.id = id;
    e.aboard = CW_INVALID_ENTITY;
    e.pilot = craft->pilot;
    e.dock = craft->dock;
    e.docking = (uint8_t)craft->docking;
    e.flags = (uint8_t)(SNAP_CRAFT | (craft->resting ? SNAP_RESTING : 0) | (craft->assist ? SNAP_ASSIST : 0)
                        | (craft->mode == CRAFT_LANDING ? SNAP_LANDING : 0));
    for (int m = 0; m < CRAFT_MAX_MASTS; ++m) {
        const craft_sail *sail = &craft->sails[m];
        bool rigged = m < craft->masts;
        e.sail[m] = rigged ? bits_of(sail->set) : 0;
        e.brace[m] = rigged ? bits_of(sail->brace) : 0;
        e.hand[m] = rigged ? sail->hand : CW_INVALID_ENTITY;
        float fill = !rigged ? 0.0f : sail->fill < -1.0f ? -1.0f : sail->fill > 1.0f ? 1.0f : sail->fill;
        e.fill[m] = (int8_t)(int)(fill * 127.0f + (fill < 0.0f ? -0.5f : 0.5f));
    }
    for (int k = 0; k < CRAFT_GROUPS; ++k) {
        float level = craft->throttle[k] < 0.0f ? 0.0f : craft->throttle[k] > 1.0f ? 1.0f : craft->throttle[k];
        e.throttle[k] = (uint8_t)(level * 255.0f + 0.5f);
    }
    e.quiet = (uint8_t)(craft->quiet_ticks < 0 ? 0 : craft->quiet_ticks > 255 ? 255 : craft->quiet_ticks);
    v3 velocity = craft->velocity, spin = craft->spin;
    quat orientation = craft->orientation;
    const float p[3] = {position.x, position.y, position.z}, v[3] = {velocity.x, velocity.y, velocity.z};
    const float q[4] = {orientation.x, orientation.y, orientation.z, orientation.w}, w[3] = {spin.x, spin.y, spin.z};
    for (int k = 0; k < 3; ++k) {
        e.position[k] = bits_of(p[k]);
        e.velocity[k] = bits_of(v[k]);
        e.spin[k] = bits_of(w[k]);
    }
    for (int k = 0; k < 4; ++k) e.orientation[k] = bits_of(q[k]);
    return e;
}

void snapshot_craft_state(const snapshot_entry *e, v3 *position, v3 *velocity, quat *orientation, v3 *spin) {
    *position = v3_make(float_of(e->position[0]), float_of(e->position[1]), float_of(e->position[2]));
    *velocity = v3_make(float_of(e->velocity[0]), float_of(e->velocity[1]), float_of(e->velocity[2]));
    *orientation = quat_make(float_of(e->orientation[0]), float_of(e->orientation[1]), float_of(e->orientation[2]),
                             float_of(e->orientation[3]));
    *spin = v3_make(float_of(e->spin[0]), float_of(e->spin[1]), float_of(e->spin[2]));
}

void snapshot_craft_apply(const snapshot_entry *e, Craft *craft, v3 *position) {
    snapshot_craft_state(e, position, &craft->velocity, &craft->orientation, &craft->spin);
    craft->orientation = quat_normalize(craft->orientation);
    craft->pilot = e->pilot;
    craft->dock = e->dock;
    craft->docking = e->docking <= DOCK_DOCKED ? (int)e->docking : (int)DOCK_NONE;
    craft->resting = (e->flags & SNAP_RESTING) != 0;
    craft->assist = (e->flags & SNAP_ASSIST) != 0;
    craft->mode = (e->flags & SNAP_LANDING) ? CRAFT_LANDING : CRAFT_FLIGHT;
    craft->quiet_ticks = e->quiet;
    for (int m = 0; m < CRAFT_MAX_MASTS; ++m) {
        craft_sail *sail = &craft->sails[m];
        sail->set = float_of(e->sail[m]);
        sail->brace = float_of(e->brace[m]);
        sail->hand = e->hand[m];
        sail->fill = (float)e->fill[m] / 127.0f;
    }
    for (int k = 0; k < CRAFT_GROUPS; ++k) craft->throttle[k] = (float)e->throttle[k] / 255.0f;
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
        uint32_t change = 0;
        if (!b) {
            b = &zero;
            change = CHANGE_NEW | CHANGE_ABOARD | CHANGE_POSITION | CHANGE_VELOCITY | CHANGE_ANGLES | CHANGE_FLAGS | CHANGE_STATS;
            if (e->flags & SNAP_CRAFT) change |= CHANGE_ROTATION | CHANGE_PILOT | CHANGE_ENGINES;
        } else {
            if (e->aboard != b->aboard) change |= CHANGE_ABOARD;
            if (memcmp(e->position, b->position, sizeof(e->position)) != 0) change |= CHANGE_POSITION;
            if (memcmp(e->velocity, b->velocity, sizeof(e->velocity)) != 0) change |= CHANGE_VELOCITY;
            if (e->yaw != b->yaw || e->pitch != b->pitch) change |= CHANGE_ANGLES;
            if (e->flags != b->flags) change |= CHANGE_FLAGS;
            if (e->hits_made != b->hits_made || e->hits_taken != b->hits_taken) change |= CHANGE_STATS;
            if (memcmp(e->orientation, b->orientation, sizeof(e->orientation)) != 0 || memcmp(e->spin, b->spin, sizeof(e->spin)) != 0)
                change |= CHANGE_ROTATION;
            if (e->pilot != b->pilot || memcmp(e->hand, b->hand, sizeof(e->hand)) != 0 || e->dock != b->dock || e->docking != b->docking)
                change |= CHANGE_PILOT;
            if (memcmp(e->throttle, b->throttle, sizeof(e->throttle)) != 0 || e->quiet != b->quiet
                || memcmp(e->sail, b->sail, sizeof(e->sail)) != 0 || memcmp(e->fill, b->fill, sizeof(e->fill)) != 0
                || memcmp(e->brace, b->brace, sizeof(e->brace)) != 0)
                change |= CHANGE_ENGINES;
        }
        write_id(w, e->id);
        net_write_uvarint(w, change);
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
        if (change & CHANGE_ROTATION) {
            for (int k = 0; k < 4; ++k) net_write_svarint(w, (int64_t)e->orientation[k] - b->orientation[k]);
            for (int k = 0; k < 3; ++k) net_write_svarint(w, (int64_t)e->spin[k] - b->spin[k]);
        }
        if (change & CHANGE_PILOT) {
            write_id(w, e->pilot);
            for (int m = 0; m < CRAFT_MAX_MASTS; ++m) write_id(w, e->hand[m]);
            write_id(w, e->dock);
            cw_writer_u8(w, e->docking);
        }
        if (change & CHANGE_ENGINES) {
            cw_writer_bytes(w, e->throttle, sizeof(e->throttle));
            cw_writer_u8(w, e->quiet);
            for (int m = 0; m < CRAFT_MAX_MASTS; ++m) {
                net_write_svarint(w, (int64_t)e->sail[m] - b->sail[m]);
                net_write_svarint(w, (int64_t)e->brace[m] - b->brace[m]);
                cw_writer_i8(w, e->fill[m]);
            }
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
        uint64_t change = 0;
        if (!read_id(r, &id) || !net_read_uvarint(r, &change) || change > CHANGE_ALL) return false;
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
        if (change & CHANGE_ROTATION) {
            for (int k = 0; k < 7; ++k) {
                int64_t d = 0;
                if (!net_read_svarint(r, &d)) return false;
                if (k < 4) e->orientation[k] = (int32_t)(b->orientation[k] + d);
                else e->spin[k - 4] = (int32_t)(b->spin[k - 4] + d);
            }
        }
        if (change & CHANGE_PILOT) {
            if (!read_id(r, &e->pilot)) return false;
            for (int m = 0; m < CRAFT_MAX_MASTS; ++m)
                if (!read_id(r, &e->hand[m])) return false;
            if (!read_id(r, &e->dock)) return false;
            cw_reader_u8(r, &e->docking);
        }
        if (change & CHANGE_ENGINES) {
            cw_reader_bytes(r, e->throttle, sizeof(e->throttle));
            cw_reader_u8(r, &e->quiet);
            for (int m = 0; m < CRAFT_MAX_MASTS; ++m) {
                int64_t d = 0;
                if (!net_read_svarint(r, &d)) return false;
                e->sail[m] = (int32_t)(b->sail[m] + d);
                if (!net_read_svarint(r, &d)) return false;
                e->brace[m] = (int32_t)(b->brace[m] + d);
                cw_reader_i8(r, &e->fill[m]);
            }
        }
        if (!cw_reader_ok(r)) return false;
        ++out->count;
    }
    return true;
}
