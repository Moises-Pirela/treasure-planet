// The renderer: meshes go in once, then each frame is handed over whole as plain
// data: a camera, an environment (the sun, the sky, the ambient light), a flat array
// of point lights and a flat array of draws. Nothing is retained between frames and
// nothing calls back; the caller never touches OpenGL or Vulkan, and the two
// backends draw the same frame from the same shader source
// (tools/compare_captures.py checks this).
//
// What a frame goes through (src/render/frame.cpp, headless and tested):
//   1. the camera becomes a view-projection and the sky's basis vectors;
//   2. the sun's shadow is fitted: R_SHADOW_CASCADES cascades along the view, each an
//      orthographic box around a slice of the frustum, snapped to its texel grid so
//      shadows do not shimmer when the camera moves;
//   3. every pass (each cascade, the main view) gets the list of draws whose bounding
//      sphere touches it;
//   4. the nearest R_MAX_POINT_LIGHTS lights are kept, the rest dropped;
//   5. the post-processing stack (r_post) becomes a flat list of steps over a fixed
//      set of render targets: the outline, the haze, bloom's mip chain, the lens
//      flare, the composite (tonemap and grading), anti-aliasing;
// and the backend then executes the result (src/render/backend.h) without a decision
// of its own: shadow passes, the lit pass, the sky, the post steps.
//
// Colours are linear. Lighting is in linear radiance into a floating-point scene
// target; the composite applies exposure, a tone curve, the grading and gamma. An
// unlit draw's colour is taken as the display colour it should come out as, and
// passes through the composite untouched, which is what overlays want.
#ifndef TP_RENDERER_H
#define TP_RENDERER_H

#include "../platform/platform.h"
#include "../tp_math.h"
#include <stddef.h>
#include <stdint.h>

enum r_backend_kind { R_BACKEND_VULKAN, R_BACKEND_OPENGL };

#define R_MAX_MESHES 256         // mesh handles are 1..R_MAX_MESHES-1
#define R_MAX_POINT_LIGHTS 32    // per frame, after selection; the shader's array
#define R_MAX_MATERIALS 16       // per frame; the shader's array
#define R_SHADOW_CASCADES 4

typedef uint32_t r_mesh;         // 0 is no mesh

struct r_vertex { float position[3], normal[3]; };

struct r_camera {
    v3 position;
    v3 forward;                  // unit; the frame's up is world +y
    v3 velocity;                 // metres per second, for what depends on motion (an atmosphere's entry)
    float fov_y;                 // radians
    float near_plane;            // metres; the far plane is at infinity
};

// The one directional light. `shadow_distance` is how far from the camera its
// shadows reach (0: no shadows); what is beyond is lit as if unshadowed.
struct r_sun {
    v3 direction;                // unit, towards the sun
    v3 color;                    // linear radiance; black is no sun
    float shadow_distance;
    uint32_t shadow_cascades;    // how many cascades share that distance, up to R_SHADOW_CASCADES (0: all)
    uint32_t shadow_resolution;  // texels across each cascade's map, R_SHADOW_RESOLUTION_MIN..MAX (0: the minimum)
};
#define R_SHADOW_RESOLUTION_MIN 256
#define R_SHADOW_RESOLUTION_MAX 4096

// A point light fades to nothing at `radius` (an inverse-square law, windowed).
// `color` is its radiance at one metre.
struct r_point_light {
    v3 position;
    float radius;
    v3 color;
};

// The sky is procedural: a gradient from the zenith through the horizon to the
// ground, the sun's disc with a glow around it, and layers over that, drawn in
// the order given: fields of stars, nebulae, and celestial bodies (moons, other
// worlds, a far sun) with a day side towards the sun. Every layer is made from
// its seed by integer hashing, so it is the same on every GPU.
enum r_sky_layer_kind {
    R_SKY_STARS,                 // points hashed over the sky: density, brightness, color (a tint; zero is natural), size (a scale; 0 is 1)
    R_SKY_NEBULA,                // glowing cloud: color and color2 mixed by noise, density (cover), detail, brightness;
                                 //   about `direction` within `size` radians (no direction: the whole sky); drift turns it about `axis`
    R_SKY_BODY,                  // a sphere of angular radius `size` at `direction`, lit by the sun: color and color2
                                 //   mixed by noise (detail 0: plain), brightness of its own light (a star, with a glow);
                                 //   rings in the plane across `axis` from ring_inner to ring_outer radii, in color2
};
struct r_sky_layer {
    int kind;                    // r_sky_layer_kind
    uint32_t seed;
    v3 direction;                // unit, where it is (a body's centre, a nebula's)
    v3 axis;                     // unit: a body's spin axis (its rings lie across it), what a nebula turns about
    float size;                  // radians
    v3 color, color2;            // linear; a body's are its albedo
    float brightness;
    float density;               // 0..1
    float detail;                // noise frequency over the sphere of directions
    float drift;                 // radians a second
    float ring_inner, ring_outer;   // in body radii (0: no rings)
};
#define R_MAX_SKY_LAYERS 8
struct r_sky {
    v3 zenith, horizon, ground;  // linear radiance
    float sun_disc;              // angular radius of the disc, radians (0: none)
    float sun_glow;              // strength of the halo around it (0: none)
    r_sky_layer layers[R_MAX_SKY_LAYERS];
    uint32_t layer_count;
};

