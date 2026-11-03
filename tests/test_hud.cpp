// The overlay without a window: glyphs are the right shapes, and the party panel's
// pixels project to the top right of the screen from any camera.
#include "../src/client/hud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } \
} while (0)

static int glyph_pixels(char c) {
    const uint8_t *rows = hud_glyph(c);
    int n = 0;
    for (int r = 0; r < HUD_GLYPH_HEIGHT; ++r)
        for (int b = 0; b < HUD_GLYPH_WIDTH; ++b) n += (rows[r] >> b) & 1;
    return n;
}

// Where a draw's centre lands on screen, in normalised device coordinates.
static bool project(const m4 *view_proj, const r_draw *d, float *x, float *y, float *depth) {
    float px = d->model.m[12], py = d->model.m[13], pz = d->model.m[14];
    float cx = view_proj->m[0] * px + view_proj->m[4] * py + view_proj->m[8] * pz + view_proj->m[12];
    float cy = view_proj->m[1] * px + view_proj->m[5] * py + view_proj->m[9] * pz + view_proj->m[13];
    float cz = view_proj->m[2] * px + view_proj->m[6] * py + view_proj->m[10] * pz + view_proj->m[14];
    float cw = view_proj->m[3] * px + view_proj->m[7] * py + view_proj->m[11] * pz + view_proj->m[15];
    if (cw <= 0.0f) return false;
    *x = cx / cw;
    *y = cy / cw;
    *depth = cz / cw;
    return true;
}

int main(void) {
    // Glyphs: every printable we use has ink, except the space; letters have no
    // stray bits outside five columns; lowercase is uppercase.
    CHECK(glyph_pixels(' ') == 0);
    for (char c = 'A'; c <= 'Z'; ++c) {
        CHECK(glyph_pixels(c) >= 5);
        CHECK(memcmp(hud_glyph(c), hud_glyph((char)(c - 'A' + 'a')), HUD_GLYPH_HEIGHT) == 0);
        for (int r = 0; r < HUD_GLYPH_HEIGHT; ++r) CHECK((hud_glyph(c)[r] & ~0x1f) == 0);
    }
    for (char c = '0'; c <= '9'; ++c) CHECK(glyph_pixels(c) >= 5);
    CHECK(glyph_pixels('I') == 11 && glyph_pixels('*') == 11 && glyph_pixels('.') == 4);
    CHECK(hud_glyph('~') == hud_glyph('?'));   // unknown characters draw as '?'
    CHECK(hud_text_width("ABC", 0.01f) > 0.169f && hud_text_width("ABC", 0.01f) < 0.171f);

    // A camera looking somewhere arbitrary; the panel must still sit top right.
    const float yaws[] = {0.0f, 1.3f, -2.5f}, pitches[] = {0.0f, 0.6f, -0.4f};
    for (int view = 0; view < 3; ++view) {
        v3 eye = v3_make(12.0f, 3.5f, -40.0f);
        float yaw = yaws[view], pitch = pitches[view], aspect = 16.0f / 9.0f;
        m4 projection = m4_perspective(HUD_FOV, aspect, 0.1f);
        m4 view_proj = m4_mul(projection, m4_look_to(eye, look_direction(yaw, pitch)));
        hud_frame frame = hud_begin(eye, yaw, pitch, aspect, 1);

        party_view party = {};
        party.party = 7;
        party.count = 3;
        strcpy(party.members[0].name, "Alice");
        party.members[0].leader = party.members[0].online = true;
        strcpy(party.members[1].name, "Bob");
        party.members[1].online = true;
        strcpy(party.members[1].world, "Verdant Depths");
        strcpy(party.members[2].name, "Carol");
        party.last_result = PARTY_ERROR_FULL;
        draw_list list = {};
        hud_party(&list, &frame, &party, "Bob");
        CHECK(list.count > 300);
        float min_x = 2.0f, max_x = -2.0f, min_y = 2.0f, max_y = -2.0f;
        int in_front = 0;
        for (size_t i = 0; i < list.count; ++i) {
            float x, y, depth;
            if (!project(&view_proj, &list.items[i], &x, &y, &depth)) continue;
            ++in_front;
            CHECK(depth > 0.3f);                // nearer than the world (reversed depth: near is 1)
            CHECK(list.items[i].color[3] == 0.0f);   // unlit
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
        CHECK(in_front == (int)list.count);
        CHECK(max_x < 1.0f && max_x > 0.9f);    // flush with the right edge, inside the margin
        CHECK(min_x > 0.2f);                    // a panel, not a banner
        CHECK(max_y < 1.0f && max_y > 0.9f);    // at the top
        CHECK(min_y > 0.2f);                    // and not running off the bottom
        free(list.items);

        // With nothing to show but the hint, fewer pixels; with an invitation, more.
        party_view none = {};
        draw_list hint = {};
        hud_party(&hint, &frame, &none, "Bob");
        CHECK(hint.count > 50 && hint.count < list.count);
        none.invite = 3;
        strcpy(none.invited_by, "Alice");
        draw_list invited = {};
        hud_party(&invited, &frame, &none, "Bob");
        CHECK(invited.count > hint.count);
        free(hint.items);
        free(invited.items);
    }
    if (failures) {
        printf("test_hud: %d failed\n", failures);
        return 1;
    }
    puts("test_hud: ok");
    return 0;
}
