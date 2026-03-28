#ifndef AFFIX_TABLE_H
#define AFFIX_TABLE_H

#include "translation.h"
#include <stdbool.h>

typedef struct {
    char *affix_path;       // DBR path to individual affix record
    char *translation;      // Resolved display name
    float weight;           // Drop weight from randomizer table
    char *effect_family;    // Family key: filename with trailing _NN stripped
    int tier;               // Numeric tier from trailing _NN (0 if none)
    char *stat_summary;     // Human-readable stat summary (e.g. "+33 Strength")
    char *stat_category;    // Stat type names only (e.g. "Strength", "Pets: Elemental Damage")
    char *stat_values;      // Compact numeric values (e.g. "+33", "60 / +12%")
} TQAffixEntry;

typedef struct {
    TQAffixEntry *entries;
    int count;
} TQAffixList;

typedef struct TQItemAffixes_tag {
    TQAffixList prefixes;
    TQAffixList suffixes;
} TQItemAffixes;

// Build the global item->affix-table map by scanning all loot item table records.
// Call once after asset_manager_init().
// tr: translation table for resolving affix display names.
void
affix_table_init(TQTranslation *tr);

// Get valid prefixes and suffixes for the given item base_name.
// item_base_name: DBR path of the item base record.
// tr: translation table for resolving affix display names.
// Returns: allocated TQItemAffixes, or NULL if item has no affix tables.
//          Caller must call affix_result_free() when done.
TQItemAffixes *
affix_table_get(const char *item_base_name, TQTranslation *tr);

// Check if an item can have its affixes modified.
// base_name: DBR path of the item base record.
// Returns: true if the item supports affix modification.
bool
item_can_modify_affixes(const char *base_name);

// Check if an Epic/Legendary item can have forge affixes applied.
// base_name: DBR path of the item base record.
// Returns: true if forge affixes can be applied.
bool
item_can_forge_affixes(const char *base_name);

// Get forge-specific affixes for an Epic/Legendary item from the Dvergr
// Master Forge tables.
// item_base_name: DBR path of the item base record.
// tr: translation table for resolving affix display names.
// Returns: allocated TQItemAffixes, or NULL if ineligible.
//          Caller must call affix_result_free() when done.
TQItemAffixes *
affix_table_get_forge(const char *item_base_name, TQTranslation *tr);

// Free an affix result returned by affix_table_get or affix_table_get_forge.
// affixes: the result to free (NULL-safe).
void
affix_result_free(TQItemAffixes *affixes);

// Free the global affix table (call at shutdown).
void
affix_table_free(void);

#endif
