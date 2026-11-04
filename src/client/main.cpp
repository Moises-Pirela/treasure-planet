// The client: reads the keyboard and mouse into one player's input and draws what is
// in the world. It owns no game state beyond where the camera looks. Alone, it ticks
// the simulation itself; with --connect, the world is a replica fed by a server and
// the input goes there instead (src/net/client.h).
#include "../game/game.h"
#include "../net/client.h"
#include "../platform/platform.h"
#include "../render/materials.h"
#include "../render/renderer.h"
#include "hud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOUSE_RADIANS_PER_COUNT 0.0025f
// A planet's landmass mesh comes in levels of detail, by how far the camera is
// from its surface in radii: the finest up close, coarser ones further out, each
// `beyond` its distance. Segments around; half as many from pole to pole.
struct planet_lod { int segments; float beyond; };
static const planet_lod PLANET_LODS[] = {{384, 0.0f}, {128, 2.5f}, {48, 6.0f}};
#define PLANET_LOD_COUNT 3
#define CLOUD_SEGMENTS 192           // the cloud layer's sphere up close; the game's sphere further out
#define CLOUD_LOD_BEYOND 2.5f
// A world casts and takes shadows only while the camera is this close above its
// surface: its land and its clouds in every cascade are the dearest thing in the
// shadow pass, and from further away no shadow on them can be seen.
#define PLANET_SHADOW_ALTITUDE 220.0f
#define MAX_PLANET_MESHES (8 * PLANET_LOD_COUNT)
#define MAX_PITCH 1.5f
#define STICK_PER_COUNT 0.004f       // how far a count of mouse moves the flight stick, of its full throw
#define STICK_DEADZONE 0.06f         // ...and how far from the middle it must be to turn anything

// What the client keeps between frames about how its player looks and flies: the
// look (in the frame the player stands in, which is a craft's aboard one), the
// flight stick where the mouse has pushed it, and whether the flight computer is off.
struct view_state {
    float yaw, pitch;
    cw_entity_id frame;          // the craft the look is in, or CW_INVALID_ENTITY: the world
    quat frame_orientation;      // its orientation when last seen, should it go
    bool piloting;
    float stick_x, stick_y;      // -1..1
    bool decoupled;
};

static view_state view_make(float yaw, float pitch) {
    view_state v = {};
    v.yaw = yaw;
    v.pitch = pitch;
    v.frame = CW_INVALID_ENTITY;
    v.frame_orientation = quat_identity();
    return v;
}

// The craft our player stands in: from the replica's prediction online, the world alone.
static cw_entity_id craft_of(game *g, const net_client *net, cw_entity_id player) {
    if (!cw_world_is_valid(&g->world, player)) return CW_INVALID_ENTITY;
    return net ? net_client_craft_of(net, player) : game_aboard(g, player);
}

// Keeps the look steady as the frame changes under it: stepping into a craft the
// yaw and pitch become that craft's angles for the same direction, and back again
// stepping out. Taking the helm faces the bow; the stick starts in the middle.
static void follow_frame(game *g, const net_client *net, cw_entity_id player, view_state *v) {
    cw_entity_id craft = craft_of(g, net, player);
    bool piloting = craft != CW_INVALID_ENTITY && CW_GET(&g->world, craft, g->c_craft, Craft)->pilot == player;
    if (craft != v->frame) {
        v3 direction = quat_rotate(v->frame == CW_INVALID_ENTITY ? quat_identity() : v->frame_orientation,
                                   look_direction(v->yaw, v->pitch));
        if (craft != CW_INVALID_ENTITY) direction = quat_unrotate(CW_GET(&g->world, craft, g->c_craft, Craft)->orientation, direction);
        look_angles(direction, &v->yaw, &v->pitch);
        v->frame = craft;
    }
    if (piloting != v->piloting) {
        v->stick_x = v->stick_y = 0.0f;
        if (piloting) v->yaw = v->pitch = 0.0f;
        v->piloting = piloting;
    }
    if (craft != CW_INVALID_ENTITY) v->frame_orientation = CW_GET(&g->world, craft, g->c_craft, Craft)->orientation;
}

struct options {
    r_backend_kind backend;
    const char *connect;         // host[:port] of a zone server; NULL plays alone
    const char *directory;       // host[:port] of a directory to find the zone through instead
    const char *place;           // the place to ask the directory for; NULL: the hub
    const char *name;            // our display name online; NULL makes one up
    const char *region;          // "eu", "na"...: the shards to prefer
    const char *capture_path;    // render one scripted frame, write it and exit
    int capture_ticks;
    const char *dungeon;         // alone: walk this dungeon instead of the universe
    bool craft;                  // alone: start at the helm of a craft in the yard
    bool stats;                  // print the frame's and the GPU's times every second
    float bench_seconds;         // hold still (--at, --look) this long without vsync, printing the stats, and exit
    bool has_position;
    v3 position;
    v3 velocity;                 // --moving: the camera's velocity for a capture, for an atmosphere's entry
    float yaw, pitch;
};

