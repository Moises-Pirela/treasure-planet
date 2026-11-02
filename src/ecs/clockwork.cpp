#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#include "cw_internal.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static size_t page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    long page = sysconf(_SC_PAGESIZE);
    cw_require(page > 0);
    return (size_t)page;
#endif
}
static size_t page_round(size_t bytes) {
    size_t page = page_size();
    return cw_mul(cw_add(bytes, page - 1) / page, page);
}
static void *vm_reserve(size_t bytes) {
#ifdef _WIN32
    void *ptr = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
    cw_require(ptr != NULL);
#else
    void *ptr = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    cw_require(ptr != MAP_FAILED);
#endif
    return ptr;
}
static void vm_commit(void *ptr, size_t bytes) {
    if (!bytes) return;
#ifdef _WIN32
    cw_require(VirtualAlloc(ptr, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL);
#else
    cw_require(mprotect(ptr, bytes, PROT_READ | PROT_WRITE) == 0);
#endif
}
static void vm_release(void *ptr, size_t bytes) {
    if (!ptr) return;
#ifdef _WIN32
    (void)bytes;
    cw_require(VirtualFree(ptr, 0, MEM_RELEASE) != 0);
#else
    cw_require(munmap(ptr, bytes) == 0);
#endif
}
static void ids_reserve(cw_id_list *ids, size_t count) {
    if (count <= ids->capacity) return;
    size_t capacity = ids->capacity ? cw_mul(ids->capacity, 2) : 16;
    if (capacity < count) capacity = count;
    ids->data = CW_RESIZE(ids->data, capacity, sizeof(*ids->data));
    ids->capacity = capacity;
}
static void ids_push(cw_id_list *ids, cw_entity_id id) {
    ids_reserve(ids, cw_add(ids->count, 1));
    ids->data[ids->count++] = id;
}
static void ids_remove(cw_id_list *ids, cw_entity_id id) {
    for (size_t i = 0; i < ids->count; ++i) {
        if (ids->data[i] != id) continue;
        memmove(ids->data + i, ids->data + i + 1,
                (ids->count - i - 1) * sizeof(*ids->data));
        --ids->count;
        return;
    }
}
static cw_entity empty_entity(void) {
    cw_entity e = {0};
    e.id = CW_INVALID_ENTITY;
    return e;
}
static void pool_reserve(cw_component_pool *p, size_t count) {
    if (count <= p->capacity) return;
    size_t needed = page_round(cw_mul(count, p->elem_size));
    if (needed > p->reserved_bytes) {
        size_t reserved = p->reserved_bytes ? cw_mul(p->reserved_bytes, 2)
            : cw_mul(CW_POOL_RESERVE_ENTITIES, p->elem_size);
        if (reserved < needed) reserved = needed;
        reserved = page_round(reserved);
        void *data = vm_reserve(reserved);
        vm_commit(data, needed);
        if (p->data) {
            memcpy(data, p->data, cw_mul(p->capacity, p->elem_size));
            vm_release(p->data, p->reserved_bytes);
        }
        p->data = data;
        p->reserved_bytes = reserved;
    } else if (needed > p->committed_bytes) {
        vm_commit((unsigned char *)p->data + p->committed_bytes,
                  needed - p->committed_bytes);
    }
    size_t words = cw_add(count, 63) / 64;
    p->bitset = CW_RESIZE(p->bitset, words, sizeof(*p->bitset));
    memset(p->bitset + p->bitset_count, 0,
           (words - p->bitset_count) * sizeof(*p->bitset));
    p->bitset_count = words;
    p->committed_bytes = needed;
    p->capacity = count;
}
static void *pool_slot(cw_component_pool *p, size_t id) {
    return (unsigned char *)p->data + id * p->elem_size;
}
static void pool_clear(cw_component_pool *p, size_t id) {
    void *slot = pool_slot(p, id);
    if (p->clear) p->clear(slot);
    else memset(slot, 0, p->elem_size);
}
/* Cached query maintenance hooks, defined in the query section below. */
static void queries_entity_created(cw_world *w, cw_entity_id id);
static void queries_entity_changed(cw_world *w, cw_entity_id id);
static void queries_component_changed(cw_world *w, cw_entity_id id, cw_component_id ct);
static void queries_clear_all(cw_world *w);
static void queries_grow(cw_world *w, size_t words);
static void queries_release_all(cw_world *w);
/* Change observer hooks, defined in the change tracking section below. */
static void observers_record(cw_world *w, cw_entity_id id, cw_component_id ct, cw_change kind);
static void observers_grow(cw_world *w, size_t words);
static void observers_release_all(cw_world *w);

/* Adds the component if absent. Returns true when membership changed. */
static bool comp_mark(cw_world *w, cw_entity_id id, cw_component_id ct) {
    size_t i = (size_t)id;
    if (cw_sig_has(w->entities[i].signature, ct)) return false;
    w->pools[ct].bitset[i >> 6] |= UINT64_C(1) << (i & 63);
    cw_sig_set(&w->entities[i].signature, ct);
    queries_component_changed(w, id, ct);
    observers_record(w, id, ct, CW_CHANGE_ADDED);
    observers_record(w, id, ct, CW_CHANGE_MODIFIED);
    return true;
}
static void entity_clear(cw_world *w, cw_entity_id id) {
    /* Only the components the entity actually carries need removing. */
    for (unsigned wi = 0; wi < CW_SIGNATURE_WORDS; ++wi)
        for (uint64_t word = w->entities[id].signature.words[wi]; word; word &= word - 1)
            cw_comp_remove(w, id, (cw_component_id)(wi * 64 + cw_ctz64(word)));
}

bool cw_sig_set(cw_signature *sig, cw_component_id ct) {
    if (!sig || ct >= CW_MAX_COMPONENTS) return false;
    sig->words[ct >> 6] |= UINT64_C(1) << (ct & 63);
    return true;
}
bool cw_sig_clear(cw_signature *sig, cw_component_id ct) {
    if (!sig || ct >= CW_MAX_COMPONENTS) return false;
    sig->words[ct >> 6] &= ~(UINT64_C(1) << (ct & 63));
    return true;
}
bool cw_sig_has(cw_signature sig, cw_component_id ct) {
    return ct < CW_MAX_COMPONENTS && (sig.words[ct >> 6] & (UINT64_C(1) << (ct & 63))) != 0;
}
cw_signature cw_mask(const cw_component_id *types, size_t count) {
    cw_signature mask = {0};
    if (types) for (size_t i = 0; i < count; ++i) cw_sig_set(&mask, types[i]);
    return mask;
}
bool cw_sig_empty(cw_signature sig) {
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i) if (sig.words[i]) return false;
    return true;
}
bool cw_entity_is_active(cw_entity e) { return (e.flags & CW_ACTIVE) != 0; }
bool cw_query_visible(cw_entity e) {
    return cw_entity_is_active(e) && !(e.flags & CW_TO_DESTROY);
}
void cw_world_init(cw_world *w, void *user) {
    cw_require(w != NULL);
    memset(w, 0, sizeof(*w));
    w->user = user;
    w->last_created = CW_INVALID_ENTITY;
    cw_world_ensure_capacity(w, 0);
}
void cw_world_destroy(cw_world *w) {
    if (!w) return;
    cw_world_reset(w);
    queries_release_all(w);
    observers_release_all(w);
    cw_commands_destroy(&w->commands);
    free(w->visible);
    free(w->entities);
    free(w->recycled_ids.data);
    free(w->pending_destroy.data);
    free(w->created.data);
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        cw_component_pool *p = &w->pools[i];
        vm_release(p->data, p->reserved_bytes);
        free(p->bitset);
        free(p->name);
    }
    memset(w, 0, sizeof(*w));
}
void cw_world_ensure_capacity(cw_world *w, cw_entity_id id) {
    if (!w || id < 0) return;
    cw_require((uint64_t)id < SIZE_MAX);
    if ((size_t)id < w->capacity) return;
    size_t target = cw_mul(cw_add((size_t)id / CW_CHUNK_SIZE, 1), CW_CHUNK_SIZE);
    w->entities = CW_RESIZE(w->entities, target, sizeof(*w->entities));
    for (size_t i = w->capacity; i < target; ++i) w->entities[i] = empty_entity();
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i)
        if (w->pools[i].elem_size) pool_reserve(&w->pools[i], target);
    size_t old_words = cw_entity_words(w), words = cw_add(target, 63) / 64;
    w->visible = CW_RESIZE(w->visible, words, sizeof(*w->visible));
    memset(w->visible + old_words, 0, (words - old_words) * sizeof(*w->visible));
    queries_grow(w, words);
    observers_grow(w, words);
    w->capacity = target;
}
cw_entity_id cw_world_create_entity(cw_world *w) {
    if (!w) return CW_INVALID_ENTITY;
    cw_entity_id id;
    if (w->recycled_ids.count) id = w->recycled_ids.data[--w->recycled_ids.count];
    else {
        cw_require(w->next_id < INT64_MAX);
        id = w->next_id++;
    }
    cw_world_ensure_capacity(w, id);
    w->entities[id] = empty_entity();
    w->entities[id].id = id;
    w->entities[id].flags = CW_ACTIVE;
    w->last_created = id;
    ids_push(&w->created, id);
    queries_entity_created(w, id);
    return id;
}
bool cw_world_restore_entity(cw_world *w, cw_entity_id id) {
    if (!w || id < 0 || id == INT64_MAX) return false;
    cw_world_ensure_capacity(w, id);
    cw_entity *e = &w->entities[id];
    if (cw_entity_is_active(*e)) {
        if (e->flags & CW_TO_DESTROY) {
            e->flags &= (uint8_t)~CW_TO_DESTROY;
            ids_remove(&w->pending_destroy, id);
            queries_entity_changed(w, id);
        }
        return false;
    }
    *e = empty_entity();
    e->id = id;
    e->flags = CW_ACTIVE;
    ids_remove(&w->recycled_ids, id);
    if (id >= w->next_id) w->next_id = id + 1;
    w->last_created = id;
    ids_push(&w->created, id);
    queries_entity_created(w, id);
    return true;
}
bool cw_world_rebuild_entity(cw_world *w, cw_entity_id id) {
    if (!cw_world_is_valid(w, id)) return false;
    entity_clear(w, id);
    w->entities[id].flags = CW_ACTIVE;
    ids_remove(&w->pending_destroy, id);
    ids_push(&w->created, id);
    queries_entity_changed(w, id);
    return true;
}
void cw_world_mark_destroy(cw_world *w, cw_entity_id id) {
    if (!cw_world_is_valid(w, id) || (w->entities[id].flags & CW_TO_DESTROY)) return;
    w->entities[id].flags |= CW_TO_DESTROY;
    ids_push(&w->pending_destroy, id);
    queries_entity_changed(w, id);
}
bool cw_world_is_valid(const cw_world *w, cw_entity_id id) {
    return w && id >= 0 && (uint64_t)id < w->capacity && cw_entity_is_active(w->entities[id]);
}
size_t cw_world_entity_count(const cw_world *w) {
    size_t n = 0;
    if (w) for (size_t i = 0; i < w->capacity; ++i) n += cw_entity_is_active(w->entities[i]);
    return n;
}
void cw_world_reserve_entities(cw_world *w, size_t count) {
    if (!w) return;
    size_t additional = count > w->recycled_ids.count ? count - w->recycled_ids.count : 0;
    if (additional) {
        cw_require((uint64_t)additional <= (uint64_t)(INT64_MAX - w->next_id));
        cw_world_ensure_capacity(w, w->next_id + (cw_entity_id)additional - 1);
    }
    ids_reserve(&w->created, cw_add(w->created.count, count));
}
void cw_world_sweep(cw_world *w) {
    if (!w) return;
    for (size_t i = 0; i < w->pending_destroy.count; ++i) {
        cw_entity_id id = w->pending_destroy.data[i];
        if (!cw_world_is_valid(w, id) || !(w->entities[id].flags & CW_TO_DESTROY)) continue;
        /* Marked entities are already invisible, so the cache is untouched here. */
        entity_clear(w, id);
        w->entities[id] = empty_entity();
        if (w->recycle_ids) ids_push(&w->recycled_ids, id);
    }
    w->pending_destroy.count = 0;
}
void cw_world_reset(cw_world *w) {
    if (!w) return;
    for (size_t i = 0; i < w->capacity; ++i) {
        cw_entity *e = &w->entities[i];
        if (!cw_entity_is_active(*e) || (e->flags & CW_TO_DESTROY)) continue;
        e->flags |= CW_TO_DESTROY;
        ids_push(&w->pending_destroy, (cw_entity_id)i);
    }
    queries_clear_all(w);
    cw_commands_clear(&w->commands);
    cw_world_sweep(w);
    w->recycled_ids.count = w->created.count = 0;
    w->next_id = 0;
    w->last_created = CW_INVALID_ENTITY;
    w->tick = 0;
}
cw_id_view cw_world_created(const cw_world *w) {
    cw_id_view view = {w ? w->created.data : NULL, w ? w->created.count : 0};
    return view;
}
cw_id_view cw_world_pending_destroy(const cw_world *w) {
    cw_id_view view = {w ? w->pending_destroy.data : NULL, w ? w->pending_destroy.count : 0};
    return view;
}
cw_memory_stats cw_world_memory(const cw_world *w) {
    cw_memory_stats m = {0};
    if (!w) return m;
    m.entities = cw_mul(w->capacity, sizeof(*w->entities));
    m.bitsets = cw_mul(cw_entity_words(w), sizeof(*w->visible));
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        const cw_component_pool *p = &w->pools[i];
        if (!p->elem_size) continue;
        m.pool_committed = cw_add(m.pool_committed, p->committed_bytes);
        m.pool_reserved = cw_add(m.pool_reserved, p->reserved_bytes);
        m.bitsets = cw_add(m.bitsets, cw_mul(p->bitset_count, sizeof(*p->bitset)));
    }
    const cw_query_registry *r = &w->queries;
    m.queries = cw_mul(r->cache_capacity, sizeof(*r->caches));
    m.queries = cw_add(m.queries, cw_mul(r->handle_capacity, sizeof(*r->handles)));
    m.queries = cw_add(m.queries, cw_mul(r->handle_count, sizeof(**r->handles)));
    m.queries = cw_add(m.queries, cw_mul(r->lists_capacity, sizeof(*r->lists)));
    for (size_t i = 0; i < r->cache_count; ++i)
        m.queries = cw_add(m.queries, cw_add(sizeof(*r->caches[i]),
                                             cw_mul(r->caches[i]->words, sizeof(uint64_t))));
    const cw_observer_registry *o = &w->observers;
    m.observers = cw_mul(o->capacity, sizeof(*o->items));
    m.observers = cw_add(m.observers, cw_mul(o->lists_capacity, sizeof(*o->lists)));
    for (size_t i = 0; i < o->count; ++i)
        m.observers = cw_add(m.observers, cw_add(sizeof(*o->items[i]),
                                                 cw_mul(o->items[i]->words, 2 * CW_CHANGE_KINDS * sizeof(uint64_t))));
    m.commands = cw_add(w->commands.capacity, cw_mul(w->commands.created.capacity, sizeof(cw_entity_id)));
    m.lists = cw_mul(cw_add(cw_add(w->recycled_ids.capacity, w->pending_destroy.capacity),
                            w->created.capacity), sizeof(cw_entity_id));
    m.total = cw_add(cw_add(cw_add(cw_add(cw_add(cw_add(m.entities, m.pool_committed), m.bitsets), m.queries),
                            m.observers), m.commands), m.lists);
    return m;
}

