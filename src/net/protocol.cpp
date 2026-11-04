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

void item_write(cw_writer *w, const item *it) {
    cw_writer_u32(w, it->id);
    cw_writer_u8(w, it->slot);
    cw_writer_u8(w, it->rarity);
    cw_writer_u16(w, it->power);
    cw_writer_u32(w, it->seed);
}
bool item_read(cw_reader *r, item *it) {
    memset(it, 0, sizeof(*it));
    cw_reader_u32(r, &it->id);
    cw_reader_u8(r, &it->slot);
    cw_reader_u8(r, &it->rarity);
    cw_reader_u16(r, &it->power);
    cw_reader_u32(r, &it->seed);
    return cw_reader_ok(r) && it->slot < SLOT_COUNT && it->rarity < RARITY_COUNT;
}
void character_write(cw_writer *w, const character *c) {
    cw_writer_u32(w, c->version);
    for (int i = 0; i < SLOT_COUNT; ++i) item_write(w, &c->equipment[i]);
    for (int i = 0; i < STASH_SIZE; ++i) item_write(w, &c->stash[i]);
}
bool character_read(cw_reader *r, character *c) {
    memset(c, 0, sizeof(*c));
    cw_reader_u32(r, &c->version);
    for (int i = 0; i < SLOT_COUNT; ++i)
        if (!item_read(r, &c->equipment[i])) return false;
    for (int i = 0; i < STASH_SIZE; ++i)
        if (!item_read(r, &c->stash[i])) return false;
    return cw_reader_ok(r);
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
    case NET_C_EQUIPMENT: return g->c_equipment;
    case NET_C_LOOT: return g->c_loot;
    case NET_C_OWNED: return g->c_owned;
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
        cw_writer_u16(w, p->hits_made);
        cw_writer_u16(w, p->hits_taken);
        write_name(w, p->name);
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
        cw_writer_f32(w, s->phase);   // the derived fields follow from universe time
        return true;
    }
    case NET_C_PLANET: {
        const Planet *p = CW_GET(world, id, g->c_planet, Planet);
        if (!p) return false;
        write_name(w, p->name);
        cw_writer_f32(w, p->land.radius);
        cw_writer_f32(w, p->land.continents);
        cw_writer_f32(w, p->land.relief);
        cw_writer_f32(w, p->land.sea_level);
        cw_writer_f32(w, p->land.mountains);
        cw_writer_f32(w, p->land.shore);
        cw_writer_f32(w, p->land.pad_radius);
        cw_writer_u32(w, p->land.seed);
        return true;
    }
    case NET_C_EQUIPMENT: {
        const Equipment *e = CW_GET(world, id, g->c_equipment, Equipment);
        for (int i = 0; i < SLOT_COUNT; ++i) item_write(w, &e->items[i]);
        return true;
    }
    case NET_C_LOOT: {
        item_write(w, &CW_GET(world, id, g->c_loot, Loot)->it);
        return true;
    }
    case NET_C_OWNED: {
        cw_writer_u64(w, CW_GET(world, id, g->c_owned, Owned)->account);
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
        cw_reader_u16(r, &p.hits_made);
        cw_reader_u16(r, &p.hits_taken);
        if (!read_name(r, p.name)) return false;
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
        cw_reader_f32(r, &s.phase);
        game_ship_at_time(&s, game_time_of_tick(g, world->tick));
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_ship, &s, sizeof(s));
    }
    case NET_C_PLANET: {
        Planet p = {};
        if (!read_name(r, p.name)) return false;
        cw_reader_f32(r, &p.land.radius);
        cw_reader_f32(r, &p.land.continents);
        cw_reader_f32(r, &p.land.relief);
        cw_reader_f32(r, &p.land.sea_level);
        cw_reader_f32(r, &p.land.mountains);
        cw_reader_f32(r, &p.land.shore);
        cw_reader_f32(r, &p.land.pad_radius);
        cw_reader_u32(r, &p.land.seed);
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_planet, &p, sizeof(p));
    }
    case NET_C_EQUIPMENT: {
        Equipment e = {};
        for (int i = 0; i < SLOT_COUNT; ++i)
            if (!item_read(r, &e.items[i])) return false;
        return cw_comp_set(world, id, g->c_equipment, &e, sizeof(e));
    }
    case NET_C_LOOT: {
        Loot loot = {};
        if (!item_read(r, &loot.it)) return false;
        return cw_comp_set(world, id, g->c_loot, &loot, sizeof(loot));
    }
    case NET_C_OWNED: {
        Owned owned = {};
        cw_reader_u64(r, &owned.account);
        return cw_reader_ok(r) && cw_comp_set(world, id, g->c_owned, &owned, sizeof(owned));
    }
    default:
        return false;
    }
}

