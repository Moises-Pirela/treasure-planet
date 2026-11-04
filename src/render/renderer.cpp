#include "backend.h"
#include "shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct model_slot {
    r_model_part parts[R_MAX_MODEL_PARTS];
    uint32_t count;                      // 0: free
};

struct renderer {
    const r_backend *backend;
    void *state;
    r_shader_set shaders;                // the material shaders the build embedded
    float mesh_radius[R_MAX_MESHES];     // bounding sphere of each mesh about its origin, for culling
    model_slot models[R_MAX_MODELS];     // slot 0 stays empty: model 0 means none
    r_scratch scratch;
    r_packet packet;
};

// Every materials/*.frag the build embedded is a material shader, named by its
// stem; one that says MATERIAL_CUTOUT cuts holes, and the shadow pass needs its
// own program for it. "surface" is what a material of no shader draws as.
static void find_shaders(r_shader_set *set) {
    memset(set, 0, sizeof(*set));
    uint32_t count = 0;
    const r_shader_file *files = r_shader_files(&count);
    for (uint32_t i = 0; i < count && set->count < R_MAX_SHADERS; ++i) {
        const char *name = files[i].name;
        size_t length = strlen(name);
        if (strncmp(name, "materials/", 10) != 0 || length < 16 || strcmp(name + length - 5, ".frag") != 0) continue;
        r_shader_info *info = &set->items[set->count];
        size_t stem = length - 10 - 5;
        if (stem >= sizeof(info->name) || length >= sizeof(info->file)) continue;
        memcpy(info->name, name + 10, stem);
        info->name[stem] = 0;
        strcpy(info->file, name);
        info->cutout = strstr((const char *)files[i].data, "#define MATERIAL_CUTOUT") != NULL;
        if (strcmp(info->name, "surface") == 0) set->surface = set->count;
        ++set->count;
    }
}

renderer *renderer_create(r_backend_kind kind, plat_window *window) {
    r_shader_set shaders;
    find_shaders(&shaders);
    if (!shaders.count) {
        fputs("renderer: no material shaders were built in\n", stderr);
        return NULL;
    }
    const r_backend *order[2] = {&R_BACKEND_VK_API, &R_BACKEND_GL_API};
    if (kind == R_BACKEND_OPENGL) {
        order[0] = &R_BACKEND_GL_API;
        order[1] = &R_BACKEND_VK_API;
    }
    for (int i = 0; i < 2; ++i) {
        void *state = order[i]->create(window, &shaders);
        if (state) {
            renderer *r = (renderer *)calloc(1, sizeof(renderer));
            r->backend = order[i];
            r->state = state;
            r->shaders = shaders;
            return r;
        }
        fprintf(stderr, "renderer: %s is not available\n", order[i]->name);
    }
    return NULL;
}

void renderer_destroy(renderer *r) {
    if (!r) return;
    r->backend->destroy(r->state);
    r_scratch_free(&r->scratch);
    free(r);
}

const char *renderer_backend_name(const renderer *r) { return r->backend->name; }

r_shader renderer_shader(const renderer *r, const char *name) {
    for (uint32_t i = 0; i < r->shaders.count; ++i)
        if (strcmp(r->shaders.items[i].name, name) == 0) return i + 1;
    return 0;
}

r_mesh renderer_mesh_create(renderer *r, const r_vertex *vertices, uint32_t vertex_count,
                            const uint32_t *indices, uint32_t index_count) {
    r_mesh mesh = r->backend->mesh_create(r->state, vertices, vertex_count, indices, index_count);
    if (!mesh) return 0;
    float radius2 = 0.0f;
    for (uint32_t i = 0; i < vertex_count; ++i) {
        const float *p = vertices[i].position;
        float d2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
        if (d2 > radius2) radius2 = d2;
    }
    r->mesh_radius[mesh] = sqrtf(radius2);
    return mesh;
}

