// The shipwright's screen (shipwright_ui.h).
#include "shipwright_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Layout: two panels either side of the ship, in the overlay's units.
// ---------------------------------------------------------------------------
static const float PANEL_WIDTH = 1.04f, PAD = 0.03f;
static const float TAB_TOP = 0.745f, TAB_BOTTOM = 0.67f;
static const float ROW_TOP = 0.62f, ROW_HEIGHT = 0.064f;
static const float CHOICE_TOP = 0.72f, CHOICE_HEIGHT = 0.06f;
static const float BUTTON_TOP = -0.7f, BUTTON_BOTTOM = -0.84f;
static const float TITLE = 0.0065f, TEXT = 0.0045f, SMALL = 0.004f;

static float left_of(float aspect) { return -aspect + 0.04f; }
static float right_of(float aspect) { return aspect - 0.04f; }

shop_box shipwright_ui_tab_box(float aspect, int tab) {
    float x0 = left_of(aspect) + PAD, width = (PANEL_WIDTH - 2.0f * PAD - (float)(SHOP_TABS - 1) * 0.015f) / (float)SHOP_TABS;
    shop_box b = {x0 + (float)tab * (width + 0.015f), TAB_BOTTOM, 0, TAB_TOP};
    b.x1 = b.x0 + width;
    return b;
}

shop_box shipwright_ui_row_box(float aspect, int shown) {
    float top = ROW_TOP - (float)shown * ROW_HEIGHT;
    shop_box b = {left_of(aspect) + PAD, top - ROW_HEIGHT + 0.008f, left_of(aspect) + PANEL_WIDTH - PAD, top};
    return b;
}

shop_box shipwright_ui_choice_box(float aspect, int choice) {
    float top = CHOICE_TOP - (float)choice * CHOICE_HEIGHT;
    shop_box b = {right_of(aspect) - PANEL_WIDTH + PAD, top - CHOICE_HEIGHT + 0.008f, right_of(aspect) - PAD, top};
    return b;
}

shop_box shipwright_ui_button_box(float aspect) {
    shop_box b = {right_of(aspect) - PANEL_WIDTH + PAD, BUTTON_BOTTOM, right_of(aspect) - PAD, BUTTON_TOP};
    return b;
}

static bool inside(shop_box b, float x, float y) { return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1; }

// ---------------------------------------------------------------------------
// Words
// ---------------------------------------------------------------------------
void shipwright_format_doubloons(int64_t amount, char *out, size_t capacity) {
    char digits[32];
    bool negative = amount < 0;
    uint64_t value = negative ? (uint64_t)(-amount) : (uint64_t)amount;
    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
    char grouped[48];
    size_t n = strlen(digits), k = 0;
    if (negative) grouped[k++] = '-';
    for (size_t i = 0; i < n && k + 2 < sizeof(grouped); ++i) {
        grouped[k++] = digits[i];
        size_t left = n - i - 1;
        if (left > 0 && left % 3 == 0) grouped[k++] = ',';
    }
    grouped[k] = 0;
    snprintf(out, capacity, "%s", grouped);
}

static const char *kind_plural(int kind) {
    static const char *const NAMES[SLOT_KINDS] = {"MAIN ENGINES", "RETROS", "LIFT THRUSTERS", "DOWN THRUSTERS", "SIDE JETS", "GYROS", "MASTS",
                                                  "SAILS"};
    return kind >= 0 && kind < SLOT_KINDS ? NAMES[kind] : "?";
}

int shipwright_ui_choices(int slot_kind, int *out, int capacity) {
    int n = 0;
    for (int c = 1; c < ship_component_count() && n < capacity; ++c)
        if (ship_fits(c, slot_kind)) out[n++] = c;
    if (ship_fits(SHIP_NOTHING, slot_kind) && n < capacity) out[n++] = SHIP_NOTHING;
    return n;
}

// ---------------------------------------------------------------------------
// The lists
// ---------------------------------------------------------------------------
int shipwright_ui_rows(const shipwright_ui *ui, const shipwright_view *view, shop_row *out, int capacity) {
    int n = 0;
    const character *c = view->books;
    if (!c) return 0;
    if (ui->tab == SHOP_HULLS) {
        for (int h = 0; h < HULL_COUNT && n < capacity; ++h) out[n++] = {SHOP_ROW_HULL, h};
    } else if (ui->tab == SHOP_FITTINGS) {
        const ship_loadout *l = fleet_sailing(c);
        ship_slot slots[SHIP_MAX_SLOTS];
        int count = ship_slots(l->hull, slots, SHIP_MAX_SLOTS);
        for (int kind = 0; kind < SLOT_KINDS; ++kind) {
            if (ship_hull(l->hull)->slots[kind] <= 0) continue;
            if (n < capacity) out[n++] = {SHOP_ROW_KIND, kind};
            if (kind != ui->expanded) continue;
            for (int i = 0; i < count && n < capacity; ++i)
                if (slots[i].kind == kind) out[n++] = {SHOP_ROW_SLOT, i};
        }
    } else if (ui->tab == SHOP_LOOKS) {
        for (int f = 0; f < LOOK_FIELDS && n < capacity; ++f) out[n++] = {SHOP_ROW_LOOK, f};
    } else {
        for (int k = 1; k < ship_component_count() && n < capacity; ++k)
            if (c->ships.store[k] > 0) out[n++] = {SHOP_ROW_STORED, k};
    }
    return n;
}

