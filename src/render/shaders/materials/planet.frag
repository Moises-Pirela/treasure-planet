// A world on its sphere. With land (relief > 0) the mesh arrives already shaped
// by it, so the surface is coloured by how high it stands: sea, land, snow above
// the line, the flat about the top the landing pad is set into. Without land it
// is a gas giant: three tones mixed by latitude bands bent by noise. Either way
// clouds may be painted over it, the atmosphere glows at the rim and brighter on
// the day side, the night side glows where the noise is high (lava, cities), and
// the terminator is tinted. The air's effects go by the sphere's normal, not the
// land's: a hillside turned from the eye is not the limb.
// Parameters (src/render/materials.h: r_planet_params):
//   0: base rgb                     1: second rgb, w bands
//   2: third rgb, w swirl           3: clouds rgb, w cover
//   4: atmosphere rgb, w strength   5: night rgb, w cover
//   6: terminator rgb, w width      7: detail, contrast, atmosphere power, seed
//   8: continents, relief, sea level, mountains
//   9: snow line, shore, pad radius
#include "lit.glsl"
#include "noise.glsl"

Surface shade(Fragment f) {
    Surface s = surface_default(f);
    s.rim_light = false;
    vec4 base = material_param(0u), second = material_param(1u), third = material_param(2u);
    vec4 clouds = material_param(3u), atmosphere = material_param(4u), night = material_param(5u);
    vec4 terminator = material_param(6u), params = material_param(7u), land = material_param(8u), land2 = material_param(9u);
    float detail = params.x, contrast = params.y, relief = land.y;
    uint seed = uint(params.w);
    float scale = length(u_model[0].xyz);
    vec3 dir = normalize(f.surface);
    vec3 sphere_normal = normalize(mat3(u_model) * dir);
    float rim = 1.0 - max(dot(sphere_normal, f.view), 0.0);
    float facing_sun = dot(sphere_normal, u_sun_dir.xyz);
    float footprint = length(fwidth(dir));
    float cells = footprint * detail;              // noise cells per pixel, at the first octave
    // The noise for what this world has: a gas giant's spots or anything's night
    // lights, and painted clouds, which a world with a cloud layer does not paint.
    float spots = 0.0, cloud = 0.0;
    if (relief <= 0.0 || night.w > 0.0) spots = fbm(dir * detail * 2.0, seed + 101u, 3u, cells * 2.0);
    if (clouds.w > 0.0) cloud = fbm(dir * detail * 1.5, seed + 202u, 3u, cells * 1.5);

    vec3 albedo;
    if (relief > 0.0) {
        float elevation = length(f.surface) - scale;
        float pad = 1.0 - smoothstep(land2.z, land2.z * 1.6, acos(clamp(dir.y, -1.0, 1.0)) * scale);
        float ground = max(smoothstep(0.0, max(land2.y * relief, 0.01), elevation), pad);
        albedo = mix(base.rgb, second.rgb, ground);
        albedo = mix(albedo, third.rgb, smoothstep(land2.x - 0.1, land2.x + 0.1, elevation / relief));
    } else {
        float n = fbm(dir * detail, seed, 4u, cells);
        float latitude = dir.y + (n - 0.5) * third.w;
        float level = second.w > 0.0 ? 0.5 + 0.5 * sin(latitude * second.w * PI) : n;
        albedo = mix(base.rgb, second.rgb, clamp((level - 0.5) * contrast + 0.5, 0.0, 1.0));
        albedo = mix(albedo, third.rgb, clamp((spots - 0.75) * contrast * 2.0 + 0.5, 0.0, 1.0));
    }
    float cover = clamp((cloud - 1.0 + clouds.w) * contrast, 0.0, 1.0);
    s.albedo = mix(albedo, clouds.rgb, cover);
    float day = smoothstep(-1.0, 1.0, facing_sun);
    s.emission = atmosphere.rgb * atmosphere.w * pow(rim, params.z) * day;
    float lights = clamp((spots - 1.0 + night.w) * contrast, 0.0, 1.0);
    s.emission += night.rgb * lights * (1.0 - cover) * (1.0 - day);
    float width = terminator.w;
    float line = width > 0.0 ? exp(-facing_sun * facing_sun / (width * width)) : 0.0;
    s.tint = mix(vec3(1.0), terminator.rgb, line);
    return s;
}
