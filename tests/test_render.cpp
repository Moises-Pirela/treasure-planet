// The renderer's frame preparation, without a GPU: the GPU blocks have the layout
// the shader expects, the shadow cascades enclose the slices of view they stand
// for and hold still as the camera moves, the passes contain what they should and
// nothing else, the lights are chosen by nearness, and the post stack's steps are
// wired as its data says with nothing that is off costing a step.
#include "../src/render/backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
// A function rather than an `if` in the macro: many of the conditions are
// compile-time constants (sizes and offsets), which MSVC would otherwise flag.
static void check(bool ok, const char *what, int line) {
    if (ok) return;
    printf("FAILED %s:%d: %s\n", __FILE__, line, what);
    ++failures;
}
#define CHECK(condition) check((condition), #condition, __LINE__)

static r_camera camera_at(v3 position, v3 forward) {
    r_camera c = {};
    c.position = position;
    c.forward = v3_normalize(forward);
    c.fov_y = 70.0f * TP_PI / 180.0f;
    c.near_plane = 0.1f;
    return c;
}

static r_environment lit_environment(void) {
    r_environment e = {};
    e.sun.direction = v3_normalize(v3_make(0.5f, 0.8f, 0.35f));
    e.sun.color = v3_make(1, 1, 1);
    e.sun.shadow_distance = 160.0f;
    e.ambient_sky = v3_make(0.1f, 0.1f, 0.1f);
    e.sky.sun_disc = 0.02f;
    return e;
}

static r_draw draw_at(v3 position, float scale, r_mesh mesh, uint32_t flags) {
    r_draw d = {};
    d.model = m4_translate_scale(position, v3_make(scale, scale, scale));
    d.color[0] = d.color[1] = d.color[2] = d.color[3] = 1.0f;
    d.flags = flags;
    d.mesh = mesh;
    return d;
}

static bool pass_has(const r_packet *p, r_pass pass, uint32_t draw) {
    for (uint32_t i = 0; i < pass.count; ++i)
        if (p->indices[pass.first + i] == draw) return true;
    return false;
}

// The corners of the camera's frustum slice between two view distances.
static void slice_corners(const r_camera *c, float aspect, float near_d, float far_d, v3 out[8]) {
    v3 right = v3_normalize(v3_cross(c->forward, v3_make(0, 1, 0)));
    v3 up = v3_cross(right, c->forward);
    float tan_y = tanf(c->fov_y * 0.5f), tan_x = tan_y * aspect;
    int n = 0;
    for (int end = 0; end < 2; ++end) {
        float d = end ? far_d : near_d;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                out[n++] = v3_add(v3_add(v3_add(c->position, v3_scale(c->forward, d)),
                                         v3_scale(right, d * tan_x * (float)sx)),
                                  v3_scale(up, d * tan_y * (float)sy));
    }
}

