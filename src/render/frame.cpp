// Preparing a frame: from the caller's r_frame to the r_packet a backend executes.
// Pure arithmetic over plain arrays, no API and no window, so the tests run it as
// it is. Everything a backend would otherwise have to decide is decided here, once,
// the same way for both.
#include "backend.h"
#include <stdlib.h>
#include <string.h>

// How far behind a cascade's slice a caster may stand and still throw its shadow
// into it: a mast, a wall, a ship passing overhead. Depth is a 32-bit float, so
// the range costs nothing.
#define R_SHADOW_CASTER_REACH 400.0f
#define R_SHADOW_NORMAL_OFFSET 1.5f      // texels
#define R_SHADOW_DEPTH_BIAS 0.00002f     // in depth units: a centimetre or two of the caster reach
#define R_STAR_CELLS 48.0f               // per cube face
#define R_SPLIT_BLEND 0.7f               // 0: uniform splits, 1: logarithmic
#define R_BLOOM_SMALLEST_MIP 4           // pixels: the coarsest mip is at least this across

static void put3(float out[4], v3 v, float w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

static float or_default(float value, float fallback) { return value > 0.0f ? value : fallback; }

void r_scratch_free(r_scratch *scratch) {
    free(scratch->indices);
    memset(scratch, 0, sizeof(*scratch));
}

// ---------------------------------------------------------------------------
// Shadow cascades
// ---------------------------------------------------------------------------
void r_cascade_splits(float near_plane, float shadow_distance, uint32_t cascades, float out[R_SHADOW_CASCADES]) {
    // The practical split scheme: between uniform (good far away) and logarithmic
    // (good up close), measured from a metre rather than the near plane, which
    // would crowd every cascade into the first few metres.
    float from = near_plane > 1.0f ? near_plane : 1.0f;
    if (shadow_distance < from + 1.0f) shadow_distance = from + 1.0f;
    if (cascades < 1) cascades = 1;
    if (cascades > R_SHADOW_CASCADES) cascades = R_SHADOW_CASCADES;
    for (uint32_t i = 0; i < R_SHADOW_CASCADES; ++i) {
        float share = (float)(i < cascades ? i + 1 : cascades) / (float)cascades;
        float uniform = from + (shadow_distance - from) * share;
        float logarithmic = from * powf(shadow_distance / from, share);
        out[i] = uniform + (logarithmic - uniform) * R_SPLIT_BLEND;
    }
}

static v3 sun_up_hint(v3 light_forward) {
    // Any fixed vector not parallel to the light: fixed so the cascade's frame, and
    // with it the texel grid, does not turn as the camera does.
    return fabsf(light_forward.y) < 0.9f ? v3_make(0, 1, 0) : v3_make(0, 0, 1);
}

m4 r_cascade_fit(const r_camera *camera, float aspect, float near_d, float far_d, v3 sun_direction, uint32_t resolution, float *texel) {
    // The slice of the frustum between near_d and far_d, and its bounding sphere.
    // A sphere rather than a tight box: it does not change size as the camera
    // turns, so the shadow texels stay the same size, and with the snapping below
    // the same texels.
    float tan_y = tanf(camera->fov_y * 0.5f), tan_x = tan_y * aspect;
    float k2 = tan_x * tan_x + tan_y * tan_y;
    float t = (near_d + far_d) * (1.0f + k2) * 0.5f;    // the centre on the axis that makes both ends equidistant
    if (t > far_d) t = far_d;
    float to_far = sqrtf((far_d - t) * (far_d - t) + far_d * far_d * k2);
    float to_near = sqrtf((t - near_d) * (t - near_d) + near_d * near_d * k2);
    float radius = to_far > to_near ? to_far : to_near;
    v3 centre = v3_add(camera->position, v3_scale(camera->forward, t));

    // The sun's frame. The centre is snapped to the texel grid in that frame, so a
    // camera moving by less than a texel leaves the shadow map where it was.
    v3 light_forward = v3_scale(sun_direction, -1.0f);
    v3 up = sun_up_hint(light_forward);
    m4 frame = m4_look_to_up(v3_make(0, 0, 0), light_forward, up);
    *texel = 2.0f * radius / (float)resolution;
    v3 in_frame = m4_transform_point(&frame, centre);
    in_frame.x = floorf(in_frame.x / *texel) * *texel;
    in_frame.y = floorf(in_frame.y / *texel) * *texel;
    // Back to the world: the frame is a rotation, so its inverse is its transpose.
    v3 snapped = v3_make(frame.m[0] * in_frame.x + frame.m[1] * in_frame.y + frame.m[2] * in_frame.z,
                         frame.m[4] * in_frame.x + frame.m[5] * in_frame.y + frame.m[6] * in_frame.z,
                         frame.m[8] * in_frame.x + frame.m[9] * in_frame.y + frame.m[10] * in_frame.z);

    v3 eye = v3_sub(snapped, v3_scale(light_forward, radius + R_SHADOW_CASTER_REACH));
    m4 view = m4_look_to_up(eye, light_forward, up);
    m4 projection = m4_ortho(radius, radius, 2.0f * radius + R_SHADOW_CASTER_REACH);
    return m4_mul(projection, view);
}

// ---------------------------------------------------------------------------
// Culling
// ---------------------------------------------------------------------------
struct bounding_sphere { v3 centre; float radius; };

static bounding_sphere draw_bounds(const r_draw *draw, const float *mesh_radius) {
    bounding_sphere s;
    const float *m = draw->model.m;
    s.centre = v3_make(m[12], m[13], m[14]);
    float sx = v3_length(v3_make(m[0], m[1], m[2]));
    float sy = v3_length(v3_make(m[4], m[5], m[6]));
    float sz = v3_length(v3_make(m[8], m[9], m[10]));
    float scale = sx > sy ? sx : sy;
    if (sz > scale) scale = sz;
    float radius = mesh_radius && draw->mesh < R_MAX_MESHES ? mesh_radius[draw->mesh] : 0.0f;
    s.radius = radius > 0.0f ? radius * scale : -1.0f;    // unknown: never culled
    return s;
}

struct plane { v3 normal; float d; };    // dot(normal, p) + d >= 0 is inside

static plane plane_normalized(float a, float b, float c, float d) {
    float length = sqrtf(a * a + b * b + c * c);
    plane p = {v3_make(a / length, b / length, c / length), d / length};
    return p;
}

// The view frustum's planes from its matrix (Gribb and Hartmann). The far plane is
// at infinity, so there are five.
static void frustum_planes(const m4 *view_proj, plane out[5]) {
    const float *m = view_proj->m;
    // Row i of the matrix: m[i], m[4 + i], m[8 + i], m[12 + i].
#define ROW(i, k) m[(k) * 4 + (i)]
    out[0] = plane_normalized(ROW(3, 0) + ROW(0, 0), ROW(3, 1) + ROW(0, 1), ROW(3, 2) + ROW(0, 2), ROW(3, 3) + ROW(0, 3));   // left
    out[1] = plane_normalized(ROW(3, 0) - ROW(0, 0), ROW(3, 1) - ROW(0, 1), ROW(3, 2) - ROW(0, 2), ROW(3, 3) - ROW(0, 3));   // right
    out[2] = plane_normalized(ROW(3, 0) + ROW(1, 0), ROW(3, 1) + ROW(1, 1), ROW(3, 2) + ROW(1, 2), ROW(3, 3) + ROW(1, 3));   // bottom
    out[3] = plane_normalized(ROW(3, 0) - ROW(1, 0), ROW(3, 1) - ROW(1, 1), ROW(3, 2) - ROW(1, 2), ROW(3, 3) - ROW(1, 3));   // top
    out[4] = plane_normalized(ROW(3, 0) - ROW(2, 0), ROW(3, 1) - ROW(2, 1), ROW(3, 2) - ROW(2, 2), ROW(3, 3) - ROW(2, 3));   // near: z <= w
#undef ROW
}

static bool sphere_in_frustum(const plane *planes, int count, bounding_sphere s) {
    if (s.radius < 0.0f) return true;
    for (int i = 0; i < count; ++i)
        if (v3_dot(planes[i].normal, s.centre) + planes[i].d < -s.radius) return false;
    return true;
}

// A cascade's box in its own clip space: x and y in [-1, 1], depth in [0, 1].
static bool sphere_in_cascade(const m4 *shadow, float radius_metres, float depth_metres, bounding_sphere s) {
    if (s.radius < 0.0f) return true;
    v3 c = m4_transform_point(shadow, s.centre);
    float rxy = s.radius / radius_metres, rz = s.radius / depth_metres;
    return c.x + rxy >= -1.0f && c.x - rxy <= 1.0f && c.y + rxy >= -1.0f && c.y - rxy <= 1.0f
        && c.z + rz >= 0.0f && c.z - rz <= 1.0f;
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------
// The nearest lights win a slot, nearness being the distance at which the light's
// reach ends, so a big lamp far away still beats a candle further still. A light
// whose reach is outside the view costs every fragment a loop round for nothing,
// so those are dropped first.
static uint32_t select_lights(const r_frame *frame, const plane *frustum, r_gpu_lights *out) {
    float score[R_MAX_POINT_LIGHTS];
    uint32_t chosen[R_MAX_POINT_LIGHTS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < frame->light_count; ++i) {
        const r_point_light *light = &frame->lights[i];
        if (light->radius <= 0.0f || (light->color.x <= 0.0f && light->color.y <= 0.0f && light->color.z <= 0.0f)) continue;
        bounding_sphere reach = {light->position, light->radius};
        if (!sphere_in_frustum(frustum, 5, reach)) continue;
        float s = v3_length(v3_sub(light->position, frame->camera.position)) - light->radius;
        if (count == R_MAX_POINT_LIGHTS && s >= score[count - 1]) continue;
        // Insert in order of score; the list is at most R_MAX_POINT_LIGHTS long.
        uint32_t at = count < R_MAX_POINT_LIGHTS ? count++ : count - 1;
        while (at > 0 && score[at - 1] > s) {
            score[at] = score[at - 1];
            chosen[at] = chosen[at - 1];
            --at;
        }
        score[at] = s;
        chosen[at] = i;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const r_point_light *light = &frame->lights[chosen[i]];
        put3(out->position[i], light->position, light->radius);
        put3(out->color[i], light->color, 0.0f);
    }
    return count;
}

// ---------------------------------------------------------------------------
// The post-processing stack
// ---------------------------------------------------------------------------
uint32_t r_bloom_mips_that_fit(int width, int height) {
    int smaller = width < height ? width : height;
    uint32_t mips = 0;
    while (mips < R_BLOOM_MIPS && (smaller >> (mips + 1)) >= R_BLOOM_SMALLEST_MIP) ++mips;
    return mips;
}


// White balance as three multipliers: temperature warms (red up, blue down) or
// cools, tint goes magenta (green down) or green; then scaled so the luminance
// of white stays put, so the exposure does not change with the balance.
static void white_balance(float temperature, float tint, float out[4]) {
    float r = 1.0f + 0.25f * temperature + 0.125f * tint;
    float g = 1.0f - 0.25f * tint;
    float b = 1.0f - 0.25f * temperature + 0.125f * tint;
    float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    out[0] = r / luminance;
    out[1] = g / luminance;
    out[2] = b / luminance;
}

static r_post_step *add_step(r_packet *out, int program, int target, int input0, int input1, int input2) {
    r_post_step *step = &out->steps[out->step_count++];
    memset(step, 0, sizeof(*step));
    step->program = (uint8_t)program;
    step->target = (uint8_t)target;
    step->inputs[0] = (uint8_t)input0;
    step->inputs[1] = (uint8_t)input1;
    step->inputs[2] = (uint8_t)input2;
    return step;
}

static void prepare_post(const r_post *post, const r_haze *haze, int width, int height, r_packet *out) {
    r_gpu_post *g = &out->post;
    uint32_t fit = r_bloom_mips_that_fit(width, height);

    // The full-size steps before the bloom: the outline, then the haze over it,
    // ping-ponging between the scene target and its twin; what follows reads
    // whichever holds the result.
    bool outline = post->outline.strength > 0.0f;
    bool hazed = haze->density > 0.0f;
    g->outline[0] = outline ? post->outline.strength : 0.0f;
    g->outline[1] = or_default(post->outline.width, 1.0f);
    g->outline[2] = or_default(post->outline.depth_threshold, 0.05f);
    g->outline[3] = post->outline.normal_threshold;
    put3(g->outline_color, post->outline.color, post->outline.fade_distance);
    put3(g->haze_color, haze->color, hazed ? haze->density : 0.0f);
    g->haze_shape[0] = haze->base;
    g->haze_shape[1] = haze->falloff;
    g->haze_shape[2] = haze->start;
    g->haze_shape[3] = haze->sky_distance;
    put3(g->haze_sun, haze->sun_color, haze->sun_power);

    // What is on, and how far the bloom chain goes: the flare is made from one of
    // the bloom's downsamples, so it needs the chain down to its mip even with the
    // bloom itself off.
    bool bloom = post->bloom.intensity > 0.0f && fit > 0;
    uint32_t bloom_mips = 0;
    if (bloom) {
        bloom_mips = post->bloom.mips ? post->bloom.mips : fit;
        if (bloom_mips > fit) bloom_mips = fit;
    }
    bool flare = post->flare.intensity > 0.0f && fit > 0;
    uint32_t flare_mip = flare ? (post->flare.source_mip < fit ? post->flare.source_mip : fit - 1) : 0;
    uint32_t down_mips = bloom_mips;
    if (flare && flare_mip + 1 > down_mips) down_mips = flare_mip + 1;
    bool fxaa = post->antialiasing.mode == R_AA_FXAA;

    // The block.
    g->tonemap[0] = or_default(post->tonemap.exposure, 1.0f);
    g->tonemap[1] = (float)post->tonemap.curve;
    g->tonemap[2] = post->tonemap.white_point;
    g->tonemap[3] = or_default(post->grading.output_gamma, 2.2f);
    white_balance(post->grading.temperature, post->grading.tint, g->balance);
    g->balance[3] = 1.0f + post->grading.saturation;
    g->grade[0] = 1.0f + post->grading.contrast;
    put3(g->lift, post->grading.lift, 0.0f);
    put3(g->gamma, v3_make(1.0f / (1.0f + post->grading.gamma.x), 1.0f / (1.0f + post->grading.gamma.y), 1.0f / (1.0f + post->grading.gamma.z)), 0.0f);
    put3(g->gain, v3_add(v3_make(1, 1, 1), post->grading.gain), 0.0f);
    g->vignette[0] = post->grading.vignette;
    g->vignette[1] = post->grading.vignette_radius;
    g->vignette[2] = post->grading.vignette_softness;
    g->bloom[0] = bloom ? post->bloom.intensity : 0.0f;
    g->bloom[1] = post->bloom.threshold;
    g->bloom[2] = post->bloom.knee;
    g->flare[0] = flare ? post->flare.intensity : 0.0f;
    g->flare[1] = (float)post->flare.ghosts;
    g->flare[2] = post->flare.dispersal;
    g->flare[3] = post->flare.falloff;
    g->flare_halo[0] = post->flare.halo_width;
    g->flare_halo[1] = post->flare.halo_intensity;
    g->flare_halo[2] = post->flare.chromatic;
    g->flare_halo[3] = post->flare.threshold;
    g->aberration[0] = post->aberration.strength;
    g->fxaa[0] = post->antialiasing.edge_threshold_min;
    g->fxaa[1] = post->antialiasing.edge_threshold;
    g->fxaa[2] = post->antialiasing.subpixel;
    uint32_t iterations = post->antialiasing.iterations < R_FXAA_MAX_ITERATIONS ? post->antialiasing.iterations : R_FXAA_MAX_ITERATIONS;
    g->fxaa[3] = (float)iterations;

    // The steps. The outline and the haze at full size, down the bloom chain, back
    // up it, the flare, the composite, the anti-aliasing; nothing that is off
    // costs a step.
    out->step_count = 0;
    int scene = R_TARGET_SCENE;
    if (outline) {
        add_step(out, R_POST_OUTLINE, R_TARGET_SCENE_B, scene, R_TARGET_DEPTH, R_TARGET_DEPTH);
        scene = R_TARGET_SCENE_B;
    }
    if (hazed) {
        int other = scene == R_TARGET_SCENE ? R_TARGET_SCENE_B : R_TARGET_SCENE;
        add_step(out, R_POST_HAZE, other, scene, R_TARGET_DEPTH, R_TARGET_DEPTH);
        scene = other;
    }
    for (uint32_t i = 0; i < down_mips; ++i) {
        int source = i == 0 ? scene : R_TARGET_BLOOM_DOWN + (int)i - 1;
        r_post_step *step = add_step(out, R_POST_DOWNSAMPLE, R_TARGET_BLOOM_DOWN + (int)i, source, source, source);
        step->params[0] = i == 0 ? 1.0f : 0.0f;
    }
    int bloom_result = scene;
    if (bloom) {
        bloom_result = R_TARGET_BLOOM_DOWN;
        if (bloom_mips > 1) {
            for (int i = (int)bloom_mips - 2; i >= 0; --i) {
                int coarser = i == (int)bloom_mips - 2 ? R_TARGET_BLOOM_DOWN + i + 1 : R_TARGET_BLOOM_UP + i + 1;
                r_post_step *step = add_step(out, R_POST_UPSAMPLE, R_TARGET_BLOOM_UP + i, coarser, R_TARGET_BLOOM_DOWN + i, coarser);
                step->params[0] = or_default(post->bloom.radius, 1.0f);
                step->params[1] = post->bloom.scatter;
            }
            bloom_result = R_TARGET_BLOOM_UP;
        }
    }
    if (flare) {
        int source = R_TARGET_BLOOM_DOWN + (int)flare_mip;
        add_step(out, R_POST_FLARE, R_TARGET_FLARE, source, source, source);
    }
    r_post_step *composite = add_step(out, R_POST_COMPOSITE, fxaa ? R_TARGET_LDR : R_TARGET_WINDOW, scene,
                                      bloom ? bloom_result : scene, flare ? R_TARGET_FLARE : scene);
    composite->params[0] = fxaa ? 1.0f : 0.0f;
    composite->params[1] = bloom ? 1.0f : 0.0f;
    composite->params[2] = flare ? 1.0f : 0.0f;
    if (fxaa) add_step(out, R_POST_FXAA, R_TARGET_WINDOW, R_TARGET_LDR, R_TARGET_LDR, R_TARGET_LDR);
}

// ---------------------------------------------------------------------------
// The sky's layers
// ---------------------------------------------------------------------------
// Laid out for the shader as common.glsl's u_sky_layers says; a layer of no kind
// the shader knows is left out. The seed goes as a float, so it keeps 24 bits.
static uint32_t prepare_sky_layers(const r_sky *sky, float time, r_gpu_frame *gpu) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < sky->layer_count && i < R_MAX_SKY_LAYERS && count < R_MAX_SKY_LAYERS; ++i) {
        const r_sky_layer *layer = &sky->layers[i];
        if (layer->kind < R_SKY_STARS || layer->kind > R_SKY_BODY) continue;
        float (*g)[4] = gpu->sky_layers[count++];
        bool aimed = layer->direction.x != 0.0f || layer->direction.y != 0.0f || layer->direction.z != 0.0f;
        bool spun = layer->axis.x != 0.0f || layer->axis.y != 0.0f || layer->axis.z != 0.0f;
        put3(g[0], aimed ? v3_normalize(layer->direction) : v3_make(0, 0, 0), (float)layer->kind);
        put3(g[1], spun ? v3_normalize(layer->axis) : v3_make(0, 1, 0), layer->size);
        put3(g[2], layer->color, layer->brightness);
        put3(g[3], layer->color2, layer->density);
        g[4][0] = layer->detail;
        g[4][1] = layer->drift * time;
        g[4][2] = (float)(layer->seed & 0xffffffu);
        g[4][3] = layer->ring_inner;
        g[5][0] = layer->ring_outer;
        g[5][1] = g[5][2] = g[5][3] = 0.0f;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Atmospheres
// ---------------------------------------------------------------------------
static float altitude_in(const r_atmosphere *atmosphere, v3 at) {
    return v3_length(v3_sub(at, atmosphere->centre)) - atmosphere->radius;
}

float r_atmosphere_immersion(const r_atmosphere *atmosphere, v3 at) {
    if (atmosphere->height <= 0.0f) return 0.0f;
    float depth = 1.0f - altitude_in(atmosphere, at) / atmosphere->height;
    if (depth <= 0.0f) return 0.0f;
    if (depth >= 1.0f) return 1.0f;
    return powf(depth, or_default(atmosphere->falloff, 1.0f));
}

float r_atmosphere_entry(const r_atmosphere *atmosphere, v3 at, v3 velocity) {
    if (atmosphere->entry_speed <= 0.0f) return 0.0f;
    float depth = r_atmosphere_immersion(atmosphere, at);
    float band = 4.0f * depth * (1.0f - depth);        // 0 at the shell's edge and at the surface, 1 half way
    float speed = v3_length(velocity) / atmosphere->entry_speed;
    if (speed > 1.0f) speed = 1.0f;
    return band * speed;
}

// How much of the cloud deck the camera is in: 0 outside, 1 in the middle of it.
static float cloud_band(const r_atmosphere *atmosphere, v3 at) {
    if (atmosphere->cloud_altitude <= 0.0f || atmosphere->cloud_thickness <= 0.0f) return 0.0f;
    float off = fabsf(altitude_in(atmosphere, at) - atmosphere->cloud_altitude) / (atmosphere->cloud_thickness * 0.5f);
    if (off >= 1.0f) return 0.0f;
    return 1.0f - off * off;
}

// Every atmosphere's shell goes to the block (the nearest R_MAX_AIRS), for the
// haze on what is seen through them from anywhere: from orbit a world is veiled
// by the whole column of its air, and clears as the camera comes down through it.
// The one the camera is deepest in sets the sky by that depth, and the cloud deck
// where the camera is in it hazes everything; the entry goes to the post block as
// a point on the screen to streak towards. None leaves it all at zero: open space.
static void prepare_atmosphere(const r_frame *frame, r_gpu_frame *gpu, r_gpu_post *post) {
    const r_atmosphere *inside = NULL;
    float immersion = 0.0f;
    uint32_t airs = 0;
    float air_altitude[R_MAX_AIRS];
    for (uint32_t i = 0; i < frame->atmosphere_count; ++i) {
        const r_atmosphere *a = &frame->atmospheres[i];
        if (a->height <= 0.0f) continue;
        float depth = r_atmosphere_immersion(a, frame->camera.position);
        if (depth > immersion || (depth == immersion && !inside)) {
            immersion = depth;
            inside = a;
        }
        // Into the shells, the nearest first, the farthest dropped past R_MAX_AIRS.
        float altitude = altitude_in(a, frame->camera.position);
        uint32_t at = airs < R_MAX_AIRS ? airs++ : airs - 1;
        if (at == R_MAX_AIRS - 1 && airs == R_MAX_AIRS && altitude >= air_altitude[at]) continue;
        while (at > 0 && air_altitude[at - 1] > altitude) {
            air_altitude[at] = air_altitude[at - 1];
            memcpy(gpu->air_sphere[at], gpu->air_sphere[at - 1], sizeof(gpu->air_sphere[at]));
            memcpy(gpu->air_haze[at], gpu->air_haze[at - 1], sizeof(gpu->air_haze[at]));
            memcpy(gpu->cloud_sphere[at], gpu->cloud_sphere[at - 1], sizeof(gpu->cloud_sphere[at]));
            memcpy(gpu->cloud_layer[at], gpu->cloud_layer[at - 1], sizeof(gpu->cloud_layer[at]));
            memcpy(gpu->cloud_turn[at], gpu->cloud_turn[at - 1], sizeof(gpu->cloud_turn[at]));
            --at;
        }
        air_altitude[at] = altitude;
        put3(gpu->air_sphere[at], a->centre, a->radius + a->height);
        put3(gpu->air_haze[at], a->haze, a->density);
        bool layer = a->cloud_altitude > 0.0f && a->cloud_cover > 0.0f;
        put3(gpu->cloud_sphere[at], a->centre, layer ? a->radius + a->cloud_altitude : 0.0f);
        gpu->cloud_layer[at][0] = a->cloud_cover;
        gpu->cloud_layer[at][1] = a->cloud_detail;
        gpu->cloud_layer[at][2] = a->cloud_sharpness;
        gpu->cloud_layer[at][3] = (float)a->cloud_seed;
        gpu->cloud_turn[at][0] = a->cloud_drift * frame->time;
    }
    gpu->counts[2] = airs;
    if (!inside) return;
    float deck = cloud_band(inside, frame->camera.position);
    put3(gpu->haze, inside->cloud_color, inside->cloud_density * deck);
    put3(gpu->air_zenith, v3_lerp(inside->zenith, inside->cloud_color, deck), immersion);
    put3(gpu->air_horizon, v3_lerp(inside->horizon, inside->cloud_color, deck), inside->sun_glow * immersion);
    gpu->air[0] = inside->star_fade * immersion;

    float entry = r_atmosphere_entry(inside, frame->camera.position, frame->camera.velocity);
    if (entry <= 0.0f) return;
    // Where the camera is going, on the screen: the velocity as a direction through
    // the projection. Behind the camera it lands outside the screen, which still
    // streaks away from it.
    v3 v = v3_normalize(frame->camera.velocity);
    const float *m = gpu->view_proj.m;
    float cx = m[0] * v.x + m[4] * v.y + m[8] * v.z;
    float cy = m[1] * v.x + m[5] * v.y + m[9] * v.z;
    float cw = m[3] * v.x + m[7] * v.y + m[11] * v.z;
    if (fabsf(cw) < 1e-4f) cw = cw < 0.0f ? -1e-4f : 1e-4f;
    post->entry[0] = (cx / cw) * 0.5f + 0.5f;
    post->entry[1] = (cy / cw) * 0.5f + 0.5f;
    post->entry[2] = entry;
    post->entry[3] = frame->post.entry.blur;
    put3(post->entry_glow, v3_scale(inside->entry_glow, frame->post.entry.glow), (float)frame->post.entry.samples);
    post->entry_streaks[0] = frame->post.entry.streaks;
    post->entry_streaks[1] = frame->time;
    post->entry_streaks[2] = frame->post.entry.streak_length;
    post->entry_streaks[3] = frame->post.entry.streak_speed;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------
// The materials' parameters as given, and each one's shader as the backend
// knows it: a material of no shader, or past the table, draws as the surface.
static void prepare_materials(const r_frame *frame, const r_shader_set *shaders, r_packet *out) {
    memset(&out->materials, 0, sizeof(out->materials));
    uint32_t surface = shaders ? shaders->surface : 0;
    uint32_t count = frame->material_count < R_MAX_MATERIALS ? frame->material_count : R_MAX_MATERIALS;
    for (uint32_t i = 0; i < R_MAX_MATERIALS; ++i) {
        uint32_t shader = surface;
        if (i < count) {
            const r_material *m = &frame->materials[i];
            memcpy(out->materials.items[i], m->params, sizeof(m->params));
            if (shaders && m->shader >= 1 && m->shader <= shaders->count) shader = m->shader - 1;
        }
        out->material_shader[i] = (uint8_t)shader;
        out->material_cutout[i] = shaders && shader < shaders->count && shaders->items[shader].cutout ? 1 : 0;
    }
}

// Orders a pass's draws by their material's shader, so a backend changes
// program as seldom as can be; a counting sort, stable, through `temp`.
static void sort_by_shader(const r_frame *frame, const r_packet *packet, uint32_t *indices, uint32_t count, uint32_t *temp) {
    uint32_t starts[R_MAX_SHADERS + 1] = {};
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t material = frame->draws[indices[i]].material;
        ++starts[packet->material_shader[material < R_MAX_MATERIALS ? material : 0] + 1];
    }
    for (uint32_t s = 1; s <= R_MAX_SHADERS; ++s) starts[s] += starts[s - 1];
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t material = frame->draws[indices[i]].material;
        temp[starts[packet->material_shader[material < R_MAX_MATERIALS ? material : 0]]++] = indices[i];
    }
    memcpy(indices, temp, count * sizeof(uint32_t));
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------
uint32_t r_model_draws(const r_model_part *parts, uint32_t count, const m4 *transform, const float color[4], uint32_t flags,
                       r_draw *out, uint32_t capacity) {
    uint32_t n = count < capacity ? count : capacity;
    for (uint32_t i = 0; i < n; ++i) {
        r_draw *d = &out[i];
        memset(d, 0, sizeof(*d));
        d->model = m4_mul(*transform, parts[i].transform);
        memcpy(d->color, color, sizeof(d->color));
        d->flags = flags;
        d->material = parts[i].material;
        d->mesh = parts[i].mesh;
    }
    return n;
}

// ---------------------------------------------------------------------------
// The whole frame
// ---------------------------------------------------------------------------
static uint32_t *reserve(r_scratch *scratch, size_t count) {
    if (scratch->capacity < count) {
        scratch->capacity = count + count / 2 + 64;
        scratch->indices = (uint32_t *)realloc(scratch->indices, scratch->capacity * sizeof(uint32_t));
    }
    return scratch->indices;
}

// Which of the selected lights reach a draw: a bit per light whose sphere of reach
// meets the draw's bounding sphere. A draw of unknown size gets them all.
static uint32_t light_mask(const r_gpu_lights *lights, uint32_t light_count, bounding_sphere bounds) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < light_count; ++i) {
        if (bounds.radius < 0.0f) {
            mask |= 1u << i;
            continue;
        }
        v3 at = v3_make(lights->position[i][0], lights->position[i][1], lights->position[i][2]);
        float reach = lights->position[i][3] + bounds.radius;
        v3 d = v3_sub(at, bounds.centre);
        if (v3_dot(d, d) <= reach * reach) mask |= 1u << i;
    }
    return mask;
}