cw_component_id cw_comp_block(cw_world *w, size_t count) {
    if (!w || !count || count > CW_MAX_COMPONENTS - w->next_block) return CW_INVALID_COMPONENT;
    cw_component_id base = (cw_component_id)w->next_block;
    w->next_block += (unsigned)count;
    return base;
}
cw_component_id cw_cid(cw_component_id base, unsigned offset) {
    if (base >= CW_MAX_COMPONENTS || offset >= CW_MAX_COMPONENTS - (unsigned)base)
        return CW_INVALID_COMPONENT;
    return (cw_component_id)(base + offset);
}
bool cw_comp_register(cw_world *w, cw_component_id ct, size_t size,
                      size_t alignment, const char *name, cw_component_fn clear) {
    if (!w || ct >= CW_MAX_COMPONENTS || w->pools[ct].elem_size) return false;
    if (!alignment || (alignment & (alignment - 1)) || alignment > page_size()) return false;
    if (!size) size = 1;
    if (size % alignment) return false;
    cw_component_pool *p = &w->pools[ct];
    p->elem_size = size;
    p->alignment = alignment;
    p->clear = clear;
    p->name = cw_copy_string(name && *name ? name : "?");
    pool_reserve(p, w->capacity > CW_CHUNK_SIZE ? w->capacity : CW_CHUNK_SIZE);
    return true;
}
bool cw_comp_register_at(cw_world *w, cw_component_id base, unsigned offset,
                         size_t size, size_t alignment, const char *name, cw_component_fn clear) {
    cw_component_id ct = cw_cid(base, offset);
    return w && ct < w->next_block && cw_comp_register(w, ct, size, alignment, name, clear);
}
void *cw_comp_ptr(cw_world *w, cw_entity_id id, cw_component_id ct) {
    if (!cw_registered(w, ct) || !cw_world_is_valid(w, id)) return NULL;
    return pool_slot(&w->pools[ct], (size_t)id);
}
void *cw_comp_get(cw_world *w, cw_entity_id id, cw_component_id ct, size_t size) {
    if (!cw_registered(w, ct) || w->pools[ct].elem_size != (size ? size : 1)) return NULL;
    return cw_comp_ptr(w, id, ct);
}
void *cw_comp_write(cw_world *w, cw_entity_id id, cw_component_id ct, size_t size) {
    void *slot = cw_comp_get(w, id, ct, size);
    if (slot && cw_sig_has(w->entities[id].signature, ct)) observers_record(w, id, ct, CW_CHANGE_MODIFIED);
    return slot;
}
bool cw_comp_touch(cw_world *w, cw_entity_id id, cw_component_id ct) {
    if (!cw_registered(w, ct) || !cw_comp_has(w, id, ct)) return false;
    observers_record(w, id, ct, CW_CHANGE_MODIFIED);
    return true;
}
bool cw_comp_set(cw_world *w, cw_entity_id id, cw_component_id ct,
                 const void *value, size_t size) {
    void *slot = cw_comp_get(w, id, ct, size);
    if (!slot || (!value && size)) return false;
    if (size) memmove(slot, value, size);
    if (!comp_mark(w, id, ct)) observers_record(w, id, ct, CW_CHANGE_MODIFIED);
    return true;
}
bool cw_comp_add(cw_world *w, cw_entity_id id, cw_component_id ct) {
    void *slot = cw_comp_ptr(w, id, ct);
    if (!slot) return false;
    if (!cw_comp_has(w, id, ct) && w->pools[ct].init) w->pools[ct].init(slot);
    comp_mark(w, id, ct);
    return true;
}
bool cw_comp_remove(cw_world *w, cw_entity_id id, cw_component_id ct) {
    if (!cw_registered(w, ct) || !cw_comp_has(w, id, ct)) return false;
    pool_clear(&w->pools[ct], (size_t)id);
    w->pools[ct].bitset[(size_t)id >> 6] &= ~(UINT64_C(1) << ((size_t)id & 63));
    cw_sig_clear(&w->entities[id].signature, ct);
    queries_component_changed(w, id, ct);
    observers_record(w, id, ct, CW_CHANGE_REMOVED);
    return true;
}
bool cw_comp_has(const cw_world *w, cw_entity_id id, cw_component_id ct) {
    return cw_world_is_valid(w, id) && cw_sig_has(w->entities[id].signature, ct);
}
bool cw_comp_set_init(cw_world *w, cw_component_id ct, cw_component_fn init) {
    if (!cw_registered(w, ct)) return false;
    w->pools[ct].init = init;
    return true;
}
const char *cw_comp_name(const cw_world *w, cw_component_id ct) {
    return cw_registered(w, ct) ? w->pools[ct].name : "?";
}
bool cw_comp_by_name(const cw_world *w, const char *name, cw_component_id *ct) {
    if (!w || !name || !ct) return false;
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        if (w->pools[i].elem_size && strcmp(w->pools[i].name, name) == 0) {
            *ct = (cw_component_id)i;
            return true;
        }
    }
    return false;
}
size_t cw_registered_components(const cw_world *w, cw_component_id *out, size_t capacity) {
    size_t n = 0;
    if (w) for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        if (!w->pools[i].elem_size) continue;
        if (out && n < capacity) out[n] = (cw_component_id)i;
        ++n;
    }
    return n;
}
bool cw_comp_set_codec(cw_world *w, cw_component_id ct, cw_codec codec) {
    if (!cw_registered(w, ct)) return false;
    w->pools[ct].codec = codec;
    return true;
}
bool cw_comp_set_transient(cw_world *w, cw_component_id ct, bool transient) {
    if (!cw_registered(w, ct)) return false;
    w->pools[ct].transient = transient;
    return true;
}
bool cw_comp_is_transient(const cw_world *w, cw_component_id ct) {
    return cw_registered(w, ct) && w->pools[ct].transient;
}
bool cw_comp_marshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                     void *data, size_t capacity, size_t *size) {
    if (size) *size = 0;
    if (!size || (!data && capacity) || !cw_registered(w, ct) || !cw_comp_has(w, id, ct)) return false;
    return w->pools[ct].codec.marshal && w->pools[ct].codec.marshal(w, id, ct, data, capacity, size);
}
bool cw_comp_unmarshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                       const void *data, size_t size) {
    if ((!data && size) || !cw_comp_ptr(w, id, ct) || !w->pools[ct].codec.unmarshal) return false;
    if (!w->pools[ct].codec.unmarshal(w, id, ct, data, size)) return false;
    if (!comp_mark(w, id, ct)) observers_record(w, id, ct, CW_CHANGE_MODIFIED);
    return true;
}


