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
#include "shipwright_ui.h"
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
    bool at_sails;               // at a craft's sail station: W/S/A/D work the sail
    float stick_x, stick_y;      // -1..1
    bool decoupled;
    bool landing;                // landing mode: set on taking the helm of a craft at rest, L switches it
    // At a helm the camera follows the craft from behind (V switches to the helm's
    // own eyes and back); it turns after the craft a little late, which shows the turn.
    bool third_person;
    quat chase;                  // the chase camera's orientation, easing after the craft's
    bool chase_valid;
    double chase_time;
    // Studying a star chart: the chart table's part, or CW_INVALID_ENTITY. The camera
    // leans over its hologram and the HUD reads it out; F or a step ends it.
    cw_entity_id studying;
    // At the shipwright's counter, trading: the screen, and the answers heard so far.
    shipwright_ui shop;
    shipwright_input shop_input; // the pointer as it was this frame, for the screen's hover
    uint32_t shop_answers;
    const character *books;      // the character the screen reads (the server's copy online), or NULL
};

static view_state view_make(float yaw, float pitch) {
    view_state v = {};
    v.yaw = yaw;
    v.pitch = pitch;
    v.frame = CW_INVALID_ENTITY;
    v.frame_orientation = quat_identity();
    v.third_person = true;       // the pilot's camera starts behind the craft
    v.studying = CW_INVALID_ENTITY;
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
    v->at_sails = craft != CW_INVALID_ENTITY && game_craft_station_of(CW_GET(&g->world, craft, g->c_craft, Craft), player) >= 0;
    if (craft != v->frame) {
        v3 direction = quat_rotate(v->frame == CW_INVALID_ENTITY ? quat_identity() : v->frame_orientation,
                                   look_direction(v->yaw, v->pitch));
        if (craft != CW_INVALID_ENTITY) direction = quat_unrotate(CW_GET(&g->world, craft, g->c_craft, Craft)->orientation, direction);
        look_angles(direction, &v->yaw, &v->pitch);
        v->frame = craft;
    }
    if (piloting != v->piloting) {
        v->stick_x = v->stick_y = 0.0f;
        v->chase_valid = false;
        if (piloting) {
            v->yaw = v->pitch = 0.0f;
            v->landing = CW_GET(&g->world, craft, g->c_craft, Craft)->resting;   // on the ground: staying there
        }
        v->piloting = piloting;
    }
    if (craft != CW_INVALID_ENTITY) v->frame_orientation = CW_GET(&g->world, craft, g->c_craft, Craft)->orientation;
    // Docking, the autopilot has the ship: the stick rests in the middle (a push on
    // it would call the docking off), and landing mode is on, for when she is in.
    if (piloting && CW_GET(&g->world, craft, g->c_craft, Craft)->docking != DOCK_NONE) {
        v->stick_x = v->stick_y = 0.0f;
        v->landing = true;
    }
}

// ---------------------------------------------------------------------------
// Star charts: a hologram of the universe over each chart table, for everyone to
// see. It keeps to the world's axes whichever way the ship turns (north stays north
// on the table), and shows the planets, the hub, the liners and every craft, this
// one in gold with a line where its bow points.
// ---------------------------------------------------------------------------
// Where a chart table's hologram floats: over its top, along its craft's up.
static v3 chart_holo(game *g, cw_entity_id part, float alpha) {
    cw_world *w = &g->world;
    const Attachment *a = CW_GET(w, part, g->c_attachment, Attachment);
    v3 origin;
    quat q;
    game_craft_pose(g, a->ship, alpha, &origin, &q);
    float top = CW_GET(w, part, g->c_solid, Solid)->half.y;
    return v3_add(origin, quat_rotate(q, v3_add(a->offset, v3_make(0, top + CRAFT_CHART_RADIUS + 0.1f, 0))));
}

// A point of the universe in a hologram: scaled into its ball, and what lies
// beyond the ball held at its edge.
// A thin box along `direction` (unit): a streak, an arrow's shaft.
static m4 m4_along(v3 at, v3 direction, float half_length, float half_width) {
    v3 side = v3_normalize(v3_cross(fabsf(direction.y) < 0.9f ? v3_make(0, 1, 0) : v3_make(1, 0, 0), direction));
    v3 up = v3_cross(direction, side);
    m4 r = m4_identity();
    r.m[0] = side.x * half_width; r.m[1] = side.y * half_width; r.m[2] = side.z * half_width;
    r.m[4] = up.x * half_width;   r.m[5] = up.y * half_width;   r.m[6] = up.z * half_width;
    r.m[8] = direction.x * half_length; r.m[9] = direction.y * half_length; r.m[10] = direction.z * half_length;
    r.m[12] = at.x; r.m[13] = at.y; r.m[14] = at.z;
    return r;
}

// A planet's sphere in a hologram: smaller than to scale (they would crowd it), and
// never too small to see.
static float chart_size(float radius, float fit_extent) {
    float size = 0.6f * radius * CRAFT_CHART_RADIUS / fit_extent;
    return size < 0.01f ? 0.01f : size;
}

static v3 chart_point(v3 holo, v3 fit_centre, float fit_extent, v3 world) {
    v3 d = v3_scale(v3_sub(world, fit_centre), CRAFT_CHART_RADIUS / fit_extent);
    float length = v3_length(d);
    if (length > CRAFT_CHART_RADIUS * 1.05f) d = v3_scale(d, CRAFT_CHART_RADIUS * 1.05f / length);
    return v3_add(holo, d);
}

// The chart table a player aboard a craft stands at (within CRAFT_CHART_REACH), if any.
static cw_entity_id chart_at_hand(game *g, const net_client *net, cw_entity_id player) {
    cw_world *w = &g->world;
    cw_entity_id craft = craft_of(g, net, player);
    if (craft == CW_INVALID_ENTITY || !cw_comp_has(w, player, g->c_aboard)) return CW_INVALID_ENTITY;
    v3 feet = CW_GET(w, player, g->c_aboard, Aboard)->position;
    cw_entity_id charts[4];
    int n = game_craft_charts(g, craft, charts, 4);
    for (int i = 0; i < n && i < 4; ++i) {
        v3 at = CW_GET(w, charts[i], g->c_attachment, Attachment)->offset;
        float dx = feet.x - at.x, dz = feet.z - at.z;
        if (dx * dx + dz * dz <= CRAFT_CHART_REACH * CRAFT_CHART_REACH) return charts[i];
    }
    return CW_INVALID_ENTITY;
}

