// Database Browser: categorization + category/index builders.
// Split from ui_db_browser.c; shared types/decls in ui_db_browser_internal.h.

#include "ui_db_browser_internal.h"

#include "config.h"
#include "item_stats.h"
#include "asset_lookup.h"
#include "ui_skills_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Map a GEAR_* flag to its leaf category, or -1 if the flag isn't a single
// browsable gear type.
static int
gear_flag_to_cat(uint32_t flag)
{
  switch(flag)
  {
    case GEAR_SWORD:  return(CAT_WPN_SWORD);
    case GEAR_AXE:    return(CAT_WPN_AXE);
    case GEAR_MACE:   return(CAT_WPN_MACE);
    case GEAR_SPEAR:  return(CAT_WPN_SPEAR);
    case GEAR_BOW:    return(CAT_WPN_BOW);
    case GEAR_STAFF:  return(CAT_WPN_STAFF);
    case GEAR_THROWN: return(CAT_WPN_THROWN);
    case GEAR_HEAD:   return(CAT_ARM_HEAD);
    case GEAR_TORSO:  return(CAT_ARM_TORSO);
    case GEAR_ARM:    return(CAT_ARM_ARM);
    case GEAR_LEG:    return(CAT_ARM_LEG);
    case GEAR_SHIELD: return(CAT_ARM_SHIELD);
    case GEAR_RING:   return(CAT_JEW_RING);
    case GEAR_AMULET: return(CAT_JEW_AMULET);
    default:          return(-1);
  }
}

// Decide which browse category a record belongs to.
//
// Mirrors tqdb's filtering: skip old/default templates; gear must be a known
// equipment class carrying a Magical/Rare/Epic/Legendary itemClassification
// (common/broken omitted); relics, charms, artifacts and scrolls are always
// included.
//
// path: full DBR record path
// returns: a DbCat value, or -1 if the record is not a browsable item
static int
db_categorize(const char *path)
{
  if(!path)
    return(-1);

  // Drop the parser's excluded template trees.
  if(path_contains_ci(path, "\\old\\") || path_contains_ci(path, "\\default\\"))
    return(-1);

  // Equipment: classify by gear type, then require a real rarity.
  uint32_t gear = item_gear_type(path);

  if(gear != 0)
  {
    int cat = gear_flag_to_cat(gear);

    if(cat < 0)
      return(-1);

    const char *cls = dbr_get_string(path, "itemClassification");

    if(!cls)
      return(-1);

    if(strcasecmp(cls, "Magical")   != 0 &&
       strcasecmp(cls, "Rare")      != 0 &&
       strcasecmp(cls, "Epic")      != 0 &&
       strcasecmp(cls, "Legendary") != 0)
      return(-1);

    return(cat);
  }

  // Non-gear item bases: relics, charms, artifacts, scrolls.
  const char *cls = dbr_get_string(path, "Class");

  if(!cls)
    return(-1);

  if(strcasecmp(cls, "ItemRelic") == 0)
    return(CAT_RELIC);
  if(strcasecmp(cls, "ItemCharm") == 0)
    return(CAT_CHARM);
  if(strcasecmp(cls, "ItemArtifact") == 0)
    return(CAT_ARTIFACT);
  if(strcasecmp(cls, "OneShot_Scroll") == 0)
    return(CAT_SCROLL);

  return(-1);
}

// -- Display name resolution ------------------------------------------------

// Resolve a human-readable name for an item record.  Tries itemNameTag, then
// the description tag (relics/charms/scrolls/artifacts), translating via the
// loaded table; falls back to a path-derived name.  Returns a malloc'd string
// the caller owns.
char *
db_item_display_name(DbBrowserState *st, const char *path)
{
  const char *tag = dbr_get_string(path, "itemNameTag");

  if(!tag || !tag[0])
    tag = dbr_get_string(path, "description");

  if(tag && tag[0])
  {
    const char *name = translation_get(st->tr, tag);

    // translation_get returns the tag itself when no translation exists.
    if(name && name[0] && strcmp(name, tag) != 0)
      return(g_strdup(name));
  }

  // Last resort: derive from the path (returns malloc'd).
  char *pretty = pretty_name_from_path(path);

  return(pretty ? pretty : g_strdup(path));
}

// -- Build the category index ----------------------------------------------

// Single pass over every record path: categorize, and for matches build a
// DbBrowseItem (name, color, shard count) into the right per-category store.
void
build_category_index(DbBrowserState *st)
{
  TQArzFile *arz = st->arz;

  if(!arz)
    return;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    const char *path = arz->records[i].path;
    int cat = db_categorize(path);

    if(cat < 0)
      continue;

    DbBrowseItem *it = g_object_new(DB_TYPE_BROWSE_ITEM, NULL);

    it->path  = g_strdup(path);
    it->name  = db_item_display_name(st, path);
    it->color = get_item_color(path, NULL, NULL);  // static string; do not free
    it->var1  = 0;

    // Relics/charms render their completed texture at full shard count.
    if(item_is_relic_or_charm(path))
    {
      int max_shards = relic_max_shards(path);

      if(max_shards > 0)
        it->var1 = (uint32_t)max_shards;
    }

    // Lowercased copy for search.
    it->name_lc = g_ascii_strdown(it->name, -1);

    g_list_store_append(st->cat_stores[cat], it);
    g_object_unref(it);
    st->cat_counts[cat]++;
  }
}

