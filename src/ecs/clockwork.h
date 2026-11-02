/* Ported to C-style C++ from clockwork-c (github.com/Moises-Pirela/clockwork-c, 4602476).
 * The ECS core only: reflection, scenes, game modules and assets stayed behind. */
#ifndef CLOCKWORK_H
#define CLOCKWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CW_CHUNK_SIZE 256
#define CW_MAX_COMPONENTS 192
#define CW_SIGNATURE_WORDS ((CW_MAX_COMPONENTS + 63) / 64)
#ifndef CW_POOL_RESERVE_ENTITIES
#define CW_POOL_RESERVE_ENTITIES ((size_t)1 << 24)
#endif

typedef int64_t cw_entity_id;
typedef uint8_t cw_component_id;
#define CW_INVALID_ENTITY INT64_C(-1)
#define CW_INVALID_COMPONENT UINT8_MAX

enum { CW_ACTIVE = 1, CW_DEAD = 2, CW_TO_DESTROY = 4 };
typedef struct { uint64_t words[CW_SIGNATURE_WORDS]; } cw_signature;
typedef struct {
    cw_entity_id id;
    uint8_t flags;
    cw_signature signature;
} cw_entity;

typedef struct cw_world cw_world;
typedef void (*cw_component_fn)(void *slot);
typedef struct {
    bool (*marshal)(cw_world *, cw_entity_id, cw_component_id,
                    void *data, size_t capacity, size_t *size);
    bool (*unmarshal)(cw_world *, cw_entity_id, cw_component_id,
                      const void *data, size_t size);
} cw_codec;

typedef struct {
    void *data;
    uint64_t *bitset;
    size_t elem_size, alignment, capacity, bitset_count;
    size_t reserved_bytes, committed_bytes;
    char *name;
    cw_component_fn clear, init;
    cw_codec codec;
    bool transient;
} cw_component_pool;

typedef struct { cw_entity_id *data; size_t count, capacity; } cw_id_list;
typedef struct { const cw_entity_id *data; size_t count; } cw_id_view;

/* Cached queries owned by a world; see the query section below. A cache is
 * the entity bitset for one filter and is shared by every handle that uses
 * that filter; a handle adds the declared component access. */
typedef struct cw_query_cache cw_query_cache;
typedef struct cw_cached_query cw_cached_query;
typedef struct {
    cw_query_cache **caches;     /* one per distinct filter, unordered */
    size_t cache_count, cache_capacity;
    cw_cached_query **handles;   /* one per distinct filter + access, unordered */
    size_t handle_count, handle_capacity;
    /* Interest index: caches whose filter mentions component ct live in
     * lists[offsets[ct] .. offsets[ct + 1]); the extra slot at
     * CW_MAX_COMPONENTS holds caches that match component-less entities. */
    cw_query_cache **lists;
    size_t offsets[CW_MAX_COMPONENTS + 2];
    size_t lists_capacity;
    bool index_dirty;
} cw_query_registry;

/* Change observer owned by a world; see the change tracking section below. */
typedef struct cw_observer cw_observer;
typedef struct {
    cw_observer **items;         /* every live observer, unordered */
    size_t count, capacity;
    /* Index: observers of component ct live in lists[offsets[ct] .. offsets[ct + 1]). */
    cw_observer **lists;
    size_t offsets[CW_MAX_COMPONENTS + 1];
    size_t lists_capacity;
    bool index_dirty;
} cw_observer_registry;

/* Deferred mutations; see the command buffer section below. */
typedef struct { size_t applied, failed; } cw_flush_result;
typedef struct {
    unsigned char *data;         /* command stream */
    size_t size, capacity;
    size_t count;                /* commands recorded */
    size_t pending;              /* entities created by this buffer, not yet flushed */
    cw_id_list created;          /* real ids of the entities created by the last flush */
    cw_flush_result last;        /* result of the last non-empty flush */
    bool flushing;
} cw_commands;

struct cw_world {
    cw_entity *entities;
    size_t capacity;
    cw_id_list recycled_ids, pending_destroy, created;
    cw_component_pool pools[CW_MAX_COMPONENTS];
    cw_entity_id next_id, last_created;
    unsigned next_block;
    uint64_t tick;
    bool recycle_ids;
    void *user;                  /* the application's (a game module's) per-world state */
    const void *host;            /* context the host program attaches (cw_game: the asset registry) */
    uint64_t *visible;           /* one bit per entity: active and not pending destruction */
    cw_query_registry queries;
    cw_observer_registry observers;
    cw_commands commands;        /* default buffer, flushed by cw_sim_dispatch */
};

bool cw_sig_set(cw_signature *sig, cw_component_id ct);
bool cw_sig_clear(cw_signature *sig, cw_component_id ct);
bool cw_sig_has(cw_signature sig, cw_component_id ct);
bool cw_sig_empty(cw_signature sig);
cw_signature cw_mask(const cw_component_id *types, size_t count);
bool cw_entity_is_active(cw_entity entity);
bool cw_query_visible(cw_entity entity);

void cw_world_init(cw_world *w, void *user);
void cw_world_destroy(cw_world *w);
void cw_world_reset(cw_world *w);
cw_entity_id cw_world_create_entity(cw_world *w);
bool cw_world_restore_entity(cw_world *w, cw_entity_id id);
bool cw_world_rebuild_entity(cw_world *w, cw_entity_id id);
void cw_world_mark_destroy(cw_world *w, cw_entity_id id);
void cw_world_sweep(cw_world *w);
bool cw_world_is_valid(const cw_world *w, cw_entity_id id);
size_t cw_world_entity_count(const cw_world *w);
void cw_world_ensure_capacity(cw_world *w, cw_entity_id id);
void cw_world_reserve_entities(cw_world *w, size_t count);
cw_id_view cw_world_created(const cw_world *w);
cw_id_view cw_world_pending_destroy(const cw_world *w);