/* ------------------------------------------------------------------------- */
/* Filters                                                                   */
/* ------------------------------------------------------------------------- */
static bool sig_subset(cw_signature inner, cw_signature outer) {
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i)
        if (inner.words[i] & ~outer.words[i]) return false;
    return true;
}
static bool sig_intersects(cw_signature a, cw_signature b) {
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i) if (a.words[i] & b.words[i]) return true;
    return false;
}
/* Match against a signature only; visibility is checked by the caller. */
static bool filter_matches_sig(const cw_filter *f, bool any_empty, const cw_signature *sig) {
    bool any_hit = any_empty;
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i) {
        uint64_t word = sig->words[i];
        if ((word & f->all.words[i]) != f->all.words[i] || (word & f->none.words[i])) return false;
        any_hit |= (word & f->any.words[i]) != 0;
    }
    return any_hit;
}
cw_filter cw_filter_all(cw_signature all) {
    cw_filter f = {};
    f.all = all;
    return f;
}
cw_filter cw_filter_make(cw_signature all, cw_signature any, cw_signature none) {
    cw_filter f;
    f.all = all;
    f.any = any;
    f.none = none;
    return f;
}
bool cw_filter_valid(cw_filter f) {
    if (sig_intersects(f.all, f.none)) return false;
    return cw_sig_empty(f.any) || !sig_subset(f.any, f.none);
}
bool cw_filter_equal(cw_filter a, cw_filter b) {
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i)
        if (a.all.words[i] != b.all.words[i] || a.any.words[i] != b.any.words[i]
            || a.none.words[i] != b.none.words[i]) return false;
    return true;
}
bool cw_filter_matches(cw_filter f, cw_entity e) {
    return cw_query_visible(e) && filter_matches_sig(&f, cw_sig_empty(f.any), &e.signature);
}
bool cw_filter_disjoint(cw_filter a, cw_filter b) {
    /* A component one filter requires and the other forbids separates them;
     * so does an `any` set that the other filter forbids entirely. */
    if (sig_intersects(a.all, b.none) || sig_intersects(b.all, a.none)) return true;
    if (!cw_sig_empty(a.any) && sig_subset(a.any, b.none)) return true;
    return !cw_sig_empty(b.any) && sig_subset(b.any, a.none);
}

