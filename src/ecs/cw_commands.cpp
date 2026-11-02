#include "cw_internal.h"

/* Stream layout: a 16-byte aligned header followed by a payload of `size`
 * bytes, padded so the next header is 16-byte aligned again. */
enum { OP_CREATE, OP_DESTROY, OP_ADD, OP_SET, OP_UPDATE, OP_REMOVE, OP_CALL };
#define CMD_ALIGN 16
typedef struct {
    cw_entity_id entity;
    size_t size;
    void (*fn)(void);
    uint32_t op;
    uint32_t component;
} cmd_header;
static size_t align_up(size_t n) { return cw_add(n, CMD_ALIGN - 1) & ~(size_t)(CMD_ALIGN - 1); }
/* Pending ids count down from -2 so that -1 stays CW_INVALID_ENTITY. */
static cw_entity_id pending_id(size_t index) { return -2 - (cw_entity_id)index; }
static size_t pending_index(cw_entity_id id) { return (size_t)(-2 - id); }

void cw_commands_init(cw_commands *c) {
    cw_require(c != NULL);
    memset(c, 0, sizeof(*c));
}
void cw_commands_destroy(cw_commands *c) {
    if (!c) return;
    free(c->data);
    free(c->created.data);
    memset(c, 0, sizeof(*c));
}
void cw_commands_reserve(cw_commands *c, size_t bytes) {
    if (!c || bytes <= c->capacity) return;
    bytes = align_up(bytes);
    unsigned char *data = (unsigned char *)cw_resize(NULL, bytes, 1);   /* fresh block: keeps the stream 16-byte aligned */
    if (c->size) memcpy(data, c->data, c->size);
    free(c->data);
    c->data = data;
    c->capacity = bytes;
}
void cw_commands_clear(cw_commands *c) {
    if (!c || c->flushing) return;
    c->size = c->count = c->pending = 0;
    c->created.count = 0;
}
size_t cw_commands_count(const cw_commands *c) { return c ? c->count : 0; }
bool cw_commands_is_empty(const cw_commands *c) { return !c || c->count == 0; }
bool cw_entity_is_pending(cw_entity_id id) { return id <= -2; }

static bool valid_target(const cw_commands *c, cw_entity_id id) {
    return id >= 0 || (cw_entity_is_pending(id) && pending_index(id) < c->pending);
}
static bool record(cw_commands *c, uint32_t op, cw_entity_id entity, cw_component_id ct,
                   void (*fn)(void), const void *data, size_t size) {
    if (!c || c->flushing) return false;
    size_t needed = cw_add(c->size, cw_add(align_up(sizeof(cmd_header)), align_up(size)));
    if (needed > c->capacity) {
        size_t capacity = c->capacity ? cw_mul(c->capacity, 2) : 1024;
        cw_commands_reserve(c, capacity > needed ? capacity : needed);
    }
    cmd_header *h = (cmd_header *)(c->data + c->size);
    memset(h, 0, sizeof(*h));
    h->op = op;
    h->entity = entity;
    h->component = ct;
    h->fn = fn;
    h->size = size;
    if (size) memcpy(c->data + c->size + align_up(sizeof(cmd_header)), data, size);
    c->size = needed;
    ++c->count;
    return true;
}
cw_entity_id cw_commands_create(cw_commands *c) {
    if (!c || c->flushing) return CW_INVALID_ENTITY;
    cw_entity_id id = pending_id(c->pending);
    if (!record(c, OP_CREATE, id, CW_INVALID_COMPONENT, NULL, NULL, 0)) return CW_INVALID_ENTITY;
    ++c->pending;
    return id;
}
bool cw_commands_destroy_entity(cw_commands *c, cw_entity_id id) {
    return c && valid_target(c, id) && record(c, OP_DESTROY, id, CW_INVALID_COMPONENT, NULL, NULL, 0);
}
bool cw_commands_add(cw_commands *c, cw_entity_id id, cw_component_id ct) {
    return c && valid_target(c, id) && ct < CW_MAX_COMPONENTS
        && record(c, OP_ADD, id, ct, NULL, NULL, 0);
}
bool cw_commands_set(cw_commands *c, cw_entity_id id, cw_component_id ct, const void *value, size_t size) {
    return c && valid_target(c, id) && ct < CW_MAX_COMPONENTS && (value || !size)
        && record(c, OP_SET, id, ct, NULL, value, size);
}
bool cw_commands_update(cw_commands *c, cw_entity_id id, cw_component_id ct,
                        cw_update_fn fn, const void *data, size_t size) {
    return c && fn && valid_target(c, id) && ct < CW_MAX_COMPONENTS && (data || !size)
        && record(c, OP_UPDATE, id, ct, (void (*)(void))fn, data, size);
}
bool cw_commands_remove(cw_commands *c, cw_entity_id id, cw_component_id ct) {
    return c && valid_target(c, id) && ct < CW_MAX_COMPONENTS
        && record(c, OP_REMOVE, id, ct, NULL, NULL, 0);
}
bool cw_commands_call(cw_commands *c, cw_command_fn fn, const void *data, size_t size) {
    return c && fn && (data || !size)
        && record(c, OP_CALL, CW_INVALID_ENTITY, CW_INVALID_COMPONENT, (void (*)(void))fn, data, size);
}