/* Heap and virtual memory owned by a world, in bytes. */
typedef struct {
    size_t entities;        /* entity table */
    size_t pool_committed;  /* component storage backed by committed pages */
    size_t pool_reserved;   /* component address space reserved (not counted in total) */
    size_t bitsets;         /* component membership bitsets and the visibility bitset */
    size_t queries;         /* cached query bitsets, registry and interest index */
    size_t observers;       /* change observer bitsets, registry and index */
    size_t commands;        /* the world's default command buffer */
    size_t lists;           /* recycled, pending-destroy and created id lists */
    size_t total;           /* everything above except pool_reserved */
} cw_memory_stats;
cw_memory_stats cw_world_memory(const cw_world *w);

cw_component_id cw_comp_block(cw_world *w, size_t count);
cw_component_id cw_cid(cw_component_id base, unsigned offset);
bool cw_comp_register(cw_world *w, cw_component_id ct, size_t size,
                      size_t alignment, const char *name, cw_component_fn clear);
bool cw_comp_register_at(cw_world *w, cw_component_id base, unsigned offset,
                         size_t size, size_t alignment, const char *name,
                         cw_component_fn clear);
#define CW_REGISTER(w, ct, T) \
    cw_comp_register((w), (ct), sizeof(T), alignof(T), #T, NULL)
void *cw_comp_ptr(cw_world *w, cw_entity_id id, cw_component_id ct);
void *cw_comp_get(cw_world *w, cw_entity_id id, cw_component_id ct, size_t size);
#define CW_GET(w, id, ct, T) ((T *)cw_comp_get((w), (id), (ct), sizeof(T)))
/* Like cw_comp_get, but records a modification for change observers. */
void *cw_comp_write(cw_world *w, cw_entity_id id, cw_component_id ct, size_t size);
#define CW_WRITE(w, id, ct, T) ((T *)cw_comp_write((w), (id), (ct), sizeof(T)))
/* Record a modification made through a previously obtained pointer. */
bool cw_comp_touch(cw_world *w, cw_entity_id id, cw_component_id ct);
bool cw_comp_set(cw_world *w, cw_entity_id id, cw_component_id ct,
                 const void *value, size_t size);
bool cw_comp_add(cw_world *w, cw_entity_id id, cw_component_id ct);
bool cw_comp_remove(cw_world *w, cw_entity_id id, cw_component_id ct);
bool cw_comp_has(const cw_world *w, cw_entity_id id, cw_component_id ct);
bool cw_comp_set_init(cw_world *w, cw_component_id ct, cw_component_fn init);
const char *cw_comp_name(const cw_world *w, cw_component_id ct);
bool cw_comp_by_name(const cw_world *w, const char *name, cw_component_id *ct);
size_t cw_registered_components(const cw_world *w, cw_component_id *out, size_t capacity);
bool cw_comp_set_codec(cw_world *w, cw_component_id ct, cw_codec codec);
bool cw_comp_set_transient(cw_world *w, cw_component_id ct, bool transient);
bool cw_comp_is_transient(const cw_world *w, cw_component_id ct);
bool cw_comp_marshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                     void *data, size_t capacity, size_t *size);
bool cw_comp_unmarshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                       const void *data, size_t size);

/* ---------------------------------------------------------------------------
 * Queries
 *
 * A filter selects visible entities (active and not marked for destruction)
 * whose signature contains every component in `all`, at least one component
 * in `any` (ignored when `any` is empty) and no component in `none`.
 * Components named by a filter need not be registered yet; an unregistered
 * component is simply never present.
 *
 * Ad-hoc queries (cw_query_iter / cw_query_iter_filter) read the component
 * pools directly and carry no state beyond the iterator value.
 *
 * Cached queries (cw_query_create) keep a bitset of the matching entities and
 * an O(1) count that the world maintains eagerly on every entity lifecycle
 * change (create, restore, rebuild, mark-destroy, sweep, reset) and every
 * component membership change (add/set/unmarshal of a new component, remove).
 * Only queries whose filter mentions the changed component are touched. The
 * cache grows automatically with the world and is unaffected by late component
 * registration or pool growth.
 *
 * Access declarations: every cached query declares which components the
 * code driving it reads and writes (cw_access). Declarations are not
 * enforced at access time; they are metadata for scheduling and conflict
 * detection: two queries conflict on a component when one writes it and the
 * other reads or writes it, unless their filters are provably disjoint
 * (cw_filter_disjoint). Writes may name any component, including ones the
 * filter excludes (adding a component is a write); reads may not name a
 * component in `none`, since it is never present. cw_query_create declares
 * the conservative default of reading every component in `all` and `any`
 * and writing nothing; use cw_query_create_access to declare precisely.
 *
 * Ownership and lifetime:
 *   - Cached queries are allocated and owned by the world. cw_query_create
 *     returns a stable handle; creating an identical filter and access twice
 *     returns the same handle with its reference count bumped, and handles
 *     with the same filter but different access share one underlying entity
 *     cache. Every successful create must be paired with cw_query_destroy,
 *     which releases the handle (and the cache, once unused) when the last
 *     reference is gone. cw_world_destroy releases every remaining query and
 *     invalidates all handles. cw_world_reset keeps queries alive (they
 *     simply become empty), since they describe schema, not entities.
 *   - A cw_query iterator is a plain value that borrows the world (and the
 *     cached query, if any). It needs no cleanup and may be copied freely. It
 *     must not outlive its world or cached query.
 *
 * Mutation during iteration is safe for everything except destroying the
 * world or the cached query being iterated: entities may be created, marked,
 * swept, restored, rebuilt, and components may be added or removed. An entity
 * that stops matching before it is reached is never yielded. An entity that
 * starts matching is yielded by a cached iteration if its id is above the
 * cursor (the live bitset is read on every step); an ad-hoc iteration
 * snapshots 64 entities at a time and may miss one that starts matching inside
 * the current block. Ids are yielded in ascending order. Creating or
 * destroying other queries from inside an iteration is allowed.
 *
 * Allocation-free use: a cw_query iterator is a stack value and no query
 * function allocates after the cached query has been created, so walking,
 * counting, testing membership and fetching components are safe in hot
 * loops. Prefer the early-exit helpers over collecting: cw_query_is_empty
 * and cw_query_count are O(1) on cached queries; cw_query_first,
 * cw_query_single and cw_query_count_filter stop as soon as the answer is
 * known; cw_query_matches tests one entity against any iterator.
 *
 * Component access: cw_comp_ptr/cw_comp_get/CW_GET work on any id a query
 * yields. cw_query_get/cw_query_write (CW_QUERY_GET/CW_QUERY_WRITE) are the
 * checked forms: they return NULL unless the entity is in the query, carries
 * the component, and the component is covered by the query's declared access
 * (reads or writes for get, writes for write), and cw_query_write records a
 * modification for observers. Pool storage is virtually reserved for CW_POOL_RESERVE_ENTITIES
 * entities, so component pointers stay valid across entity creation and world
 * growth up to that size; beyond it, growth may relocate a pool and every
 * pointer into it must be re-fetched. Pointers never survive cw_comp_remove,
 * sweep of that entity, or cw_world_destroy. Queries are single-threaded.
 * ------------------------------------------------------------------------- */