void r_prepare(const r_frame *frame, const float *mesh_radius, const r_shader_set *shaders, int width, int height,
               r_scratch *scratch, r_packet *out) {
    memset(out, 0, sizeof(*out));
    const r_camera *camera = &frame->camera;
    const r_environment *env = &frame->environment;
    r_gpu_frame *gpu = &out->frame;
    float aspect = height > 0 ? (float)width / (float)height : 1.0f;

    // The camera.
    v3 forward = v3_normalize(camera->forward);
    v3 right = v3_normalize(v3_cross(forward, v3_make(0, 1, 0)));
    v3 up = v3_cross(right, forward);
    m4 view = m4_look_to(camera->position, forward);
    m4 projection = m4_perspective(camera->fov_y, aspect, camera->near_plane);
    gpu->view_proj = m4_mul(projection, view);
    float tan_y = tanf(camera->fov_y * 0.5f);
    put3(gpu->camera_position, camera->position, 0.0f);
    put3(gpu->camera_right, right, tan_y * aspect);
    put3(gpu->camera_up, up, tan_y);
    put3(gpu->camera_forward, forward, camera->near_plane);

    // The environment.
    v3 sun_dir = v3_normalize(env->sun.direction);
    bool has_sun = env->sun.color.x > 0.0f || env->sun.color.y > 0.0f || env->sun.color.z > 0.0f;
    put3(gpu->sun_dir, sun_dir, cosf(env->sky.sun_disc));
    put3(gpu->sun_color, has_sun ? env->sun.color : v3_make(0, 0, 0), env->sky.sun_glow);
    put3(gpu->ambient_sky, env->ambient_sky, 0.0f);
    put3(gpu->ambient_ground, env->ambient_ground, 0.0f);
    put3(gpu->sky_zenith, env->sky.zenith, 0.0f);
    put3(gpu->sky_horizon, env->sky.horizon, 0.0f);
    put3(gpu->sky_ground, env->sky.ground, 0.0f);
    gpu->sky[2] = R_STAR_CELLS;
    gpu->sky[3] = frame->time;
    gpu->counts[1] = prepare_sky_layers(&env->sky, frame->time, gpu);
    gpu->sky[0] = (float)gpu->counts[1];
    const r_shading *shading = &env->shading;
    gpu->ramp[0] = shading->ramp_softness;
    gpu->ramp[1] = shading->ramp_wrap;
    gpu->ramp[2] = shading->ramp_flatness;
    gpu->ramp[3] = shading->ramp_steps;
    bool tinted = shading->shadow_tint.x > 0.0f || shading->shadow_tint.y > 0.0f || shading->shadow_tint.z > 0.0f;
    put3(gpu->shadow_tint, tinted ? shading->shadow_tint : v3_make(1, 1, 1), shading->rim_power);
    put3(gpu->rim, shading->rim, shading->rim_strength);

    // The shadow cascades.
    uint32_t cascades = 0;
    if (has_sun && env->sun.shadow_distance > 0.0f) {
        cascades = env->sun.shadow_cascades ? env->sun.shadow_cascades : R_SHADOW_CASCADES;
        if (cascades > R_SHADOW_CASCADES) cascades = R_SHADOW_CASCADES;
    }
    uint32_t resolution = env->sun.shadow_resolution;
    if (resolution < R_SHADOW_RESOLUTION_MIN) resolution = R_SHADOW_RESOLUTION_MIN;
    if (resolution > R_SHADOW_RESOLUTION_MAX) resolution = R_SHADOW_RESOLUTION_MAX;
    out->shadow_resolution = resolution;
    float radius_metres[R_SHADOW_CASCADES], depth_metres[R_SHADOW_CASCADES];
    if (cascades) {
        r_cascade_splits(camera->near_plane, env->sun.shadow_distance, cascades, gpu->cascade_splits);
        float from = camera->near_plane;
        for (uint32_t c = 0; c < cascades; ++c) {
            gpu->shadow[c] = r_cascade_fit(camera, aspect, from, gpu->cascade_splits[c], sun_dir, resolution, &gpu->cascade_texel[c]);
            radius_metres[c] = gpu->cascade_texel[c] * (float)resolution * 0.5f;
            depth_metres[c] = 2.0f * radius_metres[c] + R_SHADOW_CASTER_REACH;
            from = gpu->cascade_splits[c];
        }
        gpu->shadow_params[0] = 1.0f / (float)resolution;
        gpu->shadow_params[1] = R_SHADOW_NORMAL_OFFSET;
        gpu->shadow_params[2] = R_SHADOW_DEPTH_BIAS;
        gpu->shadow_params[3] = (float)cascades;
    }
    out->cascade_count = cascades;

    // The lights, and the passes: which draws each one contains.
    plane planes[5];
    frustum_planes(&gpu->view_proj, planes);
    gpu->counts[0] = select_lights(frame, planes, &out->lights);
    // The index array holds the passes' draw lists, then a light mask per draw,
    // then room to sort the main pass in.
    uint32_t *indices = reserve(scratch, (size_t)frame->draw_count * (cascades + 3));
    uint32_t *masks = indices + (size_t)frame->draw_count * (cascades + 1);
    uint32_t *temp = masks + frame->draw_count;
    prepare_materials(frame, shaders, out);
    uint32_t n = 0;
    out->draws = frame->draws;
    out->indices = indices;
    for (uint32_t c = 0; c < cascades; ++c) {
        out->shadow[c].first = n;
        for (uint32_t i = 0; i < frame->draw_count; ++i) {
            const r_draw *draw = &frame->draws[i];
            if (!draw->mesh || (draw->flags & (R_DRAW_UNLIT | R_DRAW_NO_SHADOW))) continue;
            if (sphere_in_cascade(&gpu->shadow[c], radius_metres[c], depth_metres[c], draw_bounds(draw, mesh_radius)))
                indices[n++] = i;
        }
        out->shadow[c].count = n - out->shadow[c].first;
    }
    out->main.first = n;
    for (uint32_t i = 0; i < frame->draw_count; ++i) {
        const r_draw *draw = &frame->draws[i];
        masks[i] = 0;
        if (!draw->mesh) continue;
        bounding_sphere bounds = draw_bounds(draw, mesh_radius);
        if (!sphere_in_frustum(planes, 5, bounds)) continue;
        indices[n++] = i;
        masks[i] = (draw->flags & R_DRAW_UNLIT) ? 0 : light_mask(&out->lights, gpu->counts[0], bounds);
    }
    out->main.count = n - out->main.first;
    out->light_masks = masks;
    sort_by_shader(frame, out, indices + out->main.first, out->main.count, temp);

    prepare_post(&frame->post, &env->haze, width, height, out);
    prepare_atmosphere(frame, gpu, &out->post);
    out->capture_rgba = frame->capture_rgba;
}