static bool parse_options(int argc, char **argv, options *o) {
    memset(o, 0, sizeof(*o));
    o->backend = R_BACKEND_VULKAN;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(arg, "--renderer") && more) {
            const char *name = argv[++i];
            if (!strcmp(name, "vulkan")) o->backend = R_BACKEND_VULKAN;
            else if (!strcmp(name, "gl") || !strcmp(name, "opengl")) o->backend = R_BACKEND_OPENGL;
            else return false;
        } else if (!strcmp(arg, "--connect") && more) {
            o->connect = argv[++i];
        } else if (!strcmp(arg, "--directory") && more) {
            o->directory = argv[++i];
        } else if (!strcmp(arg, "--place") && more) {
            o->place = argv[++i];
        } else if (!strcmp(arg, "--name") && more) {
            o->name = argv[++i];
        } else if (!strcmp(arg, "--region") && more) {
            o->region = argv[++i];
        } else if (!strcmp(arg, "--capture") && more) {
            o->capture_path = argv[++i];
        } else if (!strcmp(arg, "--ticks") && more) {
            o->capture_ticks = atoi(argv[++i]);
        } else if (!strcmp(arg, "--dungeon") && more) {
            o->dungeon = argv[++i];
        } else if (!strcmp(arg, "--craft")) {
            o->craft = true;
        } else if (!strcmp(arg, "--stats")) {
            o->stats = true;
        } else if (!strcmp(arg, "--bench") && more) {
            o->bench_seconds = (float)atof(argv[++i]);
            o->stats = true;
        } else if (!strcmp(arg, "--at") && i + 3 < argc) {
            o->position = v3_make((float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3]));
            o->has_position = true;
            i += 3;
        } else if (!strcmp(arg, "--moving") && i + 3 < argc) {
            o->velocity = v3_make((float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3]));
            i += 3;
        } else if (!strcmp(arg, "--look") && i + 2 < argc) {
            o->yaw = (float)atof(argv[i + 1]) * TP_PI / 180.0f;
            o->pitch = (float)atof(argv[i + 2]) * TP_PI / 180.0f;
            i += 2;
        } else {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The look: how each kind of world is lit, as data. Space has one hard sun and no
// air to soften it, so the shadows are black but for the starlight; a dungeon is
// lit by a low sun and whatever glows in it.
// ---------------------------------------------------------------------------
static r_sky_layer sky_stars(uint32_t seed, float density, float brightness, float size) {
    r_sky_layer l = {};
    l.kind = R_SKY_STARS;
    l.seed = seed;
    l.density = density;
    l.brightness = brightness;
    l.size = size;
    return l;
}
static r_sky_layer sky_nebula(uint32_t seed, v3 direction, float size, v3 color, v3 color2, float brightness, float density, float detail) {
    r_sky_layer l = {};
    l.kind = R_SKY_NEBULA;
    l.seed = seed;
    l.direction = direction;
    l.axis = v3_make(0, 1, 0);
    l.size = size;
    l.color = color;
    l.color2 = color2;
    l.brightness = brightness;
    l.density = density;
    l.detail = detail;
    l.drift = 0.002f;
    return l;
}
static r_sky_layer sky_body(uint32_t seed, v3 direction, float size, v3 color, v3 color2, float detail, float brightness) {
    r_sky_layer l = {};
    l.kind = R_SKY_BODY;
    l.seed = seed;
    l.direction = direction;
    l.axis = v3_make(0, 1, 0);
    l.size = size;
    l.color = color;
    l.color2 = color2;
    l.detail = detail;
    l.brightness = brightness;
    l.drift = 0.01f;
    return l;
}

// The sky over space: two nebulae (one about a point, one a faint wash over
// everything), fine stars and a few bright ones, a ringed giant low on one side,
// a moon high on the other, and a red dwarf with its glow; all in the order they
// are drawn, so the bodies stand in front of the rest.
static void space_sky(r_sky *sky) {
    uint32_t n = 0;
    sky->layers[n++] = sky_nebula(7, v3_normalize(v3_make(-0.4f, 0.25f, -0.85f)), 1.2f, v3_make(0.28f, 0.08f, 0.42f), v3_make(0.05f, 0.3f, 0.36f), 1.2f, 0.6f, 2.5f);
    sky->layers[n++] = sky_nebula(21, v3_make(0, 0, 0), 0.0f, v3_make(0.07f, 0.04f, 0.12f), v3_make(0.02f, 0.06f, 0.1f), 0.8f, 0.5f, 1.6f);
    sky->layers[n++] = sky_stars(12345, 0.25f, 0.5f, 0.7f);
    sky->layers[n++] = sky_stars(777, 0.04f, 1.6f, 1.3f);
    r_sky_layer giant = sky_body(3, v3_normalize(v3_make(0.7f, 0.3f, -0.7f)), 0.09f, v3_make(0.85f, 0.6f, 0.35f), v3_make(0.55f, 0.35f, 0.25f), 4.0f, 0.0f);
    giant.axis = v3_normalize(v3_make(0.5f, 0.75f, -0.45f));
    giant.ring_inner = 1.5f;
    giant.ring_outer = 2.3f;
    sky->layers[n++] = giant;
    sky->layers[n++] = sky_body(4, v3_normalize(v3_make(-0.5f, 0.55f, 0.4f)), 0.035f, v3_make(0.7f, 0.7f, 0.75f), v3_make(0.45f, 0.45f, 0.5f), 6.0f, 0.0f);
    sky->layers[n++] = sky_body(5, v3_normalize(v3_make(-0.8f, -0.1f, -0.5f)), 0.012f, v3_make(1.0f, 0.45f, 0.25f), v3_make(1.0f, 0.45f, 0.25f), 0.0f, 3.0f);
    sky->layer_count = n;
}

static r_environment environment_of(const game *g) {
    r_environment e = {};
    if (g->dungeon[0]) {
        e.sun.direction = v3_normalize(v3_make(0.55f, 0.32f, 0.25f));
        e.sun.color = v3_make(1.0f, 0.55f, 0.3f);
        e.sun.shadow_distance = 120.0f;
        e.sun.shadow_resolution = 2048;
        e.sky.zenith = v3_make(0.008f, 0.008f, 0.025f);
        e.sky.horizon = v3_make(0.25f, 0.09f, 0.05f);
        e.sky.ground = v3_make(0.01f, 0.006f, 0.004f);
        e.sky.sun_disc = 0.03f;
        e.sky.sun_glow = 0.8f;
        e.sky.layers[0] = sky_stars(12345, 0.08f, 0.8f, 1.0f);
        e.sky.layer_count = 1;
        // The damp: a warm dark haze along the floor, thinning upwards, glowing
        // towards the low sun.
        e.haze.color = v3_make(0.12f, 0.06f, 0.05f);
        e.haze.density = 0.012f;
        e.haze.base = 1.0f;
        e.haze.falloff = 0.12f;
        e.haze.start = 2.0f;
        e.haze.sky_distance = 150.0f;
        e.haze.sun_color = v3_make(0.5f, 0.2f, 0.08f);
        e.haze.sun_power = 6.0f;
        e.ambient_sky = v3_make(0.11f, 0.08f, 0.10f);
        e.ambient_ground = v3_make(0.05f, 0.035f, 0.035f);
        e.shading.ramp_softness = 0.3f;
        e.shading.ramp_wrap = 0.1f;
        e.shading.ramp_flatness = 0.6f;
        e.shading.shadow_tint = v3_make(0.8f, 0.6f, 0.75f);
        e.shading.rim = v3_make(0.8f, 0.3f, 0.12f);
        e.shading.rim_strength = 0.4f;
        e.shading.rim_power = 3.0f;
    } else {
        e.sun.direction = v3_normalize(v3_make(0.5f, 0.8f, 0.35f));
        e.sun.color = v3_make(1.05f, 0.98f, 0.88f);
        e.sun.shadow_distance = 160.0f;
        e.sun.shadow_resolution = 2048;
        e.sky.zenith = v3_make(0.008f, 0.008f, 0.03f);
        e.sky.horizon = v3_make(0.07f, 0.04f, 0.13f);
        e.sky.ground = v3_make(0.008f, 0.008f, 0.03f);    // space has no floor: the zenith again
        e.sky.sun_disc = 0.02f;
        e.sky.sun_glow = 0.35f;
        space_sky(&e.sky);
        // The dust: a faint cold veil that gives the piers their distance.
        e.haze.color = v3_make(0.04f, 0.05f, 0.09f);
        e.haze.density = 0.0008f;
        e.haze.sky_distance = 100.0f;
        e.ambient_sky = v3_make(0.10f, 0.11f, 0.17f);
        e.ambient_ground = v3_make(0.05f, 0.035f, 0.05f);
        e.shading.ramp_softness = 0.25f;
        e.shading.ramp_wrap = 0.1f;
        e.shading.ramp_flatness = 0.5f;
        e.shading.shadow_tint = v3_make(0.7f, 0.72f, 1.0f);
        e.shading.rim = v3_make(0.45f, 0.35f, 0.8f);
        e.shading.rim_strength = 0.6f;
        e.shading.rim_power = 3.0f;
    }
    return e;
}

// What the picture goes through after lighting, as data too. Space is crisp and
// cold with a hard sun to bloom; a dungeon is warmer, softer and darker at the
// edges, with its lamps glowing.
static r_post post_of(const game *g) {
    r_post p = {};
    p.tonemap.curve = R_TONEMAP_ACES;
    p.tonemap.exposure = g->dungeon[0] ? 0.6f : 0.52f;
    // The drawn line: ink along every silhouette and crease, fading off with distance.
    p.outline.strength = g->dungeon[0] ? 0.7f : 0.6f;
    p.outline.width = 1.0f;
    p.outline.depth_threshold = 0.03f;
    p.outline.normal_threshold = 0.25f;
    p.outline.color = v3_make(0.02f, 0.01f, 0.03f);
    p.outline.fade_distance = 250.0f;
    p.bloom.threshold = 1.0f;
    p.bloom.knee = 0.5f;
    p.bloom.scatter = 0.7f;
    p.bloom.radius = 1.0f;
    p.flare.threshold = 4.0f;
    p.flare.ghosts = 4;
    p.flare.dispersal = 0.3f;
    p.flare.falloff = 6.0f;
    p.flare.halo_width = 0.45f;
    p.flare.halo_intensity = 0.5f;
    p.flare.chromatic = 0.004f;
    p.flare.source_mip = 2;
    p.entry.blur = 0.06f;
    p.entry.samples = 8;
    p.entry.streaks = 22.0f;
    p.entry.streak_length = 1.5f;
    p.entry.streak_speed = 1.5f;
    p.entry.glow = 0.8f;
    p.antialiasing.mode = R_AA_FXAA;
    p.antialiasing.edge_threshold_min = 0.0312f;
    p.antialiasing.edge_threshold = 0.125f;
    p.antialiasing.subpixel = 0.75f;
    p.antialiasing.iterations = 12;
    if (g->dungeon[0]) {
        p.grading.temperature = 0.25f;
        p.grading.contrast = 0.1f;
        p.grading.saturation = 0.1f;
        p.grading.lift = v3_make(0.01f, 0.005f, 0.0f);
        p.grading.vignette = 0.5f;
        p.grading.vignette_radius = 0.45f;
        p.grading.vignette_softness = 0.6f;
        p.bloom.intensity = 0.12f;
        p.bloom.threshold = 0.8f;
        p.flare.intensity = 0.1f;
        p.aberration.strength = 0.006f;
    } else {
        p.grading.temperature = -0.1f;
        p.grading.contrast = 0.05f;
        p.grading.saturation = 0.15f;
        p.grading.vignette = 0.3f;
        p.grading.vignette_radius = 0.6f;
        p.grading.vignette_softness = 0.5f;
        p.bloom.intensity = 0.08f;
        p.bloom.threshold = 1.4f;    // the sun and the lamps, not the stars
        p.flare.intensity = 0.15f;
        p.aberration.strength = 0.004f;
    }
    return p;
}

// How each planet is made, by name, as data: the tones for its sea, land and
// peaks (its land itself is the simulation's, in the Planet component, so that
// both agree), or its bands when it is a gas giant; its clouds, the glow of its atmosphere, what shines
// on its night side, the tint along its terminator; and its air, as met on the way
// down: how high it reaches, the sky from inside, the haze and its cloud deck. A
// planet not listed is made from its colour. (An air's centre and radius are the
// planet's, filled in per planet, as are its cloud layer's cover and seed, which
// are the land's. The plasma entry is off, its speed 0: ships here come down
// slowly, and the air and the land do the arriving.)
struct planet_look {
    const char *name;
    r_planet_params surface;     // its land's numbers are the game's, filled in per planet
    r_atmosphere air;
    float entry_shake;           // metres the camera shakes at the entry's full strength
};
static const planet_look PLANET_LOOKS[] = {
    {"Verdant",
     {{0.06f, 0.2f, 0.3f}, {0.22f, 0.5f, 0.2f}, {0.85f, 0.85f, 0.8f}, 0.0f, 0.0f, 3.0f, 2.5f,
      {0.9f, 0.93f, 0.95f}, 0.45f, {0.3f, 0.85f, 0.7f}, 1.2f, 3.0f, {0.9f, 0.75f, 0.35f}, 0.12f, {1.0f, 0.6f, 0.4f}, 0.15f,
      0, 0, 0, 0, 0.92f, 0, 0, 0},
     {{0, 0, 0}, 0.0f, 520.0f, 1.6f, {0.2f, 0.42f, 0.75f}, {0.55f, 0.7f, 0.8f}, {0.45f, 0.6f, 0.7f}, 0.0015f, 0.9f, 0.6f,
      45.0f, 24.0f, 0.03f, {0.85f, 0.88f, 0.9f}, 0.0f, 7.0f, 4.0f, 0.55f, 0.004f, 0, 0.0f, {1.0f, 0.55f, 0.2f}}, 0.0f},
    {"Ember",
     {{1.0f, 0.3f, 0.05f}, {0.16f, 0.09f, 0.07f}, {0.4f, 0.35f, 0.33f}, 0.0f, 0.0f, 4.0f, 3.0f,
      {0.3f, 0.2f, 0.17f}, 0.25f, {1.0f, 0.45f, 0.15f}, 1.0f, 4.0f, {1.2f, 0.35f, 0.05f}, 0.5f, {1.0f, 0.5f, 0.3f}, 0.2f,
      0, 0, 0, 0, 0.85f, 0, 0, 0},
     {{0, 0, 0}, 0.0f, 640.0f, 1.6f, {0.3f, 0.1f, 0.05f}, {0.8f, 0.35f, 0.12f}, {0.6f, 0.25f, 0.1f}, 0.002f, 0.8f, 1.0f,
      70.0f, 36.0f, 0.02f, {0.35f, 0.2f, 0.15f}, 0.0f, 6.0f, 3.0f, 0.4f, 0.006f, 0, 0.0f, {1.0f, 0.4f, 0.1f}}, 0.0f},
    {"Azure",
     {{0.05f, 0.15f, 0.5f}, {0.45f, 0.62f, 0.8f}, {0.95f, 0.97f, 1.0f}, 0.0f, 0.0f, 2.5f, 2.0f,
      {0.85f, 0.9f, 1.0f}, 0.2f, {0.4f, 0.6f, 1.0f}, 1.5f, 3.0f, {0.2f, 0.4f, 0.9f}, 0.1f, {0.8f, 0.7f, 1.0f}, 0.15f,
      0, 0, 0, 0, 0.55f, 0, 0, 0},
     {{0, 0, 0}, 0.0f, 580.0f, 1.6f, {0.18f, 0.32f, 0.8f}, {0.6f, 0.7f, 0.95f}, {0.45f, 0.55f, 0.85f}, 0.0015f, 0.95f, 0.5f,
      55.0f, 28.0f, 0.03f, {0.9f, 0.93f, 1.0f}, 0.0f, 8.0f, 3.0f, 0.6f, 0.01f, 0, 0.0f, {0.6f, 0.7f, 1.0f}}, 0.0f},
};

static const planet_look *planet_look_of(const Planet *planet) {
    for (size_t i = 0; i < sizeof(PLANET_LOOKS) / sizeof(PLANET_LOOKS[0]); ++i)
        if (!strcmp(PLANET_LOOKS[i].name, planet->name)) return &PLANET_LOOKS[i];
    return NULL;
}

// The look's colours with the game's land: the height field the simulation stands
// players on is the one the client shaped the planet's mesh by, to the same
// numbers and the same seed, so what is walked on is what is seen.
static r_planet_params planet_surface(const Planet *planet, v3 color) {
    const planet_look *look = planet_look_of(planet);
    r_planet_params p = {};
    if (look) {
        p = look->surface;
    } else {
        p.base = v3_scale(color, 0.5f);
        p.second = color;
        p.third = v3_add(color, v3_make(0.3f, 0.3f, 0.3f));
        p.detail = 3.0f;
        p.contrast = 2.0f;
        p.snow_line = 0.8f;
        p.atmosphere = color;
        p.atmosphere_strength = 1.0f;
        p.atmosphere_power = 3.0f;
        p.terminator_tint = v3_make(1.0f, 1.0f, 1.0f);
    }
    p.continents = planet->land.continents;
    p.relief = planet->land.relief;
    p.sea_level = planet->land.sea_level;
    p.mountains = planet->land.mountains;
    p.shore = planet->land.shore;
    p.pad_radius = planet->land.pad_radius;
    p.seed = planet->land.seed;
    return p;
}

// A planet's landmass, as a mesh: the sphere displaced by the very height field
// the simulation stands players on (game_planet_height), with the normals taken
// from the heights a step either way, so the land that is drawn is the land that
// is walked on.
struct planet_meshes {
    struct { uint32_t seed; int level; r_mesh mesh; } items[MAX_PLANET_MESHES];
    uint32_t count;
};

static int planet_lod_for(float distance_in_radii) {
    int level = 0;
    for (int i = 1; i < PLANET_LOD_COUNT; ++i)
        if (distance_in_radii >= PLANET_LODS[i].beyond) level = i;
    return level;
}

static v3 planet_direction(float polar, float around) {
    return v3_make(sinf(polar) * sinf(around), cosf(polar), sinf(polar) * cosf(around));
}

static r_mesh planet_mesh_make(renderer *r, const PlanetLand *land, int level) {
    const int segments = PLANET_LODS[level].segments, rings = segments / 2;
    const int stride = segments + 1;
    uint32_t vertex_count = (uint32_t)(stride * (rings + 1));
    uint32_t index_count = (uint32_t)(segments * rings * 6);
    r_vertex *vertices = (r_vertex *)malloc(vertex_count * sizeof(r_vertex));
    uint32_t *indices = (uint32_t *)malloc(index_count * sizeof(uint32_t));
    // The field once per vertex, into the grid; the normals then come from the
    // neighbours in it, so the land costs one evaluation a vertex and no more.
    v3 *points = (v3 *)malloc(vertex_count * sizeof(v3));
    for (int ring = 0; ring <= rings; ++ring) {
        float polar = TP_PI * (float)ring / (float)rings;
        for (int segment = 0; segment <= segments; ++segment) {
            v3 dir = planet_direction(polar, 2.0f * TP_PI * (float)segment / (float)segments);
            points[ring * stride + segment] = v3_scale(dir, land->radius + game_planet_height(land, dir));
        }
    }
    r_vertex *v = vertices;
    for (int ring = 0; ring <= rings; ++ring) {
        for (int segment = 0; segment <= segments; ++segment) {
            v3 here = points[ring * stride + segment];
            v3 along = points[(ring < rings ? ring + 1 : ring - 1) * stride + segment];
            v3 across = points[ring * stride + (segment < segments ? segment + 1 : segment - 1)];
            if (ring == rings) along = v3_sub(v3_scale(here, 2.0f), along);      // at the poles, the other way about
            if (segment == segments) across = v3_sub(v3_scale(here, 2.0f), across);
            v3 dir = v3_normalize(here);
            v3 normal = v3_normalize(v3_cross(v3_sub(along, here), v3_sub(across, here)));
            if (v3_dot(normal, dir) < 0.0f) normal = v3_scale(normal, -1.0f);
            if (v3_dot(normal, normal) < 0.5f) normal = dir;                     // a degenerate corner: the sphere's own
            v3 position = v3_scale(here, 1.0f / land->radius);                   // a unit sphere: the draw scales it
            v->position[0] = position.x;
            v->position[1] = position.y;
            v->position[2] = position.z;
            v->normal[0] = normal.x;
            v->normal[1] = normal.y;
            v->normal[2] = normal.z;
            ++v;
        }
    }
    free(points);
    uint32_t *out = indices;
    for (uint32_t ring = 0; ring < (uint32_t)rings; ++ring)
        for (uint32_t segment = 0; segment < (uint32_t)segments; ++segment) {
            uint32_t top = ring * (uint32_t)stride + segment, bottom = top + (uint32_t)stride;
            *out++ = top;     *out++ = bottom;      *out++ = bottom + 1;
            *out++ = top;     *out++ = bottom + 1;  *out++ = top + 1;
        }
    r_mesh mesh = renderer_mesh_create(r, vertices, vertex_count, indices, index_count);
    free(vertices);
    free(indices);
    return mesh;
}

// Every level at once, the first time a planet is seen: that is while the world
// loads, whereas the finest level would otherwise be made in the middle of the
// flight down, a dropped frame just as the land comes up.
static r_mesh planet_mesh_of(renderer *r, planet_meshes *cache, const PlanetLand *land, int level) {
    for (uint32_t i = 0; i < cache->count; ++i)
        if (cache->items[i].seed == land->seed && cache->items[i].level == level) return cache->items[i].mesh;
    if (cache->count + PLANET_LOD_COUNT > MAX_PLANET_MESHES) return 0;
    r_mesh wanted = 0;
    for (int made = 0; made < PLANET_LOD_COUNT; ++made) {
        r_mesh mesh = planet_mesh_make(r, land, made);
        cache->items[cache->count].seed = land->seed;
        cache->items[cache->count].level = made;
        cache->items[cache->count].mesh = mesh;
        ++cache->count;
        if (made == level) wanted = mesh;
    }
    return wanted;
}

// The cloud layer's parameters: the planet's clouds, as a layer of their own, in
// the air's colour and pattern.
static r_cloud_params cloud_layer(const r_atmosphere *air) {
    r_cloud_params c = {};
    c.color = air->cloud_color;
    c.cover = air->cloud_cover;
    c.detail = air->cloud_detail;
    c.sharpness = air->cloud_sharpness;
    c.drift = air->cloud_drift;
    c.translucency = air->cloud_translucency;
    c.seed = air->cloud_seed;
    return c;
}

// A planet's air about it, from its look, or made from its colour.
static r_atmosphere planet_air(const Planet *planet, v3 centre, float radius, v3 color) {
    const planet_look *look = planet_look_of(planet);
    r_atmosphere a;
    if (look) {
        a = look->air;
    } else {
        memset(&a, 0, sizeof(a));
        a.height = radius * 1.2f;
        a.zenith = v3_scale(color, 0.4f);
        a.horizon = v3_scale(color, 0.8f);
        a.haze = v3_scale(color, 0.7f);
        a.density = 1.0f / radius;
        a.star_fade = 0.8f;
        a.sun_glow = 0.5f;
        a.cloud_detail = 7.0f;
        a.cloud_sharpness = 4.0f;
        a.cloud_translucency = 0.5f;
        a.cloud_drift = 0.005f;
        a.entry_glow = color;
    }
    a.centre = centre;
    a.radius = radius;
    // The cloud layer is the planet's clouds, from the same seed as its land.
    a.cloud_cover = look ? look->surface.cloud_cover : 0.4f;
    a.cloud_seed = planet->land.seed;
    return a;
}

// The frame's tables: the materials (index 0 is the plain surface, the rest are
// added as the draws need them) and the atmospheres, one per planet in the world;
// and the material shaders, looked up once.
#define MAX_ATMOSPHERES 16
struct frame_tables {
    r_shader surface_shader, planet_shader, cloud_shader;
    r_material materials[R_MAX_MATERIALS];
    uint32_t material_count;
    r_atmosphere atmospheres[MAX_ATMOSPHERES];
    float shakes[MAX_ATMOSPHERES];   // each atmosphere's entry shake, metres
    uint32_t atmosphere_count;
    planet_meshes planets;
};

static uint32_t material_push(frame_tables *tables, const r_material *m) {
    if (tables->material_count == 0) tables->material_count = 1;
    if (tables->material_count == R_MAX_MATERIALS) return 0;
    tables->materials[tables->material_count] = *m;
    return tables->material_count++;
}

static void atmosphere_push(frame_tables *tables, const r_atmosphere *a, float shake) {
    if (tables->atmosphere_count == MAX_ATMOSPHERES) return;
    tables->shakes[tables->atmosphere_count] = shake;
    tables->atmospheres[tables->atmosphere_count++] = *a;
}

// The point lights, derived from what is in the world rather than stored in it:
// loot glows in its rarity's colour, a dungeon's exit slab and the lantern over a
// dungeon door are lit, and every ship carries a lantern.
#define MAX_LIGHTS 128
struct light_list {
    r_point_light items[MAX_LIGHTS];
    uint32_t count;
};

static void light_push(light_list *lights, v3 at, float radius, v3 color) {
    if (lights->count == MAX_LIGHTS) return;
    r_point_light *l = &lights->items[lights->count++];
    l->position = at;
    l->radius = radius;
    l->color = color;
}

static void collect_lights(game *g, float alpha, const net_client *net, light_list *lights) {
    cw_world *w = &g->world;
    lights->count = 0;
    for (int i = 0; i < g->place_count; ++i) {
        const game_place *place = &g->places[i];
        if (place->kind == PLACE_ENTRANCE) light_push(lights, v3_add(place->center, v3_make(0, 1.6f, 0)), 12.0f, v3_make(4.0f, 3.0f, 1.8f));
        if (place->kind == PLACE_EXIT) light_push(lights, place->center, 12.0f, v3_make(5.0f, 4.5f, 2.5f));
        if (place->kind == PLACE_LANDMARK) light_push(lights, place->center, 60.0f, v3_make(14.0f, 9.0f, 4.0f));   // the monument's beacon
        if (place->kind == PLACE_YARD) light_push(lights, v3_add(place->center, v3_make(0, 2.0f, -1.0f)), 10.0f, v3_make(3.0f, 2.6f, 1.6f));
    }
    // A craft carries a lantern over its deck, and its thrusters glow as they fire,
    // on the side they blow from. (Only the craft we fly knows its throttles.)
    cw_query crafts = cw_query_begin(g->q_crafts);
    cw_entity_id craft;
    while (cw_query_next(&crafts, &craft)) {
        const Craft *c = CW_GET(w, craft, g->c_craft, Craft);
        v3 at;
        quat q;
        game_craft_pose(g, craft, alpha, &at, &q);
        light_push(lights, v3_add(at, quat_rotate(q, v3_add(c->cabin_center, v3_make(0, c->cabin_half.y, 0)))), 12.0f,
                   v3_make(3.5f, 2.8f, 1.8f));
    }
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id part;
    while (cw_query_next(&parts, &part)) {
        const Attachment *a = CW_GET(w, part, g->c_attachment, Attachment);
        if (!cw_comp_has(w, part, g->c_part) || !cw_comp_has(w, a->ship, g->c_craft)) continue;
        const Part *p = CW_GET(w, part, g->c_part, Part);
        int group = game_part_group(p);
        float burn = group >= 0 ? CW_GET(w, a->ship, g->c_craft, Craft)->throttle[group] : 0.0f;
        if (burn < 0.02f) continue;
        v3 at;
        quat q;
        game_craft_pose(g, a->ship, alpha, &at, &q);
        v3 exhaust = v3_add(a->offset, v3_scale(v3_normalize(p->thrust), -0.6f));
        light_push(lights, v3_add(at, quat_rotate(q, exhaust)), 2.0f + 4.0f * burn, v3_scale(v3_make(4.0f, 1.8f, 0.6f), burn));
    }
    cw_query ships = cw_query_begin(g->q_ships);
    cw_entity_id id;
    while (cw_query_next(&ships, &id)) {
        const Transform *t = CW_GET(w, id, g->c_transform, Transform);
        light_push(lights, v3_add(v3_lerp(t->previous, t->position, alpha), v3_make(0, 2.5f, 0)), 14.0f, v3_make(4.0f, 3.2f, 2.0f));
    }
    cw_query loot = cw_query_begin(g->q_loot);
    while (cw_query_next(&loot, &id)) {
        if (net && !net_client_entity_visible(net, id)) continue;
        const Transform *t = CW_GET(w, id, g->c_transform, Transform);
        float rgb[3];
        item_rarity_color(CW_GET(w, id, g->c_loot, Loot)->it.rarity, rgb);
        light_push(lights, v3_add(t->position, v3_make(0, 0.5f, 0)), 5.0f, v3_scale(v3_make(rgb[0], rgb[1], rgb[2]), 3.0f));
    }
}

// The title bar: where the player stands relative to the ships, after `prefix`.
static void set_status(plat_window *window, game *g, cw_entity_id player, const char *prefix) {
    cw_world *w = &g->world;
    if (!cw_world_is_valid(w, player)) {
        plat_window_set_title(window, prefix);
        return;
    }
    v3 at = CW_GET(w, player, g->c_transform, Transform)->position;
    cw_entity_id craft = game_aboard(g, player);
    if (craft != CW_INVALID_ENTITY) {
        const Craft *c = CW_GET(w, craft, g->c_craft, Craft);
        char text[320];
        snprintf(text, sizeof(text), "%s: %s the %s, %.0f m/s, %s", prefix, c->pilot == player ? "at the helm of" : "aboard",
                 c->name, v3_length(c->velocity), c->pilot == CW_INVALID_ENTITY ? "nobody at the helm" : c->assist ? "assist on" : "decoupled");
        plat_window_set_title(window, text);
        return;
    }
    cw_entity_id aboard = game_ship_at(g, at);
    cw_entity_id shown = aboard;
    if (shown == CW_INVALID_ENTITY) {
        // Otherwise the nearest ship that can be walked onto right now.
        float best = 1e30f;
        cw_query ships = cw_query_begin(g->q_ships);
        cw_entity_id id;
        while (cw_query_next(&ships, &id)) {
            if (CW_GET(w, id, g->c_ship, Ship)->state != SHIP_DOCKED) continue;
            float distance = v3_length(v3_sub(CW_GET(w, id, g->c_transform, Transform)->position, at));
            if (distance < best) {
                best = distance;
                shown = id;
            }
        }
    }
    char text[320];
    if (shown == CW_INVALID_ENTITY) {
        snprintf(text, sizeof(text), "%s: every ship is away", prefix);
    } else {
        const Ship *ship = CW_GET(w, shown, g->c_ship, Ship);
        const Planet *planet = CW_GET(w, ship->planet, g->c_planet, Planet);
        const char *place = ship->stop == STOP_PLANET ? planet->name : "the hub";
        if (ship->state == SHIP_FLYING)
            snprintf(text, sizeof(text), "%s: aboard %s, reaching %s in %.0f s",
                     prefix, ship->name, place, ship->flight_seconds - ship->timer);
        else
            snprintf(text, sizeof(text), "%s: %s %s at %s, leaves in %.0f s",
                     prefix, aboard != CW_INVALID_ENTITY ? "aboard" : "nearest:", ship->name, place,
                     ship->dock_seconds - ship->timer);
    }
    plat_window_set_title(window, text);
}

static bool write_bmp(const char *path, const uint8_t *rgba, int width, int height) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t pixels = (uint32_t)width * (uint32_t)height * 4;
    uint32_t file_header[3] = {54u + pixels, 0, 54};
    int32_t info[10] = {40, width, -height, 1 | (32 << 16), 0, (int32_t)pixels, 2835, 2835, 0, 0};
    fwrite("BM", 1, 2, f);
    fwrite(file_header, 4, 3, f);
    fwrite(info, 4, 10, f);
    uint8_t *row = (uint8_t *)malloc((size_t)width * 4);
    for (int y = 0; y < height; ++y) {
        const uint8_t *in = rgba + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x) {
            row[x * 4] = in[x * 4 + 2];
            row[x * 4 + 1] = in[x * 4 + 1];
            row[x * 4 + 2] = in[x * 4];
            row[x * 4 + 3] = 255;
        }
        fwrite(row, 4, (size_t)width, f);
    }
    free(row);
    fclose(f);
    return true;
}