// The kind of slot a fittings row is about.
static int row_slot_kind(const character *c, shop_row row) {
    if (row.kind == SHOP_ROW_KIND) return row.index;
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(fleet_sailing(c)->hull, slots, SHIP_MAX_SLOTS);
    return row.index >= 0 && row.index < n ? slots[row.index].kind : -1;
}

// What is fitted where a fittings row points: its slot's, or its kind's when every
// slot of the kind holds the same (-1: they differ).
static int row_fitted(const character *c, shop_row row) {
    const ship_loadout *l = fleet_sailing(c);
    if (row.kind == SHOP_ROW_SLOT) return l->fit[row.index];
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(l->hull, slots, SHIP_MAX_SLOTS), fitted = -2;
    for (int i = 0; i < n; ++i) {
        if (slots[i].kind != row.index) continue;
        if (fitted == -2) fitted = l->fit[i];
        else if (fitted != l->fit[i]) return -1;
    }
    return fitted < 0 ? -1 : fitted;
}

static int find_row(const shop_row *rows, int n, shop_row wanted) {
    for (int i = 0; i < n; ++i)
        if (rows[i].kind == wanted.kind && rows[i].index == wanted.index) return i;
    return -1;
}

// Selects a row of the list: a kind's heading opens its slots under it, and the
// choice starts at what is fitted there.
static void select_row(shipwright_ui *ui, const shipwright_view *view, shop_row row) {
    if (ui->tab == SHOP_FITTINGS) {
        int kind = row_slot_kind(view->books, row);
        if (kind >= 0) ui->expanded = kind;
        int fitted = row_fitted(view->books, row);
        int choices[SHIP_CATALOGUE_MAX];
        int count = shipwright_ui_choices(kind, choices, SHIP_CATALOGUE_MAX);
        ui->choice = fitted >= 0 ? fitted : count > 0 ? choices[0] : SHIP_NOTHING;
    } else if (ui->tab == SHOP_LOOKS && row.kind == SHOP_ROW_LOOK) {
        ui->choice = fleet_sailing(view->books)->look.field[row.index];
    }
    shop_row rows[64];
    int n = shipwright_ui_rows(ui, view, rows, 64);
    int at = find_row(rows, n, row);
    ui->row = at >= 0 ? at : 0;
    if (ui->row < ui->scroll) ui->scroll = ui->row;
    if (ui->row >= ui->scroll + SHOP_LIST_ROWS) ui->scroll = ui->row - SHOP_LIST_ROWS + 1;
}

void shipwright_ui_open(shipwright_ui *ui) {
    float turn = ui->turn;
    memset(ui, 0, sizeof(*ui));
    ui->open = true;
    ui->turn = turn;
    ui->tab = SHOP_FITTINGS;
    ui->expanded = SLOT_MAIN;
    ui->choice = -1;   // set from the books at the first update
}

void shipwright_ui_close(shipwright_ui *ui) { ui->open = false; }

// ---------------------------------------------------------------------------
// The button, and what the selection would make of her
// ---------------------------------------------------------------------------
static void set_notice(shipwright_ui *ui, const char *text, bool bad) {
    snprintf(ui->notice, sizeof(ui->notice), "%s", text);
    ui->notice_bad = bad;
    ui->notice_time = 4.0f;
}

