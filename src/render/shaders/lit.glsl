// The lit pass's framework, which a material shader includes. A material shader
// is one file in shaders/materials/ that defines
//
//     Surface shade(Fragment f);
//
// what the surface is at a fragment: its albedo, what it emits, a tint, the
// normal to light by, its alpha (cut below ALPHA_CUT when the material says
// MATERIAL_CUTOUT), how much light comes through it from behind, and whether
// the sky's rim light falls on it. Everything else is here: the sun through the
// cascaded shadows, the painted light ramp and shadow tint, the point lights that
// reach the draw, the rim, the haze of the air between the surface and the eye.
// The same material shader compiles twice: with LIT_PASS into the lit pass, and
// with SHADOW_PASS into the shadow pass, where only a cutout's holes matter.

#include "common.glsl"
#include "noise.glsl"

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_surface;    // object space, in metres
layout(location = 2) in vec3 v_world;

struct Fragment {
    vec3 world;          // where, in the world
    vec3 normal;         // the mesh's, unit, turned to face the eye on a two-sided draw's back face
    vec3 surface;        // where, in the draw's own space, in metres
    vec3 view;           // unit, towards the eye
    vec3 color;          // the draw's colour
    float rim;           // 1 - dot(normal, view): 1 at the silhouette
    float facing_sun;    // dot(normal, towards the sun)
    bool two_sided;
};

struct Surface {
    vec3 albedo;
    vec3 emission;       // radiance of its own
    vec3 tint;           // multiplied into the lit result
    vec3 normal;         // to light by
    float alpha;         // a cutout material is cut where this is below ALPHA_CUT
    float translucency;  // 0..1: the sun through from behind (a cloud's underside)
    bool rim_light;      // the sky's light grazing the silhouette
};
#define ALPHA_CUT 0.5

// What a material starts from: the draw's colour, lit plainly.
Surface surface_default(Fragment f) {
    Surface s;
    s.albedo = f.color;
    s.emission = vec3(0.0);
    s.tint = vec3(1.0);
    s.normal = f.normal;
    s.alpha = 1.0;
    s.translucency = 0.0;
    s.rim_light = true;
    return s;
}

Surface shade(Fragment f);

Fragment fragment_here() {
    Fragment f;
    f.world = v_world;
    f.surface = v_surface;
    f.normal = normalize(v_normal);
    f.two_sided = (u_flags & FLAG_TWO_SIDED) != 0u;
    if (f.two_sided && !gl_FrontFacing) f.normal = -f.normal;
    f.view = normalize(u_camera_position.xyz - v_world);
    f.color = u_color.rgb;
    f.rim = 1.0 - max(dot(f.normal, f.view), 0.0);
    f.facing_sun = dot(f.normal, u_sun_dir.xyz);
    return f;
}

#ifdef SHADOW_PASS
// Depth only; a cutout material's holes let the sun through.
void main() {
#ifdef MATERIAL_CUTOUT
    if (shade(fragment_here()).alpha < ALPHA_CUT) discard;
#endif
}
#endif

#ifdef LIT_PASS
layout(location = 0) out vec4 o_color;
SAMPLER_AT(2) sampler2DArrayShadow u_shadow_map;

// How much of the sun reaches this point: 0 in shadow, 1 lit. Picks the first
// cascade the point is within, and fades to lit at the end of the last so the
// shadows' reach has no edge.
float sun_visibility(vec3 world, vec3 normal, float view_depth) {
    int cascades = int(u_shadow_params.w);
    if (cascades == 0) return 1.0;
    float reach = u_cascade_splits[cascades - 1];
    if (view_depth >= reach) return 1.0;
    int c = 0;
    for (int i = 0; i < cascades - 1; ++i) c += int(view_depth > u_cascade_splits[i]);
    // Look up a little off the surface along its normal, more where the light is
    // grazing, so a surface does not shadow itself in the texel steps of its own depth.
    float facing = clamp(dot(normal, u_sun_dir.xyz), 0.0, 1.0);
    float offset = u_cascade_texel[c] * u_shadow_params.y * (1.0 - facing);
    vec4 clip = u_shadow[c] * vec4(world + normal * offset, 1.0);
    vec2 uv = clip.xy * 0.5 + 0.5;
#ifdef VULKAN
    uv.y = 1.0 - uv.y;    // the map was rasterised y up, but Vulkan's row 0 is the top
#endif
    // Depth is reversed: nearer the sun is larger. The compare is "reference >=
    // stored", so lit where this point is at least as near as the nearest caster.
    float reference = clip.z + u_shadow_params.z;
    float texel = u_shadow_params.x;
    // The 3x3 filter's corners first: where they agree, so does the rest.
    float corners = texture(u_shadow_map, vec4(uv + vec2(-texel, -texel), float(c), reference))
                  + texture(u_shadow_map, vec4(uv + vec2(texel, -texel), float(c), reference))
                  + texture(u_shadow_map, vec4(uv + vec2(-texel, texel), float(c), reference))
                  + texture(u_shadow_map, vec4(uv + vec2(texel, texel), float(c), reference));
    float lit = corners * 0.25;
    if (corners > 0.0 && corners < 4.0) {
        lit = corners + texture(u_shadow_map, vec4(uv, float(c), reference))
            + texture(u_shadow_map, vec4(uv + vec2(0.0, -texel), float(c), reference))
            + texture(u_shadow_map, vec4(uv + vec2(0.0, texel), float(c), reference))
            + texture(u_shadow_map, vec4(uv + vec2(-texel, 0.0), float(c), reference))
            + texture(u_shadow_map, vec4(uv + vec2(texel, 0.0), float(c), reference));
        lit /= 9.0;
    }
    return mix(lit, 1.0, smoothstep(reach * 0.85, reach, view_depth));
}

