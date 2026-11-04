// The contract between the renderer and a graphics API.
//
// A backend executes an r_packet, which src/render/frame.cpp prepares from an
// r_frame: the uniform blocks are already laid out for the shader, every pass
// already knows which draws it contains, the lights are already chosen, the post
// steps are listed. In order:
//   1. shadow: for each cascade in use, depth only, into layer `c` of the shadow
//      map array from the cascade's matrix; front faces culled;
//   2. main: the lit pass into the HDR scene target, back faces culled, sampling
//      the shadow map;
//   3. sky: a full-screen triangle at depth 0, drawn where nothing else was;
//   4. the post steps: each one program, up to three input targets, one output
//      target, four parameters, drawn as a full-screen triangle. The last one
//      writes the window.
//
// Render targets are a fixed table (r_target): the backend owns one of each at the
// window's size over its shift, and r_target_info says what it is. One of them,
// R_TARGET_DEPTH, is the scene's depth buffer itself, readable by a post step
// (sampled unfiltered, depth in red) and never a step's output.
//
// Clip space is the same on both: y up, depth 0..1, reversed (near is 1), so the
// depth test is "greater" and depth buffers clear to 0; the sky uses "greater or
// equal" at depth 0 exactly. Triangles wind counter-clockwise when seen from the
// front. Colour targets are 8-bit and not sRGB: the shader does its own gamma.
#ifndef TP_RENDER_BACKEND_H
#define TP_RENDER_BACKEND_H

#include "renderer.h"


#define R_BLOOM_MIPS 6               // the most halvings the bloom chain can have
#define R_MAX_AIRS 4                 // atmospheres whose haze the shader sees at once: the nearest
#define R_SKY_LAYER_VEC4S 6          // a sky layer's size in the frame block
#define R_FXAA_MAX_ITERATIONS 12

// The shader's bindings (descriptor set 0 in Vulkan). A post step's inputs are
// texture units R_BIND_INPUT + i in OpenGL, descriptor sets 1 + i in Vulkan.
#define R_BIND_FRAME 0
#define R_BIND_LIGHTS 1
#define R_BIND_SHADOW_MAP 2
#define R_BIND_POST 3
#define R_BIND_MATERIALS 5
#define R_BIND_INPUT 4

// The render targets. R_TARGET_WINDOW is what is presented (8-bit); the rest are
// the backend's own, floating point (hdr) or 8-bit, at the window's size shifted
// right by `shift`.
enum r_target {
    R_TARGET_WINDOW,
    R_TARGET_SCENE,              // HDR, with the depth buffer: the lit pass and the sky
    R_TARGET_DEPTH,              // the scene's depth, as the steps read it
    R_TARGET_SCENE_B,            // HDR: the other half of a ping-pong with the scene for the full-size steps
    R_TARGET_LDR,                // the composite's output when anti-aliasing follows
    R_TARGET_FLARE,              // HDR, quarter size
    R_TARGET_BLOOM_DOWN,         // HDR, R_BLOOM_MIPS of them from half size down
    R_TARGET_BLOOM_UP = R_TARGET_BLOOM_DOWN + R_BLOOM_MIPS,    // the same sizes, on the way back up
    R_TARGET_COUNT = R_TARGET_BLOOM_UP + R_BLOOM_MIPS,
};
struct r_target_desc { bool hdr; int shift; bool depth; };
static inline r_target_desc r_target_info(int target) {
    r_target_desc d = {false, 0, false};
    if (target == R_TARGET_SCENE || target == R_TARGET_SCENE_B) d.hdr = true;
    else if (target == R_TARGET_DEPTH) d.depth = true;
    else if (target == R_TARGET_FLARE) { d.hdr = true; d.shift = 2; }
    else if (target >= R_TARGET_BLOOM_DOWN && target < R_TARGET_COUNT) {
        d.hdr = true;
        d.shift = 1 + (target - R_TARGET_BLOOM_DOWN) % R_BLOOM_MIPS;
    }
    return d;
}

enum r_post_program {
    R_POST_DOWNSAMPLE,           // inputs: the finer mip; params.x: 1 on the first, which thresholds and tames fireflies
    R_POST_UPSAMPLE,             // inputs: the coarser result, this level's downsample; params: radius, scatter
    R_POST_FLARE,                // inputs: a bloom mip
    R_POST_COMPOSITE,            // inputs: the scene, the bloom, the flare; params: write luma to alpha, bloom on, flare on
    R_POST_FXAA,                 // inputs: the composite's output with luma in alpha
    R_POST_OUTLINE,              // inputs: the scene, the depth
    R_POST_HAZE,                 // inputs: the scene, the depth
    R_POST_PROGRAM_COUNT,
};
struct r_post_step {
    uint8_t program;             // r_post_program
    uint8_t target;              // r_target
    uint8_t inputs[3];           // r_target; an unused input is a valid target the program ignores
    float params[4];
};
#define R_MAX_POST_STEPS (2 * R_BLOOM_MIPS + 5)