// -- Sets -------------------------------------------------------------------

// Compare a '\'-delimited path segment (case-insensitively) to a literal.
static bool
db_seg_eq(const char *seg, const char *lit)
{
  size_t len = strlen(lit);

  return(strncasecmp(seg, lit, len) == 0 && (seg[len] == '\\' || seg[len] == '\0'));
}

// True if a path segment starts (case-insensitively) with the given prefix.
static bool
db_seg_prefix(const char *seg, const char *pfx)
{
  return(strncasecmp(seg, pfx, strlen(pfx)) == 0);
}

// Decide whether a record path is an item-set DBR, matching tqdb's SETS globs
// (resources.py): records\item\sets\*.dbr and records\xpack*\item*\set*\*.dbr.
// Directory depth is enforced exactly, so dev/sandbox set trees are excluded.
static bool
db_is_set_path(const char *path)
{
  size_t n = strlen(path);

  if(n < 5 || strcasecmp(path + n - 4, ".dbr") != 0)
    return(false);

  const char *p0 = path;
  const char *p1 = strchr(p0, '\\');

  if(!db_seg_eq(p0, "records") || !p1)
    return(false);
  p1++;

  const char *p2 = strchr(p1, '\\');

  if(!p2)
    return(false);
  p2++;

  const char *p3 = strchr(p2, '\\');

  if(!p3)
    return(false);
  p3++;

  const char *p3end = strchr(p3, '\\');

  if(!p3end)
    // Exactly four segments -> records\item\sets\<file>.dbr
    return(db_seg_eq(p1, "item") && db_seg_eq(p2, "sets"));

  // Five+ segments: the file must sit directly under the set* directory.
  if(strchr(p3end + 1, '\\'))
    return(false);

  return(db_seg_prefix(p1, "xpack") && db_seg_prefix(p2, "item") &&
         db_seg_prefix(p3, "set"));
}

// Resolve a set member's translated display name, or NULL if the member path
// doesn't resolve to a real, named item (placeholders, bare directories and
// untranslated/template records are rejected -- mirrors tqdb's member filter).
// Returns an internal pointer (translation table); do not free.
const char *
db_set_member_name(DbBrowserState *st, const char *member_path)
{
  if(!member_path || !member_path[0] || strcmp(member_path, "#") == 0)
    return(NULL);

  size_t n = strlen(member_path);

  if(n < 4 || strcasecmp(member_path + n - 4, ".dbr") != 0)
    return(NULL);

  TQArzRecordData *md = asset_get_dbr(member_path);

  if(!md)
    return(NULL);

  const char *tag = record_get_string_fast(md, INT_description);

  if(!tag || !tag[0])
    tag = record_get_string_fast(md, INT_itemNameTag);

  if(!tag || !tag[0])
    return(NULL);

  const char *name = translation_get(st->tr, tag);

  // translation_get echoes the tag when no translation exists.
  if(!name || !name[0] || strcmp(name, tag) == 0)
    return(NULL);

  return(name);
}

// Single pass over every record path: find item-set DBRs, validate the set has
// a translatable name and at least one real member, and build one DbBrowseItem
// per set into the Sets store.  The icon is drawn from the first valid member
// (set DBRs have no bitmap of their own).
void
build_set_index(DbBrowserState *st)
{
  TQArzFile *arz = st->arz;

  if(!arz)
    return;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    const char *path = arz->records[i].path;

    if(!path || !db_is_set_path(path))
      continue;

    // Set name must exist and be translatable.
    const char *set_tag = dbr_get_string(path, "setName");

    if(!set_tag || !set_tag[0])
      continue;

    const char *set_name = translation_get(st->tr, set_tag);

    if(!set_name || !set_name[0] || strcmp(set_name, set_tag) == 0)
      continue;

    // Need at least one real member; the first one supplies the icon.
    TQArzRecordData *set_data = asset_get_dbr(path);
    TQVariable *members = set_data ? arz_record_get_var(set_data, INT_setMembers)
                                   : NULL;
    const char *icon_member = NULL;

    if(members && members->type == TQ_VAR_STRING)
      for(uint32_t m = 0; m < members->count && !icon_member; m++)
        if(db_set_member_name(st, members->value.str[m]))
          icon_member = members->value.str[m];

    if(!icon_member)
      continue;  // template / dangling set with no displayable members

    DbBrowseItem *it = g_object_new(DB_TYPE_BROWSE_ITEM, NULL);

    it->path      = g_strdup(path);
    it->name      = g_strdup(set_name);
    it->name_lc   = g_ascii_strdown(it->name, -1);
    it->icon_path = g_strdup(icon_member);
    it->color     = "#40FF40";  // set green, matching the in-item tooltip
    it->var1      = 0;
    it->is_set    = true;

    g_list_store_append(st->cat_stores[CAT_SET], it);
    g_object_unref(it);
    st->cat_counts[CAT_SET]++;
  }
}

