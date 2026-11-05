#include "hud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

r_draw *draw_push(draw_list *list) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 256;
        list->items = (r_draw *)realloc(list->items, list->capacity * sizeof(r_draw));
    }
    r_draw *d = &list->items[list->count++];
    memset(d, 0, sizeof(*d));
    return d;
}

// ---------------------------------------------------------------------------
// The font: capitals, digits and some punctuation, 5 wide and 7 tall.
// ---------------------------------------------------------------------------
#define G(a, b, c, d, e, f, g) {a, b, c, d, e, f, g}
static const uint8_t LETTERS[26][7] = {
    G(0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11), G(0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e),
    G(0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e), G(0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c),
    G(0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f), G(0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10),
    G(0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f), G(0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11),
    G(0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e), G(0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c),
    G(0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11), G(0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f),
    G(0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11), G(0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11),
    G(0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e), G(0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10),
    G(0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d), G(0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11),
    G(0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e), G(0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04),
    G(0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e), G(0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04),
    G(0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a), G(0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11),
    G(0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04), G(0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f),
};
static const uint8_t DIGITS[10][7] = {
    G(0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e), G(0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e),
    G(0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f), G(0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e),
    G(0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02), G(0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e),
    G(0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e), G(0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    G(0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e), G(0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c),
};
struct symbol_glyph { char c; uint8_t rows[7]; };
static const symbol_glyph SYMBOLS[] = {
    {' ', G(0, 0, 0, 0, 0, 0, 0)},
    {'*', G(0x00, 0x04, 0x15, 0x0e, 0x15, 0x04, 0x00)},
    {'-', G(0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00)},
    {'.', G(0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c)},
    {',', G(0x00, 0x00, 0x00, 0x00, 0x06, 0x04, 0x08)},
    {':', G(0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00)},
    {'(', G(0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02)},
    {')', G(0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08)},
    {'/', G(0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00)},
    {'?', G(0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04)},
    {'!', G(0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04)},
    {'\'', G(0x06, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00)},
    {'_', G(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f)},
    {'+', G(0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00)},
    {'=', G(0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00)},
    {'>', G(0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08)},
    {'<', G(0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02)},
    {'[', G(0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e)},
    {']', G(0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e)},
};

const uint8_t *hud_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return LETTERS[c - 'A'];
    if (c >= '0' && c <= '9') return DIGITS[c - '0'];
    for (size_t i = 0; i < sizeof(SYMBOLS) / sizeof(SYMBOLS[0]); ++i)
        if (SYMBOLS[i].c == c) return SYMBOLS[i].rows;
    return SYMBOLS[9].rows;   // '?'
}

// ---------------------------------------------------------------------------
// Placing
// ---------------------------------------------------------------------------
hud_frame hud_begin(v3 eye, float yaw, float pitch, float aspect, r_mesh cube) {
    return hud_begin_view(eye, look_direction(yaw, pitch), v3_make(0, 1, 0), aspect, cube);
}

hud_frame hud_begin_view(v3 eye, v3 forward, v3 up, float aspect, r_mesh cube) {
    hud_frame f;
    f.eye = eye;
    f.forward = v3_normalize(forward);
    f.right = v3_normalize(v3_cross(f.forward, up));
    f.up = v3_cross(f.right, f.forward);
    f.aspect = aspect;
    f.quad = cube;
    return f;
}

// A screen unit in metres at the overlay's distance: the half height of the view.
static float unit_metres(void) { return HUD_DISTANCE * tanf(HUD_FOV * 0.5f); }

// A flat rectangle of `half_x` by `half_y` screen units centred at (x, y), facing
// the eye, `back` times as far as the overlay's distance (1: on it; a little more,
// behind what is drawn on it, as a panel behind its text).
static void quad(draw_list *list, const hud_frame *f, float x, float y, float half_x, float half_y, v3 color, float back) {
    float u = unit_metres() * back, distance = HUD_DISTANCE * back;
    v3 at = v3_add(v3_add(v3_add(f->eye, v3_scale(f->forward, distance)), v3_scale(f->right, x * u)), v3_scale(f->up, y * u));
    float sx = half_x * u, sy = half_y * u;
    r_draw *d = draw_push(list);
    d->mesh = f->quad;
    m4 m = m4_identity();
    m.m[0] = f->right.x * sx;  m.m[1] = f->right.y * sx;  m.m[2] = f->right.z * sx;
    m.m[4] = f->up.x * sy;     m.m[5] = f->up.y * sy;     m.m[6] = f->up.z * sy;
    m.m[8] = f->forward.x * 0.0005f; m.m[9] = f->forward.y * 0.0005f; m.m[10] = f->forward.z * 0.0005f;
    m.m[12] = at.x;
    m.m[13] = at.y;
    m.m[14] = at.z;
    d->model = m;
    d->color[0] = color.x;
    d->color[1] = color.y;
    d->color[2] = color.z;
    d->color[3] = 1.0f;
    d->flags = R_DRAW_UNLIT;
}

