#ifndef CW_INTERNAL_H
#define CW_INTERNAL_H
#include "clockwork.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void cw_require(bool condition) {
    if (!condition) {
        fputs("clockwork: allocation failure, size overflow, or invalid lifecycle use\n", stderr);
        abort();
    }
}
static inline size_t cw_mul(size_t a, size_t b) {
    cw_require(!b || a <= SIZE_MAX / b);
    return a * b;
}
static inline size_t cw_add(size_t a, size_t b) {
    cw_require(a <= SIZE_MAX - b);
    return a + b;
}
static inline void *cw_resize(void *ptr, size_t count, size_t size) {
    void *result = realloc(ptr, cw_mul(count, size));
    cw_require(result != NULL);
    return result;
}
/* realloc through cw_resize, keeping the pointer's type. */
#define CW_RESIZE(ptr, count, size) ((decltype(ptr))cw_resize((ptr), (count), (size)))
static inline char *cw_copy_string(const char *s) {
    size_t n = cw_add(strlen(s ? s : ""), 1);
    char *copy = (char *)cw_resize(NULL, n, 1);
    memcpy(copy, s ? s : "", n);
    return copy;
}
static inline bool cw_registered(const cw_world *w, cw_component_id ct) {
    return w && ct < CW_MAX_COMPONENTS && w->pools[ct].elem_size != 0;
}
#if (defined(__GNUC__) || defined(__clang__)) && !defined(CW_NO_BUILTINS)
#define CW_HAVE_BUILTINS 1
#endif
static inline unsigned cw_ctz64(uint64_t bits) {
#ifdef CW_HAVE_BUILTINS
    return (unsigned)__builtin_ctzll(bits);
#else
    unsigned n = 0;
    if (!(bits & UINT64_C(0xffffffff))) { n += 32; bits >>= 32; }
    if (!(bits & UINT64_C(0xffff))) { n += 16; bits >>= 16; }
    if (!(bits & UINT64_C(0xff))) { n += 8; bits >>= 8; }
    if (!(bits & UINT64_C(0xf))) { n += 4; bits >>= 4; }
    if (!(bits & UINT64_C(0x3))) { n += 2; bits >>= 2; }
    if (!(bits & UINT64_C(0x1))) ++n;
    return n;
#endif
}
static inline unsigned cw_popcount64(uint64_t bits) {
#ifdef CW_HAVE_BUILTINS
    return (unsigned)__builtin_popcountll(bits);
#else
    bits -= (bits >> 1) & UINT64_C(0x5555555555555555);
    bits = (bits & UINT64_C(0x3333333333333333)) + ((bits >> 2) & UINT64_C(0x3333333333333333));
    bits = (bits + (bits >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return (unsigned)((bits * UINT64_C(0x0101010101010101)) >> 56);
#endif
}
/* Number of 64-bit words needed to hold one bit per entity slot. */
static inline size_t cw_entity_words(const cw_world *w) {
    return w ? cw_add(w->capacity, 63) / 64 : 0;
}

/* Entity bitset for one filter; shared by every handle with that filter. */
struct cw_query_cache {
    cw_world *world;
    cw_filter filter;
    cw_signature interest;      /* all | any | none */
    bool any_empty;             /* filter.any has no components */
    bool matches_empty;         /* a component-less visible entity matches */
    uint64_t *bits;             /* one bit per entity slot, cw_entity_words(world) words */
    size_t words;
    size_t count;               /* number of set bits */
    size_t refs;                /* handles using this cache */
    size_t index;               /* position in world->queries.caches */
};
/* User-facing query: a cache plus the declared component access. */
struct cw_cached_query {
    cw_query_cache *cache;
    cw_access access;
    size_t refs;
    size_t index;               /* position in world->queries.handles */
};
/* One consumer's change cursor for one component. */
struct cw_observer {
    cw_world *world;
    cw_component_id component;
    uint64_t *pending[CW_CHANGE_KINDS];    /* accumulating since the last observe */
    uint64_t *observed[CW_CHANGE_KINDS];   /* stable result of the last observe */
    size_t pending_count[CW_CHANGE_KINDS], observed_count[CW_CHANGE_KINDS];
    size_t words;
    uint64_t observations;
    size_t index;               /* position in world->observers.items */
};
#endif
