// Dungeons: one per planet, generated from the name so every server that builds
// "Verdant Depths" builds the same one. A dungeon world holds nothing else: no
// ships, no planets, and its own coordinate space with the first room at the origin.
//
// The layout is a string of rooms heading north (-z), each joined to the next by a
// corridor, with walls all round and a few pillars and crates to walk around. The
// spawn is the middle of the first room; the exit portal stands at the far end of
// the last, and walking into it is walking out of the dungeon.
#include "game.h"
#include <stdio.h>
#include <string.h>

#define DUNGEON_WALL_HEIGHT 4.0f
#define DUNGEON_WALL_THICK 0.5f
#define DUNGEON_CORRIDOR_HALF 1.5f
#define DUNGEON_FLOOR_HALF 0.5f

void game_dungeon_name(const char *place, char *out, size_t capacity) { snprintf(out, capacity, "%s Depths", place); }

struct dungeon_random { uint32_t state; };
static uint32_t next_random(dungeon_random *r) {
    r->state = r->state * 1664525u + 1013904223u;
    return r->state >> 8;
}
static float random_range(dungeon_random *r, float lo, float hi) {
    return lo + (hi - lo) * (float)next_random(r) / 16777216.0f;
}

static cw_entity_id block(game *g, v3 center, v3 half, v3 color) {
    cw_world *w = &g->world;
    cw_entity_id id = cw_world_create_entity(w);
    Transform t = {center, center};
    Solid solid = {half};
    Visual visual = {MESH_CUBE, half, color};
    cw_comp_set(w, id, g->c_transform, &t, sizeof(t));
    cw_comp_set(w, id, g->c_solid, &solid, sizeof(solid));
    cw_comp_set(w, id, g->c_visual, &visual, sizeof(visual));
    return id;
}

// A wall along x (axis 0) or z (axis 2) with a gap of `gap_half` around `gap_at`
// (no gap when gap_half is 0).
static void wall(game *g, int axis, float at, float from, float to, float gap_at, float gap_half, v3 color) {
    const float h = DUNGEON_WALL_HEIGHT * 0.5f, t = DUNGEON_WALL_THICK;
    float starts[2] = {from, gap_at + gap_half}, ends[2] = {gap_half > 0.0f ? gap_at - gap_half : to, to};
    for (int i = 0; i < (gap_half > 0.0f ? 2 : 1); ++i) {
        if (ends[i] - starts[i] < 0.01f) continue;
        float mid = (starts[i] + ends[i]) * 0.5f, half = (ends[i] - starts[i]) * 0.5f;
        if (axis == 0) block(g, v3_make(mid, h, at), v3_make(half, h, t), color);
        else block(g, v3_make(at, h, mid), v3_make(t, h, half), color);
    }
}

struct dungeon_room {
    float x, half_x, half_z;     // centre line and size
    float near_z, far_z;         // the +z and -z walls
    float corridor_in, corridor_out;     // x of the corridor from the previous room / to the next
    float corridor_length;       // of the corridor to the next room
};

