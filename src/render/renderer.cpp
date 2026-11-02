#include "backend.h"
#include <stdio.h>
#include <stdlib.h>

struct renderer {
    const r_backend *backend;
    void *state;
};

renderer *renderer_create(r_backend_kind kind, plat_window *window) {
    const r_backend *order[2] = {&R_BACKEND_VK_API, &R_BACKEND_GL_API};
    if (kind == R_BACKEND_OPENGL) {
        order[0] = &R_BACKEND_GL_API;
        order[1] = &R_BACKEND_VK_API;
    }
    for (int i = 0; i < 2; ++i) {
        void *state = order[i]->create(window);
        if (state) {
            renderer *r = (renderer *)calloc(1, sizeof(renderer));
            r->backend = order[i];
            r->state = state;
            return r;
        }
        fprintf(stderr, "renderer: %s is not available\n", order[i]->name);
    }
    return NULL;
}

void renderer_destroy(renderer *r) {
    if (!r) return;
    r->backend->destroy(r->state);
    free(r);
}

const char *renderer_backend_name(const renderer *r) { return r->backend->name; }

r_mesh renderer_mesh_create(renderer *r, const r_vertex *vertices, uint32_t vertex_count,
                            const uint32_t *indices, uint32_t index_count) {
    return r->backend->mesh_create(r->state, vertices, vertex_count, indices, index_count);
}

void renderer_mesh_destroy(renderer *r, r_mesh mesh) { r->backend->mesh_destroy(r->state, mesh); }

void renderer_submit(renderer *r, const r_frame *frame, int width, int height) {
    if (width > 0 && height > 0) r->backend->submit(r->state, frame, width, height);
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
