// A post step: FXAA 3.11 (Lottes), in the form Simon Rodriguez set out. Find an
// edge from the luma in the input's alpha, walk along it both ways to find its
// ends, and blend towards the other side by how far along the edge this pixel
// is; plus a blend for sub-pixel aliasing.
#define POST_STEP
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;
INPUT_AT(2) sampler2D u_input2;

// FXAA 3.11 (Lottes), in the form Simon Rodriguez set out: find an edge from the
// luma in the input's alpha, walk along it both ways to find its ends, and blend
// towards the other side by how far along the edge this pixel is; plus a blend
// for sub-pixel aliasing.
float luma_at(vec2 uv) { return texture(u_input0, uv).a; }
void main() {
    vec4 centre = texture(u_input0, v_uv);
    float lc = centre.a;
    float ld = luma_at(v_uv + vec2(0.0, -u_texel.y)), lu = luma_at(v_uv + vec2(0.0, u_texel.y));
    float ll = luma_at(v_uv + vec2(-u_texel.x, 0.0)), lr = luma_at(v_uv + vec2(u_texel.x, 0.0));
    float lmin = min(lc, min(min(ld, lu), min(ll, lr)));
    float lmax = max(lc, max(max(ld, lu), max(ll, lr)));
    float range = lmax - lmin;
    if (range < max(u_fxaa.x, lmax * u_fxaa.y)) {
        o_color = vec4(centre.rgb, 1.0);
        return;
    }
    float ldl = luma_at(v_uv + vec2(-u_texel.x, -u_texel.y)), lur = luma_at(v_uv + vec2(u_texel.x, u_texel.y));
    float lul = luma_at(v_uv + vec2(-u_texel.x, u_texel.y)), ldr = luma_at(v_uv + vec2(u_texel.x, -u_texel.y));
    float ldu = ld + lu, llr = ll + lr;
    float lleft = ldl + lul, ldown = ldl + ldr, lright = ldr + lur, lup = lul + lur;
    float edge_h = abs(-2.0 * ll + lleft) + abs(-2.0 * lc + ldu) * 2.0 + abs(-2.0 * lr + lright);
    float edge_v = abs(-2.0 * lu + lup) + abs(-2.0 * lc + llr) * 2.0 + abs(-2.0 * ld + ldown);
    bool horizontal = edge_h >= edge_v;

    // Which side of the edge this pixel is on, and the luma at the edge.
    float l1 = horizontal ? ld : ll, l2 = horizontal ? lu : lr;
    float g1 = l1 - lc, g2 = l2 - lc;
    bool steepest1 = abs(g1) >= abs(g2);
    float gradient_scaled = 0.25 * max(abs(g1), abs(g2));
    float step_length = horizontal ? u_texel.y : u_texel.x;
    float luma_local = 0.5 * ((steepest1 ? l1 : l2) + lc);
    if (steepest1) step_length = -step_length;
    vec2 uv = v_uv;
    if (horizontal) uv.y += step_length * 0.5; else uv.x += step_length * 0.5;

    // Walk both ways until the luma departs from the edge's.
    vec2 offset = horizontal ? vec2(u_texel.x, 0.0) : vec2(0.0, u_texel.y);
    vec2 uv1 = uv - offset, uv2 = uv + offset;
    float end1 = luma_at(uv1) - luma_local, end2 = luma_at(uv2) - luma_local;
    bool reached1 = abs(end1) >= gradient_scaled, reached2 = abs(end2) >= gradient_scaled;
    const float STRIDE[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    int iterations = int(u_fxaa.w);
    for (int i = 2; i < iterations; ++i) {
        if (reached1 && reached2) break;
        if (!reached1) {
            uv1 -= offset * STRIDE[i];
            end1 = luma_at(uv1) - luma_local;
        }
        if (!reached2) {
            uv2 += offset * STRIDE[i];
            end2 = luma_at(uv2) - luma_local;
        }
        reached1 = abs(end1) >= gradient_scaled;
        reached2 = abs(end2) >= gradient_scaled;
    }
    float distance1 = horizontal ? v_uv.x - uv1.x : v_uv.y - uv1.y;
    float distance2 = horizontal ? uv2.x - v_uv.x : uv2.y - v_uv.y;
    bool nearer1 = distance1 < distance2;
    float pixel_offset = -min(distance1, distance2) / (distance1 + distance2) + 0.5;
    bool centre_smaller = lc < luma_local;
    bool correct_variation = ((nearer1 ? end1 : end2) < 0.0) != centre_smaller;
    float final_offset = correct_variation ? pixel_offset : 0.0;

    // Sub-pixel aliasing: blend by how far this pixel is from its neighbourhood's average.
    float average = (1.0 / 12.0) * (2.0 * (ldu + llr) + lleft + lright);
    float sub = clamp(abs(average - lc) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    final_offset = max(final_offset, sub * sub * u_fxaa.z);

    vec2 final_uv = v_uv;
    if (horizontal) final_uv.y += final_offset * step_length; else final_uv.x += final_offset * step_length;
    o_color = vec4(texture(u_input0, final_uv).rgb, 1.0);
}