// With --stats, once a second, averaged over the second: the frame (with the worst
// one), the CPU's time ticking the world and building the frame's draws, and the
// GPU's time per pass.
struct stats_log {
    bool on;
    double started, previous;
    double tick_seconds, build_seconds, worst, worst_tick, worst_build;
    r_timings sum;
    int frames, timed_frames;
};

static double frame_build_seconds;    // draw_world's time up to the submit, for the log

static void stats_frame(stats_log *log, renderer *r, double now, double tick_seconds) {
    if (!log->on) return;
    if (log->previous == 0.0) {
        log->started = log->previous = now;
        return;
    }
    double frame = now - log->previous;
    log->previous = now;
    if (frame > log->worst) log->worst = frame;
    if (tick_seconds > log->worst_tick) log->worst_tick = tick_seconds;
    if (frame_build_seconds > log->worst_build) log->worst_build = frame_build_seconds;
    log->tick_seconds += tick_seconds;
    log->build_seconds += frame_build_seconds;
    ++log->frames;
    r_timings t = renderer_timings(r);
    if (t.valid) {
        for (int p = 0; p < R_PASS_COUNT; ++p) log->sum.ms[p] += t.ms[p];
        log->sum.total_ms += t.total_ms;
        ++log->timed_frames;
    }
    if (now - log->started < 1.0) return;
    double per_frame = 1000.0 / log->frames;
    float per_timed = log->timed_frames ? 1.0f / (float)log->timed_frames : 0.0f;
    char line[512];
    int n = snprintf(line, sizeof(line), "frame %.2f ms (worst %.2f), tick %.2f (worst %.2f), build %.2f (worst %.2f), gpu %.2f:",
                     (now - log->started) * per_frame, log->worst * 1000.0, log->tick_seconds * per_frame, log->worst_tick * 1000.0,
                     log->build_seconds * per_frame, log->worst_build * 1000.0, log->sum.total_ms * per_timed);
    for (int p = 0; p < R_PASS_COUNT; ++p)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %.2f", r_pass_name(p), log->sum.ms[p] * per_timed);
    fputs(line, stderr);
    fputc('\n', stderr);
    double previous = log->previous;
    bool on = log->on;
    memset(log, 0, sizeof(*log));
    log->on = on;
    log->started = log->previous = previous;
}

