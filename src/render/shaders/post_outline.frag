// A post step: the outline. Lines where the depth breaks (a silhouette) or the
// surface turns (a crease), from the depth buffer alone: a plane's stored depth
// (near over the view depth) is linear across the screen, so its second
// difference is nothing on a surface and the size of the break at an edge; the
// normals are rebuilt from the depths about each pixel, taking the nearer
// neighbour each way so that an edge does not smear into them. The lines go on
// in the line's colour by the strength, and fade with distance; an unlit pixel
// (the overlay) is left as it is, and makes no line on what is next to it.
// Inputs: the scene, the depth.
#define POST_STEP
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;

bool inside(ivec2 p) {
    ivec2 size = textureSize(u_input1, 0);
    return p.x >= 0 && p.y >= 0 && p.x < size.x && p.y < size.y;
}
float stored_at(ivec2 p) {
    return texelFetch(u_input1, clamp(p, ivec2(0), textureSize(u_input1, 0) - 1), 0).r;
}
// The stored depth at `p`, or, past the picture's edge, what a flat surface
// through `here` and the pixel on the other side would have there, so the edge
// of the picture is not an edge.
float stored_beyond(ivec2 p, ivec2 other, float here) {
    return inside(p) ? stored_at(p) : 2.0 * here - stored_at(other);
}
// Where the pixel's surface is, relative to the eye; w: its stored depth (0: the sky).
vec4 position_at(ivec2 p) {
    float stored = stored_at(p);
    vec3 ray = view_ray(post_ndc((vec2(p) + 0.5) * u_texel.xy));
    return vec4(ray * view_depth_of(stored), stored);
}
// The normal of the surface just past `p` on one side: from the slope between
// the two pixels beyond it that way (so a crease at `p` is not crossed) and the
// slope across it the other way.
vec3 normal_beyond(ivec2 p, ivec2 along, ivec2 across) {
    vec3 slope = position_at(p + 2 * along).xyz - position_at(p + along).xyz;
    vec3 other = position_at(p + along + across).xyz - position_at(p + along - across).xyz;
    return normalize(cross(slope, other));
}
bool unlit_at(ivec2 p) {
    return texelFetch(u_input0, clamp(p, ivec2(0), textureSize(u_input0, 0) - 1), 0).a <= UNLIT_ALPHA;
}

void main() {
    vec4 scene = texture(u_input0, v_uv);
    o_color = scene;
    if (scene.a <= UNLIT_ALPHA) return;
    ivec2 p = ivec2(floor(v_uv / u_texel.xy));
    float here = stored_at(p);
    if (here <= 0.0) return;    // the sky takes no line
    int w = int(max(u_outline.y + 0.5, 1.0));
    ivec2 x = ivec2(w, 0), y = ivec2(0, w);
    if (unlit_at(p - x) || unlit_at(p + x) || unlit_at(p - y) || unlit_at(p + y)) return;

    // The break in depth, as a fraction of the depth here.
    float l = stored_beyond(p - x, p + x, here), r = stored_beyond(p + x, p - x, here);
    float d = stored_beyond(p - y, p + y, here), u = stored_beyond(p + y, p - y, here);
    float breaks = (abs(l + r - 2.0 * here) + abs(u + d - 2.0 * here)) / here;
    float threshold = u_outline.z;
    float edge = smoothstep(threshold, threshold * 2.0, breaks);
    // The turn of the surface, as 1 - the cosine between the normals.
    if (u_outline.w > 0.0 && edge < 1.0) {
        // The surfaces either side of the pixel each way, so a crease under it is
        // lined whichever of them the pixel itself lands on. (The cross products
        // are taken in the same order, for the same handedness.)
        float turn = 0.0;
        if (inside(p - 2 * x) && inside(p + 2 * x) && inside(p - y) && inside(p + y)) {
            vec3 l = -normal_beyond(p, -x, y), r = normal_beyond(p, x, y);
            turn = max(turn, 1.0 - dot(l, r));
        }
        if (inside(p - 2 * y) && inside(p + 2 * y) && inside(p - x) && inside(p + x)) {
            vec3 d = normal_beyond(p, -y, x), u = -normal_beyond(p, y, x);
            turn = max(turn, 1.0 - dot(d, u));
        }
        edge = max(edge, smoothstep(u_outline.w, u_outline.w * 2.0, turn));
    }
    float fade = u_outline_color.w > 0.0 ? 1.0 - smoothstep(u_outline_color.w * 0.5, u_outline_color.w, view_depth_of(here)) : 1.0;
    o_color = vec4(mix(scene.rgb, u_outline_color.rgb, edge * u_outline.x * fade), scene.a);
}
