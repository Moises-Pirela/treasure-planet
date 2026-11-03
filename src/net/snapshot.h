// Snapshots: where every player is, at one server tick, as the server tells one
// client. Only players are in them; ships are a function of universe time and
// everything else is static and arrived with the baseline.
//
// An entry is quantised (millimetres, millimetres per second, 16-bit angles) and a
// snapshot is encoded as a delta against a base: the last snapshot the client said
// it had. Each player costs one flag byte plus whatever changed, which for someone
// standing still is two bytes; a first snapshot, or one after the base was lost,
// is encoded against nothing and costs the full fields. Entries are sorted by id.
//
// Positions of players aboard a ship are relative to the ship, so a client can
// place them on the deck it is predicting rather than where the deck was.
#ifndef TP_SNAPSHOT_H
#define TP_SNAPSHOT_H

#include "../game/game.h"
#include "net.h"

#define SNAPSHOT_RING 32             // ticks of history each side keeps for delta coding
#define SNAPSHOT_POSITION_SCALE 1000.0f     // units per metre
#define SNAPSHOT_VELOCITY_SCALE 1000.0f
#define SNAPSHOT_YAW_SCALE (65536.0f / (2.0f * TP_PI))
#define SNAPSHOT_PITCH_SCALE (32767.0f / TP_PI)

enum snapshot_flags { SNAP_GROUNDED = 1 };

struct snapshot_entry {
    cw_entity_id id;
    cw_entity_id aboard;         // the ship position is relative to, or CW_INVALID_ENTITY
    int32_t position[3], velocity[3];
    uint16_t yaw;
    int16_t pitch;
    uint8_t flags;
    uint16_t hits_made, hits_taken;
    float priority;              // sender only: distance to the receiving player
};

struct snapshot {
    uint32_t tick;
    uint32_t last_input_seq;     // newest input of the receiving client applied by this tick
    int8_t input_lead;           // inputs of the client still queued after it: its clock runs
                                 // ahead by this much, and it steers to keep that at the target
    snapshot_entry *entries;     // grows as needed (snapshot_reserve); freed by snapshot_free
    uint16_t count, capacity;
};

void snapshot_reserve(snapshot *s, uint16_t count);
void snapshot_free(snapshot *s);

// A ring of the last SNAPSHOT_RING snapshots, found by tick. Memory grows to the
// largest snapshot seen, so a client that hears about few players costs little.
struct snapshot_ring {
    snapshot items[SNAPSHOT_RING];
    int next;
};

void snapshot_ring_init(snapshot_ring *ring);
void snapshot_ring_destroy(snapshot_ring *ring);
snapshot *snapshot_ring_find(snapshot_ring *ring, uint32_t tick);
// The slot to fill for a new snapshot, with room for `count` entries; it is part of
// the ring once `tick` is set.
snapshot *snapshot_ring_push(snapshot_ring *ring, uint16_t count);

// Quantisation.
snapshot_entry snapshot_entry_make(cw_entity_id id, cw_entity_id aboard, v3 position, v3 velocity, float yaw,
                                   float pitch, bool grounded, uint16_t hits_made, uint16_t hits_taken);
v3 snapshot_position(const snapshot_entry *e);   // relative to the ship when aboard
v3 snapshot_velocity(const snapshot_entry *e);
float snapshot_yaw(const snapshot_entry *e);
float snapshot_pitch(const snapshot_entry *e);
const snapshot_entry *snapshot_find(const snapshot *s, cw_entity_id id);
void snapshot_sort(snapshot *s);

// Header: u32 tick, u32 base tick (0: none), u32 last input seq, i8 input lead, u16 count. Then
// `count` entries delta-coded against `base`, which may be NULL. The writer must
// be fixed-size: on overflow the caller drops entries and tries again.
bool snapshot_encode(const snapshot *s, const snapshot *base, cw_writer *w);
// Reads the header only, so the caller can look the base up.
bool snapshot_peek(cw_reader *r, uint32_t *tick, uint32_t *base_tick);
// Decodes into `out` (grown as needed) using `base`, which must be the snapshot the
// header named, or NULL when it named none.
bool snapshot_decode(cw_reader *r, const snapshot *base, snapshot *out);

#endif