// A flat square of `half` screen units centred at (x, y), facing the eye.
static void pixel(draw_list *list, const hud_frame *f, float x, float y, float half, v3 color) {
    quad(list, f, x, y, half, half, color, 1.0f);
}

void hud_rect(draw_list *list, const hud_frame *f, float x0, float y0, float x1, float y1, v3 color, int layer) {
    float back = 1.0f + 0.002f * (float)(layer < 1 ? 1 : layer);
    quad(list, f, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, fabsf(x1 - x0) * 0.5f, fabsf(y1 - y0) * 0.5f, color, back);
}

float hud_text_width(const char *text, float pixel_size) {
    size_t n = strlen(text);
    return n ? ((float)n * HUD_CELL_WIDTH - 1.0f) * pixel_size : 0.0f;
}

void hud_text(draw_list *list, const hud_frame *f, float x, float y, const char *text, v3 color, float pixel_size,
              bool right_aligned) {
    if (right_aligned) x -= hud_text_width(text, pixel_size);
    for (const char *c = text; *c; ++c, x += HUD_CELL_WIDTH * pixel_size) {
        const uint8_t *rows = hud_glyph(*c);
        // A run of lit pixels along a row is one quad: a fraction of the draws.
        for (int row = 0; row < HUD_GLYPH_HEIGHT; ++row)
            for (int column = 0; column < HUD_GLYPH_WIDTH;) {
                if (!(rows[row] & (0x10 >> column))) {
                    ++column;
                    continue;
                }
                int end = column;
                while (end + 1 < HUD_GLYPH_WIDTH && (rows[row] & (0x10 >> (end + 1)))) ++end;
                float middle = ((float)column + (float)end + 1.0f) * 0.5f;
                quad(list, f, x + middle * pixel_size, y - ((float)row + 0.5f) * pixel_size, (float)(end - column + 1) * pixel_size * 0.5f,
                     pixel_size * 0.5f, color, 1.0f);
                column = end + 1;
            }
    }
}

