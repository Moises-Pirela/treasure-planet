#include "cw_internal.h"
#include <math.h>

void cw_sim_init(cw_sim *s, cw_sim_role role) {
    cw_require(s != NULL);
    memset(s, 0, sizeof(*s));
    s->role = role;
}
void cw_sim_destroy(cw_sim *s) {
    if (!s) return;
    cw_require(s->dispatch_depth == 0);
    for (size_t i = 0; i < s->count; ++i) free(s->systems[i].name);
    free(s->systems);
    memset(s, 0, sizeof(*s));
}
static cw_system *find_system(cw_sim *s, const char *name) {
    if (!s || !name) return NULL;
    for (size_t i = 0; i < s->count; ++i)
        if (strcmp(s->systems[i].name, name) == 0) return &s->systems[i];
    return NULL;
}
bool cw_sim_register(cw_sim *s, cw_system_desc desc) {
    if (!s || !desc.fn || s->dispatch_depth) return false;
    if (desc.authority != CW_SHARED && desc.authority != CW_HOST_ONLY) return false;
    cw_system *reg = desc.name && *desc.name ? find_system(s, desc.name) : NULL;
    if (!reg) {
        if (s->count == s->capacity) {
            s->capacity = s->capacity ? cw_mul(s->capacity, 2) : 16;
            s->systems = CW_RESIZE(s->systems, s->capacity, sizeof(*s->systems));
        }
        reg = &s->systems[s->count++];
        memset(reg, 0, sizeof(*reg));
        reg->name = cw_copy_string(desc.name);
        cw_require(s->next_sequence != UINT64_MAX);
        reg->sequence = s->next_sequence++;
    }
    reg->phases = desc.phases;
    reg->fn = desc.fn;
    reg->authority = desc.authority;
    reg->order = desc.order;
    reg->run_when_paused = desc.run_when_paused;
    reg->enabled = true;
    s->sorted = false;
    return true;
}
bool cw_sim_remove(cw_sim *s, const char *name) {
    if (!s || s->dispatch_depth) return false;
    cw_system *reg = find_system(s, name);
    if (!reg) return false;
    size_t i = (size_t)(reg - s->systems);
    free(reg->name);
    memmove(reg, reg + 1, (s->count - i - 1) * sizeof(*reg));
    --s->count;
    s->sorted = false;
    return true;
}
bool cw_sim_set_enabled(cw_sim *s, const char *name, bool enabled) {
    cw_system *reg = find_system(s, name);
    if (!reg) return false;
    reg->enabled = enabled;
    return true;
}
const cw_system *cw_sim_find(const cw_sim *s, const char *name) {
    if (!s || !name) return NULL;
    for (size_t i = 0; i < s->count; ++i)
        if (strcmp(s->systems[i].name, name) == 0) return &s->systems[i];
    return NULL;
}
static int compare_systems(const void *left, const void *right) {
    const cw_system *a = (const cw_system *)left, *b = (const cw_system *)right;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    return a->sequence < b->sequence ? -1 : a->sequence > b->sequence;
}
static void sim_sort(cw_sim *s) {
    if (!s->sorted) {
        if (s->count > 1) qsort(s->systems, s->count, sizeof(*s->systems), compare_systems);
        s->sorted = true;
    }
}
const cw_system *cw_sim_systems(cw_sim *s, size_t *count) {
    if (count) *count = s ? s->count : 0;
    if (!s) return NULL;
    sim_sort(s);
    return s->systems;
}
static bool pausable(cw_phase phase) {
    return phase == CW_EARLY_UPDATE || phase == CW_PHYSICS_UPDATE || phase == CW_UPDATE
        || phase == CW_LATE_UPDATE || phase == CW_END_FRAME;
}
void cw_sim_dispatch(cw_sim *s, cw_world *w, cw_phase phase, float dt) {
    if (!s || !w || phase < CW_INIT || phase > CW_END_FRAME) return;
    sim_sort(s);
    ++s->dispatch_depth;
    for (size_t i = 0; i < s->count; ++i) {
        cw_system reg = s->systems[i];
        if (!reg.enabled || !(reg.phases & CW_PHASE(phase))) continue;
        if (s->role == CW_CLIENT && reg.authority == CW_HOST_ONLY) continue;
        if (s->paused && pausable(phase) && !reg.run_when_paused) continue;
        reg.fn(w, phase, dt);
    }
    if (--s->dispatch_depth == 0) cw_commands_flush(&w->commands, w);
}
void cw_sim_replay(cw_sim *s, cw_world *w, float dt) {
    if (!s || !w) return;
    sim_sort(s);
    ++s->dispatch_depth;
    for (size_t i = 0; i < s->count; ++i) {
        cw_system reg = s->systems[i];
        if (reg.enabled && (reg.phases & CW_PHASE(CW_UPDATE)) && reg.authority == CW_SHARED)
            reg.fn(w, CW_UPDATE, dt);
    }
    if (--s->dispatch_depth == 0) cw_commands_flush(&w->commands, w);
}
void cw_sim_entity_lifecycle(cw_sim *s, cw_world *w, float dt) {
    if (!s || !w) return;
    if (w->pending_destroy.count) cw_sim_dispatch(s, w, CW_ENTITY_DESTROYED, dt);
    cw_world_sweep(w);
    if (w->created.count) {
        cw_sim_dispatch(s, w, CW_ENTITY_CREATED, dt);
        w->created.count = 0;
    }
}
void cw_sim_tick(cw_sim *s, cw_world *w, float dt) {
    if (!s || !w) return;
    cw_sim_dispatch(s, w, CW_EARLY_UPDATE, dt);
    cw_sim_dispatch(s, w, CW_PHYSICS_UPDATE, dt);
    cw_sim_dispatch(s, w, CW_UPDATE, dt);
    cw_sim_dispatch(s, w, CW_LATE_UPDATE, dt);
    cw_sim_dispatch(s, w, CW_END_FRAME, dt);
    cw_sim_entity_lifecycle(s, w, dt);
    ++w->tick;
}

cw_clock cw_clock_make(int hz) {
    cw_clock c = {0};
    c.hz = hz > 0 ? hz : 1;
    c.dt = 1.0f / (float)c.hz;
    c.max_steps = CW_MAX_STEPS_PER_FRAME;
    c.max_catchup = CW_MAX_CATCHUP_STEPS_PER_FRAME;
    c.backlog_target = CW_INPUT_BACKLOG_TARGET;
    return c;
}
int cw_clock_advance(cw_clock *c, float frame_dt) {
    if (!c || !isfinite(frame_dt) || frame_dt < 0 || !isfinite(c->dt) || c->dt <= 0
        || c->max_steps <= 0) return 0;
    c->accumulator += frame_dt < CW_MAX_FRAME_DT ? frame_dt : CW_MAX_FRAME_DT;
    int steps = 0;
    while (c->accumulator >= c->dt && steps < c->max_steps) {
        c->accumulator -= c->dt;
        ++steps;
    }
    if (steps == c->max_steps) c->accumulator = 0;
    return steps;
}
bool cw_clock_should_drain(const cw_clock *c, int backlog, int taken, int drained) {
    if (!c || taken < 0 || drained < 0 || backlog <= c->backlog_target
        || drained >= c->max_catchup) return false;
    return (int64_t)taken + drained < c->max_steps;
}
float cw_clock_alpha(const cw_clock *c) {
    if (!c || !isfinite(c->dt) || c->dt <= 0 || !isfinite(c->accumulator)) return 0;
    float alpha = c->accumulator / c->dt;
    return alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
}
void cw_clock_reset(cw_clock *c) { if (c) c->accumulator = 0; }