void game_build_dungeon(game *g, const char *name, const char *return_place, v3 return_position, uint32_t loot_seed) {
    strncpy(g->dungeon, name, GAME_NAME_CAPACITY - 1);
    dungeon_random random = {2166136261u};
    for (const char *c = name; *c; ++c) random.state = (random.state ^ (unsigned char)*c) * 16777619u;
    v3 stone = v3_make(0.32f + random_range(&random, 0, 0.1f), 0.3f, 0.36f + random_range(&random, 0, 0.1f));
    v3 floor_color = v3_scale(stone, 0.7f), pillar_color = v3_scale(stone, 1.25f);
    v3 crate_color = v3_make(0.45f, 0.32f, 0.2f);

    // The plan first: rooms heading north, each overlapping the previous in x by
    // enough for a straight corridor between their centre lines.
    dungeon_room rooms[GAME_MAX_PLACES];
    memset(rooms, 0, sizeof(rooms));
    int count = 4 + (int)(next_random(&random) % 3);
    float z = 0.0f;
    for (int i = 0; i < count; ++i) {
        dungeon_room *r = &rooms[i];
        r->half_x = random_range(&random, 5.0f, 8.0f);
        r->half_z = random_range(&random, 5.0f, 8.0f);
        r->x = i == 0 ? 0.0f : rooms[i - 1].x + random_range(&random, -3.0f, 3.0f);
        if (i > 0) {
            float limit = rooms[i - 1].half_x + r->half_x - DUNGEON_CORRIDOR_HALF - 1.0f;
            if (r->x - rooms[i - 1].x > limit) r->x = rooms[i - 1].x + limit;
            if (rooms[i - 1].x - r->x > limit) r->x = rooms[i - 1].x - limit;
            r->corridor_in = rooms[i - 1].corridor_out = (r->x + rooms[i - 1].x) * 0.5f;
        }
        r->near_z = z;
        r->far_z = z - 2.0f * r->half_z;
        r->corridor_length = random_range(&random, 6.0f, 12.0f);
        z = r->far_z - r->corridor_length;
    }

    g->dungeon_rooms = 0;
    for (int i = 0; i < count; ++i) {
        const dungeon_room *r = &rooms[i];
        bool has_previous = i > 0, has_next = i + 1 < count;
        v3 center = v3_make(r->x, 0.0f, (r->near_z + r->far_z) * 0.5f);
        g->dungeon_path[g->dungeon_rooms++] = center;

        block(g, v3_make(r->x, -DUNGEON_FLOOR_HALF, center.z), v3_make(r->half_x, DUNGEON_FLOOR_HALF, r->half_z), floor_color);
        wall(g, 0, r->near_z, r->x - r->half_x, r->x + r->half_x, r->corridor_in, has_previous ? DUNGEON_CORRIDOR_HALF : 0.0f, stone);
        wall(g, 0, r->far_z, r->x - r->half_x, r->x + r->half_x, r->corridor_out, has_next ? DUNGEON_CORRIDOR_HALF : 0.0f, stone);
        wall(g, 2, r->x - r->half_x, r->far_z, r->near_z, 0.0f, 0.0f, stone);
        wall(g, 2, r->x + r->half_x, r->far_z, r->near_z, 0.0f, 0.0f, stone);

        // Something to walk around, clear of the centre line the path runs along.
        int clutter = 1 + (int)(next_random(&random) % 3);
        for (int k = 0; k < clutter; ++k) {
            float side = next_random(&random) % 2 ? 1.0f : -1.0f;
            float cx = r->x + side * random_range(&random, DUNGEON_CORRIDOR_HALF + 1.5f, r->half_x - 1.5f);
            float cz = center.z + random_range(&random, -(r->half_z - 2.0f), r->half_z - 2.0f);
            if (next_random(&random) % 2)
                block(g, v3_make(cx, DUNGEON_WALL_HEIGHT * 0.5f, cz), v3_make(0.6f, DUNGEON_WALL_HEIGHT * 0.5f, 0.6f), pillar_color);
            else
                block(g, v3_make(cx, 0.6f, cz), v3_make(0.7f, 0.6f, 0.7f), crate_color);
        }

        // A chest, off the path, in most rooms: its contents differ every run.
        if (i > 0 && next_random(&random) % 4 != 0) {
            float side = next_random(&random) % 2 ? 1.0f : -1.0f;
            float cx = r->x + side * (r->half_x - 2.5f), cz = center.z + (i % 2 ? 1.5f : -1.5f);
            item it = item_roll(loot_seed * 2654435761u + (uint32_t)i * 40503u + 17u, 1 + i);
            game_spawn_loot(g, v3_make(cx, 0.25f, cz), &it);
        }

        if (has_next) {
            float c_near = r->far_z, c_far = r->far_z - r->corridor_length;
            block(g, v3_make(r->corridor_out, -DUNGEON_FLOOR_HALF, (c_near + c_far) * 0.5f),
                  v3_make(DUNGEON_CORRIDOR_HALF, DUNGEON_FLOOR_HALF, r->corridor_length * 0.5f), floor_color);
            wall(g, 2, r->corridor_out - DUNGEON_CORRIDOR_HALF, c_far, c_near, 0.0f, 0.0f, stone);
            wall(g, 2, r->corridor_out + DUNGEON_CORRIDOR_HALF, c_far, c_near, 0.0f, 0.0f, stone);
        } else {
            // The exit: a glowing slab set into the far wall, and the way out in front of it.
            block(g, v3_make(r->x, 2.0f, r->far_z + 0.8f), v3_make(1.6f, 1.8f, 0.2f), v3_make(0.9f, 0.85f, 0.5f));
            game_declare_place(g, "exit", PLACE_EXIT, v3_make(r->x, 2.0f, r->far_z + 2.0f), v3_make(1.6f, 2.0f, 1.0f),
                               return_position, return_place);
        }
    }
    g->spawn = g->dungeon_path[0];
}

int game_dungeon_path(const game *g, v3 *out, int capacity) {
    int n = g->dungeon_rooms < capacity ? g->dungeon_rooms : capacity;
    for (int i = 0; i < n; ++i) out[i] = g->dungeon_path[i];
    return n;
}
