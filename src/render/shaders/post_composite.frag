// A post step: the composite. The scene with the bloom and the flare added, an
// atmosphere's entry, the colour channels pulled apart radially, exposure and
// white balance, the tone curve, the grading, the vignette, gamma. Luma goes to
// alpha when anti-aliasing follows.
#define POST_STEP
#include "common.glsl"
#include "noise.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;
INPUT_AT(2) sampler2D u_input2;

// The composite: the scene with the bloom and the flare added, the colour
// channels pulled apart radially, exposure and white balance, the tone curve, the
// grading, the vignette, gamma. Luma goes to alpha when anti-aliasing follows.
vec3 hable(vec3 x) {
    return ((x * (0.15 * x + 0.05) + 0.004) / (x * (0.15 * x + 0.5) + 0.06)) - 0.0667;
}
vec3 tone_curve(vec3 x) {
    int curve = int(u_tonemap.y);
    float white = u_tonemap.z;
    if (curve == 1) return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    if (curve == 2) return clamp((white > 0.0 ? x * (1.0 + x / (white * white)) : x) / (1.0 + x), 0.0, 1.0);
    if (curve == 3) return clamp(hable(x) / hable(vec3(white > 0.0 ? white : 11.2)), 0.0, 1.0);
    return clamp(x, 0.0, 1.0);
}
void main() {
    vec4 scene = texture(u_input0, v_uv);
    if (scene.a <= UNLIT_ALPHA) {
        vec3 as_given = pow(scene.rgb, vec3(1.0 / u_tonemap.w));
        o_color = vec4(as_given, u_params.x > 0.0 ? dot(as_given, vec3(0.299, 0.587, 0.114)) : 1.0);
        return;
    }
    // The aberration pulls the scene's channels apart; the bloom and the flare are
    // blurred anyway, so they are taken whole.
    vec3 hdr = scene.rgb;
    if (u_aberration.x > 0.0) {
        vec2 radial = v_uv - 0.5;
        vec2 shift = radial * (2.0 * dot(radial, radial)) * u_aberration.x;
        hdr.r = texture(u_input0, v_uv - shift).r;
        hdr.b = texture(u_input0, v_uv + shift).b;
    }
    // Entering an atmosphere at speed: the picture streaks towards where the camera
    // is going, and plasma streaks glow around that point, more towards the edges.
    if (u_entry.z > 0.0) {
        vec2 from = v_uv - u_entry.xy;
        int samples = int(u_entry_glow.w);
        if (samples > 1) {
            vec2 step = -from * u_entry.w * u_entry.z / float(samples);
            vec3 sum = vec3(0.0);
            for (int i = 0; i < samples; ++i) sum += texture(u_input0, v_uv + step * float(i)).rgb;
            hdr = mix(hdr, sum / float(samples), u_entry.z);
        }
        // Sparse streaks: noise around the point of travel (on a circle, so there is
        // no seam) and along the radius, streaming outwards with time, the faint
        // ones cut away, the centre left clear.
        float radius = length(from);
        vec2 around = normalize(from) * u_entry_streaks.x;
        float streak = value_noise(vec3(around, (radius * 2.0 - u_entry_streaks.y * u_entry_streaks.w) * u_entry_streaks.z), 7u);
        streak = smoothstep(0.55, 0.85, streak);
        float edge = smoothstep(0.15, 0.7, radius);
        hdr += u_entry_glow.rgb * u_entry.z * edge * streak;
    }
    if (u_params.y > 0.0) hdr += texture(u_input1, v_uv).rgb * u_bloom.x;
    if (u_params.z > 0.0) hdr += texture(u_input2, v_uv).rgb * u_flare.x;
    vec3 color = tone_curve(hdr * u_tonemap.x * u_balance.rgb);
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = luminance + (color - luminance) * u_balance.w;
    color = (color - 0.18) * u_grade.x + 0.18;                  // about middle grey
    color = clamp(color * u_gain.rgb + u_lift.rgb * (1.0 - color), 0.0, 1.0);
    if (u_gamma.rgb != vec3(1.0)) color = pow(color, u_gamma.rgb);
    if (u_vignette.x > 0.0) {
        float d = length(v_uv - 0.5) / length(vec2(0.5));
        color *= 1.0 - u_vignette.x * smoothstep(u_vignette.y, u_vignette.y + u_vignette.z, d);
    }
    color = pow(color, vec3(1.0 / u_tonemap.w));
    o_color = vec4(color, u_params.x > 0.0 ? dot(color, vec3(0.299, 0.587, 0.114)) : 1.0);
}