typedef struct { cw_signature all, any, none; } cw_filter;
cw_filter cw_filter_all(cw_signature all);
cw_filter cw_filter_make(cw_signature all, cw_signature any, cw_signature none);
bool cw_filter_valid(cw_filter filter);
bool cw_filter_equal(cw_filter a, cw_filter b);
bool cw_filter_matches(cw_filter filter, cw_entity entity);
/* True when no entity can ever match both filters. */
bool cw_filter_disjoint(cw_filter a, cw_filter b);

typedef struct { cw_signature reads, writes; } cw_access;
cw_access cw_access_make(cw_signature reads, cw_signature writes);
cw_access cw_access_read(cw_signature reads);
cw_access cw_access_write(cw_signature writes);
cw_access cw_access_default(cw_filter filter);   /* reads all | any, writes nothing */
bool cw_access_equal(cw_access a, cw_access b);
bool cw_access_valid(cw_filter filter, cw_access access);
cw_signature cw_access_components(cw_access access);          /* reads | writes */
cw_access cw_access_union(cw_access a, cw_access b);          /* combined declaration */
cw_signature cw_access_conflicts(cw_access a, cw_access b);   /* components in conflict */

typedef enum { CW_CHANGE_ADDED, CW_CHANGE_MODIFIED, CW_CHANGE_REMOVED, CW_CHANGE_KINDS } cw_change;
typedef struct {
    cw_world *world;
    cw_filter filter;
    cw_query_cache *cache;      /* cached iteration, else NULL */
    cw_observer *observer;      /* change iteration, else NULL */
    cw_change kind;
    size_t word_index, base;    /* bitset iteration: base is the next id to examine */
    uint64_t current_word;
} cw_query;
/* Ad-hoc queries. cw_query_iter(w, mask) is equivalent to cw_filter_all(mask). */
cw_query cw_query_iter(cw_world *w, cw_signature mask);
cw_query cw_query_iter_filter(cw_world *w, cw_filter filter);
bool cw_query_next(cw_query *query, cw_entity_id *id);
size_t cw_query_collect(cw_world *w, cw_signature mask, cw_entity_id *out, size_t capacity);
size_t cw_query_collect_filter(cw_world *w, cw_filter filter, cw_entity_id *out, size_t capacity);

/* Cached queries. */
cw_cached_query *cw_query_create(cw_world *w, cw_filter filter);
cw_cached_query *cw_query_create_access(cw_world *w, cw_filter filter, cw_access access);
void cw_query_destroy(cw_cached_query *query);
cw_world *cw_query_world(const cw_cached_query *query);
cw_filter cw_query_filter(const cw_cached_query *query);
cw_access cw_query_access(const cw_cached_query *query);
size_t cw_query_refs(const cw_cached_query *query);
/* Components on which the two queries conflict; empty when the queries
 * belong to different worlds or their filters are provably disjoint. */
cw_signature cw_query_conflicts(const cw_cached_query *a, const cw_cached_query *b);
bool cw_query_shares_cache(const cw_cached_query *a, const cw_cached_query *b);
/* Checked component access honouring the declared reads/writes. */
void *cw_query_get(const cw_cached_query *query, cw_entity_id id, cw_component_id ct, size_t size);
void *cw_query_write(cw_cached_query *query, cw_entity_id id, cw_component_id ct, size_t size);
#define CW_QUERY_GET(q, id, ct, T) ((T *)cw_query_get((q), (id), (ct), sizeof(T)))
#define CW_QUERY_WRITE(q, id, ct, T) ((T *)cw_query_write((q), (id), (ct), sizeof(T)))
cw_query cw_query_begin(cw_cached_query *query);
size_t cw_query_count(const cw_cached_query *query);
bool cw_query_contains(const cw_cached_query *query, cw_entity_id id);
size_t cw_query_gather(const cw_cached_query *query, cw_entity_id *out, size_t capacity);
const uint64_t *cw_query_bits(const cw_cached_query *query, size_t *words);
typedef bool (*cw_query_fn)(cw_world *w, cw_entity_id id, void *user);
size_t cw_query_each(cw_cached_query *query, cw_query_fn fn, void *user);

