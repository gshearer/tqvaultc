#ifndef ITEM_STATS_H
#define ITEM_STATS_H

#include "character.h"
#include "vault.h"
#include "translation.h"

void item_stats_init(void);
void item_stats_free(void);

void item_format_stats(TQItem *item, TQTranslation *tr, char *buffer, size_t size);
void vault_item_format_stats(TQVaultItem *item, TQTranslation *tr, char *buffer, size_t size);

// Returns total resistance (summed across all item components) for the given DBR attribute name.
// e.g. attr_name = "defensiveFire"
float item_get_resistance(TQItem *item, const char *attr_name);

// Returns the number of shards needed to complete a relic/charm.
int relic_max_shards(const char *relic_path);

// Returns a malloc'd short stat summary string from a bonus DBR (e.g. "+30% Attack Speed").
// Caller must free(). Returns NULL if no stats found.
char* item_bonus_stat_summary(const char *record_path);

#endif
