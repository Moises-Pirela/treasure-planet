// Wavefront OBJ, the parts of it a mesh needs: v, vn, f. A face's corners are
// "v", "v/vt", "v//vn" or "v/vt/vn"; faces of any size are fanned into
// triangles; indices may be negative (from the end). A file without normals
// gets flat ones, a vertex per corner. Pure: text in, arrays out.
#include "renderer.h"
#include <stdlib.h>
#include <string.h>

struct obj_corner { int position, normal; };

static const char *skip_space(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
    return p;
}

static const char *read_float(const char *p, const char *end, float *out) {
    char *stop;
    *out = strtof(p, &stop);
    return stop > p && stop <= end ? stop : NULL;
}

// A face corner: v, v/vt, v//vn or v/vt/vn. Missing parts are 0.
static const char *read_corner(const char *p, const char *end, obj_corner *out) {
    char *stop;
    long v = strtol(p, &stop, 10);
    if (stop == p) return NULL;
    out->position = (int)v;
    out->normal = 0;
    p = stop;
    if (p < end && *p == '/') {
        ++p;
        if (p < end && *p != '/') p = (strtol(p, &stop, 10), stop);   // the texture coordinate, unused
        if (p < end && *p == '/') {
            ++p;
            long n = strtol(p, &stop, 10);
            if (stop == p) return NULL;
            out->normal = (int)n;
            p = stop;
        }
    }
    return p;
}

struct grow { void *items; uint32_t count, capacity, size; };
static void *grow_push(grow *g) {
    if (g->count == g->capacity) {
        g->capacity = g->capacity ? g->capacity * 2 : 256;
        g->items = realloc(g->items, (size_t)g->capacity * g->size);
    }
    return (char *)g->items + (size_t)g->count++ * g->size;
}

static int resolve(int index, uint32_t count) {
    if (index > 0 && (uint32_t)index <= count) return index - 1;
    if (index < 0 && (uint32_t)(-index) <= count) return (int)count + index;
    return -1;
}

bool r_obj_parse(const char *text, size_t size, r_obj_mesh *out) {
    memset(out, 0, sizeof(*out));
    grow positions = {NULL, 0, 0, sizeof(v3)}, normals = {NULL, 0, 0, sizeof(v3)};
    grow vertices = {NULL, 0, 0, sizeof(r_vertex)}, indices = {NULL, 0, 0, sizeof(uint32_t)};
    bool ok = true;
    const char *p = text, *end = text + size;
    while (p < end && ok) {
        const char *line_end = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        const char *q = skip_space(p, line_end);
        if (line_end - q >= 2 && q[0] == 'v' && (q[1] == ' ' || q[1] == 'n')) {
            bool normal = q[1] == 'n';
            q += normal ? 2 : 1;
            v3 value;
            float *parts[3] = {&value.x, &value.y, &value.z};
            for (int k = 0; k < 3 && q; ++k) q = read_float(skip_space(q, line_end), line_end, parts[k]);
            if (!q) ok = false;
            else *(v3 *)grow_push(normal ? &normals : &positions) = value;
        } else if (line_end - q >= 2 && q[0] == 'f' && q[1] == ' ') {
            q += 1;
            obj_corner corners[64];
            int count = 0;
            for (;;) {
                q = skip_space(q, line_end);
                if (q >= line_end || count == 64) break;
                q = read_corner(q, line_end, &corners[count]);
                if (!q) break;
                ++count;
            }
            if (!q || count < 3) {
                ok = false;
                break;
            }
            // Fanned from the first corner; each corner a vertex of its own, with
            // the face's own normal when the file gives none.
            for (int t = 1; t + 1 < count && ok; ++t) {
                obj_corner tri[3] = {corners[0], corners[t], corners[t + 1]};
                v3 at[3] = {};   // every corner is set, or the face is refused; GCC cannot always tell
                for (int k = 0; k < 3; ++k) {
                    int index = resolve(tri[k].position, positions.count);
                    if (index < 0) ok = false;
                    else at[k] = ((v3 *)positions.items)[index];
                }
                if (!ok) break;
                v3 flat = v3_normalize(v3_cross(v3_sub(at[1], at[0]), v3_sub(at[2], at[0])));
                for (int k = 0; k < 3; ++k) {
                    v3 n = flat;
                    int index = resolve(tri[k].normal, normals.count);
                    if (tri[k].normal != 0 && index < 0) {
                        ok = false;
                        break;
                    }
                    if (index >= 0) n = ((v3 *)normals.items)[index];
                    r_vertex *v = (r_vertex *)grow_push(&vertices);
                    v->position[0] = at[k].x;
                    v->position[1] = at[k].y;
                    v->position[2] = at[k].z;
                    v->normal[0] = n.x;
                    v->normal[1] = n.y;
                    v->normal[2] = n.z;
                    *(uint32_t *)grow_push(&indices) = vertices.count - 1;
                }
            }
        }
        p = line_end + 1;
    }
    free(positions.items);
    free(normals.items);
    if (!ok || !indices.count) {
        free(vertices.items);
        free(indices.items);
        return false;
    }
    out->vertices = (r_vertex *)vertices.items;
    out->vertex_count = vertices.count;
    out->indices = (uint32_t *)indices.items;
    out->index_count = indices.count;
    return true;
}

void r_obj_free(r_obj_mesh *mesh) {
    free(mesh->vertices);
    free(mesh->indices);
    memset(mesh, 0, sizeof(*mesh));
}