// The use key at a chart table studies it, and a step or the use key again stops.
// Studying, the player stands still. The chart is the client's alone: nothing of
// this goes to the server but the standing still.
static void chart_use(game *g, const net_client *net, cw_entity_id player, view_state *v, PlayerInput *in) {
    if (v->studying != CW_INVALID_ENTITY) {
        bool gone = v->piloting || chart_at_hand(g, net, player) != v->studying;
        if (gone || in->use || in->forward != 0.0f || in->strafe != 0.0f || in->jump) {
            v->studying = CW_INVALID_ENTITY;
            in->use = false;
            return;
        }
        in->forward = in->strafe = 0.0f;
        in->jump = false;
        return;
    }
    if (!in->use || v->piloting || v->at_sails) return;
    cw_entity_id chart = chart_at_hand(g, net, player);
    if (chart == CW_INVALID_ENTITY) return;
    v->studying = chart;
    in->use = false;
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
    bool craft;                  // alone: start at the helm of a craft in the yard...
    int hull;                    // ...of this hull class (ships.h), fitted as standard
    bool stats;                  // print the frame's and the GPU's times every second
    bool currents;               // alone: the solar current flows (off for now)
    int captains;                // alone: captains sailing their ships about (default 4)
    float bench_seconds;         // hold still (--at, --look) this long without vsync, printing the stats, and exit
    bool has_position;
    v3 position;
    v3 velocity;                 // --moving: the camera's velocity for a capture, for an atmosphere's entry
    float yaw, pitch;
};

