// The sky: a gradient from the zenith through the horizon to the ground, then
// the layers in their order (fields of stars, nebulae, celestial bodies with
// their day side towards the sun and their rings), the sun's disc and its glow;
// inside an atmosphere, the planet's sky by how deep the camera is, and the
// stars and nebulae fade by how much of them the air hides.
#include "common.glsl"
#include "noise.glsl"

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

// A sky layer's fields, as common.glsl lays them out.
#define LAYER(i, slot) u_sky_layers[(i) * SKY_LAYER_VEC4S + (slot)]

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

vec3 stars(vec3 dir, float density, float brightness, vec3 tint_by, float size_scale, uint seed) {
    vec3 a = abs(dir);
    uint face;
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = dir.x > 0.0 ? 0u : 1u; uv = dir.zy / a.x; }
    else if (a.y >= a.z) { face = dir.y > 0.0 ? 2u : 3u; uv = dir.xz / a.y; }
    else { face = dir.z > 0.0 ? 4u : 5u; uv = dir.xy / a.z; }
    float cells = u_sky.z;
    vec2 cell = floor((uv * 0.5 + 0.5) * cells);
    uint h = hash(uint(cell.x) + hash(uint(cell.y) + hash(face + seed)));
    if (float(h & 0xffffu) / 65535.0 >= density) return vec3(0.0);
    // Kept away from the cell's edges, so the star's glow never runs into the next cell.
    vec2 at = 0.2 + 0.6 * vec2(float((h >> 16) & 0xffu), float((h >> 24) & 0xffu)) / 255.0;
    vec3 star = normalize(face_direction(face, ((cell + at) / cells) * 2.0 - 1.0));
    uint h2 = hash(h);
    float size = (0.0012 + 0.0022 * float(h2 & 0xffu) / 255.0) * size_scale;   // radians
    float glow_strength = 0.25 + 0.75 * float((h2 >> 8) & 0xffu) / 255.0;
    vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.9, 0.75), float((h2 >> 16) & 0xffu) / 255.0) * tint_by;
    float d = length(dir - star);
    float glow = exp(-d * d / (size * size));
    return tint * glow * glow_strength * glow_strength * brightness;
}

// A direction turned about an axis.
vec3 turned(vec3 dir, vec3 axis, float angle) {
    float c = cos(angle), s = sin(angle);
    return dir * c + cross(axis, dir) * s + axis * dot(axis, dir) * (1.0 - c);
}

// A nebula: glowing cloud where the noise passes the cover, two colours mixed by
// a second noise, filaments from ridged noise through it; about a direction
// within its size, or over the whole sky.
vec3 nebula(vec3 dir, uint i) {
    vec4 where = LAYER(i, 0), spin = LAYER(i, 1), colour = LAYER(i, 2), colour2 = LAYER(i, 3), shape = LAYER(i, 4);
    float reach = 1.0;
    if (dot(where.xyz, where.xyz) > 0.0) {
        float angle = acos(clamp(dot(dir, where.xyz), -1.0, 1.0));
        reach = 1.0 - smoothstep(spin.w * 0.45, spin.w, angle);
        if (reach <= 0.0) return vec3(0.0);
    }
    vec3 p = turned(dir, spin.xyz, shape.y) * shape.x;
    uint seed = uint(shape.z);
    float body = fbm(p, seed, 5u, 0.0);
    float cover = colour2.w;
    float cloud = clamp((body - (1.0 - cover)) / max(cover, 1e-3), 0.0, 1.0);
    cloud *= cloud;
    if (cloud <= 0.0) return vec3(0.0);    // most of the sky: the rest is six more octaves of nothing
    float filaments = ridged(p * 1.7 + 3.1, seed + 7u, 3u, 0.0);
    vec3 tone = mix(colour.rgb, colour2.rgb, fbm(p * 0.6 + 11.0, seed + 13u, 3u, 0.0));
    return tone * cloud * (0.4 + 1.2 * filaments * filaments) * colour.w * reach;
}

