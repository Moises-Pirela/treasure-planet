#include "cw_internal.h"

/* ------------------------------------------------------------------------- */
/* Writer                                                                    */
/* ------------------------------------------------------------------------- */
void cw_writer_init(cw_writer *w) {
    cw_require(w != NULL);
    memset(w, 0, sizeof(*w));
    w->owned = true;
}
void cw_writer_init_fixed(cw_writer *w, void *data, size_t capacity) {
    cw_require(w != NULL);
    memset(w, 0, sizeof(*w));
    w->data = (unsigned char *)data;
    w->capacity = data ? capacity : 0;
}
void cw_writer_destroy(cw_writer *w) {
    if (!w) return;
    if (w->owned) free(w->data);
    memset(w, 0, sizeof(*w));
}
void cw_writer_clear(cw_writer *w) {
    if (!w) return;
    w->size = 0;
    w->overflow = false;
}
void cw_writer_reserve(cw_writer *w, size_t capacity) {
    if (!w || !w->owned || capacity <= w->capacity) return;
    w->data = CW_RESIZE(w->data, capacity, 1);
    w->capacity = capacity;
}
unsigned char *cw_writer_release(cw_writer *w, size_t *size) {
    if (size) *size = 0;
    if (!w || !w->owned) return NULL;
    unsigned char *data = w->data;
    if (size) *size = w->size;
    cw_writer_init(w);
    return data;
}
size_t cw_writer_size(const cw_writer *w) { return w ? w->size : 0; }
bool cw_writer_ok(const cw_writer *w) { return w && !w->overflow; }
void *cw_writer_claim(cw_writer *w, size_t size) {
    if (!w) return NULL;
    /* The byte count itself must not wrap; that is an overflow too. */
    if (size > SIZE_MAX - w->size) { w->overflow = true; return NULL; }
    size_t end = w->size + size;
    if (w->owned) {
        if (end > w->capacity) {
            size_t capacity = w->capacity ? cw_mul(w->capacity, 2) : 256;
            cw_writer_reserve(w, capacity > end ? capacity : end);
        }
        void *slot = w->data + w->size;
        w->size = end;
        return slot;
    }
    void *slot = NULL;
    if (w->data) {
        if (end <= w->capacity) slot = w->data + w->size;
        else w->overflow = true;
    }
    w->size = end;
    return slot;
}
static bool put(cw_writer *w, const void *data, size_t size) {
    void *slot = cw_writer_claim(w, size);
    if (slot && size) memcpy(slot, data, size);
    return w && !w->overflow;
}
static bool put_le(cw_writer *w, uint64_t value, unsigned bytes) {
    unsigned char raw[8];
    for (unsigned i = 0; i < bytes; ++i) raw[i] = (unsigned char)(value >> (8 * i));
    return put(w, raw, bytes);
}
bool cw_writer_bytes(cw_writer *w, const void *data, size_t size) {
    if (!w || (!data && size)) return false;
    return put(w, data, size);
}
bool cw_writer_u8(cw_writer *w, uint8_t value) { return put_le(w, value, 1); }
bool cw_writer_u16(cw_writer *w, uint16_t value) { return put_le(w, value, 2); }
bool cw_writer_u32(cw_writer *w, uint32_t value) { return put_le(w, value, 4); }
bool cw_writer_u64(cw_writer *w, uint64_t value) { return put_le(w, value, 8); }
bool cw_writer_i8(cw_writer *w, int8_t value) { return put_le(w, (uint8_t)value, 1); }
bool cw_writer_i16(cw_writer *w, int16_t value) { return put_le(w, (uint16_t)value, 2); }
bool cw_writer_i32(cw_writer *w, int32_t value) { return put_le(w, (uint32_t)value, 4); }
bool cw_writer_i64(cw_writer *w, int64_t value) { return put_le(w, (uint64_t)value, 8); }
bool cw_writer_f32(cw_writer *w, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return put_le(w, bits, 4);
}
bool cw_writer_f64(cw_writer *w, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return put_le(w, bits, 8);
}
static bool patch_le(cw_writer *w, size_t offset, uint64_t value, unsigned bytes) {
    if (!w || offset > w->size || bytes > w->size - offset) return false;
    /* Measuring writers and the unstored tail of an overflowed fixed buffer
     * have nothing to patch; the field is in range, so that is not an error. */
    if (!w->data || offset + bytes > w->capacity) return !w->data;
    for (unsigned i = 0; i < bytes; ++i) w->data[offset + i] = (unsigned char)(value >> (8 * i));
    return true;
}
bool cw_writer_patch_u32(cw_writer *w, size_t offset, uint32_t value) {
    return patch_le(w, offset, value, 4);
}
bool cw_writer_patch_u64(cw_writer *w, size_t offset, uint64_t value) {
    return patch_le(w, offset, value, 8);
}

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */
cw_reader cw_reader_make(const void *data, size_t size) {
    cw_reader r = {0};
    r.data = (const unsigned char *)data;
    r.size = data ? size : 0;
    r.error = !data && size;
    return r;
}
size_t cw_reader_remaining(const cw_reader *r) { return r && !r->error ? r->size - r->offset : 0; }
bool cw_reader_ok(const cw_reader *r) { return r && !r->error; }
bool cw_reader_at_end(const cw_reader *r) { return cw_reader_ok(r) && r->offset == r->size; }
/* Consumes `size` bytes and reports where they start. The offset never
 * exceeds the size, so the remaining-bytes comparison cannot wrap. */