// -- Affixes ----------------------------------------------------------------

// Aggregates one logical affix while the index is being built.  tqdb keys
// affixes by their translation tag and merges same-tag records (e.g. the three
// "Allfather's" pet-damage rolls share tag x2tagPrefix03), so an affix can
// span several DBR records ("variants") and the equipment types of every table
// that references any of them.
typedef struct {
  int kind;            // 0 == prefix, 1 == suffix
  GHashTable *types;   // set<char*>: equipment-type display labels (keys owned)
  GPtrArray *variants; // char* DBR paths sharing this tag (deduped, owned)
} DbAffixAgg;

static void
db_affix_agg_free(gpointer data)
{
  DbAffixAgg *a = data;

  if(!a)
    return;

  if(a->types)
    g_hash_table_destroy(a->types);
  if(a->variants)
    g_ptr_array_free(a->variants, TRUE);
  g_free(a);
}

// Normalize a DBR path to lowercase with backslash separators (for hash keys).
// Returns a newly-allocated string (free with g_free).
static char *
db_norm_path(const char *path)
{
  char *norm = g_ascii_strdown(path, -1);

  for(char *p = norm; *p; p++)
    if(*p == '/')
      *p = '\\';

  return(norm);
}

// Map an affix-table filename prefix (the part before the first '_') to a
// human-readable equipment-type label.  Mirrors tqdb's get_affix_table_type()
// (utils/core.py): match the first known equipment prefix, longest forms first.
// The "mage"/"melee" split distinguishes caster (Intelligence) gear from
// fighter (Strength/Dexterity) gear.  Returns a static string, or NULL for an
// unrecognized prefix (e.g. monster-infrequent affix tables).
static const char *
db_affix_gear_label(const char *file_prefix)
{
  static const struct { const char *pfx; const char *label; } MAP[] = {
    { "armmage",    "Arm Armor (Caster)" },
    { "armsmage",   "Arm Armor (Caster)" },
    { "armmelee",   "Arm Armor (Fighter)" },
    { "armsmelee",  "Arm Armor (Fighter)" },
    { "headmage",   "Head Armor (Caster)" },
    { "headmelee",  "Head Armor (Fighter)" },
    { "legmage",    "Leg Armor (Caster)" },
    { "legsmage",   "Leg Armor (Caster)" },
    { "legmelee",   "Leg Armor (Fighter)" },
    { "legsmelee",  "Leg Armor (Fighter)" },
    { "torsomage",  "Torso Armor (Caster)" },
    { "torsomelee", "Torso Armor (Fighter)" },
    { "amulet",     "Amulet" },
    { "ring",       "Ring" },
    { "shield",     "Shield" },
    { "axe",        "Axe" },
    { "bow",        "Bow" },
    { "club",       "Mace" },
    { "spear",      "Spear" },
    { "staff",      "Staff" },
    { "sword",      "Sword" },
    { "roh",        "Throwing Weapon" },
  };

  for(size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
    if(strncasecmp(file_prefix, MAP[i].pfx, strlen(MAP[i].pfx)) == 0)
      return(MAP[i].label);

  return(NULL);
}

// If lower_path (lowercased, backslash-separated) is an affix randomizer table
// matching tqdb's AFFIX_TABLES globs --
//   records\item*\lootmagicalaffixes\<prefix|suffix>\tables*\<file>.dbr
//   records\xpack*\item*\lootmagicalaffixes\<prefix|suffix>\tables*\<file>.dbr
// -- write a g_strdup'd equipment-type label to *out_label (caller frees) and
// return true; otherwise return false.  Unknown filename prefixes fall back to
// the capitalized raw prefix (mirrors tqdb keeping the bare file prefix).
static bool
db_affix_table_label(const char *lower_path, char **out_label)
{
  *out_label = NULL;

  if(strncmp(lower_path, "records\\", 8) != 0)
    return(false);

  const char *marker = strstr(lower_path, "\\lootmagicalaffixes\\");

  if(!marker)
    return(false);

  // Tail after the marker: <prefix|suffix> \ tables* \ <file>.dbr
  const char *seg_a = marker + strlen("\\lootmagicalaffixes\\");
  const char *s1 = strchr(seg_a, '\\');

  if(!s1)
    return(false);

  size_t alen = (size_t)(s1 - seg_a);

  if(!((alen == 6 && strncmp(seg_a, "prefix", 6) == 0) ||
       (alen == 6 && strncmp(seg_a, "suffix", 6) == 0)))
    return(false);

  const char *seg_b = s1 + 1;
  const char *s2 = strchr(seg_b, '\\');

  if(!s2 || strncmp(seg_b, "tables", 6) != 0)
    return(false);

  const char *file = s2 + 1;

  if(strchr(file, '\\'))
    return(false);  // must be exactly one file segment under tables*

  // File token = everything before the first '_' or '.'.
  char token[64];
  size_t t = 0;

  for(const char *p = file; *p && *p != '_' && *p != '.' && t < sizeof(token) - 1; p++)
    token[t++] = *p;
  token[t] = '\0';

  const char *label = db_affix_gear_label(token);

  if(label)
    *out_label = g_strdup(label);
  else
  {
    // Unknown prefix: capitalize the raw token as a fallback label.
    char *cap = g_strdup(token);

    if(cap[0])
      cap[0] = g_ascii_toupper(cap[0]);
    *out_label = cap;
  }

  return(true);
}

// Resolve an affix's display name: its lootRandomizerName translation tag,
// then the raw FileDescription, then a path-derived fallback.  Caller frees.
static char *
db_affix_display_name(DbBrowserState *st, const char *path)
{
  const char *tag = dbr_get_string(path, "lootRandomizerName");

  if(tag && tag[0])
  {
    const char *name = translation_get(st->tr, tag);

    if(name && name[0] && strcmp(name, tag) != 0)
      return(g_strdup(name));
  }

  const char *fd = dbr_get_string(path, "FileDescription");

  if(fd && fd[0])
    return(g_strdup(fd));

  char *pretty = pretty_name_from_path(path);

  return(pretty ? pretty : g_strdup(path));
}

// qsort/g_ptr_array_sort helper: order DbBrowseItem* by display name (CI).
static int
db_affix_item_cmp(gconstpointer a, gconstpointer b)
{
  const DbBrowseItem *x = *(DbBrowseItem * const *)a;
  const DbBrowseItem *y = *(DbBrowseItem * const *)b;

  return(g_ascii_strcasecmp(x->name ? x->name : "", y->name ? y->name : ""));
}

// -- Display sort ----------------------------------------------------------

// Difficulty-tier rank from an item DBR filename: legendary=3, epic=2,
// normal=1, untiered=0.  Titan Quest encodes the tier as a "_n_/_e_/_l_"
// infix in the basename (mi_l_wraith, u_e_alexander, us_n_..., ...) -- the same
// convention that makes the three versions of a Monster Infrequent, and an
// epic/legendary unique pair, share one display name.  Used only to break ties
// between equally-named items, so a stray match on an untiered item is benign.
int
db_item_tier_rank(const char *path)
{
  if(!path)
    return(0);

  // Isolate the basename (record paths use '\\', but stay tolerant of '/').
  const char *base = path;

  for(const char *p = path; *p; p++)
    if(*p == '\\' || *p == '/')
      base = p + 1;

  // First "_<t>_" infix with t in {n,e,l} wins.
  for(const char *u = strchr(base, '_'); u && u[1]; u = strchr(u + 1, '_'))
  {
    if(u[2] == '_')
    {
      switch(g_ascii_tolower((unsigned char)u[1]))
      {
        case 'l': return(3);
        case 'e': return(2);
        case 'n': return(1);
      }
    }
  }

  return(0);
}

// g_list_store_sort comparator: alphabetical by display name (CI), then -- for
// same-named difficulty variants -- legendary->epic->normal, then by path so
// the order is stable and deterministic.
static int
db_browse_item_cmp(gconstpointer a, gconstpointer b, gpointer user_data)
{
  (void)user_data;

  const DbBrowseItem *x = (const DbBrowseItem *)a;
  const DbBrowseItem *y = (const DbBrowseItem *)b;

  int c = g_ascii_strcasecmp(x->name ? x->name : "", y->name ? y->name : "");

  if(c != 0)
    return(c);

  int tx = db_item_tier_rank(x->path);
  int ty = db_item_tier_rank(y->path);

  if(tx != ty)
    return(ty - tx);  // higher tier first

  return(g_ascii_strcasecmp(x->path ? x->path : "", y->path ? y->path : ""));
}

void
db_browser_sort_stores(DbBrowserState *st)
{
  for(int cat = 0; cat < CAT_COUNT; cat++)
  {
    // Skill categories keep their mastery-first, in-game tree order.
    if(cat >= CAT_SKILL_DEFENSE && cat <= CAT_SKILL_NEIDAN)
      continue;

    if(st->cat_stores[cat])
      g_list_store_sort(st->cat_stores[cat], db_browse_item_cmp, NULL);
  }
}

// qsort helper: order char* by strcmp (for sorting equipment-type labels).
static int
db_strptr_cmp(const void *a, const void *b)
{
  return(strcmp(*(const char * const *)a, *(const char * const *)b));
}

// Join an equipment-type label set into one sorted ", "-separated string.
// Returns a newly-allocated string (possibly empty); caller frees with g_free.
static char *
db_join_sorted_types(GHashTable *types)
{
  guint n = g_hash_table_size(types);

  if(n == 0)
    return(g_strdup(""));

  const char **arr = g_new(const char *, n);
  GHashTableIter it;
  gpointer k, v;
  guint i = 0;

  g_hash_table_iter_init(&it, types);
  while(g_hash_table_iter_next(&it, &k, &v))
    arr[i++] = k;

  qsort(arr, n, sizeof(char *), db_strptr_cmp);

  GString *s = g_string_new(NULL);

  for(guint j = 0; j < n; j++)
  {
    if(j)
      g_string_append(s, ", ");
    g_string_append(s, arr[j]);
  }

  g_free(arr);
  return(g_string_free(s, FALSE));
}

// Append a DBR path to an affix's variant list, deduping case-insensitively.
static void
db_affix_add_variant(GPtrArray *variants, const char *path)
{
  for(guint i = 0; i < variants->len; i++)
    if(strcasecmp(g_ptr_array_index(variants, i), path) == 0)
      return;

  g_ptr_array_add(variants, g_strdup(path));
}

// Build the Prefixes/Suffixes index.  Ports tqdb's parse_affixes (main.py):
// walk every affix randomizer table, and for each `randomizerName*` affix it
// references, record which equipment type that table is for.  Affixes are
// keyed by their translation tag (lootRandomizerName) and merged the way tqdb
// does -- same-tag records become one entry whose properties are the union of
// its variant rolls (e.g. the three "Allfather's" pet-damage records) and
// whose gear is the union of every referencing table's type.  Prefix vs suffix
// comes from the affix record's own path (\prefix\ / \suffix\).  Properties are
// rendered live at detail time via add_stats_from_record.
void
build_affix_index(DbBrowserState *st)
{
  TQArzFile *arz = st->arz;

  if(!arz)
    return;

  // "P|<tag>" / "S|<tag>" (tag-less records keyed by "<kind>|@<path>") -> agg
  GHashTable *affixes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, db_affix_agg_free);

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    const char *path = arz->records[i].path;

    if(!path)
      continue;

    char *lower = db_norm_path(path);
    char *label = NULL;
    bool is_table = db_affix_table_label(lower, &label);

    g_free(lower);

    if(!is_table)
      continue;

    TQArzRecordData *table = asset_get_dbr(path);

    if(!table)
    {
      g_free(label);
      continue;
    }

    for(uint32_t v = 0; v < table->num_vars; v++)
    {
      TQVariable *var = &table->vars[v];

      if(!var->name ||
         strncasecmp(var->name, "randomizerName", 14) != 0 ||
         var->type != TQ_VAR_STRING || var->count == 0 ||
         !var->value.str || !var->value.str[0] || !var->value.str[0][0])
        continue;

      const char *affix_path = var->value.str[0];
      size_t plen = strlen(affix_path);

      // Skip malformed randomizer entries (a few tables carry a numeric
      // placeholder instead of an affix path); tqdb likewise requires the
      // referenced affix DBR to exist.
      if(plen < 4 || strcasecmp(affix_path + plen - 4, ".dbr") != 0)
        continue;

      // Classify prefix vs suffix by the record's own path.
      int kind;

      if(path_contains_ci(affix_path, "\\suffix\\"))
        kind = 1;
      else if(path_contains_ci(affix_path, "\\prefix\\"))
        kind = 0;
      else
        continue;  // not a standard prefix/suffix record

      // Merge key: the translation tag (so tiers/rolls of one named affix
      // collapse), falling back to the path for tag-less records.
      const char *tag = dbr_get_string(affix_path, "lootRandomizerName");
      char *key;

      if(tag && tag[0])
        key = g_strdup_printf("%c|%s", kind ? 'S' : 'P', tag);
      else
      {
        char *np = db_norm_path(affix_path);

        key = g_strdup_printf("%c|@%s", kind ? 'S' : 'P', np);
        g_free(np);
      }

      DbAffixAgg *agg = g_hash_table_lookup(affixes, key);

      if(!agg)
      {
        agg = g_new0(DbAffixAgg, 1);
        agg->kind = kind;
        agg->types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        agg->variants = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(affixes, key, agg);  // takes ownership of key
      }
      else
        g_free(key);

      if(!g_hash_table_contains(agg->types, label))
        g_hash_table_add(agg->types, g_strdup(label));

      db_affix_add_variant(agg->variants, affix_path);
    }

    g_free(label);
  }

  // Build one item per merged affix, bucketed and sorted by name.
  GPtrArray *buckets[2];  // [0] = prefixes, [1] = suffixes

  buckets[0] = g_ptr_array_new();
  buckets[1] = g_ptr_array_new();

  GHashTableIter it;
  gpointer k, val;

  g_hash_table_iter_init(&it, affixes);
  while(g_hash_table_iter_next(&it, &k, &val))
  {
    DbAffixAgg *agg = val;

    if(agg->variants->len == 0)
      continue;

    const char *first = g_ptr_array_index(agg->variants, 0);

    DbBrowseItem *item = g_object_new(DB_TYPE_BROWSE_ITEM, NULL);

    item->path        = g_strdup(first);
    item->name        = db_affix_display_name(st, first);
    item->name_lc     = g_ascii_strdown(item->name, -1);
    item->color       = get_item_color(first, NULL, NULL);
    item->is_affix    = true;
    item->equip_types = db_join_sorted_types(agg->types);

    // Snapshot the variant DBR paths as a GStrv for the detail pane.
    guint vn = agg->variants->len;

    item->variants = g_new0(char *, vn + 1);
    for(guint x = 0; x < vn; x++)
      item->variants[x] = g_strdup(g_ptr_array_index(agg->variants, x));

    g_ptr_array_add(buckets[agg->kind], item);
  }

  int cats[2] = { CAT_PREFIX, CAT_SUFFIX };

  for(int b = 0; b < 2; b++)
  {
    g_ptr_array_sort(buckets[b], db_affix_item_cmp);

    for(guint j = 0; j < buckets[b]->len; j++)
    {
      DbBrowseItem *item = g_ptr_array_index(buckets[b], j);

      g_list_store_append(st->cat_stores[cats[b]], item);
      g_object_unref(item);  // store holds the reference now
    }

    st->cat_counts[cats[b]] = (int)buckets[b]->len;
    g_ptr_array_free(buckets[b], TRUE);
  }

  g_hash_table_destroy(affixes);
}

