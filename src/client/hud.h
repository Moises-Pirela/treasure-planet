// The overlay: text and panels drawn with what the renderer has, which is meshes.
// Every run of lit pixels along a row of a glyph is a tiny unlit quad placed a
// quarter of a metre in front of the eye, in the camera's frame, so it stays on screen whatever the camera does
// and needs no second pipeline. A mock-up, and honest about it: a real UI pass
// comes with a texture and an orthographic projection later, and this is the layer
// it replaces.
//
// Coordinates: a virtual screen with y from -1 (bottom) to 1 (top) and x from
// -aspect to aspect, matching what a 70 degree vertical field of view shows.
#ifndef TP_HUD_H
#define TP_HUD_H

#include "../game/game.h"
#include "../net/social.h"
#include "../render/renderer.h"

#define HUD_FOV (70.0f * TP_PI / 180.0f)   // must match the game camera's
#define HUD_DISTANCE 0.25f                 // metres in front of the eye the overlay hangs
#define HUD_GLYPH_WIDTH 5
#define HUD_GLYPH_HEIGHT 7
#define HUD_CELL_WIDTH 6                   // glyph plus a column of space
#define HUD_LINE_HEIGHT 9                  // glyph plus two rows of space

struct draw_list {
    r_draw *items;
    size_t count, capacity;
};
r_draw *draw_push(draw_list *list);

// The camera's frame for this frame.
struct hud_frame {
    v3 eye, right, up, forward;
    float aspect;
    r_mesh quad;                 // a cube mesh does: it is scaled flat
};
hud_frame hud_begin(v3 eye, float yaw, float pitch, float aspect, r_mesh cube);
// The same for a camera that may be rolled: `up` need only be roughly up.
hud_frame hud_begin_view(v3 eye, v3 forward, v3 up, float aspect, r_mesh cube);

// Draws `text` with its top-left (or top-right, when right_aligned) at (x, y) in
// screen units, each pixel `pixel` screen units wide. Lowercase is drawn as capitals.
void hud_text(draw_list *list, const hud_frame *frame, float x, float y, const char *text, v3 color, float pixel,
              bool right_aligned);
float hud_text_width(const char *text, float pixel);   // screen units
// A filled rectangle between two corners, in screen units, behind the text: `layer`
// 1 just behind it (a highlight), 2 further (a panel), and so on.
void hud_rect(draw_list *list, const hud_frame *frame, float x0, float y0, float x1, float y1, v3 color, int layer);

// The party panel, top right: members one under another with who leads, where they
// are and whether they are online; or the invitation waiting; or how to start one.
void hud_party(draw_list *list, const hud_frame *frame, const party_view *party, const char *my_name);
// The gear panel, bottom right: what is worn, slot by slot in rarity colours, the
// stash (numbered, for wearing with the number keys), and the last pickup.
void hud_gear(draw_list *list, const hud_frame *frame, const character *gear, const item *last_pickup, float pickup_flash);
// Loot in view, bottom centre: its name in its rarity's colour.
void hud_loot(draw_list *list, const hud_frame *frame, const item *it);
// At a helm, bottom left: the craft, how fast it goes and climbs, how high it is
// and how hard gravity pulls, the flight computer's mode, what its thrusters are
// doing, and the keys; and in the middle of the screen the stick: a ring where it
// rests and a mark where it is. A passenger gets the readings without the controls.
enum hud_flight_state { HUD_FLIGHT, HUD_DECOUPLED, HUD_LANDING, HUD_LANDED, HUD_UNMANNED, HUD_DOCKING, HUD_DOCKED };
struct hud_flight_view {
    const char *craft;
    bool pilot;                  // false: a passenger's panel
    float speed, climb;          // m/s, and m/s away from gravity
    float altitude;              // metres above what is under it; < 0: nothing in range
    float gravity;               // m/s^2
    int state;                   // hud_flight_state
    const char *dock;            // the dock she makes for or lies at (HUD_DOCKING, HUD_DOCKED)
    int crew;                    // players aboard, the pilot included
    int complement;              // the crew her hull is meant for (0: not said)
    int masts;                   // how many sails the SAIL and FILL bars speak for
    float sail, sail_fill;       // all her sail as one: 0..1 set, -1..1 filled (aback when negative)
    bool currents;               // whether the solar current flows (off for now: no current, no fill)
    float current;               // m/s, the solar current where the craft is
    float current_from;          // degrees off the bow it comes from, right positive (180: astern)
    float stick_x, stick_y;      // -1..1, right and up
    float throttle[CRAFT_GROUPS];    // 0..1 per thruster group (game.h)
};
void hud_flight(draw_list *list, const hud_frame *frame, const hud_flight_view *view);
// At a sail station, bottom left: how far its mast's sail is set, a scale of its
// brace (where it is, and where it would draw best), the current, and what the
// sail is doing with it.
struct hud_sails_view {
    const char *craft;
    const char *mast;            // which mast's, on a ship of more than one ("MAINMAST"); NULL or "MAST": her only one
    float sail, fill;            // 0..1 set; -1..1 how well it draws
    float brace, best_brace;     // radians, to starboard positive
    float brace_max;
    bool currents;               // whether the solar current flows (off for now: the sail is for show)
    float current, current_from; // m/s; degrees off the bow it comes from, right positive
    float sailing_speed;         // m/s the sail drives her towards (negative: astern)
    float speed;                 // m/s she makes along her bow
};
void hud_sails(draw_list *list, const hud_frame *frame, const hud_sails_view *view);
// Where a point in the world lands on the overlay's screen (its x and y units);
// false when it is behind the eye.
bool hud_project(const hud_frame *frame, v3 point, float *x, float *y);
// Studying a star chart, right of middle: where everything is from the ship, one
// line each (how far, which way off the bow, how far above or below its deck).
struct hud_chart_line {
    const char *name;
    v3 color;
    float distance;              // metres, to its surface
    float bearing;               // degrees off the bow, right positive
    float rise;                  // metres above the ship's deck, along its up
};
void hud_chart(draw_list *list, const hud_frame *frame, const hud_chart_line *lines, int count);
// A small label centred at a point on the screen.
void hud_label(draw_list *list, const hud_frame *frame, float x, float y, const char *text, v3 color);
// A line of advice above the bottom centre: what a key would do here.
void hud_prompt(draw_list *list, const hud_frame *frame, const char *text, v3 color);
// What a player wears, drawn on their capsule: a box per slot, coloured by rarity.
void hud_draw_gear(draw_list *list, r_mesh cube, v3 feet, float yaw, const Equipment *gear);

// The 5x7 glyph of a character, one byte per row, bit 4 the leftmost column.
const uint8_t *hud_glyph(char c);

#endif