static bool take(cw_reader *r, size_t size, const unsigned char **at) {
    *at = NULL;
    if (!r || r->error) return false;
    if (size > r->size - r->offset) { r->error = true; return false; }
    if (r->data) *at = r->data + r->offset;
    r->offset += size;
    return true;
}
bool cw_reader_skip(cw_reader *r, size_t size) {
    const unsigned char *at;
    return take(r, size, &at);
}
bool cw_reader_bytes(cw_reader *r, void *out, size_t size) {
    const unsigned char *at;
    if (!r || (!out && size) || !take(r, size, &at)) return false;
    if (size) memcpy(out, at, size);
    return true;
}
const void *cw_reader_view(cw_reader *r, size_t size) {
    const unsigned char *at;
    take(r, size, &at);
    return at;
}
bool cw_reader_sub(cw_reader *r, size_t size, cw_reader *sub) {
    const unsigned char *at;
    if (!sub) return false;
    bool ok = take(r, size, &at);
    *sub = cw_reader_make(ok ? at : NULL, ok ? size : 0);
    sub->error = !ok;                    /* a failed sub-reader is poisoned */
    return ok;
}
static bool get_le(cw_reader *r, uint64_t *out, unsigned bytes) {
    const unsigned char *at;
    *out = 0;
    if (!take(r, bytes, &at)) return false;
    for (unsigned i = 0; i < bytes; ++i) *out |= (uint64_t)at[i] << (8 * i);
    return true;
}
/* Two's complement reinterpretation without relying on implementation-defined
 * narrowing of out-of-range values. */
static int64_t to_signed(uint64_t value, unsigned bytes) {
    if (bytes < 8) {
        uint64_t range = UINT64_C(1) << (8 * bytes);
        return value < range / 2 ? (int64_t)value : (int64_t)value - (int64_t)range;
    }
    /* ~value = 2^64 - 1 - value fits in int64 once the sign bit is set. */
    return value <= (uint64_t)INT64_MAX ? (int64_t)value : -(int64_t)(~value) - 1;
}
bool cw_reader_u8(cw_reader *r, uint8_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 1);
    if (out) *out = (uint8_t)v;
    return ok;
}
bool cw_reader_u16(cw_reader *r, uint16_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 2);
    if (out) *out = (uint16_t)v;
    return ok;
}
bool cw_reader_u32(cw_reader *r, uint32_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 4);
    if (out) *out = (uint32_t)v;
    return ok;
}
bool cw_reader_u64(cw_reader *r, uint64_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 8);
    if (out) *out = v;
    return ok;
}
bool cw_reader_i8(cw_reader *r, int8_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 1);
    if (out) *out = (int8_t)to_signed(v, 1);
    return ok;
}
bool cw_reader_i16(cw_reader *r, int16_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 2);
    if (out) *out = (int16_t)to_signed(v, 2);
    return ok;
}
bool cw_reader_i32(cw_reader *r, int32_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 4);
    if (out) *out = (int32_t)to_signed(v, 4);
    return ok;
}
bool cw_reader_i64(cw_reader *r, int64_t *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 8);
    if (out) *out = to_signed(v, 8);
    return ok;
}
bool cw_reader_f32(cw_reader *r, float *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 4);
    if (out) {
        uint32_t bits = (uint32_t)v;
        memcpy(out, &bits, sizeof(*out));
    }
    return ok;
}
bool cw_reader_f64(cw_reader *r, double *out) {
    uint64_t v = 0;
    bool ok = r && out && get_le(r, &v, 8);
    if (out) memcpy(out, &v, sizeof(*out));
    return ok;
}
