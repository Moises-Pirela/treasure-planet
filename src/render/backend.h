// The contract between the renderer and a graphics API.
//
// Clip space is the same on both: y up, depth 0..1, reversed (near is 1), so the
// depth test is "greater" and the depth buffer clears to 0. Triangles wind
// counter-clockwise when seen from the front. Colour targets are 8-bit and not
// sRGB: the shader does its own gamma.
#ifndef TP_RENDER_BACKEND_H
#define TP_RENDER_BACKEND_H

#include "renderer.h"

struct r_backend {
    const char *name;
    void *(*create)(plat_window *window);    // NULL when this API cannot run here
    void (*destroy)(void *state);
    r_mesh (*mesh_create)(void *state, const r_vertex *vertices, uint32_t vertex_count,
                          const uint32_t *indices, uint32_t index_count);
    void (*mesh_destroy)(void *state, r_mesh mesh);
    void (*submit)(void *state, const r_frame *frame, int width, int height);
};

extern const r_backend R_BACKEND_GL_API;
extern const r_backend R_BACKEND_VK_API;

#endif
