// The client: reads the keyboard and mouse into one player's input and draws what is
// in the world. It owns no game state beyond where the camera looks. Alone, it ticks
// the simulation itself; with --connect, the world is a replica fed by a server and
// the input goes there instead (src/net/client.h).
#include "../game/game.h"
#include "../net/client.h"
#include "../platform/platform.h"
#include "../render/renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOUSE_RADIANS_PER_COUNT 0.0025f
#define MAX_PITCH 1.5f
#define KEY_ESCAPE 0x1B
#define KEY_SPACE 0x20
#define STAR_COUNT 900

struct options {
    r_backend_kind backend;
    const char *connect;         // host[:port] of a zone server; NULL plays alone
    const char *capture_path;    // render one scripted frame, write it and exit
    int capture_ticks;
    bool has_position;
    v3 position;
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
        } else if (!strcmp(arg, "--capture") && more) {
            o->capture_path = argv[++i];
        } else if (!strcmp(arg, "--ticks") && more) {
            o->capture_ticks = atoi(argv[++i]);
        } else if (!strcmp(arg, "--at") && i + 3 < argc) {
            o->position = v3_make((float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3]));
            o->has_position = true;
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

// A shell of small triangles facing the middle, drawn around the camera.
static r_mesh create_stars(renderer *r) {
    r_vertex *vertices = (r_vertex *)calloc(STAR_COUNT * 3, sizeof(r_vertex));
    uint32_t *indices = (uint32_t *)malloc(STAR_COUNT * 3 * sizeof(uint32_t));
    uint32_t seed = 12345;     // the same sky on every machine and in every capture
    for (uint32_t i = 0; i < STAR_COUNT; ++i) {
        float angles[2];
        for (int k = 0; k < 2; ++k) {
            seed = seed * 1664525u + 1013904223u;
            angles[k] = (float)(seed >> 8) / 16777216.0f;
        }
        float z = angles[0] * 2.0f - 1.0f, around = angles[1] * 2.0f * TP_PI, ring = sqrtf(1.0f - z * z);
        v3 direction = v3_make(ring * cosf(around), z, ring * sinf(around));
        v3 side = v3_normalize(v3_cross(direction, fabsf(z) < 0.9f ? v3_make(0, 1, 0) : v3_make(1, 0, 0)));
        v3 up = v3_cross(side, direction);
        const float distance = 5000.0f, size = 6.0f;
        v3 center = v3_scale(direction, distance);
        v3 corners[3] = {v3_add(center, v3_scale(up, size)), v3_add(center, v3_scale(side, -size)), v3_add(center, v3_scale(side, size))};
        for (uint32_t c = 0; c < 3; ++c) {
            vertices[i * 3 + c].position[0] = corners[c].x;
            vertices[i * 3 + c].position[1] = corners[c].y;
            vertices[i * 3 + c].position[2] = corners[c].z;
            vertices[i * 3 + c].normal[1] = 1.0f;
            indices[i * 3 + c] = i * 3 + c;
        }
    }
    r_mesh mesh = renderer_mesh_create(r, vertices, STAR_COUNT * 3, indices, STAR_COUNT * 3);
    free(vertices);
    free(indices);
    return mesh;
}

// The title bar: where the player stands relative to the ships, after `prefix`.
static void set_status(plat_window *window, game *g, cw_entity_id player, const char *prefix) {
    cw_world *w = &g->world;
    if (!cw_world_is_valid(w, player)) {
        plat_window_set_title(window, prefix);
        return;
    }
    v3 at = CW_GET(w, player, g->c_transform, Transform)->position;
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

struct draw_list {
    r_draw *items;
    size_t count, capacity;
};

static r_draw *draw_push(draw_list *list) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 256;
        list->items = (r_draw *)realloc(list->items, list->capacity * sizeof(r_draw));
    }
    return &list->items[list->count++];
}

// Where a player's eyes are, from their feet.
static v3 eye_of(v3 feet) { return v3_add(feet, v3_make(0, PLAYER_EYE, 0)); }

