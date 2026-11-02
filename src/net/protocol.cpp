#include "protocol.h"
#include <string.h>

static void write_v3(cw_writer *w, v3 v) {
    cw_writer_f32(w, v.x);
    cw_writer_f32(w, v.y);
    cw_writer_f32(w, v.z);
}
static v3 read_v3(cw_reader *r) {
    v3 v = {};
    cw_reader_f32(r, &v.x);
    cw_reader_f32(r, &v.y);
    cw_reader_f32(r, &v.z);
    return v;
}
static void write_name(cw_writer *w, const char *name) {
    uint8_t length = (uint8_t)strnlen(name, GAME_NAME_CAPACITY - 1);
    cw_writer_u8(w, length);
    cw_writer_bytes(w, name, length);
}
static bool read_name(cw_reader *r, char *name) {
    uint8_t length = 0;
    cw_reader_u8(r, &length);
    if (length >= GAME_NAME_CAPACITY) return false;
    memset(name, 0, GAME_NAME_CAPACITY);
    return cw_reader_bytes(r, name, length);
}

cw_component_id net_component_id(const game *g, int component) {
    switch (component) {
    case NET_C_TRANSFORM: return g->c_transform;
    case NET_C_SOLID: return g->c_solid;
    case NET_C_ATTACHMENT: return g->c_attachment;
    case NET_C_VISUAL: return g->c_visual;
    case NET_C_PLAYER: return g->c_player;
    case NET_C_SHIP: return g->c_ship;
    case NET_C_PLANET: return g->c_planet;
    default: return CW_INVALID_COMPONENT;
    }
}

bool net_component_write(cw_writer *w, game *g, cw_entity_id id, int component) {
    cw_world *world = &g->world;
    if (!cw_comp_has(world, id, net_component_id(g, component))) return false;
    switch (component) {
    case NET_C_TRANSFORM: {
        const Transform *t = CW_GET(world, id, g->c_transform, Transform);
        if (!t) return false;
        write_v3(w, t->position);
        return true;
    }
    case NET_C_SOLID: {
        const Solid *s = CW_GET(world, id, g->c_solid, Solid);
        if (!s) return false;
        write_v3(w, s->half);
        return true;
    }
    case NET_C_ATTACHMENT: {
        const Attachment *a = CW_GET(world, id, g->c_attachment, Attachment);
        if (!a) return false;
        cw_writer_i64(w, a->ship);
        write_v3(w, a->offset);
        return true;
    }
    case NET_C_VISUAL: {
        const Visual *v = CW_GET(world, id, g->c_visual, Visual);
        if (!v) return false;
        cw_writer_u8(w, (uint8_t)v->mesh);
        write_v3(w, v->half);
        write_v3(w, v->color);
        return true;
    }
    case NET_C_PLAYER: {
        const Player *p = CW_GET(world, id, g->c_player, Player);
        if (!p) return false;
        write_v3(w, p->velocity);
        cw_writer_f32(w, p->yaw);
        cw_writer_f32(w, p->pitch);
        cw_writer_u8(w, p->grounded ? 1 : 0);
        return true;
    }
    case NET_C_SHIP: {
        const Ship *s = CW_GET(world, id, g->c_ship, Ship);
        if (!s) return false;
        write_name(w, s->name);
        write_v3(w, s->half);
        write_v3(w, s->hub_dock);
        write_v3(w, s->planet_dock);
        cw_writer_i64(w, s->planet);
        cw_writer_f32(w, s->cruise_height);
        cw_writer_f32(w, s->flight_seconds);
        cw_writer_f32(w, s->dock_seconds);
        cw_writer_f32(w, s->timer);
        cw_writer_u8(w, (uint8_t)s->state);
        cw_writer_u8(w, (uint8_t)s->stop);
        return true;
    }
    case NET_C_PLANET: {
        const Planet *p = CW_GET(world, id, g->c_planet, Planet);
        if (!p) return false;
        write_name(w, p->name);
        return true;
    }
    default:
        return false;
    }
}

