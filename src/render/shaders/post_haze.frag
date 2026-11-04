// A post step: the haze. Everything fades into the haze's colour by the density
// along its line of sight, integrated exactly: the same below the base height,
// thinning above it by the falloff, none before the start; the sky is as far as
// said, or infinitely far. Towards the sun the haze glows in the sun's colour.
// An unlit pixel (the overlay) is left as it is.
// Inputs: the scene, the depth.
#define POST_STEP
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;

// The density along the way from the eye at height y0, climbing dy a metre,
// between t0 and t1 metres out. Above the base the density is rho * exp(-f *
// (y - base)), below it rho; the way is split where it crosses the base, and
// the part above it integrates in closed form.
float haze_amount(float y0, float dy, float t0, float t1) {
    float rho = u_haze_color.w, base = u_haze_shape.x, f = u_haze_shape.y;
    t1 = max(t1, t0);
    if (f <= 0.0) return rho * (t1 - t0);
    float h0 = y0 - base;
    if (abs(dy) < 1e-5) return rho * exp(-f * max(h0, 0.0)) * (t1 - t0);
    float tb = clamp(-h0 / dy, t0, t1);
    float below, a, b;
    if (dy > 0.0) { below = tb - t0; a = tb; b = t1; }
    else { below = t1 - tb; a = t0; b = tb; }
    float ha = max(h0 + dy * a, 0.0), hb = max(h0 + dy * b, 0.0);
    float above = (exp(-f * ha) - exp(-f * hb)) / (f * dy);
    return rho * (below + above);
}

void main() {
    vec4 scene = texture(u_input0, v_uv);
    o_color = scene;
    if (scene.a <= UNLIT_ALPHA) return;
    float stored = texture(u_input1, v_uv).r;
    vec3 ray = view_ray(post_ndc(v_uv));
    float stretch = length(ray);
    vec3 dir = ray / stretch;
    float far = stored > 0.0 ? view_depth_of(stored) * stretch : (u_haze_shape.w > 0.0 ? u_haze_shape.w : 1e7);
    float amount = haze_amount(u_camera_position.y, dir.y, u_haze_shape.z, far);
    float opacity = 1.0 - exp(-amount);
    vec3 tone = u_haze_color.rgb;
    if (u_haze_sun.w > 0.0) tone += u_haze_sun.rgb * pow(max(dot(dir, u_sun_dir.xyz), 0.0), u_haze_sun.w);
    o_color = vec4(mix(scene.rgb, tone, opacity), scene.a);
}