/* Early-exit helpers. cw_query_single reports how many entities match and
 * stores the first match (CW_INVALID_ENTITY when none). */
typedef enum { CW_SINGLE_NONE, CW_SINGLE_ONE, CW_SINGLE_MANY } cw_single;
bool cw_query_matches(const cw_query *iterator, cw_entity_id id);   /* any iterator */
bool cw_query_is_empty(const cw_cached_query *query);               /* O(1) */
bool cw_query_first(const cw_cached_query *query, cw_entity_id *id);
cw_single cw_query_single(const cw_cached_query *query, cw_entity_id *id);
bool cw_query_first_filter(cw_world *w, cw_filter filter, cw_entity_id *id);
cw_single cw_query_single_filter(cw_world *w, cw_filter filter, cw_entity_id *id);
size_t cw_query_count_filter(cw_world *w, cw_filter filter, size_t limit);  /* stops at limit */

/* ---------------------------------------------------------------------------
 * Change tracking
 *
 * An observer watches one component and accumulates three sets of entities:
 *   CW_CHANGE_ADDED     the component was added to the entity
 *   CW_CHANGE_MODIFIED  the component's value was written (additions count
 *                       as writes; so do cw_comp_set, cw_comp_unmarshal,
 *                       cw_comp_write/CW_WRITE and cw_comp_touch; plain
 *                       cw_comp_get/CW_GET pointer writes are invisible)
 *   CW_CHANGE_REMOVED   the component was removed, including by sweep,
 *                       rebuild and reset
 *
 * Each observer is one consumer's private cursor: cw_observer_observe moves
 * everything accumulated since the previous call into the observed sets and
 * starts accumulating afresh, without affecting any other observer. The
 * observed sets stay stable until the next observe, so a consumer can walk
 * them at leisure (but must not observe while walking).
 *
 * Events are recorded as they happen, so within one window an entity may be
 * both added and removed (for instance when its id was recycled) or modified
 * and then removed. cw_observer_count/has/bits/gather report the raw sets;
 * cw_observer_iter additionally skips added/modified entities that no longer
 * carry the component or are invisible (pending destruction), so every id it
 * yields can be read with CW_GET. Removed entities may be dead already.
 *
 * For conflict detection an observer is a reader of its component
 * (cw_observer_access): it conflicts with any query that writes the
 * component, regardless of filters, because additions and removals anywhere
 * are events too. Observers never conflict with each other.
 *
 * Observers are owned by the world: create them once per consumer, destroy
 * them when done, or let cw_world_destroy release the remainder. They
 * survive cw_world_reset (the reset itself shows up as removals), grow with
 * the world, and may watch components that are registered later.
 * ------------------------------------------------------------------------- */
cw_observer *cw_observer_create(cw_world *w, cw_component_id ct);
void cw_observer_destroy(cw_observer *observer);
cw_world *cw_observer_world(const cw_observer *observer);
cw_component_id cw_observer_component(const cw_observer *observer);
void cw_observer_observe(cw_observer *observer);
uint64_t cw_observer_observations(const cw_observer *observer);  /* times observed */
size_t cw_observer_count(const cw_observer *observer, cw_change kind);    /* observed */
size_t cw_observer_pending(const cw_observer *observer, cw_change kind);  /* not yet observed */
bool cw_observer_has(const cw_observer *observer, cw_change kind, cw_entity_id id);
const uint64_t *cw_observer_bits(const cw_observer *observer, cw_change kind, size_t *words);
cw_query cw_observer_iter(cw_observer *observer, cw_change kind);
size_t cw_observer_gather(const cw_observer *observer, cw_change kind, cw_entity_id *out, size_t capacity);
cw_access cw_observer_access(const cw_observer *observer);
cw_signature cw_observer_conflicts(const cw_observer *observer, const cw_cached_query *query);

/* ---------------------------------------------------------------------------
 * Command buffers: deferred mutations
 *
 * Recording a command never touches the world, so it is safe from inside any
 * iteration; cw_commands_flush applies the recorded commands to a world in
 * recording order and then empties the buffer. Commands that can no longer
 * apply (the entity was destroyed meanwhile, the component is unregistered,
 * a size mismatch) are skipped and counted in cw_flush_result.failed.
 *
 * cw_commands_create returns a pending id (negative, see
 * cw_entity_is_pending) that later commands in the same buffer may target;
 * after the flush the real ids are available from cw_commands_created, in
 * recording order, until the next non-empty flush or clear (flushing an
 * empty buffer is a no-op).
 *
 * cw_commands_update applies a callback to a component at flush time, which
 * is the tool for accumulating changes such as damage from several sources:
 * every recorded update runs, in order, against the then-current value.
 * cw_commands_call defers arbitrary work. Callbacks run during the flush and
 * must not record into the buffer being flushed (recording returns false).
 *
 * Ownership: a buffer is a value owned by whoever declares it (typically one
 * per system, later one per thread); cw_commands_init/destroy manage its
 * memory and it is not tied to a world until flushed. Recording is
 * allocation-free once cw_commands_reserve has sized the stream; otherwise
 * it grows geometrically. Payloads are copied at record time and aligned to
 * 16 bytes when handed back to callbacks.
 *
 * Every world owns a default buffer (cw_world_commands) that cw_sim_dispatch
 * and cw_sim_replay flush when the outermost dispatch of a phase completes,
 * so a system may simply record into it and rely on the changes being
 * visible to the next phase. cw_world_reset and cw_world_destroy discard it.
 * ------------------------------------------------------------------------- */
