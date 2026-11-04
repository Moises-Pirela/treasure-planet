// A planet's land. The same field twice over: the renderer displaces the planet's
// sphere by it in a shader, and this is what the simulation stands players on, so
// that what is walked on is what is seen. Integer hashing and plain float
// arithmetic, in the same steps and the same order as shaders.glsl, so both sides
// agree wherever a player stands.
#include "game.h"

// Octaves of noise in the field: the finest is 2^(n-1) times the continents'
// frequency, and the mesh the renderer makes of it has to have vertices that fine.
#define PLANET_OCTAVES 6
#define PLANET_MOUNTAIN_SCALE 8.0f   // the mountains' noise against the continents': hills within a land

static uint32_t planet_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float lattice(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = planet_hash((uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu ^ seed);
    return (float)h / 4294967295.0f;
}

static float value_noise(v3 p, uint32_t seed) {
    float cx = floorf(p.x), cy = floorf(p.y), cz = floorf(p.z);
    float fx = p.x - cx, fy = p.y - cy, fz = p.z - cz;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    fz = fz * fz * (3.0f - 2.0f * fz);
    int32_t ix = (int32_t)cx, iy = (int32_t)cy, iz = (int32_t)cz;
    float a0 = lattice(ix, iy, iz, seed), a1 = lattice(ix, iy + 1, iz, seed);
    float a2 = lattice(ix, iy, iz + 1, seed), a3 = lattice(ix, iy + 1, iz + 1, seed);
    float x0 = a0 + (lattice(ix + 1, iy, iz, seed) - a0) * fx;
    float x1 = a1 + (lattice(ix + 1, iy + 1, iz, seed) - a1) * fx;
    float x2 = a2 + (lattice(ix + 1, iy, iz + 1, seed) - a2) * fx;
    float x3 = a3 + (lattice(ix + 1, iy + 1, iz + 1, seed) - a3) * fx;
    float y0 = x0 + (x1 - x0) * fy, y1 = x2 + (x3 - x2) * fy;
    return y0 + (y1 - y0) * fz;
}

static float fbm(v3 p, uint32_t seed, int octaves) {
    float sum = 0.0f, amplitude = 0.5f, total = 0.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += value_noise(p, seed + (uint32_t)octave) * amplitude;
        total += amplitude;
        p = v3_scale(p, 2.0f);
        amplitude *= 0.5f;
    }
    return sum / total;
}

static float ridged(v3 p, uint32_t seed, int octaves) {
    float sum = 0.0f, amplitude = 0.5f, total = 0.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += (1.0f - fabsf(2.0f * value_noise(p, seed + (uint32_t)octave) - 1.0f)) * amplitude;
        total += amplitude;
        p = v3_scale(p, 2.0f);
        amplitude *= 0.5f;
    }
    return sum / total;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float smooth_step(float edge0, float edge1, float x) {
    float t = clamp01((x - edge0) / (edge1 - edge0 != 0.0f ? edge1 - edge0 : 1e-6f));
    return t * t * (3.0f - 2.0f * t);
}

float game_planet_height(const PlanetLand *land, v3 direction) {
    if (land->relief <= 0.0f) return 0.0f;
    float from_pole = acosf(direction.y < -1.0f ? -1.0f : (direction.y > 1.0f ? 1.0f : direction.y)) * land->radius;
    if (from_pole <= land->pad_radius) return 0.0f;            // the landing pad's flat
    float pad = 1.0f - smooth_step(land->pad_radius, land->pad_radius * 1.6f, from_pole);
    float continent = fbm(v3_scale(direction, land->continents), land->seed + 303u, PLANET_OCTAVES);
    float on_land = smooth_step(land->sea_level - land->shore, land->sea_level + land->shore, continent);
    if (pad > on_land) on_land = pad;
    if (on_land <= 0.0f) return 0.0f;                          // open sea: no mountains to work out
    float lowland = clamp01((continent - land->sea_level) / (1.0f - land->sea_level > 1e-3f ? 1.0f - land->sea_level : 1e-3f));
    float ridge = ridged(v3_scale(direction, land->continents * PLANET_MOUNTAIN_SCALE), land->seed + 404u, PLANET_OCTAVES);
    float shape = lowland + (ridge * lowland - lowland) * land->mountains;
    return on_land * land->relief * shape * (1.0f - pad);
}

float game_planet_surface_y(const PlanetLand *land, v3 centre, v3 position) {
    float dx = position.x - centre.x, dz = position.z - centre.z;
    float flat = dx * dx + dz * dz;
    bool above = position.y >= centre.y;
    v3 at = position;
    // The vertical line through the point meets the land where the radius there
    // allows; the direction and the height settle in a couple of rounds.
    for (int round = 0; round < 3; ++round) {
        v3 direction = v3_normalize(v3_sub(at, centre));
        if (v3_length(v3_sub(at, centre)) < 1e-4f) return -1e30f;
        float radius = land->radius + game_planet_height(land, direction);
        float square = radius * radius - flat;
        if (square <= 0.0f) return -1e30f;                     // past the limb: no ground under it
        at.y = centre.y + (above ? sqrtf(square) : -sqrtf(square));
    }
    return at.y;
}

v3 game_planet_point(const PlanetLand *land, v3 centre, v3 heading, float arc) {
    v3 flat = v3_make(heading.x, 0.0f, heading.z);
    float length = v3_length(flat);
    flat = length > 1e-4f ? v3_scale(flat, 1.0f / length) : v3_make(0, 0, 1);
    float angle = arc / land->radius;
    v3 direction = v3_normalize(v3_add(v3_scale(v3_make(0, 1, 0), cosf(angle)), v3_scale(flat, sinf(angle))));
    return v3_add(centre, v3_scale(direction, land->radius + game_planet_height(land, direction)));
}
