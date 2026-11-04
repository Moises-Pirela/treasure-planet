// Every shader of both backends, from one source. Exactly one of VERTEX or
// FRAGMENT is defined, and exactly one program: PROGRAM_MESH, PROGRAM_SHADOW or
// PROGRAM_SKY for the scene, PROGRAM_POST for the post steps' vertex stage, and
// PROGRAM_DOWNSAMPLE, PROGRAM_UPSAMPLE, PROGRAM_FLARE, PROGRAM_COMPOSITE or
// PROGRAM_FXAA for their fragment stages.
// There is no #version line: the GL backend prepends it at runtime, glslc gets
// -std=450core. VULKAN is defined by the compiler for the SPIR-V build.
// After editing, run tools/compile_shaders.ps1 or .sh (needs glslc from the Vulkan
// SDK); the Vulkan backend refuses SPIR-V built from another version of this file.
//
// The blocks below match r_gpu_frame, r_gpu_lights, r_gpu_post, r_push and
// r_post_push in src/render/backend.h. Nothing in here is a tuning value: every
// number that shapes the picture arrives in a block.

#define SHADOW_CASCADES 4
#define MAX_POINT_LIGHTS 32
#define MAX_MATERIALS 16
#define MAX_AIRS 4
#define FLAG_UNLIT 1u
#define FLAG_EMISSIVE 4u
#define MATERIAL_PLANET 1u
#define PI 3.14159265

#ifdef VULKAN
#define UNIFORM_BLOCK(slot) layout(set = 0, binding = slot, std140) uniform
#define SAMPLER_AT(slot) layout(set = 0, binding = slot) uniform
#define INPUT_AT(i) layout(set = 1 + i, binding = 0) uniform
#define VERTEX_INDEX gl_VertexIndex
#else
#define UNIFORM_BLOCK(slot) layout(binding = slot, std140) uniform
#define SAMPLER_AT(slot) layout(binding = slot) uniform
#define INPUT_AT(i) layout(binding = 4 + i) uniform
#define VERTEX_INDEX gl_VertexID
#endif

UNIFORM_BLOCK(0) Frame {
    mat4 u_view_proj;
    mat4 u_shadow[SHADOW_CASCADES];   // world -> a cascade's clip space (orthographic, w = 1)
    vec4 u_camera_position;
    vec4 u_camera_right;              // w: tan of half the horizontal field of view
    vec4 u_camera_up;                 // w: tan of half the vertical one
    vec4 u_camera_forward;
    vec4 u_sun_dir;                   // xyz: towards the sun; w: cos of the disc's angular radius
    vec4 u_sun_color;                 // w: the glow around the disc
    vec4 u_ambient_sky;
    vec4 u_ambient_ground;
    vec4 u_sky_zenith;
    vec4 u_sky_horizon;
    vec4 u_sky_ground;
    vec4 u_cascade_splits;            // view distance at which each cascade ends
    vec4 u_cascade_texel;             // metres per shadow texel, per cascade
    vec4 u_shadow_params;             // x: 1 / resolution, y: normal offset in texels, z: depth bias, w: cascades in use
    vec4 u_stars;                     // x: density, y: brightness, z: cells per cube face
    vec4 u_ramp;                      // x: softness, y: wrap, z: flatness, w: steps
    vec4 u_shadow_tint;               // rgb; (1, 1, 1) for none; w: rim power
    vec4 u_rim;                       // rgb: colour, w: strength
    vec4 u_haze;                      // rgb: the haze about the camera (the cloud deck's), w: its density (0: none)
    vec4 u_air_zenith;                // rgb: the atmosphere's sky overhead, w: the immersion
    vec4 u_air_horizon;               // rgb: its sky at the horizon, w: its sun glow times the immersion
    vec4 u_air;                       // x: star fade times the immersion
    vec4 u_air_sphere[MAX_AIRS];      // the atmospheres' shells: xyz centre, w radius to the top of the air
    vec4 u_air_haze[MAX_AIRS];        // rgb: each one's haze, w: its density per metre of air
    uvec4 u_counts;                   // x: point lights, y: star seed, z: atmospheres
};