typedef void (*cw_update_fn)(cw_world *w, cw_entity_id id, cw_component_id ct,
                             void *component, const void *data, size_t size);
typedef void (*cw_command_fn)(cw_world *w, const void *data, size_t size);

void cw_commands_init(cw_commands *commands);
void cw_commands_destroy(cw_commands *commands);
void cw_commands_reserve(cw_commands *commands, size_t bytes);
void cw_commands_clear(cw_commands *commands);
size_t cw_commands_count(const cw_commands *commands);
bool cw_commands_is_empty(const cw_commands *commands);
bool cw_entity_is_pending(cw_entity_id id);
cw_entity_id cw_commands_create(cw_commands *commands);
bool cw_commands_destroy_entity(cw_commands *commands, cw_entity_id id);
bool cw_commands_add(cw_commands *commands, cw_entity_id id, cw_component_id ct);
bool cw_commands_set(cw_commands *commands, cw_entity_id id, cw_component_id ct,
                     const void *value, size_t size);
#define CW_COMMANDS_SET(c, id, ct, ptr) cw_commands_set((c), (id), (ct), (ptr), sizeof(*(ptr)))
bool cw_commands_update(cw_commands *commands, cw_entity_id id, cw_component_id ct,
                        cw_update_fn fn, const void *data, size_t size);
bool cw_commands_remove(cw_commands *commands, cw_entity_id id, cw_component_id ct);
bool cw_commands_call(cw_commands *commands, cw_command_fn fn, const void *data, size_t size);
cw_flush_result cw_commands_flush(cw_commands *commands, cw_world *w);
cw_flush_result cw_commands_last_flush(const cw_commands *commands);
cw_id_view cw_commands_created(const cw_commands *commands);
cw_commands *cw_world_commands(cw_world *w);

/* ---------------------------------------------------------------------------
 * Byte buffers
 *
 * Bounds-checked primitives for binary encoding. Every multi-byte integer is
 * written and read little-endian regardless of the host; floats are their
 * IEEE-754 bit patterns. Signed integers are two's complement.
 *
 * A cw_writer is a value owned by whoever declares it and comes in three
 * flavours:
 *   - growable (cw_writer_init): a heap buffer managed by the writer that
 *     grows geometrically; cw_writer_destroy frees it, or
 *     cw_writer_release hands it to the caller, who frees it with free().
 *     Writes never fail (allocation failure aborts, as everywhere else).
 *   - fixed (cw_writer_init_fixed with storage): writes that do not fit set
 *     the sticky `overflow` flag and return false, but `size` keeps counting
 *     the bytes the content needs, so a too-small buffer still reports the
 *     required size. Nothing is written past the capacity.
 *   - measuring (cw_writer_init_fixed with NULL): stores nothing, only
 *     counts; writes return true.
 * This mirrors the codec marshal contract, so a codec can implement marshal
 * as cw_writer_init_fixed(&wr, data, capacity) ... *size = wr.size;
 * return !wr.overflow.
 *
 * A cw_reader borrows a byte range; reads past the end fail, set the sticky
 * `error` flag, store 0 in the output and leave the position unchanged, so a
 * sequence of reads can be checked once with cw_reader_ok. cw_reader_sub
 * carves a length-delimited span into its own bounded reader.
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned char *data;
    size_t size, capacity;       /* size: bytes written, or required when overflowed/measuring */
    bool owned;                  /* growable heap buffer managed by the writer */
    bool overflow;               /* sticky: a write did not fit a fixed buffer */
} cw_writer;
void cw_writer_init(cw_writer *w);                                   /* growable */
void cw_writer_init_fixed(cw_writer *w, void *data, size_t capacity); /* NULL measures */
void cw_writer_destroy(cw_writer *w);
void cw_writer_clear(cw_writer *w);                /* size = 0, overflow cleared; keeps storage */
void cw_writer_reserve(cw_writer *w, size_t capacity);               /* growable only */
unsigned char *cw_writer_release(cw_writer *w, size_t *size);        /* growable only */
size_t cw_writer_size(const cw_writer *w);
bool cw_writer_ok(const cw_writer *w);                               /* !overflow */
/* Reserves `size` bytes at the end and returns them for the caller to fill;
 * NULL when measuring or when they do not fit (the size still advances). */
void *cw_writer_claim(cw_writer *w, size_t size);
bool cw_writer_bytes(cw_writer *w, const void *data, size_t size);
bool cw_writer_u8(cw_writer *w, uint8_t value);
bool cw_writer_u16(cw_writer *w, uint16_t value);
bool cw_writer_u32(cw_writer *w, uint32_t value);
bool cw_writer_u64(cw_writer *w, uint64_t value);
bool cw_writer_i8(cw_writer *w, int8_t value);
bool cw_writer_i16(cw_writer *w, int16_t value);
bool cw_writer_i32(cw_writer *w, int32_t value);
bool cw_writer_i64(cw_writer *w, int64_t value);
bool cw_writer_f32(cw_writer *w, float value);
bool cw_writer_f64(cw_writer *w, double value);
/* Overwrite a previously written field, e.g. a length learned later. */
bool cw_writer_patch_u32(cw_writer *w, size_t offset, uint32_t value);
bool cw_writer_patch_u64(cw_writer *w, size_t offset, uint64_t value);