// Matches the shader's Frame block (std140). Every member is 16-byte aligned by
// construction, so the C layout is the GPU layout.
struct r_gpu_frame {
    m4 view_proj;
    m4 shadow[R_SHADOW_CASCADES];    // world -> a cascade's clip space (orthographic, w = 1)
    float camera_position[4];
    float camera_right[4];           // w: tan of half the horizontal field of view
    float camera_up[4];              // w: tan of half the vertical one
    float camera_forward[4];         // w: the near plane, which reversed depth is the near plane over
    float sun_dir[4];                // xyz: towards the sun; w: cos of the disc's angular radius
    float sun_color[4];              // w: the glow around the disc
    float ambient_sky[4], ambient_ground[4];
    float sky_zenith[4], sky_horizon[4], sky_ground[4];
    float cascade_splits[4];         // view distance at which each cascade ends
    float cascade_texel[4];          // metres per shadow texel, per cascade
    float shadow_params[4];          // x: 1 / resolution, y: normal offset in texels, z: depth bias, w: cascades in use
    float sky[4];                    // x: sky layers, z: star cells per cube face, w: time
    float ramp[4];                   // x: softness, y: wrap, z: flatness, w: steps
    float shadow_tint[4];            // rgb; (1, 1, 1) for none; w: rim power
    float rim[4];                    // rgb: colour, w: strength
    float haze[4];                   // rgb: the haze about the camera (the cloud deck's), w: its density (0: none)
    float air_zenith[4];             // rgb: the atmosphere's sky overhead, w: the immersion
    float air_horizon[4];            // rgb: its sky at the horizon, w: its sun glow times the immersion
    float air[4];                    // x: star fade times the immersion
    float air_sphere[R_MAX_AIRS][4]; // the atmospheres' shells: xyz centre, w radius to the top of the air
    float air_haze[R_MAX_AIRS][4];   // rgb: each one's haze, w: its density per metre of air
    float cloud_sphere[R_MAX_AIRS][4];   // each one's cloud layer: xyz centre, w radius (0: none)
    float cloud_layer[R_MAX_AIRS][4];    // x: cover, y: detail, z: sharpness, w: seed
    float cloud_turn[R_MAX_AIRS][4];     // x: how far the layer has turned
    float sky_layers[R_MAX_SKY_LAYERS][R_SKY_LAYER_VEC4S][4];   // each: see common.glsl's u_sky_layers
    uint32_t counts[4];              // x: point lights, y: sky layers, z: atmospheres
};
static_assert(sizeof(r_gpu_frame) == 64 * (1 + R_SHADOW_CASCADES) + (23 + 5 * R_MAX_AIRS + R_SKY_LAYER_VEC4S * R_MAX_SKY_LAYERS) * 16,
              "Frame block layout");

// Matches the shader's Lights block (std140): two arrays of vec4.
struct r_gpu_lights {
    float position[R_MAX_POINT_LIGHTS][4];   // w: radius
    float color[R_MAX_POINT_LIGHTS][4];
};

// Matches the shader's Post block (std140).
struct r_gpu_post {
    float tonemap[4];                // x: exposure, y: curve, z: white point (0: the curve's own), w: output gamma
    float balance[4];                // rgb: white balance multipliers, w: saturation
    float grade[4];                  // x: contrast
    float lift[4], gamma[4], gain[4];   // rgb; gamma as the exponent
    float vignette[4];               // x: strength, y: radius, z: softness
    float bloom[4];                  // x: intensity, y: threshold, z: knee
    float flare[4];                  // x: intensity, y: ghosts, z: dispersal, w: falloff
    float flare_halo[4];             // x: width, y: intensity, z: chromatic, w: threshold
    float aberration[4];             // x: strength
    float fxaa[4];                   // x: threshold min, y: threshold, z: subpixel, w: iterations
    float entry[4];                  // xy: where the camera is going, in uv; z: strength (0: none); w: blur length
    float entry_glow[4];             // rgb: the plasma's colour times the glow; w: samples
    float entry_streaks[4];          // x: streaks around the point of travel, y: time, z: streak length, w: streak speed
    float haze_color[4];             // rgb; w: density per metre (0: none)
    float haze_shape[4];             // x: base height, y: falloff above it, z: start distance, w: the sky's distance (0: infinite)
    float haze_sun[4];               // rgb: the glow towards the sun, w: its power
    float outline[4];                // x: strength (0: none), y: width in pixels, z: depth threshold, w: normal threshold (0: none)
    float outline_color[4];          // rgb; w: fade distance (0: never)
};

// Matches the shader's Materials block (std140): every material's parameters.
struct r_gpu_materials {
    float items[R_MAX_MATERIALS][R_MATERIAL_VEC4S][4];
};