bool net_component_read(cw_reader *r, game *g, cw_entity_id id, int component) {
    cw_world *world = &g->world;
    switch (component) {
    case NET_C_TRANSFORM: {
        Transform t = {};
        t.position = t.previous = read_v3(r);
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_transform, &t, sizeof(t));
    }
    case NET_C_SOLID: {
        Solid s = {};
        s.half = read_v3(r);
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_solid, &s, sizeof(s));
    }
    case NET_C_ATTACHMENT: {
        Attachment a = {};
        cw_reader_i64(r, &a.ship);
        a.offset = read_v3(r);
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_attachment, &a, sizeof(a));
    }
    case NET_C_VISUAL: {
        Visual v = {};
        uint8_t mesh = 0;
        cw_reader_u8(r, &mesh);
        v.mesh = mesh;
        v.half = read_v3(r);
        v.color = read_v3(r);
        if (mesh >= MESH_COUNT) return false;
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_visual, &v, sizeof(v));
    }
    case NET_C_PLAYER: {
        Player p = {};
        p.velocity = read_v3(r);
        cw_reader_f32(r, &p.yaw);
        cw_reader_f32(r, &p.pitch);
        uint8_t grounded = 0;
        cw_reader_u8(r, &grounded);
        p.grounded = grounded != 0;
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_player, &p, sizeof(p));
    }
    case NET_C_SHIP: {
        Ship s = {};
        if (!read_name(r, s.name)) return false;
        s.half = read_v3(r);
        s.hub_dock = read_v3(r);
        s.planet_dock = read_v3(r);
        cw_reader_i64(r, &s.planet);
        cw_reader_f32(r, &s.cruise_height);
        cw_reader_f32(r, &s.flight_seconds);
        cw_reader_f32(r, &s.dock_seconds);
        cw_reader_f32(r, &s.timer);
        uint8_t state = 0, stop = 0;
        cw_reader_u8(r, &state);
        cw_reader_u8(r, &stop);
        s.state = state;
        s.stop = stop;
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_ship, &s, sizeof(s));
    }
    case NET_C_PLANET: {
        Planet p = {};
        if (!read_name(r, p.name)) return false;
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_planet, &p, sizeof(p));
    }
    default:
        return false;
    }
}

bool net_entity_write(cw_writer *w, game *g, cw_entity_id id) {
    cw_world *world = &g->world;
    if (!cw_world_is_valid(world, id)) return false;
    cw_writer_i64(w, id);
    size_t count_offset = w->size;
    cw_writer_u8(w, 0);
    uint8_t count = 0;
    for (int component = 0; component < NET_C_COUNT; ++component) {
        if (!cw_comp_has(world, id, net_component_id(g, component))) continue;
        cw_writer_u8(w, (uint8_t)component);
        size_t size_offset = w->size;
        cw_writer_u16(w, 0);
        size_t start = w->size;
        if (!net_component_write(w, g, id, component)) return false;
        // Patch the length. cw_writer has no u16 patch; the buffer is ours to edit
        // when it is not merely measuring.
        uint16_t size = (uint16_t)(w->size - start);
        if (w->data && size_offset + 2 <= w->capacity) {
            w->data[size_offset] = (unsigned char)(size & 0xff);
            w->data[size_offset + 1] = (unsigned char)(size >> 8);
        }
        ++count;
    }
    if (w->data && count_offset < w->capacity) w->data[count_offset] = count;
    return cw_writer_ok(w);
}