/* ------------------------------------------------------------------------- */
/* Access declarations                                                       */
/* ------------------------------------------------------------------------- */
static cw_signature sig_or(cw_signature a, cw_signature b) {
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i) a.words[i] |= b.words[i];
    return a;
}
static cw_signature sig_and(cw_signature a, cw_signature b) {
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i) a.words[i] &= b.words[i];
    return a;
}
cw_access cw_access_make(cw_signature reads, cw_signature writes) {
    cw_access a;
    a.reads = reads;
    a.writes = writes;
    return a;
}
cw_access cw_access_read(cw_signature reads) {
    cw_access a = {};
    a.reads = reads;
    return a;
}
cw_access cw_access_write(cw_signature writes) {
    cw_access a = {};
    a.writes = writes;
    return a;
}
cw_access cw_access_default(cw_filter f) { return cw_access_read(sig_or(f.all, f.any)); }
bool cw_access_equal(cw_access a, cw_access b) {
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i)
        if (a.reads.words[i] != b.reads.words[i] || a.writes.words[i] != b.writes.words[i]) return false;
    return true;
}
bool cw_access_valid(cw_filter f, cw_access a) { return !sig_intersects(a.reads, f.none); }
cw_signature cw_access_components(cw_access a) { return sig_or(a.reads, a.writes); }
cw_access cw_access_union(cw_access a, cw_access b) {
    return cw_access_make(sig_or(a.reads, b.reads), sig_or(a.writes, b.writes));
}
cw_signature cw_access_conflicts(cw_access a, cw_access b) {
    /* write/write and write/read overlaps, in either direction */
    return sig_or(sig_and(a.writes, cw_access_components(b)), sig_and(b.writes, a.reads));
}

