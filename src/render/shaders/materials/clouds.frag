// A layer of cloud on a sphere: its colour where the noise, turned about the axis
// by the drift over time, passes the cover; a hole elsewhere, the edge cut at
// ALPHA_CUT as sharply as the contrast says. Drawn two-sided, lit through by its
// translucency; the shadow pass cuts the same holes.
// Parameters (src/render/materials.h: r_cloud_params):
//   0: colour rgb, w cover
//   1: detail, contrast, seed, drift
//   2: x translucency
#define MATERIAL_CUTOUT
#include "lit.glsl"
#include "noise.glsl"

Surface shade(Fragment f) {
    Surface s = surface_default(f);
    vec4 colour = material_param(0u), params = material_param(1u);
    vec3 dir = normalize(f.surface);
    s.alpha = cloud_cover_at(dir, colour.w, params.x, params.y, uint(params.z), params.w * u_sky.w, length(fwidth(dir)) * params.x);
    s.albedo = colour.rgb;
    s.translucency = material_param(2u).x;
    s.rim_light = false;
    return s;
}