void renderer_mesh_destroy(renderer *r, r_mesh mesh) {
    r->backend->mesh_destroy(r->state, mesh);
    if (mesh < R_MAX_MESHES) r->mesh_radius[mesh] = 0.0f;
}

r_mesh renderer_mesh_load_obj(renderer *r, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return 0;
    }
    char *text = (char *)malloc((size_t)size);
    size_t read = fread(text, 1, (size_t)size, f);
    fclose(f);
    r_obj_mesh obj;
    r_mesh mesh = 0;
    if (read == (size_t)size && r_obj_parse(text, read, &obj)) {
        mesh = renderer_mesh_create(r, obj.vertices, obj.vertex_count, obj.indices, obj.index_count);
        r_obj_free(&obj);
    }
    free(text);
    return mesh;
}

r_model renderer_model_create(renderer *r, const r_model_part *parts, uint32_t count) {
    if (!count || count > R_MAX_MODEL_PARTS) return 0;
    for (r_model m = 1; m < R_MAX_MODELS; ++m) {
        if (r->models[m].count) continue;
        memcpy(r->models[m].parts, parts, count * sizeof(r_model_part));
        r->models[m].count = count;
        return m;
    }
    return 0;
}

void renderer_model_destroy(renderer *r, r_model model) {
    if (model && model < R_MAX_MODELS) r->models[model].count = 0;
}

uint32_t renderer_model_draws(const renderer *r, r_model model, const m4 *transform, const float color[4], uint32_t flags,
                              r_draw *out, uint32_t capacity) {
    if (!model || model >= R_MAX_MODELS) return 0;
    return r_model_draws(r->models[model].parts, r->models[model].count, transform, color, flags, out, capacity);
}

const char *r_pass_name(int pass) {
    static const char *const NAMES[R_PASS_COUNT] = {"shadow", "main", "sky", "downsample", "upsample", "flare", "composite", "fxaa", "outline", "haze"};
    return pass >= 0 && pass < R_PASS_COUNT ? NAMES[pass] : "?";
}

r_timings renderer_timings(const renderer *r) { return r->backend->timings(r->state); }

void renderer_submit(renderer *r, const r_frame *frame, int width, int height) {
    if (width <= 0 || height <= 0) return;
    r_prepare(frame, r->mesh_radius, &r->shaders, width, height, &r->scratch, &r->packet);
    r->backend->submit(r->state, &r->packet, width, height);
}