static void work_out(shipwright_ui *ui, const shipwright_view *view, const shop_row *row) {
    const character *c = view->books;
    ui->can_order = false;
    ui->has_preview = false;
    ui->look_preview = false;
    memset(&ui->pending, 0, sizeof(ui->pending));
    snprintf(ui->button, sizeof(ui->button), " ");
    if (!c || !row) return;
    char price[32];
    const ship_loadout *sailing = fleet_sailing(c);
    ship_loadout preview = *sailing;
    shipwright_order o = {};
    if (ui->tab == SHOP_HULLS) {
        int h = row->index;
        preview = fleet_owns(c, h) ? c->ships.loadouts[h] : ship_default_loadout(h);
        o.hull = (uint8_t)h;
        if (!fleet_owns(c, h)) {
            o.op = SHIPWRIGHT_BUY_HULL;
            shipwright_format_doubloons(ship_hull_price(h), price, sizeof(price));
            snprintf(ui->button, sizeof(ui->button), "BUY HER: %s", price);
        } else if (c->ships.sailing != h) {
            o.op = SHIPWRIGHT_SAIL;
            snprintf(ui->button, sizeof(ui->button), "SAIL HER");
        } else {
            snprintf(ui->button, sizeof(ui->button), "YOU SAIL HER");
        }
    } else if (ui->tab == SHOP_FITTINGS) {
        o.op = row->kind == SHOP_ROW_KIND ? SHIPWRIGHT_FIT_ALL : SHIPWRIGHT_FIT;
        o.slot = (uint8_t)row->index;
        o.component = (uint8_t)(ui->choice < 0 ? 0 : ui->choice);
        ship_slot slots[SHIP_MAX_SLOTS];
        int n = ship_slots(sailing->hull, slots, SHIP_MAX_SLOTS), changed = 0;
        for (int i = 0; i < n; ++i)
            if ((o.op == SHIPWRIGHT_FIT ? i == o.slot : slots[i].kind == o.slot) && preview.fit[i] != o.component) {
                preview.fit[i] = o.component;
                ++changed;
            }
        int64_t cost = 0;
        int result = shipwright_quote(c, &o, &cost);
        shipwright_format_doubloons(cost, price, sizeof(price));
        const char *verb = o.op == SHIPWRIGHT_FIT_ALL ? "FIT ALL" : "FIT";
        if (result == SHIPWRIGHT_UNCHANGED) snprintf(ui->button, sizeof(ui->button), "FITTED");
        else if (result != SHIPWRIGHT_DONE) snprintf(ui->button, sizeof(ui->button), "%s", shipwright_result_text(result));
        else if (o.component == SHIP_NOTHING) snprintf(ui->button, sizeof(ui->button), "TAKE OUT %d, INTO STORE", changed);
        else if (cost == 0) snprintf(ui->button, sizeof(ui->button), "%s FROM STORE", verb);
        else snprintf(ui->button, sizeof(ui->button), "%s: %s", verb, price);
        if (result != SHIPWRIGHT_DONE) o.op = SHIPWRIGHT_SHOW;   // nothing to order
    } else if (ui->tab == SHOP_LOOKS) {
        o.op = SHIPWRIGHT_STYLE;
        o.slot = (uint8_t)row->index;
        o.component = (uint8_t)(ui->choice < 0 ? 0 : ui->choice);
        preview.look.field[row->index] = o.component;
        ui->look_preview = true;
        ui->preview_look = preview.look;
        int64_t cost = 0;
        int result = shipwright_quote(c, &o, &cost);
        shipwright_format_doubloons(cost, price, sizeof(price));
        static const char *const VERBS[LOOK_FIELDS] = {"PAINT HER", "PAINT HER TRIM", "PAINT HER THRUSTERS", "PAINT HER SAILS", "CUT HER SAILS",
                                                      "REBUILD HER"};
        if (result == SHIPWRIGHT_UNCHANGED) snprintf(ui->button, sizeof(ui->button), "AS SHE IS");
        else if (result != SHIPWRIGHT_DONE) snprintf(ui->button, sizeof(ui->button), "%s", shipwright_result_text(result));
        else snprintf(ui->button, sizeof(ui->button), "%s: %s", VERBS[row->index], price);
        if (result != SHIPWRIGHT_DONE) o.op = SHIPWRIGHT_SHOW;
    } else {
        o.op = SHIPWRIGHT_SELL;
        o.component = (uint8_t)row->index;
        shipwright_format_doubloons(ship_component_price(row->index) / 2, price, sizeof(price));
        snprintf(ui->button, sizeof(ui->button), "SELL ONE: +%s", price);
    }
    if (o.op != SHIPWRIGHT_SHOW) {
        int64_t cost = 0;
        int result = shipwright_quote(c, &o, &cost);
        ui->can_order = result == SHIPWRIGHT_DONE;
        if (result == SHIPWRIGHT_CANNOT_AFFORD) snprintf(ui->button, sizeof(ui->button), "%s", shipwright_result_text(result));
    }
    ui->pending = o;
    if (ui->tab == SHOP_HULLS || ui->tab == SHOP_FITTINGS) {
        ui->has_preview = game_ship_stats(sailing, &ui->now) && game_ship_stats(&preview, &ui->preview);
    }
}