typedef struct {
    const unsigned char *data;
    size_t size, offset;
    bool error;                  /* sticky: a read ran past the end */
} cw_reader;
cw_reader cw_reader_make(const void *data, size_t size);
size_t cw_reader_remaining(const cw_reader *r);
bool cw_reader_ok(const cw_reader *r);
bool cw_reader_at_end(const cw_reader *r);                           /* ok and fully consumed */
bool cw_reader_skip(cw_reader *r, size_t size);
bool cw_reader_bytes(cw_reader *r, void *out, size_t size);
/* Borrow `size` bytes in place; NULL when they are not available. */
const void *cw_reader_view(cw_reader *r, size_t size);
bool cw_reader_sub(cw_reader *r, size_t size, cw_reader *sub);
bool cw_reader_u8(cw_reader *r, uint8_t *out);
bool cw_reader_u16(cw_reader *r, uint16_t *out);
bool cw_reader_u32(cw_reader *r, uint32_t *out);
bool cw_reader_u64(cw_reader *r, uint64_t *out);
bool cw_reader_i8(cw_reader *r, int8_t *out);
bool cw_reader_i16(cw_reader *r, int16_t *out);
bool cw_reader_i32(cw_reader *r, int32_t *out);
bool cw_reader_i64(cw_reader *r, int64_t *out);
bool cw_reader_f32(cw_reader *r, float *out);
bool cw_reader_f64(cw_reader *r, double *out);

/* ---------------------------------------------------------------------------
 * Snapshots
 *
 * cw_snapshot_save serialises a world's entities into a versioned binary
 * image appended to a cw_writer; cw_snapshot_load rebuilds that image in a
 * fresh world whose component schema the caller has registered beforehand.
 *
 * What is captured: every active entity (including those marked for
 * destruction, which are re-marked on load), the components it carries, the
 * tick, the id allocator (next_id, recycle_ids, the recycled id list in
 * order) and last_created. Entity ids are preserved exactly, and after a
 * load the world hands out the same ids the saved world would have. Not
 * captured: the registry itself, cached queries, observers, command buffers,
 * the created list and the user pointer; loaded entities are reported in
 * cw_world_created like any restored entity so EntityCreated systems can
 * rebuild transient state.
 *
 * Schema: components are matched by registered name, never by id, so the
 * loading world may register them in any order. Each present component is
 * encoded by its marshal callback, or, when it has none and the save
 * config allows it, as the raw bytes of its slot (host layout; the loader
 * requires an identical element size and host byte order). Transient
 * components are skipped unless include_transient is set. Only components
 * some entity actually carries appear in the image.
 *
 * Ownership and failure: the caller owns the writer, the input bytes, the
 * world and the configs; nothing is retained. A failed save truncates the
 * writer back to its initial size; with a fixed writer that is too small the
 * error reports the bytes required. A load validates the whole image against
 * the limits and the registered schema before touching the world, so most
 * failures leave it untouched; if a codec then rejects a payload the world
 * is reset, so it is fresh again either way. The world must be fresh to
 * begin with: no entities, next_id zero, no recycled ids (a new or reset
 * world).
 *
 * Limits: every count and length in the image is checked against the load
 * config before any memory is committed, which makes untrusted input safe
 * to pass; zero fields take the defaults below, SIZE_MAX / INT64_MAX mean
 * unlimited. Trailing bytes after the image are an error; cw_snapshot_peek
 * reports the image size for callers embedding snapshots in larger streams.
 *
 * Format (CW_SNAPSHOT_FORMAT_VERSION, all integers little-endian):
 *   header   magic "CWSN", u16 version, u16 flags (bit 0: raw payloads
 *            were written on a big-endian host), u64 total size including
 *            the header, u32 user version, u32 section count
 *   section  u32 tag, u64 payload length, payload; unknown tags are skipped
 *     "WRLD" u64 tick, i64 next_id, i64 last_created, u8 recycle_ids
 *     "SCHM" u16 count, then per component: u8 saved id, u8 encoding
 *            (1 codec, 2 raw), u64 element size, u16 name length, name
 *     "ENTS" u64 count, then per entity in ascending id order: i64 id,
 *            u8 flags (bit 2: marked for destruction), u8 component count,
 *            then per component: u8 saved id, u32 length, payload
 *     "RCYL" u64 count, i64 ids in list order
 * ------------------------------------------------------------------------- */
#define CW_SNAPSHOT_FORMAT_VERSION 1
#define CW_SNAPSHOT_DEFAULT_MAX_ENTITIES ((size_t)1 << 20)
#define CW_SNAPSHOT_DEFAULT_MAX_ENTITY_ID (INT64_C(1) << 20)
#define CW_SNAPSHOT_DEFAULT_MAX_COMPONENT_BYTES ((size_t)1 << 20)
#define CW_SNAPSHOT_NAME_CAPACITY 64

typedef enum {
    CW_SNAPSHOT_OK,
    CW_SNAPSHOT_ERROR_ARGUMENT,          /* NULL world, writer or data */
    CW_SNAPSHOT_ERROR_NO_CODEC,          /* save: component has no marshal and raw is disabled */
    CW_SNAPSHOT_ERROR_MARSHAL,           /* save: a marshal callback failed */
    CW_SNAPSHOT_ERROR_OVERFLOW,          /* save: fixed writer too small; offset = bytes required */
    CW_SNAPSHOT_ERROR_TRUNCATED,         /* load: the image ends early */
    CW_SNAPSHOT_ERROR_TRAILING_DATA,     /* load: bytes follow the image */
    CW_SNAPSHOT_ERROR_MAGIC,             /* load: not a snapshot */
    CW_SNAPSHOT_ERROR_VERSION,           /* load: unsupported format version or flags */
    CW_SNAPSHOT_ERROR_CORRUPT,           /* load: structurally inconsistent */
    CW_SNAPSHOT_ERROR_LIMIT,             /* load: a configured limit was exceeded */
    CW_SNAPSHOT_ERROR_WORLD_NOT_FRESH,   /* load: the world already holds state */
    CW_SNAPSHOT_ERROR_UNKNOWN_COMPONENT, /* load: name not registered in the world */
    CW_SNAPSHOT_ERROR_SCHEMA,            /* load: registration incompatible with the image */
    CW_SNAPSHOT_ERROR_UNMARSHAL          /* load: an unmarshal callback rejected its payload */
} cw_snapshot_status;
typedef struct {
    cw_snapshot_status status;
    size_t offset;                       /* byte offset in the image where the problem was found */
    cw_entity_id entity;                 /* entity involved, or CW_INVALID_ENTITY */
    cw_component_id component;           /* world component involved, or CW_INVALID_COMPONENT */
    char name[CW_SNAPSHOT_NAME_CAPACITY]; /* component name involved (truncated), or "" */
} cw_snapshot_error;
const char *cw_snapshot_status_string(cw_snapshot_status status);