r_mesh renderer_mesh_cube(renderer *r) {
    // One face per row: its normal and two edges, with edge_a x edge_b = normal.
    static const float FACES[6][9] = {
        { 1, 0, 0,   0, 0,-1,   0, 1, 0},
        {-1, 0, 0,   0, 0, 1,   0, 1, 0},
        { 0, 1, 0,   1, 0, 0,   0, 0,-1},
        { 0,-1, 0,   1, 0, 0,   0, 0, 1},
        { 0, 0, 1,   1, 0, 0,   0, 1, 0},
        { 0, 0,-1,  -1, 0, 0,   0, 1, 0},
    };
    static const float CORNERS[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    r_vertex vertices[24];
    uint32_t indices[36];
    for (uint32_t f = 0; f < 6; ++f) {
        const float *row = FACES[f];
        for (uint32_t c = 0; c < 4; ++c) {
            r_vertex *v = &vertices[f * 4 + c];
            for (int k = 0; k < 3; ++k) {
                v->position[k] = row[k] + row[3 + k] * CORNERS[c][0] + row[6 + k] * CORNERS[c][1];
                v->normal[k] = row[k];
            }
        }
        const uint32_t QUAD[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t i = 0; i < 6; ++i) indices[f * 6 + i] = f * 4 + QUAD[i];
    }
    return renderer_mesh_create(r, vertices, 24, indices, 36);
}

r_mesh renderer_mesh_sphere(renderer *r, int segments, int rings) {
    uint32_t vertex_count = (uint32_t)((segments + 1) * (rings + 1));
    uint32_t index_count = (uint32_t)(segments * rings * 6);
    r_vertex *vertices = (r_vertex *)malloc(vertex_count * sizeof(r_vertex));
    uint32_t *indices = (uint32_t *)malloc(index_count * sizeof(uint32_t));
    r_vertex *v = vertices;
    for (int ring = 0; ring <= rings; ++ring) {
        float polar = TP_PI * (float)ring / (float)rings;     // 0 at the north pole
        for (int segment = 0; segment <= segments; ++segment) {
            float around = 2.0f * TP_PI * (float)segment / (float)segments;
            float x = sinf(polar) * sinf(around), y = cosf(polar), z = sinf(polar) * cosf(around);
            v->position[0] = v->normal[0] = x;
            v->position[1] = v->normal[1] = y;
            v->position[2] = v->normal[2] = z;
            ++v;
        }
    }
    uint32_t *out = indices, stride = (uint32_t)segments + 1;
    for (uint32_t ring = 0; ring < (uint32_t)rings; ++ring)
        for (uint32_t segment = 0; segment < (uint32_t)segments; ++segment) {
            uint32_t top = ring * stride + segment, bottom = top + stride;
            *out++ = top;     *out++ = bottom;      *out++ = bottom + 1;
            *out++ = top;     *out++ = bottom + 1;  *out++ = top + 1;
        }
    r_mesh mesh = renderer_mesh_create(r, vertices, vertex_count, indices, index_count);
    free(vertices);
    free(indices);
    return mesh;
}

r_mesh renderer_mesh_capsule(renderer *r, float radius, float height, int segments, int rings) {
    // A sphere whose rings are split at the equator and pulled apart by the
    // cylinder's length, so the normals stay those of the sphere: rings is per
    // hemisphere, and the two equator rings share the seam.
    float shift = height - 2.0f * radius;
    if (shift < 0.0f) shift = 0.0f;
    int total_rings = rings * 2 + 1;       // rings 0..rings are the top half, rings+1.. the bottom
    uint32_t vertex_count = (uint32_t)((segments + 1) * (total_rings + 1));
    uint32_t index_count = (uint32_t)(segments * total_rings * 6);
    r_vertex *vertices = (r_vertex *)malloc(vertex_count * sizeof(r_vertex));
    uint32_t *indices = (uint32_t *)malloc(index_count * sizeof(uint32_t));
    r_vertex *v = vertices;
    for (int ring = 0; ring <= total_rings; ++ring) {
        bool top = ring <= rings;
        int local = top ? ring : ring - 1;                         // 0..2*rings over the whole sphere
        float polar = TP_PI * (float)local / (float)(2 * rings);   // 0 at the north pole, pi at the south
        float center_y = top ? radius + shift : radius;
        for (int segment = 0; segment <= segments; ++segment) {
            float around = 2.0f * TP_PI * (float)segment / (float)segments;
            float x = sinf(polar) * sinf(around), y = cosf(polar), z = sinf(polar) * cosf(around);
            v->normal[0] = x;
            v->normal[1] = y;
            v->normal[2] = z;
            v->position[0] = x * radius;
            v->position[1] = center_y + y * radius;
            v->position[2] = z * radius;
            ++v;
        }
    }
    uint32_t *out = indices, stride = (uint32_t)segments + 1;
    for (uint32_t ring = 0; ring < (uint32_t)total_rings; ++ring)
        for (uint32_t segment = 0; segment < (uint32_t)segments; ++segment) {
            uint32_t a = ring * stride + segment, b = a + stride;
            *out++ = a;  *out++ = b;      *out++ = b + 1;
            *out++ = a;  *out++ = b + 1;  *out++ = a + 1;
        }
    r_mesh mesh = renderer_mesh_create(r, vertices, vertex_count, indices, index_count);
    free(vertices);
    free(indices);
    return mesh;
}
