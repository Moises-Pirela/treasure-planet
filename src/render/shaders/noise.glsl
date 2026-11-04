// Noise from integer hashing, so that it is the same on every GPU and in every
// capture: a hash, value noise on a lattice, and octaves of it, plain and ridged.
// For the materials that want it (the sky, the planets, the clouds); include
// after common.glsl. lit.glsl includes it and so may a material; glslc, unlike the
// runtime assembler, takes every #include it sees, hence the guard.
#ifndef NOISE_GLSL
#define NOISE_GLSL

uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Value noise (one hash per lattice corner, the coordinates folded in by
// multiplication with large odd constants), in 0..1.
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

// How much cloud there is at a direction of a cloud layer's sphere: the noise,
// turned about the axis by `turn`, against the cover, the edges as sharp as said;
// 0 is a hole. The clouds material draws by it, and the lit pass shadows by it,
// so a cloud's shadow is the cloud's own shape.
float cloud_cover_at(vec3 dir, float cover, float detail, float sharpness, uint seed, float turn, float cells) {
    vec3 turned = vec3(dir.x * cos(turn) - dir.z * sin(turn), dir.y, dir.x * sin(turn) + dir.z * cos(turn));
    float noise = fbm(turned * detail, seed + 202u, 3u, cells);
    return clamp((noise - 1.0 + cover) * sharpness, 0.0, 1.0);
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

#endif
