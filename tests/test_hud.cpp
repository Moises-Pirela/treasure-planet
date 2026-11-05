// The overlay without a window: glyphs are the right shapes, and the party panel's
// pixels project to the top right of the screen from any camera.
#include "../src/client/hud.h"
#include "../src/client/shipwright_ui.h"
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

// The shipwright's screen, clicked through without a window: a choice and the
// button make an order; the keys do the same; the tabs list hulls and the store;
// a purse with a bottom finds the button shut; everything drawn is on the screen,
// in the two panels either side of the ship.
static shipwright_input pointer_at(shop_box b) {
    shipwright_input in = {};
    in.pointer = true;
    in.click = true;
    in.x = (b.x0 + b.x1) * 0.5f;
    in.y = (b.y0 + b.y1) * 0.5f;
    return in;
}
static void test_shipwright_screen(void) {
    const float aspect = 16.0f / 9.0f;
    character books = {};
    character_fit_out(&books);
    shipwright_ui ui = {};
    shipwright_ui_open(&ui);
    shipwright_view view = {};
    view.books = &books;
    shipwright_input none = {};
    shipwright_order order;
    CHECK(!shipwright_ui_update(&ui, aspect, &view, &none, 0.016f, &order));
    int brass = ship_component_find("Brass Drive"), furnace = ship_component_find("Iron Furnace");
    // The fittings first: the main engines' heading, their choices, the fitted one picked.
    shop_row rows[64];
    int n = shipwright_ui_rows(&ui, &view, rows, 64);
    CHECK(ui.tab == SHOP_FITTINGS && n == SLOT_KINDS + 2 && rows[0].kind == SHOP_ROW_KIND && rows[0].index == SLOT_MAIN);
    CHECK(rows[1].kind == SHOP_ROW_SLOT && rows[2].kind == SHOP_ROW_SLOT);   // the two engines, opened under it
    CHECK(ui.choice == brass && !ui.can_order && strcmp(ui.button, "FITTED") == 0);
    int choices[SHIP_CATALOGUE_MAX];
    CHECK(shipwright_ui_choices(SLOT_MAIN, choices, SHIP_CATALOGUE_MAX) == 4 && choices[1] == furnace && choices[3] == SHIP_NOTHING);
    CHECK(shipwright_ui_choices(SLOT_MAST, choices, SHIP_CATALOGUE_MAX) == 3);   // never nothing
    // Pick the furnace, then the button: every main engine.
    shipwright_input click = pointer_at(shipwright_ui_choice_box(aspect, 1));
    CHECK(!shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order));
    CHECK(ui.choice == furnace && ui.can_order && strncmp(ui.button, "FIT ALL: 4,800", 14) == 0);
    CHECK(ui.has_preview && ui.preview.accel[CRAFT_GROUP_FORWARD] > ui.now.accel[CRAFT_GROUP_FORWARD]);
    click = pointer_at(shipwright_ui_button_box(aspect));
    CHECK(shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order));
    CHECK(order.op == SHIPWRIGHT_FIT_ALL && order.slot == SLOT_MAIN && order.component == furnace);
    CHECK(shipwright_apply(&books, &order) == SHIPWRIGHT_DONE);
    shipwright_ui_answer(&ui, SHIPWRIGHT_DONE, &order);
    CHECK(ui.notice_time > 0.0f && !ui.notice_bad && strstr(ui.notice, "Iron Furnace") != NULL);
    CHECK(!shipwright_ui_update(&ui, aspect, &view, &none, 0.016f, &order) && strcmp(ui.button, "FITTED") == 0);
    // The keys: down to the first engine, right to the next fitting, Enter. Brass
    // back from store costs nothing.
    shipwright_input key = {};
    key.down = true;
    shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order);
    CHECK(ui.row == 1 && ui.choice == furnace);
    key = {};
    key.left = true;
    shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order);
    CHECK(ui.choice == brass && strcmp(ui.button, "FIT FROM STORE") == 0);
    key = {};
    key.enter = true;
    CHECK(shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order));
    CHECK(order.op == SHIPWRIGHT_FIT && order.slot == 0 && order.component == brass);
    CHECK(shipwright_apply(&books, &order) == SHIPWRIGHT_DONE && books.ships.store[brass] == 1);
    // Down past the engines to the retros: their heading opens, the engines close.
    for (int i = 0; i < 2; ++i) {
        key = {};
        key.down = true;
        shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order);
    }
    n = shipwright_ui_rows(&ui, &view, rows, 64);
    CHECK(rows[ui.row].kind == SHOP_ROW_KIND && rows[ui.row].index == SLOT_RETRO && ui.expanded == SLOT_RETRO && n == SLOT_KINDS + 2);
    // The hulls: Tab, then the galleon's row, then the button.
    key = {};
    key.next_tab = true;
    shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order);
    // The looks: a row a field; a paint tried on her before it is bought.
    CHECK(ui.tab == SHOP_LOOKS && shipwright_ui_rows(&ui, &view, rows, 64) == LOOK_FIELDS && rows[0].kind == SHOP_ROW_LOOK);
    CHECK(ui.row == 0 && ui.choice == SHIP_PAINT_AS_BUILT && ui.look_preview && strcmp(ui.button, "AS SHE IS") == 0);
    click = pointer_at(shipwright_ui_choice_box(aspect, 5));
    shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order);
    CHECK(ui.choice == 5 && ui.preview_look.field[LOOK_HULL_PAINT] == 5 && strcmp(ui.button, "PAINT HER: 400") == 0);
    CHECK(!ui.has_preview);   // no numbers: she flies the same
    key = {};
    key.enter = true;
    CHECK(shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order));
    CHECK(order.op == SHIPWRIGHT_STYLE && order.slot == LOOK_HULL_PAINT && order.component == 5);
    CHECK(shipwright_apply(&books, &order) == SHIPWRIGHT_DONE && fleet_sailing(&books)->look.field[LOOK_HULL_PAINT] == 5);
    {
        hud_frame frame = hud_begin(v3_make(0, 2, 0), 0.3f, -0.1f, aspect, 1);
        draw_list looks = {};
        shipwright_ui_draw(&looks, &frame, &ui, &view, &none);
        CHECK(looks.count > 1000);
        free(looks.items);
    }
    key = {};
    key.down = true;   // the sail's cut
    for (int i = 0; i < LOOK_SAIL_CUT; ++i) shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order);
    key = {};
    key.right = true;
    shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order);
    CHECK(ui.row == LOOK_SAIL_CUT && ui.choice == CUT_LATEEN && ui.preview_look.field[LOOK_SAIL_CUT] == CUT_LATEEN);
    key = {};
    key.next_tab = true;
    shipwright_ui_update(&ui, aspect, &view, &key, 0.016f, &order);
    CHECK(ui.tab == SHOP_STORE && shipwright_ui_rows(&ui, &view, rows, 64) == 2);   // a brass drive and a furnace in store
    CHECK(!ui.look_preview);
    CHECK(strncmp(ui.button, "SELL ONE", 8) == 0 && ui.can_order);
    click = pointer_at(shipwright_ui_tab_box(aspect, SHOP_HULLS));
    shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order);
    CHECK(ui.tab == SHOP_HULLS && shipwright_ui_rows(&ui, &view, rows, 64) == HULL_COUNT);
    click = pointer_at(shipwright_ui_row_box(aspect, HULL_GALLEON));
    shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order);
    CHECK(ui.row == HULL_GALLEON && strcmp(ui.button, "BUY HER: 150,000") == 0);
    CHECK(ui.preview.mass > ui.now.mass * 5.0f && ui.preview.turn_accel.y < ui.now.turn_accel.y);
    click = pointer_at(shipwright_ui_button_box(aspect));
    CHECK(shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order) && order.op == SHIPWRIGHT_BUY_HULL && order.hull == HULL_GALLEON);
    CHECK(shipwright_apply(&books, &order) == SHIPWRIGHT_DONE);
    CHECK(!shipwright_ui_update(&ui, aspect, &view, &none, 0.016f, &order) && strcmp(ui.button, "SAIL HER") == 0);
    // Drawn: plenty, all on the screen, and in the panels, clear of the ship between them.
    hud_frame frame = hud_begin(v3_make(0, 2, 0), 0.3f, -0.1f, aspect, 1);
    m4 view_proj = m4_mul(m4_perspective(HUD_FOV, aspect, 0.1f), m4_look_to(v3_make(0, 2, 0), look_direction(0.3f, -0.1f)));
    draw_list list = {};
    shipwright_ui_draw(&list, &frame, &ui, &view, &none);
    CHECK(list.count > 1500);
    int middle = 0;
    for (size_t i = 0; i < list.count; ++i) {
        float x, y, depth;
        CHECK(project(&view_proj, &list.items[i], &x, &y, &depth));
        CHECK(fabsf(x) < 1.0f && fabsf(y) < 1.0f);
        if (fabsf(x) < 0.35f && y > -0.8f) ++middle;
    }
    CHECK(middle == 0);
    free(list.items);
    // A purse with a bottom, and not enough in it: the button says so, and is shut.
    character poor = {};
    character_fit_out(&poor);
    poor.bottomless = false;
    poor.doubloons = 5000;
    view.books = &poor;
    shipwright_ui_open(&ui);
    shipwright_ui_update(&ui, aspect, &view, &none, 0.016f, &order);
    click = pointer_at(shipwright_ui_tab_box(aspect, SHOP_HULLS));
    shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order);
    click = pointer_at(shipwright_ui_row_box(aspect, HULL_CUTTER));
    CHECK(!shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order));
    CHECK(!ui.can_order && strcmp(ui.button, "NOT ENOUGH DOUBLOONS") == 0);
    click = pointer_at(shipwright_ui_button_box(aspect));
    CHECK(!shipwright_ui_update(&ui, aspect, &view, &click, 0.016f, &order));
    shipwright_ui_answer(&ui, SHIPWRIGHT_CANNOT_AFFORD, &order);
    CHECK(ui.notice_bad && strcmp(ui.notice, "NOT ENOUGH DOUBLOONS") == 0);
    char money[32];
    shipwright_format_doubloons(1234567, money, sizeof(money));
    CHECK(strcmp(money, "1,234,567") == 0);
    shipwright_format_doubloons(900, money, sizeof(money));
    CHECK(strcmp(money, "900") == 0);
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
            CHECK(list.items[i].flags == R_DRAW_UNLIT);   // drawn as is, over everything
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
    // Aboard a banking craft the camera rolls, and the overlay rolls with it: the
    // party panel stays top right, the flight panel bottom left, the stick in the
    // middle, resting at the centre.
    {
        v3 eye = v3_make(-60.0f, 8.0f, 14.0f);
        quat roll = quat_mul(quat_from_yaw(0.7f), quat_from_axis_angle(v3_make(0, 0, 1), 0.5f));
        v3 forward = quat_rotate(roll, v3_make(0, 0, -1)), up = quat_rotate(roll, v3_make(0, 1, 0));
        float aspect = 16.0f / 9.0f;
        m4 view_proj = m4_mul(m4_perspective(HUD_FOV, aspect, 0.1f), m4_look_to_up(eye, forward, up));
        hud_frame frame = hud_begin_view(eye, forward, up, aspect, 1);
        party_view none = {};
        draw_list panel = {};
        hud_party(&panel, &frame, &none, "Bob");
        float min_x = 2.0f, max_y = -2.0f;
        for (size_t i = 0; i < panel.count; ++i) {
            float x, y, depth;
            CHECK(project(&view_proj, &panel.items[i], &x, &y, &depth));
            if (x < min_x) min_x = x;
            if (y > max_y) max_y = y;
        }
        CHECK(min_x > 0.2f && max_y < 1.0f && max_y > 0.9f);
        free(panel.items);

        hud_flight_view flight = {};
        flight.craft = "Sloop";
        flight.speed = 42.0f;
        flight.climb = -1.5f;
        flight.pilot = true;
        flight.state = HUD_LANDING;
        flight.altitude = 12.5f;
        flight.gravity = 20.0f;
        flight.throttle[CRAFT_GROUP_UP] = 0.6f;
        draw_list list = {};
        hud_flight(&list, &frame, &flight);
        CHECK(list.count > 400);
        int centre = 0, left_bottom = 0;
        for (size_t i = 0; i < list.count; ++i) {
            float x, y, depth;
            if (!project(&view_proj, &list.items[i], &x, &y, &depth)) continue;
            CHECK(fabsf(x) < 1.0f && fabsf(y) < 1.0f);
            if (fabsf(x) < 0.02f && fabsf(y) < 0.04f) ++centre;
            if (x < -0.3f && y < 0.0f) ++left_bottom;
        }
        CHECK(centre >= 5);                 // the stick's cross, resting in the middle
        CHECK(left_bottom > 300);           // the panel
        free(list.items);
        flight.stick_x = 1.0f;              // full right: the cross moves right
        draw_list moved = {};
        hud_flight(&moved, &frame, &flight);
        int right_of_centre = 0;
        for (size_t i = 0; i < moved.count; ++i) {
            float x, y, depth;
            if (project(&view_proj, &moved.items[i], &x, &y, &depth) && x > 0.05f && fabsf(y) < 0.04f) ++right_of_centre;
        }
        CHECK(right_of_centre >= 5);
        free(moved.items);
        // A passenger reads the same panel, without the stick or the keys.
        flight.pilot = false;
        flight.sail = 0.8f;
        flight.sail_fill = -0.3f;
        flight.current = 24.0f;
        flight.current_from = 140.0f;
        flight.state = HUD_UNMANNED;
        draw_list rider = {};
        hud_flight(&rider, &frame, &flight);
        int middle = 0;
        for (size_t i = 0; i < rider.count; ++i) {
            float x, y, depth;
            if (!project(&view_proj, &rider.items[i], &x, &y, &depth)) continue;
            if (fabsf(x) < 0.2f && fabsf(y) < 0.3f) ++middle;
            CHECK(y > -1.0f && y < 1.0f);   // all of it on the screen
        }
        CHECK(middle == 0 && rider.count > 150 && rider.count < list.count);
        free(rider.items);

        // The sail station's panel: on the screen, and its brace mark (the brightest,
        // largest pixels of the scale) to the right when braced to starboard.
        hud_sails_view sails = {};
        sails.craft = "SLOOP";
        sails.sail = 0.7f;
        sails.fill = 0.8f;
        sails.best_brace = 0.6f;
        sails.brace_max = 1.4f;
        sails.current = 22.0f;
        sails.current_from = -120.0f;
        sails.sailing_speed = 14.0f;
        sails.speed = 9.0f;
        float marks[2];
        for (int side = 0; side < 2; ++side) {
            sails.brace = side ? 1.4f : -1.4f;
            draw_list panel2 = {};
            hud_sails(&panel2, &frame, &sails);
            CHECK(panel2.count > 300);
            // The mark's three pixels are the only ones of their size (half 0.0035
            // screen units, a screen unit being HUD_DISTANCE * tan(fov / 2) metres).
            const float mark = 0.0035f * HUD_DISTANCE * tanf(HUD_FOV * 0.5f);
            float sum = 0.0f;
            int n = 0;
            for (size_t i = 0; i < panel2.count; ++i) {
                float x, y, depth;
                if (!project(&view_proj, &panel2.items[i], &x, &y, &depth)) continue;
                CHECK(x > -1.0f && x < 0.3f && y > -1.0f && y < 0.0f);   // bottom left
                const float *m = panel2.items[i].model.m;
                if (fabsf(sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) - mark) < mark * 0.01f) {
                    sum += x;
                    ++n;
                }
            }
            CHECK(n == 3);
            marks[side] = n ? sum / (float)n : 0.0f;
            free(panel2.items);
        }
        CHECK(marks[1] > marks[0] + 0.05f);
    }
    // Labels go where the camera draws the point: hud_project agrees with the real
    // projection (its x in the overlay's units, aspect times the NDC's).
    {
        v3 eye = v3_make(3.0f, 2.0f, -7.0f);
        quat turn = quat_mul(quat_from_yaw(-0.9f), quat_from_axis_angle(v3_make(1, 0, 0), -0.4f));
        v3 forward = quat_rotate(turn, v3_make(0, 0, -1)), up = quat_rotate(turn, v3_make(0, 1, 0));
        float aspect = 16.0f / 9.0f;
        m4 view_proj = m4_mul(m4_perspective(HUD_FOV, aspect, 0.1f), m4_look_to_up(eye, forward, up));
        hud_frame frame = hud_begin_view(eye, forward, up, aspect, 1);
        const v3 points[] = {v3_add(eye, v3_scale(forward, 5.0f)), v3_make(4.0f, 1.0f, -12.0f), v3_make(-1.0f, -0.5f, -11.0f)};
        for (int i = 0; i < 3; ++i) {
            float hx, hy, nx, ny, depth;
            r_draw d = {};
            d.model = m4_translate_scale(points[i], v3_make(1, 1, 1));
            CHECK(hud_project(&frame, points[i], &hx, &hy));
            CHECK(project(&view_proj, &d, &nx, &ny, &depth));
            CHECK(fabsf(hx - nx * aspect) < 1e-3f && fabsf(hy - ny) < 1e-3f);
        }
        CHECK(!hud_project(&frame, v3_sub(eye, forward), &aspect, &aspect));   // behind: nowhere
        aspect = 16.0f / 9.0f;
        hud_chart_line lines[2] = {{"VERDANT", v3_make(0.3f, 0.7f, 0.4f), 1650.0f, -35.0f, -140.0f},
                                   {"HUB", v3_make(0.8f, 0.8f, 0.8f), 40.0f, 179.0f, 2.0f}};
        draw_list chart = {};
        hud_chart(&chart, &frame, lines, 2);
        CHECK(chart.count > 200);
        for (size_t i = 0; i < chart.count; ++i) {
            float x, y, depth;
            CHECK(project(&view_proj, &chart.items[i], &x, &y, &depth));
            CHECK(x > 0.0f && x < 1.0f && fabsf(y) < 0.6f);   // right of middle
        }
        free(chart.items);
    }
    test_shipwright_screen();
    if (failures) {
        printf("test_hud: %d failed\n", failures);
        return 1;
    }
    puts("test_hud: ok");
    return 0;
}