// -- Skills -----------------------------------------------------------------

// The 11 base masteries, in sidebar order (matching CAT_SKILL_DEFENSE..NEIDAN).
// Each mastery's skill tree DBR enumerates that mastery's skills via skillName1
// ..N -- exactly the list the visual skill manager reads (ui_skills_tree.c).
// Kept local (mirroring that file's mastery_defs[]) for the same reason the
// set/affix logic is duplicated: this view renders straight from the DB.
static const struct {
  const char *mastery_dbr;
  const char *tree_dbr;
} DB_MASTERY[] = {
  { "records\\skills\\defensive\\defensivemastery.dbr",
    "records\\skills\\defensive\\defensiveskilltree.dbr" },
  { "records\\skills\\earth\\earthmastery.dbr",
    "records\\skills\\earth\\earthskilltree.dbr" },
  { "records\\skills\\hunting\\huntingmastery.dbr",
    "records\\skills\\hunting\\huntingskilltree.dbr" },
  { "records\\skills\\nature\\naturemastery.dbr",
    "records\\skills\\nature\\natureskilltree.dbr" },
  { "records\\skills\\spirit\\spiritmastery.dbr",
    "records\\skills\\spirit\\spiritskilltree.dbr" },
  { "records\\skills\\storm\\stormmastery.dbr",
    "records\\skills\\storm\\stormskilltree.dbr" },
  { "records\\skills\\warfare\\warfaremastery.dbr",
    "records\\skills\\warfare\\warfareskilltree.dbr" },
  { "records\\xpack\\skills\\dream\\dreammastery.dbr",
    "records\\xpack\\skills\\dream\\dreamskilltree.dbr" },
  { "records\\xpack2\\skills\\runemaster\\runemaster_mastery.dbr",
    "records\\xpack2\\skills\\runemaster\\runemaster_skilltree.dbr" },
  { "records\\skills\\stealth\\stealthmastery.dbr",
    "records\\skills\\stealth\\stealthskilltree.dbr" },
  { "records\\xpack4\\skills\\neidan\\neidanmastery.dbr",
    "records\\xpack4\\skills\\neidan\\neidanskilltree.dbr" },
};
#define DB_NUM_MASTERY 11