UNIFORM_BLOCK(1) Lights {
    vec4 u_light_position[MAX_POINT_LIGHTS];   // w: radius
    vec4 u_light_color[MAX_POINT_LIGHTS];
};

UNIFORM_BLOCK(3) Post {
    vec4 u_tonemap;                   // x: exposure, y: curve, z: white point (0: the curve's own), w: output gamma
    vec4 u_balance;                   // rgb: white balance, w: saturation
    vec4 u_grade;                     // x: contrast
    vec4 u_lift;
    vec4 u_gamma;                     // as the exponent
    vec4 u_gain;
    vec4 u_vignette;                  // x: strength, y: radius, z: softness
    vec4 u_bloom;                     // x: intensity, y: threshold, z: knee
    vec4 u_flare;                     // x: intensity, y: ghosts, z: dispersal, w: falloff
    vec4 u_flare_halo;                // x: width, y: intensity, z: chromatic, w: threshold
    vec4 u_aberration;                // x: strength
    vec4 u_fxaa;                      // x: threshold min, y: threshold, z: subpixel, w: iterations
    vec4 u_entry;                     // xy: where the camera is going, in uv; z: strength (0: none); w: blur length
    vec4 u_entry_glow;                // rgb: the plasma's colour times the glow; w: samples
    vec4 u_entry_streaks;             // x: streaks around the point of travel, y: time, z: streak length, w: streak speed
};

struct Material {
    vec4 base;                        // w: kind
    vec4 second;                      // w: bands
    vec4 third;                       // w: swirl
    vec4 clouds;                      // w: cover
    vec4 atmosphere;                  // w: strength
    vec4 night;                       // w: cover
    vec4 terminator;                  // rgb: tint, w: width
    vec4 params;                      // x: detail, y: contrast, z: atmosphere power, w: seed
    vec4 land;                        // x: continents, y: relief, z: sea level, w: mountains
    vec4 land2;                       // x: snow line, y: shore, z: pad radius
};
UNIFORM_BLOCK(5) Materials {
    Material u_materials[MAX_MATERIALS];
};

#if defined(PROGRAM_MESH) || defined(PROGRAM_SHADOW) || defined(PROGRAM_SKY)
#ifdef VULKAN
layout(push_constant) uniform Draw {
    mat4 u_model;
    vec4 u_color;
    uint u_flags;
    uint u_material;
    uint u_lights;                    // a bit per point light that reaches this draw
    uint u_cascade;
};
#else
layout(location = 0) uniform mat4 u_model;
layout(location = 4) uniform vec4 u_color;
layout(location = 5) uniform uint u_flags;
layout(location = 6) uniform uint u_cascade;
layout(location = 7) uniform uint u_material;
layout(location = 8) uniform uint u_lights;
#endif
#else
#ifdef VULKAN
layout(push_constant) uniform Step {
    vec4 u_texel;                     // xy: one texel of the first input, zw: one of the target, in uv
    vec4 u_params;                    // the step's
};
#else
layout(location = 0) uniform vec4 u_texel;
layout(location = 1) uniform vec4 u_params;
#endif
#endif

// The scene target is floating point: lit pixels hold linear radiance with alpha
// 1, an unlit draw holds the linear colour it wants shown with alpha 0, and the
// composite passes those through with nothing but the display's gamma.
#define UNLIT_ALPHA 0.0