/* ------------------------------------------------------------------------- */
/* Word-wise evaluation shared by ad-hoc iteration and cache rebuilds         */
/* ------------------------------------------------------------------------- */
static uint64_t pool_word(const cw_world *w, unsigned ct, size_t wi) {
    const cw_component_pool *p = &w->pools[ct];
    return p->elem_size && wi < p->bitset_count ? p->bitset[wi] : 0;
}
/* Matching bits for entities [wi * 64, wi * 64 + 64). */
static uint64_t filter_word(const cw_world *w, const cw_filter *f, bool any_empty, size_t wi) {
    uint64_t word = wi < cw_entity_words(w) ? w->visible[wi] : 0;
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS && word; ++i)
        for (uint64_t m = f->all.words[i]; m && word; m &= m - 1)
            word &= pool_word(w, i * 64 + cw_ctz64(m), wi);
    if (!any_empty && word) {
        uint64_t hit = 0;
        for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i)
            for (uint64_t m = f->any.words[i]; m; m &= m - 1)
                hit |= pool_word(w, i * 64 + cw_ctz64(m), wi);
        word &= hit;
    }
    for (unsigned i = 0; i < CW_SIGNATURE_WORDS && word; ++i)
        for (uint64_t m = f->none.words[i]; m && word; m &= m - 1)
            word &= ~pool_word(w, i * 64 + cw_ctz64(m), wi);
    return word;
}

/* ------------------------------------------------------------------------- */
/* Cached query maintenance                                                  */
/* ------------------------------------------------------------------------- */
static void cache_set_bit(cw_query_cache *q, size_t id, bool match) {
    uint64_t *word = &q->bits[id >> 6], bit = UINT64_C(1) << (id & 63);
    bool had = (*word & bit) != 0;
    if (match == had) return;
    if (match) { *word |= bit; ++q->count; }
    else { *word &= ~bit; --q->count; }
}
static void cache_rebuild(cw_query_cache *q) {
    cw_world *w = q->world;
    q->count = 0;
    for (size_t wi = 0; wi < q->words; ++wi) {
        q->bits[wi] = filter_word(w, &q->filter, q->any_empty, wi);
        q->count += cw_popcount64(q->bits[wi]);
    }
}
static void index_rebuild(cw_world *w) {
    cw_query_registry *r = &w->queries;
    size_t total = 0;
    memset(r->offsets, 0, sizeof(r->offsets));
    for (size_t i = 0; i < r->cache_count; ++i) {
        cw_query_cache *q = r->caches[i];
        for (unsigned ct = 0; ct < CW_MAX_COMPONENTS; ++ct)
            if (cw_sig_has(q->interest, (cw_component_id)ct)) { ++r->offsets[ct + 1]; ++total; }
        if (q->matches_empty) { ++r->offsets[CW_MAX_COMPONENTS + 1]; ++total; }
    }
    for (unsigned slot = 0; slot <= CW_MAX_COMPONENTS; ++slot) r->offsets[slot + 1] += r->offsets[slot];
    if (total > r->lists_capacity) {
        r->lists = CW_RESIZE(r->lists, total, sizeof(*r->lists));
        r->lists_capacity = total;
    }
    size_t fill[CW_MAX_COMPONENTS + 1];
    memcpy(fill, r->offsets, sizeof(fill));
    for (size_t i = 0; i < r->cache_count; ++i) {
        cw_query_cache *q = r->caches[i];
        for (unsigned ct = 0; ct < CW_MAX_COMPONENTS; ++ct)
            if (cw_sig_has(q->interest, (cw_component_id)ct)) r->lists[fill[ct]++] = q;
        if (q->matches_empty) r->lists[fill[CW_MAX_COMPONENTS]++] = q;
    }
    r->index_dirty = false;
}
static void index_range(cw_world *w, unsigned slot, cw_query_cache ***begin, cw_query_cache ***end) {
    cw_query_registry *r = &w->queries;
    if (r->index_dirty) index_rebuild(w);
    *begin = *end = NULL;
    if (!r->lists) return;
    *begin = r->lists + r->offsets[slot];
    *end = r->lists + r->offsets[slot + 1];
}
/* A fresh entity has no components and is visible; only queries that accept
 * component-less entities can match it, and no query holds a stale bit for a
 * slot that was inactive. */
static void queries_entity_created(cw_world *w, cw_entity_id id) {
    w->visible[(size_t)id >> 6] |= UINT64_C(1) << ((size_t)id & 63);
    if (!w->queries.cache_count) return;
    cw_query_cache **it, **end;
    index_range(w, CW_MAX_COMPONENTS, &it, &end);
    for (; it != end; ++it) cache_set_bit(*it, (size_t)id, true);
}
/* Visibility or the whole signature changed: re-evaluate every query. */
static void queries_entity_changed(cw_world *w, cw_entity_id id) {
    const cw_entity *e = &w->entities[id];
    bool visible = cw_query_visible(*e);
    uint64_t bit = UINT64_C(1) << ((size_t)id & 63);
    if (visible) w->visible[(size_t)id >> 6] |= bit;
    else w->visible[(size_t)id >> 6] &= ~bit;
    for (size_t i = 0; i < w->queries.cache_count; ++i) {
        cw_query_cache *q = w->queries.caches[i];
        cache_set_bit(q, (size_t)id, visible && filter_matches_sig(&q->filter, q->any_empty, &e->signature));
    }
}
static void queries_component_changed(cw_world *w, cw_entity_id id, cw_component_id ct) {
    const cw_entity *e = &w->entities[id];
    /* Invisible entities hold no bits, so only visible ones can be affected. */
    if (!w->queries.cache_count || !cw_query_visible(*e)) return;
    cw_query_cache **it, **end;
    index_range(w, ct, &it, &end);
    for (; it != end; ++it)
        cache_set_bit(*it, (size_t)id, filter_matches_sig(&(*it)->filter, (*it)->any_empty, &e->signature));
}
static void queries_clear_all(cw_world *w) {
    if (w->visible) memset(w->visible, 0, cw_entity_words(w) * sizeof(*w->visible));
    for (size_t i = 0; i < w->queries.cache_count; ++i) {
        cw_query_cache *q = w->queries.caches[i];
        memset(q->bits, 0, q->words * sizeof(*q->bits));
        q->count = 0;
    }
}
static void queries_grow(cw_world *w, size_t words) {
    for (size_t i = 0; i < w->queries.cache_count; ++i) {
        cw_query_cache *q = w->queries.caches[i];
        if (words <= q->words) continue;
        q->bits = CW_RESIZE(q->bits, words, sizeof(*q->bits));
        memset(q->bits + q->words, 0, (words - q->words) * sizeof(*q->bits));
        q->words = words;
    }
}
static void cache_free(cw_query_cache *q) {
    cw_query_registry *r = &q->world->queries;
    cw_query_cache *last = r->caches[--r->cache_count];
    r->caches[q->index] = last;
    last->index = q->index;
    r->index_dirty = true;
    free(q->bits);
    free(q);
}
static void handle_free(cw_cached_query *h) {
    cw_query_registry *r = &h->cache->world->queries;
    cw_cached_query *last = r->handles[--r->handle_count];
    r->handles[h->index] = last;
    last->index = h->index;
    cw_require(h->cache->refs > 0);
    if (--h->cache->refs == 0) cache_free(h->cache);
    free(h);
}
static void queries_release_all(cw_world *w) {
    while (w->queries.handle_count) handle_free(w->queries.handles[w->queries.handle_count - 1]);
    cw_require(w->queries.cache_count == 0);
    free(w->queries.caches);
    free(w->queries.handles);
    free(w->queries.lists);
    memset(&w->queries, 0, sizeof(w->queries));
}