// A celestial body: a sphere of angular radius `size` at `direction`, lit by the
// sun (so it has a phase), its albedo two tones mixed by noise, its own light if
// it has any (with a glow about it), and rings across its axis, shadowed by it.
// Returns how much of the pixel it covers, with the colour of that (`tone`,
// premultiplied) and the glow about it, which adds.
float body(vec3 dir, uint i, float pixel, out vec3 tone, out vec3 glow) {
    vec4 where = LAYER(i, 0), spin = LAYER(i, 1), colour = LAYER(i, 2), colour2 = LAYER(i, 3), shape = LAYER(i, 4);
    vec3 centre = where.xyz, axis = spin.xyz;
    float size = spin.w;
    float ring_inner = shape.w, ring_outer = LAYER(i, 5).x;
    float toward = dot(dir, centre);
    float angle = acos(clamp(toward, -1.0, 1.0));
    float reach = max(size * max(ring_outer, 1.0), size) + pixel;
    glow = vec3(0.0);
    tone = vec3(0.0);
    if (colour.w > 0.0) glow = colour.rgb * colour.w * 0.6 * exp(-max(angle - size, 0.0) / (size * 1.5));
    if (angle > reach) return 0.0;

    // The body as a unit sphere at unit distance along `centre`; the visible point
    // at this direction, and its normal, which faces the eye at the middle.
    vec3 u = normalize(cross(centre, abs(centre.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 v = cross(centre, u);
    vec2 local = vec2(dot(dir, u), dot(dir, v)) / sin(size);
    float r2 = dot(local, local);
    float disc = 1.0 - smoothstep(size - pixel, size + pixel, angle);
    vec3 surface = vec3(0.0);
    if (disc > 0.0) {
        float z = sqrt(max(1.0 - min(r2, 1.0), 0.0));
        vec3 normal = normalize(u * local.x + v * local.y - centre * z);
        vec3 albedo = colour.rgb;
        if (shape.x > 0.0) {
            vec3 p = turned(normal, axis, shape.y) * shape.x;
            float bands = fbm(p + vec3(0.0, dot(normal, axis) * shape.x * 2.0, 0.0), uint(shape.z), 4u, 0.0);
            albedo = mix(colour.rgb, colour2.rgb, smoothstep(0.35, 0.65, bands));
        }
        float lit = clamp((dot(normal, u_sun_dir.xyz) + 0.05) / 1.05, 0.0, 1.0);
        surface = albedo * (u_sun_color.rgb * lit + vec3(0.01)) + colour.rgb * colour.w;
    }
    // The rings, in the body's own frame (x, y across the view in body radii, z
    // away along it: a body is far enough that its view is orthographic): where
    // the line of sight crosses the rings' plane, the radius there; hidden
    // behind the body, and hiding it in front; in the body's shadow where the way
    // to the sun passes through it.
    float ring = 0.0;
    vec3 ring_tone = vec3(0.0);
    if (ring_outer > ring_inner && ring_inner > 0.0) {
        vec3 axis_local = vec3(dot(axis, u), dot(axis, v), dot(axis, centre));
        if (abs(axis_local.z) > 1e-4) {
            vec3 at = vec3(local, -dot(local, axis_local.xy) / axis_local.z);
            float radius = length(at);
            if (radius >= ring_inner && radius <= ring_outer) {
                float edge = pixel / sin(size);
                ring = smoothstep(ring_inner, ring_inner + edge, radius) * (1.0 - smoothstep(ring_outer - edge, ring_outer, radius));
                ring *= 0.55 + 0.45 * value_noise(vec3(radius * 6.0, 0.5, 0.5), uint(shape.z) + 29u);
                ring *= 0.5 + 0.5 * smoothstep(0.0, 0.15, abs(axis_local.z));   // thin edge on
                vec3 sun_local = vec3(dot(u_sun_dir.xyz, u), dot(u_sun_dir.xyz, v), dot(u_sun_dir.xyz, centre));
                float lit = abs(axis_local.z * sun_local.z + dot(axis_local.xy, sun_local.xy)) * 0.8 + 0.2;
                float toward = dot(at, sun_local);
                bool shadowed = toward < 0.0 && length(at - sun_local * toward) < 1.0;
                ring_tone = colour2.rgb * u_sun_color.rgb * (shadowed ? 0.05 : lit);
                if (at.z > 0.0 && r2 < 1.0) ring *= 1.0 - disc;   // behind the body
            }
        }
    }
    // The rings over the body (behind it, they were cut by the disc above).
    tone = surface * disc * (1.0 - ring) + ring_tone * ring;
    return disc * (1.0 - ring) + ring;
}

void main() {
    vec3 dir = normalize(view_ray(v_ndc));
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
    // The layers, in their order. The faint ones (stars, nebulae) add, and the air
    // hides them; a body stands in front of what was drawn before it, and the air
    // hides most of it.
    float space = 1.0 - u_air.x;
    float pixel = length(fwidth(dir));
    for (uint i = 0u; i < u_counts.y; ++i) {
        vec4 where = LAYER(i, 0), colour = LAYER(i, 2), colour2 = LAYER(i, 3), shape = LAYER(i, 4);
        uint kind = uint(where.w);
        if (kind == uint(SKY_STARS)) {
            vec3 tint = dot(colour.rgb, colour.rgb) > 0.0 ? colour.rgb : vec3(1.0);
            float size_scale = LAYER(i, 1).w > 0.0 ? LAYER(i, 1).w : 1.0;
            color += stars(dir, colour2.w, colour.w, tint, size_scale, uint(shape.z)) * space;
        } else if (kind == uint(SKY_NEBULA)) {
            color += nebula(dir, i) * space;
        } else if (kind == uint(SKY_BODY)) {
            vec3 tone, glow;
            float cover = body(dir, i, pixel, tone, glow);
            float shown = mix(0.25, 1.0, space);
            color = color * (1.0 - cover * shown) + tone * shown + glow * space;
        }
    }
    float toward_sun = dot(dir, u_sun_dir.xyz);
    color += u_sun_color.rgb * (u_sun_color.w + u_air_horizon.w) * pow(max(toward_sun, 0.0), 48.0);
    if (toward_sun >= u_sun_dir.w && u_sun_dir.w < 1.0) color += u_sun_color.rgb * 12.0;
    o_color = vec4(color, 1.0);
}