// Resolve the record that actually carries a skill's description + leveled
// stats: usually the skill itself, but buff/toggle/pet skills keep them one or
// more refs deep (e.g. a pet modifier -> its pet skill -> that skill's buff).
// Generalizes resolve_effective_skill_path() (ui_skills_tree.c) to follow the
// buff/pet chain recursively.  Returns a session-stable cached record path, or
// NULL if no description-bearing record is found.
const char *
db_skill_effective_path(const char *path, int depth)
{
  if(!path || !path[0] || depth > 4)
    return(NULL);

  const char *desc = dbr_get_string(path, "skillBaseDescription");

  if(desc && desc[0])
    return(path);

  static const char *refs[] = { "buffSkillName", "petSkillName" };

  for(int r = 0; r < 2; r++)
  {
    const char *ref = dbr_get_string(path, refs[r]);

    if(ref && ref[0])
    {
      const char *found = db_skill_effective_path(ref, depth + 1);

      if(found)
        return(found);
    }
  }

  return(NULL);
}

// Resolve a skill's max allocatable level, following the pet/buff ref chain
// when the record itself lacks skillMaxLevel.  Mirrors resolve_skill_levels().
int
db_skill_max_level(const char *path, int depth)
{
  if(!path || !path[0] || depth > 4)
    return(0);

  TQArzRecordData *d = asset_get_dbr(path);

  if(!d)
    return(0);

  TQVariable *mv = arz_record_get_var(d, arz_intern("skillMaxLevel"));

  if(mv && mv->count > 0)
    return(arz_record_get_int(d, "skillMaxLevel", 1, NULL));

  for(int r = 0; r < 2; r++)
  {
    const char *rp = record_get_string_fast(d, r == 0 ? INT_petSkillName
                                                      : INT_buffSkillName);

    if(rp && rp[0])
    {
      int got = db_skill_max_level(rp, depth + 1);

      if(got > 0)
        return(got);
    }
  }

  return(0);
}