// Where a player's eyes are, from their feet.
static v3 eye_of(v3 feet) { return v3_add(feet, v3_make(0, PLAYER_EYE, 0)); }

// The camera shakes with an atmosphere's entry: by its strength, the look's
// amount, hashed from the time so it never repeats.
static v3 entry_shake(const frame_tables *tables, v3 eye, v3 velocity, double time) {
    float strength = 0.0f, amount = 0.0f;
    for (uint32_t i = 0; i < tables->atmosphere_count; ++i) {
        float e = r_atmosphere_entry(&tables->atmospheres[i], eye, velocity);
        if (e > strength) {
            strength = e;
            amount = tables->shakes[i];
        }
    }
    if (strength <= 0.0f) return v3_make(0, 0, 0);
    uint32_t t = (uint32_t)(time * 47.0);
    float x = (float)((t * 1664525u + 1013904223u) >> 8) / 16777216.0f - 0.5f;
    float y = (float)(((t + 7u) * 1664525u + 1013904223u) >> 8) / 16777216.0f - 0.5f;
    return v3_scale(v3_make(x, y, 0), strength * amount * 2.0f);
}

// Draws the world from `player`'s eyes (or from the spawn while there is no player
// yet). Other players are their capsule plus a small block at the eyes, pointing
// where they look. `net`, when set, hides replicas the server stopped updating.
// `velocity` is the camera's, for an atmosphere's entry.
static void draw_world(renderer *r, plat_window *window, game *g, cw_entity_id player, const view_state *view,
                       float alpha, double time, v3 velocity, const r_mesh *meshes, r_mesh cloud_mesh, draw_list *list,
                       light_list *lights, frame_tables *tables, const net_client *net, const party_view *party,
                       const char *my_name, uint8_t **capture) {
    double build_start = plat_seconds();
    cw_world *w = &g->world;
    int width, height;
    plat_window_size(window, &width, &height);
    if (width <= 0 || height <= 0) return;

    // The eye, where the player's frame has it: in a craft, the craft's pose as drawn
    // with the player's place on it, so the deck and the eye never part.
    v3 eye = eye_of(g->spawn), forward = look_direction(view->yaw, view->pitch), up = v3_make(0, 1, 0);
    cw_entity_id craft = craft_of(g, net, player);
    if (craft != CW_INVALID_ENTITY && cw_comp_has(w, player, g->c_aboard)) {
        v3 at;
        quat q;
        game_craft_pose(g, craft, alpha, &at, &q);
        const Aboard *a = CW_GET(w, player, g->c_aboard, Aboard);
        eye = v3_add(at, quat_rotate(q, eye_of(v3_lerp(a->previous, a->position, alpha))));
        forward = quat_rotate(q, forward);
        up = quat_rotate(q, up);
        if (net) eye = v3_add(eye, net_client_view_offset(net));
        if (velocity.x == 0.0f && velocity.y == 0.0f && velocity.z == 0.0f) velocity = CW_GET(w, craft, g->c_craft, Craft)->velocity;
    } else if (cw_world_is_valid(w, player)) {
        const Transform *pt = CW_GET(w, player, g->c_transform, Transform);
        eye = eye_of(v3_lerp(pt->previous, pt->position, alpha));
        if (net) eye = v3_add(eye, net_client_view_offset(net));   // a fading misprediction
        if (velocity.x == 0.0f && velocity.y == 0.0f && velocity.z == 0.0f)
            velocity = v3_scale(v3_sub(pt->position, pt->previous), (float)GAME_TICK_HZ);
    }
    eye = v3_add(eye, entry_shake(tables, eye, velocity, time));   // last frame's atmospheres: a frame late is fine

    r_frame frame = {};
    frame.time = (float)time;
    frame.camera.position = eye;
    frame.camera.forward = forward;
    frame.camera.up = up;
    frame.camera.velocity = velocity;
    frame.camera.fov_y = HUD_FOV;
    frame.camera.near_plane = 0.1f;
    frame.environment = environment_of(g);
    frame.post = post_of(g);
    collect_lights(g, alpha, net, lights);
    frame.lights = lights->items;
    frame.light_count = lights->count;
    // Material 0: the plain surface, with the metre checker.
    r_surface_params plain = {0.08f};
    tables->materials[0] = r_material_surface(tables->surface_shader, &plain);
    tables->material_count = 1;
    tables->atmosphere_count = 0;

    list->count = 0;
    cw_component_id drawn[] = {g->c_transform, g->c_visual};
    cw_query visuals = cw_query_iter(w, cw_mask(drawn, 2));
    cw_entity_id id;
    while (cw_query_next(&visuals, &id)) {
        if (id == player) continue;                              // first person: no body in view
        if (net && !net_client_entity_visible(net, id)) continue;
        const Transform *t = CW_GET(w, id, g->c_transform, Transform);
        const Visual *visual = CW_GET(w, id, g->c_visual, Visual);
        v3 at = v3_lerp(t->previous, t->position, alpha);
        r_draw *draw = draw_push(list);
        draw->mesh = meshes[visual->mesh];
        draw->model = m4_translate_scale(at, visual->half);
        // A craft's parts, and whoever stands in a craft, turn with it: drawn from its
        // pose as drawn this frame, so nothing on it lags it.
        cw_entity_id carrier = CW_INVALID_ENTITY;
        if (cw_comp_has(w, id, g->c_attachment) && cw_comp_has(w, CW_GET(w, id, g->c_attachment, Attachment)->ship, g->c_craft))
            carrier = CW_GET(w, id, g->c_attachment, Attachment)->ship;
        quat turned = quat_identity();
        if (carrier != CW_INVALID_ENTITY) {
            v3 origin;
            game_craft_pose(g, carrier, alpha, &origin, &turned);
            at = v3_add(origin, quat_rotate(turned, CW_GET(w, id, g->c_attachment, Attachment)->offset));
            draw->model = m4_pose(at, turned, visual->half);
        } else if (cw_comp_has(w, id, g->c_player) && (carrier = craft_of(g, net, id)) != CW_INVALID_ENTITY) {
            v3 origin;
            game_craft_pose(g, carrier, alpha, &origin, &turned);
            draw->model = m4_pose(at, turned, visual->half);
        }
        draw->color[0] = visual->color.x;
        draw->color[1] = visual->color.y;
        draw->color[2] = visual->color.z;
        draw->color[3] = 1.0f;
        draw->flags = 0;
        if (cw_comp_has(w, id, g->c_planet)) {
            const Planet *planet = CW_GET(w, id, g->c_planet, Planet);
            const planet_look *look = planet_look_of(planet);
            r_atmosphere air = planet_air(planet, at, visual->half.x, visual->color);
            atmosphere_push(tables, &air, look ? look->entry_shake : 0.0f);
            float altitude = v3_length(v3_sub(eye, at)) - visual->half.x;
            float radii = altitude / visual->half.x;
            uint32_t shadows = altitude < PLANET_SHADOW_ALTITUDE ? 0 : R_DRAW_NO_SHADOW | R_DRAW_UNSHADOWED;
            draw->flags |= shadows;
            r_planet_params surface = planet_surface(planet, visual->color);
            bool layer = air.cloud_altitude > 0.0f && air.cloud_cover > 0.0f;
            if (layer) {
                // The clouds are a layer of their own above the land, not painted on
                // it. It casts no shadow of its own: the lit pass shadows by the
                // layer's shape, which is far cheaper than the layer in every cascade.
                r_cloud_params clouds = cloud_layer(&air);
                surface.cloud_cover = 0.0f;
                r_material cloud_material = r_material_clouds(tables->cloud_shader, &clouds);
                r_draw *deck = draw_push(list);
                deck->mesh = radii < CLOUD_LOD_BEYOND ? cloud_mesh : meshes[MESH_SPHERE];
                float reach = visual->half.x + air.cloud_altitude;
                deck->model = m4_translate_scale(at, v3_make(reach, reach, reach));
                deck->color[0] = deck->color[1] = deck->color[2] = deck->color[3] = 1.0f;
                deck->flags = R_DRAW_TWO_SIDED | R_DRAW_NO_SHADOW | shadows;
                deck->material = material_push(tables, &cloud_material);
            }
            r_material planet_material = r_material_planet(tables->planet_shader, &surface);
            draw->material = material_push(tables, &planet_material);
            if (planet->land.relief > 0.0f) {
                r_mesh land = planet_mesh_of(r, &tables->planets, &planet->land, planet_lod_for(radii));
                if (land) draw->mesh = land;
            }
        }
        if (net && cw_comp_has(w, id, net->c_replica) && CW_GET(w, id, net->c_replica, Replica)->flash > 0.0f) {
            // Just tagged: a white flash that fades.
            float f = CW_GET(w, id, net->c_replica, Replica)->flash / NET_HIT_FLASH;
            for (int k = 0; k < 3; ++k) draw->color[k] += (1.0f - draw->color[k]) * f;
        }
        if (cw_comp_has(w, id, g->c_player)) {
            const Player *p = CW_GET(w, id, g->c_player, Player);
            // Their angles are in the frame they stand in: the craft's, aboard one.
            v3 looking = quat_rotate(turned, look_direction(p->yaw, p->pitch));
            float world_yaw, world_pitch;
            look_angles(quat_rotate(turned, look_direction(p->yaw, 0.0f)), &world_yaw, &world_pitch);
            // Their gear, for everyone to see, always.
            if (cw_comp_has(w, id, g->c_equipment)) hud_draw_gear(list, meshes[MESH_CUBE], at, world_yaw, CW_GET(w, id, g->c_equipment, Equipment));
            v3 nose = v3_add(v3_add(at, quat_rotate(turned, v3_make(0, PLAYER_EYE, 0))), v3_scale(looking, PLAYER_RADIUS));
            r_draw *face = draw_push(list);
            face->mesh = meshes[MESH_CUBE];
            face->model = m4_translate_scale(nose, v3_make(0.08f, 0.08f, 0.08f));
            face->color[0] = face->color[1] = face->color[2] = 0.1f;
            face->color[3] = 1.0f;
            face->flags = 0;
        }
    }
    // The overlay, in the camera's frame: the party panel top right, gear bottom
    // right, and what loot we are looking at (online); at a helm, the flight panel
    // and the stick; and what the use key would do here.
    hud_frame hud = hud_begin_view(eye, forward, up, (float)width / (float)height, meshes[MESH_CUBE]);
    if (party) {
        hud_party(list, &hud, party, my_name);
        if (net && net->gear_known) hud_gear(list, &hud, &net->gear, &net->last_pickup, net->pickup_flash);
        item looked_at;
        if (net && net_client_loot_in_view(net, 6.0f, &looked_at)) hud_loot(list, &hud, &looked_at);
    }
    if (cw_world_is_valid(w, player)) {
        const v3 advice = v3_make(0.95f, 0.85f, 0.5f), refusal = v3_make(0.95f, 0.45f, 0.4f);
        if (craft != CW_INVALID_ENTITY && cw_comp_has(w, player, g->c_aboard)) {
            const Craft *c = CW_GET(w, craft, g->c_craft, Craft);
            if (c->pilot == player) {
                hud_flight_view flight = {};
                flight.craft = c->name;
                flight.speed = v3_length(c->velocity);
                flight.climb = c->velocity.y;
                flight.assist = !view->decoupled;
                flight.stick_x = view->stick_x;
                flight.stick_y = view->stick_y;
                memcpy(flight.throttle, c->throttle, sizeof(flight.throttle));
                hud_flight(list, &hud, &flight);
            } else if (v3_length(v3_sub(CW_GET(w, player, g->c_aboard, Aboard)->position, c->helm)) <= CRAFT_HELM_REACH) {
                bool free = c->pilot == CW_INVALID_ENTITY || !cw_world_is_valid(w, c->pilot);
                if (!game_can_pilot(g, player, craft)) hud_prompt(list, &hud, "THIS HELM IS NOT YOURS", refusal);
                else if (!free) hud_prompt(list, &hud, "SOMEONE IS AT THE HELM", refusal);
                else hud_prompt(list, &hud, "F  TAKE THE HELM", advice);
            }
        } else {
            v3 feet = CW_GET(w, player, g->c_transform, Transform)->position;
            for (int i = 0; i < g->place_count; ++i) {
                const game_place *place = &g->places[i];
                if (place->kind == PLACE_YARD && fabsf(feet.x - place->center.x) <= place->half.x
                    && fabsf(feet.y - place->center.y) <= place->half.y && fabsf(feet.z - place->center.z) <= place->half.z)
                    hud_prompt(list, &hud, game_craft_of(g, game_owner(g, player)) != CW_INVALID_ENTITY
                                               ? "F  TOW YOUR SHIP BACK TO ITS BERTH" : "F  CALL YOUR SHIP", advice);
            }
        }
    }
    frame.draws = list->items;
    frame.draw_count = (uint32_t)list->count;
    frame.materials = tables->materials;
    frame.material_count = tables->material_count;
    frame.atmospheres = tables->atmospheres;
    frame.atmosphere_count = tables->atmosphere_count;
    if (capture) {
        *capture = (uint8_t *)malloc((size_t)width * (size_t)height * 4);
        frame.capture_rgba = *capture;
    }
    frame_build_seconds = plat_seconds() - build_start;
    renderer_submit(r, &frame, width, height);
}

