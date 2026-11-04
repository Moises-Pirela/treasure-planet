// What the simulation's own files share and nothing outside them needs: the
// systems each file registers, and the collision boxes players and crafts use.
#ifndef TP_GAME_INTERNAL_H
#define TP_GAME_INTERNAL_H

#include "game.h"

// World data reachable from a system. Systems get only the world, and the world's
// user pointer is how they find the component ids and queries.
static inline game *game_of(cw_world *w) { return (game *)w->user; }

extern const float GAME_GRAVITY;     // m/s^2
extern const float GAME_STEP_HEIGHT; // metres a walker steps up without jumping

// A box to collide with: the world's solids in world space, a craft's parts in its frame.
struct game_box {
    v3 center, half;
};
#define GAME_MAX_BOXES 64

// The solids listed in the grid cell of `at` (game.cpp); returns the count.
int game_solids_near(const game *g, v3 at, cw_entity_id *out);
// A craft's parts, as listed this tick: `*first` indexes g->craft_parts; returns the count.
int game_craft_part_range(const game *g, cw_entity_id craft, int *first);
// The boxes of a craft's parts in its frame, at most `capacity`; returns the count.
int game_craft_boxes(game *g, cw_entity_id craft, game_box *out, int capacity);
// Puts a craft's parts where its pose has them, now and last tick.
void game_place_craft_parts(game *g, cw_entity_id craft);

void game_system_crafts(cw_world *w, cw_phase phase, float dt);   // shared: flight, contacts
void game_system_yard(cw_world *w, cw_phase phase, float dt);     // host: terminals, towing

#endif
