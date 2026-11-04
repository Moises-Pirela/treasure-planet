// The overlay: text and panels drawn with what the renderer has, which is meshes.
// Every pixel of a glyph is a tiny unlit quad placed a quarter of a metre in front
// of the eye, in the camera's frame, so it stays on screen whatever the camera does
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

// The party panel, top right: members one under another with who leads, where they
// are and whether they are online; or the invitation waiting; or how to start one.
void hud_party(draw_list *list, const hud_frame *frame, const party_view *party, const char *my_name);
// The gear panel, bottom right: what is worn, slot by slot in rarity colours, the
// stash (numbered, for wearing with the number keys), and the last pickup.
void hud_gear(draw_list *list, const hud_frame *frame, const character *gear, const item *last_pickup, float pickup_flash);
// Loot in view, bottom centre: its name in its rarity's colour.
void hud_loot(draw_list *list, const hud_frame *frame, const item *it);
// At a helm, bottom left: the craft, how fast it goes and climbs, the flight
// computer's state, what its thrusters are doing, and the keys; and in the middle
// of the screen the stick: a ring where it rests and a mark where it is.
struct hud_flight_view {
    const char *craft;
    float speed, climb;          // m/s, and m/s up the world's y
    bool assist;
    float stick_x, stick_y;      // -1..1, right and up
    float throttle[CRAFT_GROUPS];    // 0..1 per thruster group (game.h)
};
void hud_flight(draw_list *list, const hud_frame *frame, const hud_flight_view *view);
// A line of advice above the bottom centre: what a key would do here.
void hud_prompt(draw_list *list, const hud_frame *frame, const char *text, v3 color);
// What a player wears, drawn on their capsule: a box per slot, coloured by rarity.
void hud_draw_gear(draw_list *list, r_mesh cube, v3 feet, float yaw, const Equipment *gear);

// The 5x7 glyph of a character, one byte per row, bit 4 the leftmost column.
const uint8_t *hud_glyph(char c);

#endif
