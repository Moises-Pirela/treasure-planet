// What every shader of both backends shares: the blocks the facade fills (they
// match r_gpu_frame, r_gpu_lights, r_gpu_post, r_gpu_materials, r_push and
// r_post_push in src/render/backend.h) and a few helpers. Included first by every
// program. There is no #version line: the GL backend prepends it at runtime,
// glslc gets -std=450core. VULKAN is defined by the compiler for the SPIR-V
// build; a post step's program defines POST_STEP before including this.
// Nothing in here is a tuning value: every number that shapes the picture
// arrives in a block.

#define SHADOW_CASCADES 4
#define MAX_POINT_LIGHTS 32
#define MAX_MATERIALS 16
#define MATERIAL_VEC4S 12
#define MAX_AIRS 4
#define MAX_SKY_LAYERS 8
#define SKY_LAYER_VEC4S 6
#define SKY_STARS 0
#define SKY_NEBULA 1
#define SKY_BODY 2
#define FLAG_UNLIT 1u
#define FLAG_TWO_SIDED 4u
#define FLAG_UNSHADOWED 8u
#define PI 3.14159265

#ifdef VULKAN
#define UNIFORM_BLOCK(slot) layout(set = 0, binding = slot, std140) uniform
#define SAMPLER_AT(slot) layout(set = 0, binding = slot) uniform
#define INPUT_AT(i) layout(set = 1 + i, binding = 0) uniform
#define VERTEX_INDEX gl_VertexIndex
#else
#define UNIFORM_BLOCK(slot) layout(binding = slot, std140) uniform
#define SAMPLER_AT(slot) layout(binding = slot) uniform
#define INPUT_AT(i) layout(binding = 4 + i) uniform
#define VERTEX_INDEX gl_VertexID
#endif

UNIFORM_BLOCK(0) Frame {
    mat4 u_view_proj;
    mat4 u_shadow[SHADOW_CASCADES];   // world -> a cascade's clip space (orthographic, w = 1)
    vec4 u_camera_position;
    vec4 u_camera_right;              // w: tan of half the horizontal field of view
    vec4 u_camera_up;                 // w: tan of half the vertical one
    vec4 u_camera_forward;            // w: the near plane (depth is reversed: near / view depth)
    vec4 u_sun_dir;                   // xyz: towards the sun; w: cos of the disc's angular radius
    vec4 u_sun_color;                 // w: the glow around the disc
    vec4 u_ambient_sky;
    vec4 u_ambient_ground;
    vec4 u_sky_zenith;
    vec4 u_sky_horizon;
    vec4 u_sky_ground;
    vec4 u_cascade_splits;            // view distance at which each cascade ends
    vec4 u_cascade_texel;             // metres per shadow texel, per cascade
    vec4 u_shadow_params;             // x: 1 / resolution, y: normal offset in texels, z: depth bias, w: cascades in use
    vec4 u_sky;                       // x: sky layers, z: star cells per cube face, w: time
    vec4 u_ramp;                      // x: softness, y: wrap, z: flatness, w: steps
    vec4 u_shadow_tint;               // rgb; (1, 1, 1) for none; w: rim power
    vec4 u_rim;                       // rgb: colour, w: strength
    vec4 u_haze;                      // rgb: the haze about the camera (the cloud deck's), w: its density (0: none)
    vec4 u_air_zenith;                // rgb: the atmosphere's sky overhead, w: the immersion
    vec4 u_air_horizon;               // rgb: its sky at the horizon, w: its sun glow times the immersion
    vec4 u_air;                       // x: star fade times the immersion
    vec4 u_air_sphere[MAX_AIRS];      // the atmospheres' shells: xyz centre, w radius to the top of the air
    vec4 u_air_haze[MAX_AIRS];        // rgb: each one's haze, w: its density per metre of air
    vec4 u_cloud_sphere[MAX_AIRS];    // each one's cloud layer: xyz centre, w radius (0: none)
    vec4 u_cloud_layer[MAX_AIRS];     // x: cover, y: detail, z: sharpness, w: seed
    vec4 u_cloud_turn[MAX_AIRS];      // x: how far the layer has turned
    // The sky's layers, SKY_LAYER_VEC4S vec4 each, in the order they are drawn:
    //   0: xyz direction, w kind (SKY_*)
    //   1: xyz axis, w size (radians)
    //   2: rgb colour, w brightness
    //   3: rgb second colour, w density
    //   4: x detail, y how far it has turned (radians), z seed, w ring inner radius
    //   5: x ring outer radius
    vec4 u_sky_layers[MAX_SKY_LAYERS * SKY_LAYER_VEC4S];
    uvec4 u_counts;                   // x: point lights, y: sky layers, z: atmospheres
};

UNIFORM_BLOCK(1) Lights {
    vec4 u_light_position[MAX_POINT_LIGHTS];   // w: radius
    vec4 u_light_color[MAX_POINT_LIGHTS];
};

