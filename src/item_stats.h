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

// Per-attribute requirement-reduction percentages (0-100) that apply to a
// specific item for the active character.  Used to annotate the tooltip's
// requirement lines with the reduced value.  All-zero means no reduction.
typedef struct {
  int level;
  int strength;
  int dexterity;
  int intelligence;
} ItemReqReduction;

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

// As above, but annotates requirement lines with reduced values when the given
// requirement reduction is non-zero.  Pass NULL for `reduction` to disable the
// annotation (equivalent to the non-_ex variants).
void
item_format_stats_ex(TQItem *item, TQTranslation *tr,
                     const ItemReqReduction *reduction, char *buffer, size_t size);
void
vault_item_format_stats_ex(TQVaultItem *item, TQTranslation *tr,
                          const ItemReqReduction *reduction, char *buffer, size_t size);

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

// True for "single-piece" relics/charms (completedRelicLevel <= 1), e.g. the
// Atlantis quest_artifice charm and the Ragnarok nerthusmistletoe relic.  These
// are quest rewards awarded already complete: they never exist as shards, so
// they do not stack, never take a quantity, and never roll a completion bonus.
// Safe to call on any path -- non single-piece relics/charms return false.
bool
relic_is_single_piece(const char *relic_path);

// Returns a malloc'd short stat summary string from a bonus DBR
// (e.g. "+30% Attack Speed"). Caller must free().
// record_path: DBR path to the bonus record.
// tr: optional translation table (may be NULL) for mastery name resolution.
// Returns: summary string, or NULL if no stats found.
char *
item_bonus_stat_summary(const char *record_path, TQTranslation *tr);

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

// Strip Pango/HTML markup tags from a string, producing plain text (decoding
// the common XML entities).  dst_size must be at least strlen(src)+1.  GTK-free,
// so it's shared by the GUI search and the headless Database Browser blob-gen.
void
strip_pango_markup(char *dst, size_t dst_size, const char *src);

// ── Shared search-query matcher ────────────────────────────────────────────
// One GTK-free matcher reused by the main-window global search, the Database
// Browser search box, and the headless self-tests.  The mode is auto-detected
// from the raw query text:
//   - REGEX:   if it carries an alternation/grouping/class metacharacter
//              (| ( ) [ ]) it compiles to a case-insensitive GRegex matched
//              unanchored (substring semantics), so e.g.
//              "vitality damage|elemental damage" matches either phrase.
//   - LITERAL: if that GRegex fails to compile (an incomplete pattern typed so
//              far, e.g. "(vita") it degrades to a case-insensitive substring of
//              the raw text, so the UI never breaks mid-type.
//   - TOKENS:  otherwise the query is split on whitespace and EVERY token must
//              appear (order-independent AND) -- the original fast behavior, so
//              plain typing and chars like + . % stay literal.
//   - EMPTY:   blank / whitespace-only query (matches everything; callers treat
//              it as "no search active").
// All match haystacks are expected to be already lowercased.
typedef struct SearchQuery SearchQuery;

typedef enum {
  SEARCH_QUERY_EMPTY,
  SEARCH_QUERY_TOKENS,
  SEARCH_QUERY_REGEX,
  SEARCH_QUERY_LITERAL,
} SearchQueryMode;

// Compile a raw (UTF-8) query string.  Never returns NULL.  Free with
// search_query_free().
SearchQuery *
search_query_compile(const char *raw);

// True if the query is blank (no tokens / whitespace only) -> "no search".
bool
search_query_is_empty(const SearchQuery *q);

// Match the query against an already-lowercased haystack.  An empty query (or
// NULL) matches everything; a NULL haystack never matches a non-empty query.
bool
search_query_match(const SearchQuery *q, const char *haystack_lc);

// The detected mode, and a short human-readable name (for the self-tests).
SearchQueryMode
search_query_mode(const SearchQuery *q);
const char *
search_query_mode_name(const SearchQuery *q);

void
search_query_free(SearchQuery *q);

// Append all stat lines from a single DBR record to a BufWriter.
void
add_stats_from_record(const char *record_path, TQTranslation *tr, BufWriter *w,
                      const char *color, int shard_index);

// Case-insensitive substring search within a path.
bool
path_contains_ci(const char *path, const char *needle);

// Compute the level/dex/int/str requirements for a single DBR record.
// out is filled with [level, dexterity, intelligence, strength]; entries that
// are not set on the record are left as 0.  Reads static requirement fields and
// falls back to itemCost level-scaling equations for gear.
void
item_record_requirements(const char *record_path, int out[4]);

// Compute the aggregate requirements of a full item across its base, prefix,
// suffix and socketed relic records, keeping the maximum per requirement type.
// NULL/empty paths are skipped.  out: [level, dexterity, intelligence, strength].
void
item_requirements(const char *base_name, const char *prefix_name,
                  const char *suffix_name, const char *relic_name,
                  const char *relic_name2, int out[4]);

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
