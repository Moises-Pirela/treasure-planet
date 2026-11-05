// The shipwright: where a player's doubloons become ships. The rules of the trade,
// as pure functions on a character (items.h), so the server, a client alone and the
// tests all keep the same books. Her look (her paints, the cut of her sails, the
// shape of her hull) is bought the same way, a field at a time: each costs the
// same whatever it was before, and changes nothing of how she flies.
//
// A player owns hulls and sails one of them; each owned hull keeps its own fittings
// (its loadout), and fittings taken out go into the player's store, to be fitted
// again for nothing or sold back for half what they cost. Fitting a slot takes the
// fitting from store if there is one there, and buys it if not. A new hull comes
// fitted as standard. Everyone starts with a sloop; and, for now, with a purse that
// never runs dry (`bottomless`), which pays for everything and is never the lighter
// for it (`spent` keeps the tally).
//
// The order is what a client sends (MSG_SHIPWRIGHT): the server checks the player
// is at the shipwright's (game_at_place_kind, PLACE_SHIPWRIGHT), applies it to the
// character it holds for them, saves it, sends it back, and has their ship rebuilt
// to the new loadout (game_refit_craft) if nobody is aboard it.
#ifndef TP_SHIPWRIGHT_H
#define TP_SHIPWRIGHT_H

#include "items.h"

enum shipwright_op {
    SHIPWRIGHT_SHOW,             // nothing bought: bring the ship round to its berth, to be looked at
    SHIPWRIGHT_BUY_HULL,         // `hull`: buy it, fitted as standard
    SHIPWRIGHT_SAIL,             // `hull`: sail this one of those owned
    SHIPWRIGHT_FIT,              // `slot` of the sailed hull: fit `component` (from store, or bought)
    SHIPWRIGHT_FIT_ALL,          // every slot of kind `slot` of the sailed hull: fit `component`
    SHIPWRIGHT_SELL,             // one `component` from store, for half its price
    SHIPWRIGHT_STYLE,            // the look of the sailed hull: field `slot` (look_field) made `component`
    SHIPWRIGHT_OPS
};
struct shipwright_order {
    uint8_t op;                  // shipwright_op
    uint8_t hull;
    uint8_t slot;                // a slot's index (FIT), a slot_kind (FIT_ALL), a look_field (STYLE)
    uint8_t component;           // the catalogue's index; for STYLE, the field's choice
};
enum shipwright_result {
    SHIPWRIGHT_DONE,
    SHIPWRIGHT_UNCHANGED,        // it was so already
    SHIPWRIGHT_CANNOT_AFFORD,
    SHIPWRIGHT_NOT_OWNED,        // a hull to sail that is not theirs
    SHIPWRIGHT_OWNED,            // a hull to buy that is theirs already
    SHIPWRIGHT_NOT_IN_STORE,     // nothing of that to sell
    SHIPWRIGHT_STORE_FULL,
    SHIPWRIGHT_INVALID,          // no such hull, slot or fitting, or one that does not go there
    SHIPWRIGHT_RESULTS
};

// Makes a character's fleet and purse whole: a sloop, fitted as standard, for one
// who has no hull; a sailed hull they own; every owned hull's loadout valid. And,
// for now, the bottomless purse everyone is granted.
void character_fit_out(character *c);
// The loadout of the hull a character sails.
const ship_loadout *fleet_sailing(const character *c);
bool fleet_owns(const character *c, int hull);
// What an order would cost (negative: what it would pay), and whether it could be
// done: the result shipwright_apply would give, without doing it.
int shipwright_quote(const character *c, const shipwright_order *order, int64_t *cost);
// Does it: the character changed (and its version bumped) only when DONE.
int shipwright_apply(character *c, const shipwright_order *order);
const char *shipwright_result_text(int result);   // "NOT ENOUGH DOUBLOONS"

void shipwright_order_write(cw_writer *w, const shipwright_order *order);
bool shipwright_order_read(cw_reader *r, shipwright_order *order);

#endif
