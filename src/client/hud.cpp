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

// A flat square of `half` screen units centred at (x, y), facing the eye.
static void pixel(draw_list *list, const hud_frame *f, float x, float y, float half, v3 color) {
    float u = unit_metres();
    v3 at = v3_add(v3_add(v3_add(f->eye, v3_scale(f->forward, HUD_DISTANCE)), v3_scale(f->right, x * u)), v3_scale(f->up, y * u));
    float s = half * u;
    r_draw *d = draw_push(list);
    d->mesh = f->quad;
    m4 m = m4_identity();
    m.m[0] = f->right.x * s;   m.m[1] = f->right.y * s;   m.m[2] = f->right.z * s;
    m.m[4] = f->up.x * s;      m.m[5] = f->up.y * s;      m.m[6] = f->up.z * s;
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

float hud_text_width(const char *text, float pixel_size) {
    size_t n = strlen(text);
    return n ? ((float)n * HUD_CELL_WIDTH - 1.0f) * pixel_size : 0.0f;
}

void hud_text(draw_list *list, const hud_frame *f, float x, float y, const char *text, v3 color, float pixel_size,
              bool right_aligned) {
    if (right_aligned) x -= hud_text_width(text, pixel_size);
    for (const char *c = text; *c; ++c, x += HUD_CELL_WIDTH * pixel_size) {
        const uint8_t *rows = hud_glyph(*c);
        for (int row = 0; row < HUD_GLYPH_HEIGHT; ++row)
            for (int column = 0; column < HUD_GLYPH_WIDTH; ++column)
                if (rows[row] & (0x10 >> column))
                    pixel(list, f, x + ((float)column + 0.5f) * pixel_size, y - ((float)row + 0.5f) * pixel_size,
                          pixel_size * 0.5f, color);
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
    float left = -f->aspect + margin, y = -1.0f + margin + 9.0f * HUD_LINE_HEIGHT * pixel_size;
    char line[64];
    hud_text(list, f, left, y, view->craft, gold, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    snprintf(line, sizeof(line), "SPEED %.0f M/S", view->speed);
    hud_text(list, f, left, y, line, white, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    snprintf(line, sizeof(line), "CLIMB %+.1f", view->climb);
    hud_text(list, f, left, y, line, white, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    hud_text(list, f, left, y, view->assist ? "ASSIST ON" : "DECOUPLED", view->assist ? green : amber, pixel_size, false);
    y -= HUD_LINE_HEIGHT * pixel_size;
    // The thrusters: the main engines and the lift, the two that carry the craft.
    static const struct { const char *name; int group; } shown[] = {
        {"MAIN", CRAFT_GROUP_FORWARD}, {"RETRO", CRAFT_GROUP_BACK}, {"LIFT", CRAFT_GROUP_UP}};
    for (size_t i = 0; i < sizeof(shown) / sizeof(shown[0]); ++i) {
        hud_text(list, f, left, y, shown[i].name, dim, small, false);
        bar(list, f, left + 0.17f, y + 0.001f, view->throttle[shown[i].group], fire, v3_scale(dim, 0.4f));
        y -= HUD_LINE_HEIGHT * small + 0.004f;
    }
    y -= 0.01f;
    hud_text(list, f, left, y, "W/S THRUST  A/D SLIDE  SPACE/C LIFT", dim, small, false);
    y -= HUD_LINE_HEIGHT * small;
    hud_text(list, f, left, y, "MOUSE STEER  Q/E ROLL  X ASSIST  F LEAVE", dim, small, false);

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