static float stick_axis(float v) {
    float size = fabsf(v);
    if (size <= STICK_DEADZONE) return 0.0f;
    return (v < 0.0f ? -1.0f : 1.0f) * (size - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
}

// The keyboard and mouse, into one tick's worth of input. On foot the mouse looks
// and W A S D walk. At a helm the mouse pushes the flight stick (it stays where it
// is pushed, and turns the craft as far as it is from the middle), W and S thrust
// forward and back, A and D slide, Space and C (or Ctrl) lift and sink, Q and E
// roll, and X switches the flight computer off and on. F uses what is at hand: a
// helm, or a yard's terminal. The look persists between frames, in `v`.
static PlayerInput read_input(plat_window *window, view_state *v) {
    const plat_input *input = plat_window_input(window);
    if (input->keys[PLAT_KEY_ESCAPE]) plat_window_capture_mouse(window, false);
    if (input->clicked) plat_window_capture_mouse(window, true);
    bool use = input->pressed['F'];
    if (input->pressed['X']) v->decoupled = !v->decoupled;
    PlayerInput in = {};
    float w = input->keys['W'] ? 1.0f : 0.0f, s = input->keys['S'] ? 1.0f : 0.0f;
    float a = input->keys['A'] ? 1.0f : 0.0f, d = input->keys['D'] ? 1.0f : 0.0f;
    in.forward = w - s;
    in.strafe = d - a;
    if (v->piloting) {
        if (input->mouse_captured) {
            v->stick_x += input->mouse_dx * STICK_PER_COUNT;
            v->stick_y -= input->mouse_dy * STICK_PER_COUNT;   // the mouse away from you: the nose up
            float throw_ = sqrtf(v->stick_x * v->stick_x + v->stick_y * v->stick_y);
            if (throw_ > 1.0f) {
                v->stick_x /= throw_;
                v->stick_y /= throw_;
            }
        }
        in.lift = (input->keys[PLAT_KEY_SPACE] ? 1.0f : 0.0f)
                - (input->keys['C'] || input->keys[PLAT_KEY_LEFT_CONTROL] ? 1.0f : 0.0f);
        in.turn_roll = (input->keys['E'] ? 1.0f : 0.0f) - (input->keys['Q'] ? 1.0f : 0.0f);
        in.turn_yaw = stick_axis(v->stick_x);
        in.turn_pitch = stick_axis(v->stick_y);
        v->yaw = v->pitch = 0.0f;   // looking out over the bow
    } else {
        if (input->mouse_captured) {
            v->yaw -= input->mouse_dx * MOUSE_RADIANS_PER_COUNT;
            v->pitch -= input->mouse_dy * MOUSE_RADIANS_PER_COUNT;
            if (v->pitch > MAX_PITCH) v->pitch = MAX_PITCH;
            if (v->pitch < -MAX_PITCH) v->pitch = -MAX_PITCH;
        }
        in.jump = input->keys[PLAT_KEY_SPACE];
        in.act = input->clicked && input->mouse_captured;   // a click while playing is a swing
    }
    in.use = use;
    in.decoupled = v->decoupled;
    in.yaw = v->yaw;
    in.pitch = v->pitch;
    return in;
}

static const char *net_state_name(net_client_state state) {
    switch (state) {
    case NC_CONNECTING: return "connecting";
    case NC_LOADING: return "loading the world";
    case NC_CONNECTED: return "online";
    default: return "disconnected";
    }
}

// Online: the world is the server's. The session predicts our player and the ships
// and interpolates everyone else; we hand it the input and draw what it holds.
static int run_online(const options *o, plat_window *window, renderer *r, const char *backend,
                      const r_mesh *meshes, r_mesh cloud_mesh, draw_list *list, light_list *lights, frame_tables *tables,
                      view_state *view) {
    stats_log stats = {};
    stats.on = o->stats;
    if (!net_init()) {
        fputs("the network could not be initialised\n", stderr);
        return 1;
    }
    net_session session;
    net_identity identity = net_identity_local(o->name);
    if (o->region) strncpy(identity.region, o->region, NET_REGION_NAME - 1);
    bool ok = o->directory ? net_session_init_directory(&session, o->directory, o->place, NET_INTERP_DELAY, &identity)
                           : net_session_init(&session, o->connect, NET_INTERP_DELAY, &identity);
    if (!ok) {
        fprintf(stderr, "%s: %s\n", o->directory ? o->directory : o->connect, session.failure);
        net_shutdown();
        return 1;
    }
    fprintf(stderr, "connecting %s %s\n", o->directory ? "through the directory at" : "to", o->directory ? o->directory : o->connect);

    double last_status = 0.0;
    int status = 0;
    game *view_world = NULL;     // the world the view's frame belongs to
    plat_window_capture_mouse(window, true);
    while (plat_window_poll(window)) {
        PlayerInput in = read_input(window, view);
        double now = net_seconds();
        // Party keys, on the press: F1 create, I invite whoever is in view, F2 accept,
        // F3 leave, F4 decline.
        const plat_input *input = plat_window_input(window);
        const bool *pressed = input->pressed;
        if (pressed[PLAT_KEY_F1]) net_session_party_request(&session, PARTY_CREATE, NULL);
        if (pressed[PLAT_KEY_F2]) net_session_party_request(&session, PARTY_ACCEPT, NULL);
        if (pressed[PLAT_KEY_F3]) net_session_party_request(&session, PARTY_LEAVE, NULL);
        if (pressed[PLAT_KEY_F4]) net_session_party_request(&session, PARTY_DECLINE, NULL);
        if (pressed['I'] && session.link) {
            char name[ACCOUNT_NAME_CAPACITY];
            if (net_client_player_in_view(session.link, 6.0f, name)) net_session_party_request(&session, PARTY_INVITE, name);
        }
        for (int k = 0; k < 9; ++k)
            if (pressed['1' + k]) net_session_equip(&session, k);   // wear stash item k
        net_session_frame(&session, &in, now);
        double tick_seconds = net_seconds() - now;
        net_client_state state = net_session_state(&session);
        if (state == NC_DISCONNECTED) {
            fprintf(stderr, "disconnected: %s\n", session.failure);
            status = 1;
            break;
        }
        game *g = session.world;
        net_client *link = session.link;
        if (!link) {   // still asking the directory
            if (now - last_status > 0.25) {
                char title[160];
                snprintf(title, sizeof(title), "Treasure Planet [%s] asking %s for %s", backend, o->directory, o->place ? o->place : "hub");
                plat_window_set_title(window, title);
                last_status = now;
            }
            continue;
        }
        cw_entity_id player = state == NC_CONNECTED ? link->player : CW_INVALID_ENTITY;
        if (now - last_status > 0.25) {
            char address[32], prefix[200];
            net_address_format(link->server, address, sizeof(address));
            const Player *me = cw_world_is_valid(&g->world, player) ? CW_GET(&g->world, player, g->c_player, Player) : NULL;
            snprintf(prefix, sizeof(prefix), "Treasure Planet [%s] %s %s, %zu players, %.0f ms input / %.1f ms net, %.0f%% loss, %d hand-offs (%.0f ms), tagged %u/%u%s",
                     backend, address, net_state_name(state), cw_query_count(g->q_all_players), link->rtt * 1000.0f,
                     link->connection.rtt * 1000.0, link->connection.loss * 100.0,
                     session.handoffs, session.last_handoff_seconds * 1000.0f, me ? me->hits_made : 0u,
                     me ? me->hits_taken : 0u, link->hit_flash > 0.0f ? " HIT" : "");
            if (link->world_name[0]) {
                // In a dungeon: no ships to report, only where we are.
                char title[256];
                snprintf(title, sizeof(title), "%s: in %s", prefix, link->world_name);
                plat_window_set_title(window, title);
            } else {
                set_status(window, g, player, prefix);
            }
            last_status = now;
        }
        if (g != view_world) {   // a new world (a hand-off): its entities are not the old one's
            view->frame = CW_INVALID_ENTITY;
            view->piloting = false;
            view_world = g;
        }
        follow_frame(g, link, player, view);
        draw_world(r, window, g, player, view, net_client_alpha(link), now, v3_make(0, 0, 0), meshes, cloud_mesh, list,
                   lights, tables, link, net_session_party(&session), session.identity.name, NULL);
        stats_frame(&stats, r, now, tick_seconds);
    }
    net_session_destroy(&session);
    net_shutdown();
    return status;
}

static int run(const options *o) {
    plat_window *window = plat_window_create("Treasure Planet", 1280, 720);
    if (!window) {
        fputs("could not create a window\n", stderr);
        return 1;
    }
    if (o->bench_seconds > 0.0f) plat_set_vsync(false);
    renderer *r = renderer_create(o->backend, window);
    if (!r) {
        fputs("no renderer could start\n", stderr);
        plat_window_destroy(window);
        return 1;
    }
    plat_window_show(window);
    const char *backend = renderer_backend_name(r);
    fprintf(stderr, "renderer: %s\n", backend);

    r_mesh meshes[MESH_COUNT];
    meshes[MESH_CUBE] = renderer_mesh_cube(r);
    meshes[MESH_SPHERE] = renderer_mesh_sphere(r, 96, 64);
    meshes[MESH_CAPSULE] = renderer_mesh_capsule(r, PLAYER_RADIUS, PLAYER_HEIGHT, 24, 8);
    r_mesh cloud_mesh = renderer_mesh_sphere(r, CLOUD_SEGMENTS, CLOUD_SEGMENTS / 2);

    view_state view = view_make(o->yaw, o->pitch);
    draw_list list = {};
    light_list *lights = (light_list *)calloc(1, sizeof(light_list));
    frame_tables *tables = (frame_tables *)calloc(1, sizeof(frame_tables));
    tables->surface_shader = renderer_shader(r, "surface");
    tables->planet_shader = renderer_shader(r, "planet");
    tables->cloud_shader = renderer_shader(r, "clouds");
    if (!tables->surface_shader || !tables->planet_shader || !tables->cloud_shader)
        fputs("the built-in material shaders are missing: check src/render/shaders/materials\n", stderr);
    int status = 0;

    if (o->connect || o->directory) {
        status = run_online(o, window, r, backend, meshes, cloud_mesh, &list, lights, tables, &view);
        free(list.items);
        free(lights);
        free(tables);
        renderer_destroy(r);
        plat_window_destroy(window);
        return status;
    }

    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    if (o->dungeon) game_build_dungeon(g, o->dungeon, "hub", v3_make(0, 0, 12), 1);
    else game_build_universe(g);
    cw_entity_id player = game_spawn_player(g, o->has_position ? o->position : g->spawn, true);
    if (o->craft && !o->dungeon) {
        // Called to a berth, and we at its helm: stepping aboard boards it, a use takes it.
        cw_entity_id craft = game_call_craft(g, player);
        if (craft != CW_INVALID_ENTITY) {
            game_tick(g);   // the part lists the players step against
            Transform *t = CW_GET(&g->world, player, g->c_transform, Transform);
            t->position = t->previous = game_craft_to_world(g, craft, v3_add(CW_GET(&g->world, craft, g->c_craft, Craft)->helm, v3_make(0, 0.05f, 0)));
            game_tick(g);
            CW_GET(&g->world, player, g->c_input, PlayerInput)->use = true;
            game_tick(g);
            CW_GET(&g->world, player, g->c_input, PlayerInput)->use = false;
        }
    }
    follow_frame(g, NULL, player, &view);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "Treasure Planet [%s]", backend);

    if (o->capture_path) {
        // Scripted: no clock and no input, so every run produces the same picture.
        for (int i = 0; i < o->capture_ticks; ++i) game_tick(g);
        follow_frame(g, NULL, player, &view);
        uint8_t *pixels = NULL;
        // A few frames first, so a fresh swapchain has settled on the window's size.
        for (int i = 0; i < 4 && plat_window_poll(window); ++i)
            draw_world(r, window, g, player, &view, 1.0f, 0.0, o->velocity, meshes, cloud_mesh, &list, lights, tables, NULL, NULL, NULL, NULL);
        plat_window_poll(window);
        draw_world(r, window, g, player, &view, 1.0f, 0.0, o->velocity, meshes, cloud_mesh, &list, lights, tables, NULL, NULL, NULL, &pixels);
        int width, height;
        plat_window_size(window, &width, &height);
        if (!pixels || !write_bmp(o->capture_path, pixels, width, height)) {
            fprintf(stderr, "could not write %s\n", o->capture_path);
            status = 1;
        } else {
            fprintf(stderr, "wrote %s (%dx%d, %zu draws)\n", o->capture_path, width, height, list.count);
        }
        free(pixels);
    } else if (o->bench_seconds > 0.0f) {
        // Nothing ticks, so the same view is measured for as long as asked.
        stats_log stats = {};
        stats.on = true;
        double start = plat_seconds();
        while (plat_window_poll(window) && plat_seconds() - start < o->bench_seconds) {
            double now = plat_seconds();
            draw_world(r, window, g, player, &view, 1.0f, now, o->velocity, meshes, cloud_mesh, &list, lights, tables, NULL, NULL, NULL, NULL);
            stats_frame(&stats, r, now, 0.0);
        }
    } else {
        cw_clock clock = cw_clock_make(GAME_TICK_HZ);
        double last = plat_seconds(), last_status = 0.0;
        stats_log stats = {};
        stats.on = o->stats;
        plat_window_capture_mouse(window, true);
        bool use_latched = false;
        while (plat_window_poll(window)) {
            PlayerInput *in = CW_GET(&g->world, player, g->c_input, PlayerInput);
            *in = read_input(window, &view);
            // The use key is an edge: one tick takes it, however many run this frame.
            if (in->use) use_latched = true;

            double now = plat_seconds();
            int steps = cw_clock_advance(&clock, (float)(now - last));
            last = now;
            for (int i = 0; i < steps; ++i) {
                in->use = use_latched;
                use_latched = false;
                game_tick(g);
            }
            follow_frame(g, NULL, player, &view);
            double tick_seconds = plat_seconds() - now;

            if (now - last_status > 0.25) {
                set_status(window, g, player, prefix);
                last_status = now;
            }
            draw_world(r, window, g, player, &view, cw_clock_alpha(&clock), now, v3_make(0, 0, 0), meshes, cloud_mesh, &list, lights, tables, NULL, NULL, NULL, NULL);
            stats_frame(&stats, r, now, tick_seconds);
        }
    }

    free(list.items);
    free(lights);
    free(tables);
    game_destroy(g);
    free(g);
    renderer_destroy(r);
    plat_window_destroy(window);
    return status;
}

int main(int argc, char **argv) {
    options o;
    if (!parse_options(argc, argv, &o)) {
        fputs("usage: treasure_planet [--renderer vulkan|gl] [--connect host[:port] | --directory host[:port] [--place NAME]] [--name NAME]\n"
              "                       [--dungeon NAME | --craft] [--stats] [--bench SECONDS | --capture out.bmp [--ticks N]] [--at X Y Z] [--look YAW PITCH] [--moving X Y Z]\n", stderr);
        return 2;
    }
    return run(&o);
}
