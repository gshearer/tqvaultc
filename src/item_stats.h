#ifndef ITEM_STATS_H
#define ITEM_STATS_H

#include "character.h"
#include "vault.h"
#include "translation.h"
#include "arz.h"
#include <stdarg.h>

// Initialize the item stats subsystem (call once at startup).
void
item_stats_init(void);

// Free all item stats resources.
void
item_stats_free(void);

// Format stats for a character equipment item into buffer.
// item: the equipment item.
// tr: translation table for display names.
// buffer: output buffer.
// size: buffer capacity.
void
item_format_stats(TQItem *item, TQTranslation *tr, char *buffer, size_t size);

// Format stats for a vault item into buffer.
// item: the vault item.
// tr: translation table for display names.
// buffer: output buffer.
// size: buffer capacity.
void
vault_item_format_stats(TQVaultItem *item, TQTranslation *tr, char *buffer, size_t size);

// Returns total resistance (summed across all item components) for the given
// DBR attribute name, e.g. attr_name = "defensiveFire".
// item: the equipment item.
// attr_name: DBR attribute name to query.
// Returns: total resistance value.
float
item_get_resistance(TQItem *item, const char *attr_name);

// Like item_get_resistance but excludes stats that have an associated
// Chance < 100%.
// item: the equipment item.
// attr_name: DBR attribute name to query.
// Returns: guaranteed stat value.
float
item_get_guaranteed_stat(TQItem *item, const char *attr_name);

// Like item_get_guaranteed_stat but with an explicit chance attribute name
// (for attrs where the chance variable doesn't follow the {attr}Chance convention).
// item: the equipment item.
// attr_name: DBR attribute name to query.
// chance_attr: explicit chance attribute name.
// Returns: guaranteed stat value.
float
item_get_guaranteed_stat_ex(TQItem *item, const char *attr_name, const char *chance_attr);

// Returns total guaranteed mean damage summed across all item components.
// For each component, if max > min, uses (min + max) / 2.
// item: the equipment item.
// min_attr: DBR attribute for minimum damage.
// max_attr: DBR attribute for maximum damage.
// chance_attr: DBR attribute for chance (NULL if always guaranteed).
// Returns: total guaranteed mean damage.
float
item_get_guaranteed_damage_mean(TQItem *item, const char *min_attr,
                                const char *max_attr, const char *chance_attr);

// Returns total DOT damage (min * duration) summed across all item components,
// only for guaranteed effects (chance == 0 or chance >= 100).
// item: the equipment item.
// min_attr: DBR attribute for minimum damage.
// dur_attr: DBR attribute for duration.
// chance_attr: DBR attribute for chance.
// Returns: total guaranteed DOT damage.
float
item_get_guaranteed_dot(TQItem *item, const char *min_attr, const char *dur_attr, const char *chance_attr);

// Returns the number of shards needed to complete a relic/charm.
// relic_path: DBR path to the relic/charm record.
// Returns: max shard count, or 0 if unknown.
int
relic_max_shards(const char *relic_path);

// Returns a malloc'd short stat summary string from a bonus DBR
// (e.g. "+30% Attack Speed"). Caller must free().
// record_path: DBR path to the bonus record.
// Returns: summary string, or NULL if no stats found.
char *
item_bonus_stat_summary(const char *record_path);

// BufWriter: O(1) append instead of O(N) strlen

typedef struct {
  char *buf;
  size_t size;
  size_t pos;
} BufWriter;

// Initialize a BufWriter with the given buffer and size.
void
buf_init(BufWriter *w, char *buffer, size_t size);

// Append formatted text to a BufWriter.
void
buf_write(BufWriter *w, const char *fmt, ...);

// Internal helpers shared between item_stats.c and item_stats_format.c

// Fast variable lookup using interned name + shard index.
float
dbr_get_float_fast(TQArzRecordData *data, const char *interned_name, int si);

// Get string variable from a pre-fetched record using interned name.
const char*
record_get_string_fast(TQArzRecordData *data, const char *interned_name);

// Get string variable by loading a record path first.
const char*
get_record_variable_string(const char *record_path, const char *interned_name);

// Determine the display color for an item based on classification and path.
const char*
get_item_color(const char *base_name, const char *prefix_name, const char *suffix_name);

// Derive a human-readable name from a DBR path.
char*
pretty_name_from_path(const char *path);

// Escape Pango markup special characters in a string.
char*
escape_markup(const char *str);

// Append all stat lines from a single DBR record to a BufWriter.
void
add_stats_from_record(const char *record_path, TQTranslation *tr, BufWriter *w,
                      const char *color, int shard_index);

// Case-insensitive substring search within a path.
bool
path_contains_ci(const char *path, const char *needle);

// Pre-interned variable name pointers (shared across item_stats modules)
extern const char *INT_itemNameTag, *INT_description, *INT_lootRandomizerName, *INT_FileDescription;
extern const char *INT_itemClassification, *INT_itemText;
extern const char *INT_characterBaseAttackSpeedTag, *INT_artifactClassification;
extern const char *INT_itemSkillName, *INT_buffSkillName, *INT_skillDisplayName;
extern const char *INT_itemSkillAutoController, *INT_triggerType, *INT_itemSkillLevel;
extern const char *INT_skillBaseDescription, *INT_petSkillName, *INT_skillChanceWeight;
extern const char *INT_itemSetName, *INT_setName, *INT_setMembers;
extern const char *INT_completedRelicLevel;
extern const char *INT_dexterityRequirement, *INT_intelligenceRequirement;
extern const char *INT_strengthRequirement, *INT_levelRequirement;
extern const char *INT_itemLevel, *INT_itemCostName, *INT_Class;

#endif
