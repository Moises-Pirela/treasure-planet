#include "cw_internal.h"

/* Image layout is documented with the public API in clockwork.h. */
#define TAG(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
enum {
    SEC_WORLD, SEC_SCHEMA, SEC_ENTITIES, SEC_RECYCLED, SEC_COUNT
};
static const uint32_t SECTION_TAGS[SEC_COUNT] = {
    TAG('W', 'R', 'L', 'D'), TAG('S', 'C', 'H', 'M'), TAG('E', 'N', 'T', 'S'), TAG('R', 'C', 'Y', 'L')
};
enum { ENC_CODEC = 1, ENC_RAW = 2 };
enum { FLAG_BIG_ENDIAN_RAW = 1, FLAGS_KNOWN = FLAG_BIG_ENDIAN_RAW };
enum { HEADER_SIZE = 24 };
static const unsigned char MAGIC[4] = {'C', 'W', 'S', 'N'};

static bool host_big_endian(void) {
    uint16_t one = 1;
    unsigned char first;
    memcpy(&first, &one, 1);
    return first == 0;
}
static void error_clear(cw_snapshot_error *e) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->entity = CW_INVALID_ENTITY;
    e->component = CW_INVALID_COMPONENT;
}
static bool fail_at(cw_snapshot_error *e, cw_snapshot_status status, size_t offset,
                    cw_entity_id id, cw_component_id ct, const char *name, size_t name_len) {
    if (!e) return false;
    error_clear(e);
    e->status = status;
    e->offset = offset;
    e->entity = id;
    e->component = ct;
    if (name) {
        if (name_len >= sizeof(e->name)) name_len = sizeof(e->name) - 1;
        memcpy(e->name, name, name_len);
        e->name[name_len] = 0;
    }
    return false;
}
static bool fail(cw_snapshot_error *e, cw_snapshot_status status, size_t offset) {
    return fail_at(e, status, offset, CW_INVALID_ENTITY, CW_INVALID_COMPONENT, NULL, 0);
}
const char *cw_snapshot_status_string(cw_snapshot_status status) {
    static const char *const names[] = {
        "ok", "invalid argument", "component has no codec", "marshal failed",
        "output buffer too small", "image truncated", "trailing data after image",
        "not a snapshot", "unsupported version or flags", "image corrupt",
        "limit exceeded", "world is not fresh", "component not registered",
        "registration incompatible with image", "unmarshal failed"
    };
    return (unsigned)status < sizeof(names) / sizeof(*names) ? names[status] : "unknown status";
}
cw_snapshot_load_config cw_snapshot_load_defaults(void) {
    cw_snapshot_load_config c = {0};
    c.max_entities = CW_SNAPSHOT_DEFAULT_MAX_ENTITIES;
    c.max_entity_id = CW_SNAPSHOT_DEFAULT_MAX_ENTITY_ID;
    c.max_component_bytes = CW_SNAPSHOT_DEFAULT_MAX_COMPONENT_BYTES;
    return c;
}

