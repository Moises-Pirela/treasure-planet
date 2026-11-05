// The shipwright's screen: opened at the stall's counter in the yard, a vendor's
// books either side of a view of the player's own ship turning on its berth.
//
//   left:   the purse, four tabs (HULLS, FITTINGS, LOOKS, STORE) and the tab's list
//   middle: the ship itself, in the world, rebuilt as the orders go through
//   right:  what is selected: its choices, what it costs, what it would make of her
//           (her stats now, and with it), and the button that orders it. A look
//           chosen is tried on the ship in the middle before it is bought (paint
//           and the cut of her sails; a hull's shape shows once she is built).
//
// The screen decides nothing: it turns the pointer and the keys into orders
// (shipwright.h) for whoever keeps the books, the server or a client alone, and
// shows their answer. It is plain data and draws with the overlay (hud.h), so it
// runs without a window (tests/test_hud.cpp clicks through it).
//
// Keys: Tab the next tab, Up and Down the list, Left and Right the choices, Enter
// the button; the wheel scrolls the list; Esc or the use key closes it.
#ifndef TP_SHIPWRIGHT_UI_H
#define TP_SHIPWRIGHT_UI_H

#include "../game/shipwright.h"
#include "hud.h"

enum shop_tab { SHOP_HULLS, SHOP_FITTINGS, SHOP_LOOKS, SHOP_STORE, SHOP_TABS };
#define SHOP_LIST_ROWS 16        // rows of a list shown at once

// A row of a tab's list.
enum shop_row_kind { SHOP_ROW_HULL, SHOP_ROW_KIND, SHOP_ROW_SLOT, SHOP_ROW_STORED, SHOP_ROW_LOOK };
struct shop_row {
    int kind;                    // shop_row_kind
    int index;                   // a hull, a slot_kind (every slot of it), a slot, a catalogue index, a look_field
};

struct shipwright_ui {
    bool open;
    int tab;                     // shop_tab
    int row;                     // the selected row of the tab's list
    int scroll;                  // the first row shown
    int expanded;                // fittings: the slot_kind whose slots are listed under it
    int choice;                  // fittings: the catalogue index picked for the selection; looks: the field's value
    char notice[80];             // the shipwright's last word, for a few seconds
    bool notice_bad;
    float notice_time;
    float turn;                  // radians round the ship the camera has come
    // What the button would do, and what it would make of her, as the last update found them.
    bool can_order;
    shipwright_order pending;
    char button[64];
    bool has_preview;
    ship_stats now, preview;     // her stats as she is, and with the selection
    bool look_preview;           // a look being tried on her: drawn so in the middle
    ship_look preview_look;
};

struct shipwright_input {
    float x, y;                  // the pointer, in the overlay's units (hud.h)
    bool pointer;                // the pointer is free: hover and clicks count
    bool click;
    float scroll;                // wheel notches, away positive
    bool up, down, left, right, enter, next_tab;   // pressed this frame
};

// What the screen shows besides the books: whose they are, and their ship.
struct shipwright_view {
    const character *books;      // NULL until the character is known
    const Craft *ship;           // the player's craft, if there is one
    bool crew_aboard;            // someone is aboard it: the refit waits for them to leave
};

void shipwright_ui_open(shipwright_ui *ui);
void shipwright_ui_close(shipwright_ui *ui);
// One frame of it: the pointer, the keys and the wheel. True, with `order` filled,
// when the player asks for something.
bool shipwright_ui_update(shipwright_ui *ui, float aspect, const shipwright_view *view, const shipwright_input *in, float dt,
                          shipwright_order *order);
// The shipwright's answer to an order: said on the screen.
void shipwright_ui_answer(shipwright_ui *ui, int result, const shipwright_order *order);
void shipwright_ui_draw(draw_list *list, const hud_frame *frame, const shipwright_ui *ui, const shipwright_view *view,
                        const shipwright_input *in);

// The tab's list as it stands, into `out` (at most `capacity`); the count.
int shipwright_ui_rows(const shipwright_ui *ui, const shipwright_view *view, shop_row *out, int capacity);
// Where things are on the screen, for the pointer and for tests: a tab's button, a
// list row as shown (0 the first shown), a choice, and the order button.
struct shop_box {
    float x0, y0, x1, y1;
};
shop_box shipwright_ui_tab_box(float aspect, int tab);
shop_box shipwright_ui_row_box(float aspect, int shown);
shop_box shipwright_ui_choice_box(float aspect, int choice);
shop_box shipwright_ui_button_box(float aspect);
// The choices for a kind of slot, in the order shown: the catalogue's fittings
// for it (and nothing, where a slot may be empty); returns the count.
int shipwright_ui_choices(int slot_kind, int *out, int capacity);
// "12,400"
void shipwright_format_doubloons(int64_t amount, char *out, size_t capacity);

#endif