// Haze over the whole picture, a post step by depth: everything fades into its
// colour by the air along the line of sight, thickest below `base` and thinning
// above it, lit towards the sun. Per world rather than per planet: a dungeon's
// damp, the dust between the hub's piers. (A planet's own air is its
// r_atmosphere, shaded in the lit pass.) Zero density is none.
struct r_haze {
    v3 color;                    // linear radiance: what things fade into
    float density;               // per metre, at and below the base
    float base;                  // world y below which it is full
    float falloff;               // per metre above the base: how fast it thins (0: the same at every height)
    float start;                 // metres from the eye before any
    float sky_distance;          // how far away the sky counts as (0: infinitely far, so uniform haze hides it whole)
    v3 sun_color;                // what it glows towards the sun (zero: none)
    float sun_power;             // how tight that glow is about the sun
};

// The painterly treatment of everything lit: a softened, wrapped, optionally
// stepped light ramp in place of plain Lambert; a tint over what the key light
// does not reach, so shadows go cool rather than black; and a rim of the sky's
// colour on every silhouette. Zero is plain Lambert, no tint, no rim.
struct r_shading {
    float ramp_softness;         // half-width of the terminator band, as a cosine
    float ramp_wrap;             // pushes the terminator into the dark side (0: none)
    float ramp_flatness;         // 0..1: from Lambert's gradation to the flat band; the painted-versus-drawn dial
    float ramp_steps;            // bands the ramp into this many steps (0: smooth)
    v3 shadow_tint;              // multiplied into the light where the sun does not reach (zero: none)
    v3 rim;                      // the rim light's colour, usually the sky's (zero: none)
    float rim_strength;
    float rim_power;             // how tight to the silhouette (higher: thinner)
};

struct r_environment {
    r_sun sun;
    r_sky sky;
    v3 ambient_sky, ambient_ground;   // hemisphere ambient: what faces up and what faces down
    r_shading shading;
    r_haze haze;
};

// ---------------------------------------------------------------------------
// The post-processing stack. Every field's zero is "off" or "neutral", so an
// all-zero r_post is a plain clamp-and-gamma copy of the scene, and every number
// that shapes the picture is here rather than in a shader.
// ---------------------------------------------------------------------------
enum r_tonemap_curve {
    R_TONEMAP_CLAMP,             // none: radiance clipped at 1
    R_TONEMAP_ACES,              // Narkowicz's fit of the ACES curve
    R_TONEMAP_REINHARD,          // extended Reinhard, with a white point
    R_TONEMAP_FILMIC,            // Hable's filmic curve, with a white point
};
struct r_tonemap {
    int curve;                   // r_tonemap_curve
    float exposure;              // 0 is 1
    float white_point;           // the radiance that comes out white (0: the curve's own)
};
struct r_grading {               // applied after the tone curve, on display-linear colour
    float temperature, tint;     // -1..1: warmer or cooler, magenta or green (1 is a quarter of a channel)
    float contrast;              // 0 neutral: the slope about middle grey is 1 + contrast
    float saturation;            // 0 neutral
    v3 lift, gamma, gain;        // 0 neutral: the shadows, the midtones, the highlights
    float vignette;              // 0 none: how dark the corners go
    float vignette_radius;       // where it starts, as a fraction of the half-diagonal
    float vignette_softness;     // over what distance it comes in
    float output_gamma;          // the display's, 0 is 2.2
};
struct r_bloom {
    float intensity;             // 0 off
    float threshold;             // radiance below which nothing blooms
    float knee;                  // the softness of that threshold
    float scatter;               // 0..1: how much of each coarser mip comes through; how far the light spreads
    float radius;                // the upsample filter's radius, in texels of the coarser mip (0 is 1)
    uint32_t mips;               // halvings of resolution (0: as many as fit, up to R_BLOOM_MIPS)
};
struct r_lens_flare {            // made from the picture itself: ghosts of what is bright, mirrored through the centre
    float intensity;             // 0 off
    float threshold;             // radiance below which nothing flares
    uint32_t ghosts;
    float dispersal;             // the spacing of the ghosts along the line through the centre
    float falloff;               // how fast a ghost fades towards the edge of the screen
    float halo_width;            // the ring's radius, as a fraction of the screen
    float halo_intensity;
    float chromatic;             // the ghosts' colour fringing, as a fraction of the screen
    uint32_t source_mip;         // the bloom mip the flare is made from (0: half resolution)
};
struct r_chromatic_aberration {
    float strength;              // 0 off: the colour channels' radial separation at the corners, as a fraction of the screen
};
enum r_aa_mode { R_AA_NONE, R_AA_FXAA };
struct r_entry {                 // the look of an atmosphere's entry (the strength comes from the atmosphere and the speed)
    float blur;                  // how far the picture streaks towards where the camera is going, as a fraction of the screen
    uint32_t samples;            // taps along that streak (0: no blur)
    float streaks;               // how many plasma streaks around the point of travel
    float streak_length;         // how many streak lengths fit from the point of travel to the screen's edge (lower: longer)
    float streak_speed;          // how fast they stream past, in streak lengths a second
    float glow;                  // the plasma's brightness at full strength
};
struct r_antialiasing {
    int mode;                    // r_aa_mode
    float edge_threshold_min;    // FXAA: contrast below which nothing is done
    float edge_threshold;        // FXAA: contrast relative to the local maximum below which nothing is done
    float subpixel;              // FXAA: 0..1, how much sub-pixel aliasing is blended away
    uint32_t iterations;         // FXAA: how far an edge is followed (up to R_FXAA_MAX_ITERATIONS)
};
// Lines along the edges: where the depth breaks (a silhouette) or the surface
// turns (a crease), from the depth buffer alone, drawn before the haze so they
// fade with distance like everything else. The drawn look.
struct r_outline {
    float strength;              // 0 off: how much of the line's colour goes on
    v3 color;                    // linear; black is ink
    float width;                 // pixels (0 is 1)
    float depth_threshold;       // a break in depth this large, as a fraction of the depth, is an edge (0 is 0.05)
    float normal_threshold;      // a turn this large, as 1 - the cosine between neighbours' normals, is an edge (0: creases not lined)
    float fade_distance;         // metres by which the lines have faded out (0: never)
};
struct r_post {
    r_tonemap tonemap;
    r_grading grading;
    r_outline outline;
    r_bloom bloom;
    r_lens_flare flare;
    r_chromatic_aberration aberration;
    r_antialiasing antialiasing;
    r_entry entry;
};