UNIFORM_BLOCK(3) Post {
    vec4 u_tonemap;                   // x: exposure, y: curve, z: white point (0: the curve's own), w: output gamma
    vec4 u_balance;                   // rgb: white balance, w: saturation
    vec4 u_grade;                     // x: contrast
    vec4 u_lift;
    vec4 u_gamma;                     // as the exponent
    vec4 u_gain;
    vec4 u_vignette;                  // x: strength, y: radius, z: softness
    vec4 u_bloom;                     // x: intensity, y: threshold, z: knee
    vec4 u_flare;                     // x: intensity, y: ghosts, z: dispersal, w: falloff
    vec4 u_flare_halo;                // x: width, y: intensity, z: chromatic, w: threshold
    vec4 u_aberration;                // x: strength
    vec4 u_fxaa;                      // x: threshold min, y: threshold, z: subpixel, w: iterations
    vec4 u_entry;                     // xy: where the camera is going, in uv; z: strength (0: none); w: blur length
    vec4 u_entry_glow;                // rgb: the plasma's colour times the glow; w: samples
    vec4 u_entry_streaks;             // x: streaks around the point of travel, y: time, z: streak length, w: streak speed
    vec4 u_haze_color;                // rgb; w: density per metre (0: none)
    vec4 u_haze_shape;                // x: base height, y: falloff above it, z: start distance, w: the sky's distance (0: infinite)
    vec4 u_haze_sun;                  // rgb: the glow towards the sun, w: its power
    vec4 u_outline;                   // x: strength (0: none), y: width in pixels, z: depth threshold, w: normal threshold (0: none)
    vec4 u_outline_color;             // rgb; w: fade distance (0: never)
};

// Every material's parameters, MATERIAL_VEC4S vec4 each, meaning what its shader
// says they mean (src/render/materials.h lays out the built-in ones).
UNIFORM_BLOCK(5) Materials {
    vec4 u_materials[MAX_MATERIALS * MATERIAL_VEC4S];
};

#ifndef POST_STEP
#ifdef VULKAN
layout(push_constant) uniform Draw {
    mat4 u_model;
    vec4 u_color;
    uint u_flags;
    uint u_material;
    uint u_lights;                    // a bit per point light that reaches this draw
    uint u_cascade;
};
#else
layout(location = 0) uniform mat4 u_model;
layout(location = 4) uniform vec4 u_color;
layout(location = 5) uniform uint u_flags;
layout(location = 6) uniform uint u_cascade;
layout(location = 7) uniform uint u_material;
layout(location = 8) uniform uint u_lights;
#endif
// The draw's material's parameters.
vec4 material_param(uint index) { return u_materials[u_material * MATERIAL_VEC4S + index]; }
#else
#ifdef VULKAN
layout(push_constant) uniform Step {
    vec4 u_texel;                     // xy: one texel of the first input, zw: one of the target, in uv
    vec4 u_params;                    // the step's
};
#else
layout(location = 0) uniform vec4 u_texel;
layout(location = 1) uniform vec4 u_params;
#endif
// Where a step's pixel is in clip space: the targets are stored y up in OpenGL
// and y down in Vulkan (post.vert flips the uv the same way).
vec2 post_ndc(vec2 uv) {
#ifdef VULKAN
    return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
#else
    return uv * 2.0 - 1.0;
#endif
}
#endif

// The way out of the eye through a point of the screen, unnormalised: one unit
// of view depth along it. The scene's depth buffer holds near / view depth (0:
// the sky, infinitely far).
vec3 view_ray(vec2 ndc) {
    return u_camera_forward.xyz + u_camera_right.xyz * (ndc.x * u_camera_right.w) + u_camera_up.xyz * (ndc.y * u_camera_up.w);
}
float view_depth_of(float stored) {
    return stored > 0.0 ? u_camera_forward.w / stored : 0.0;
}

// The scene target is floating point: lit pixels hold linear radiance with alpha
// 1, an unlit draw holds the linear colour it wants shown with alpha 0, and the
// composite passes those through with nothing but the display's gamma.
#define UNLIT_ALPHA 0.0

// How much of the way from `from` to `to` lies inside a sphere: the air a sight
// line crosses.
float air_path(vec3 from, vec3 to, vec4 sphere) {
    vec3 d = to - from;
    vec3 f = from - sphere.xyz;
    float a = dot(d, d);
    if (a < 1e-6) return 0.0;
    float b = 2.0 * dot(f, d);
    float c = dot(f, f) - sphere.w * sphere.w;
    float discriminant = b * b - 4.0 * a * c;
    if (discriminant <= 0.0) return 0.0;
    float root = sqrt(discriminant);
    float t0 = clamp((-b - root) / (2.0 * a), 0.0, 1.0);
    float t1 = clamp((-b + root) / (2.0 * a), 0.0, 1.0);
    return (t1 - t0) * sqrt(a);
}
