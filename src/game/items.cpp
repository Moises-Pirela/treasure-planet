#include "items.h"
#include <stdio.h>
#include <string.h>

bool item_exists(const item *it) { return it->id != 0; }

static uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

item item_roll(uint32_t seed, int level) {
    item it = {};
    uint32_t r = mix(seed);
    it.id = seed ? seed : 1;
    it.seed = seed;
    it.slot = (uint8_t)(r % SLOT_COUNT);
    // Rarity: half common, then a quarter, an eighth, a sixteenth, and the rest legendary.
    uint32_t roll = mix(r) % 1000;
    it.rarity = roll < 500 ? RARITY_COMMON : roll < 750 ? RARITY_UNCOMMON : roll < 875 ? RARITY_RARE : roll < 970 ? RARITY_EPIC : RARITY_LEGENDARY;
    it.power = (uint16_t)(10 * (level < 1 ? 1 : level) + 5 * it.rarity + (int)(mix(r + 1) % 10));
    return it;
}

static const char *const ADJECTIVES[RARITY_COUNT][4] = {
    {"Rusty", "Plain", "Worn", "Patched"},
    {"Sturdy", "Polished", "Brass", "Keen"},
    {"Gilded", "Starlit", "Etheric", "Captain's"},
    {"Nebular", "Solar", "Voidglass", "Admiral's"},
    {"Treasure", "Flint's", "Aurelian", "Ancient"},
};
static const char *const NOUNS[SLOT_COUNT][4] = {
    {"Cap", "Hat", "Helm", "Goggles"},
    {"Coat", "Jacket", "Vest", "Cuirass"},
    {"Boots", "Greaves", "Leggings", "Striders"},
    {"Cutlass", "Pistol", "Sabre", "Harpoon"},
    {"Compass", "Charm", "Locket", "Spyglass"},
};
static const char *const SUFFIXES[8] = {"", "", " of the Tide", " of Etherium", " of the Deep", " of Montressor", " of Orbits", " of the Gale"};

void item_name(const item *it, char *out, size_t capacity) {
    if (!item_exists(it)) {
        snprintf(out, capacity, "-");
        return;
    }
    uint32_t r = mix(it->seed ^ 0x9e3779b9u);
    int rarity = it->rarity < RARITY_COUNT ? it->rarity : 0, slot = it->slot < SLOT_COUNT ? it->slot : 0;
    snprintf(out, capacity, "%s %s%s", ADJECTIVES[rarity][r % 4], NOUNS[slot][(r >> 4) % 4],
             it->rarity >= RARITY_RARE ? SUFFIXES[(r >> 8) % 8] : "");
}

const char *item_slot_name(int slot) {
    static const char *const NAMES[SLOT_COUNT] = {"head", "chest", "legs", "weapon", "trinket"};
    return slot >= 0 && slot < SLOT_COUNT ? NAMES[slot] : "?";
}
const char *item_rarity_name(int rarity) {
    static const char *const NAMES[RARITY_COUNT] = {"common", "uncommon", "rare", "epic", "legendary"};
    return rarity >= 0 && rarity < RARITY_COUNT ? NAMES[rarity] : "?";
}
void item_rarity_color(int rarity, float rgb[3]) {
    static const float COLORS[RARITY_COUNT][3] = {
        {0.6f, 0.6f, 0.6f}, {0.3f, 0.85f, 0.3f}, {0.3f, 0.5f, 1.0f}, {0.7f, 0.3f, 0.95f}, {1.0f, 0.6f, 0.1f},
    };
    const float *c = COLORS[rarity >= 0 && rarity < RARITY_COUNT ? rarity : 0];
    rgb[0] = c[0];
    rgb[1] = c[1];
    rgb[2] = c[2];
}

int character_stash_count(const character *c) {
    int n = 0;
    for (int i = 0; i < STASH_SIZE; ++i) n += item_exists(&c->stash[i]);
    return n;
}

static bool stash_put(character *c, const item *it) {
    for (int i = 0; i < STASH_SIZE; ++i)
        if (!item_exists(&c->stash[i])) {
            c->stash[i] = *it;
            return true;
        }
    return false;
}

bool character_take(character *c, const item *it) {
    if (!item_exists(it) || it->slot >= SLOT_COUNT) return false;
    item *worn = &c->equipment[it->slot];
    if (!item_exists(worn)) {
        *worn = *it;
    } else if (it->power > worn->power) {
        if (!stash_put(c, worn)) return false;
        *worn = *it;
    } else if (!stash_put(c, it)) {
        return false;
    }
    ++c->version;
    return true;
}

bool character_equip(character *c, int stash_index) {
    if (stash_index < 0 || stash_index >= STASH_SIZE || !item_exists(&c->stash[stash_index])) return false;
    item it = c->stash[stash_index];
    item *worn = &c->equipment[it.slot];
    c->stash[stash_index] = *worn;   // the old piece (or nothing) takes its place
    *worn = it;
    ++c->version;
    return true;
}