// The light ramp: how much of a light reaches a surface facing it by `facing`
// (a cosine). Lambert, wrapped into the dark side, blended by the flatness with
// a flat band whose terminator is softness wide; banded into steps when asked.
// All zero is plain Lambert.
float ramp(float facing) {
    float softness = u_ramp.x, wrap = u_ramp.y, flatness = u_ramp.z, steps = u_ramp.w;
    float wrapped = clamp((facing + wrap) / (1.0 + wrap), 0.0, 1.0);
    float band = softness > 0.0 ? smoothstep(-softness, softness, facing + wrap) : wrapped;
    float t = mix(wrapped, band, flatness);
    if (steps > 0.0) t = floor(t * steps + 0.5) / steps;
    return t;
}

void main() {
    if ((u_flags & FLAG_UNLIT) != 0u) {
        o_color = vec4(u_color.rgb, UNLIT_ALPHA);
        return;
    }
    Fragment f = fragment_here();
    Surface s = shade(f);
#ifdef MATERIAL_CUTOUT
    if (s.alpha < ALPHA_CUT) discard;
#endif
    vec3 normal = normalize(s.normal);
    vec3 to_camera = u_camera_position.xyz - f.world;
    float view_depth = -dot(to_camera, u_camera_forward.xyz);
    float facing_sun = dot(normal, u_sun_dir.xyz);

    vec3 emission = s.emission;
    // The rim: the sky's light grazing the silhouette, more on what faces up.
    if (s.rim_light) emission += u_rim.rgb * u_rim.w * pow(f.rim, u_shadow_tint.w) * (normal.y * 0.5 + 0.5);

    float sky_facing = normal.y * 0.5 + 0.5;
    vec3 light = mix(u_ambient_ground.rgb, u_ambient_sky.rgb, sky_facing);
    float key = ramp(facing_sun);
    if (f.two_sided) {
        // A two-sided thing (a cloud layer) casts shadows but takes none, since a
        // single surface in the shadow map would shadow itself wherever the sun
        // grazes it; and the sun comes through it from behind by its translucency.
        key = max(key, ramp(-facing_sun) * s.translucency);
    } else if (key > 0.0 && (u_flags & FLAG_UNSHADOWED) == 0u) {
        key *= sun_visibility(f.world, normal, view_depth);
    }
    // A cloud layer's shadow, by the layer itself rather than a shadow map: where
    // the way up to the sun leaves the layer's sphere, the cover there. Not on a
    // two-sided draw, which is such a layer, and would shadow itself.
    if (key > 0.0 && !f.two_sided) {
        for (uint i = 0u; i < u_counts.z; ++i) {
            vec4 sphere = u_cloud_sphere[i];
            if (sphere.w <= 0.0) continue;
            vec3 from = f.world - sphere.xyz;
            float inside = dot(from, from);
            if (inside >= sphere.w * sphere.w) continue;    // above the clouds
            float b = dot(from, u_sun_dir.xyz);
            float t = -b + sqrt(max(b * b - inside + sphere.w * sphere.w, 0.0));
            vec4 layer = u_cloud_layer[i];
            float cover = cloud_cover_at(normalize(from + u_sun_dir.xyz * t), layer.x, layer.y, layer.z, uint(layer.w), u_cloud_turn[i].x, 0.0);
            key *= 1.0 - smoothstep(ALPHA_CUT - 0.15, ALPHA_CUT + 0.15, cover);
        }
    }
    light += u_sun_color.rgb * key;
    // Only the lights whose reach touches this draw, a bit each.
    for (uint mask = u_lights; mask != 0u; mask &= mask - 1u) {
        uint i = uint(findLSB(mask));
        vec3 to_light = u_light_position[i].xyz - f.world;
        float radius = u_light_position[i].w;
        float d2 = dot(to_light, to_light);
        float window = clamp(1.0 - d2 * d2 / (radius * radius * radius * radius), 0.0, 1.0);
        float falloff = window * window / (d2 + 1.0);
        float facing = ramp(dot(normal, to_light * inversesqrt(max(d2, 1e-6))));
        light += u_light_color[i].rgb * falloff * facing;
    }
    // Where the key light does not reach, the shadow's tint.
    light *= mix(u_shadow_tint.rgb, vec3(1.0), key);
    vec3 radiance = s.albedo * light * s.tint + emission;
    // Aerial perspective: what is seen through an atmosphere fades into its haze by
    // the length of the way through its air, from orbit as much as from inside; and
    // the haze about the camera (a cloud deck) by the whole distance.
    for (uint i = 0u; i < u_counts.z; ++i) {
        float way = air_path(u_camera_position.xyz, f.world, u_air_sphere[i]);
        if (way > 0.0) radiance = mix(radiance, u_air_haze[i].rgb, 1.0 - exp(-way * u_air_haze[i].w));
    }
    if (u_haze.w > 0.0) radiance = mix(radiance, u_haze.rgb, 1.0 - exp(-length(to_camera) * u_haze.w));
    o_color = vec4(radiance, 1.0);
}
#endif