/* ------------------------------------------------------------------------- */
/* Save                                                                      */
/* ------------------------------------------------------------------------- */
typedef struct {
    cw_world *w;
    cw_writer *out;
    cw_snapshot_error *error;
    size_t start;
    bool overflow_before;
} saver;
/* Every failure leaves the writer as it was found. */
static bool abandon(saver *s, cw_snapshot_status status, cw_entity_id id, cw_component_id ct) {
    size_t offset = s->out->size - s->start;
    const char *name = ct != CW_INVALID_COMPONENT ? cw_comp_name(s->w, ct) : NULL;
    s->out->size = s->start;
    s->out->overflow = s->overflow_before;
    return fail_at(s->error, status, offset, id, ct, name, name ? strlen(name) : 0);
}
static size_t begin_section(cw_writer *out, unsigned section) {
    cw_writer_u32(out, SECTION_TAGS[section]);
    size_t at = out->size;
    cw_writer_u64(out, 0);
    return at;
}
static void end_section(cw_writer *out, size_t at) {
    if (!out->overflow) cw_writer_patch_u64(out, at, (uint64_t)(out->size - (at + 8)));
}
static bool save_component(saver *s, cw_entity_id id, cw_component_id ct) {
    cw_world *w = s->w;
    cw_component_pool *p = &w->pools[ct];
    cw_writer_u8(s->out, ct);
    if (!p->codec.marshal) {
        if (p->elem_size > UINT32_MAX) return abandon(s, CW_SNAPSHOT_ERROR_LIMIT, id, ct);
        cw_writer_u32(s->out, (uint32_t)p->elem_size);
        cw_writer_bytes(s->out, cw_comp_ptr(w, id, ct), p->elem_size);
        return true;
    }
    size_t size = 0, written = 0;
    if (!cw_comp_marshal(w, id, ct, NULL, 0, &size)) return abandon(s, CW_SNAPSHOT_ERROR_MARSHAL, id, ct);
    if (size > UINT32_MAX) return abandon(s, CW_SNAPSHOT_ERROR_LIMIT, id, ct);
    cw_writer_u32(s->out, (uint32_t)size);
    /* Marshal straight into the image; measuring or overflowed writers have
     * no storage, and their size already accounts for the payload. */
    void *payload = cw_writer_claim(s->out, size);
    if (payload && (!cw_comp_marshal(w, id, ct, payload, size, &written) || written != size))
        return abandon(s, CW_SNAPSHOT_ERROR_MARSHAL, id, ct);
    return true;
}
/* What an image includes; snapshots and serialization differ only here. */
typedef struct {
    bool include_transient;
    bool allow_raw;
    bool visible_only;           /* skip entities marked for destruction */
    bool include_allocator;      /* recycled id list and last_created */
    uint32_t user_version;
} policy;
static bool included(const cw_entity *e, const policy *p) {
    return p->visible_only ? cw_query_visible(*e) : cw_entity_is_active(*e);
}
static bool encode(saver *s, const policy *cfg) {
    cw_world *w = s->w;
    cw_writer *out = s->out;

    /* The schema lists only components some included entity carries. */
    cw_signature used = {0};
    uint64_t entity_count = 0;
    for (size_t i = 0; i < w->capacity; ++i) {
        const cw_entity *e = &w->entities[i];
        if (!included(e, cfg)) continue;
        ++entity_count;
        for (unsigned wi = 0; wi < CW_SIGNATURE_WORDS; ++wi) used.words[wi] |= e->signature.words[wi];
    }
    unsigned schema_count = 0;
    for (unsigned ct = 0; ct < CW_MAX_COMPONENTS; ++ct) {
        if (!cw_sig_has(used, (cw_component_id)ct)) continue;
        const cw_component_pool *p = &w->pools[ct];
        if (!cw_registered(w, (cw_component_id)ct) || (p->transient && !cfg->include_transient)) {
            cw_sig_clear(&used, (cw_component_id)ct);
            continue;
        }
        if (!p->codec.marshal && !cfg->allow_raw)
            return abandon(s, CW_SNAPSHOT_ERROR_NO_CODEC, CW_INVALID_ENTITY, (cw_component_id)ct);
        if (strlen(p->name) > UINT16_MAX)
            return abandon(s, CW_SNAPSHOT_ERROR_LIMIT, CW_INVALID_ENTITY, (cw_component_id)ct);
        ++schema_count;
    }

    cw_writer_bytes(out, MAGIC, sizeof(MAGIC));
    cw_writer_u16(out, (uint16_t)CW_SNAPSHOT_FORMAT_VERSION);
    cw_writer_u16(out, (uint16_t)(host_big_endian() ? FLAG_BIG_ENDIAN_RAW : 0));
    size_t total_at = out->size;
    cw_writer_u64(out, 0);
    cw_writer_u32(out, cfg->user_version);
    cw_writer_u32(out, (uint32_t)SEC_COUNT);

    size_t at = begin_section(out, SEC_WORLD);
    cw_writer_u64(out, w->tick);
    cw_writer_i64(out, w->next_id);
    cw_writer_i64(out, cfg->include_allocator ? w->last_created : CW_INVALID_ENTITY);
    cw_writer_u8(out, (uint8_t)(w->recycle_ids ? 1 : 0));
    end_section(out, at);

    at = begin_section(out, SEC_SCHEMA);
    cw_writer_u16(out, (uint16_t)schema_count);
    for (unsigned ct = 0; ct < CW_MAX_COMPONENTS; ++ct) {
        if (!cw_sig_has(used, (cw_component_id)ct)) continue;
        const cw_component_pool *p = &w->pools[ct];
        cw_writer_u8(out, (uint8_t)ct);
        cw_writer_u8(out, (uint8_t)(p->codec.marshal ? ENC_CODEC : ENC_RAW));
        cw_writer_u64(out, p->elem_size);
        cw_writer_u16(out, (uint16_t)strlen(p->name));
        cw_writer_bytes(out, p->name, strlen(p->name));
    }
    end_section(out, at);

    at = begin_section(out, SEC_ENTITIES);
    cw_writer_u64(out, entity_count);
    for (size_t i = 0; i < w->capacity; ++i) {
        const cw_entity *e = &w->entities[i];
        if (!included(e, cfg)) continue;
        cw_signature saved = e->signature;
        unsigned count = 0;
        for (unsigned wi = 0; wi < CW_SIGNATURE_WORDS; ++wi) {
            saved.words[wi] &= used.words[wi];
            count += cw_popcount64(saved.words[wi]);
        }
        cw_writer_i64(out, e->id);
        cw_writer_u8(out, (uint8_t)(e->flags & CW_TO_DESTROY));
        cw_writer_u8(out, (uint8_t)count);
        for (unsigned wi = 0; wi < CW_SIGNATURE_WORDS; ++wi)
            for (uint64_t word = saved.words[wi]; word; word &= word - 1)
                if (!save_component(s, e->id, (cw_component_id)(wi * 64 + cw_ctz64(word)))) return false;
    }
    end_section(out, at);

    at = begin_section(out, SEC_RECYCLED);
    size_t recycled = cfg->include_allocator ? w->recycled_ids.count : 0;
    cw_writer_u64(out, recycled);
    for (size_t i = 0; i < recycled; ++i) cw_writer_i64(out, w->recycled_ids.data[i]);
    end_section(out, at);

    if (out->overflow) return abandon(s, CW_SNAPSHOT_ERROR_OVERFLOW, CW_INVALID_ENTITY, CW_INVALID_COMPONENT);
    cw_writer_patch_u64(out, total_at, (uint64_t)(out->size - s->start));
    return true;
}
bool cw_snapshot_save(cw_world *w, const cw_snapshot_save_config *config,
                      cw_writer *out, cw_snapshot_error *error) {
    policy p = {0};
    if (config) {
        p.include_transient = config->include_transient;
        p.allow_raw = config->allow_raw;
        p.user_version = config->user_version;
    }
    p.include_allocator = true;
    error_clear(error);
    if (!w || !out) return fail(error, CW_SNAPSHOT_ERROR_ARGUMENT, 0);
    saver s = {w, out, error, out->size, out->overflow};
    return encode(&s, &p);
}
void cw_buffer_free(cw_buffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}
bool cw_world_serialize(cw_world *w, const cw_serialize_config *config,
                        cw_buffer *out, cw_snapshot_error *error) {
    policy p = {0};
    if (config) {
        p.allow_raw = config->allow_raw;
        p.user_version = config->user_version;
    }
    p.visible_only = true;
    error_clear(error);
    if (out) memset(out, 0, sizeof(*out));
    if (!w || !out) return fail(error, CW_SNAPSHOT_ERROR_ARGUMENT, 0);
    cw_writer writer;
    cw_writer_init(&writer);
    saver s = {w, &writer, error, 0, false};
    if (!encode(&s, &p)) {
        cw_writer_destroy(&writer);
        return false;
    }
    out->data = cw_writer_release(&writer, &out->size);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Load                                                                      */
/* ------------------------------------------------------------------------- */
enum { MAP_UNDECLARED = -1, MAP_SKIP = -2 };
typedef struct {
    cw_world *w;
    cw_snapshot_load_config cfg;
    cw_snapshot_error *error;
    const unsigned char *base;
    cw_reader sections[SEC_COUNT];
    bool present[SEC_COUNT];
    uint16_t version, flags;
    uint32_t user_version;
    uint64_t total, tick, entity_count, recycled_count;
    uint16_t schema_count;
    int64_t next_id, last_created, max_id;
    bool recycle;
    /* Saved component id -> world component id, MAP_SKIP or MAP_UNDECLARED. */
    int16_t map[CW_MAX_COMPONENTS];
    uint8_t enc[CW_MAX_COMPONENTS];
    uint64_t elem[CW_MAX_COMPONENTS];
} loader;
static size_t where(const loader *L, const cw_reader *r) {
    return r->data ? (size_t)(r->data - L->base) + r->offset : r->offset;
}
static bool corrupt(loader *L, const cw_reader *r) {
    return fail(L->error, CW_SNAPSHOT_ERROR_CORRUPT, where(L, r));
}
static bool limit(loader *L, const cw_reader *r) {
    return fail(L->error, CW_SNAPSHOT_ERROR_LIMIT, where(L, r));
}
static bool parse_header(loader *L, const void *data, size_t size) {
    if (!data) return fail(L->error, CW_SNAPSHOT_ERROR_ARGUMENT, 0);
    L->base = (const unsigned char *)data;
    cw_reader r = cw_reader_make(data, size);
    unsigned char magic[sizeof(MAGIC)];
    uint32_t section_count;
    if (!cw_reader_bytes(&r, magic, sizeof(magic))) return fail(L->error, CW_SNAPSHOT_ERROR_TRUNCATED, 0);
    if (memcmp(magic, MAGIC, sizeof(MAGIC)) != 0) return fail(L->error, CW_SNAPSHOT_ERROR_MAGIC, 0);
    cw_reader_u16(&r, &L->version);
    cw_reader_u16(&r, &L->flags);
    cw_reader_u64(&r, &L->total);
    cw_reader_u32(&r, &L->user_version);
    cw_reader_u32(&r, &section_count);
    if (!cw_reader_ok(&r)) return fail(L->error, CW_SNAPSHOT_ERROR_TRUNCATED, r.offset);
    if (L->version != CW_SNAPSHOT_FORMAT_VERSION) return fail(L->error, CW_SNAPSHOT_ERROR_VERSION, 4);
    if (L->flags & ~(uint16_t)FLAGS_KNOWN) return fail(L->error, CW_SNAPSHOT_ERROR_VERSION, 6);
    if (L->total < (uint64_t)HEADER_SIZE) return fail(L->error, CW_SNAPSHOT_ERROR_CORRUPT, 8);
    if (L->total > size) return fail(L->error, CW_SNAPSHOT_ERROR_TRUNCATED, 8);
    if (L->total < size) return fail(L->error, CW_SNAPSHOT_ERROR_TRAILING_DATA, (size_t)L->total);
    for (uint32_t i = 0; i < section_count; ++i) {
        uint32_t tag;
        uint64_t length;
        cw_reader_u32(&r, &tag);
        if (!cw_reader_u64(&r, &length)) return fail(L->error, CW_SNAPSHOT_ERROR_TRUNCATED, r.offset);
        if (length > cw_reader_remaining(&r)) return fail(L->error, CW_SNAPSHOT_ERROR_TRUNCATED, r.offset);
        cw_reader section;
        cw_reader_sub(&r, (size_t)length, &section);
        unsigned known = 0;
        while (known < SEC_COUNT && SECTION_TAGS[known] != tag) ++known;
        if (known == SEC_COUNT) continue;                    /* unknown sections are skipped */
        if (L->present[known]) return corrupt(L, &section);  /* duplicate */
        L->present[known] = true;
        L->sections[known] = section;
    }
    if (!cw_reader_at_end(&r)) return corrupt(L, &r);
    for (unsigned i = 0; i < SEC_COUNT; ++i) if (!L->present[i]) return corrupt(L, &r);

    cw_reader world = L->sections[SEC_WORLD];
    uint8_t recycle;
    cw_reader_u64(&world, &L->tick);
    cw_reader_i64(&world, &L->next_id);
    cw_reader_i64(&world, &L->last_created);
    cw_reader_u8(&world, &recycle);
    if (!cw_reader_at_end(&world) || L->next_id < 0 || L->last_created < -1 || recycle > 1)
        return corrupt(L, &world);
    L->recycle = recycle != 0;
    cw_reader peek = L->sections[SEC_SCHEMA];
    if (!cw_reader_u16(&peek, &L->schema_count)) return corrupt(L, &peek);
    peek = L->sections[SEC_ENTITIES];
    if (!cw_reader_u64(&peek, &L->entity_count)) return corrupt(L, &peek);
    peek = L->sections[SEC_RECYCLED];
    if (!cw_reader_u64(&peek, &L->recycled_count)) return corrupt(L, &peek);
    return true;
}
bool cw_snapshot_peek(const void *data, size_t size, cw_snapshot_info *info, cw_snapshot_error *error) {
    loader L = {0};
    L.error = error;
    error_clear(error);
    if (!parse_header(&L, data, size)) return false;
    if (info) {
        memset(info, 0, sizeof(*info));
        info->format_version = L.version;
        info->flags = L.flags;
        info->user_version = L.user_version;
        info->size = L.total;
        info->tick = L.tick;
        info->next_id = L.next_id;
        info->entity_count = L.entity_count;
        info->recycled_count = L.recycled_count;
        info->component_count = L.schema_count;
    }
    return true;
}
static bool find_component(const cw_world *w, const char *name, size_t length, cw_component_id *ct) {
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        const cw_component_pool *p = &w->pools[i];
        if (!p->elem_size || strlen(p->name) != length || memcmp(p->name, name, length) != 0) continue;
        *ct = (cw_component_id)i;
        return true;
    }
    return false;
}
static bool parse_schema(loader *L) {
    cw_reader r = L->sections[SEC_SCHEMA];
    uint16_t count;
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) L->map[i] = MAP_UNDECLARED;
    cw_reader_u16(&r, &count);
    if (count > CW_MAX_COMPONENTS) return corrupt(L, &r);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t id, encoding;
        uint64_t elem_size;
        uint16_t name_length;
        size_t entry_at = where(L, &r);
        cw_reader_u8(&r, &id);
        cw_reader_u8(&r, &encoding);
        cw_reader_u64(&r, &elem_size);
        cw_reader_u16(&r, &name_length);
        const char *name = (const char *)cw_reader_view(&r, name_length);
        if (!cw_reader_ok(&r) || !name_length) return corrupt(L, &r);
        if (id >= CW_MAX_COMPONENTS || L->map[id] != MAP_UNDECLARED) return corrupt(L, &r);
        if (encoding != ENC_CODEC && encoding != ENC_RAW) return corrupt(L, &r);
        cw_component_id ct;
        if (!find_component(L->w, name, name_length, &ct)) {
            if (!L->cfg.ignore_unknown_components)
                return fail_at(L->error, CW_SNAPSHOT_ERROR_UNKNOWN_COMPONENT, entry_at,
                               CW_INVALID_ENTITY, CW_INVALID_COMPONENT, name, name_length);
            L->map[id] = MAP_SKIP;
            continue;
        }
        const cw_component_pool *p = &L->w->pools[ct];
        bool compatible = encoding == ENC_CODEC
            ? p->codec.unmarshal != NULL
            : L->cfg.allow_raw && elem_size == p->elem_size
                && ((L->flags & FLAG_BIG_ENDIAN_RAW) != 0) == host_big_endian();
        if (!compatible)
            return fail_at(L->error, CW_SNAPSHOT_ERROR_SCHEMA, entry_at, CW_INVALID_ENTITY, ct, name, name_length);
        L->map[id] = (int16_t)ct;
        L->enc[id] = encoding;
        L->elem[id] = elem_size;
    }
    return cw_reader_at_end(&r) || corrupt(L, &r);
}
static bool unmarshal_failed(loader *L, size_t offset, cw_entity_id id, cw_component_id ct) {
    const char *name = cw_comp_name(L->w, ct);
    return fail_at(L->error, CW_SNAPSHOT_ERROR_UNMARSHAL, offset, id, ct, name, strlen(name));
}
/* Walks the entity records, validating them against the limits and schema.
 * With `bits` set it marks every entity id; with `apply` set it also
 * restores the entities into the world. Validation is repeated on every walk
 * so that an apply never trusts unchecked input. */
