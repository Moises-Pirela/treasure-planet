// The shipwright's books (shipwright.h).
#include "shipwright.h"
#include <string.h>

bool fleet_owns(const character *c, int hull) {
    return hull >= 0 && hull < HULL_COUNT && (c->ships.owned & (1u << hull)) != 0;
}

const ship_loadout *fleet_sailing(const character *c) { return &c->ships.loadouts[c->ships.sailing < HULL_COUNT ? c->ships.sailing : 0]; }

void character_fit_out(character *c) {
    fleet *f = &c->ships;
    f->owned &= (uint8_t)((1u << HULL_COUNT) - 1);
    if (!f->owned) f->owned = 1u << HULL_SLOOP;
    for (int h = 0; h < HULL_COUNT; ++h) {
        if (!fleet_owns(c, h)) memset(&f->loadouts[h], 0, sizeof(f->loadouts[h]));
        else if (f->loadouts[h].hull != h || !ship_loadout_valid(&f->loadouts[h])) f->loadouts[h] = ship_default_loadout(h);
    }
    if (!fleet_owns(c, f->sailing)) {
        f->sailing = HULL_SLOOP;
        for (int h = 0; h < HULL_COUNT && !fleet_owns(c, f->sailing); ++h) f->sailing = (uint8_t)h;
    }
    for (int i = 0; i < SHIP_CATALOGUE_MAX; ++i)
        if (i == SHIP_NOTHING || i >= ship_component_count()) f->store[i] = 0;
        else if (f->store[i] > FLEET_STORE_MAX) f->store[i] = FLEET_STORE_MAX;
    if (c->doubloons < 0) c->doubloons = 0;
    c->bottomless = true;   // for now, everyone's purse: until there is a way to earn doubloons
}

// The slots of the sailed hull an order would change, and for each what is there now.
static int slots_changed(const character *c, const shipwright_order *o, int *which) {
    const ship_loadout *l = fleet_sailing(c);
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(l->hull, slots, SHIP_MAX_SLOTS), count = 0;
    for (int i = 0; i < n; ++i) {
        bool wanted = o->op == SHIPWRIGHT_FIT ? i == o->slot : slots[i].kind == o->slot;
        if (wanted && l->fit[i] != o->component) which[count++] = i;
    }
    return count;
}

// Whether a fitting may go in the sailed hull's slot (FIT) or kind of slot (FIT_ALL).
static bool order_fits(const character *c, const shipwright_order *o) {
    const ship_loadout *l = fleet_sailing(c);
    if (o->component >= ship_component_count()) return false;
    if (o->op == SHIPWRIGHT_FIT_ALL) return o->slot < SLOT_KINDS && ship_fits(o->component, o->slot) && l->hull < HULL_COUNT
                                        && ship_hull(l->hull)->slots[o->slot] > 0;
    ship_slot slots[SHIP_MAX_SLOTS];
    int n = ship_slots(l->hull, slots, SHIP_MAX_SLOTS);
    return o->slot < n && ship_fits(o->component, slots[o->slot].kind);
}