// The material shaders a renderer was made with, in the order the backend
// compiled them: a draw's material names one by index (r_packet.material_shader).
struct r_shader_info {
    char name[32];                   // "planet"
    char file[64];                   // "materials/planet.frag"
    bool cutout;                     // cuts holes (MATERIAL_CUTOUT): the shadow pass needs its own program
};
struct r_shader_set {
    r_shader_info items[R_MAX_SHADERS];
    uint32_t count;
    uint32_t surface;                // the index of "surface", what a material of no shader draws as
};

// Matches the shader's Draw block: Vulkan push constants, OpenGL uniforms.
struct r_push {
    m4 model;
    float color[4];
    uint32_t flags;
    uint32_t material;
    uint32_t lights;                 // a bit per selected point light whose reach touches the draw
    uint32_t cascade;                // shadow pass only: which layer is being drawn
};
#define R_PUSH_DRAW_SIZE ((uint32_t)offsetof(r_push, cascade))   // what changes per draw

// Matches the shader's Step block, a post step's push constants / uniforms.
struct r_post_push {
    float texel[4];                  // xy: one texel of the first input, zw: one of the target, in uv
    float params[4];                 // the step's
};

struct r_pass { uint32_t first, count; };    // a range of r_packet.indices
static_assert(R_MAX_POINT_LIGHTS <= 32, "a draw's lights are a 32-bit mask");

struct r_packet {
    r_gpu_frame frame;
    r_gpu_lights lights;
    r_gpu_post post;
    r_gpu_materials materials;
    uint8_t material_shader[R_MAX_MATERIALS];   // into the shader set, per material
    uint8_t material_cutout[R_MAX_MATERIALS];   // the material's shader cuts holes
    r_post_step steps[R_MAX_POST_STEPS];
    uint32_t step_count;                     // at least one: the composite
    const r_draw *draws;                     // the frame's draws, unchanged
    const uint32_t *indices;                 // into draws; the passes are ranges of it
    const uint32_t *light_masks;             // per draw (indexed like draws): which selected lights reach it
    r_pass shadow[R_SHADOW_CASCADES];
    uint32_t cascade_count;                  // same as frame.shadow_params[3]
    uint32_t shadow_resolution;              // texels across each cascade's map; the backend reallocates when it changes
    r_pass main;
    uint8_t *capture_rgba;
};

struct r_backend {
    const char *name;
    void *(*create)(plat_window *window, const r_shader_set *shaders);    // NULL when this API cannot run here
    void (*destroy)(void *state);
    r_mesh (*mesh_create)(void *state, const r_vertex *vertices, uint32_t vertex_count,
                          const uint32_t *indices, uint32_t index_count);
    void (*mesh_destroy)(void *state, r_mesh mesh);
    void (*submit)(void *state, const r_packet *packet, int width, int height);
    r_timings (*timings)(void *state);       // of the last frame whose queries are in
};

// A post program's pass, for the timings.
static inline int r_post_program_pass(int program) { return R_PASS_DOWNSAMPLE + program; }

extern const r_backend R_BACKEND_GL_API;
extern const r_backend R_BACKEND_VK_API;

// ---------------------------------------------------------------------------
// Preparing a frame (frame.cpp): pure, no API, no window, so tests/test_render.cpp
// can run it headless.
// ---------------------------------------------------------------------------
struct r_scratch {                           // the index storage a packet points into
    uint32_t *indices;
    size_t capacity;
};
void r_scratch_free(r_scratch *scratch);

// `mesh_radius[mesh]` is the bounding sphere (about the origin) of each mesh in
// object space, as renderer_mesh_create records it; a draw of a mesh with no entry
// is never culled. `shaders` maps the materials' shaders to the backend's (NULL:
// everything is shader 0). `out` points into `scratch` afterwards and is valid
// until the next call.
void r_prepare(const r_frame *frame, const float *mesh_radius, const r_shader_set *shaders, int width, int height,
               r_scratch *scratch, r_packet *out);

// How deep a camera at `at` is in an atmosphere: 0 outside the shell, 1 at the surface.
float r_atmosphere_immersion(const r_atmosphere *atmosphere, v3 at);

// How many bloom halvings a window of this size can hold (the coarsest mip stays
// a few pixels across), at most R_BLOOM_MIPS.
uint32_t r_bloom_mips_that_fit(int width, int height);

// Pieces of the above, exposed for the tests.
// The view distances where cascade i ends, for a sun reaching `shadow_distance`.
// `cascades` of them are used; the rest repeat the last.
void r_cascade_splits(float near_plane, float shadow_distance, uint32_t cascades, float out[R_SHADOW_CASCADES]);
// Fits one cascade: the slice of the camera's frustum between view distances
// `near_d` and `far_d`, seen from the sun. Returns the world-to-clip matrix and the
// size of a shadow texel in metres.
m4 r_cascade_fit(const r_camera *camera, float aspect, float near_d, float far_d, v3 sun_direction, uint32_t resolution, float *texel);

#endif