/* ------------------------------------------------------------------------- */
/* Cached query API                                                          */
/* ------------------------------------------------------------------------- */
static cw_query_cache *cache_acquire(cw_world *w, cw_filter filter) {
    cw_query_registry *r = &w->queries;
    for (size_t i = 0; i < r->cache_count; ++i) {
        if (!cw_filter_equal(r->caches[i]->filter, filter)) continue;
        cw_require(r->caches[i]->refs != SIZE_MAX);
        ++r->caches[i]->refs;
        return r->caches[i];
    }
    if (r->cache_count == r->cache_capacity) {
        r->cache_capacity = r->cache_capacity ? cw_mul(r->cache_capacity, 2) : 8;
        r->caches = CW_RESIZE(r->caches, r->cache_capacity, sizeof(*r->caches));
    }
    cw_query_cache *c = (cw_query_cache *)cw_resize(NULL, 1, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->world = w;
    c->filter = filter;
    c->interest = sig_or(sig_or(filter.all, filter.any), filter.none);
    c->any_empty = cw_sig_empty(filter.any);
    c->matches_empty = c->any_empty && cw_sig_empty(filter.all);
    c->words = cw_entity_words(w);
    c->bits = (uint64_t *)cw_resize(NULL, c->words ? c->words : 1, sizeof(*c->bits));
    c->refs = 1;
    c->index = r->cache_count;
    r->caches[r->cache_count++] = c;
    r->index_dirty = true;
    cache_rebuild(c);
    return c;
}
cw_cached_query *cw_query_create(cw_world *w, cw_filter filter) {
    return cw_query_create_access(w, filter, cw_access_default(filter));
}
cw_cached_query *cw_query_create_access(cw_world *w, cw_filter filter, cw_access access) {
    if (!w || !cw_filter_valid(filter) || !cw_access_valid(filter, access)) return NULL;
    cw_query_registry *r = &w->queries;
    for (size_t i = 0; i < r->handle_count; ++i) {
        cw_cached_query *h = r->handles[i];
        if (!cw_filter_equal(h->cache->filter, filter) || !cw_access_equal(h->access, access)) continue;
        cw_require(h->refs != SIZE_MAX);
        ++h->refs;
        return h;
    }
    if (r->handle_count == r->handle_capacity) {
        r->handle_capacity = r->handle_capacity ? cw_mul(r->handle_capacity, 2) : 8;
        r->handles = CW_RESIZE(r->handles, r->handle_capacity, sizeof(*r->handles));
    }
    cw_cached_query *h = (cw_cached_query *)cw_resize(NULL, 1, sizeof(*h));
    memset(h, 0, sizeof(*h));
    h->cache = cache_acquire(w, filter);
    h->access = access;
    h->refs = 1;
    h->index = r->handle_count;
    r->handles[r->handle_count++] = h;
    return h;
}
void cw_query_destroy(cw_cached_query *h) {
    if (!h) return;
    cw_require(h->refs > 0);
    if (--h->refs == 0) handle_free(h);
}
cw_world *cw_query_world(const cw_cached_query *h) { return h ? h->cache->world : NULL; }
cw_filter cw_query_filter(const cw_cached_query *h) {
    cw_filter none = {};
    return h ? h->cache->filter : none;
}
cw_access cw_query_access(const cw_cached_query *h) {
    cw_access none = {};
    return h ? h->access : none;
}
size_t cw_query_refs(const cw_cached_query *h) { return h ? h->refs : 0; }
cw_signature cw_query_conflicts(const cw_cached_query *a, const cw_cached_query *b) {
    cw_signature none = {0};
    if (!a || !b || a->cache->world != b->cache->world) return none;
    if (cw_filter_disjoint(a->cache->filter, b->cache->filter)) return none;
    return cw_access_conflicts(a->access, b->access);
}
bool cw_query_shares_cache(const cw_cached_query *a, const cw_cached_query *b) {
    return a && b && a->cache == b->cache;
}
void *cw_query_get(const cw_cached_query *h, cw_entity_id id, cw_component_id ct, size_t size) {
    if (!h || !cw_query_contains(h, id) || !cw_sig_has(cw_access_components(h->access), ct)) return NULL;
    cw_world *w = h->cache->world;
    if (!cw_sig_has(w->entities[id].signature, ct)) return NULL;
    return cw_comp_get(w, id, ct, size);
}
void *cw_query_write(cw_cached_query *h, cw_entity_id id, cw_component_id ct, size_t size) {
    if (!h || !cw_sig_has(h->access.writes, ct)) return NULL;
    void *slot = cw_query_get(h, id, ct, size);
    if (slot) observers_record(h->cache->world, id, ct, CW_CHANGE_MODIFIED);
    return slot;
}
size_t cw_query_count(const cw_cached_query *h) { return h ? h->cache->count : 0; }
bool cw_query_is_empty(const cw_cached_query *h) { return !h || h->cache->count == 0; }
bool cw_query_first(const cw_cached_query *h, cw_entity_id *id) {
    if (id) *id = CW_INVALID_ENTITY;
    if (!h || !id || !h->cache->count) return false;
    const cw_query_cache *c = h->cache;
    for (size_t wi = 0; wi < c->words; ++wi) {
        if (!c->bits[wi]) continue;
        *id = (cw_entity_id)(wi * 64 + cw_ctz64(c->bits[wi]));
        return true;
    }
    return false;
}
cw_single cw_query_single(const cw_cached_query *h, cw_entity_id *id) {
    cw_entity_id first;
    if (!cw_query_first(h, &first)) { if (id) *id = CW_INVALID_ENTITY; return CW_SINGLE_NONE; }
    if (id) *id = first;
    return h->cache->count == 1 ? CW_SINGLE_ONE : CW_SINGLE_MANY;
}
bool cw_query_contains(const cw_cached_query *h, cw_entity_id id) {
    if (!h || id < 0 || (uint64_t)id >= (uint64_t)h->cache->words * 64) return false;
    return (h->cache->bits[(size_t)id >> 6] >> ((size_t)id & 63)) & 1;
}
size_t cw_query_gather(const cw_cached_query *h, cw_entity_id *out, size_t capacity) {
    if (!h) return 0;
    const cw_query_cache *c = h->cache;
    size_t n = 0;
    for (size_t wi = 0; wi < c->words; ++wi) {
        for (uint64_t word = c->bits[wi]; word; word &= word - 1) {
            if (out && n < capacity) out[n] = (cw_entity_id)(wi * 64 + cw_ctz64(word));
            ++n;
        }
    }
    return n;
}
const uint64_t *cw_query_bits(const cw_cached_query *h, size_t *words) {
    if (words) *words = h ? h->cache->words : 0;
    return h ? h->cache->bits : NULL;
}
cw_query cw_query_begin(cw_cached_query *h) {
    cw_query it = {0};
    if (!h) return it;
    it.world = h->cache->world;
    it.filter = h->cache->filter;
    it.cache = h->cache;
    return it;
}
size_t cw_query_each(cw_cached_query *h, cw_query_fn fn, void *user) {
    if (!h || !fn) return 0;
    cw_query it = cw_query_begin(h);
    cw_entity_id id;
    size_t n = 0;
    while (cw_query_next(&it, &id)) {
        ++n;
        if (!fn(h->cache->world, id, user)) break;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Iteration                                                                 */
/* ------------------------------------------------------------------------- */
cw_query cw_query_iter(cw_world *w, cw_signature mask) {
    return cw_query_iter_filter(w, cw_filter_all(mask));
}
cw_query cw_query_iter_filter(cw_world *w, cw_filter filter) {
    cw_query q = {0};
    q.world = w;
    q.filter = filter;
    return q;
}
/* Bitset-backed iteration (cached queries and observers): the bitset is read
 * live from the cursor, so mutations made during the walk are honoured
 * immediately, and the source is re-fetched each step because the world may
 * grow (and reallocate) while walking. */
static bool bitset_next(cw_query *q, cw_entity_id *id) {
    for (;;) {
        const uint64_t *bits;
        size_t words;
        if (q->cache) { bits = q->cache->bits; words = q->cache->words; }
        else { bits = q->observer->observed[q->kind]; words = q->observer->words; }
        size_t wi = q->base >> 6;
        if (wi >= words) return false;
        uint64_t word = bits[wi] & (UINT64_MAX << (q->base & 63));
        while (!word) {
            if (++wi >= words) { q->base = words * 64; return false; }
            word = bits[wi];
        }
        size_t found = wi * 64 + cw_ctz64(word);
        q->base = found + 1;
        /* Added/modified walks only yield entities that still carry the component. */
        if (q->observer && q->kind != CW_CHANGE_REMOVED) {
            const cw_entity *e = found < q->world->capacity ? &q->world->entities[found] : NULL;
            if (!e || !cw_query_visible(*e) || !cw_sig_has(e->signature, q->observer->component)) continue;
        }
        *id = (cw_entity_id)found;
        return true;
    }
}
bool cw_query_next(cw_query *q, cw_entity_id *id) {
    if (id) *id = CW_INVALID_ENTITY;
    if (!q || !q->world || !id) return false;
    if (q->cache || q->observer) return bitset_next(q, id);
    cw_world *w = q->world;
    bool any_empty = cw_sig_empty(q->filter.any);
    for (;;) {
        while (q->current_word) {
            unsigned bit = cw_ctz64(q->current_word);
            q->current_word &= q->current_word - 1;
            size_t candidate = q->base + bit;
            /* Re-verify: the word is a snapshot and the walk may have mutated the world. */
            if (candidate >= w->capacity || !cw_query_visible(w->entities[candidate])
                || !filter_matches_sig(&q->filter, any_empty, &w->entities[candidate].signature)) continue;
            *id = (cw_entity_id)candidate;
            return true;
        }
        if (q->word_index >= cw_entity_words(w)) return false;
        q->base = q->word_index * 64;
        q->current_word = filter_word(w, &q->filter, any_empty, q->word_index++);
    }
}
size_t cw_query_collect(cw_world *w, cw_signature mask, cw_entity_id *out, size_t capacity) {
    return cw_query_collect_filter(w, cw_filter_all(mask), out, capacity);
}
bool cw_query_matches(const cw_query *it, cw_entity_id id) {
    if (!it || !it->world || id < 0 || (uint64_t)id >= it->world->capacity) return false;
    if (it->cache)
        return (it->cache->bits[(size_t)id >> 6] >> ((size_t)id & 63)) & 1;
    if (it->observer) {
        if (!cw_observer_has(it->observer, it->kind, id)) return false;
        if (it->kind == CW_CHANGE_REMOVED) return true;
        const cw_entity *e = &it->world->entities[id];
        return cw_query_visible(*e) && cw_sig_has(e->signature, it->observer->component);
    }
    return cw_filter_matches(it->filter, it->world->entities[id]);
}
bool cw_query_first_filter(cw_world *w, cw_filter filter, cw_entity_id *id) {
    cw_query q = cw_query_iter_filter(w, filter);
    return cw_query_next(&q, id);
}
cw_single cw_query_single_filter(cw_world *w, cw_filter filter, cw_entity_id *id) {
    cw_query q = cw_query_iter_filter(w, filter);
    cw_entity_id first, second;
    if (!cw_query_next(&q, &first)) { if (id) *id = CW_INVALID_ENTITY; return CW_SINGLE_NONE; }
    if (id) *id = first;
    return cw_query_next(&q, &second) ? CW_SINGLE_MANY : CW_SINGLE_ONE;
}
size_t cw_query_count_filter(cw_world *w, cw_filter filter, size_t limit) {
    cw_query q = cw_query_iter_filter(w, filter);
    cw_entity_id id;
    size_t n = 0;
    while (n < limit && cw_query_next(&q, &id)) ++n;
    return n;
}
size_t cw_query_collect_filter(cw_world *w, cw_filter filter, cw_entity_id *out, size_t capacity) {
    cw_query q = cw_query_iter_filter(w, filter);
    cw_entity_id id;
    size_t n = 0;
    while (cw_query_next(&q, &id)) {
        if (out && n < capacity) out[n] = id;
        ++n;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Change tracking                                                           */
/* ------------------------------------------------------------------------- */
static void observer_index_rebuild(cw_world *w) {
    cw_observer_registry *r = &w->observers;
    memset(r->offsets, 0, sizeof(r->offsets));
    for (size_t i = 0; i < r->count; ++i) ++r->offsets[r->items[i]->component + 1];
    for (unsigned ct = 0; ct < CW_MAX_COMPONENTS; ++ct) r->offsets[ct + 1] += r->offsets[ct];
    if (r->count > r->lists_capacity) {
        r->lists = CW_RESIZE(r->lists, r->count, sizeof(*r->lists));
        r->lists_capacity = r->count;
    }
    size_t fill[CW_MAX_COMPONENTS];
    memcpy(fill, r->offsets, sizeof(fill));
    for (size_t i = 0; i < r->count; ++i) r->lists[fill[r->items[i]->component]++] = r->items[i];
    r->index_dirty = false;
}
static void observers_record(cw_world *w, cw_entity_id id, cw_component_id ct, cw_change kind) {
    cw_observer_registry *r = &w->observers;
    if (!r->count) return;
    if (r->index_dirty) observer_index_rebuild(w);
    if (!r->lists) return;
    uint64_t bit = UINT64_C(1) << ((size_t)id & 63);
    size_t wi = (size_t)id >> 6;
    for (size_t i = r->offsets[ct]; i < r->offsets[ct + 1]; ++i) {
        cw_observer *o = r->lists[i];
        uint64_t *word = &o->pending[kind][wi];
        if (*word & bit) continue;
        *word |= bit;
        ++o->pending_count[kind];
    }
}
static void observers_grow(cw_world *w, size_t words) {
    for (size_t i = 0; i < w->observers.count; ++i) {
        cw_observer *o = w->observers.items[i];
        if (words <= o->words) continue;
        for (unsigned k = 0; k < CW_CHANGE_KINDS; ++k) {
            o->pending[k] = (uint64_t *)cw_resize(o->pending[k], words, sizeof(uint64_t));
            memset(o->pending[k] + o->words, 0, (words - o->words) * sizeof(uint64_t));
            o->observed[k] = (uint64_t *)cw_resize(o->observed[k], words, sizeof(uint64_t));
            memset(o->observed[k] + o->words, 0, (words - o->words) * sizeof(uint64_t));
        }
        o->words = words;
    }
}
static void observer_free(cw_observer *o) {
    cw_observer_registry *r = &o->world->observers;
    cw_observer *last = r->items[--r->count];
    r->items[o->index] = last;
    last->index = o->index;
    r->index_dirty = true;
    for (unsigned k = 0; k < CW_CHANGE_KINDS; ++k) { free(o->pending[k]); free(o->observed[k]); }
    free(o);
}
static void observers_release_all(cw_world *w) {
    while (w->observers.count) observer_free(w->observers.items[w->observers.count - 1]);
    free(w->observers.items);
    free(w->observers.lists);
    memset(&w->observers, 0, sizeof(w->observers));
}
cw_observer *cw_observer_create(cw_world *w, cw_component_id ct) {
    if (!w || ct >= CW_MAX_COMPONENTS) return NULL;
    cw_observer_registry *r = &w->observers;
    if (r->count == r->capacity) {
        r->capacity = r->capacity ? cw_mul(r->capacity, 2) : 8;
        r->items = CW_RESIZE(r->items, r->capacity, sizeof(*r->items));
    }
    cw_observer *o = (cw_observer *)cw_resize(NULL, 1, sizeof(*o));
    memset(o, 0, sizeof(*o));
    o->world = w;
    o->component = ct;
    o->words = cw_entity_words(w);
    for (unsigned k = 0; k < CW_CHANGE_KINDS; ++k) {
        o->pending[k] = (uint64_t *)cw_resize(NULL, o->words ? o->words : 1, sizeof(uint64_t));
        o->observed[k] = (uint64_t *)cw_resize(NULL, o->words ? o->words : 1, sizeof(uint64_t));
        memset(o->pending[k], 0, o->words * sizeof(uint64_t));
        memset(o->observed[k], 0, o->words * sizeof(uint64_t));
    }
    o->index = r->count;
    r->items[r->count++] = o;
    r->index_dirty = true;
    return o;
}
void cw_observer_destroy(cw_observer *o) { if (o) observer_free(o); }
cw_world *cw_observer_world(const cw_observer *o) { return o ? o->world : NULL; }
cw_component_id cw_observer_component(const cw_observer *o) {
    return o ? o->component : CW_INVALID_COMPONENT;
}
void cw_observer_observe(cw_observer *o) {
    if (!o) return;
    for (unsigned k = 0; k < CW_CHANGE_KINDS; ++k) {
        /* The old observed set becomes the (cleared) accumulator. */
        uint64_t *recycled = o->observed[k];
        o->observed[k] = o->pending[k];
        o->observed_count[k] = o->pending_count[k];
        memset(recycled, 0, o->words * sizeof(uint64_t));
        o->pending[k] = recycled;
        o->pending_count[k] = 0;
    }
    ++o->observations;
}
uint64_t cw_observer_observations(const cw_observer *o) { return o ? o->observations : 0; }
size_t cw_observer_count(const cw_observer *o, cw_change kind) {
    return o && kind < CW_CHANGE_KINDS ? o->observed_count[kind] : 0;
}
size_t cw_observer_pending(const cw_observer *o, cw_change kind) {
    return o && kind < CW_CHANGE_KINDS ? o->pending_count[kind] : 0;
}
bool cw_observer_has(const cw_observer *o, cw_change kind, cw_entity_id id) {
    if (!o || kind >= CW_CHANGE_KINDS || id < 0 || (uint64_t)id >= (uint64_t)o->words * 64) return false;
    return (o->observed[kind][(size_t)id >> 6] >> ((size_t)id & 63)) & 1;
}
const uint64_t *cw_observer_bits(const cw_observer *o, cw_change kind, size_t *words) {
    bool ok = o && kind < CW_CHANGE_KINDS;
    if (words) *words = ok ? o->words : 0;
    return ok ? o->observed[kind] : NULL;
}
cw_query cw_observer_iter(cw_observer *o, cw_change kind) {
    cw_query it = {0};
    if (!o || kind >= CW_CHANGE_KINDS) return it;
    it.world = o->world;
    it.observer = o;
    it.kind = kind;
    return it;
}
size_t cw_observer_gather(const cw_observer *o, cw_change kind, cw_entity_id *out, size_t capacity) {
    if (!o || kind >= CW_CHANGE_KINDS) return 0;
    size_t n = 0;
    for (size_t wi = 0; wi < o->words; ++wi) {
        for (uint64_t word = o->observed[kind][wi]; word; word &= word - 1) {
            if (out && n < capacity) out[n] = (cw_entity_id)(wi * 64 + cw_ctz64(word));
            ++n;
        }
    }
    return n;
}
cw_access cw_observer_access(const cw_observer *o) {
    cw_access a = {};
    if (o) cw_sig_set(&a.reads, o->component);
    return a;
}
cw_signature cw_observer_conflicts(const cw_observer *o, const cw_cached_query *q) {
    cw_signature none = {0};
    if (!o || !q || o->world != q->cache->world) return none;
    return cw_access_conflicts(cw_observer_access(o), q->access);
}
