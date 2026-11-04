// The built-in material shaders' parameters, laid out as their shaders read them
// (src/render/shaders/materials/*.frag), with a function each that makes an
// r_material of them. A new material shader gets a struct like these, or reads
// the vec4s straight.
#ifndef TP_RENDER_MATERIALS_H
#define TP_RENDER_MATERIALS_H

#include "renderer.h"
#include <string.h>

// materials/surface.frag: the draw's colour, checkered by the metre.
struct r_surface_params {
    float checker;               // the checker's contrast (0: none)
};

// materials/planet.frag: a world on its sphere. With relief, the mesh arrives
// shaped by its land and the surface is coloured by how high it stands: `base`
// is the sea, `second` the land, `third` above the `snow_line`; without, a gas
// giant of `base` where the noise or the bands are low, `second` where high,
// `third` in the brightest spots. Clouds may be painted over it, the atmosphere
// glows at the rim, `night` is emitted on the dark side where the noise is high,
// the terminator is tinted. Every colour is linear; every amount's zero is none.
struct r_planet_params {
    v3 base, second, third;
    float bands;                 // latitude bands across the sphere (0: the noise alone)
    float swirl;                 // how far the noise bends the bands, in latitude
    float detail;                // noise frequency over the sphere
    float contrast;              // how sharply the tones separate (1: as the noise is)
    v3 clouds;                   // painted clouds' colour
    float cloud_cover;           // 0..1 (0: none painted; a cloud layer is a material of its own)
    v3 atmosphere;
    float atmosphere_strength;   // the rim's brightness
    float atmosphere_power;      // how tight to the rim it is (higher: thinner)
    v3 night;
    float night_cover;           // 0..1: how much of the dark side glows
    v3 terminator_tint;          // multiplied in along the day/night line (1,1,1: none)
    float terminator_width;      // how wide that line is, as a cosine
    float continents;            // the land's noise frequency (for the pad's flat and the coast)
    float relief;                // metres the highest land rises (0: no land, a gas giant)
    float sea_level;             // 0..1 of the land noise below which is sea
    float mountains;             // 0..1 of the relief that is mountains
    float snow_line;             // 0..1 of the relief above which is `third`
    float shore;                 // how soft the coast is, as a fraction of the relief
    float pad_radius;            // metres about the top flattened for the landing pad
    uint32_t seed;
};

// materials/clouds.frag: a layer of cloud on a sphere, cut into cloud and hole.
struct r_cloud_params {
    v3 color;
    float cover;                 // 0..1 of the sphere
    float detail;                // noise frequency over the sphere
    float sharpness;             // how hard the edges are
    float drift;                 // radians a second the layer turns about the axis
    float translucency;          // 0..1: how much of the sun comes through from behind
    uint32_t seed;
};

static inline void r_param3(float out[4], v3 v, float w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

static inline r_material r_material_surface(r_shader shader, const r_surface_params *p) {
    r_material m;
    memset(&m, 0, sizeof(m));
    m.shader = shader;
    m.params[0][0] = p->checker;
    return m;
}

static inline r_material r_material_planet(r_shader shader, const r_planet_params *p) {
    r_material m;
    memset(&m, 0, sizeof(m));
    m.shader = shader;
    r_param3(m.params[0], p->base, 0.0f);
    r_param3(m.params[1], p->second, p->bands);
    r_param3(m.params[2], p->third, p->swirl);
    r_param3(m.params[3], p->clouds, p->cloud_cover);
    r_param3(m.params[4], p->atmosphere, p->atmosphere_strength);
    r_param3(m.params[5], p->night, p->night_cover);
    r_param3(m.params[6], p->terminator_tint, p->terminator_width);
    m.params[7][0] = p->detail;
    m.params[7][1] = p->contrast;
    m.params[7][2] = p->atmosphere_power;
    m.params[7][3] = (float)p->seed;
    m.params[8][0] = p->continents;
    m.params[8][1] = p->relief;
    m.params[8][2] = p->sea_level;
    m.params[8][3] = p->mountains;
    m.params[9][0] = p->snow_line;
    m.params[9][1] = p->shore;
    m.params[9][2] = p->pad_radius;
    return m;
}

static inline r_material r_material_clouds(r_shader shader, const r_cloud_params *p) {
    r_material m;
    memset(&m, 0, sizeof(m));
    m.shader = shader;
    r_param3(m.params[0], p->color, p->cover);
    m.params[1][0] = p->detail;
    m.params[1][1] = p->sharpness;
    m.params[1][2] = (float)p->seed;
    m.params[1][3] = p->drift;
    m.params[2][0] = p->translucency;
    return m;
}

#endif
