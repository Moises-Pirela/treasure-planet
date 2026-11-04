// A post step: the lens flare, from the picture itself. What is bright, mirrored
// through the centre, repeated along the line to the centre as ghosts with their
// colours fringed, and a halo ring; both fade towards the edges.
#define POST_STEP
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;
INPUT_AT(2) sampler2D u_input2;

// The lens flare, from the picture itself: what is bright, mirrored through the
// centre, repeated along the line to the centre as ghosts with their colours
// fringed, and a halo ring; both fade towards the edges.
vec3 bright(vec2 uv) { return max(texture(u_input0, uv).rgb - u_flare_halo.w, 0.0); }
void main() {
    vec2 centre = vec2(0.5);
    vec2 uv = 1.0 - v_uv;
    vec2 ghost_step = (centre - uv) * u_flare.z;
    vec2 direction = ghost_step / max(length(ghost_step), 1e-5);
    float reach = length(centre);
    vec3 result = vec3(0.0);
    int ghosts = int(u_flare.y);
    vec2 fringe = direction * u_flare_halo.z;
    for (int i = 0; i < ghosts; ++i) {
        vec2 at = uv + ghost_step * float(i);
        float weight = pow(max(1.0 - length(centre - at) / reach, 0.0), u_flare.w);
        result += vec3(bright(at - fringe).r, bright(at).g, bright(at + fringe).b) * weight;
    }
    vec2 halo_at = uv + direction * u_flare_halo.x;
    float halo_weight = pow(max(1.0 - length(centre - halo_at) / reach, 0.0), u_flare.w);
    result += bright(halo_at) * halo_weight * u_flare_halo.y;
    o_color = vec4(result, 1.0);
}
