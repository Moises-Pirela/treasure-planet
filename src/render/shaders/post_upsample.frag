// A post step: bloom, up. The coarser result through a 3x3 tent of the given
// radius (four bilinear taps at half-texel offsets, the same 1-2-1 kernel for a
// quarter of the fetches), blended with this level's downsample by the scatter.
#define POST_STEP
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;
INPUT_AT(2) sampler2D u_input2;

// Bloom, up: the coarser result through a 3x3 tent of the given radius, blended
// with this level's downsample by the scatter. The tent is four bilinear taps at
// half-texel offsets, which is the same 1-2-1 kernel for a quarter of the fetches.
vec3 tap(vec2 offset) { return texture(u_input0, v_uv + offset * u_texel.xy * u_params.x).rgb; }
void main() {
    vec3 coarser = (tap(vec2(-0.5, 0.5)) + tap(vec2(0.5, 0.5)) + tap(vec2(-0.5, -0.5)) + tap(vec2(0.5, -0.5))) * 0.25;
    vec3 finer = texture(u_input1, v_uv).rgb;
    o_color = vec4(mix(finer, coarser, u_params.y), 1.0);
}