// A planet's air, as the camera meets it: a shell from the surface up to `height`,
// entered gradually on the way in. Inside it the sky becomes the planet's, what
// is far fades into its haze, the stars thin out and the sun's halo spreads; all
// of it by how deep the camera is, so a ship's descent crosses it seamlessly.
struct r_atmosphere {
    v3 centre;
    float radius;                // the planet's
    float height;                // how far above the surface the air reaches
    float falloff;               // how the immersion grows with depth: 1 linearly, higher later (0 is 1)
    v3 zenith, horizon;          // the sky from inside, overhead and at the horizon
    v3 haze;                     // what distant things fade towards, also the sky below the horizon
    float density;               // of the haze at the surface, per metre
    float star_fade;             // 0..1: how many of the stars the air hides
    float sun_glow;              // the sun's halo from inside, added to the sky's
    // A cloud deck: a band of thick haze at an altitude, passed through on the way down (0 altitude: none).
    float cloud_altitude;        // above the surface
    float cloud_thickness;
    float cloud_density;         // of the haze in the deck, per metre
    v3 cloud_color;              // what the haze turns into in the deck, and the cloud layer's colour
    // The cloud layer at the deck's altitude, which a client draws as a sphere with
    // the "clouds" material of these numbers, and whose shadow the lit pass casts
    // on whatever is under it, by the layer's own shape (no altitude: no layer).
    float cloud_cover;           // 0..1 of the sphere
    float cloud_detail;          // its noise frequency over the sphere
    float cloud_sharpness;       // how hard its edges are (the material's contrast)
    float cloud_translucency;    // how much of the sun comes through it from behind
    float cloud_drift;           // radians a second it turns about the planet's axis
    uint32_t cloud_seed;
    // Entering at speed: the picture streaks towards where the camera is going and
    // plasma glows at the edges, most in the middle of the shell and gone by the
    // surface (0 speed: never).
    float entry_speed;           // metres per second at which the entry is at full strength
    v3 entry_glow;               // the plasma's colour
};
// How strong an entry is at `at` moving at `velocity`: 0..1, the speed against
// the atmosphere's, times a band over the depth that peaks half way down.
float r_atmosphere_entry(const r_atmosphere *atmosphere, v3 at, v3 velocity);