// Integer hashing, so the stars and the planets are the same on every GPU and in
// every capture.
uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Value noise from the integer hash (one hash per lattice corner, the
// coordinates folded in by multiplication with large odd constants), in 0..1.
float lattice(uvec3 at, uint seed) {
    return float(hash(at.x * 0x8da6b343u ^ at.y * 0xd8163841u ^ at.z * 0xcb1ab31fu ^ seed)) / 4294967295.0;
}
float value_noise(vec3 p, uint seed) {
    vec3 cell = floor(p);
    vec3 f = p - cell;
    f = f * f * (3.0 - 2.0 * f);
    uvec3 c = uvec3(ivec3(cell));
    vec4 x = mix(vec4(lattice(c, seed), lattice(c + uvec3(0, 1, 0), seed), lattice(c + uvec3(0, 0, 1), seed), lattice(c + uvec3(0, 1, 1), seed)),
                 vec4(lattice(c + uvec3(1, 0, 0), seed), lattice(c + uvec3(1, 1, 0), seed), lattice(c + uvec3(1, 0, 1), seed), lattice(c + uvec3(1, 1, 1), seed)),
                 f.x);
    vec2 y = mix(x.xz, x.yw, f.y);
    return mix(y.x, y.y, f.z);
}
// Octaves of it. `cells` is how many of the first octave's cells a pixel spans:
// octaves finer than a pixel are left out, which is cheaper and does not alias
// (0: every octave).
float fbm(vec3 p, uint seed, uint octaves, float cells) {
    float sum = 0.0, amplitude = 0.5, total = 0.0;
    for (uint octave = 0u; octave < octaves; ++octave) {
        if (cells > 1.0) break;
        sum += value_noise(p, seed + octave) * amplitude;
        total += amplitude;
        p *= 2.0;
        cells *= 2.0;
        amplitude *= 0.5;
    }
    return total > 0.0 ? sum / total : 0.5;
}
// Ridged: the noise folded about its middle, so that it peaks along lines: mountains.
float ridged(vec3 p, uint seed, uint octaves, float cells) {
    float sum = 0.0, amplitude = 0.5, total = 0.0;
    for (uint octave = 0u; octave < octaves; ++octave) {
        if (cells > 1.0) break;
        sum += (1.0 - abs(2.0 * value_noise(p, seed + octave) - 1.0)) * amplitude;
        total += amplitude;
        p *= 2.0;
        cells *= 2.0;
        amplitude *= 0.5;
    }
    return total > 0.0 ? sum / total : 0.5;
}

// A planet's land at a point of its unit sphere: its height in metres above the
// sphere (0 at sea), how much of the point is land (0 sea, 1 land, the coast
// between) and the continent noise it came from. Continents where the noise passes
// the sea level, ridged mountains on them, a flat about the top for the landing
// pad. `scale` is the planet's radius in metres.
void terrain(Material m, vec3 dir, float scale, float cells, out float height, out float land, out float continent) {
    float continents = m.land.x, relief = m.land.y, sea = m.land.z, mountains = m.land.w, shore = m.land2.y;
    uint seed = uint(m.params.w);
    continent = fbm(dir * continents, seed + 303u, 4u, cells);
    land = smoothstep(sea - shore, sea + shore, continent);
    float lowland = clamp((continent - sea) / max(1.0 - sea, 1e-3), 0.0, 1.0);
    float ridge = ridged(dir * continents * 3.0, seed + 404u, 4u, cells * 3.0);
    float pad = 1.0 - smoothstep(m.land2.z, m.land2.z * 1.6, acos(clamp(dir.y, -1.0, 1.0)) * scale);
    land = max(land, pad);
    height = land * relief * mix(lowland, ridge * lowland, mountains) * (1.0 - pad);
}

// A planet mesh's vertex, displaced by its land, and the land's normal in object
// space from the heights a little way along the surface either side. Without land
// the sphere is left as it is.
#define TERRAIN_STEP 0.004   // radians: about the planet mesh's own spacing
vec3 planet_vertex(Material m, vec3 position, float scale, out vec3 normal) {
    vec3 dir = normalize(position);
    if (m.land.y <= 0.0) {
        normal = dir;
        return position;
    }
    vec3 t1 = normalize(cross(dir, abs(dir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(dir, t1);
    vec3 d1 = normalize(dir + t1 * TERRAIN_STEP), d2 = normalize(dir + t2 * TERRAIN_STEP);
    float h, h1, h2, land, continent;
    terrain(m, dir, scale, 0.0, h, land, continent);
    terrain(m, d1, scale, 0.0, h1, land, continent);
    terrain(m, d2, scale, 0.0, h2, land, continent);
    vec3 p0 = dir * (scale + h), p1 = d1 * (scale + h1), p2 = d2 * (scale + h2);
    normal = normalize(cross(p1 - p0, p2 - p0));
    return dir * (1.0 + h / scale);
}

// ---------------------------------------------------------------------------
// The shadow pass: depth only, from one cascade's point of view
// ---------------------------------------------------------------------------
#ifdef PROGRAM_SHADOW
#ifdef VERTEX
layout(location = 0) in vec3 a_position;
void main() {
    vec3 position = a_position;
    Material material = u_materials[u_material];
    if (uint(material.base.w) == MATERIAL_PLANET) {
        vec3 normal;
        position = planet_vertex(material, a_position, length(u_model[0].xyz), normal);
    }
    gl_Position = u_shadow[u_cascade] * u_model * vec4(position, 1.0);
}
#endif
#ifdef FRAGMENT
void main() {}
#endif
#endif

// ---------------------------------------------------------------------------
// The lit pass
// ---------------------------------------------------------------------------
#ifdef PROGRAM_MESH
#ifdef VERTEX
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_surface;   // object space, in metres
layout(location = 2) out vec3 v_world;

void main() {
    vec3 scale = vec3(length(u_model[0].xyz), length(u_model[1].xyz), length(u_model[2].xyz));
    vec3 position = a_position, normal = a_normal;
    Material material = u_materials[u_material];
    if (uint(material.base.w) == MATERIAL_PLANET) position = planet_vertex(material, a_position, scale.x, normal);
    v_normal = mat3(u_model) * (normal / (scale * scale));
    v_surface = position * scale;
    v_world = (u_model * vec4(position, 1.0)).xyz;
    gl_Position = u_view_proj * vec4(v_world, 1.0);
}
#endif

#ifdef FRAGMENT
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_surface;
layout(location = 2) in vec3 v_world;
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

// A planet's surface at a point of its unit sphere: the albedo, what it emits (the
// atmosphere's rim, the night side's glow) and the tint along the terminator.
void planet(Material m, vec3 dir, float scale, float rim, float facing_sun, out vec3 albedo, out vec3 emission, out vec3 tint) {
    float detail = m.params.x, contrast = m.params.y;
    uint seed = uint(m.params.w);
    float footprint = length(fwidth(dir));
    float cells = footprint * detail;              // noise cells per pixel, at the first octave
    float spots = fbm(dir * detail * 2.0, seed + 101u, 3u, cells * 2.0);
    float clouds = fbm(dir * detail * 1.5, seed + 202u, 3u, cells * 1.5);
    if (m.land.y > 0.0) {
        // A world with land: the sea, darker where deep; the land; snow above the line.
        float height, land, continent;
        terrain(m, dir, scale, footprint * m.land.x, height, land, continent);
        float depth = clamp((m.land.z - continent) / max(m.land.z, 1e-3), 0.0, 1.0);
        vec3 sea = m.base.rgb * (1.0 - 0.5 * depth);
        albedo = mix(sea, m.second.rgb, land);
        float elevation = height / m.land.y;
        albedo = mix(albedo, m.third.rgb, smoothstep(m.land2.x - 0.1, m.land2.x + 0.1, elevation));
    } else {
        float n = fbm(dir * detail, seed, 4u, cells);
        float latitude = dir.y + (n - 0.5) * m.third.w;
        float level = m.second.w > 0.0 ? 0.5 + 0.5 * sin(latitude * m.second.w * PI) : n;
        albedo = mix(m.base.rgb, m.second.rgb, clamp((level - 0.5) * contrast + 0.5, 0.0, 1.0));
        albedo = mix(albedo, m.third.rgb, clamp((spots - 0.75) * contrast * 2.0 + 0.5, 0.0, 1.0));
    }
    float cover = clamp((clouds - 1.0 + m.clouds.w) * contrast, 0.0, 1.0);
    albedo = mix(albedo, m.clouds.rgb, cover);
    float day = smoothstep(-1.0, 1.0, facing_sun);
    emission = m.atmosphere.rgb * m.atmosphere.w * pow(rim, m.params.z) * day;
    float lights = clamp((spots - 1.0 + m.night.w) * contrast, 0.0, 1.0);
    emission += m.night.rgb * lights * (1.0 - cover) * (1.0 - day);
    float width = m.terminator.w;
    float line = width > 0.0 ? exp(-facing_sun * facing_sun / (width * width)) : 0.0;
    tint = mix(vec3(1.0), m.terminator.rgb, line);
}

// How much of the way from `from` to `to` lies inside a sphere: the air a sight
// line crosses.
float air_path(vec3 from, vec3 to, vec4 sphere) {
    vec3 d = to - from;
    vec3 f = from - sphere.xyz;
    float a = dot(d, d);
    if (a < 1e-6) return 0.0;
    float b = 2.0 * dot(f, d);
    float c = dot(f, f) - sphere.w * sphere.w;
    float discriminant = b * b - 4.0 * a * c;
    if (discriminant <= 0.0) return 0.0;
    float root = sqrt(discriminant);
    float t0 = clamp((-b - root) / (2.0 * a), 0.0, 1.0);
    float t1 = clamp((-b + root) / (2.0 * a), 0.0, 1.0);
    return (t1 - t0) * sqrt(a);
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
    vec3 color = u_color.rgb;
    if ((u_flags & FLAG_UNLIT) != 0u) {
        o_color = vec4(color, UNLIT_ALPHA);
        return;
    }
    if ((u_flags & FLAG_EMISSIVE) != 0u) {
        o_color = vec4(color, 1.0);
        return;
    }
    vec3 normal = normalize(v_normal);
    vec3 to_camera = u_camera_position.xyz - v_world;
    float rim = 1.0 - max(dot(normal, normalize(to_camera)), 0.0);
    float view_depth = -dot(to_camera, u_camera_forward.xyz);
    float facing_sun = dot(normal, u_sun_dir.xyz);

    vec3 albedo, emission = vec3(0.0), tint = vec3(1.0);
    Material material = u_materials[u_material];
    bool world = uint(material.base.w) == MATERIAL_PLANET;
    if (world) {
        // The air's effects (the rim, the terminator, the night) go by the sphere's
        // normal, not the land's: a hillside turned from the eye is not the limb.
        vec3 dir = normalize(v_surface);
        vec3 sphere_normal = normalize(mat3(u_model) * dir);
        float sphere_rim = 1.0 - max(dot(sphere_normal, normalize(to_camera)), 0.0);
        planet(material, dir, length(u_model[0].xyz), sphere_rim, dot(sphere_normal, u_sun_dir.xyz), albedo, emission, tint);
    } else {
        // A metre checker, so walking and flying read as motion without textures.
        vec3 cell = floor(v_surface + 0.001);
        float checker = 0.92 + 0.08 * mod(cell.x + cell.y + cell.z, 2.0);
        albedo = color * checker;
        // The rim: the sky's light grazing the silhouette, more on what faces up.
        emission += u_rim.rgb * u_rim.w * pow(rim, u_shadow_tint.w) * (normal.y * 0.5 + 0.5);
    }

    float sky_facing = normal.y * 0.5 + 0.5;
    vec3 light = mix(u_ambient_ground.rgb, u_ambient_sky.rgb, sky_facing);
    float key = ramp(facing_sun);
    if (key > 0.0) key *= sun_visibility(v_world, normal, view_depth);
    light += u_sun_color.rgb * key;
    // Only the lights whose reach touches this draw, a bit each.
    for (uint mask = u_lights; mask != 0u; mask &= mask - 1u) {
        uint i = uint(findLSB(mask));
        vec3 to_light = u_light_position[i].xyz - v_world;
        float radius = u_light_position[i].w;
        float d2 = dot(to_light, to_light);
        float window = clamp(1.0 - d2 * d2 / (radius * radius * radius * radius), 0.0, 1.0);
        float falloff = window * window / (d2 + 1.0);
        float facing = ramp(dot(normal, to_light * inversesqrt(max(d2, 1e-6))));
        light += u_light_color[i].rgb * falloff * facing;
    }
    // Where the key light does not reach, the shadow's tint.
    light *= mix(u_shadow_tint.rgb, vec3(1.0), key);
    vec3 radiance = albedo * light * tint + emission;
    // Aerial perspective: what is seen through an atmosphere fades into its haze by
    // the length of the way through its air, from orbit as much as from inside; and
    // the haze about the camera (a cloud deck) by the whole distance.
    for (uint i = 0u; i < u_counts.z; ++i) {
        float way = air_path(u_camera_position.xyz, v_world, u_air_sphere[i]);
        if (way > 0.0) radiance = mix(radiance, u_air_haze[i].rgb, 1.0 - exp(-way * u_air_haze[i].w));
    }
    if (u_haze.w > 0.0) radiance = mix(radiance, u_haze.rgb, 1.0 - exp(-length(to_camera) * u_haze.w));
    o_color = vec4(radiance, 1.0);
}
#endif
#endif

// ---------------------------------------------------------------------------
// The sky: one triangle over the screen, at the far plane
// ---------------------------------------------------------------------------
#ifdef PROGRAM_SKY
#ifdef VERTEX
layout(location = 0) out vec2 v_ndc;
void main() {
    v_ndc = vec2(VERTEX_INDEX == 1 ? 3.0 : -1.0, VERTEX_INDEX == 2 ? 3.0 : -1.0);
    gl_Position = vec4(v_ndc, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

// The sky is tiled as a cube: the direction picks a face and a cell on it, the
// cell's hash says whether it holds a star, where in the cell, and how bright. A
// star's size is angular, so it is the same in every direction and at every
// resolution.
vec3 face_direction(uint face, vec2 uv) {
    float sign = (face & 1u) == 0u ? 1.0 : -1.0;
    if (face < 2u) return vec3(sign, uv.y, uv.x);
    if (face < 4u) return vec3(uv.x, sign, uv.y);
    return vec3(uv.x, uv.y, sign);
}

vec3 stars(vec3 dir) {
    vec3 a = abs(dir);
    uint face;
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = dir.x > 0.0 ? 0u : 1u; uv = dir.zy / a.x; }
    else if (a.y >= a.z) { face = dir.y > 0.0 ? 2u : 3u; uv = dir.xz / a.y; }
    else { face = dir.z > 0.0 ? 4u : 5u; uv = dir.xy / a.z; }
    float cells = u_stars.z;
    vec2 cell = floor((uv * 0.5 + 0.5) * cells);
    uint h = hash(uint(cell.x) + hash(uint(cell.y) + hash(face + u_counts.y)));
    if (float(h & 0xffffu) / 65535.0 >= u_stars.x) return vec3(0.0);
    // Kept away from the cell's edges, so the star's glow never runs into the next cell.
    vec2 at = 0.2 + 0.6 * vec2(float((h >> 16) & 0xffu), float((h >> 24) & 0xffu)) / 255.0;
    vec3 star = normalize(face_direction(face, ((cell + at) / cells) * 2.0 - 1.0));
    uint h2 = hash(h);
    float size = 0.0012 + 0.0022 * float(h2 & 0xffu) / 255.0;             // radians
    float brightness = 0.25 + 0.75 * float((h2 >> 8) & 0xffu) / 255.0;
    vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.9, 0.75), float((h2 >> 16) & 0xffu) / 255.0);
    float d = length(dir - star);
    float glow = exp(-d * d / (size * size));
    return tint * glow * brightness * brightness * u_stars.y;
}

void main() {
    vec3 dir = normalize(u_camera_forward.xyz + u_camera_right.xyz * (v_ndc.x * u_camera_right.w)
                         + u_camera_up.xyz * (v_ndc.y * u_camera_up.w));
    float height = dir.y;
    vec3 color = height >= 0.0 ? mix(u_sky_horizon.rgb, u_sky_zenith.rgb, pow(height, 0.5))
                               : mix(u_sky_horizon.rgb, u_sky_ground.rgb, pow(-height, 0.35));
    // Inside an atmosphere, the planet's sky instead, by how deep the camera is;
    // below its horizon, its haze. Blended as the eye sees it (in the display's
    // gamma), since a daylit sky is many times brighter than space and a linear
    // blend would be day almost at once.
    float immersion = u_air_zenith.w;
    if (immersion > 0.0) {
        vec3 below = u_haze.w > 0.0 ? u_haze.rgb : u_air_haze[0].rgb;
        vec3 air = height >= 0.0 ? mix(u_air_horizon.rgb, u_air_zenith.rgb, pow(height, 0.5))
                                 : mix(u_air_horizon.rgb, below, smoothstep(0.0, 0.25, -height));
        vec3 encode = vec3(1.0 / u_tonemap.w);
        color = pow(mix(pow(color, encode), pow(air, encode), immersion), vec3(u_tonemap.w));
    }
    float toward_sun = dot(dir, u_sun_dir.xyz);
    color += u_sun_color.rgb * (u_sun_color.w + u_air_horizon.w) * pow(max(toward_sun, 0.0), 48.0);
    if (toward_sun >= u_sun_dir.w && u_sun_dir.w < 1.0) color += u_sun_color.rgb * 12.0;
    color += stars(dir) * (1.0 - u_air.x);
    o_color = vec4(color, 1.0);
}
#endif
#endif

// ---------------------------------------------------------------------------
// The post steps: one triangle over the target, sampling up to three inputs
// ---------------------------------------------------------------------------
#ifdef PROGRAM_POST
#ifdef VERTEX
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 ndc = vec2(VERTEX_INDEX == 1 ? 3.0 : -1.0, VERTEX_INDEX == 2 ? 3.0 : -1.0);
    v_uv = ndc * 0.5 + 0.5;
#ifdef VULKAN
    v_uv.y = 1.0 - v_uv.y;    // targets are rasterised y up, but Vulkan's row 0 is the top
#endif
    gl_Position = vec4(ndc, 0.0, 1.0);
}
#endif
#endif

#if defined(PROGRAM_DOWNSAMPLE) || defined(PROGRAM_UPSAMPLE) || defined(PROGRAM_FLARE) || defined(PROGRAM_COMPOSITE) || defined(PROGRAM_FXAA)
#ifdef FRAGMENT
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
INPUT_AT(0) sampler2D u_input0;
INPUT_AT(1) sampler2D u_input1;
INPUT_AT(2) sampler2D u_input2;
#endif
#endif

// Bloom, down: one mip to the next, half the size. Jimenez's 13 taps, five
// overlapping quads with the middle one weighted four times a corner one. The
// first mip also thresholds (with a soft knee), drops unlit pixels, and weights
// each quad down by its brightness (Karis), so one blazing pixel does not
// flicker as a whole blob.
#if defined(PROGRAM_DOWNSAMPLE) && defined(FRAGMENT)
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
#endif

// Bloom, up: the coarser result through a 3x3 tent of the given radius, blended
// with this level's downsample by the scatter. The tent is four bilinear taps at
// half-texel offsets, which is the same 1-2-1 kernel for a quarter of the fetches.
#if defined(PROGRAM_UPSAMPLE) && defined(FRAGMENT)
vec3 tap(vec2 offset) { return texture(u_input0, v_uv + offset * u_texel.xy * u_params.x).rgb; }
void main() {
    vec3 coarser = (tap(vec2(-0.5, 0.5)) + tap(vec2(0.5, 0.5)) + tap(vec2(-0.5, -0.5)) + tap(vec2(0.5, -0.5))) * 0.25;
    vec3 finer = texture(u_input1, v_uv).rgb;
    o_color = vec4(mix(finer, coarser, u_params.y), 1.0);
}
#endif

// The lens flare, from the picture itself: what is bright, mirrored through the
// centre, repeated along the line to the centre as ghosts with their colours
// fringed, and a halo ring; both fade towards the edges.
#if defined(PROGRAM_FLARE) && defined(FRAGMENT)
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
#endif

// The composite: the scene with the bloom and the flare added, the colour
// channels pulled apart radially, exposure and white balance, the tone curve, the
// grading, the vignette, gamma. Luma goes to alpha when anti-aliasing follows.
#if defined(PROGRAM_COMPOSITE) && defined(FRAGMENT)
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
#endif

// FXAA 3.11 (Lottes), in the form Simon Rodriguez set out: find an edge from the
// luma in the input's alpha, walk along it both ways to find its ends, and blend
// towards the other side by how far along the edge this pixel is; plus a blend
// for sub-pixel aliasing.
#if defined(PROGRAM_FXAA) && defined(FRAGMENT)
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
#endif
