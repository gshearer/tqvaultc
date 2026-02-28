#ifndef CHARACTER_H
#define CHARACTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "vault.h"

/* Grid dimensions for character inventory panels */
#define CHAR_INV_COLS 12
#define CHAR_INV_ROWS  5
#define CHAR_BAG_COLS  8
#define CHAR_BAG_ROWS  5

typedef struct {
    char *base_name;
    uint32_t seed;
    char *prefix_name;
    char *suffix_name;
    char *relic_name;
    char *relic_bonus;
    char *relic_name2;
    char *relic_bonus2;
    uint32_t var1;   /* relic/charm slot 1 shard count */
    uint32_t var2;   /* relic/charm slot 2 shard count */
} TQItem;

/**
 * TQCharacter - Represents a Titan Quest player character (.chr)
 */
typedef struct {
    char *filepath;
    uint8_t *raw_data;
    size_t data_size;

    // Extracted statistics
    char *character_name;
    char *class_name;
    uint32_t level;
    uint32_t experience;
    uint32_t kills;
    uint32_t deaths;

    float strength;
    float dexterity;
    float intelligence;
    float health;
    float mana;

    char *mastery1;
    char *mastery2;

    TQItem *equipment[12]; // Head, Neck, Chest, Legs, Arms, Ring1, Ring2, Wep1, Shld1, Wep2, Shld2, Artifact

    /* Inventory: sacks[0] = main 12×5, sacks[1..3] = extra bags 8×5 */
    TQVaultSack inv_sacks[4];
    int num_inv_sacks;

    /* Splice boundary offsets for binary save */
    size_t inv_block_start;    /* offset of "numberOfSacks" key length prefix */
    size_t inv_block_end;      /* offset after final inventory end_block value */
    size_t equip_block_start;  /* offset after useAlternate's value */
    size_t equip_block_end;    /* offset after final equipment end_block value */
    bool has_atlantis;         /* true if relicName2 fields were present */
} TQCharacter;

TQCharacter* character_load(const char *filepath);
void character_free(TQCharacter *character);
int character_save(TQCharacter *character, const char *filepath);

#endif