static bool walk_entities(loader *L, uint64_t *bits, bool apply) {
    cw_reader r = L->sections[SEC_ENTITIES];
    uint64_t count;
    cw_reader_u64(&r, &count);
    if (count > L->cfg.max_entities) return limit(L, &r);
    int64_t previous = -1;
    for (uint64_t i = 0; i < count; ++i) {
        int64_t id;
        uint8_t flags, components;
        cw_reader_i64(&r, &id);
        cw_reader_u8(&r, &flags);
        if (!cw_reader_u8(&r, &components)) return corrupt(L, &r);
        if (id < 0 || id <= previous || id >= L->next_id) return corrupt(L, &r);
        if (id > L->cfg.max_entity_id) return limit(L, &r);
        if ((flags & ~(uint8_t)CW_TO_DESTROY) || components > CW_MAX_COMPONENTS) return corrupt(L, &r);
        previous = id;
        if (bits) bits[(size_t)id >> 6] |= UINT64_C(1) << (id & 63);
        if (apply && !cw_world_restore_entity(L->w, id)) return corrupt(L, &r);
        cw_signature seen = {0};
        for (unsigned c = 0; c < components; ++c) {
            uint8_t saved;
            uint32_t length;
            cw_reader_u8(&r, &saved);
            if (!cw_reader_u32(&r, &length)) return corrupt(L, &r);
            if (saved >= CW_MAX_COMPONENTS || L->map[saved] == MAP_UNDECLARED || cw_sig_has(seen, saved))
                return corrupt(L, &r);
            cw_sig_set(&seen, saved);
            if (length > L->cfg.max_component_bytes) return limit(L, &r);
            if (L->map[saved] != MAP_SKIP && L->enc[saved] == ENC_RAW && length != L->elem[saved])
                return corrupt(L, &r);
            size_t payload_at = where(L, &r);
            const void *payload = cw_reader_view(&r, length);
            if (!cw_reader_ok(&r)) return corrupt(L, &r);
            if (!apply || L->map[saved] == MAP_SKIP) continue;
            cw_component_id ct = (cw_component_id)L->map[saved];
            bool ok = L->enc[saved] == ENC_RAW
                ? cw_comp_set(L->w, id, ct, payload, length)
                : cw_comp_unmarshal(L->w, id, ct, payload, length);
            if (!ok) return unmarshal_failed(L, payload_at, id, ct);
        }
        if (apply && (flags & CW_TO_DESTROY)) cw_world_mark_destroy(L->w, id);
    }
    L->max_id = previous;
    return cw_reader_at_end(&r) || corrupt(L, &r);
}
/* Validates the recycled list; with `bits` set it also rejects ids that are
 * live entities or duplicates. */