// ---------------------------------------------------------------------------
// Materials: how a draw's surface is made. A material is a shader and the
// parameters that shader reads, R_MATERIAL_VEC4S vec4 of them meaning whatever
// the shader says (src/render/materials.h lays the built-in ones out). A shader
// is a file in src/render/shaders/materials, named by its stem: adding one is
// adding a file, and renderer_shader finds it. A frame carries a table of
// materials and a draw names one by index; index 0 is expected to be the plain
// surface (materials.h: r_material_surface).
// ---------------------------------------------------------------------------
#define R_MAX_SHADERS 16
#define R_MATERIAL_VEC4S 12
typedef uint32_t r_shader;       // 0 is none, which draws as the plain surface
struct renderer;

struct r_material {
    r_shader shader;
    float params[R_MATERIAL_VEC4S][4];
};

// The material shader of that name ("planet"), or 0 when there is none.
r_shader renderer_shader(const renderer *r, const char *name);

enum r_draw_flags {
    R_DRAW_UNLIT = 1u << 0,      // drawn in its own colour, casting no shadow
    R_DRAW_NO_SHADOW = 1u << 1,  // lit, but casts no shadow
    R_DRAW_TWO_SIDED = 1u << 2,  // neither face culled, lit from whichever faces the eye (a cloud layer seen from below)
    R_DRAW_UNSHADOWED = 1u << 3, // takes no shadow: the sun reaches it whatever stands in the way (a world seen from afar)
};

struct r_draw {
    m4 model;
    float color[4];              // rgb: linear albedo (display colour when unlit); a: unused
    uint32_t flags;              // r_draw_flags
    uint32_t material;           // index into the frame's materials
    r_mesh mesh;
};

struct r_frame {
    float time;                  // seconds, for what moves on its own (the entry's streaks)
    r_camera camera;
    r_environment environment;
    r_post post;
    const r_material *materials;
    uint32_t material_count;     // up to R_MAX_MATERIALS; none at all draws everything as the plain surface
    const r_atmosphere *atmospheres;
    uint32_t atmosphere_count;   // any number; the one the camera is deepest in counts
    const r_point_light *lights;
    uint32_t light_count;        // any number; the nearest R_MAX_POINT_LIGHTS are used
    const r_draw *draws;
    uint32_t draw_count;
    uint8_t *capture_rgba;       // when set, receives width * height * 4 bytes, top row first
};

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

// ---------------------------------------------------------------------------
// Models: a thing made of parts, each a mesh with a material at a place in the
// model's frame. renderer_model_draws lays a model out as draws, which is all a
// model is to a frame.
// ---------------------------------------------------------------------------
#define R_MAX_MODELS 64
#define R_MAX_MODEL_PARTS 64
typedef uint32_t r_model;        // 0 is no model

struct r_model_part {
    r_mesh mesh;
    uint32_t material;           // an index into the frame's materials
    m4 transform;                // in the model's frame
};

r_model renderer_model_create(renderer *r, const r_model_part *parts, uint32_t count);
void renderer_model_destroy(renderer *r, r_model model);
// The model's parts as draws at `transform`, with the colour and the flags for
// each; returns how many were written (no more than `capacity`).
uint32_t renderer_model_draws(const renderer *r, r_model model, const m4 *transform, const float color[4], uint32_t flags,
                              r_draw *out, uint32_t capacity);
// The same for parts in hand, without a renderer.
uint32_t r_model_draws(const r_model_part *parts, uint32_t count, const m4 *transform, const float color[4], uint32_t flags,
                       r_draw *out, uint32_t capacity);

// Meshes from Wavefront OBJ text: positions, normals (made flat per face when
// the file has none), faces of any size fanned into triangles. The parse is
// pure and allocates; free what it gives with r_obj_free.
struct r_obj_mesh {
    r_vertex *vertices;
    uint32_t vertex_count;
    uint32_t *indices;
    uint32_t index_count;
};
bool r_obj_parse(const char *text, size_t size, r_obj_mesh *out);
void r_obj_free(r_obj_mesh *mesh);
r_mesh renderer_mesh_load_obj(renderer *r, const char *path);   // 0 when the file cannot be read or parsed

// Where the GPU's time goes, pass by pass: what the timers measured for the most
// recent frame whose results are in. Milliseconds; `valid` is false until a frame
// has completed (or when the backend cannot time).
enum r_pass_kind {
    R_PASS_SHADOW,               // every cascade
    R_PASS_MAIN,                 // the lit pass
    R_PASS_SKY,
    R_PASS_DOWNSAMPLE,           // the post steps, by program
    R_PASS_UPSAMPLE,
    R_PASS_FLARE,
    R_PASS_COMPOSITE,
    R_PASS_FXAA,
    R_PASS_OUTLINE,
    R_PASS_HAZE,
    R_PASS_COUNT,
};
struct r_timings {
    float ms[R_PASS_COUNT];
    float total_ms;              // from the first pass to the last
    bool valid;
};
const char *r_pass_name(int pass);       // of an r_pass_kind
r_timings renderer_timings(const renderer *r);

#endif
