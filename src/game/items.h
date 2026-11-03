// Items: what a player wears and carries. Plain data, deterministic names, nothing
// that needs a database to mean something: an item is five numbers.
//
// A character is one player's equipment (one item per slot) and stash. Characters
// belong to the backend (social.h keeps and saves them); a zone server holds a copy
// for each player it hosts, changes it when they pick something up, and sends the
// change back. What a player wears is replicated on their entity, so everyone sees
// everyone's gear.
#ifndef TP_ITEMS_H
#define TP_ITEMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum item_slot { SLOT_HEAD, SLOT_CHEST, SLOT_LEGS, SLOT_WEAPON, SLOT_TRINKET, SLOT_COUNT };
enum item_rarity { RARITY_COMMON, RARITY_UNCOMMON, RARITY_RARE, RARITY_EPIC, RARITY_LEGENDARY, RARITY_COUNT };

struct item {
    uint32_t id;                 // 0: no item; otherwise unique to this drop
    uint8_t slot;                // item_slot
    uint8_t rarity;              // item_rarity
    uint16_t power;              // what "better" means
    uint32_t seed;               // the look and the name
};

#define STASH_SIZE 24
struct character {
    item equipment[SLOT_COUNT];
    item stash[STASH_SIZE];
    uint32_t version;            // bumped on every change, so copies can tell
};

bool item_exists(const item *it);
// A drop: rolled from a seed, slot and rarity chosen by it, power by rarity and level.
item item_roll(uint32_t seed, int level);
// "Gilded Coat of the Tide", into `out`.
void item_name(const item *it, char *out, size_t capacity);
const char *item_slot_name(int slot);
const char *item_rarity_name(int rarity);
// Rarity colours for drawing gear on a player (linear RGB).
void item_rarity_color(int rarity, float rgb[3]);

// Takes an item: worn if the slot is empty or it is better (the old piece goes to
// the stash), else stashed. False when there is no room anywhere.
bool character_take(character *c, const item *it);
bool character_equip(character *c, int stash_index);   // swap a stash item into its slot
int character_stash_count(const character *c);

#endif
