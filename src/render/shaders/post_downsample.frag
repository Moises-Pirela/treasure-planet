// A post step: bloom, down. Jimenez's 13 taps, five overlapping quads with the
// middle one weighted four times a corner one. The first mip also thresholds
// (with a soft knee), drops unlit pixels, and weights each quad down by its
// brightness (Karis), so one blazing pixel does not flicker as a whole blob.
#define POST_STEP
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;
INPUT_AT(2) sampler2D u_input2;

// Bloom, down: one mip to the next, half the size. Jimenez's 13 taps, five
// overlapping quads with the middle one weighted four times a corner one. The
// first mip also thresholds (with a soft knee), drops unlit pixels, and weights
// each quad down by its brightness (Karis), so one blazing pixel does not
// flicker as a whole blob.
bool first() { return u_params.x > 0.0; }
vec3 tap(vec2 offset) {
    vec4 s = texture(u_input0, v_uv + offset * u_texel.xy);
    return s.rgb * (first() ? s.a : 1.0);
}
float quad_weight(vec3 c) { return first() ? 1.0 / (1.0 + max(c.r, max(c.g, c.b))) : 1.0; }
void main() {
    vec3 a = tap(vec2(-2, 2)), b = tap(vec2(0, 2)), c = tap(vec2(2, 2));
    vec3 d = tap(vec2(-2, 0)), e = tap(vec2(0, 0)), f = tap(vec2(2, 0));
    vec3 g = tap(vec2(-2, -2)), h = tap(vec2(0, -2)), i = tap(vec2(2, -2));
    vec3 j = tap(vec2(-1, 1)), k = tap(vec2(1, 1)), l = tap(vec2(-1, -1)), m = tap(vec2(1, -1));
    vec3 quads[5] = vec3[5]((a + b + d + e) * 0.25, (b + c + e + f) * 0.25, (d + e + g + h) * 0.25,
                            (e + f + h + i) * 0.25, (j + k + l + m) * 0.25);
    float shares[5] = float[5](0.125, 0.125, 0.125, 0.125, 0.5);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int n = 0; n < 5; ++n) {
        float w = shares[n] * quad_weight(quads[n]);
        sum += quads[n] * w;
        total += w;
    }
    vec3 color = sum / total;
    if (first()) {
        float brightness = max(color.r, max(color.g, color.b));
        float knee = u_bloom.z;
        float soft = clamp(brightness - u_bloom.y + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-5);
        color *= max(soft, brightness - u_bloom.y) / max(brightness, 1e-5);
    }
    o_color = vec4(color, 1.0);
}