static cw_entity_id resolve(const cw_commands *c, cw_entity_id id) {
    if (!cw_entity_is_pending(id)) return id;
    size_t index = pending_index(id);
    return index < c->created.count ? c->created.data[index] : CW_INVALID_ENTITY;
}
static bool apply(cw_commands *c, cw_world *w, const cmd_header *h, const void *payload) {
    cw_entity_id id = resolve(c, h->entity);
    cw_component_id ct = (cw_component_id)h->component;
    switch (h->op) {
        case OP_CREATE: {
            cw_entity_id created = cw_world_create_entity(w);
            if (c->created.count == c->created.capacity) {
                c->created.capacity = c->created.capacity ? cw_mul(c->created.capacity, 2) : 16;
                c->created.data = CW_RESIZE(c->created.data, c->created.capacity, sizeof(*c->created.data));
            }
            c->created.data[c->created.count++] = created;
            return true;
        }
        case OP_DESTROY:
            if (!cw_world_is_valid(w, id)) return false;
            cw_world_mark_destroy(w, id);
            return true;
        case OP_ADD: return cw_comp_add(w, id, ct);
        case OP_SET: return cw_comp_set(w, id, ct, payload, h->size);
        case OP_UPDATE: {
            if (!cw_comp_has(w, id, ct)) return false;
            ((cw_update_fn)h->fn)(w, id, ct, cw_comp_ptr(w, id, ct), payload, h->size);
            cw_comp_touch(w, id, ct);
            return true;
        }
        case OP_REMOVE: return cw_comp_remove(w, id, ct);
        case OP_CALL:
            ((cw_command_fn)h->fn)(w, payload, h->size);
            return true;
        default: return false;
    }
}
cw_flush_result cw_commands_flush(cw_commands *c, cw_world *w) {
    cw_flush_result r = {0, 0};
    if (!c || !w || c->flushing || !c->count) return r;   /* an empty flush is a no-op */
    c->flushing = true;
    c->created.count = 0;
    for (size_t offset = 0; offset < c->size;) {
        const cmd_header *h = (const cmd_header *)(c->data + offset);
        const void *payload = c->data + offset + align_up(sizeof(cmd_header));
        if (apply(c, w, h, payload)) ++r.applied; else ++r.failed;
        offset += align_up(sizeof(cmd_header)) + align_up(h->size);
    }
    c->size = c->count = c->pending = 0;
    c->last = r;
    c->flushing = false;
    return r;
}
cw_flush_result cw_commands_last_flush(const cw_commands *c) {
    cw_flush_result none = {0, 0};
    return c ? c->last : none;
}
cw_id_view cw_commands_created(const cw_commands *c) {
    cw_id_view view = {c ? c->created.data : NULL, c ? c->created.count : 0};
    return view;
}
cw_commands *cw_world_commands(cw_world *w) { return w ? &w->commands : NULL; }