// ---------------------------------------------------------------------------
// The party panel
// ---------------------------------------------------------------------------
void hud_party(draw_list *list, const hud_frame *f, const party_view *party, const char *my_name) {
    const float pixel_size = 0.006f, small = 0.0045f, margin = 0.04f;
    const v3 white = v3_make(0.95f, 0.95f, 0.9f), gold = v3_make(1.0f, 0.85f, 0.3f), dim = v3_make(0.55f, 0.55f, 0.6f);
    const v3 green = v3_make(0.5f, 0.9f, 0.5f), red = v3_make(0.95f, 0.45f, 0.4f);
    float right = f->aspect - margin, y = 1.0f - margin;
    char line[64];

    if (party->invite) {
        snprintf(line, sizeof(line), "%s INVITES YOU", party->invited_by);
        hud_text(list, f, right, y, line, gold, pixel_size, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
        hud_text(list, f, right, y, "F2 ACCEPT  F4 DECLINE", dim, small, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
    }
    if (!party->party) {
        hud_text(list, f, right, y, "NO PARTY", dim, pixel_size, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
        hud_text(list, f, right, y, "F1 CREATE  I INVITE IN VIEW", dim, small, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
    } else {
        snprintf(line, sizeof(line), "PARTY %d/%d", party->count, SOCIAL_PARTY_SIZE);
        hud_text(list, f, right, y, line, gold, pixel_size, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
        for (int i = 0; i < party->count; ++i) {
            const party_member_view *m = &party->members[i];
            bool me = strcmp(m->name, my_name) == 0;
            snprintf(line, sizeof(line), "%s%s%s", m->leader ? "* " : "", m->name, me ? " (YOU)" : "");
            hud_text(list, f, right, y, line, !m->online ? dim : me ? green : white, pixel_size, true);
            y -= (HUD_LINE_HEIGHT - 1) * pixel_size;
            hud_text(list, f, right, y, !m->online ? "OFFLINE" : m->world[0] ? m->world : "IN THE UNIVERSE", dim, small, true);
            y -= HUD_LINE_HEIGHT * small + 2.0f * pixel_size;
        }
        hud_text(list, f, right, y, "F3 LEAVE", dim, small, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
    }
    if (party->last_result != PARTY_OK) {
        hud_text(list, f, right, y, party_result_string(party->last_result), red, small, true);
    }
}

// ---------------------------------------------------------------------------
// Gear
// ---------------------------------------------------------------------------
static v3 rarity_v3(int rarity) {
    float rgb[3];
    item_rarity_color(rarity, rgb);
    return v3_make(rgb[0], rgb[1], rgb[2]);
}

void hud_gear(draw_list *list, const hud_frame *f, const character *gear, const item *last_pickup, float pickup_flash) {
    const float pixel_size = 0.005f, small = 0.004f, margin = 0.04f;
    const v3 gold = v3_make(1.0f, 0.85f, 0.3f), dim = v3_make(0.55f, 0.55f, 0.6f);
    float right = f->aspect - margin;
    char line[80], name[48];
    // Worn, one line per slot, up from the bottom; then the stash above.
    int stash_count = character_stash_count(gear);
    float y = -1.0f + margin + (SLOT_COUNT + 2) * HUD_LINE_HEIGHT * pixel_size;
    if (pickup_flash > 0.0f && last_pickup && item_exists(last_pickup)) {
        item_name(last_pickup, name, sizeof(name));
        snprintf(line, sizeof(line), "+ %s", name);
        hud_text(list, f, right, y + HUD_LINE_HEIGHT * pixel_size, line, rarity_v3(last_pickup->rarity), pixel_size, true);
    }
    hud_text(list, f, right, y, "GEAR", gold, pixel_size, true);
    y -= HUD_LINE_HEIGHT * pixel_size;
    for (int slot = 0; slot < SLOT_COUNT; ++slot) {
        const item *it = &gear->equipment[slot];
        item_name(it, name, sizeof(name));
        snprintf(line, sizeof(line), "%s  %s", item_slot_name(slot), name);
        hud_text(list, f, right, y, line, item_exists(it) ? rarity_v3(it->rarity) : dim, pixel_size, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
    }
    snprintf(line, sizeof(line), "STASH %d/%d   1-9 WEAR", stash_count, STASH_SIZE);
    hud_text(list, f, right, y, line, dim, small, true);
    y -= HUD_LINE_HEIGHT * small;
    // The first nine stash items, numbered as the keys that wear them, in small text
    // going down... there is no room below, so they go up the side instead.
    y = -1.0f + margin + (SLOT_COUNT + 3) * HUD_LINE_HEIGHT * pixel_size + HUD_LINE_HEIGHT * small;
    for (int i = 0, shown = 0; i < STASH_SIZE && shown < 9; ++i) {
        if (!item_exists(&gear->stash[i])) continue;
        item_name(&gear->stash[i], name, sizeof(name));
        snprintf(line, sizeof(line), "%d %s", i + 1, name);
        hud_text(list, f, right, y, line, rarity_v3(gear->stash[i].rarity), small, true);
        y += HUD_LINE_HEIGHT * small;
        ++shown;
        if (i >= 9) break;   // only the first nine slots have keys
    }
}

void hud_loot(draw_list *list, const hud_frame *f, const item *it) {
    char name[48], line[80];
    item_name(it, name, sizeof(name));
    snprintf(line, sizeof(line), "%s %s (%u)", item_rarity_name(it->rarity), name, (unsigned)it->power);
    hud_text(list, f, -hud_text_width(line, 0.006f) * 0.5f, -0.6f, line, rarity_v3(it->rarity), 0.006f, false);
}

// ---------------------------------------------------------------------------
// Flight
// ---------------------------------------------------------------------------
void hud_prompt(draw_list *list, const hud_frame *f, const char *text, v3 color) {
    const float size = 0.006f;
    hud_text(list, f, -hud_text_width(text, size) * 0.5f, -0.45f, text, color, size, false);
}

// A bar of `cells` squares, the first `fraction` of them lit.
static void bar(draw_list *list, const hud_frame *f, float x, float y, float fraction, v3 on, v3 off) {
    const int cells = 10;
    const float size = 0.012f;
    for (int i = 0; i < cells; ++i) {
        bool lit = fraction * (float)cells > (float)i + 0.05f;
        pixel(list, f, x + ((float)i + 0.5f) * size, y - size * 0.5f, size * 0.4f, lit ? on : off);
    }
}

void hud_flight(draw_list *list, const hud_frame *f, const hud_flight_view *view) {
    const float pixel_size = 0.006f, small = 0.0045f, margin = 0.04f;
    const v3 gold = v3_make(1.0f, 0.85f, 0.3f), white = v3_make(0.95f, 0.95f, 0.9f), dim = v3_make(0.55f, 0.55f, 0.6f);
    const v3 green = v3_make(0.5f, 0.9f, 0.5f), amber = v3_make(1.0f, 0.6f, 0.2f), fire = v3_make(1.0f, 0.55f, 0.2f);
    const v3 blue = v3_make(0.5f, 0.75f, 1.0f);
    float left = -f->aspect + margin, y = -1.0f + margin + (view->pilot ? 15.5f : 12.0f) * HUD_LINE_HEIGHT * pixel_size;
    char line[64];
    hud_text(list, f, left, y, view->craft, gold, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    if (view->complement > 0) snprintf(line, sizeof(line), "CREW %d OF %d", view->crew, view->complement);
    else snprintf(line, sizeof(line), "CREW %d", view->crew);
    hud_text(list, f, left, y, line, dim, small, false);
    y -= HUD_LINE_HEIGHT * small;
    snprintf(line, sizeof(line), "SPEED %.0f M/S", view->speed);
    hud_text(list, f, left, y, line, white, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    snprintf(line, sizeof(line), "CLIMB %+.1f", view->climb);
    hud_text(list, f, left, y, line, white, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    if (view->altitude >= 0.0f) snprintf(line, sizeof(line), "ALT %.1f M", view->altitude);
    else snprintf(line, sizeof(line), "ALT -");
    hud_text(list, f, left, y, line, white, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    snprintf(line, sizeof(line), view->gravity > 0.05f ? "GRAVITY %.1f" : "NO GRAVITY", view->gravity);
    hud_text(list, f, left, y, line, dim, small, false);
    y -= HUD_LINE_HEIGHT * small;
    static const char *const STATES[] = {"FLIGHT", "DECOUPLED", "LANDING", "LANDED", "NOBODY AT THE HELM", "DOCKING", "DOCKED"};
    const v3 state_colors[] = {green, amber, blue, blue, dim, gold, gold};
    int state = view->state >= 0 && view->state <= HUD_DOCKED ? view->state : HUD_FLIGHT;
    if (state == HUD_UNMANNED && view->sail > 0.01f)
        hud_text(list, f, left, y, "UNDER SAIL, NOBODY AT THE HELM", gold, pixel_size, false);
    else if ((state == HUD_DOCKING || state == HUD_DOCKED) && view->dock) {
        snprintf(line, sizeof(line), "%s %s %s", STATES[state], state == HUD_DOCKING ? "FOR" : "AT", view->dock);
        hud_text(list, f, left, y, line, state_colors[state], pixel_size, false);
    } else hud_text(list, f, left, y, STATES[state], state_colors[state], pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    // The current, from where off the bow, and the sail: how far set, how full.
    int from = (int)(fabsf(view->current_from) + 0.5f);
    char way[24];
    if (from <= 10) snprintf(way, sizeof(way), "AHEAD");
    else if (from >= 170) snprintf(way, sizeof(way), "ASTERN");
    else snprintf(way, sizeof(way), "%d %s", from, view->current_from > 0.0f ? "RIGHT" : "LEFT");
    if (view->currents) {
        snprintf(line, sizeof(line), "CURRENT %.0f M/S FROM %s", view->current, way);
        hud_text(list, f, left, y, line, v3_make(1.0f, 0.8f, 0.4f), small, false);
        y -= HUD_LINE_HEIGHT * small + 0.004f;
    }
    hud_text(list, f, left, y, view->masts > 1 ? "SAILS" : "SAIL", dim, small, false);
    bar(list, f, left + 0.17f, y + 0.001f, view->sail, white, v3_scale(dim, 0.4f));
    y -= HUD_LINE_HEIGHT * small + 0.004f;
    if (view->currents) {
        hud_text(list, f, left, y, view->sail_fill < -0.02f ? "ABACK" : "FILL", view->sail_fill < -0.02f ? amber : dim, small, false);
        bar(list, f, left + 0.17f, y + 0.001f, fabsf(view->sail_fill), view->sail_fill < 0.0f ? amber : gold, v3_scale(dim, 0.4f));
        y -= HUD_LINE_HEIGHT * small + 0.004f;
    }
    // The thrusters: the main engines and the lift, the two that carry the craft.
    static const struct { const char *name; int group; } shown[] = {
        {"MAIN", CRAFT_GROUP_FORWARD}, {"RETRO", CRAFT_GROUP_BACK}, {"LIFT", CRAFT_GROUP_UP}};
    for (size_t i = 0; i < sizeof(shown) / sizeof(shown[0]); ++i) {
        hud_text(list, f, left, y, shown[i].name, dim, small, false);
        bar(list, f, left + 0.17f, y + 0.001f, view->throttle[shown[i].group], fire, v3_scale(dim, 0.4f));
        y -= HUD_LINE_HEIGHT * small + 0.004f;
    }
    if (!view->pilot) return;
    y -= 0.01f;
    hud_text(list, f, left, y, "W/S THRUST  A/D SLIDE  SPACE/C LIFT", dim, small, false);
    y -= HUD_LINE_HEIGHT * small;
    hud_text(list, f, left, y, "MOUSE STEER  Q/E ROLL  SAILS: AT THE MAST", dim, small, false);
    y -= HUD_LINE_HEIGHT * small;
    hud_text(list, f, left, y, "X ASSIST  L LANDING  G DOCK  V CAMERA  K CLEAR DECK  F LEAVE", dim, small, false);

    // The stick: a ring of pixels where it rests, a cross where it is.
    const float ring = 0.12f;
    for (int i = 0; i < 24; ++i) {
        float angle = 2.0f * TP_PI * (float)i / 24.0f;
        pixel(list, f, cosf(angle) * ring, sinf(angle) * ring, 0.0025f, dim);
    }
    float x = view->stick_x * ring, sy = view->stick_y * ring;
    for (int k = -2; k <= 2; ++k) {
        pixel(list, f, x + (float)k * 0.006f, sy, 0.003f, white);
        if (k) pixel(list, f, x, sy + (float)k * 0.006f, 0.003f, white);
    }
}

// "AHEAD", "ASTERN", or so many degrees to a side.
static void bearing_words(float degrees, char *out, size_t capacity) {
    int off = (int)(fabsf(degrees) + 0.5f);
    if (off <= 10) snprintf(out, capacity, "AHEAD");
    else if (off >= 170) snprintf(out, capacity, "ASTERN");
    else snprintf(out, capacity, "%d %s", off, degrees > 0.0f ? "RIGHT" : "LEFT");
}

void hud_sails(draw_list *list, const hud_frame *f, const hud_sails_view *view) {
    const float pixel_size = 0.006f, small = 0.0045f, margin = 0.04f;
    const v3 gold = v3_make(1.0f, 0.85f, 0.3f), white = v3_make(0.95f, 0.95f, 0.9f), dim = v3_make(0.55f, 0.55f, 0.6f);
    const v3 amber = v3_make(1.0f, 0.6f, 0.2f), current = v3_make(1.0f, 0.8f, 0.4f);
    float left = -f->aspect + margin, y = -1.0f + margin + 12.0f * HUD_LINE_HEIGHT * pixel_size;
    char line[80], way[24];
    if (view->mast && strcmp(view->mast, "MAST") != 0) snprintf(line, sizeof(line), "THE %s'S %s", view->craft, view->mast);
    else snprintf(line, sizeof(line), "THE %s'S SAILS", view->craft);
    hud_text(list, f, left, y, line, gold, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    hud_text(list, f, left, y, "SET", dim, small, false);
    bar(list, f, left + 0.17f, y + 0.001f, view->sail, white, v3_scale(dim, 0.4f));
    y -= HUD_LINE_HEIGHT * small + 0.006f;
    // The brace: a scale from hard to port to hard to starboard, a gold mark where
    // the sail would draw best, a white one where it is.
    hud_text(list, f, left, y, "BRACE", dim, small, false);
    const float scale_left = left + 0.17f, scale_width = 0.24f;
    for (int i = 0; i <= 16; ++i)
        pixel(list, f, scale_left + scale_width * (float)i / 16.0f, y - 0.006f, i == 8 ? 0.0025f : 0.0015f, dim);
    float range = view->brace_max > 0.0f ? view->brace_max : 1.0f;
    float best = scale_left + scale_width * 0.5f * (1.0f + view->best_brace / range);
    float at = scale_left + scale_width * 0.5f * (1.0f + view->brace / range);
    for (int k = -2; k <= 2; ++k) pixel(list, f, best, y - 0.006f + (float)k * 0.005f, 0.0028f, gold);
    for (int k = -1; k <= 1; ++k) pixel(list, f, at, y - 0.006f + (float)k * 0.005f, 0.0035f, white);
    y -= HUD_LINE_HEIGHT * small + 0.006f;
    int degrees = (int)(fabsf(view->brace) * 180.0f / TP_PI + 0.5f);
    if (degrees == 0) snprintf(line, sizeof(line), "SQUARE");
    else snprintf(line, sizeof(line), "%d TO %s", degrees, view->brace > 0.0f ? "STARBOARD" : "PORT");
    int best_degrees = (int)(fabsf(view->best_brace) * 180.0f / TP_PI + 0.5f);
    char best_line[32];
    if (best_degrees == 0) snprintf(best_line, sizeof(best_line), "SQUARE");
    else snprintf(best_line, sizeof(best_line), "%d TO %s", best_degrees, view->best_brace > 0.0f ? "STARBOARD" : "PORT");
    char both[160];
    snprintf(both, sizeof(both), "BRACED %s, BEST %s", line, best_line);
    hud_text(list, f, left, y, both, white, small, false);
    y -= HUD_LINE_HEIGHT * small + 0.004f;
    if (!view->currents) {
        hud_text(list, f, left, y, view->sail > 0.01f ? "SET, THEY KEEP HER WAY WITH NOBODY AT THE HELM" : "FURLED: WITH NOBODY AT THE HELM, HER ENGINES STOP",
                 current, small, false);
        y -= HUD_LINE_HEIGHT * pixel_size + 0.01f;
        hud_text(list, f, left, y, "W/S HOIST/TAKE IN  A/D BRACE  F LEAVE", dim, small, false);
        return;
    }
    bearing_words(view->current_from, way, sizeof(way));
    snprintf(line, sizeof(line), "CURRENT %.0f M/S FROM %s", view->current, way);
    hud_text(list, f, left, y, line, current, small, false);
    y -= HUD_LINE_HEIGHT * small + 0.004f;
    if (view->sail <= 0.01f) snprintf(line, sizeof(line), "FURLED: THE CURRENT HAS HER");
    else if (view->sailing_speed < 0.0f) snprintf(line, sizeof(line), "ABACK: DRIVING HER ASTERN");
    else snprintf(line, sizeof(line), "DRIVING HER TO %.0f M/S, MAKING %.0f", view->sailing_speed, view->speed);
    hud_text(list, f, left, y, line, view->sailing_speed < 0.0f && view->sail > 0.01f ? amber : white, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size + 0.01f;
    hud_text(list, f, left, y, "W/S HOIST/TAKE IN  A/D BRACE  F LEAVE", dim, small, false);
}

bool hud_project(const hud_frame *f, v3 point, float *x, float *y) {
    v3 d = v3_sub(point, f->eye);
    float depth = v3_dot(d, f->forward);
    if (depth <= 0.01f) return false;
    float t = tanf(HUD_FOV * 0.5f);
    *x = v3_dot(d, f->right) / (depth * t);
    *y = v3_dot(d, f->up) / (depth * t);
    return true;
}

void hud_label(draw_list *list, const hud_frame *f, float x, float y, const char *text, v3 color) {
    const float size = 0.0045f;
    hud_text(list, f, x - hud_text_width(text, size) * 0.5f, y + HUD_GLYPH_HEIGHT * size * 0.5f, text, color, size, false);
}

void hud_chart(draw_list *list, const hud_frame *f, const hud_chart_line *lines, int count) {
    const float pixel_size = 0.0055f, small = 0.0045f, margin = 0.04f;
    const v3 gold = v3_make(1.0f, 0.85f, 0.3f), dim = v3_make(0.55f, 0.55f, 0.6f);
    float right = f->aspect - margin, y = 0.45f;
    hud_text(list, f, right, y, "STAR CHART", gold, pixel_size, true);
    y -= HUD_LINE_HEIGHT * pixel_size * 1.5f;
    char line[96];
    for (int i = 0; i < count; ++i) {
        const hud_chart_line *l = &lines[i];
        float bearing = l->bearing;
        int degrees = (int)(fabsf(bearing) + 0.5f);
        const char *side = degrees == 0 ? "AHEAD" : degrees >= 180 ? "ASTERN" : bearing > 0.0f ? "RIGHT" : "LEFT";
        char way[24];
        if (degrees == 0 || degrees >= 180) snprintf(way, sizeof(way), "%s", side);
        else snprintf(way, sizeof(way), "%d %s", degrees, side);
        char height[24];
        if (fabsf(l->rise) < 5.0f) snprintf(height, sizeof(height), "LEVEL");
        else snprintf(height, sizeof(height), "%.0f %s", fabsf(l->rise), l->rise > 0.0f ? "ABOVE" : "BELOW");
        if (l->distance >= 1000.0f) snprintf(line, sizeof(line), "%s  %.1f KM  %s  %s", l->name, l->distance / 1000.0f, way, height);
        else snprintf(line, sizeof(line), "%s  %.0f M  %s  %s", l->name, l->distance < 0.0f ? 0.0f : l->distance, way, height);
        hud_text(list, f, right, y, line, l->color, pixel_size, true);
        y -= HUD_LINE_HEIGHT * pixel_size;
    }
    y -= HUD_LINE_HEIGHT * small;
    hud_text(list, f, right, y, "GOLD: THIS SHIP   BLUE: OTHERS", dim, small, true);
    y -= HUD_LINE_HEIGHT * small;
    hud_text(list, f, right, y, "F OR MOVE TO STEP BACK", dim, small, true);
}

// A box in world space: `half` extents, axis-aligned (gear does not turn with the
// player except the weapon's side, which follows the yaw).
static void box(draw_list *list, r_mesh cube, v3 center, v3 half, v3 color) {
    r_draw *d = draw_push(list);
    d->mesh = cube;
    d->model = m4_translate_scale(center, half);
    d->color[0] = color.x;
    d->color[1] = color.y;
    d->color[2] = color.z;
    d->color[3] = 1.0f;
    d->flags = 0;
}

void hud_draw_gear(draw_list *list, r_mesh cube, v3 feet, float yaw, const Equipment *gear) {
    v3 forward = look_direction(yaw, 0.0f), right = v3_make(cosf(yaw), 0.0f, -sinf(yaw));
    for (int slot = 0; slot < SLOT_COUNT; ++slot) {
        const item *it = &gear->items[slot];
        if (!item_exists(it)) continue;
        v3 color = rarity_v3(it->rarity);
        switch (slot) {
        case SLOT_HEAD: box(list, cube, v3_add(feet, v3_make(0, PLAYER_HEIGHT + 0.08f, 0)), v3_make(0.22f, 0.09f, 0.22f), color); break;
        case SLOT_CHEST: box(list, cube, v3_add(feet, v3_make(0, 1.25f, 0)), v3_make(0.34f, 0.18f, 0.34f), color); break;
        case SLOT_LEGS: box(list, cube, v3_add(feet, v3_make(0, 0.55f, 0)), v3_make(0.33f, 0.18f, 0.33f), color); break;
        case SLOT_WEAPON: box(list, cube, v3_add(v3_add(feet, v3_scale(right, 0.42f)), v3_make(0, 1.0f, 0)), v3_make(0.05f, 0.45f, 0.05f), color); break;
        case SLOT_TRINKET: box(list, cube, v3_add(v3_add(feet, v3_scale(forward, 0.32f)), v3_make(0, 1.38f, 0)), v3_make(0.07f, 0.07f, 0.07f), color); break;
        default: break;
        }
    }
}