typedef struct {
    bool include_transient;              /* also save transient components */
    bool allow_raw;                      /* copy slot bytes when a component has no marshal */
    uint32_t user_version;               /* application schema version stored in the header */
} cw_snapshot_save_config;
typedef struct {
    size_t max_entities;                 /* entity records and recycled ids, each */
    cw_entity_id max_entity_id;          /* highest id accepted (bounds the world capacity) */
    size_t max_component_bytes;          /* per component payload */
    bool allow_raw;                      /* accept raw-encoded components */
    bool ignore_unknown_components;      /* skip components the world has not registered */
} cw_snapshot_load_config;
cw_snapshot_load_config cw_snapshot_load_defaults(void);

typedef struct {
    uint16_t format_version, flags;
    uint32_t user_version;
    uint64_t size;                       /* bytes of the whole image */
    uint64_t tick;
    cw_entity_id next_id;
    uint64_t entity_count, recycled_count;
    uint16_t component_count;            /* schema entries */
} cw_snapshot_info;

/* A NULL config selects the defaults. */
bool cw_snapshot_save(cw_world *w, const cw_snapshot_save_config *config,
                      cw_writer *out, cw_snapshot_error *error);
bool cw_snapshot_load(cw_world *w, const void *data, size_t size,
                      const cw_snapshot_load_config *config, cw_snapshot_error *error);
/* Validates the header and section structure only; no schema is needed. */
bool cw_snapshot_peek(const void *data, size_t size, cw_snapshot_info *info,
                      cw_snapshot_error *error);

/* ---------------------------------------------------------------------------
 * World serialization
 *
 * cw_world_serialize writes the observable state of a world into a freshly
 * allocated buffer in the snapshot format above: the visible entities
 * (active and not marked for destruction) with their ids preserved, their
 * non-transient components identified by registered name, the tick and the
 * next entity id. Entities pending destruction, transient components, the
 * recycled id list and last_created are left out (the image carries an
 * empty recycled list and last_created = CW_INVALID_ENTITY); next_id still
 * guarantees that the omitted ids are never handed out again after a load.
 * Encoding follows the snapshot rules: marshal callback, or raw slot bytes
 * with allow_raw, else CW_SNAPSHOT_ERROR_NO_CODEC.
 *
 * Ownership: on success the caller owns the buffer and releases it with
 * cw_buffer_free (the world and config are not retained); on failure the
 * buffer is empty and nothing is allocated. cw_snapshot_load and
 * cw_snapshot_peek accept the result. A NULL config selects the defaults.
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned char *data;
    size_t size;
} cw_buffer;
void cw_buffer_free(cw_buffer *buffer);
typedef struct {
    bool allow_raw;                      /* copy slot bytes when a component has no marshal */
    uint32_t user_version;               /* application schema version stored in the header */
} cw_serialize_config;
bool cw_world_serialize(cw_world *w, const cw_serialize_config *config,
                        cw_buffer *out, cw_snapshot_error *error);

/* cw_world_deserialize rebuilds a serialized image in a fresh world (new or
 * reset: no entities, next_id zero, no recycled ids) whose component schema
 * the caller has registered, in any order and at any ids: component types
 * are resolved by registered name. Entity ids, components, the tick and
 * next_id are restored exactly, so entities created afterwards receive the
 * ids the original world would have handed out next.
 *
 * It is strict where cw_snapshot_load is configurable: a component name the
 * world has not registered fails with CW_SNAPSHOT_ERROR_UNKNOWN_COMPONENT,
 * an unsupported format version (or, with check_user_version, a different
 * user version) fails with CW_SNAPSHOT_ERROR_VERSION. The limits work as in
 * cw_snapshot_load_config (zero selects the defaults).
 *
 * Failure never leaves a half-built world: structure, limits and schema are
 * validated before anything is created, and if a codec rejects a payload
 * afterwards the entities built so far are released through the normal
 * reset path (cleanup callbacks run) and the world is fresh again, with its
 * prior settings, so the caller may retry or load something else. The
 * return value is false in every such case. Loaded entities are reported
 * in cw_world_created, like restored entities. */
typedef struct {
    size_t max_entities;
    cw_entity_id max_entity_id;
    size_t max_component_bytes;
    bool allow_raw;                      /* accept raw-encoded components */
    bool check_user_version;             /* require the image's user version to match */
    uint32_t user_version;
} cw_deserialize_config;
cw_deserialize_config cw_deserialize_defaults(void);
bool cw_world_deserialize(cw_world *w, const void *data, size_t size,
                          const cw_deserialize_config *config, cw_snapshot_error *error);