int main(void) {
    // The GPU layouts: what the shader's std140 blocks and push constants expect.
    CHECK(sizeof(r_gpu_frame) == 64 * (1 + R_SHADOW_CASCADES) + (23 + 5 * R_MAX_AIRS + R_SKY_LAYER_VEC4S * R_MAX_SKY_LAYERS) * 16);
    CHECK(sizeof(r_gpu_lights) == 2 * R_MAX_POINT_LIGHTS * 16);
    CHECK(offsetof(r_gpu_frame, shadow) == 64 && offsetof(r_gpu_frame, camera_position) == 64 * (1 + R_SHADOW_CASCADES));
    CHECK(offsetof(r_gpu_frame, counts) == sizeof(r_gpu_frame) - 16);
    CHECK(sizeof(r_push) == 96 && R_PUSH_DRAW_SIZE == 92 && offsetof(r_push, flags) == 80 && offsetof(r_push, material) == 84);
    CHECK(sizeof(r_gpu_materials) == R_MAX_MATERIALS * R_MATERIAL_VEC4S * 16);
    CHECK(sizeof(r_gpu_post) == 20 * 16 && sizeof(r_post_push) == 32);
    CHECK(r_target_info(R_TARGET_SCENE).hdr && r_target_info(R_TARGET_SCENE).shift == 0);
    CHECK(r_target_info(R_TARGET_SCENE_B).hdr && r_target_info(R_TARGET_SCENE_B).shift == 0 && !r_target_info(R_TARGET_SCENE_B).depth);
    CHECK(r_target_info(R_TARGET_DEPTH).depth && !r_target_info(R_TARGET_DEPTH).hdr && r_target_info(R_TARGET_DEPTH).shift == 0);
    CHECK(!r_target_info(R_TARGET_WINDOW).hdr && !r_target_info(R_TARGET_LDR).hdr);
    CHECK(r_target_info(R_TARGET_BLOOM_DOWN).shift == 1 && r_target_info(R_TARGET_BLOOM_DOWN + 2).shift == 3);
    CHECK(r_target_info(R_TARGET_BLOOM_UP).shift == 1 && r_target_info(R_TARGET_BLOOM_UP + R_BLOOM_MIPS - 1).shift == R_BLOOM_MIPS);

    // Splits: increasing, the last one is the sun's reach, none before the camera.
    float splits[R_SHADOW_CASCADES];
    r_cascade_splits(0.1f, 160.0f, R_SHADOW_CASCADES, splits);
    CHECK(splits[0] > 1.0f);
    for (int i = 1; i < R_SHADOW_CASCADES; ++i) CHECK(splits[i] > splits[i - 1]);
    CHECK(fabsf(splits[R_SHADOW_CASCADES - 1] - 160.0f) < 1e-3f);
    {
        float two[R_SHADOW_CASCADES];
        r_cascade_splits(0.1f, 160.0f, 2, two);
        CHECK(two[0] > splits[0] && fabsf(two[1] - 160.0f) < 1e-3f && two[2] == two[1] && two[3] == two[1]);
    }

    // A cascade encloses its slice of the view from any camera, and its texel is
    // the box's width over the map's.
    const v3 forwards[] = {v3_make(0, 0, -1), v3_make(1, 0, 0), v3_make(0.3f, -0.6f, 0.5f), v3_make(0, 0.99f, 0.1f)};
    const v3 suns[] = {v3_make(0.5f, 0.8f, 0.35f), v3_make(0, 1, 0), v3_make(1, 0.05f, 0)};
    for (int f = 0; f < 4; ++f)
        for (int s = 0; s < 3; ++s) {
            r_camera c = camera_at(v3_make(12, 3.5f, -40), forwards[f]);
            v3 sun = v3_normalize(suns[s]);
            float from = c.near_plane;
            for (int i = 0; i < R_SHADOW_CASCADES; ++i) {
                float texel;
                m4 m = r_cascade_fit(&c, 16.0f / 9.0f, from, splits[i], sun, 2048, &texel);
                v3 corners[8];
                slice_corners(&c, 16.0f / 9.0f, from, splits[i], corners);
                for (int k = 0; k < 8; ++k) {
                    v3 p = m4_transform_point(&m, corners[k]);
                    CHECK(p.x >= -1.001f && p.x <= 1.001f && p.y >= -1.001f && p.y <= 1.001f);
                    CHECK(p.z >= 0.0f && p.z <= 1.0f);
                }
                // The box is about as big as it needs to be: the far corners reach
                // near its edge (seen from the side, a slice is only as wide as its
                // narrower tangent, which is what the bound allows for).
                float furthest = 0.0f;
                for (int k = 4; k < 8; ++k) {
                    v3 p = m4_transform_point(&m, corners[k]);
                    float r = sqrtf(p.x * p.x + p.y * p.y);
                    if (r > furthest) furthest = r;
                }
                CHECK(furthest > 0.45f);
                CHECK(texel > 0.0f && texel < 1.0f);
                from = splits[i];
            }
        }

    // Texel snapping: as the camera walks, the map moves by whole texels, so a
    // shadow's edge stays on the same texels rather than shimmering.
    {
        r_camera a = camera_at(v3_make(0, 1.65f, 0), v3_make(0, 0, -1));
        v3 sun = v3_normalize(v3_make(0.5f, 0.8f, 0.35f));
        float texel_a, texel_b;
        m4 ma = r_cascade_fit(&a, 16.0f / 9.0f, 0.1f, splits[0], sun, 2048, &texel_a);
        for (int step = 1; step <= 50; ++step) {
            r_camera b = a;
            b.position = v3_add(a.position, v3_make(0.0137f * (float)step, 0.0f, -0.0071f * (float)step));
            m4 mb = r_cascade_fit(&b, 16.0f / 9.0f, 0.1f, splits[0], sun, 2048, &texel_b);
            CHECK(fabsf(texel_a - texel_b) < 1e-6f);
            // The same world point lands a whole number of texels away in both maps.
            v3 pa = m4_transform_point(&ma, v3_make(3, 0, -5)), pb = m4_transform_point(&mb, v3_make(3, 0, -5));
            float dx = (pb.x - pa.x) * 2048.0f * 0.5f, dy = (pb.y - pa.y) * 2048.0f * 0.5f;
            CHECK(fabsf(dx - roundf(dx)) < 1e-2f && fabsf(dy - roundf(dy)) < 1e-2f);
        }
    }

    // A whole frame: the passes.
    {
        float mesh_radius[R_MAX_MESHES] = {};
        mesh_radius[1] = sqrtf(3.0f);    // a unit cube
        r_draw draws[8];
        draws[0] = draw_at(v3_make(0, 0, -10), 1.0f, 1, 0);                  // ahead: drawn, casts
        draws[1] = draw_at(v3_make(0, 0, 10), 1.0f, 1, 0);                   // behind the camera: not drawn, but may cast
        draws[2] = draw_at(v3_make(0, 0, -10), 1.0f, 1, R_DRAW_UNLIT);       // an overlay: drawn, never casts
        draws[3] = draw_at(v3_make(0, 0, -10), 1.0f, 1, R_DRAW_NO_SHADOW);   // drawn, casts nothing
        draws[4] = draw_at(v3_make(5000, 0, 2000), 1.0f, 1, 0);              // far off behind: neither
        draws[5] = draw_at(v3_make(0, 0, -10), 1.0f, 0, 0);                  // no mesh: nowhere
        draws[6] = draw_at(v3_make(5000, 0, 2000), 1.0f, 7, 0);              // a mesh of unknown size: never culled
        draws[7] = draw_at(v3_make(300, 0, -300), 1000.0f, 1, R_DRAW_UNSHADOWED);   // huge: in view from anywhere; takes no shadow, still casts
        r_point_light lights[R_MAX_POINT_LIGHTS + 8];
        for (uint32_t i = 0; i < R_MAX_POINT_LIGHTS + 8; ++i) {
            lights[i].position = v3_make(0, 0, -(float)(R_MAX_POINT_LIGHTS + 8 - i) * 3.0f);   // the last is the nearest
            lights[i].radius = 4.0f;
            lights[i].color = v3_make(1, 1, 1);
        }
        lights[3].radius = 0.0f;                                         // dead: never chosen
        lights[4].position = v3_make(0, 0, -1000);                       // far, but its reach makes it nearer than most
        lights[4].radius = 1100.0f;
        r_frame frame = {};
        frame.camera = camera_at(v3_make(0, 1.65f, 0), v3_make(0, 0, -1));
        frame.environment = lit_environment();
        frame.lights = lights;
        frame.light_count = R_MAX_POINT_LIGHTS + 8;
        frame.draws = draws;
        frame.draw_count = 8;
        r_scratch scratch = {};
        r_packet p;
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);

        CHECK(p.cascade_count == R_SHADOW_CASCADES && p.frame.shadow_params[3] == (float)R_SHADOW_CASCADES);
        CHECK(p.shadow_resolution == R_SHADOW_RESOLUTION_MIN && p.frame.shadow_params[0] == 1.0f / (float)R_SHADOW_RESOLUTION_MIN);
        frame.environment.sun.shadow_resolution = 1024;
        frame.environment.sun.shadow_cascades = 2;
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.shadow_resolution == 1024 && p.cascade_count == 2 && p.frame.shadow_params[3] == 2.0f);
        CHECK(fabsf(p.frame.cascade_texel[0] * 1024.0f * 0.5f - p.frame.cascade_texel[0] * 512.0f) < 1e-6f);
        frame.environment.sun.shadow_resolution = 1u << 20;
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.shadow_resolution == R_SHADOW_RESOLUTION_MAX);
        frame.environment.sun.shadow_resolution = 0;
        frame.environment.sun.shadow_cascades = 0;
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.draws == draws && p.capture_rgba == NULL);
        CHECK(pass_has(&p, p.main, 0) && !pass_has(&p, p.main, 1) && pass_has(&p, p.main, 2) && pass_has(&p, p.main, 3));
        CHECK(!pass_has(&p, p.main, 4) && !pass_has(&p, p.main, 5) && pass_has(&p, p.main, 6) && pass_has(&p, p.main, 7));
        CHECK(pass_has(&p, p.shadow[0], 0) && pass_has(&p, p.shadow[0], 1));
        for (uint32_t c = 0; c < p.cascade_count; ++c) {
            CHECK(!pass_has(&p, p.shadow[c], 2) && !pass_has(&p, p.shadow[c], 3) && !pass_has(&p, p.shadow[c], 5));
            CHECK(!pass_has(&p, p.shadow[c], 4) && pass_has(&p, p.shadow[c], 6) && pass_has(&p, p.shadow[c], 7));
        }
        // Every index is a real draw, and the passes tile the index array.
        uint32_t total = 0;
        for (uint32_t c = 0; c < p.cascade_count; ++c) {
            CHECK(p.shadow[c].first == total);
            total += p.shadow[c].count;
        }
        CHECK(p.main.first == total);
        for (uint32_t i = 0; i < total + p.main.count; ++i) CHECK(p.indices[i] < 8);

        // The lights: the nearest R_MAX_POINT_LIGHTS, nearness counting reach; each
        // drawn draw knows which of them reach it.
        CHECK(p.frame.counts[0] == R_MAX_POINT_LIGHTS);
        CHECK(p.light_masks[0] != 0 && p.light_masks[1] == 0 && p.light_masks[2] == 0 && p.light_masks[5] == 0);   // ahead: lit; behind: not drawn; unlit: none
        const uint32_t every_light = ~0u >> (32 - R_MAX_POINT_LIGHTS);    // the low R_MAX_POINT_LIGHTS bits, without a shift by 32
        CHECK(p.light_masks[6] == every_light);                            // unknown size: every light
        {
            // The light at z=-9 (3 m off draw 0 at z=-10, radius 4) reaches it; the one at z=-93 does not.
            uint32_t near_index = UINT32_MAX, far_index = UINT32_MAX;
            for (uint32_t i = 0; i < p.frame.counts[0]; ++i) {
                if (p.lights.position[i][2] == -9.0f) near_index = i;
                if (p.lights.position[i][2] == -93.0f) far_index = i;
            }
            CHECK(near_index != UINT32_MAX && (p.light_masks[0] & (1u << near_index)) != 0);
            CHECK(far_index != UINT32_MAX && (p.light_masks[0] & (1u << far_index)) == 0);
        }
        bool has_far_big = false, has_dead = false, has_farthest = false;
        for (uint32_t i = 0; i < p.frame.counts[0]; ++i) {
            CHECK(p.lights.position[i][3] > 0.0f);
            if (p.lights.position[i][2] == -1000.0f) has_far_big = true;
            if (p.lights.position[i][3] == 0.0f) has_dead = true;
            if (p.lights.position[i][2] == lights[0].position.z) has_farthest = true;
            if (i > 0) {
                float a = v3_length(v3_sub(v3_make(p.lights.position[i - 1][0], p.lights.position[i - 1][1], p.lights.position[i - 1][2]), frame.camera.position)) - p.lights.position[i - 1][3];
                float b = v3_length(v3_sub(v3_make(p.lights.position[i][0], p.lights.position[i][1], p.lights.position[i][2]), frame.camera.position)) - p.lights.position[i][3];
                CHECK(a <= b);
            }
        }
        CHECK(has_far_big && !has_dead && !has_farthest);

        // The camera's frame for the sky: unit, orthogonal, with the right tangents.
        v3 f = v3_make(p.frame.camera_forward[0], p.frame.camera_forward[1], p.frame.camera_forward[2]);
        v3 r = v3_make(p.frame.camera_right[0], p.frame.camera_right[1], p.frame.camera_right[2]);
        v3 u = v3_make(p.frame.camera_up[0], p.frame.camera_up[1], p.frame.camera_up[2]);
        CHECK(fabsf(v3_length(f) - 1.0f) < 1e-5f && fabsf(v3_length(r) - 1.0f) < 1e-5f && fabsf(v3_length(u) - 1.0f) < 1e-5f);
        CHECK(fabsf(v3_dot(f, r)) < 1e-5f && fabsf(v3_dot(f, u)) < 1e-5f && fabsf(v3_dot(r, u)) < 1e-5f);
        CHECK(fabsf(p.frame.camera_up[3] - tanf(frame.camera.fov_y * 0.5f)) < 1e-5f);
        CHECK(fabsf(p.frame.camera_right[3] - tanf(frame.camera.fov_y * 0.5f) * 1280.0f / 720.0f) < 1e-5f);
        CHECK(fabsf(p.frame.sun_dir[3] - cosf(0.02f)) < 1e-6f);
        CHECK(p.frame.counts[1] == 0 && p.frame.sky[0] == 0.0f && p.frame.sky[2] > 0.0f);
        CHECK(p.frame.camera_forward[3] == frame.camera.near_plane);
        // Rolled a quarter turn, as aboard a banking craft: the picture's up is the one
        // asked for, and what is off to that side lands at the top of the screen.
        frame.camera.up = v3_make(1, 0, 0);
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.frame.camera_up[0] > 0.999f && fabsf(p.frame.camera_right[1] + 1.0f) < 1e-5f);
        {
            v3 off = v3_make(4.0f, 1.65f, -10.0f);
            const float *m = p.frame.view_proj.m;
            float x = m[0] * off.x + m[4] * off.y + m[8] * off.z + m[12], y = m[1] * off.x + m[5] * off.y + m[9] * off.z + m[13];
            float w = m[3] * off.x + m[7] * off.y + m[11] * off.z + m[15];
            CHECK(w > 0.0f && y / w > 0.3f && fabsf(x / w) < 1e-4f);
        }
        frame.camera.up = v3_make(0, 0, 0);   // unset: the world's up, as before
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(fabsf(p.frame.camera_up[1] - 1.0f) < 1e-5f);
        // No shading settings: plain Lambert, no tint (the shader multiplies by 1), no rim.
        CHECK(p.frame.ramp[0] == 0.0f && p.frame.ramp[1] == 0.0f && p.frame.ramp[2] == 0.0f);
        CHECK(p.frame.shadow_tint[0] == 1.0f && p.frame.shadow_tint[1] == 1.0f && p.frame.shadow_tint[2] == 1.0f);
        CHECK(p.frame.rim[3] == 0.0f);
        frame.environment.shading.ramp_softness = 0.3f;
        frame.environment.shading.ramp_flatness = 0.5f;
        frame.environment.shading.ramp_steps = 3.0f;
        frame.environment.shading.shadow_tint = v3_make(0.7f, 0.72f, 1.0f);
        frame.environment.shading.rim = v3_make(0.4f, 0.3f, 0.8f);
        frame.environment.shading.rim_strength = 0.6f;
        frame.environment.shading.rim_power = 3.0f;
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.frame.ramp[0] == 0.3f && p.frame.ramp[2] == 0.5f && p.frame.ramp[3] == 3.0f);
        CHECK(p.frame.shadow_tint[0] == 0.7f && p.frame.shadow_tint[2] == 1.0f && p.frame.shadow_tint[3] == 3.0f);
        CHECK(p.frame.rim[0] == 0.4f && p.frame.rim[3] == 0.6f);

        // A point ahead of the camera projects ahead, with reversed depth below 1.
        {
            v3 ahead = m4_transform_point(&p.frame.view_proj, v3_make(0, 1.65f, -10));
            float w = p.frame.view_proj.m[3] * 0 + p.frame.view_proj.m[7] * 1.65f + p.frame.view_proj.m[11] * -10 + p.frame.view_proj.m[15];
            CHECK(w > 0.0f && ahead.z / w > 0.0f && ahead.z / w < 1.0f);
        }

        // No sun: no shadow passes, and the main pass is unchanged.
        frame.environment.sun.color = v3_make(0, 0, 0);
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.cascade_count == 0 && p.frame.shadow_params[3] == 0.0f && p.main.first == 0);
        CHECK(pass_has(&p, p.main, 0) && !pass_has(&p, p.main, 1));
        frame.environment = lit_environment();
        frame.environment.sun.shadow_distance = 0.0f;
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.cascade_count == 0);

        // Nothing to draw, nothing to light.
        frame.draw_count = 0;
        frame.light_count = 0;
        r_prepare(&frame, mesh_radius, NULL, 1280, 720, &scratch, &p);
        CHECK(p.main.count == 0 && p.frame.counts[0] == 0);
        r_scratch_free(&scratch);
        CHECK(scratch.indices == NULL && scratch.capacity == 0);
    }

    // The sky's layers: laid out in their order as common.glsl reads them, the
    // directions made unit, a layer of no known kind left out, the seed kept to
    // 24 bits so it survives as a float, the drift turned into a turn by the time.
    {
        r_frame frame = {};
        frame.camera = camera_at(v3_make(0, 0, 0), v3_make(0, 0, -1));
        frame.environment = lit_environment();
        frame.time = 10.0f;
        r_sky *sky = &frame.environment.sky;
        sky->layers[0].kind = R_SKY_NEBULA;
        sky->layers[0].seed = 0x12345678u;
        sky->layers[0].direction = v3_make(0, 0, -2);
        sky->layers[0].axis = v3_make(0, 3, 0);
        sky->layers[0].size = 0.8f;
        sky->layers[0].color = v3_make(0.5f, 0.2f, 0.8f);
        sky->layers[0].color2 = v3_make(0.1f, 0.6f, 0.7f);
        sky->layers[0].brightness = 0.3f;
        sky->layers[0].density = 0.4f;
        sky->layers[0].detail = 3.0f;
        sky->layers[0].drift = 0.01f;
        sky->layers[1].kind = 7;    // no such layer
        sky->layers[2].kind = R_SKY_BODY;
        sky->layers[2].direction = v3_make(1, 1, 0);
        sky->layers[2].size = 0.1f;
        sky->layers[2].ring_inner = 1.4f;
        sky->layers[2].ring_outer = 2.2f;
        sky->layers[3].kind = R_SKY_STARS;
        sky->layers[3].density = 0.1f;
        sky->layers[3].brightness = 1.5f;
        sky->layers[3].seed = 12345;
        sky->layer_count = 4;
        r_scratch scratch = {};
        r_packet p;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.frame.counts[1] == 3 && p.frame.sky[0] == 3.0f);
        const float (*nebula)[4] = p.frame.sky_layers[0];
        CHECK(nebula[0][2] == -1.0f && nebula[0][3] == (float)R_SKY_NEBULA);
        CHECK(nebula[1][1] == 1.0f && nebula[1][3] == 0.8f);
        CHECK(nebula[2][0] == 0.5f && nebula[2][3] == 0.3f && nebula[3][1] == 0.6f && nebula[3][3] == 0.4f);
        CHECK(nebula[4][0] == 3.0f && fabsf(nebula[4][1] - 0.1f) < 1e-6f && nebula[4][2] == (float)(0x12345678u & 0xffffffu));
        const float (*body)[4] = p.frame.sky_layers[1];
        CHECK(body[0][3] == (float)R_SKY_BODY && fabsf(body[0][0] - 0.70710678f) < 1e-5f && fabsf(body[0][1] - 0.70710678f) < 1e-5f);
        CHECK(body[1][1] == 1.0f && body[1][3] == 0.1f);    // no axis given: up
        CHECK(body[4][3] == 1.4f && body[5][0] == 2.2f);
        const float (*stars)[4] = p.frame.sky_layers[2];
        CHECK(stars[0][3] == (float)R_SKY_STARS && stars[0][0] == 0.0f && stars[0][1] == 0.0f && stars[0][2] == 0.0f);
        CHECK(stars[2][3] == 1.5f && stars[3][3] == 0.1f && stars[4][2] == 12345.0f);
        CHECK(p.frame.sky_layers[3][0][3] == 0.0f);    // nothing past the count
        r_scratch_free(&scratch);
    }

    // Atmospheres: how deep the camera is, and the one it is deepest in scaled into the block.
    {
        r_atmosphere air[2] = {};
        air[0].centre = v3_make(0, -260, 0);
        air[0].radius = 260.0f;
        air[0].height = 300.0f;
        air[0].zenith = v3_make(0.2f, 0.4f, 0.8f);
        air[0].horizon = v3_make(0.6f, 0.7f, 0.8f);
        air[0].haze = v3_make(0.5f, 0.6f, 0.7f);
        air[0].density = 0.004f;
        air[0].star_fade = 0.9f;
        air[0].sun_glow = 0.6f;
        air[1] = air[0];
        air[1].centre = v3_make(5000, 0, 0);
        air[1].haze = v3_make(1, 0, 0);
        // Outside the shell, at its edge, a third of the way down, on the surface, below it.
        CHECK(r_atmosphere_immersion(&air[0], v3_make(0, 500, 0)) == 0.0f);
        CHECK(r_atmosphere_immersion(&air[0], v3_make(0, 300, 0)) == 0.0f);
        CHECK(fabsf(r_atmosphere_immersion(&air[0], v3_make(0, 200, 0)) - 1.0f / 3.0f) < 1e-5f);
        CHECK(r_atmosphere_immersion(&air[0], v3_make(0, 0, 0)) == 1.0f);
        CHECK(r_atmosphere_immersion(&air[0], v3_make(0, -100, 0)) == 1.0f);
        air[0].falloff = 2.0f;
        CHECK(fabsf(r_atmosphere_immersion(&air[0], v3_make(0, 150, 0)) - 0.25f) < 1e-5f);   // half way down, squared
        air[0].falloff = 0.0f;
        // A frame in open space leaves the block at zero; one inside fills it by the depth.
        r_frame frame = {};
        frame.camera = camera_at(v3_make(0, 1000, 0), v3_make(0, 0, -1));
        frame.environment = lit_environment();
        frame.atmospheres = air;
        frame.atmosphere_count = 2;
        r_scratch scratch = {};
        r_packet p;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.frame.haze[3] == 0.0f && p.frame.air_zenith[3] == 0.0f && p.frame.air_horizon[3] == 0.0f && p.frame.air[0] == 0.0f);
        // Both shells are in the block from anywhere, the nearer first, for the haze seen through them.
        CHECK(p.frame.counts[2] == 2);
        CHECK(p.frame.air_sphere[0][1] == -260.0f && p.frame.air_sphere[0][3] == 560.0f && p.frame.air_haze[0][3] == 0.004f && p.frame.air_haze[0][0] == 0.5f);
        CHECK(p.frame.air_sphere[1][0] == 5000.0f && p.frame.air_haze[1][0] == 1.0f);
        frame.camera.position = v3_make(4800, 0, 0);   // nearer the second: it comes first
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.frame.air_sphere[0][0] == 5000.0f && p.frame.air_sphere[1][0] == 0.0f);
        frame.camera.position = v3_make(0, 150, 0);    // half way down the first shell, nowhere near the second
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(fabsf(p.frame.air_zenith[3] - 0.5f) < 1e-5f && p.frame.air_zenith[0] == 0.2f);
        CHECK(p.frame.haze[3] == 0.0f);    // no cloud deck: nothing hazes by the plain distance
        CHECK(fabsf(p.frame.air_horizon[3] - 0.3f) < 1e-5f && fabsf(p.frame.air[0] - 0.45f) < 1e-5f);
        CHECK(p.post.entry[2] == 0.0f);    // no entry speed: never an entry

        // The entry: the speed against the atmosphere's, times a band over the depth
        // that is nothing at the shell's edge and at the surface and full half way.
        air[0].entry_speed = 60.0f;
        air[0].entry_glow = v3_make(1, 0.5f, 0.2f);
        CHECK(r_atmosphere_entry(&air[0], v3_make(0, 150, 0), v3_make(60, 0, 0)) == 1.0f);
        CHECK(r_atmosphere_entry(&air[0], v3_make(0, 150, 0), v3_make(30, 0, 0)) == 0.5f);
        CHECK(r_atmosphere_entry(&air[0], v3_make(0, 150, 0), v3_make(600, 0, 0)) == 1.0f);
        CHECK(r_atmosphere_entry(&air[0], v3_make(0, 300, 0), v3_make(60, 0, 0)) == 0.0f);
        CHECK(r_atmosphere_entry(&air[0], v3_make(0, 0, 0), v3_make(60, 0, 0)) == 0.0f);
        CHECK(fabsf(r_atmosphere_entry(&air[0], v3_make(0, 225, 0), v3_make(60, 0, 0)) - 0.75f) < 1e-5f);   // a quarter down
        CHECK(r_atmosphere_entry(&air[0], v3_make(0, 150, 0), v3_make(0, 0, 0)) == 0.0f);
        // Into the post block: full strength, heading straight ahead lands in the middle of the screen.
        frame.camera.velocity = v3_make(0, 0, -60);
        frame.post.entry.blur = 0.06f;
        frame.post.entry.samples = 8;
        frame.post.entry.streaks = 22.0f;
        frame.post.entry.streak_length = 1.5f;
        frame.post.entry.streak_speed = 1.5f;
        frame.post.entry.glow = 0.8f;
        frame.time = 12.5f;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.post.entry[2] == 1.0f && p.post.entry[3] == 0.06f);
        CHECK(fabsf(p.post.entry[0] - 0.5f) < 1e-4f && fabsf(p.post.entry[1] - 0.5f) < 1e-4f);
        CHECK(fabsf(p.post.entry_glow[0] - 0.8f) < 1e-6f && p.post.entry_glow[3] == 8.0f);
        CHECK(p.post.entry_streaks[0] == 22.0f && p.post.entry_streaks[1] == 12.5f && p.post.entry_streaks[2] == 1.5f && p.post.entry_streaks[3] == 1.5f);
        // Heading right: the point of travel is to the right of the middle.
        frame.camera.velocity = v3_make(60, 0, -60);
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.post.entry[0] > 0.6f && fabsf(p.post.entry[1] - 0.5f) < 1e-4f);

        // The cloud deck: thick haze of its colour where the camera is in it, none outside.
        air[0].cloud_altitude = 120.0f;
        air[0].cloud_thickness = 60.0f;
        air[0].cloud_density = 0.05f;
        air[0].cloud_color = v3_make(0.9f, 0.9f, 0.9f);
        air[0].cloud_cover = 0.4f;
        air[0].cloud_detail = 7.0f;
        air[0].cloud_sharpness = 4.0f;
        air[0].cloud_drift = 0.01f;
        air[0].cloud_seed = 5;
        frame.time = 3.0f;
        frame.camera.velocity = v3_make(0, 0, 0);
        frame.camera.position = v3_make(0, 120, 0);    // in the middle of the deck
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(fabsf(p.frame.haze[3] - 0.05f) < 1e-6f && p.frame.haze[0] == 0.9f && fabsf(p.frame.air_zenith[0] - 0.9f) < 1e-6f);
        frame.camera.position = v3_make(0, 135, 0);    // half way out of it: three quarters of the deck's haze
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(fabsf(p.frame.haze[3] - 0.05f * 0.75f) < 1e-6f);
        frame.camera.position = v3_make(0, 200, 0);    // above it: the shell's own haze is in the shells, not here
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.frame.haze[3] == 0.0f && p.frame.air_haze[0][3] == 0.004f);
        // The cloud layer, for its shadow: its sphere and its pattern, turned by the drift so far.
        CHECK(p.frame.cloud_sphere[0][3] == 380.0f && p.frame.cloud_sphere[0][1] == -260.0f);
        CHECK(p.frame.cloud_layer[0][0] == 0.4f && p.frame.cloud_layer[0][1] == 7.0f && p.frame.cloud_layer[0][2] == 4.0f && p.frame.cloud_layer[0][3] == 5.0f);
        CHECK(fabsf(p.frame.cloud_turn[0][0] - 0.03f) < 1e-6f);
        CHECK(p.frame.cloud_sphere[1][3] == 0.0f);    // the second air has no layer
        r_scratch_free(&scratch);
    }

    // Materials: the parameters go to the block as given; each one's shader is the
    // backend's index for it, the plain surface for a material of no shader or
    // past the table; the lit pass is ordered by shader.
    {
        r_shader_set shaders = {};
        shaders.count = 3;
        strcpy(shaders.items[0].name, "clouds");
        shaders.items[0].cutout = true;
        strcpy(shaders.items[1].name, "planet");
        strcpy(shaders.items[2].name, "surface");
        shaders.surface = 2;
        r_material materials[3] = {};
        materials[0].shader = 3;                       // surface
        materials[0].params[0][0] = 0.08f;
        materials[1].shader = 2;                       // planet
        materials[1].params[8][1] = 25.0f;
        materials[2].shader = 1;                       // clouds, a cutout
        materials[2].params[0][3] = 0.4f;
        r_draw draws[4];
        draws[0] = draw_at(v3_make(0, 0, -10), 1.0f, 1, 0);
        draws[0].material = 2;                         // clouds
        draws[1] = draw_at(v3_make(2, 0, -10), 1.0f, 1, 0);
        draws[1].material = 1;                         // planet
        draws[2] = draw_at(v3_make(-2, 0, -10), 1.0f, 1, 0);
        draws[2].material = 0;                         // surface
        draws[3] = draw_at(v3_make(0, 2, -10), 1.0f, 1, 0);
        draws[3].material = 9;                         // past the table: the surface
        r_frame frame = {};
        frame.camera = camera_at(v3_make(0, 1.65f, 0), v3_make(0, 0, -1));
        frame.environment = lit_environment();
        frame.materials = materials;
        frame.material_count = 3;
        frame.draws = draws;
        frame.draw_count = 4;
        r_scratch scratch = {};
        r_packet p;
        r_prepare(&frame, NULL, &shaders, 1280, 720, &scratch, &p);
        CHECK(p.materials.items[0][0][0] == 0.08f && p.materials.items[1][8][1] == 25.0f && p.materials.items[2][0][3] == 0.4f);
        CHECK(p.material_shader[0] == 2 && p.material_shader[1] == 1 && p.material_shader[2] == 0 && p.material_shader[9] == 2);
        CHECK(!p.material_cutout[0] && !p.material_cutout[1] && p.material_cutout[2] && !p.material_cutout[9]);
        CHECK(p.main.count == 4);
        uint32_t order[4];
        for (uint32_t i = 0; i < 4; ++i) order[i] = p.indices[p.main.first + i];
        CHECK(order[0] == 0 && order[1] == 1 && (order[2] == 2 || order[2] == 3) && order[2] != order[3]);   // clouds, planet, then the two surfaces in their order
        CHECK(order[2] == 2 && order[3] == 3);                                                               // stable
        // No shader set at all: everything is shader 0, nothing is a cutout.
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.material_shader[2] == 0 && !p.material_cutout[2]);
        r_scratch_free(&scratch);
    }

    // Models: laid out as draws at a transform, the parts' own transforms within it.
    {
        r_model_part parts[2] = {};
        parts[0].mesh = 1;
        parts[0].material = 3;
        parts[0].transform = m4_translate_scale(v3_make(0, 1, 0), v3_make(1, 2, 1));
        parts[1].mesh = 2;
        parts[1].material = 0;
        parts[1].transform = m4_identity();
        m4 at = m4_translate_scale(v3_make(10, 0, 0), v3_make(2, 2, 2));
        const float color[4] = {1, 0.5f, 0.25f, 1};
        r_draw draws[2];
        CHECK(r_model_draws(parts, 2, &at, color, R_DRAW_NO_SHADOW, draws, 2) == 2);
        CHECK(draws[0].mesh == 1 && draws[0].material == 3 && draws[1].mesh == 2 && draws[1].material == 0);
        CHECK(draws[0].flags == R_DRAW_NO_SHADOW && draws[0].color[1] == 0.5f);
        CHECK(draws[0].model.m[12] == 10.0f && draws[0].model.m[13] == 2.0f && draws[0].model.m[5] == 4.0f);   // 10 + 2 * (0, 1, 0); scales 2 * 2
        CHECK(draws[1].model.m[12] == 10.0f && draws[1].model.m[0] == 2.0f);
        CHECK(r_model_draws(parts, 2, &at, color, 0, draws, 1) == 1);   // no more than there is room for
    }

    // Wavefront OBJ: positions, normals, faces of any shape, negative indices.
    {
        const char *text =
            "# a square and a triangle\n"
            "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
            "vn 0 0 1\n"
            "f 1//1 2//1 3//1 4//1\n"
            "v 0 0 1\n"
            "f -1 1 2\n";
        r_obj_mesh mesh;
        CHECK(r_obj_parse(text, strlen(text), &mesh));
        CHECK(mesh.index_count == 9 && mesh.vertex_count == 9);   // two triangles of the square, one more
        CHECK(mesh.vertices[0].normal[2] == 1.0f && mesh.vertices[5].position[0] == 0.0f && mesh.vertices[5].position[1] == 1.0f);
        CHECK(mesh.vertices[6].position[2] == 1.0f);               // the last vertex, by -1
        float flat = mesh.vertices[6].normal[0] * 0 + fabsf(mesh.vertices[6].normal[1]) + fabsf(mesh.vertices[6].normal[2]);
        CHECK(flat > 0.99f);                                       // a flat normal, made for the face without one
        r_obj_free(&mesh);
        CHECK(!r_obj_parse("f 1 2 9\n", 8, &mesh));               // an index past the vertices
        CHECK(!r_obj_parse("v 0 0 0\n", 8, &mesh));               // no faces
    }

    // The post stack.
    {
        r_frame frame = {};
        frame.camera = camera_at(v3_make(0, 1.65f, 0), v3_make(0, 0, -1));
        frame.environment = lit_environment();
        r_scratch scratch = {};
        r_packet p;

        // All zero: one step, the composite straight to the window, and a block
        // that changes nothing but the gamma.
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.step_count == 1 && p.steps[0].program == R_POST_COMPOSITE && p.steps[0].target == R_TARGET_WINDOW);
        CHECK(p.steps[0].inputs[0] == R_TARGET_SCENE && p.steps[0].inputs[1] == R_TARGET_SCENE && p.steps[0].inputs[2] == R_TARGET_SCENE);
        CHECK(p.steps[0].params[0] == 0.0f && p.steps[0].params[1] == 0.0f && p.steps[0].params[2] == 0.0f);
        CHECK(p.post.tonemap[0] == 1.0f && p.post.tonemap[1] == (float)R_TONEMAP_CLAMP && p.post.tonemap[3] == 2.2f);
        CHECK(fabsf(p.post.balance[0] - 1.0f) < 1e-5f && fabsf(p.post.balance[1] - 1.0f) < 1e-5f && fabsf(p.post.balance[2] - 1.0f) < 1e-5f);
        CHECK(p.post.balance[3] == 1.0f && p.post.grade[0] == 1.0f);
        for (int k = 0; k < 3; ++k) CHECK(p.post.lift[k] == 0.0f && p.post.gamma[k] == 1.0f && p.post.gain[k] == 1.0f);
        CHECK(p.post.vignette[0] == 0.0f && p.post.bloom[0] == 0.0f && p.post.flare[0] == 0.0f && p.post.aberration[0] == 0.0f);

        // How many halvings a window holds: the coarsest mip stays a few pixels across.
        CHECK(r_bloom_mips_that_fit(1280, 720) == R_BLOOM_MIPS);
        CHECK(r_bloom_mips_that_fit(64, 64) == 4 && r_bloom_mips_that_fit(16, 16) == 2 && r_bloom_mips_that_fit(4, 4) == 0);

        // Bloom with three mips: down the chain, back up it, the composite reading the top.
        frame.post.bloom.intensity = 0.1f;
        frame.post.bloom.mips = 3;
        frame.post.bloom.radius = 1.5f;
        frame.post.bloom.scatter = 0.7f;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.step_count == 6);
        CHECK(p.steps[0].program == R_POST_DOWNSAMPLE && p.steps[0].inputs[0] == R_TARGET_SCENE && p.steps[0].target == R_TARGET_BLOOM_DOWN && p.steps[0].params[0] == 1.0f);
        CHECK(p.steps[1].program == R_POST_DOWNSAMPLE && p.steps[1].inputs[0] == R_TARGET_BLOOM_DOWN && p.steps[1].target == R_TARGET_BLOOM_DOWN + 1 && p.steps[1].params[0] == 0.0f);
        CHECK(p.steps[2].program == R_POST_DOWNSAMPLE && p.steps[2].inputs[0] == R_TARGET_BLOOM_DOWN + 1 && p.steps[2].target == R_TARGET_BLOOM_DOWN + 2);
        CHECK(p.steps[3].program == R_POST_UPSAMPLE && p.steps[3].inputs[0] == R_TARGET_BLOOM_DOWN + 2 && p.steps[3].inputs[1] == R_TARGET_BLOOM_DOWN + 1 && p.steps[3].target == R_TARGET_BLOOM_UP + 1);
        CHECK(p.steps[3].params[0] == 1.5f && p.steps[3].params[1] == 0.7f);
        CHECK(p.steps[4].program == R_POST_UPSAMPLE && p.steps[4].inputs[0] == R_TARGET_BLOOM_UP + 1 && p.steps[4].inputs[1] == R_TARGET_BLOOM_DOWN && p.steps[4].target == R_TARGET_BLOOM_UP);
        CHECK(p.steps[5].program == R_POST_COMPOSITE && p.steps[5].inputs[1] == R_TARGET_BLOOM_UP && p.steps[5].params[1] == 1.0f && p.steps[5].target == R_TARGET_WINDOW);
        CHECK(p.post.bloom[0] == 0.1f);
        for (uint32_t i = 0; i < p.step_count; ++i)
            for (int k = 0; k < 3; ++k) CHECK(p.steps[i].inputs[k] != p.steps[i].target);   // never reads what it writes

        // As many mips as fit, and one mip only.
        frame.post.bloom.mips = 0;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.step_count == 2 * R_BLOOM_MIPS);
        frame.post.bloom.mips = 1;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.step_count == 2 && p.steps[1].program == R_POST_COMPOSITE && p.steps[1].inputs[1] == R_TARGET_BLOOM_DOWN);
        // Too small a window for any bloom: none, and the composite says so.
        r_prepare(&frame, NULL, NULL, 4, 4, &scratch, &p);
        CHECK(p.step_count == 1 && p.post.bloom[0] == 0.0f && p.steps[0].params[1] == 0.0f);

        // The flare alone still needs the chain down to its mip; its mip is clamped to what fits.
        frame.post.bloom.intensity = 0.0f;
        frame.post.flare.intensity = 0.2f;
        frame.post.flare.source_mip = 2;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.step_count == 5);
        CHECK(p.steps[3].program == R_POST_FLARE && p.steps[3].inputs[0] == R_TARGET_BLOOM_DOWN + 2 && p.steps[3].target == R_TARGET_FLARE);
        CHECK(p.steps[4].program == R_POST_COMPOSITE && p.steps[4].inputs[1] == R_TARGET_SCENE && p.steps[4].inputs[2] == R_TARGET_FLARE);
        CHECK(p.steps[4].params[1] == 0.0f && p.steps[4].params[2] == 1.0f && p.post.flare[0] == 0.2f && p.post.bloom[0] == 0.0f);
        frame.post.flare.source_mip = 5;
        r_prepare(&frame, NULL, NULL, 16, 16, &scratch, &p);
        CHECK(p.step_count == 4 && p.steps[2].program == R_POST_FLARE && p.steps[2].inputs[0] == R_TARGET_BLOOM_DOWN + 1);

        // Anti-aliasing: the composite goes to the LDR target with luma in alpha, then FXAA to the window.
        frame.post.flare.intensity = 0.0f;
        frame.post.antialiasing.mode = R_AA_FXAA;
        frame.post.antialiasing.iterations = 100;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.step_count == 2);
        CHECK(p.steps[0].program == R_POST_COMPOSITE && p.steps[0].target == R_TARGET_LDR && p.steps[0].params[0] == 1.0f);
        CHECK(p.steps[1].program == R_POST_FXAA && p.steps[1].inputs[0] == R_TARGET_LDR && p.steps[1].target == R_TARGET_WINDOW);
        CHECK(p.post.fxaa[3] == (float)R_FXAA_MAX_ITERATIONS);

        // Everything on at full size fits the step array.
        frame.post.bloom.intensity = 0.1f;
        frame.post.bloom.mips = 0;
        frame.post.flare.intensity = 0.2f;
        r_prepare(&frame, NULL, NULL, 4096, 4096, &scratch, &p);
        CHECK(p.step_count == 2 * R_BLOOM_MIPS + 2 && p.step_count <= R_MAX_POST_STEPS);   // down, up, flare, composite, FXAA

        // The outline and the haze come first, at full size, ping-ponging between
        // the scene target and its twin, and what follows reads the result.
        frame.post.outline.strength = 0.8f;
        frame.post.outline.width = 2.0f;
        frame.post.outline.normal_threshold = 0.3f;
        frame.post.outline.color = v3_make(0.1f, 0.0f, 0.05f);
        frame.post.outline.fade_distance = 200.0f;
        r_prepare(&frame, NULL, NULL, 4096, 4096, &scratch, &p);
        CHECK(p.step_count == 2 * R_BLOOM_MIPS + 3 && p.step_count <= R_MAX_POST_STEPS);
        CHECK(p.steps[0].program == R_POST_OUTLINE && p.steps[0].inputs[0] == R_TARGET_SCENE && p.steps[0].inputs[1] == R_TARGET_DEPTH);
        CHECK(p.steps[0].target == R_TARGET_SCENE_B);
        CHECK(p.steps[1].program == R_POST_DOWNSAMPLE && p.steps[1].inputs[0] == R_TARGET_SCENE_B);
        CHECK(p.steps[p.step_count - 2].program == R_POST_COMPOSITE && p.steps[p.step_count - 2].inputs[0] == R_TARGET_SCENE_B);
        CHECK(p.post.outline[0] == 0.8f && p.post.outline[1] == 2.0f && p.post.outline[2] == 0.05f && p.post.outline[3] == 0.3f);
        CHECK(p.post.outline_color[0] == 0.1f && p.post.outline_color[2] == 0.05f && p.post.outline_color[3] == 200.0f);
        CHECK(p.post.haze_color[3] == 0.0f);
        frame.environment.haze.density = 0.01f;
        frame.environment.haze.color = v3_make(0.2f, 0.3f, 0.4f);
        frame.environment.haze.base = 5.0f;
        frame.environment.haze.falloff = 0.1f;
        frame.environment.haze.start = 2.0f;
        frame.environment.haze.sky_distance = 500.0f;
        frame.environment.haze.sun_color = v3_make(1.0f, 0.8f, 0.6f);
        frame.environment.haze.sun_power = 8.0f;
        r_prepare(&frame, NULL, NULL, 4096, 4096, &scratch, &p);
        CHECK(p.step_count == 2 * R_BLOOM_MIPS + 4 && p.step_count <= R_MAX_POST_STEPS);
        CHECK(p.steps[1].program == R_POST_HAZE && p.steps[1].inputs[0] == R_TARGET_SCENE_B && p.steps[1].inputs[1] == R_TARGET_DEPTH);
        CHECK(p.steps[1].target == R_TARGET_SCENE);
        CHECK(p.steps[2].program == R_POST_DOWNSAMPLE && p.steps[2].inputs[0] == R_TARGET_SCENE);
        CHECK(p.steps[p.step_count - 2].program == R_POST_COMPOSITE && p.steps[p.step_count - 2].inputs[0] == R_TARGET_SCENE);
        CHECK(p.post.haze_color[0] == 0.2f && p.post.haze_color[3] == 0.01f && p.post.haze_shape[0] == 5.0f && p.post.haze_shape[1] == 0.1f);
        CHECK(p.post.haze_shape[2] == 2.0f && p.post.haze_shape[3] == 500.0f && p.post.haze_sun[0] == 1.0f && p.post.haze_sun[3] == 8.0f);
        for (uint32_t i = 0; i < p.step_count; ++i) {
            for (int k = 0; k < 3; ++k) CHECK(p.steps[i].inputs[k] != p.steps[i].target);   // never reads what it writes
            CHECK(p.steps[i].target != R_TARGET_DEPTH);                                      // and never writes the depth
        }
        // The haze alone reads the scene and writes its twin.
        frame.post.outline.strength = 0.0f;
        r_prepare(&frame, NULL, NULL, 4096, 4096, &scratch, &p);
        CHECK(p.steps[0].program == R_POST_HAZE && p.steps[0].inputs[0] == R_TARGET_SCENE && p.steps[0].target == R_TARGET_SCENE_B);
        CHECK(p.steps[1].inputs[0] == R_TARGET_SCENE_B && p.post.outline[0] == 0.0f);
        frame.environment.haze.density = 0.0f;

        // The grading's block: the balance keeps white's luminance, the rest is as defined.
        frame.post.tonemap.curve = R_TONEMAP_FILMIC;
        frame.post.tonemap.exposure = 0.5f;
        frame.post.tonemap.white_point = 8.0f;
        frame.post.grading.temperature = 0.6f;
        frame.post.grading.tint = -0.3f;
        frame.post.grading.contrast = 0.2f;
        frame.post.grading.saturation = -0.5f;
        frame.post.grading.gamma = v3_make(0.25f, 0, 0);
        frame.post.grading.gain = v3_make(0, 0.1f, 0);
        frame.post.grading.lift = v3_make(0, 0, 0.05f);
        frame.post.grading.output_gamma = 2.4f;
        r_prepare(&frame, NULL, NULL, 1280, 720, &scratch, &p);
        CHECK(p.post.tonemap[0] == 0.5f && p.post.tonemap[1] == (float)R_TONEMAP_FILMIC && p.post.tonemap[2] == 8.0f && p.post.tonemap[3] == 2.4f);
        CHECK(p.post.balance[0] > p.post.balance[2]);    // warmer
        CHECK(fabsf(0.2126f * p.post.balance[0] + 0.7152f * p.post.balance[1] + 0.0722f * p.post.balance[2] - 1.0f) < 1e-5f);
        CHECK(p.post.balance[3] == 0.5f && fabsf(p.post.grade[0] - 1.2f) < 1e-6f);
        CHECK(fabsf(p.post.gamma[0] - 0.8f) < 1e-6f && p.post.gamma[1] == 1.0f && fabsf(p.post.gain[1] - 1.1f) < 1e-6f && p.post.lift[2] == 0.05f);
        r_scratch_free(&scratch);
    }

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    puts("test_render: ok");
    return 0;
}