// Draws the world from `player`'s eyes (or from the spawn while there is no player
// yet). Other players are their capsule plus a small block at the eyes, pointing
// where they look. `net`, when set, hides replicas the server stopped updating.
static void draw_world(renderer *r, plat_window *window, game *g, cw_entity_id player, float yaw, float pitch,
                       float alpha, const r_mesh *meshes, r_mesh stars, draw_list *list, const net_client *net,
                       uint8_t **capture) {
    cw_world *w = &g->world;
    int width, height;
    plat_window_size(window, &width, &height);
    if (width <= 0 || height <= 0) return;

    v3 eye = eye_of(g->spawn);
    if (cw_world_is_valid(w, player)) {
        const Transform *pt = CW_GET(w, player, g->c_transform, Transform);
        eye = eye_of(v3_lerp(pt->previous, pt->position, alpha));
        if (net) eye = v3_add(eye, net_client_view_offset(net));   // a fading misprediction
    }
    m4 view = m4_look_to(eye, look_direction(yaw, pitch));
    m4 projection = m4_perspective(70.0f * TP_PI / 180.0f, (float)width / (float)height, 0.1f);

    r_frame frame = {};
    frame.uniforms.view_proj = m4_mul(projection, view);
    v3 sun = v3_normalize(v3_make(0.5f, 0.8f, 0.35f));
    frame.uniforms.sun_dir[0] = sun.x;
    frame.uniforms.sun_dir[1] = sun.y;
    frame.uniforms.sun_dir[2] = sun.z;
    frame.uniforms.sun_color[0] = 1.0f;
    frame.uniforms.sun_color[1] = 0.96f;
    frame.uniforms.sun_color[2] = 0.88f;
    frame.uniforms.ambient[0] = 0.13f;
    frame.uniforms.ambient[1] = 0.15f;
    frame.uniforms.ambient[2] = 0.2f;
    frame.clear[0] = 0.015f;
    frame.clear[1] = 0.02f;
    frame.clear[2] = 0.05f;

    list->count = 0;
    r_draw *sky = draw_push(list);
    sky->mesh = stars;
    sky->model = m4_translate_scale(eye, v3_make(1, 1, 1));
    sky->color[0] = sky->color[1] = sky->color[2] = 0.9f;
    sky->color[3] = 0.0f;

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
        draw->color[0] = visual->color.x;
        draw->color[1] = visual->color.y;
        draw->color[2] = visual->color.z;
        draw->color[3] = 1.0f;
        if (cw_comp_has(w, id, g->c_player)) {
            const Player *p = CW_GET(w, id, g->c_player, Player);
            v3 nose = v3_add(eye_of(at), v3_scale(look_direction(p->yaw, p->pitch), PLAYER_RADIUS));
            r_draw *face = draw_push(list);
            face->mesh = meshes[MESH_CUBE];
            face->model = m4_translate_scale(nose, v3_make(0.08f, 0.08f, 0.08f));
            face->color[0] = face->color[1] = face->color[2] = 0.1f;
            face->color[3] = 1.0f;
        }
    }
    frame.draws = list->items;
    frame.draw_count = list->count;
    if (capture) {
        *capture = (uint8_t *)malloc((size_t)width * (size_t)height * 4);
        frame.capture_rgba = *capture;
    }
    renderer_submit(r, &frame, width, height);
}