/* ---------------------------------------------------------------------------
 * Files
 *
 * Convenience wrappers that pair the memory APIs above with whole-file I/O.
 * Saving encodes the complete image in memory first and only then writes it
 * to a sibling temporary file (`path` + ".tmp"), flushes it to disk and
 * renames it over `path`; an encoding or I/O failure therefore never
 * damages a valid file already at `path`, and a crash mid-write leaves at
 * most the temporary behind. Loading reads the whole file into memory
 * (bounded by `max_size`, default CW_FILE_DEFAULT_MAX_SIZE, to keep a wrong
 * path from exhausting memory) and then loads it with the same guarantees
 * as the memory APIs: the world is untouched or reset to fresh on failure.
 *
 * Ownership: paths, worlds and configs are borrowed for the call; the
 * buffer cw_file_read fills is the caller's and is released with
 * cw_buffer_free. Errors report a file status, the errno captured at the
 * failing call (0 when not applicable), and, for CW_FILE_ERROR_SNAPSHOT,
 * the full cw_snapshot_error from the memory API.
 * ------------------------------------------------------------------------- */
#define CW_FILE_DEFAULT_MAX_SIZE ((size_t)256 << 20)
typedef enum {
    CW_FILE_OK,
    CW_FILE_ERROR_ARGUMENT,              /* NULL or empty path, NULL world or buffer */
    CW_FILE_ERROR_OPEN,                  /* fopen failed (errnum set) */
    CW_FILE_ERROR_READ,                  /* read error (errnum set) */
    CW_FILE_ERROR_WRITE,                 /* write, flush or close of the temporary failed (errnum set) */
    CW_FILE_ERROR_TOO_LARGE,             /* the file exceeds max_size */
    CW_FILE_ERROR_REPLACE,               /* renaming the temporary over `path` failed (errnum set) */
    CW_FILE_ERROR_SNAPSHOT               /* the memory API failed; see `snapshot` */
} cw_file_status;
typedef struct {
    cw_file_status status;
    int errnum;
    cw_snapshot_error snapshot;
} cw_file_error;
const char *cw_file_status_string(cw_file_status status);

bool cw_file_write(const char *path, const void *data, size_t size, cw_file_error *error);
bool cw_file_read(const char *path, size_t max_size, cw_buffer *out, cw_file_error *error);
bool cw_snapshot_save_file(cw_world *w, const char *path, const cw_snapshot_save_config *config,
                           cw_file_error *error);
bool cw_snapshot_load_file(cw_world *w, const char *path, const cw_snapshot_load_config *config,
                           cw_file_error *error);
bool cw_world_serialize_file(cw_world *w, const char *path, const cw_serialize_config *config,
                             cw_file_error *error);
bool cw_world_deserialize_file(cw_world *w, const char *path, const cw_deserialize_config *config,
                               cw_file_error *error);

typedef enum {
    CW_INIT, CW_START, CW_STOP, CW_SHUTDOWN,
    CW_EARLY_UPDATE, CW_PHYSICS_UPDATE, CW_UPDATE, CW_LATE_UPDATE,
    CW_OVERLAY, CW_ENTITY_CREATED, CW_ENTITY_DESTROYED,
    CW_USER0, CW_USER1, CW_USER2, CW_USER3, CW_END_FRAME
} cw_phase;
typedef uint16_t cw_phase_mask;
#define CW_PHASE(phase) ((cw_phase_mask)(UINT16_C(1) << (phase)))
typedef enum { CW_SHARED, CW_HOST_ONLY } cw_authority;
typedef enum { CW_HOST, CW_CLIENT } cw_sim_role;
typedef void (*cw_sim_fn)(cw_world *, cw_phase, float dt);
typedef struct {
    const char *name;
    cw_phase_mask phases;
    cw_sim_fn fn;
    cw_authority authority;
    int order;
    bool run_when_paused;
} cw_system_desc;
typedef struct {
    char *name;
    cw_phase_mask phases;
    cw_sim_fn fn;
    cw_authority authority;
    int order;
    bool enabled, run_when_paused;
    uint64_t sequence;
} cw_system;
typedef struct {
    cw_system *systems;
    size_t count, capacity;
    cw_sim_role role;
    bool paused, sorted;
    uint64_t next_sequence;
    unsigned dispatch_depth;
} cw_sim;

void cw_sim_init(cw_sim *sim, cw_sim_role role);
void cw_sim_destroy(cw_sim *sim);
bool cw_sim_register(cw_sim *sim, cw_system_desc desc);
bool cw_sim_remove(cw_sim *sim, const char *name);
bool cw_sim_set_enabled(cw_sim *sim, const char *name, bool enabled);
const cw_system *cw_sim_find(const cw_sim *sim, const char *name);
const cw_system *cw_sim_systems(cw_sim *sim, size_t *count);
void cw_sim_dispatch(cw_sim *sim, cw_world *w, cw_phase phase, float dt);
void cw_sim_replay(cw_sim *sim, cw_world *w, float dt);
void cw_sim_tick(cw_sim *sim, cw_world *w, float dt);
void cw_sim_entity_lifecycle(cw_sim *sim, cw_world *w, float dt);

#define CW_DEFAULT_TICK_HZ 30
#define CW_MAX_FRAME_DT 0.25f
#define CW_MAX_STEPS_PER_FRAME 5
#define CW_MAX_CATCHUP_STEPS_PER_FRAME 1
#define CW_INPUT_BACKLOG_TARGET 1
typedef struct {
    int hz;
    float dt, accumulator;
    int max_steps, max_catchup, backlog_target;
} cw_clock;
cw_clock cw_clock_make(int hz);
int cw_clock_advance(cw_clock *clock, float frame_dt);
bool cw_clock_should_drain(const cw_clock *clock, int backlog, int taken, int drained);
float cw_clock_alpha(const cw_clock *clock);
void cw_clock_reset(cw_clock *clock);

#endif