int shipwright_quote(const character *c, const shipwright_order *o, int64_t *cost) {
    *cost = 0;
    switch (o->op) {
    case SHIPWRIGHT_SHOW:
        return SHIPWRIGHT_DONE;
    case SHIPWRIGHT_BUY_HULL:
        if (!ship_hull(o->hull)) return SHIPWRIGHT_INVALID;
        if (fleet_owns(c, o->hull)) return SHIPWRIGHT_OWNED;
        *cost = ship_hull_price(o->hull);
        break;
    case SHIPWRIGHT_SAIL:
        if (!ship_hull(o->hull)) return SHIPWRIGHT_INVALID;
        if (!fleet_owns(c, o->hull)) return SHIPWRIGHT_NOT_OWNED;
        return c->ships.sailing == o->hull ? SHIPWRIGHT_UNCHANGED : SHIPWRIGHT_DONE;
    case SHIPWRIGHT_FIT:
    case SHIPWRIGHT_FIT_ALL: {
        if (!order_fits(c, o)) return SHIPWRIGHT_INVALID;
        int which[SHIP_MAX_SLOTS];
        int n = slots_changed(c, o, which);
        if (n == 0) return SHIPWRIGHT_UNCHANGED;
        // What comes out goes into store; what goes in comes out of it first.
        const ship_loadout *l = fleet_sailing(c);
        int32_t store[SHIP_CATALOGUE_MAX];
        for (int i = 0; i < SHIP_CATALOGUE_MAX; ++i) store[i] = c->ships.store[i];
        for (int k = 0; k < n; ++k) {
            if (o->component != SHIP_NOTHING) {
                if (store[o->component] > 0) --store[o->component];
                else *cost += ship_component_price(o->component);
            }
            int out = l->fit[which[k]];
            if (out != SHIP_NOTHING && ++store[out] > FLEET_STORE_MAX) return SHIPWRIGHT_STORE_FULL;
        }
        break;
    }
    case SHIPWRIGHT_STYLE:
        if (o->slot >= LOOK_FIELDS || o->component >= ship_look_options(o->slot)) return SHIPWRIGHT_INVALID;
        if (fleet_sailing(c)->look.field[o->slot] == o->component) return SHIPWRIGHT_UNCHANGED;
        *cost = ship_look_price(o->slot, o->component);
        break;
    case SHIPWRIGHT_SELL:
        if (o->component == SHIP_NOTHING || o->component >= ship_component_count()) return SHIPWRIGHT_INVALID;
        if (c->ships.store[o->component] == 0) return SHIPWRIGHT_NOT_IN_STORE;
        *cost = -(int64_t)(ship_component_price(o->component) / 2);
        return SHIPWRIGHT_DONE;
    default:
        return SHIPWRIGHT_INVALID;
    }
    if (*cost > 0 && !c->bottomless && c->doubloons < *cost) return SHIPWRIGHT_CANNOT_AFFORD;
    return SHIPWRIGHT_DONE;
}

int shipwright_apply(character *c, const shipwright_order *o) {
    int64_t cost = 0;
    int result = shipwright_quote(c, o, &cost);
    if (result != SHIPWRIGHT_DONE || o->op == SHIPWRIGHT_SHOW) return result;
    fleet *f = &c->ships;
    switch (o->op) {
    case SHIPWRIGHT_BUY_HULL:
        f->owned |= (uint8_t)(1u << o->hull);
        f->loadouts[o->hull] = ship_default_loadout(o->hull);
        break;
    case SHIPWRIGHT_SAIL:
        f->sailing = o->hull;
        break;
    case SHIPWRIGHT_FIT:
    case SHIPWRIGHT_FIT_ALL: {
        int which[SHIP_MAX_SLOTS];
        int n = slots_changed(c, o, which);
        ship_loadout *l = &f->loadouts[f->sailing];
        for (int k = 0; k < n; ++k) {
            if (o->component != SHIP_NOTHING && f->store[o->component] > 0) --f->store[o->component];
            int out = l->fit[which[k]];
            if (out != SHIP_NOTHING) ++f->store[out];
            l->fit[which[k]] = o->component;
        }
        break;
    }
    case SHIPWRIGHT_SELL:
        --f->store[o->component];
        break;
    case SHIPWRIGHT_STYLE:
        f->loadouts[f->sailing].look.field[o->slot] = o->component;
        break;
    default:
        break;
    }
    // The purse: a sale pays into it; a purchase takes from it, unless it is bottomless.
    if (cost < 0) c->doubloons -= cost;
    else if (cost > 0) {
        if (!c->bottomless) c->doubloons -= cost;
        c->spent += cost;
    }
    ++c->version;
    return SHIPWRIGHT_DONE;
}

const char *shipwright_result_text(int result) {
    static const char *const TEXT[SHIPWRIGHT_RESULTS] = {
        "DONE", "SHE IS FITTED SO ALREADY", "NOT ENOUGH DOUBLOONS", "THAT HULL IS NOT YOURS",
        "YOU OWN THAT HULL ALREADY", "NONE OF THOSE IN STORE", "YOUR STORE IS FULL", "THAT DOES NOT GO THERE"};
    return result >= 0 && result < SHIPWRIGHT_RESULTS ? TEXT[result] : "?";
}

void shipwright_order_write(cw_writer *w, const shipwright_order *o) {
    cw_writer_u8(w, o->op);
    cw_writer_u8(w, o->hull);
    cw_writer_u8(w, o->slot);
    cw_writer_u8(w, o->component);
}

bool shipwright_order_read(cw_reader *r, shipwright_order *o) {
    memset(o, 0, sizeof(*o));
    cw_reader_u8(r, &o->op);
    cw_reader_u8(r, &o->hull);
    cw_reader_u8(r, &o->slot);
    cw_reader_u8(r, &o->component);
    return cw_reader_ok(r) && o->op < SHIPWRIGHT_OPS;
}