bool net_entity_write(cw_writer *w, game *g, cw_entity_id id) { return net_entity_write_masked(w, g, id, NET_C_ALL); }
bool net_entity_write_masked(cw_writer *w, game *g, cw_entity_id id, uint32_t components) {
    cw_world *world = &g->world;
    if (!cw_world_is_valid(world, id)) return false;
    cw_writer_i64(w, id);
    size_t count_offset = w->size;
    cw_writer_u8(w, 0);
    uint8_t count = 0;
    for (int component = 0; component < NET_C_COUNT; ++component) {
        if (!(components >> component & 1) || !cw_comp_has(world, id, net_component_id(g, component))) continue;
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

bool net_entity_read(cw_reader *r, game *g, cw_entity_id *out) { return net_entity_read_masked(r, g, out, true); }
bool net_entity_read_masked(cw_reader *r, game *g, cw_entity_id *out, bool create) {
    cw_world *world = &g->world;
    cw_entity_id id = CW_INVALID_ENTITY;
    uint8_t count = 0;
    cw_reader_i64(r, &id);
    cw_reader_u8(r, &count);
    if (!cw_reader_ok(r) || id < 0) return false;
    bool present = cw_world_is_valid(world, id) && !(world->entities[id].flags & CW_TO_DESTROY);
    if (!present && create) {
        if (!cw_world_is_valid(world, id)) cw_world_restore_entity(world, id);
        else cw_world_rebuild_entity(world, id);   // the id came back before the sweep
        present = true;
    }
    if (out) *out = id;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t component = 0;
        uint16_t size = 0;
        cw_reader_u8(r, &component);
        cw_reader_u16(r, &size);
        cw_reader payload;
        if (!cw_reader_sub(r, size, &payload)) return false;
        if (!present || component >= NET_C_COUNT) continue;   // newer server: skip what we do not know
        if (!net_component_read(&payload, g, id, component)) return false;
    }
    return true;
}

size_t net_entity_size(game *g, cw_entity_id id) { return net_entity_size_masked(g, id, NET_C_ALL); }
size_t net_entity_size_masked(game *g, cw_entity_id id, uint32_t components) {
    cw_writer w;
    cw_writer_init_fixed(&w, NULL, 0);
    net_entity_write_masked(&w, g, id, components);
    return w.size;
}

void net_keys_write(cw_writer *w, const net_keys_message *m) {
    cw_writer_u32(w, m->session);
    cw_writer_bytes(w, m->server_public, CRYPTO_KEY_SIZE);
    cw_writer_bytes(w, m->client_public, CRYPTO_KEY_SIZE);
}
bool net_keys_read(cw_reader *r, net_keys_message *m) {
    cw_reader_u32(r, &m->session);
    cw_reader_bytes(r, m->server_public, CRYPTO_KEY_SIZE);
    cw_reader_bytes(r, m->client_public, CRYPTO_KEY_SIZE);
    return cw_reader_ok(r);
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

void net_connect_write(cw_writer *w, const net_connect_message *m, const unsigned char *ticket, size_t ticket_size,
                       const unsigned char *token, size_t token_size) {
    size_t start = w->size;
    cw_writer_u32(w, m->version);
    cw_writer_u32(w, m->nonce);
    cw_writer_bytes(w, m->public_key, CRYPTO_KEY_SIZE);
    write_text(w, m->platform, ACCOUNT_PLATFORM_CAPACITY);
    write_text(w, m->name, ACCOUNT_NAME_CAPACITY);
    write_text(w, m->region, NET_REGION_NAME);
    cw_writer_u16(w, (uint16_t)m->proof_size);
    cw_writer_bytes(w, m->proof, m->proof_size);
    cw_writer_u64(w, m->challenge);
    cw_writer_u16(w, (uint16_t)ticket_size);
    cw_writer_bytes(w, ticket, ticket_size);
    cw_writer_u16(w, (uint16_t)token_size);
    cw_writer_bytes(w, token, token_size);
    while (w->size - start < NET_CONNECT_PADDING) cw_writer_u8(w, 0);
}

bool net_connect_read(cw_reader *r, net_connect_message *m, const net_zone_table *zones, bool *bad_credential) {
    memset(m, 0, sizeof(*m));
    *bad_credential = false;
    cw_reader_u32(r, &m->version);
    cw_reader_u32(r, &m->nonce);
    cw_reader_bytes(r, m->public_key, CRYPTO_KEY_SIZE);
    if (!read_text(r, m->platform, ACCOUNT_PLATFORM_CAPACITY) || !read_text(r, m->name, ACCOUNT_NAME_CAPACITY)
        || !read_text(r, m->region, NET_REGION_NAME)) return false;
    uint16_t proof_size = 0;
    cw_reader_u16(r, &proof_size);
    if (proof_size > NET_PROOF_CAPACITY || !cw_reader_bytes(r, m->proof, proof_size)) return false;
    m->proof_size = proof_size;
    cw_reader_u64(r, &m->challenge);
    uint16_t size = 0;
    cw_reader_u16(r, &size);
    if (!cw_reader_ok(r)) return false;
    if (size) {
        cw_reader t;
        if (!cw_reader_sub(r, size, &t)) return false;
        if (zones && net_ticket_read(&t, &m->ticket, zones)) m->has_ticket = true;
        else *bad_credential = true;
    }
    cw_reader_u16(r, &size);
    if (!cw_reader_ok(r)) return false;
    if (size) {
        cw_reader t;
        if (!cw_reader_sub(r, size, &t)) return false;
        if (zones && net_token_read(&t, &m->token, zones)) m->has_token = true;
        else *bad_credential = true;
    }
    return true;   // the padding that follows is ignored
}

// A redirect's ticket travels as opaque bytes: the client cannot sign or verify it.
void net_redirect_write(cw_writer *w, const net_redirect *m, const net_zone_table *zones) {
    cw_writer_u32(w, m->address.ip);
    cw_writer_u16(w, m->address.port);
    unsigned char ticket[NET_TICKET_SIZE];
    cw_writer t;
    cw_writer_init_fixed(&t, ticket, sizeof(ticket));
    net_ticket_write(&t, &m->ticket, zones);
    cw_writer_u16(w, (uint16_t)t.size);
    cw_writer_bytes(w, ticket, t.size);
}

bool net_redirect_read(cw_reader *r, net_address *address, unsigned char *ticket, size_t *ticket_size) {
    uint16_t size = 0;
    cw_reader_u32(r, &address->ip);
    cw_reader_u16(r, &address->port);
    cw_reader_u16(r, &size);
    if (!cw_reader_ok(r) || size > NET_TICKET_SIZE) return false;
    *ticket_size = size;
    return cw_reader_bytes(r, ticket, size);
}

void net_welcome_write(cw_writer *w, const net_welcome *m) {
    cw_writer_u32(w, m->session);
    cw_writer_i64(w, m->player);
    cw_writer_u32(w, m->tick);
    cw_writer_u8(w, m->tick_hz);
    cw_writer_u8(w, m->snapshot_hz);
    cw_writer_f64(w, m->universe_base);
    write_v3(w, m->spawn);
    cw_writer_u64(w, m->account);
    cw_writer_u8(w, m->input_target);
    write_name(w, m->world);
    write_text(w, m->zone, NET_ZONE_NAME);
    cw_writer_u8(w, (uint8_t)m->place_count);
    for (int i = 0; i < m->place_count; ++i) {
        write_name(w, m->places[i].name);
        cw_writer_u8(w, (uint8_t)m->places[i].kind);
        write_v3(w, m->places[i].center);
        write_v3(w, m->places[i].half);
        write_v3(w, m->places[i].spawn);
        write_name(w, m->places[i].destination);
    }
}
bool net_welcome_read(cw_reader *r, net_welcome *m) {
    cw_reader_u32(r, &m->session);
    cw_reader_i64(r, &m->player);
    cw_reader_u32(r, &m->tick);
    cw_reader_u8(r, &m->tick_hz);
    cw_reader_u8(r, &m->snapshot_hz);
    cw_reader_f64(r, &m->universe_base);
    m->spawn = read_v3(r);
    cw_reader_u64(r, &m->account);
    cw_reader_u8(r, &m->input_target);
    if (!read_name(r, m->world) || !read_text(r, m->zone, NET_ZONE_NAME)) return false;
    uint8_t count = 0;
    cw_reader_u8(r, &count);
    if (count > GAME_MAX_PLACES) return false;
    m->place_count = count;
    for (int i = 0; i < m->place_count; ++i) {
        uint8_t kind = 0;
        if (!read_name(r, m->places[i].name)) return false;
        cw_reader_u8(r, &kind);
        m->places[i].kind = kind;
        m->places[i].center = read_v3(r);
        m->places[i].half = read_v3(r);
        m->places[i].spawn = read_v3(r);
        if (!read_name(r, m->places[i].destination)) return false;
    }
    return cw_reader_ok(r);
}

static int8_t axis_q(float v) {
    float s = (v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v) * 127.0f;
    return (int8_t)(int)(s < 0.0f ? s - 0.5f : s + 0.5f);
}
static uint16_t yaw_q(float yaw) {
    float turns = yaw / (2.0f * TP_PI);
    turns -= floorf(turns);
    return (uint16_t)(int)(turns * 65536.0f + 0.5f);
}
static int16_t pitch_q(float pitch) {
    float p = pitch * (32767.0f / TP_PI);
    return (int16_t)(int)(p > 32767.0f ? 32767.0f : p < -32767.0f ? -32767.0f : p < 0.0f ? p - 0.5f : p + 0.5f);
}
static PlayerInput input_dequantize(int8_t forward, int8_t strafe, uint16_t yaw, int16_t pitch, uint8_t flags) {
    PlayerInput in = {};
    in.forward = (float)forward / 127.0f;
    in.strafe = (float)strafe / 127.0f;
    in.yaw = (float)yaw * (2.0f * TP_PI / 65536.0f);
    in.pitch = (float)pitch * (TP_PI / 32767.0f);
    in.jump = (flags & 1) != 0;
    in.act = (flags & 2) != 0;
    return in;
}
void net_input_quantize(PlayerInput *in) {
    *in = input_dequantize(axis_q(in->forward), axis_q(in->strafe), yaw_q(in->yaw), pitch_q(in->pitch),
                           (uint8_t)((in->jump ? 1 : 0) | (in->act ? 2 : 0)));
}

void net_input_write(cw_writer *w, const net_input_message *m) {
    cw_writer_u32(w, m->seq);
    cw_writer_u32(w, m->ack_tick);
    cw_writer_u8(w, (uint8_t)m->count);
    for (uint32_t i = 0; i < m->count && i < NET_INPUT_REDUNDANCY; ++i) {
        const PlayerInput *in = &m->inputs[i];
        cw_writer_i8(w, axis_q(in->forward));
        cw_writer_i8(w, axis_q(in->strafe));
        cw_writer_u16(w, yaw_q(in->yaw));
        cw_writer_i16(w, pitch_q(in->pitch));
        cw_writer_u8(w, (uint8_t)((in->jump ? 1 : 0) | (in->act ? 2 : 0)));
        float view = m->view_ticks[i] < 0.0f ? 0.0f : m->view_ticks[i];
        cw_writer_u32(w, (uint32_t)view);
        cw_writer_u8(w, (uint8_t)((view - floorf(view)) * 255.0f));
    }
}
bool net_input_read(cw_reader *r, net_input_message *m) {
    memset(m, 0, sizeof(*m));
    uint8_t count = 0;
    cw_reader_u32(r, &m->seq);
    cw_reader_u32(r, &m->ack_tick);
    cw_reader_u8(r, &count);
    if (count == 0 || count > NET_INPUT_REDUNDANCY) return false;
    m->count = count;
    for (uint32_t i = 0; i < m->count; ++i) {
        int8_t forward = 0, strafe = 0;
        uint16_t yaw = 0;
        int16_t pitch = 0;
        uint8_t flags = 0, fraction = 0;
        uint32_t view = 0;
        cw_reader_i8(r, &forward);
        cw_reader_i8(r, &strafe);
        cw_reader_u16(r, &yaw);
        cw_reader_i16(r, &pitch);
        cw_reader_u8(r, &flags);
        cw_reader_u32(r, &view);
        cw_reader_u8(r, &fraction);
        m->inputs[i] = input_dequantize(forward, strafe, yaw, pitch, flags);
        m->view_ticks[i] = (float)view + (float)fraction / 255.0f;
    }
    return cw_reader_ok(r);
}