// Mouse look and the keys, into one tick's worth of input. Yaw and pitch are the
// camera's and persist between frames.
static PlayerInput read_input(plat_window *window, float *yaw, float *pitch) {
    const plat_input *input = plat_window_input(window);
    if (input->keys[KEY_ESCAPE]) plat_window_capture_mouse(window, false);
    if (input->clicked) plat_window_capture_mouse(window, true);
    if (input->mouse_captured) {
        *yaw -= input->mouse_dx * MOUSE_RADIANS_PER_COUNT;
        *pitch -= input->mouse_dy * MOUSE_RADIANS_PER_COUNT;
        if (*pitch > MAX_PITCH) *pitch = MAX_PITCH;
        if (*pitch < -MAX_PITCH) *pitch = -MAX_PITCH;
    }
    PlayerInput in = {};
    in.forward = (input->keys['W'] ? 1.0f : 0.0f) - (input->keys['S'] ? 1.0f : 0.0f);
    in.strafe = (input->keys['D'] ? 1.0f : 0.0f) - (input->keys['A'] ? 1.0f : 0.0f);
    in.jump = input->keys[KEY_SPACE];
    in.yaw = *yaw;
    in.pitch = *pitch;
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
                      const r_mesh *meshes, r_mesh stars, draw_list *list, float *yaw, float *pitch) {
    if (!net_init()) {
        fputs("the network could not be initialised\n", stderr);
        return 1;
    }
    net_session session;
    if (!net_session_init(&session, o->connect, NET_INTERP_DELAY)) {
        fprintf(stderr, "%s: %s\n", o->connect, session.failure);
        net_shutdown();
        return 1;
    }
    fprintf(stderr, "connecting to %s\n", o->connect);

    double last_status = 0.0;
    int status = 0;
    plat_window_capture_mouse(window, true);
    while (plat_window_poll(window)) {
        PlayerInput in = read_input(window, yaw, pitch);
        double now = net_seconds();
        net_session_frame(&session, &in, now);
        net_client_state state = net_session_state(&session);
        if (state == NC_DISCONNECTED) {
            fprintf(stderr, "disconnected: %s\n", session.failure);
            status = 1;
            break;
        }
        game *g = session.world;
        net_client *link = session.link;
        cw_entity_id player = state == NC_CONNECTED ? link->player : CW_INVALID_ENTITY;
        if (now - last_status > 0.25) {
            char address[32], prefix[200];
            net_address_format(link->server, address, sizeof(address));
            snprintf(prefix, sizeof(prefix), "Treasure Planet [%s] %s %s, %zu players, %.0f ms, %d hand-offs (%.0f ms)",
                     backend, address, net_state_name(state), cw_query_count(g->q_all_players), link->rtt * 1000.0f,
                     session.handoffs, session.last_handoff_seconds * 1000.0f);
            set_status(window, g, player, prefix);
            last_status = now;
        }
        draw_world(r, window, g, player, *yaw, *pitch, net_client_alpha(link), meshes, stars, list, link, NULL);
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
    renderer *r = renderer_create(o->backend, window);
    if (!r) {
        fputs("no renderer could start\n", stderr);
        plat_window_destroy(window);
        return 1;
    }
    const char *backend = renderer_backend_name(r);
    fprintf(stderr, "renderer: %s\n", backend);

    r_mesh meshes[MESH_COUNT];
    meshes[MESH_CUBE] = renderer_mesh_cube(r);
    meshes[MESH_SPHERE] = renderer_mesh_sphere(r, 96, 64);
    meshes[MESH_CAPSULE] = renderer_mesh_capsule(r, PLAYER_RADIUS, PLAYER_HEIGHT, 24, 8);
    r_mesh stars = create_stars(r);

    float yaw = o->yaw, pitch = o->pitch;
    draw_list list = {};
    int status = 0;

    if (o->connect) {
        status = run_online(o, window, r, backend, meshes, stars, &list, &yaw, &pitch);
        free(list.items);
        renderer_destroy(r);
        plat_window_destroy(window);
        return status;
    }

    game *g = (game *)malloc(sizeof(game));
    game_init(g, CW_HOST);
    game_build_universe(g);
    cw_entity_id player = game_spawn_player(g, o->has_position ? o->position : g->spawn, true);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "Treasure Planet [%s]", backend);

    if (o->capture_path) {
        // Scripted: no clock and no input, so every run produces the same picture.
        for (int i = 0; i < o->capture_ticks; ++i) game_tick(g);
        uint8_t *pixels = NULL;
        // A few frames first, so a fresh swapchain has settled on the window's size.
        for (int i = 0; i < 4 && plat_window_poll(window); ++i)
            draw_world(r, window, g, player, yaw, pitch, 1.0f, meshes, stars, &list, NULL, NULL);
        plat_window_poll(window);
        draw_world(r, window, g, player, yaw, pitch, 1.0f, meshes, stars, &list, NULL, &pixels);
        int width, height;
        plat_window_size(window, &width, &height);
        if (!pixels || !write_bmp(o->capture_path, pixels, width, height)) {
            fprintf(stderr, "could not write %s\n", o->capture_path);
            status = 1;
        } else {
            fprintf(stderr, "wrote %s (%dx%d, %zu draws)\n", o->capture_path, width, height, list.count);
        }
        free(pixels);
    } else {
        cw_clock clock = cw_clock_make(GAME_TICK_HZ);
        double last = plat_seconds(), last_status = 0.0;
        plat_window_capture_mouse(window, true);
        while (plat_window_poll(window)) {
            PlayerInput *in = CW_GET(&g->world, player, g->c_input, PlayerInput);
            *in = read_input(window, &yaw, &pitch);

            double now = plat_seconds();
            int steps = cw_clock_advance(&clock, (float)(now - last));
            last = now;
            for (int i = 0; i < steps; ++i) game_tick(g);

            if (now - last_status > 0.25) {
                set_status(window, g, player, prefix);
                last_status = now;
            }
            draw_world(r, window, g, player, yaw, pitch, cw_clock_alpha(&clock), meshes, stars, &list, NULL, NULL);
        }
    }

    free(list.items);
    game_destroy(g);
    free(g);
    renderer_destroy(r);
    plat_window_destroy(window);
    return status;
}

int main(int argc, char **argv) {
    options o;
    if (!parse_options(argc, argv, &o)) {
        fputs("usage: treasure_planet [--renderer vulkan|gl] [--connect host[:port]]\n"
              "                       [--capture out.bmp [--ticks N] [--at X Y Z] [--look YAW PITCH]]\n", stderr);
        return 2;
    }
    return run(&o);
}