bool shipwright_ui_update(shipwright_ui *ui, float aspect, const shipwright_view *view, const shipwright_input *in, float dt,
                          shipwright_order *order) {
    if (!ui->open) return false;
    ui->turn += dt * 0.15f;
    if (ui->notice_time > 0.0f) ui->notice_time -= dt;
    bool asked = false;
    const character *c = view->books;
    if (!c) return false;
    shop_row rows[64];
    int n = shipwright_ui_rows(ui, view, rows, 64);
    if (ui->choice < 0 && n > 0) select_row(ui, view, rows[ui->row < n ? ui->row : 0]);

    // Tabs: the key cycles, a click picks.
    int tab = ui->tab;
    if (in->next_tab) tab = (tab + 1) % SHOP_TABS;
    for (int t = 0; t < SHOP_TABS && in->pointer && in->click; ++t)
        if (inside(shipwright_ui_tab_box(aspect, t), in->x, in->y)) tab = t;
    if (tab != ui->tab) {
        ui->tab = tab;
        ui->row = ui->scroll = 0;
        n = shipwright_ui_rows(ui, view, rows, 64);
        if (n > 0) select_row(ui, view, rows[0]);
    }
    // The list: the keys move along it, the wheel scrolls it, a click picks.
    n = shipwright_ui_rows(ui, view, rows, 64);
    if (ui->row >= n) ui->row = n > 0 ? n - 1 : 0;
    int row = ui->row;
    if (in->up && row > 0) --row;
    if (in->down && row + 1 < n) ++row;
    if (in->scroll != 0.0f) {
        ui->scroll -= (int)(in->scroll > 0.0f ? 1 : -1);
        if (ui->scroll > n - SHOP_LIST_ROWS) ui->scroll = n - SHOP_LIST_ROWS;
        if (ui->scroll < 0) ui->scroll = 0;
    }
    bool clicked = false;
    for (int shown = 0; shown < SHOP_LIST_ROWS && in->pointer && in->click; ++shown)
        if (ui->scroll + shown < n && inside(shipwright_ui_row_box(aspect, shown), in->x, in->y)) {
            row = ui->scroll + shown;
            clicked = true;
        }
    if (n > 0 && (row != ui->row || clicked)) {
        select_row(ui, view, rows[row]);
        n = shipwright_ui_rows(ui, view, rows, 64);
    }
    // The choices, for a slot or a kind of them, or a look: the keys cycle, a click picks.
    if ((ui->tab == SHOP_FITTINGS || ui->tab == SHOP_LOOKS) && n > 0 && ui->row < n) {
        int choices[SHIP_CATALOGUE_MAX];
        int count = 0, at = 0;
        if (ui->tab == SHOP_LOOKS)
            for (int v = 0; v < ship_look_options(rows[ui->row].index) && count < SHIP_CATALOGUE_MAX; ++v) choices[count++] = v;
        else count = shipwright_ui_choices(row_slot_kind(c, rows[ui->row]), choices, SHIP_CATALOGUE_MAX);
        for (int i = 0; i < count; ++i)
            if (choices[i] == ui->choice) at = i;
        if (in->left && count > 0) at = (at + count - 1) % count;
        if (in->right && count > 0) at = (at + 1) % count;
        for (int i = 0; i < count && in->pointer && in->click; ++i)
            if (inside(shipwright_ui_choice_box(aspect, i), in->x, in->y)) at = i;
        if (count > 0) ui->choice = choices[at];
    }
    work_out(ui, view, n > 0 && ui->row < n ? &rows[ui->row] : NULL);
    // The button, or Enter.
    bool pressed = in->enter || (in->pointer && in->click && inside(shipwright_ui_button_box(aspect), in->x, in->y));
    if (pressed && ui->can_order) {
        *order = ui->pending;
        asked = true;
    }
    return asked;
}