static bool walk_recycled(loader *L, uint64_t *bits, int64_t *max_id) {
    cw_reader r = L->sections[SEC_RECYCLED];
    uint64_t count;
    cw_reader_u64(&r, &count);
    if (count > L->cfg.max_entities) return limit(L, &r);
    *max_id = -1;
    for (uint64_t i = 0; i < count; ++i) {
        int64_t id;
        if (!cw_reader_i64(&r, &id)) return corrupt(L, &r);
        if (id < 0 || id >= L->next_id) return corrupt(L, &r);
        if (id > L->cfg.max_entity_id) return limit(L, &r);
        if (id > *max_id) *max_id = id;
        if (!bits) continue;
        uint64_t *word = &bits[(size_t)id >> 6], bit = UINT64_C(1) << (id & 63);
        if (*word & bit) return corrupt(L, &r);
        *word |= bit;
    }
    return cw_reader_at_end(&r) || corrupt(L, &r);
}
static bool apply(loader *L) {
    cw_world *w = L->w;
    if (L->max_id >= 0) cw_world_ensure_capacity(w, L->max_id);
    if (!walk_entities(L, NULL, true)) return false;
    cw_reader r = L->sections[SEC_RECYCLED];
    uint64_t count;
    cw_reader_u64(&r, &count);
    cw_id_list *ids = &w->recycled_ids;
    if (count > ids->capacity) {
        ids->data = CW_RESIZE(ids->data, (size_t)count, sizeof(*ids->data));
        ids->capacity = (size_t)count;
    }
    for (uint64_t i = 0; i < count; ++i) {
        int64_t id;
        if (!cw_reader_i64(&r, &id)) return corrupt(L, &r);
        ids->data[ids->count++] = id;
    }
    w->recycle_ids = L->recycle;
    w->next_id = L->next_id;
    w->last_created = L->last_created;
    w->tick = L->tick;
    return true;
}
/* Shared by snapshot loading and deserialization. */
static bool load_image(cw_world *w, const void *data, size_t size, const cw_snapshot_load_config *config,
                       bool check_user_version, uint32_t user_version, cw_snapshot_error *error) {
    loader L = {0};
    L.w = w;
    L.error = error;
    L.cfg = *config;
    if (!L.cfg.max_entities) L.cfg.max_entities = CW_SNAPSHOT_DEFAULT_MAX_ENTITIES;
    if (!L.cfg.max_entity_id) L.cfg.max_entity_id = CW_SNAPSHOT_DEFAULT_MAX_ENTITY_ID;
    if (!L.cfg.max_component_bytes) L.cfg.max_component_bytes = CW_SNAPSHOT_DEFAULT_MAX_COMPONENT_BYTES;
    error_clear(error);
    if (!w) return fail(error, CW_SNAPSHOT_ERROR_ARGUMENT, 0);
    if (w->next_id || w->recycled_ids.count || cw_world_entity_count(w))
        return fail(error, CW_SNAPSHOT_ERROR_WORLD_NOT_FRESH, 0);
    if (!parse_header(&L, data, size)) return false;
    if (check_user_version && L.user_version != user_version)
        return fail(error, CW_SNAPSHOT_ERROR_VERSION, 16);
    /* next_id bounds future allocation, so it obeys the id limit too. */
    if (L.next_id > 0 && L.next_id - 1 > L.cfg.max_entity_id)
        return fail(error, CW_SNAPSHOT_ERROR_LIMIT, where(&L, &L.sections[SEC_WORLD]));
    if (!parse_schema(&L)) return false;

    /* Validate everything before the world is touched. */
    int64_t max_recycled;
    if (!walk_entities(&L, NULL, false) || !walk_recycled(&L, NULL, &max_recycled)) return false;
    int64_t highest = L.max_id > max_recycled ? L.max_id : max_recycled;
    size_t words = highest >= 0 ? (size_t)((uint64_t)highest / 64 + 1) : 1;
    uint64_t *bits = (uint64_t *)cw_resize(NULL, words, sizeof(*bits));
    memset(bits, 0, words * sizeof(*bits));
    bool consistent = walk_entities(&L, bits, false) && walk_recycled(&L, bits, &max_recycled);
    free(bits);
    if (!consistent) return false;

    /* Only a codec can still object. The partially built world is then
     * released through the ordinary reset path (cleanup callbacks run,
     * queries and observers are told), so it is fresh again and carries the
     * settings it had before. */
    bool recycle_before = w->recycle_ids;
    uint64_t tick_before = w->tick;
    if (apply(&L)) return true;
    cw_world_reset(w);
    w->recycle_ids = recycle_before;
    w->tick = tick_before;
    return false;
}
bool cw_snapshot_load(cw_world *w, const void *data, size_t size,
                      const cw_snapshot_load_config *config, cw_snapshot_error *error) {
    cw_snapshot_load_config cfg = config ? *config : cw_snapshot_load_defaults();
    return load_image(w, data, size, &cfg, false, 0, error);
}
cw_deserialize_config cw_deserialize_defaults(void) {
    cw_deserialize_config c = {0};
    c.max_entities = CW_SNAPSHOT_DEFAULT_MAX_ENTITIES;
    c.max_entity_id = CW_SNAPSHOT_DEFAULT_MAX_ENTITY_ID;
    c.max_component_bytes = CW_SNAPSHOT_DEFAULT_MAX_COMPONENT_BYTES;
    return c;
}
bool cw_world_deserialize(cw_world *w, const void *data, size_t size,
                          const cw_deserialize_config *config, cw_snapshot_error *error) {
    cw_deserialize_config cfg = config ? *config : cw_deserialize_defaults();
    cw_snapshot_load_config limits = cw_snapshot_load_defaults();
    limits.max_entities = cfg.max_entities;
    limits.max_entity_id = cfg.max_entity_id;
    limits.max_component_bytes = cfg.max_component_bytes;
    limits.allow_raw = cfg.allow_raw;
    limits.ignore_unknown_components = false;   /* every name in the image must resolve */
    return load_image(w, data, size, &limits, cfg.check_user_version, cfg.user_version, error);
}
