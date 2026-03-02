#ifndef AFFIX_TABLE_H
#define AFFIX_TABLE_H

#include "translation.h"
#include <stdbool.h>

typedef struct {
    char *affix_path;       /* DBR path to individual affix record */
    char *translation;      /* Resolved display name */
    float weight;           /* Drop weight from randomizer table */
} TQAffixEntry;

typedef struct {
    TQAffixEntry *entries;
    int count;
} TQAffixList;

typedef struct TQItemAffixes_tag {
    TQAffixList prefixes;
    TQAffixList suffixes;
} TQItemAffixes;

/* Build the global item->affix-table map by scanning all loot item table records.
 * Call once after asset_manager_init(). */
void affix_table_init(TQTranslation *tr);

/* Get valid prefixes and suffixes for the given item base_name.
 * Returns NULL if item has no affix tables (e.g. Epic/Legendary/non-equipment).
 * Caller must call affix_result_free() when done. */
TQItemAffixes* affix_table_get(const char *item_base_name, TQTranslation *tr);

/* Check if an item can have its affixes modified */
bool item_can_modify_affixes(const char *base_name);

/* Check if an Epic/Legendary item can have forge affixes applied */
bool item_can_forge_affixes(const char *base_name);

/* Get forge-specific affixes for an Epic/Legendary item from the Dvergr Master Forge tables.
 * Returns NULL if item is ineligible or no forge tables found.
 * Caller must call affix_result_free() when done. */
TQItemAffixes* affix_table_get_forge(const char *item_base_name, TQTranslation *tr);

void affix_result_free(TQItemAffixes *affixes);
void affix_table_free(void);

#endif