bool net_entity_read(cw_reader *r, game *g, cw_entity_id *out) {
    cw_world *world = &g->world;
    cw_entity_id id = CW_INVALID_ENTITY;
    uint8_t count = 0;
    cw_reader_i64(r, &id);
    cw_reader_u8(r, &count);
    if (!cw_reader_ok(r) || id < 0) return false;
    if (!cw_world_is_valid(world, id)) cw_world_restore_entity(world, id);
    else if (world->entities[id].flags & CW_TO_DESTROY) cw_world_rebuild_entity(world, id);   // the id came back before the sweep
    if (out) *out = id;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t component = 0;
        uint16_t size = 0;
        cw_reader_u8(r, &component);
        cw_reader_u16(r, &size);
        cw_reader payload;
        if (!cw_reader_sub(r, size, &payload)) return false;
        if (component >= NET_C_COUNT) continue;   // newer server: skip what we do not know
        if (!net_component_read(&payload, g, id, component)) return false;
    }
    return true;
}

size_t net_entity_size(game *g, cw_entity_id id) {
    cw_writer w;
    cw_writer_init_fixed(&w, NULL, 0);
    net_entity_write(&w, g, id);
    return w.size;
}

void net_welcome_write(cw_writer *w, const net_welcome *m) {
    cw_writer_u32(w, m->session);
    cw_writer_i64(w, m->player);
    cw_writer_u32(w, m->tick);
    cw_writer_u8(w, m->tick_hz);
    cw_writer_u8(w, m->snapshot_hz);
}
bool net_welcome_read(cw_reader *r, net_welcome *m) {
    cw_reader_u32(r, &m->session);
    cw_reader_i64(r, &m->player);
    cw_reader_u32(r, &m->tick);
    cw_reader_u8(r, &m->tick_hz);
    cw_reader_u8(r, &m->snapshot_hz);
    return cw_reader_ok(r);
}

void net_snapshot_header_write(cw_writer *w, const net_snapshot_header *h) {
    cw_writer_u32(w, h->tick);
    cw_writer_u32(w, h->last_input_seq);
    cw_writer_u16(w, h->count);
}
bool net_snapshot_header_read(cw_reader *r, net_snapshot_header *h) {
    cw_reader_u32(r, &h->tick);
    cw_reader_u32(r, &h->last_input_seq);
    cw_reader_u16(r, &h->count);
    return cw_reader_ok(r);
}
void net_snapshot_entry_write(cw_writer *w, const net_snapshot_entry *e) {
    cw_writer_i64(w, e->id);
    write_v3(w, e->position);
    cw_writer_f32(w, e->yaw);
    cw_writer_f32(w, e->pitch);
}
bool net_snapshot_entry_read(cw_reader *r, net_snapshot_entry *e) {
    cw_reader_i64(r, &e->id);
    e->position = read_v3(r);
    cw_reader_f32(r, &e->yaw);
    cw_reader_f32(r, &e->pitch);
    return cw_reader_ok(r);
}

void net_input_write(cw_writer *w, const net_input_message *m) {
    cw_writer_u32(w, m->seq);
    cw_writer_u8(w, (uint8_t)m->count);
    for (uint32_t i = 0; i < m->count && i < NET_INPUT_REDUNDANCY; ++i) {
        const PlayerInput *in = &m->inputs[i];
        cw_writer_f32(w, in->forward);
        cw_writer_f32(w, in->strafe);
        cw_writer_f32(w, in->yaw);
        cw_writer_f32(w, in->pitch);
        cw_writer_u8(w, in->jump ? 1 : 0);
    }
}
bool net_input_read(cw_reader *r, net_input_message *m) {
    memset(m, 0, sizeof(*m));
    uint8_t count = 0;
    cw_reader_u32(r, &m->seq);
    cw_reader_u8(r, &count);
    if (count == 0 || count > NET_INPUT_REDUNDANCY) return false;
    m->count = count;
    for (uint32_t i = 0; i < m->count; ++i) {
        PlayerInput *in = &m->inputs[i];
        uint8_t jump = 0;
        cw_reader_f32(r, &in->forward);
        cw_reader_f32(r, &in->strafe);
        cw_reader_f32(r, &in->yaw);
        cw_reader_f32(r, &in->pitch);
        cw_reader_u8(r, &jump);
        in->jump = jump != 0;
    }
    return cw_reader_ok(r);
}
