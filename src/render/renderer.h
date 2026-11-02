// The renderer: meshes go in once, then each frame is a list of draws handed over
// whole. The caller never touches OpenGL or Vulkan, and the two backends draw the
// same frame from the same shader source (tools/compare_captures.py checks this).
#ifndef TP_RENDERER_H
#define TP_RENDERER_H

#include "../platform/platform.h"
#include "../tp_math.h"
#include <stddef.h>
#include <stdint.h>

enum r_backend_kind { R_BACKEND_VULKAN, R_BACKEND_OPENGL };

typedef uint32_t r_mesh;         // 0 is no mesh

struct r_vertex { float position[3], normal[3]; };

// Matches the shader's Frame block (std140).
struct r_frame_uniforms {
    m4 view_proj;
    float sun_dir[4];            // xyz: direction towards the sun
    float sun_color[4];
    float ambient[4];
};

// Matches the shader's Draw push constants.
struct r_draw {
    m4 model;
    float color[4];              // rgb: linear albedo, a: 1 lit, 0 unlit
    r_mesh mesh;
};

struct r_frame {
    r_frame_uniforms uniforms;
    float clear[3];              // display colour, not linear
    const r_draw *draws;
    size_t draw_count;
    uint8_t *capture_rgba;       // when set, receives width * height * 4 bytes, top row first
};

struct renderer;

// Falls back to the other backend when the requested one cannot start, and reports
// the one in use through renderer_backend_name.
renderer *renderer_create(r_backend_kind kind, plat_window *window);
void renderer_destroy(renderer *r);
const char *renderer_backend_name(const renderer *r);

r_mesh renderer_mesh_create(renderer *r, const r_vertex *vertices, uint32_t vertex_count,
                            const uint32_t *indices, uint32_t index_count);
r_mesh renderer_mesh_cube(renderer *r);                       // half extents of 1
r_mesh renderer_mesh_sphere(renderer *r, int segments, int rings);    // radius 1
// A capsule standing on the origin: `height` tall overall, along +y.
r_mesh renderer_mesh_capsule(renderer *r, float radius, float height, int segments, int rings);
void renderer_mesh_destroy(renderer *r, r_mesh mesh);

// The frame is complete when this returns. width/height of 0 draws nothing.
void renderer_submit(renderer *r, const r_frame *frame, int width, int height);

#endif