static bool parse_options(int argc, char **argv, options *o) {
    memset(o, 0, sizeof(*o));
    o->backend = R_BACKEND_VULKAN;
    o->captains = 4;
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
            if (more && strncmp(argv[i + 1], "--", 2) != 0) {
                o->hull = ship_hull_find(argv[++i]);
                if (o->hull < 0) {
                    fprintf(stderr, "no such hull: %s (", argv[i]);
                    for (int h = 0; h < HULL_COUNT; ++h) fprintf(stderr, "%s%s", h ? ", " : "", ship_hull(h)->name);
                    fputs(")\n", stderr);
                    return false;
                }
            }
        } else if (!strcmp(arg, "--currents")) {
            o->currents = true;
        } else if (!strcmp(arg, "--captains") && more) {
            o->captains = atoi(argv[++i]);
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
        if (place->kind == PLACE_SHIPWRIGHT) light_push(lights, v3_add(place->center, v3_make(-1.5f, 1.8f, 0)), 12.0f, v3_make(3.5f, 2.4f, 1.2f));   // his lantern
    }
    // Each dock, lit from above by its beacons: cool while it is free, warm once it is taken.
    cw_query docks = cw_query_begin(g->q_docks);
    cw_entity_id dock;
    while (cw_query_next(&docks, &dock)) {
        v3 floor = CW_GET(w, dock, g->c_transform, Transform)->position;
        bool free = game_dock_free(g, dock, CW_INVALID_ENTITY);
        light_push(lights, v3_add(floor, v3_make(0, 4.0f, 1.0f)), 16.0f, free ? v3_make(1.5f, 2.6f, 3.2f) : v3_make(3.2f, 2.2f, 1.2f));
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
        float set, fill;
        game_craft_sail_totals(c, &set, &fill);
        const char *mode = set > CRAFT_SAIL_SAILING && g->currents ? (c->pilot == CW_INVALID_ENTITY ? "under sail, nobody at the helm" : "under sail")
                         : c->pilot == CW_INVALID_ENTITY ? "nobody at the helm"
                         : c->mode == CRAFT_LANDING ? (c->resting ? "landed" : "landing mode") : c->assist ? "assist on" : "decoupled";
        snprintf(text, sizeof(text), "%s: %s the %s, %.0f m/s, %s", prefix,
                 c->pilot == player ? "at the helm of" : game_craft_station_of(c, player) >= 0 ? "at the sails of" : "aboard",
                 c->name, v3_length(c->velocity), mode);
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
// One piece of a sail, a box in the sail's own frame (x across, y up from its yard,
// z fore and aft), turned `yaw` about its own upright, into the draws.
struct sail_frame {
    v3 origin;                   // the craft's
    quat turned;                 // the craft's orientation
    v3 yard;                     // the middle of the yard, in the craft's frame
    quat brace;                  // about the mast
    v3 color;
    bool glow;
};
static void sail_piece(draw_list *list, r_mesh cube, const sail_frame *s, v3 centre, v3 half, float yaw, v3 color, float roll = 0.0f) {
    quat q = quat_mul(s->turned, quat_mul(s->brace, quat_mul(quat_from_axis_angle(v3_make(0, 1, 0), yaw),
                                                                quat_from_axis_angle(v3_make(0, 0, 1), roll))));
    v3 in_craft = v3_add(s->yard, quat_rotate(s->brace, centre));
    r_draw *d = draw_push(list);
    d->mesh = cube;
    d->model = m4_pose(v3_add(s->origin, quat_rotate(s->turned, in_craft)), q, half);
    d->color[0] = color.x;
    d->color[1] = color.y;
    d->color[2] = color.z;
    d->color[3] = 1.0f;
    d->flags = s->glow ? R_DRAW_UNLIT | R_DRAW_NO_SHADOW : 0;
}

// A sail, as its cut has it: hung from its yard as far as it is set (a roll under
// the yard when furled), bellied forward as the current fills it (back aback), and
// turned about its mast as it is braced. `offset` and `half` are its part's box,
// the sail full and square; the yard, a spar across its top, is drawn whatever.
static void draw_sail(draw_list *list, r_mesh cube, v3 origin, quat turned, v3 offset, v3 half, const craft_sail *sail, int cut, v3 color) {
    sail_frame s;
    s.origin = origin;
    s.turned = turned;
    s.yard = v3_add(offset, v3_make(0, half.y, 0));
    s.brace = quat_from_axis_angle(v3_make(0, 1, 0), -sail->brace);
    s.color = color;
    s.glow = false;
    float set = sail->set > 0.05f ? sail->set : 0.05f, fill = sail->fill;
    float width = half.x, drop = 2.0f * half.y * set, belly = 0.04f + 0.3f * fabsf(fill);
    float sag = fill > 0.0f ? -(belly - half.z) : belly - half.z;   // forward when it draws, back when aback
    sail_piece(list, cube, &s, v3_make(0, 0.03f, 0), v3_make(width + 0.12f, 0.06f, 0.06f), 0.0f, v3_make(0.3f, 0.19f, 0.11f));
    switch (cut) {
    case CUT_LATEEN:
        // Strips narrowing from the yard's length to a point.
        for (int i = 0; i < 6; ++i) {
            float t = ((float)i + 0.5f) / 6.0f;
            sail_piece(list, cube, &s, v3_make(0, -drop * t, sag), v3_make(width * (1.0f - t) + 0.05f, drop / 12.0f + 0.005f, belly), 0.0f, color);
        }
        break;
    case CUT_JUNK: {
        // Panels fanning wider towards the foot, a dark batten between each.
        v3 batten = v3_scale(color, 0.35f);
        for (int i = 0; i < 5; ++i) {
            float t = ((float)i + 0.5f) / 5.0f, across = width * (0.8f + 0.3f * t);
            sail_piece(list, cube, &s, v3_make(0, -drop * t, sag), v3_make(across, drop / 10.0f, belly), 0.0f, color);
            sail_piece(list, cube, &s, v3_make(0, -drop * ((float)i + 1.0f) / 5.0f, sag), v3_make(across + 0.06f, 0.035f, belly + 0.02f), 0.0f,
                       batten);
        }
        break;
    }
    case CUT_SOLAR: {
        // A glowing panel, and a wing swept back either side of it, ribbed.
        s.glow = true;
        v3 warm = v3_lerp(color, v3_make(1.0f, 0.78f, 0.35f), 0.45f);   // lit from within, and warmer for it
        v3 glow = v3_scale(warm, 0.7f), rib = v3_scale(warm, 1.15f);
        sail_piece(list, cube, &s, v3_make(0, -drop * 0.5f, sag), v3_make(width * 0.55f, drop * 0.5f, belly), 0.0f, glow);
        for (int side = -1; side <= 1; side += 2) {
            // Hinged at the panel's edge, swept back from it and raised towards the tip.
            float wing = width * 0.65f, sweep = 0.4f * (float)side, lift = 0.3f * (float)side;
            v3 hinge = v3_make(width * 0.55f * (float)side, -drop * 0.45f, sag);
            v3 along = v3_make(cosf(sweep) * cosf(lift) * (float)side, sinf(lift) * (float)side, sinf(sweep) * (float)side);
            v3 centre = v3_add(hinge, v3_scale(along, wing));
            sail_piece(list, cube, &s, centre, v3_make(wing, drop * 0.36f, belly * 0.6f), -sweep, glow, lift);
            for (int k = 1; k <= 3; ++k) {
                v3 at = v3_add(hinge, v3_scale(along, wing * 2.0f * (float)k / 4.0f));
                sail_piece(list, cube, &s, v3_add(at, v3_make(0, 0, 0.02f)), v3_make(0.025f, drop * 0.36f, belly * 0.6f + 0.02f), -sweep, rib, lift);
            }
        }
        break;
    }
    default:
        sail_piece(list, cube, &s, v3_make(0, -drop * 0.5f, sag), v3_make(width, drop * 0.5f, belly), 0.0f, color);
        break;
    }
}

static void draw_world(renderer *r, plat_window *window, game *g, cw_entity_id player, view_state *view,
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
    bool chasing = view->piloting && view->third_person && craft != CW_INVALID_ENTITY;
    if (chasing) {
        // Behind the craft, turning after it with a tenth of a second's lag.
        v3 at;
        quat q;
        game_craft_pose(g, craft, alpha, &at, &q);
        float dt = (float)(time - view->chase_time);
        if (dt < 0.0f || dt > 0.1f) dt = 0.1f;
        view->chase = view->chase_valid ? quat_nlerp(view->chase, q, 1.0f - expf(-dt / 0.12f)) : q;
        view->chase_valid = true;
        view->chase_time = time;
        game_chase_view(g, craft, at, view->chase, &eye, &forward, &up);
        if (velocity.x == 0.0f && velocity.y == 0.0f && velocity.z == 0.0f) velocity = CW_GET(w, craft, g->c_craft, Craft)->velocity;
    } else if (craft != CW_INVALID_ENTITY && cw_comp_has(w, player, g->c_aboard)) {
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
    // At the shipwright's: the ship, turning slowly on its berth between the panels.
    cw_entity_id shown = view->shop.open && cw_world_is_valid(w, player) ? game_craft_of(g, game_owner(g, player)) : CW_INVALID_ENTITY;
    if (shown != CW_INVALID_ENTITY) {
        v3 at;
        quat q;
        game_craft_pose(g, shown, alpha, &at, &q);
        float size = CW_GET(w, shown, g->c_craft, Craft)->radius;
        v3 centre = v3_add(at, quat_rotate(q, v3_make(0, size * 0.12f, 0)));
        float turn = view->shop.turn;
        eye = v3_add(centre, v3_make(sinf(turn) * size * 2.1f, size * 0.75f, cosf(turn) * size * 2.1f));
        forward = v3_normalize(v3_sub(centre, eye));
        up = v3_make(0, 1, 0);
    }
    // Studying a chart: leaning over its hologram, from the side we stand on.
    bool studying = view->studying != CW_INVALID_ENTITY && cw_world_is_valid(w, view->studying)
                 && cw_comp_has(w, view->studying, g->c_attachment) && cw_comp_has(w, view->studying, g->c_solid);
    if (studying) {
        v3 holo = chart_holo(g, view->studying, alpha);
        v3 away = v3_sub(eye, holo);
        away.y = 0.0f;
        away = v3_length(away) > 1e-3f ? v3_normalize(away) : v3_make(0, 0, 1);
        eye = v3_add(v3_add(holo, v3_scale(away, CRAFT_CHART_RADIUS * 2.3f)), v3_make(0, CRAFT_CHART_RADIUS * 1.3f, 0));
        forward = v3_normalize(v3_sub(holo, eye));
        up = v3_make(0, 1, 0);
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
        if (id == player && !chasing) continue;                  // first person: no body in view
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
            v3 offset = CW_GET(w, id, g->c_attachment, Attachment)->offset, half = visual->half;
            quat part_turn = turned;
            const Part *part = cw_comp_has(w, id, g->c_part) ? CW_GET(w, id, g->c_part, Part) : NULL;
            const Craft *carried = CW_GET(w, carrier, g->c_craft, Craft);
            // Its look: the craft's, or at the shipwright's the one being tried on it.
            const ship_look *look = carrier == shown && view->shop.look_preview ? &view->shop.preview_look : &carried->loadout.look;
            v3 painted = part ? ship_part_color(part->paint, look, part->base) : visual->color;
            if (part && part->kind == PART_SAIL && part->mast >= 0 && part->mast < CRAFT_MAX_MASTS) {
                --list->count;   // drawn in pieces, by its cut
                craft_sail hung = carried->sails[part->mast];
                if (carrier == shown) {   // at the shipwright's, her sails shown set, to be seen
                    hung.set = 1.0f;
                    hung.fill = 0.6f;
                }
                draw_sail(list, meshes[MESH_CUBE], origin, turned, offset, half, &hung, look->field[LOOK_SAIL_CUT], painted);
                continue;
            }
            at = v3_add(origin, quat_rotate(turned, offset));
            draw->model = m4_pose(at, part_turn, half);
            draw->color[0] = painted.x;
            draw->color[1] = painted.y;
            draw->color[2] = painted.z;
            draw->color[3] = 1.0f;
            draw->flags = 0;
            continue;
        } else if (cw_comp_has(w, id, g->c_player) && (carrier = craft_of(g, net, id)) != CW_INVALID_ENTITY) {
            v3 origin;
            game_craft_pose(g, carrier, alpha, &origin, &turned);
            if (id == player && cw_comp_has(w, id, g->c_aboard)) {   // ourselves, seen from behind: where the camera put the deck
                const Aboard *a = CW_GET(w, id, g->c_aboard, Aboard);
                at = v3_add(origin, quat_rotate(turned, v3_lerp(a->previous, a->position, alpha)));
            }
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
    // Flames from the thrusters as they fire, as long as the throttle is high, out
    // of the side they blow from. The throttles come with the snapshots, so everyone
    // sees what the pilot's engines are doing.
    cw_query parts = cw_query_begin(g->q_attachments);
    cw_entity_id part;
    while (cw_query_next(&parts, &part)) {
        const Attachment *a = CW_GET(w, part, g->c_attachment, Attachment);
        if (!cw_comp_has(w, part, g->c_part) || !cw_comp_has(w, a->ship, g->c_craft) || !cw_comp_has(w, part, g->c_solid)) continue;
        const Part *p = CW_GET(w, part, g->c_part, Part);
        int group = game_part_group(p);
        float burn = group >= 0 ? CW_GET(w, a->ship, g->c_craft, Craft)->throttle[group] : 0.0f;
        if (burn < 0.02f) continue;
        int axis = group / 2;
        v3 half = CW_GET(w, part, g->c_solid, Solid)->half;
        float sign = group % 2 ? 1.0f : -1.0f;   // it pushes along +axis by blowing out along -axis, and back
        float length = 0.3f + 1.5f * burn, reach = axis == 0 ? half.x : axis == 1 ? half.y : half.z;
        v3 out = v3_make(axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f);
        v3 flame_half = v3_scale(half, 0.6f);
        if (axis == 0) flame_half.x = length * 0.5f;
        else if (axis == 1) flame_half.y = length * 0.5f;
        else flame_half.z = length * 0.5f;
        v3 origin;
        quat q;
        game_craft_pose(g, a->ship, alpha, &origin, &q);
        v3 local = v3_add(a->offset, v3_scale(out, reach + length * 0.5f));
        r_draw *flame = draw_push(list);
        flame->mesh = meshes[MESH_CUBE];
        flame->model = m4_pose(v3_add(origin, quat_rotate(q, local)), q, flame_half);
        float glow = 1.5f + 2.5f * burn;
        flame->color[0] = 1.0f * glow;
        flame->color[1] = 0.45f * glow;
        flame->color[2] = 0.12f * glow;
        flame->color[3] = 1.0f;
        flame->flags = R_DRAW_UNLIT | R_DRAW_NO_SHADOW;
    }

    // Speed lines: streaks in a lattice fixed in the world (drifting with the solar
    // current, when it flows), wrapped about the eye, each drawn along the way the
    // world slips past the eye. The faster it slips, the longer and brighter they
    // are; below a run (6 m/s) they are not there at all, unless a current is flowing,
    // whose motes always show which way it goes.
    if (!g->dungeon[0]) {
        const int LINES = 180;
        const double box = 70.0;
        v3 flow = game_solar_current_at_tick(g, eye, w->tick);
        v3 slip = v3_sub(flow, velocity);
        float speed = v3_length(slip);
        float t = (speed - 6.0f) / 30.0f;
        float strength = g->currents ? 1.0f : t <= 0.0f ? 0.0f : t >= 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t);
        v3 way = speed > 1e-3f ? v3_scale(slip, 1.0f / speed) : v3_make(0, 0, -1);
        double now = game_time_of_tick(g, w->tick) + (double)alpha / GAME_TICK_HZ;
        for (int i = 0; i < LINES && strength > 0.01f; ++i) {
            uint32_t h = (uint32_t)i * 2654435761u;
            double base[3] = {(double)((h >> 4) & 1023) / 1024.0 * box, (double)((h >> 14) & 1023) / 1024.0 * box,
                              (double)(((h * 7919u) >> 8) & 1023) / 1024.0 * box};
            double drift[3] = {flow.x * now, flow.y * now, flow.z * now}, at[3] = {eye.x, eye.y, eye.z};
            float local[3];
            for (int k = 0; k < 3; ++k) {
                double p = fmod(base[k] + drift[k] - at[k], box);
                if (p < 0.0) p += box;
                local[k] = (float)(p - box * 0.5);
            }
            v3 offset = v3_make(local[0], local[1], local[2]);
            float distance = v3_length(offset);
            float near = 1.0f - distance / (float)(box * 0.5);
            if (near <= 0.0f || distance < 2.5f) continue;   // none in the face
            float length = 0.4f + speed * 0.08f;
            if (length > 9.0f) length = 9.0f;
            r_draw *d = draw_push(list);
            d->mesh = meshes[MESH_CUBE];
            d->model = m4_along(v3_add(eye, offset), way, length, 0.018f + 0.012f * strength);
            float glow = 1.3f * near * strength;
            if (g->currents) {   // the current's motes: gold
                d->color[0] = 1.2f * glow; d->color[1] = 0.9f * glow; d->color[2] = 0.45f * glow;
            } else {             // the rush of speed: a cold white
                d->color[0] = 0.8f * glow; d->color[1] = 0.9f * glow; d->color[2] = 1.15f * glow;
            }
            d->color[3] = 1.0f;
            d->flags = R_DRAW_UNLIT | R_DRAW_NO_SHADOW;
        }
    }

    // The star charts: a hologram over every chart table, the same for everyone.
    v3 fit_centre;
    float fit_extent;
    game_chart_fit(g, &fit_centre, &fit_extent);
    cw_query all_crafts = cw_query_begin(g->q_crafts);
    cw_entity_id charted;
    while (cw_query_next(&all_crafts, &charted)) {
        cw_entity_id charts[4];
        int chart_count = game_craft_charts(g, charted, charts, 4);
        for (int k = 0; k < chart_count && k < 4; ++k) {
            v3 holo = chart_holo(g, charts[k], alpha);
            const uint32_t glow = R_DRAW_UNLIT | R_DRAW_NO_SHADOW;
            // The projector's plate on the table top.
            {
                const Attachment *a = CW_GET(w, charts[k], g->c_attachment, Attachment);
                v3 half = CW_GET(w, charts[k], g->c_solid, Solid)->half, origin;
                quat q;
                game_craft_pose(g, charted, alpha, &origin, &q);
                r_draw *plate = draw_push(list);
                plate->mesh = meshes[MESH_CUBE];
                plate->model = m4_pose(v3_add(origin, quat_rotate(q, v3_add(a->offset, v3_make(0, half.y + 0.004f, 0)))), q,
                                       v3_make(half.x * 0.85f, 0.004f, half.z * 0.85f));
                plate->color[0] = 0.08f; plate->color[1] = 0.3f; plate->color[2] = 0.45f; plate->color[3] = 1.0f;
                plate->flags = glow;
            }
            cw_query bodies = cw_query_begin(g->q_planets);
            cw_entity_id body;
            while (cw_query_next(&bodies, &body)) {
                v3 at = chart_point(holo, fit_centre, fit_extent, CW_GET(w, body, g->c_transform, Transform)->position);
                float size = chart_size(CW_GET(w, body, g->c_planet, Planet)->land.radius, fit_extent);
                v3 color = cw_comp_has(w, body, g->c_visual) ? CW_GET(w, body, g->c_visual, Visual)->color : v3_make(0.6f, 0.6f, 0.6f);
                r_draw *d = draw_push(list);
                d->mesh = meshes[MESH_SPHERE];
                d->model = m4_translate_scale(at, v3_make(size, size, size));
                d->color[0] = color.x * 1.6f; d->color[1] = color.y * 1.6f; d->color[2] = color.z * 1.6f; d->color[3] = 1.0f;
                d->flags = glow;
            }
            // Its frame: a ring of light about its middle, and the beam up from the plate.
            for (int i = 0; i < 36; ++i) {
                float angle = 2.0f * TP_PI * (float)i / 36.0f;
                r_draw *d = draw_push(list);
                d->mesh = meshes[MESH_CUBE];
                d->model = m4_translate_scale(v3_add(holo, v3_make(cosf(angle) * CRAFT_CHART_RADIUS, 0, sinf(angle) * CRAFT_CHART_RADIUS)),
                                              v3_make(0.003f, 0.003f, 0.003f));
                d->color[0] = 0.2f; d->color[1] = 0.7f; d->color[2] = 1.0f; d->color[3] = 1.0f;
                d->flags = glow;
            }
            for (int i = 0; i < 8; ++i) {
                r_draw *d = draw_push(list);
                d->mesh = meshes[MESH_CUBE];
                d->model = m4_translate_scale(v3_add(holo, v3_make(0, -CRAFT_CHART_RADIUS - 0.1f + 0.06f * (float)i, 0)), v3_make(0.002f, 0.002f, 0.002f));
                d->color[0] = 0.15f; d->color[1] = 0.5f; d->color[2] = 0.8f; d->color[3] = 1.0f;
                d->flags = glow;
            }
            // The solar current, a grid of arrows across the chart's middle: which way it
            // flows, and (by their length) how strongly. None while it is still.
            for (int i = -3; i <= 3 && g->currents; ++i)
                for (int j = -3; j <= 3; ++j) {
                    if (i * i + j * j > 10) continue;   // within the ring
                    v3 place = v3_add(fit_centre, v3_make((float)i * fit_extent / 3.6f, 0, (float)j * fit_extent / 3.6f));
                    v3 flow = game_solar_current_at_tick(g, place, w->tick);
                    v3 flat = v3_make(flow.x, 0, flow.z);
                    float strength = v3_length(flat);
                    if (strength < 1e-3f) continue;
                    v3 way = v3_scale(flat, 1.0f / strength);
                    float length = 0.012f + 0.02f * strength / SOLAR_CURRENT_MAX;
                    r_draw *d = draw_push(list);
                    d->mesh = meshes[MESH_CUBE];
                    d->model = m4_along(chart_point(holo, fit_centre, fit_extent, place), way, length, 0.0018f);
                    d->color[0] = 1.3f; d->color[1] = 0.8f; d->color[2] = 0.25f; d->color[3] = 1.0f;
                    d->flags = glow;
                    r_draw *head = draw_push(list);   // the head, where it flows to
                    head->mesh = meshes[MESH_CUBE];
                    head->model = m4_translate_scale(v3_add(chart_point(holo, fit_centre, fit_extent, place), v3_scale(way, length)),
                                                     v3_make(0.004f, 0.004f, 0.004f));
                    head->color[0] = 1.6f; head->color[1] = 1.0f; head->color[2] = 0.3f; head->color[3] = 1.0f;
                    head->flags = glow;
                }
            // The hub, and the liners on their timetables.
            {
                r_draw *d = draw_push(list);
                d->mesh = meshes[MESH_CUBE];
                d->model = m4_translate_scale(chart_point(holo, fit_centre, fit_extent, v3_make(0, 0, 0)), v3_make(0.012f, 0.004f, 0.012f));
                d->color[0] = d->color[1] = d->color[2] = 1.2f; d->color[3] = 1.0f;
                d->flags = glow;
            }
            cw_query liners = cw_query_begin(g->q_ships);
            cw_entity_id liner;
            while (cw_query_next(&liners, &liner)) {
                r_draw *d = draw_push(list);
                d->mesh = meshes[MESH_CUBE];
                d->model = m4_translate_scale(chart_point(holo, fit_centre, fit_extent, CW_GET(w, liner, g->c_transform, Transform)->position),
                                              v3_make(0.006f, 0.006f, 0.006f));
                d->color[0] = d->color[1] = d->color[2] = 0.7f; d->color[3] = 1.0f;
                d->flags = glow;
            }
            // Every craft, a wedge along its heading; this one gold, with its course.
            cw_query ships = cw_query_begin(g->q_crafts);
            cw_entity_id ship;
            while (cw_query_next(&ships, &ship)) {
                if (net && !net_client_entity_visible(net, ship)) continue;
                v3 origin;
                quat q;
                game_craft_pose(g, ship, alpha, &origin, &q);
                bool home = ship == charted;
                v3 at = chart_point(holo, fit_centre, fit_extent, origin);
                r_draw *d = draw_push(list);
                d->mesh = meshes[MESH_CUBE];
                d->model = m4_pose(at, q, home ? v3_make(0.008f, 0.005f, 0.022f) : v3_make(0.006f, 0.004f, 0.016f));
                v3 color = home ? v3_make(1.8f, 1.3f, 0.3f) : v3_make(0.3f, 1.2f, 1.8f);
                d->color[0] = color.x; d->color[1] = color.y; d->color[2] = color.z; d->color[3] = 1.0f;
                d->flags = glow;
                if (!home) continue;
                v3 bow = quat_rotate(q, v3_make(0, 0, -1));
                for (int dot = 1; dot <= 6; ++dot) {
                    r_draw *mark = draw_push(list);
                    mark->mesh = meshes[MESH_CUBE];
                    mark->model = m4_translate_scale(v3_add(at, v3_scale(bow, 0.022f + 0.016f * (float)dot)), v3_make(0.0025f, 0.0025f, 0.0025f));
                    mark->color[0] = 1.5f; mark->color[1] = 1.1f; mark->color[2] = 0.3f; mark->color[3] = 1.0f;
                    mark->flags = glow;
                }
            }
        }
    }

    // The overlay, in the camera's frame: the party panel top right, gear bottom
    // right, and what loot we are looking at (online); at a helm, the flight panel
    // and the stick; and what the use key would do here.
    hud_frame hud = hud_begin_view(eye, forward, up, (float)width / (float)height, meshes[MESH_CUBE]);
    if (view->shop.open) {
        // The shipwright's screen, and nothing else over it.
        shipwright_view books = {};
        books.books = view->books;
        if (shown != CW_INVALID_ENTITY) {
            books.ship = CW_GET(w, shown, g->c_craft, Craft);
            cw_entity_id crew[1];
            books.crew_aboard = game_craft_crew(g, shown, crew, 1) > 0;
        }
        shipwright_ui_draw(list, &hud, &view->shop, &books, &view->shop_input);
        party = NULL;
    }
    if (party) {
        hud_party(list, &hud, party, my_name);
        if (net && net->gear_known) hud_gear(list, &hud, &net->gear, &net->last_pickup, net->pickup_flash);
        item looked_at;
        if (net && net_client_loot_in_view(net, 6.0f, &looked_at)) hud_loot(list, &hud, &looked_at);
    }
    if (cw_world_is_valid(w, player) && !view->shop.open) {
        const v3 advice = v3_make(0.95f, 0.85f, 0.5f), refusal = v3_make(0.95f, 0.45f, 0.4f);
        // The docks' names over them, near enough to matter; a free one in a lighter hand.
        {
            cw_query docks = cw_query_begin(g->q_docks);
            cw_entity_id dock;
            while (cw_query_next(&docks, &dock)) {
                v3 floor = CW_GET(w, dock, g->c_transform, Transform)->position;
                if (v3_length(v3_sub(floor, eye)) > 250.0f) continue;
                float lx, ly;
                if (!hud_project(&hud, v3_add(floor, v3_make(0, 3.0f, 0)), &lx, &ly)) continue;
                bool free = game_dock_free(g, dock, CW_INVALID_ENTITY);
                hud_label(list, &hud, lx, ly, CW_GET(w, dock, g->c_dock, Dock)->name, free ? v3_make(0.7f, 0.9f, 1.0f) : v3_make(0.5f, 0.5f, 0.55f));
            }
        }
        if (craft != CW_INVALID_ENTITY && cw_comp_has(w, player, g->c_aboard)) {
            const Craft *c = CW_GET(w, craft, g->c_craft, Craft);
            // The readings, for the pilot and passengers alike: all of it is the
            // craft's as the server has it (or as we fly it), mode and engines too.
            hud_flight_view flight = {};
            flight.craft = c->name;
            flight.pilot = c->pilot == player;
            flight.speed = v3_length(c->velocity);
            flight.climb = game_craft_climb(g, craft);
            flight.altitude = game_craft_altitude(g, craft);
            flight.gravity = v3_length(game_gravity(g, CW_GET(w, craft, g->c_transform, Transform)->position));
            bool manned = c->pilot != CW_INVALID_ENTITY && cw_world_is_valid(w, c->pilot);
            flight.state = !manned ? HUD_UNMANNED : c->mode == CRAFT_LANDING ? (c->resting ? HUD_LANDED : HUD_LANDING)
                         : c->assist ? HUD_FLIGHT : HUD_DECOUPLED;
            if (c->docking != DOCK_NONE && cw_world_is_valid(w, c->dock) && cw_comp_has(w, c->dock, g->c_dock)) {
                flight.state = c->docking == DOCK_DOCKED ? HUD_DOCKED : HUD_DOCKING;
                flight.dock = CW_GET(w, c->dock, g->c_dock, Dock)->name;
            }
            flight.stick_x = view->stick_x;
            flight.stick_y = view->stick_y;
            memcpy(flight.throttle, c->throttle, sizeof(flight.throttle));
            game_craft_sail_totals(c, &flight.sail, &flight.sail_fill);
            flight.masts = c->masts;
            flight.complement = ship_hull(c->loadout.hull) ? ship_hull(c->loadout.hull)->crew : 0;
            {
                // The current where the craft is, and where off its bow it comes from.
                v3 origin;
                quat q;
                game_craft_pose(g, craft, alpha, &origin, &q);
                v3 flow = game_solar_current_at_tick(g, origin, w->tick);
                v3 from = quat_unrotate(q, v3_scale(flow, -1.0f));
                flight.currents = g->currents;
                flight.current = v3_length(flow);
                flight.current_from = atan2f(from.x, -from.z) * 180.0f / TP_PI;
            }
            cw_query everyone = cw_query_begin(g->q_all_players);
            cw_entity_id someone;
            while (cw_query_next(&everyone, &someone))
                if ((!net || net_client_entity_visible(net, someone)) && craft_of(g, net, someone) == craft) ++flight.crew;
            int mast = game_craft_station_of(c, player);
            if (mast >= 0 && !studying) {
                // At the sails: the sail station's own panel, for its mast.
                const craft_sail *sail = &c->sails[mast];
                hud_sails_view sails = {};
                sails.craft = c->name;
                sails.mast = ship_mast_name(c->masts, mast);
                sails.sail = sail->set;
                sails.fill = sail->fill;
                sails.brace = sail->brace;
                sails.brace_max = CRAFT_BRACE_MAX;
                game_craft_sailing(g, craft, mast, &sails.best_brace, &sails.sailing_speed);
                sails.currents = g->currents;
                sails.current = flight.current;
                sails.current_from = flight.current_from;
                sails.speed = v3_dot(c->velocity, quat_rotate(c->orientation, v3_make(0, 0, -1)));
                hud_sails(list, &hud, &sails);
            } else if (studying) {
                // The chart read out: every planet and the hub, from this ship, and
                // labels by their spheres in the hologram.
                v3 holo = chart_holo(g, view->studying, alpha), origin;
                quat q;
                game_craft_pose(g, craft, alpha, &origin, &q);
                hud_chart_line lines[GAME_MAX_PLACES];
                int count = 0;
                cw_query bodies = cw_query_begin(g->q_planets);
                cw_entity_id body;
                while (cw_query_next(&bodies, &body) && count < GAME_MAX_PLACES - 1) {
                    const Planet *planet = CW_GET(w, body, g->c_planet, Planet);
                    v3 centre = CW_GET(w, body, g->c_transform, Transform)->position;
                    v3 local = quat_unrotate(q, v3_sub(centre, origin));
                    hud_chart_line *l = &lines[count++];
                    l->name = planet->name;
                    l->color = cw_comp_has(w, body, g->c_visual) ? v3_scale(CW_GET(w, body, g->c_visual, Visual)->color, 1.6f) : v3_make(1, 1, 1);
                    l->distance = v3_length(local) - planet->land.radius;
                    l->bearing = atan2f(local.x, -local.z) * 180.0f / TP_PI;
                    l->rise = local.y;
                    // Over the sphere's top, so as not to be lost against it.
                    v3 top = v3_add(chart_point(holo, fit_centre, fit_extent, centre),
                                    v3_scale(up, chart_size(planet->land.radius, fit_extent)));
                    float lx, ly;
                    if (hud_project(&hud, top, &lx, &ly)) hud_label(list, &hud, lx, ly + 0.03f, planet->name, v3_make(0.95f, 0.95f, 0.9f));
                }
                v3 hub = quat_unrotate(q, v3_sub(v3_make(0, 0, 0), origin));
                lines[count++] = {"HUB", v3_make(0.9f, 0.9f, 0.9f), v3_length(hub), atan2f(hub.x, -hub.z) * 180.0f / TP_PI, hub.y};
                float lx, ly;
                if (hud_project(&hud, chart_point(holo, fit_centre, fit_extent, v3_make(0, 0, 0)), &lx, &ly))
                    hud_label(list, &hud, lx, ly + 0.04f, "HUB", v3_make(0.9f, 0.9f, 0.9f));
                hud_chart(list, &hud, lines, count);
            } else {
                hud_flight(list, &hud, &flight);
            }
            if (!studying && c->pilot != player && mast < 0 && chart_at_hand(g, net, player) != CW_INVALID_ENTITY)
                hud_prompt(list, &hud, "F  STUDY THE STAR CHART", advice);
            // By a sail station: whether someone is working that mast's sails.
            int by = -1;
            bool taken = true;
            for (int m = 0; c->pilot != player && mast < 0 && m < c->masts && m < CRAFT_MAX_MASTS; ++m) {
                if (v3_length(v3_sub(CW_GET(w, player, g->c_aboard, Aboard)->position, c->sails[m].station)) > CRAFT_STATION_REACH) continue;
                bool busy = c->sails[m].hand != CW_INVALID_ENTITY && cw_world_is_valid(w, c->sails[m].hand);
                if (by < 0 || (taken && !busy)) {
                    by = m;
                    taken = busy;
                }
            }
            if (by >= 0) {
                char text[64];
                const char *name = ship_mast_name(c->masts, by);
                if (c->masts <= 1) snprintf(text, sizeof(text), taken ? "SOMEONE IS AT THE SAILS" : "F  WORK THE SAILS");
                else snprintf(text, sizeof(text), taken ? "SOMEONE IS AT THE %s" : "F  WORK THE %s", name);
                hud_prompt(list, &hud, text, taken ? refusal : advice);
            }
            // Our craft, someone aboard it in view: we may put them off.
            cw_entity_id looked_at = net && c->pilot != player ? net_client_player_looked_at(net, 6.0f) : CW_INVALID_ENTITY;
            if (looked_at != CW_INVALID_ENTITY && craft_of(g, net, looked_at) == craft && game_owner(g, craft) == game_owner(g, player)
                && game_owner(g, craft) != 0) {
                char text[80];
                snprintf(text, sizeof(text), "K  PUT %s OFF YOUR SHIP", CW_GET(w, looked_at, g->c_player, Player)->name);
                hud_prompt(list, &hud, text, advice);
            } else if (c->pilot != player && v3_length(v3_sub(CW_GET(w, player, g->c_aboard, Aboard)->position, c->helm)) <= CRAFT_HELM_REACH) {
                bool free = c->pilot == CW_INVALID_ENTITY || !cw_world_is_valid(w, c->pilot);
                if (!game_can_pilot(g, player, craft)) hud_prompt(list, &hud, "THIS HELM IS NOT YOURS", refusal);
                else if (!free) hud_prompt(list, &hud, "SOMEONE IS AT THE HELM", refusal);
                else hud_prompt(list, &hud, "F  TAKE THE HELM", advice);
            }
            // At the helm, flying: a dock in reach, or a docking under way.
            if (c->pilot == player) {
                char text[80];
                if (c->docking == DOCK_APPROACH || c->docking == DOCK_DESCEND) {
                    hud_prompt(list, &hud, "G OR THE CONTROLS  CALL OFF THE DOCKING", advice);
                } else if (c->docking == DOCK_NONE && game_craft_airborne(g, craft)) {
                    cw_entity_id dock = game_dock_in_reach(g, craft);
                    if (dock != CW_INVALID_ENTITY) {
                        snprintf(text, sizeof(text), "G  DOCK AT %s", CW_GET(w, dock, g->c_dock, Dock)->name);
                        hud_prompt(list, &hud, text, advice);
                    }
                }
            }
        } else {
            v3 feet = CW_GET(w, player, g->c_transform, Transform)->position;
            // Put off a craft: said while near it, so the closed gangway is no mystery.
            cw_query crafts = cw_query_begin(g->q_crafts);
            cw_entity_id near;
            while (cw_query_next(&crafts, &near))
                if (!game_may_board(g, player, near)
                    && v3_length(v3_sub(CW_GET(w, near, g->c_transform, Transform)->position, feet)) < CW_GET(w, near, g->c_craft, Craft)->radius + 6.0f)
                    hud_prompt(list, &hud, "THE OWNER HAS PUT YOU OFF THIS SHIP", refusal);
            for (int i = 0; i < g->place_count; ++i) {
                const game_place *place = &g->places[i];
                if (place->kind == PLACE_YARD && fabsf(feet.x - place->center.x) <= place->half.x
                    && fabsf(feet.y - place->center.y) <= place->half.y && fabsf(feet.z - place->center.z) <= place->half.z)
                    hud_prompt(list, &hud, game_craft_of(g, game_owner(g, player)) != CW_INVALID_ENTITY
                                               ? "F  TOW YOUR SHIP BACK TO ITS BERTH" : "F  CALL YOUR SHIP", advice);
                // The shipwright's stall: its name over the awning from across the yard,
                // and at the counter, what the use key does.
                if (place->kind == PLACE_SHIPWRIGHT) {
                    v3 sign = v3_add(place->center, v3_make(0, 3.2f, 0));
                    float lx, ly;
                    if (v3_length(v3_sub(sign, feet)) < 60.0f && hud_project(&hud, sign, &lx, &ly))
                        hud_label(list, &hud, lx, ly, "SHIPWRIGHT", v3_make(1.0f, 0.85f, 0.4f));
                    if (game_place_of_kind_at(g, feet, PLACE_SHIPWRIGHT) == i) hud_prompt(list, &hud, "F  TRADE WITH THE SHIPWRIGHT", advice);
                }
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

// The shipwright's screen, one frame of it. The use key at the stall's counter opens
// it (asking for the ship to be brought round, to be looked at) and lets the mouse
// go; Esc or the use key again closes it, as does being moved away. While it is
// open the pointer, the wheel and the keys are its. True, with `order` filled, when
// there is something for the shipwright: the caller sends it (online) or keeps the
// books itself (alone).
static bool shop_frame(plat_window *window, game *g, cw_entity_id player, view_state *v, const character *books, PlayerInput *in,
                       float dt, shipwright_order *order) {
    const plat_input *input = plat_window_input(window);
    cw_world *w = &g->world;
    bool valid = cw_world_is_valid(w, player) && cw_comp_has(w, player, g->c_transform);
    bool at_counter = valid && !cw_comp_has(w, player, g->c_aboard)
                   && game_place_of_kind_at(g, CW_GET(w, player, g->c_transform, Transform)->position, PLACE_SHIPWRIGHT) >= 0;
    if (!v->shop.open) {
        if (!in->use || !at_counter || v->piloting || v->at_sails || v->studying != CW_INVALID_ENTITY) return false;
        shipwright_ui_open(&v->shop);
        plat_window_capture_mouse(window, false);
        in->use = false;
        *order = {};
        order->op = SHIPWRIGHT_SHOW;
        return true;
    }
    if (in->use || input->pressed[PLAT_KEY_ESCAPE] || !at_counter) {
        shipwright_ui_close(&v->shop);
        plat_window_capture_mouse(window, true);
        in->use = false;
        return false;
    }
    in->use = false;
    int width = 1, height = 1;
    plat_window_size(window, &width, &height);
    float aspect = height > 0 ? (float)width / (float)height : 1.0f;
    shipwright_input ui = {};
    ui.pointer = !input->mouse_captured;
    ui.x = (input->cursor_x * 2.0f - 1.0f) * aspect;
    ui.y = 1.0f - input->cursor_y * 2.0f;
    ui.click = input->clicked;
    ui.scroll = input->scroll;
    ui.up = input->pressed[PLAT_KEY_UP] || input->pressed['W'];
    ui.down = input->pressed[PLAT_KEY_DOWN] || input->pressed['S'];
    ui.left = input->pressed[PLAT_KEY_LEFT] || input->pressed['A'];
    ui.right = input->pressed[PLAT_KEY_RIGHT] || input->pressed['D'];
    ui.enter = input->pressed[PLAT_KEY_ENTER] || input->pressed[PLAT_KEY_SPACE];
    ui.next_tab = input->pressed[PLAT_KEY_TAB];
    shipwright_view view = {};
    view.books = books;
    cw_entity_id craft = game_craft_of(g, game_owner(g, player));
    if (craft != CW_INVALID_ENTITY) {
        view.ship = CW_GET(w, craft, g->c_craft, Craft);
        cw_entity_id crew[1];
        view.crew_aboard = game_craft_crew(g, craft, crew, 1) > 0;
    }
    v->shop_input = ui;
    return shipwright_ui_update(&v->shop, aspect, &view, &ui, dt, order);
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
    if (v->shop.open) {
        // At the shipwright's the keys and the pointer are the screen's (shop_frame);
        // the player stands still, looking as they were, and may only leave it.
        PlayerInput still = {};
        still.use = input->pressed['F'];
        still.yaw = v->yaw;
        still.pitch = v->pitch;
        return still;
    }
    if (input->keys[PLAT_KEY_ESCAPE]) plat_window_capture_mouse(window, false);
    if (input->clicked) plat_window_capture_mouse(window, true);
    bool use = input->pressed['F'];
    if (input->pressed['X']) v->decoupled = !v->decoupled;
    if (input->pressed['L'] && v->piloting) v->landing = !v->landing;
    if (input->pressed['V'] && v->piloting) {
        v->third_person = !v->third_person;
        v->chase_valid = false;
    }
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
        if (v->landing) v->stick_y = 0.0f;   // held level: the stick only turns it
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
    in.dock = v->piloting && input->pressed['G'];
    in.decoupled = v->decoupled;
    in.landing = v->landing && v->piloting;
    // At the sails: W and S hoist and take in, A and D brace it to port and to
    // starboard, held; it stays where it is left. The player stands to it.
    if (v->at_sails) {
        in.sail = w - s;
        in.brace = d - a;
        in.forward = in.strafe = 0.0f;
        in.jump = in.act = false;
    }
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
    double last_frame = net_seconds();
    while (plat_window_poll(window)) {
        PlayerInput in = read_input(window, view);
        if (session.world && session.link) chart_use(session.world, session.link, session.link->player, view, &in);
        double now = net_seconds();
        float frame_dt = (float)(now - last_frame);
        last_frame = now;
        // The shipwright's screen: orders to the server, its answers on the screen.
        view->books = session.link && session.link->gear_known ? &session.link->gear : NULL;
        shipwright_order order;
        if (session.world && session.link && shop_frame(window, session.world, session.link->player, view, view->books, &in, frame_dt, &order))
            net_session_shipwright(&session, &order);
        if (session.link && session.link->shipwright_answers != view->shop_answers) {
            view->shop_answers = session.link->shipwright_answers;
            shipwright_ui_answer(&view->shop, session.link->shipwright_result, &session.link->shipwright_answered);
        }
        // Party keys, on the press: F1 create, I invite whoever is in view, F2 accept,
        // F3 leave, F4 decline.
        const plat_input *input = plat_window_input(window);
        const bool *pressed = input->pressed;
        if (pressed[PLAT_KEY_F1]) net_session_party_request(&session, PARTY_CREATE, NULL);
        if (pressed[PLAT_KEY_F2]) net_session_party_request(&session, PARTY_ACCEPT, NULL);
        if (pressed[PLAT_KEY_F3]) net_session_party_request(&session, PARTY_LEAVE, NULL);
        if (pressed[PLAT_KEY_F4]) net_session_party_request(&session, PARTY_DECLINE, NULL);
        if (pressed['I'] && session.link && !view->shop.open) {
            char name[ACCOUNT_NAME_CAPACITY];
            if (net_client_player_in_view(session.link, 6.0f, name)) net_session_party_request(&session, PARTY_INVITE, name);
        }
        for (int k = 0; k < 9 && !view->shop.open; ++k)
            if (pressed['1' + k]) net_session_equip(&session, k);   // wear stash item k
        // K: at the helm, clear the deck; else put off the player in view, if they
        // are aboard our craft. The server checks the craft is ours.
        if (pressed['K'] && session.link && !view->shop.open) {
            if (view->piloting) net_session_kick(&session, CW_INVALID_ENTITY);
            else {
                cw_entity_id target = net_client_player_looked_at(session.link, 6.0f);
                if (target != CW_INVALID_ENTITY) net_session_kick(&session, target);
            }
        }
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
            view->studying = CW_INVALID_ENTITY;
            if (view->shop.open) {
                shipwright_ui_close(&view->shop);
                plat_window_capture_mouse(window, true);
            }
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
    g->currents = o->currents;
    if (!o->dungeon && !o->capture_path && o->captains > 0) game_spawn_captains(g, o->captains, (uint32_t)(plat_seconds() * 1000.0) + 1u);
    cw_entity_id player = game_spawn_player(g, o->has_position ? o->position : g->spawn, true);
    // Alone, the books are kept here: a sloop to start with (and with --craft, the
    // hull asked for as well, sailed), and the purse everyone has for now.
    character books = {};
    character_fit_out(&books);
    if (o->craft && o->hull != HULL_SLOOP) {
        books.ships.owned |= (uint8_t)(1u << o->hull);
        books.ships.loadouts[o->hull] = ship_default_loadout(o->hull);
        books.ships.sailing = (uint8_t)o->hull;
    }
    game_set_ship_order(g, player, fleet_sailing(&books));
    view.books = &books;
    if (o->craft && !o->dungeon) {
        // Called to a berth, and we at its helm: stepping aboard boards it, a use takes it.
        cw_entity_id craft = game_call_craft(g, player, fleet_sailing(&books));
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
        bool use_latched = false, dock_latched = false;
        while (plat_window_poll(window)) {
            PlayerInput *in = CW_GET(&g->world, player, g->c_input, PlayerInput);
            *in = read_input(window, &view);
            chart_use(g, NULL, player, &view, in);
            double now = plat_seconds();
            // The shipwright's screen: alone, the books are ours to keep, by the same rules.
            shipwright_order order;
            if (shop_frame(window, g, player, &view, &books, in, (float)(now - last), &order)) {
                int result = shipwright_apply(&books, &order);
                if (result == SHIPWRIGHT_DONE || result == SHIPWRIGHT_UNCHANGED) {
                    game_set_ship_order(g, player, fleet_sailing(&books));
                    game_refit_craft(g, player, fleet_sailing(&books));
                }
                shipwright_ui_answer(&view.shop, result, &order);
            }
            // The use key is an edge: one tick takes it, however many run this frame.
            if (in->use) use_latched = true;
            if (in->dock) dock_latched = true;

            int steps = cw_clock_advance(&clock, (float)(now - last));
            last = now;
            for (int i = 0; i < steps; ++i) {
                in->use = use_latched;
                in->dock = dock_latched;
                use_latched = dock_latched = false;
                game_tick(g);
            }
            if (plat_window_input(window)->pressed['K'] && view.piloting && game_aboard(g, player) != CW_INVALID_ENTITY)
                game_kick_all(g, game_aboard(g, player), player);   // alone there is nobody to put off, but the rule is the same
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
              "                       [--dungeon NAME | --craft [sloop|cutter|brigantine|galleon]] [--currents] [--captains N] [--stats] [--bench SECONDS | --capture out.bmp [--ticks N]] [--at X Y Z] [--look YAW PITCH] [--moving X Y Z]\n", stderr);
        return 2;
    }
    return run(&o);
}
