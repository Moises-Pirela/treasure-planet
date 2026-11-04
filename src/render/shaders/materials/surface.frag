// The plain surface: the draw's colour, checkered by the metre so that walking
// and flying read as motion without textures. Material 0 of every frame.
// Parameters (src/render/materials.h: r_surface_params):
//   0: x checker contrast (0: none)
#include "lit.glsl"

Surface shade(Fragment f) {
    Surface s = surface_default(f);
    float contrast = material_param(0u).x;
    vec3 cell = floor(f.surface + 0.001);
    s.albedo *= 1.0 - contrast + contrast * mod(cell.x + cell.y + cell.z, 2.0);
    return s;
}