void shipwright_ui_answer(shipwright_ui *ui, int result, const shipwright_order *order) {
    char text[80];
    if (order->op == SHIPWRIGHT_SHOW && result == SHIPWRIGHT_DONE) return;
    if (result != SHIPWRIGHT_DONE) {
        set_notice(ui, shipwright_result_text(result), true);
        return;
    }
    const ship_component *part = ship_component_info(order->component);
    const hull_spec *hull = ship_hull(order->hull);
    switch (order->op) {
    case SHIPWRIGHT_BUY_HULL: snprintf(text, sizeof(text), "THE %s IS YOURS", hull ? hull->name : "?"); break;
    case SHIPWRIGHT_SAIL: snprintf(text, sizeof(text), "YOU SAIL THE %s NOW", hull ? hull->name : "?"); break;
    case SHIPWRIGHT_SELL: snprintf(text, sizeof(text), "SOLD: %s", part ? part->name : "?"); break;
    case SHIPWRIGHT_STYLE:
        snprintf(text, sizeof(text), "%s: %s", ship_look_field_name(order->slot), ship_look_option_name(order->slot, order->component));
        break;
    default:
        if (order->component == SHIP_NOTHING) snprintf(text, sizeof(text), "TAKEN OUT, AND INTO YOUR STORE");
        else snprintf(text, sizeof(text), "FITTED: %s", part ? part->name : "?");
        break;
    }
    set_notice(ui, text, false);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static const v3 PANEL = {0.05f, 0.04f, 0.035f}, EDGE = {0.45f, 0.34f, 0.16f}, HOVER = {0.1f, 0.085f, 0.07f};
static const v3 PICKED = {0.2f, 0.14f, 0.05f}, GOLD = {1.0f, 0.85f, 0.3f}, WHITE = {0.95f, 0.95f, 0.9f};
static const v3 DIM = {0.55f, 0.55f, 0.6f}, GREEN = {0.5f, 0.9f, 0.5f}, RED = {0.95f, 0.45f, 0.4f}, AMBER = {1.0f, 0.6f, 0.2f};

static void panel(draw_list *list, const hud_frame *f, float x0, float y0, float x1, float y1) {
    hud_rect(list, f, x0 - 0.006f, y0 - 0.006f, x1 + 0.006f, y1 + 0.006f, EDGE, 4);
    hud_rect(list, f, x0, y0, x1, y1, PANEL, 3);
}

// A square of paint, its top-left at (x, y).
static void swatch(draw_list *list, const hud_frame *f, float x, float y, v3 color) {
    hud_rect(list, f, x - 0.003f, y - 0.033f, x + 0.033f, y + 0.003f, v3_make(0.6f, 0.55f, 0.45f), 2);
    hud_rect(list, f, x, y - 0.03f, x + 0.03f, y, color, 1);
}

// A row's box, lit when picked or under the pointer.
static void row_back(draw_list *list, const hud_frame *f, shop_box b, bool picked, bool hover) {
    if (picked) hud_rect(list, f, b.x0, b.y0, b.x1, b.y1, PICKED, 2);
    else if (hover) hud_rect(list, f, b.x0, b.y0, b.x1, b.y1, HOVER, 2);
}

// Text across lines of at most `width` characters, broken at spaces; the lines drawn.
static int wrapped(draw_list *list, const hud_frame *f, float x, float y, const char *text, v3 color, float size, int width, int max_lines) {
    int lines = 0;
    const char *at = text;
    while (*at && lines < max_lines) {
        int length = (int)strlen(at), take = length;
        if (length > width) {
            take = width;
            while (take > 0 && at[take] != ' ') --take;
            if (take == 0) take = width;
        }
        char line[128];
        snprintf(line, sizeof(line), "%.*s", take < 127 ? take : 127, at);
        hud_text(list, f, x, y - (float)lines * HUD_LINE_HEIGHT * size, line, color, size, false);
        ++lines;
        at += take;
        while (*at == ' ') ++at;
    }
    return lines;
}

// One line of the comparison: the name, her number now and with the selection,
// green where it is better, red where it is worse.
static void compare(draw_list *list, const hud_frame *f, float x0, float x1, float y, const char *name, const char *format, float now,
                    float then, bool less_is_better) {
    char a[32], b[32];
    snprintf(a, sizeof(a), format, now);
    snprintf(b, sizeof(b), format, then);
    float change = now != 0.0f ? (then - now) / fabsf(now) : then - now;
    bool better = less_is_better ? change < -0.005f : change > 0.005f, worse = less_is_better ? change > 0.005f : change < -0.005f;
    hud_text(list, f, x0, y, name, DIM, SMALL, false);
    hud_text(list, f, x0 + 0.64f, y, a, WHITE, SMALL, true);
    hud_text(list, f, x1, y, b, better ? GREEN : worse ? RED : WHITE, SMALL, true);
}

void shipwright_ui_draw(draw_list *list, const hud_frame *f, const shipwright_ui *ui, const shipwright_view *view,
                        const shipwright_input *in) {
    if (!ui->open) return;
    float aspect = f->aspect, l0 = left_of(aspect), l1 = l0 + PANEL_WIDTH, r1 = right_of(aspect), r0 = r1 - PANEL_WIDTH;
    char line[96], money[32];
    panel(list, f, l0, -0.94f, l1, 0.94f);
    panel(list, f, r0, -0.94f, r1, 0.94f);
    hud_text(list, f, l0 + PAD, 0.9f, "THE SHIPWRIGHT", GOLD, TITLE, false);
    const character *c = view->books;
    if (!c) {
        hud_text(list, f, l0 + PAD, 0.8f, "HE IS LOOKING FOR YOUR PAPERS...", DIM, TEXT, false);
        return;
    }
    // The purse.
    if (c->bottomless) {
        shipwright_format_doubloons(c->spent, money, sizeof(money));
        snprintf(line, sizeof(line), "PURSE: BOTTOMLESS (SPENT %s)", money);
    } else {
        shipwright_format_doubloons(c->doubloons, money, sizeof(money));
        snprintf(line, sizeof(line), "PURSE: %s DOUBLOONS", money);
    }
    hud_text(list, f, l0 + PAD, 0.82f, line, GOLD, TEXT, false);
    // The tabs.
    static const char *const TABS[SHOP_TABS] = {"HULLS", "FITTINGS", "LOOKS", "STORE"};
    for (int t = 0; t < SHOP_TABS; ++t) {
        shop_box b = shipwright_ui_tab_box(aspect, t);
        bool hover = in && in->pointer && inside(b, in->x, in->y);
        hud_rect(list, f, b.x0, b.y0, b.x1, b.y1, t == ui->tab ? PICKED : hover ? HOVER : v3_make(0.09f, 0.08f, 0.07f), 2);
        hud_text(list, f, (b.x0 + b.x1) * 0.5f - hud_text_width(TABS[t], TEXT) * 0.5f, b.y1 - 0.022f, TABS[t], t == ui->tab ? GOLD : WHITE, TEXT, false);
    }
    // The list.
    shop_row rows[64];
    int n = shipwright_ui_rows(ui, view, rows, 64);
    const ship_loadout *sailing = fleet_sailing(c);
    for (int shown = 0; shown < SHOP_LIST_ROWS && ui->scroll + shown < n; ++shown) {
        int i = ui->scroll + shown;
        shop_box b = shipwright_ui_row_box(aspect, shown);
        row_back(list, f, b, i == ui->row, in && in->pointer && inside(b, in->x, in->y));
        float y = b.y1 - 0.02f;
        const char *left = "", *right = "";
        v3 right_color = WHITE;
        char label[64] = "", value[48] = "";
        shop_row r = rows[i];
        if (r.kind == SHOP_ROW_HULL) {
            const hull_spec *h = ship_hull(r.index);
            snprintf(label, sizeof(label), "%s  (CREW %d)", h->name, h->crew);
            if (c->ships.sailing == r.index) snprintf(value, sizeof(value), "SAILING"), right_color = GOLD;
            else if (fleet_owns(c, r.index)) snprintf(value, sizeof(value), "OWNED"), right_color = GREEN;
            else shipwright_format_doubloons(ship_hull_price(r.index), value, sizeof(value));
        } else if (r.kind == SHOP_ROW_KIND) {
            snprintf(label, sizeof(label), "%s (%d)", kind_plural(r.index), ship_hull(sailing->hull)->slots[r.index]);
            int fitted = row_fitted(c, r);
            snprintf(value, sizeof(value), "%s", fitted < 0 ? "MIXED" : ship_component_info(fitted)->name);
            right_color = fitted < 0 ? AMBER : WHITE;
        } else if (r.kind == SHOP_ROW_SLOT) {
            char slot[48];
            ship_slot_label(sailing->hull, r.index, slot, sizeof(slot));
            snprintf(label, sizeof(label), "  %s", slot);
            snprintf(value, sizeof(value), "%s", ship_component_info(sailing->fit[r.index])->name);
            right_color = DIM;
        } else if (r.kind == SHOP_ROW_LOOK) {
            int v = sailing->look.field[r.index];
            snprintf(label, sizeof(label), "%s", ship_look_field_name(r.index));
            snprintf(value, sizeof(value), "%s", ship_look_option_name(r.index, v));
            v3 paint;
            if (ship_paint_color(v, &paint) && r.index <= LOOK_SAIL_PAINT) swatch(list, f, b.x1 - 0.012f - hud_text_width(value, SMALL) - 0.045f, b.y1 - 0.012f, paint);
        } else {
            snprintf(label, sizeof(label), "%s", ship_component_info(r.index)->name);
            snprintf(value, sizeof(value), "X%u", (unsigned)c->ships.store[r.index]);
        }
        left = label;
        right = value;
        hud_text(list, f, b.x0 + 0.012f, y, left, r.kind == SHOP_ROW_SLOT ? DIM : WHITE, SMALL, false);
        hud_text(list, f, b.x1 - 0.012f, y, right, right_color, SMALL, true);
    }
    if (n == 0) hud_text(list, f, l0 + PAD, ROW_TOP - 0.03f, ui->tab == SHOP_STORE ? "NOTHING IN STORE" : "NOTHING HERE", DIM, TEXT, false);
    if (ui->scroll > 0) hud_text(list, f, l1 - PAD, ROW_TOP + 0.035f, "MORE ABOVE", DIM, SMALL, true);
    if (ui->scroll + SHOP_LIST_ROWS < n)
        hud_text(list, f, l1 - PAD, ROW_TOP - SHOP_LIST_ROWS * ROW_HEIGHT - 0.01f, "MORE BELOW", DIM, SMALL, true);
    // Her, and whether she is as the books say.
    float y = -0.56f;
    snprintf(line, sizeof(line), "YOU SAIL THE %s", ship_hull(sailing->hull)->name);
    hud_text(list, f, l0 + PAD, y, line, WHITE, TEXT, false);
    y -= HUD_LINE_HEIGHT * TEXT + 0.01f;
    if (!view->ship) hud_text(list, f, l0 + PAD, y, "NO BERTH IS FREE FOR HER", AMBER, SMALL, false);
    else if (!ship_loadout_equal(&view->ship->loadout, sailing))
        hud_text(list, f, l0 + PAD, y, view->crew_aboard ? "HER REFIT WAITS: CREW ABOARD" : "SHE IS BEING REFITTED", AMBER, SMALL, false);
    else hud_text(list, f, l0 + PAD, y, "SHE LIES AT HER BERTH, AS FITTED", GREEN, SMALL, false);
    hud_text(list, f, l0 + PAD, -0.82f, "TAB  UP/DOWN  LEFT/RIGHT  ENTER  WHEEL", DIM, SMALL, false);
    hud_text(list, f, l0 + PAD, -0.82f - HUD_LINE_HEIGHT * SMALL - 0.006f, "ESC OR F: LEAVE THE COUNTER", DIM, SMALL, false);

    // The right: what is selected.
    if (n > 0 && ui->row < n) {
        shop_row r = rows[ui->row];
        float x = r0 + PAD;
        y = 0.9f;
        const char *note = "";
        if (r.kind == SHOP_ROW_HULL) {
            const hull_spec *h = ship_hull(r.index);
            snprintf(line, sizeof(line), "THE %s", h->name);
            hud_text(list, f, x, y, line, GOLD, TITLE, false);
            snprintf(line, sizeof(line), "CREW %d, %d MAST%s, %d SLOTS", h->crew, h->slots[SLOT_MAST], h->slots[SLOT_MAST] > 1 ? "S" : "",
                     ship_slot_count(r.index));
            hud_text(list, f, x, 0.82f, line, WHITE, TEXT, false);
            wrapped(list, f, x, 0.74f, h->note, DIM, SMALL, 38, 3);
        } else if (r.kind == SHOP_ROW_STORED) {
            const ship_component *part = ship_component_info(r.index);
            hud_text(list, f, x, y, part->name, GOLD, TITLE, false);
            snprintf(line, sizeof(line), "A %s, IN STORE: %u", ship_slot_name(part->slot), (unsigned)c->ships.store[r.index]);
            hud_text(list, f, x, 0.82f, line, WHITE, TEXT, false);
            wrapped(list, f, x, 0.74f, part->note, DIM, SMALL, 38, 3);
        } else if (r.kind == SHOP_ROW_LOOK) {
            hud_text(list, f, x, y, ship_look_field_name(r.index), GOLD, TITLE, false);
            int now = sailing->look.field[r.index];
            snprintf(line, sizeof(line), "NOW: %s", ship_look_option_name(r.index, now));
            hud_text(list, f, x, 0.82f, line, WHITE, TEXT, false);
            int count = ship_look_options(r.index);
            for (int i = 0; i < count; ++i) {
                shop_box b = shipwright_ui_choice_box(aspect, i);
                row_back(list, f, b, i == ui->choice, in && in->pointer && inside(b, in->x, in->y));
                v3 paint;
                float name_x = b.x0 + 0.012f;
                if (r.index <= LOOK_SAIL_PAINT && ship_paint_color(i, &paint)) swatch(list, f, name_x, b.y1 - 0.012f, paint);
                if (r.index <= LOOK_SAIL_PAINT) name_x += 0.045f;
                hud_text(list, f, name_x, b.y1 - 0.018f, ship_look_option_name(r.index, i), i == now ? GREEN : WHITE, SMALL, false);
                shipwright_format_doubloons(ship_look_price(r.index, i), money, sizeof(money));
                hud_text(list, f, b.x1 - 0.012f, b.y1 - 0.018f, i == now ? "YOURS" : money, DIM, SMALL, true);
            }
            const char *about = ship_look_option_note(r.index, ui->choice < 0 ? 0 : ui->choice);
            float below = CHOICE_TOP - (float)count * CHOICE_HEIGHT - 0.02f;
            int lines = wrapped(list, f, x, below, about, DIM, SMALL, 38, 3);
            below -= (float)lines * HUD_LINE_HEIGHT * SMALL + 0.02f;
            hud_text(list, f, x, below, r.index == LOOK_HULL_SHAPE ? "SEEN ONCE SHE IS REBUILT. SHE FLIES THE SAME." : "TRIED ON HER, THERE. SHE FLIES THE SAME.",
                     AMBER, SMALL, false);
        } else {
            if (r.kind == SHOP_ROW_KIND) snprintf(line, sizeof(line), "ALL %s", kind_plural(r.index));
            else ship_slot_label(sailing->hull, r.index, line, sizeof(line));
            hud_text(list, f, x, y, line, GOLD, TITLE, false);
            int fitted = row_fitted(c, r);
            snprintf(line, sizeof(line), "FITTED: %s", fitted < 0 ? "MIXED" : ship_component_info(fitted)->name);
            hud_text(list, f, x, 0.82f, line, WHITE, TEXT, false);
            int choices[SHIP_CATALOGUE_MAX];
            int count = shipwright_ui_choices(row_slot_kind(c, r), choices, SHIP_CATALOGUE_MAX);
            for (int i = 0; i < count; ++i) {
                shop_box b = shipwright_ui_choice_box(aspect, i);
                row_back(list, f, b, choices[i] == ui->choice, in && in->pointer && inside(b, in->x, in->y));
                const ship_component *part = ship_component_info(choices[i]);
                hud_text(list, f, b.x0 + 0.012f, b.y1 - 0.018f, part->name, choices[i] == fitted ? GREEN : WHITE, SMALL, false);
                if (choices[i] != SHIP_NOTHING && c->ships.store[choices[i]] > 0) snprintf(money, sizeof(money), "IN STORE: %u", (unsigned)c->ships.store[choices[i]]);
                else if (choices[i] == SHIP_NOTHING) snprintf(money, sizeof(money), "FREE");
                else shipwright_format_doubloons(ship_component_price(choices[i]), money, sizeof(money));
                hud_text(list, f, b.x1 - 0.012f, b.y1 - 0.018f, money, DIM, SMALL, true);
            }
            const ship_component *picked = ship_component_info(ui->choice < 0 ? 0 : ui->choice);
            if (picked) note = picked->note;
            wrapped(list, f, x, CHOICE_TOP - (float)count * CHOICE_HEIGHT - 0.02f, note, DIM, SMALL, 38, 2);
        }
        // What it would make of her.
        if (ui->has_preview) {
            y = 0.3f;
            hud_text(list, f, x, y, "HER NUMBERS", DIM, SMALL, false);
            hud_text(list, f, x + 0.64f, y, "NOW", DIM, SMALL, true);
            hud_text(list, f, r1 - PAD, y, r.kind == SHOP_ROW_HULL ? "THAT SHIP" : "WITH IT", DIM, SMALL, true);
            const ship_stats *a = &ui->now, *b = &ui->preview;
            float step = HUD_LINE_HEIGHT * SMALL + 0.012f;
            compare(list, f, x, r1 - PAD, y -= step, "MASS, TONNES", "%.1f", a->mass / 1000.0f, b->mass / 1000.0f, true);
            compare(list, f, x, r1 - PAD, y -= step, "AHEAD, M/S2", "%.1f", a->accel[CRAFT_GROUP_FORWARD], b->accel[CRAFT_GROUP_FORWARD], false);
            compare(list, f, x, r1 - PAD, y -= step, "LIFT / WEIGHT", "%.2f", a->lift_ratio, b->lift_ratio, false);
            compare(list, f, x, r1 - PAD, y -= step, "TURN, RAD/S2", "%.2f", a->turn_accel.y, b->turn_accel.y, false);
            compare(list, f, x, r1 - PAD, y -= step, "TOP SPEED, M/S", "%.0f", a->top_speed[CRAFT_GROUP_FORWARD], b->top_speed[CRAFT_GROUP_FORWARD], false);
            compare(list, f, x, r1 - PAD, y -= step, "TOP TURN, RAD/S", "%.2f", a->top_rate.y, b->top_rate.y, false);
            compare(list, f, x, r1 - PAD, y -= step, "SAIL, SQ M", "%.0f", a->sail_area, b->sail_area, false);
            compare(list, f, x, r1 - PAD, y -= step, "SAIL POWER", "%.0f", a->sail_power, b->sail_power, false);
            compare(list, f, x, r1 - PAD, y -= step, "HEEL UNDER SAIL", "%.2f", a->heel, b->heel, true);
        }
    }
    // The button.
    shop_box b = shipwright_ui_button_box(aspect);
    bool hover = in && in->pointer && inside(b, in->x, in->y);
    hud_rect(list, f, b.x0, b.y0, b.x1, b.y1, ui->can_order ? (hover ? v3_make(0.55f, 0.4f, 0.12f) : v3_make(0.42f, 0.3f, 0.1f)) : v3_make(0.12f, 0.11f, 0.1f), 2);
    hud_text(list, f, (b.x0 + b.x1) * 0.5f - hud_text_width(ui->button, TEXT) * 0.5f, (b.y0 + b.y1) * 0.5f + 3.5f * TEXT, ui->button,
             ui->can_order ? WHITE : DIM, TEXT, false);
    // The shipwright's word, over the ship.
    if (ui->notice_time > 0.0f && ui->notice[0])
        hud_text(list, f, -hud_text_width(ui->notice, TEXT) * 0.5f, -0.86f, ui->notice, ui->notice_bad ? RED : GREEN, TEXT, false);
}