// Resolve a skill's translated display name, recursing through the buff/pet ref
// chain when the record carries no skillDisplayName of its own (generalizes
// resolve_skill_name()).  Returns g_strdup'd on success, NULL if nothing along
// the chain names the skill.
static char *
db_skill_name_rec(DbBrowserState *st, const char *path, int depth)
{
  if(!path || !path[0] || depth > 4)
    return(NULL);

  const char *tag = dbr_get_string(path, "skillDisplayName");

  if(tag && tag[0])
  {
    const char *name = translation_get(st->tr, tag);

    // translation_get echoes the tag when no translation exists.
    if(name && name[0] && strcmp(name, tag) != 0)
      return(g_strdup(name));

    // A literal (non-"tag") display name with no translation: use it raw.
    if(strncmp(tag, "tag", 3) != 0)
      return(g_strdup(tag));
  }

  static const char *refs[] = { "buffSkillName", "petSkillName" };

  for(int r = 0; r < 2; r++)
  {
    const char *ref = dbr_get_string(path, refs[r]);

    if(ref && ref[0])
    {
      char *n = db_skill_name_rec(st, ref, depth + 1);

      if(n)
        return(n);
    }
  }

  return(NULL);
}

// Display name for a skill, with a path-derived fallback.  Caller frees (g_free).
char *
db_skill_display_name(DbBrowserState *st, const char *path)
{
  char *n = db_skill_name_rec(st, path, 0);

  if(n)
    return(n);

  char *pretty = pretty_name_from_path(path);

  return(pretty ? pretty : g_strdup(path));
}

// Resolve a skill's up-icon .tex path (appending .tex when extensionless),
// recursing through the buff/pet ref chain when the record carries no bitmap of
// its own (generalizes resolve_skill_bitmaps(), up icon only).  Writes "" if
// nothing along the chain has an icon.
static void
db_skill_bitmap(const char *path, char *out, size_t outsz, int depth)
{
  out[0] = '\0';

  if(!path || !path[0] || depth > 4)
    return;

  const char *bmp = dbr_get_string(path, "skillUpBitmapName");

  if(bmp && bmp[0])
  {
    if(strrchr(bmp, '.'))
      snprintf(out, outsz, "%s", bmp);
    else
      snprintf(out, outsz, "%s.tex", bmp);

    return;
  }

  static const char *refs[] = { "buffSkillName", "petSkillName" };

  for(int r = 0; r < 2 && !out[0]; r++)
  {
    const char *ref = dbr_get_string(path, refs[r]);

    if(ref && ref[0])
      db_skill_bitmap(ref, out, outsz, depth + 1);
  }
}

// Build one DbBrowseItem for a skill (or, when is_mastery, the mastery node)
// into the given Skills category store.  The icon is a .tex bitmap loaded via
// texture_load (skills don't carry an item bitmap), so it's flagged is_skill.
static void
db_add_skill_item(DbBrowserState *st, int cat, const char *skill_path,
                  bool is_mastery)
{
  DbBrowseItem *it = g_object_new(DB_TYPE_BROWSE_ITEM, NULL);

  it->path     = g_strdup(skill_path);
  it->name     = db_skill_display_name(st, skill_path);
  it->color    = "#DAA520";  // skill gold, matching the granted-skill tooltip
  it->is_skill = true;

  // Disambiguate the mastery node from the like-named sidebar leaf.
  if(is_mastery && it->name && !strcasestr(it->name, "mastery"))
  {
    char *m = g_strdup_printf("%s Mastery", it->name);

    g_free(it->name);
    it->name = m;
  }

  it->name_lc = g_ascii_strdown(it->name, -1);

  char tex[256];

  db_skill_bitmap(skill_path, tex, sizeof(tex), 0);
  it->icon_path = tex[0] ? g_strdup(tex) : NULL;

  g_list_store_append(st->cat_stores[cat], it);
  g_object_unref(it);
  st->cat_counts[cat]++;
}

// Build the per-mastery Skills index.  Mirrors build_mastery_model()
// (ui_skills_tree.c) so the browser shows exactly the skills the in-game tree
// does: add the mastery node (its passive bonuses), then walk its skill tree
// (skillName1..N), skipping the mastery record itself.  When the mastery has an
// in-game skill-window button (the normal case for all 11 masteries), keep only
// skills that likewise have a button -- this drops auto-applied helpers such as
// the deep pet-modifier chains that aren't shown as allocatable nodes in-game.
// If a mastery has no button data, fall back to icon-bearing skills deduped by
// display name (a passive and its self-buff collapse to one).
void
build_skill_index(DbBrowserState *st)
{
  for(int m = 0; m < DB_NUM_MASTERY; m++)
  {
    int cat = CAT_SKILL_DEFENSE + m;
    const char *mastery_dbr = DB_MASTERY[m].mastery_dbr;

    // The mastery node first (labeled "<Name> Mastery").
    db_add_skill_item(st, cat, mastery_dbr, true);

    TQArzRecordData *tree = asset_get_dbr(DB_MASTERY[m].tree_dbr);

    if(!tree)
      continue;

    SkillIconPos pos;
    bool use_db = skill_layout_lookup(mastery_dbr, &pos);

    char seen[48][128];
    int num_seen = 0;

    for(int n = 1; n <= 32; n++)
    {
      char field[32];

      snprintf(field, sizeof(field), "skillName%d", n);

      const char *sp = record_get_string_fast(tree, arz_intern(field));

      if(!sp || !sp[0])
        continue;  // gap in the skillName sequence

      if(strcasestr(sp, "mastery"))
        continue;  // the mastery record (added above)

      if(use_db)
      {
        // In-game layout: show exactly the skills with a skill-window button.
        if(!skill_layout_lookup(sp, &pos))
          continue;
      }
      else
      {
        // Fallback: need a resolvable icon, deduped by display name.
        char tex[256];

        db_skill_bitmap(sp, tex, sizeof(tex), 0);
        if(!tex[0])
          continue;

        char *dname = db_skill_display_name(st, sp);
        bool dup = false;

        for(int s = 0; s < num_seen; s++)
          if(strcasecmp(seen[s], dname) == 0)
          {
            dup = true;
            break;
          }

        if(!dup && num_seen < 48)
          snprintf(seen[num_seen++], sizeof(seen[0]), "%s", dname);

        g_free(dname);

        if(dup)
          continue;
      }

      db_add_skill_item(st, cat, sp, false);
    }
  }
}

// -- Creatures + Quests (Phase 6) -------------------------------------------

// Display color for a creature row, by classification.
const char *
db_creature_color(const char *classification)
{
  if(classification && g_ascii_strcasecmp(classification, "Boss") == 0)
    return("#FF7050");
  if(classification && g_ascii_strcasecmp(classification, "Hero") == 0)
    return("#FFC850");
  return("#70C8FF");  // Quest
}

// Resolve a creature's display name: translated description tag, else its
// FileDescription, else a path-derived name.  Caller frees.
char *
db_creature_display_name(DbBrowserState *st, DbCreature *c)
{
  if(c->name_tag && c->name_tag[0])
  {
    const char *name = translation_get(st->tr, c->name_tag);

    if(name && name[0] && strcmp(name, c->name_tag) != 0)
      return(g_strdup(name));
  }

  const char *fd = dbr_get_string(c->path, "FileDescription");

  if(fd && fd[0])
    return(g_strdup(fd));

  char *pretty = pretty_name_from_path(c->path);

  return(pretty ? pretty : g_strdup(c->path));
}

// Build the creature index and populate the Monsters grid (one row per
// boss/hero/quest creature that drops indexable loot).
void
build_creature_index_grid(DbBrowserState *st)
{
  st->creatures = db_creature_index_build(st->arz);

  if(!st->creatures)
    return;

  for(guint i = 0; i < st->creatures->creatures->len; i++)
  {
    DbCreature *c = g_ptr_array_index(st->creatures->creatures, i);

    if(!c->has_drops)
      continue;

    DbBrowseItem *it = g_object_new(DB_TYPE_BROWSE_ITEM, NULL);

    it->is_creature = true;
    it->src_idx     = (int)i;
    it->path        = g_strdup(c->path);
    it->name        = db_creature_display_name(st, c);
    it->color       = db_creature_color(c->classification);
    it->name_lc     = g_ascii_strdown(it->name, -1);

    g_list_store_append(st->cat_stores[CAT_CREATURE], it);
    g_object_unref(it);
    st->cat_counts[CAT_CREATURE]++;
  }
}

// Build the quest index and populate the Quests grid (one row per quest that
// grants item rewards).
void
build_quest_index_grid(DbBrowserState *st)
{
  if(!global_config.game_folder)
    return;

  // Candidate Quests.arc archives under <game>/Resources.
  static const char *REL[] = {
    "Quests.arc", "xpack/Quests.arc", "XPack2/Quests.arc",
    "XPack3/Quests.arc", "XPack4/Quests.arc",
  };
  GPtrArray *arcs = g_ptr_array_new_with_free_func(g_free);

  for(size_t i = 0; i < G_N_ELEMENTS(REL); i++)
  {
    char *p = g_build_filename(global_config.game_folder, "Resources", REL[i], NULL);

    if(g_file_test(p, G_FILE_TEST_IS_REGULAR))
      g_ptr_array_add(arcs, p);
    else
      g_free(p);
  }

  st->quests = db_quest_index_build(st->arz,
      (const char *const *)arcs->pdata, (int)arcs->len);
  g_ptr_array_free(arcs, TRUE);

  if(!st->quests)
    return;

  for(guint i = 0; i < st->quests->quests->len; i++)
  {
    DbQuest *q = g_ptr_array_index(st->quests->quests, i);

    const char *name = translation_get(st->tr, q->title_tag);

    DbBrowseItem *it = g_object_new(DB_TYPE_BROWSE_ITEM, NULL);

    it->is_quest = true;
    it->src_idx  = (int)i;
    it->path     = g_strdup(q->title_tag);  // identity; not a droppable item
    it->name     = g_strdup((name && name[0]) ? name : q->title_tag);
    it->color    = "#DAA520";  // quest gold
    it->name_lc  = g_ascii_strdown(it->name, -1);

    g_list_store_append(st->cat_stores[CAT_QUEST], it);
    g_object_unref(it);
    st->cat_counts[CAT_QUEST]++;
  }
}
