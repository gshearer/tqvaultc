// ui_db_browser.c -- tq-db-style Database Browser
//
// A curated, categorized item browser modeled on tq-db.net.  Unlike the raw
// Database Explorer (ui_database_dialog.c, a path-tree + variable inspector),
// this presents the game's items grouped by type, each rendered with its icon,
// rarity-colored name and full in-game-style tooltip -- and lets the user
// right-click an item to pick it up and drop it into a vault.
//
// Browsable groups: droppable item bases (weapons, armor, jewelry, relics,
// charms, artifacts, scrolls), item Sets, and Affixes (Prefixes/Suffixes).
// Skills / creature-loot / quest-reward views are planned later (see TODO.md).
//
// The categorization model mirrors tqdb's parser (reference/tqdb): include only
// item records, drop old/default templates, and keep gear only when it carries
// a Magical/Rare/Epic/Legendary classification (common/broken omitted, matching
// the website).  All formatting reuses the existing C engine: item_stats.c for
// tooltips, get_item_color for rarity, load_item_texture for icons.

#include "ui.h"
#include "arz.h"
#include "config.h"
#include "item_stats.h"
#include "asset_lookup.h"
#include "ui_skills_layout.h"
#include "texture.h"
#include "vault.h"
#include "db_creatures.h"
#include "db_quests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// -- Category model ---------------------------------------------------------

// Leaf categories, in sidebar display order.  Each has a parent group label
// and a leaf label.  db_categorize() maps a record path to one of these.
typedef enum {
  CAT_WPN_SWORD = 0, CAT_WPN_AXE, CAT_WPN_MACE, CAT_WPN_SPEAR,
  CAT_WPN_BOW, CAT_WPN_STAFF, CAT_WPN_THROWN,
  CAT_ARM_HEAD, CAT_ARM_TORSO, CAT_ARM_ARM, CAT_ARM_LEG, CAT_ARM_SHIELD,
  CAT_JEW_RING, CAT_JEW_AMULET,
  CAT_RELIC, CAT_CHARM, CAT_ARTIFACT, CAT_SCROLL,
  CAT_SET,
  CAT_PREFIX, CAT_SUFFIX,
  // One leaf per mastery, in the same order as DB_MASTERY[] below.
  CAT_SKILL_DEFENSE, CAT_SKILL_EARTH, CAT_SKILL_HUNTING, CAT_SKILL_NATURE,
  CAT_SKILL_SPIRIT, CAT_SKILL_STORM, CAT_SKILL_WARFARE, CAT_SKILL_DREAM,
  CAT_SKILL_RUNE, CAT_SKILL_ROGUE, CAT_SKILL_NEIDAN,
  CAT_CREATURE,
  CAT_QUEST,
  CAT_COUNT
} DbCat;

static const struct {
  const char *group;  // parent header (consecutive equal groups share a header)
  const char *leaf;   // selectable row label
} CAT_INFO[CAT_COUNT] = {
  { "Weapons", "Sword" },   { "Weapons", "Axe" },    { "Weapons", "Mace" },
  { "Weapons", "Spear" },   { "Weapons", "Bow" },    { "Weapons", "Staff" },
  { "Weapons", "Throwing" },
  { "Armor", "Head" },      { "Armor", "Torso" },    { "Armor", "Arm" },
  { "Armor", "Leg" },       { "Armor", "Shield" },
  { "Jewelry", "Ring" },    { "Jewelry", "Amulet" },
  { "Relics", "Relics" },   { "Charms", "Charms" },
  { "Artifacts", "Artifacts" }, { "Scrolls", "Scrolls" },
  { "Sets", "Sets" },
  { "Affixes", "Prefixes" }, { "Affixes", "Suffixes" },
  { "Skills", "Defense" },  { "Skills", "Earth" },   { "Skills", "Hunting" },
  { "Skills", "Nature" },   { "Skills", "Spirit" },  { "Skills", "Storm" },
  { "Skills", "Warfare" },  { "Skills", "Dream" },   { "Skills", "Rune" },
  { "Skills", "Rogue" },    { "Skills", "Neidan" },
  { "Monsters", "Bosses & Heroes" },
  { "Quests", "Quests" },
};

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

// -- DbBrowseItem: GObject wrapping one item shown in the center grid -------

#define DB_TYPE_BROWSE_ITEM (db_browse_item_get_type())
G_DECLARE_FINAL_TYPE(DbBrowseItem, db_browse_item, DB, BROWSE_ITEM, GObject)

struct _DbBrowseItem {
  GObject parent_instance;
  char *path;        // full DBR path (also the item base_name)
  char *name;        // resolved display name (UTF-8)
  char *name_lc;     // lowercased name, for substring search
  char *icon_path;   // DBR to draw the icon from (NULL == use path); sets use
                     // their first member, which has no bitmap of their own
  const char *color; // Pango foreground color (static string from get_item_color)
  uint32_t var1;     // relic/charm shard count (full == completed texture)
  bool is_set;       // true == an item set (detail shows members + bonus tiers)
  bool is_affix;     // true == a prefix/suffix affix (detail shows props + gear)
  bool is_skill;     // true == a mastery/skill (icon is a .tex; detail = levels)
  bool is_creature;  // true == a boss/hero (detail shows its drop list)
  bool is_quest;     // true == a quest (detail shows its item rewards)
  int  src_idx;      // creature/quest index into the matching index array
  char *equip_types; // affix: comma-joined equipment-type labels it can roll on
  char **variants;   // affix: NULL-terminated DBR paths sharing this affix's tag
                     // (one logical affix may have several stat rolls)
};

G_DEFINE_FINAL_TYPE(DbBrowseItem, db_browse_item, G_TYPE_OBJECT)

static void
db_browse_item_finalize(GObject *object)
{
  DbBrowseItem *self = DB_BROWSE_ITEM(object);

  g_free(self->path);
  g_free(self->name);
  g_free(self->name_lc);
  g_free(self->icon_path);
  g_free(self->equip_types);
  g_strfreev(self->variants);
  G_OBJECT_CLASS(db_browse_item_parent_class)->finalize(object);
}

static void
db_browse_item_class_init(DbBrowseItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = db_browse_item_finalize;
}

static void
db_browse_item_init(DbBrowseItem *self)
{
  (void)self;
}

// -- Dialog state -----------------------------------------------------------

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  TQArzFile *arz;  // used only to enumerate record paths
  bool owns_arz;   // true only if we arz_load'd our own copy (fallback path)

  GListStore *cat_stores[CAT_COUNT];  // GListStore<DbBrowseItem> per category
  int cat_counts[CAT_COUNT];

  DbCreatureIndex *creatures;  // boss/hero loot index ("dropped by")
  DbQuestIndex    *quests;     // quest item-reward index ("reward from")

  GtkWidget *grid_view;
  GtkFilterListModel *filter_model;   // sits over the active category store
  GtkCustomFilter *custom_filter;     // name substring filter
  GtkSingleSelection *selection;
  char search_lc[256];                // lowercased needle

  GtkWidget *detail_pic;              // GtkPicture: large icon
  GtkWidget *detail_label;            // GtkLabel: full formatted tooltip
  GtkWidget *count_label;             // "<n> items"

  // Dialog-local held-item overlay (the main window can't render the
  // cursor-attached item while the cursor is over this separate window).
  GtkWidget *dlg_held_overlay;
  double dlg_cursor_x, dlg_cursor_y;
} DbBrowserState;

static void
db_browser_state_free(gpointer data)
{
  DbBrowserState *st = data;

  for(int i = 0; i < CAT_COUNT; i++)
    g_clear_object(&st->cat_stores[i]);

  g_clear_object(&st->custom_filter);

  if(st->creatures)
    db_creature_index_free(st->creatures);
  if(st->quests)
    db_quest_index_free(st->quests);

  // Only free the database handle if we loaded our own copy; normally we reuse
  // the shared handle owned by the asset cache.
  if(st->arz && st->owns_arz)
    arz_free(st->arz);

  g_free(st);
}

// -- Display name resolution ------------------------------------------------

// Resolve a human-readable name for an item record.  Tries itemNameTag, then
// the description tag (relics/charms/scrolls/artifacts), translating via the
// loaded table; falls back to a path-derived name.  Returns a malloc'd string
// the caller owns.
static char *
db_item_display_name(DbBrowserState *st, const char *path)
{
  const char *tag = dbr_get_string(path, "itemNameTag");

  if(!tag || !tag[0])
    tag = dbr_get_string(path, "description");

  if(tag && tag[0])
  {
    const char *name = translation_get(st->widgets->translations, tag);

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
static void
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
static const char *
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

  const char *name = translation_get(st->widgets->translations, tag);

  // translation_get echoes the tag when no translation exists.
  if(!name || !name[0] || strcmp(name, tag) == 0)
    return(NULL);

  return(name);
}

// Single pass over every record path: find item-set DBRs, validate the set has
// a translatable name and at least one real member, and build one DbBrowseItem
// per set into the Sets store.  The icon is drawn from the first valid member
// (set DBRs have no bitmap of their own).
static void
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

    const char *set_name = translation_get(st->widgets->translations, set_tag);

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
    const char *name = translation_get(st->widgets->translations, tag);

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
static void
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
static const char *
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
static int
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
    const char *name = translation_get(st->widgets->translations, tag);

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
static char *
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
static void
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
static const char *
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
static char *
db_creature_display_name(DbBrowserState *st, DbCreature *c)
{
  if(c->name_tag && c->name_tag[0])
  {
    const char *name = translation_get(st->widgets->translations, c->name_tag);

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
static void
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
static void
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

    const char *name = translation_get(st->widgets->translations, q->title_tag);

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

// -- Held-item overlay (mirrors ui_database_dialog.c) -----------------------

static void
dlg_held_overlay_draw_cb(GtkDrawingArea *da, cairo_t *cr,
                         int w, int h, gpointer ud)
{
  (void)da;
  (void)w;
  (void)h;

  DbBrowserState *st = ud;
  AppWidgets *widgets = st->widgets;

  if(!widgets->held_item)
    return;

  HeldItem *hi = widgets->held_item;

  if(!hi->texture)
    return;

  double cell = compute_cell_size(widgets);

  if(cell <= 0.0)
    cell = 32.0;

  int pw = gdk_pixbuf_get_width(hi->texture);
  int ph = gdk_pixbuf_get_height(hi->texture);
  double rw = (double)hi->item_w * cell;
  double rh = (double)hi->item_h * cell;
  double ix = st->dlg_cursor_x - rw / 2.0;
  double iy = st->dlg_cursor_y - rh / 2.0;

  cairo_save(cr);
  cairo_translate(cr, ix, iy);
  cairo_scale(cr, rw / (double)pw, rh / (double)ph);
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gdk_cairo_set_source_pixbuf(cr, hi->texture, 0, 0);
  G_GNUC_END_IGNORE_DEPRECATIONS
  cairo_paint_with_alpha(cr, 0.7);
  cairo_restore(cr);
}

static void
on_dlg_overlay_motion(GtkEventControllerMotion *ctrl,
                      double x, double y, gpointer data)
{
  (void)ctrl;
  DbBrowserState *st = data;

  st->dlg_cursor_x = x;
  st->dlg_cursor_y = y;
  if(st->widgets->held_item && st->dlg_held_overlay)
    gtk_widget_queue_draw(st->dlg_held_overlay);
}

static void
on_dlg_overlay_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
  (void)ctrl;
  DbBrowserState *st = data;

  st->dlg_cursor_x = -10000.0;
  st->dlg_cursor_y = -10000.0;
  if(st->widgets->held_item && st->dlg_held_overlay)
    gtk_widget_queue_draw(st->dlg_held_overlay);
}

// Attach a fresh copy of the given item record to the cursor for drop into an
// inventory.  Mirrors db_record_pickup() in ui_database_dialog.c.
static void
db_browse_pickup(DbBrowserState *st, DbBrowseItem *bi)
{
  AppWidgets *widgets = st->widgets;

  if(!bi || !bi->path || bi->is_set || bi->is_affix || bi->is_skill ||
     bi->is_creature || bi->is_quest)
    return;  // a set / affix / skill / creature / quest isn't a droppable item

  if(widgets->held_item)
    cancel_held_item(widgets);

  HeldItem *hi = calloc(1, sizeof(HeldItem));

  if(!hi)
    return;

  hi->item.seed       = (uint32_t)(rand() % 0x7fff);
  hi->item.base_name  = safe_strdup(bi->path);
  hi->item.var1       = bi->var1;
  hi->item.stack_size = 1;

  hi->source            = CONTAINER_VAULT;
  hi->source_sack_idx   = -1;
  hi->source_equip_slot = -1;
  hi->is_copy = true;

  get_item_dims(widgets, &hi->item, &hi->item_w, &hi->item_h);
  hi->texture = load_item_texture(widgets, hi->item.base_name, hi->item.var1);

  widgets->held_item = hi;

  // Park the main-window cursor render off-screen until the cursor crosses
  // into it; this dialog's own overlay handles rendering meanwhile.
  widgets->win_cursor_x = -10000.0;
  widgets->win_cursor_y = -10000.0;

  invalidate_tooltips(widgets);
  queue_redraw_all(widgets);
  if(st->dlg_held_overlay)
    gtk_widget_queue_draw(st->dlg_held_overlay);
}

// Right-click on the grid: locate the bound DbBrowseItem (stashed on the cell
// box during bind) and pick it up.
static void
on_grid_right_click(GtkGestureClick *gesture, int n_press,
                    double x, double y, gpointer data)
{
  (void)n_press;

  DbBrowserState *st = data;
  GtkWidget *picked = gtk_widget_pick(st->grid_view, x, y, GTK_PICK_DEFAULT);
  DbBrowseItem *bi = NULL;

  while(picked)
  {
    bi = g_object_get_data(G_OBJECT(picked), "dbitem");
    if(bi)
      break;
    picked = gtk_widget_get_parent(picked);
  }

  if(bi)
  {
    db_browse_pickup(st, bi);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  }
}

// -- Detail pane ------------------------------------------------------------

// Show the given pixbuf in the large detail-pane picture (NULL clears it).
// Borrows pb -- the caller retains ownership.
static void
db_detail_set_pixbuf(DbBrowserState *st, GdkPixbuf *pb)
{
  if(pb)
  {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    G_GNUC_END_IGNORE_DEPRECATIONS

    gtk_picture_set_paintable(GTK_PICTURE(st->detail_pic), GDK_PAINTABLE(tex));
    g_object_unref(tex);
  }
  else
  {
    gtk_picture_set_paintable(GTK_PICTURE(st->detail_pic), NULL);
  }
}

// Set the large detail-pane icon from a DBR path (NULL or no texture clears it).
static void
db_detail_set_icon(DbBrowserState *st, const char *path, uint32_t var1)
{
  GdkPixbuf *pb = path ? load_item_texture(st->widgets, path, var1) : NULL;

  db_detail_set_pixbuf(st, pb);
  if(pb)
    g_object_unref(pb);
}

// True for the int routing flags (offensive*Global, *XOR) that aren't real
// stats, so they don't establish a set-bonus tier.
static bool
db_is_routing_flag(const char *name)
{
  size_t n = strlen(name);

  return((n >= 6 && strcasecmp(name + n - 6, "Global") == 0) ||
         (n >= 3 && strcasecmp(name + n - 3, "XOR") == 0));
}

// Tier depth of a set = the longest numeric stat array (excluding routing
// flags).  The array is indexed by (set items - 1), so index t is the bonus
// for wearing t+1 pieces.  Returns 0 when the record carries no stat arrays.
static int
db_set_tier_depth(TQArzRecordData *data)
{
  int depth = 0;

  if(!data)
    return(0);

  for(uint32_t v = 0; v < data->num_vars; v++)
  {
    TQVariable *var = &data->vars[v];

    if((var->type == TQ_VAR_INT || var->type == TQ_VAR_FLOAT) &&
       !db_is_routing_flag(var->name) && (int)var->count > depth)
      depth = (int)var->count;
  }

  return(depth);
}

// Rebuild the detail pane for an item set: the set name, its members (each in
// its own rarity color) and the tiered set bonuses.  Bonus tiers reuse
// add_stats_from_record at shard_index = (set items - 1); empty tiers (the
// one-piece slot and any all-zero rows) are skipped.
static void
update_detail_set(DbBrowserState *st, DbBrowseItem *bi)
{
  TQTranslation *tr = st->widgets->translations;
  TQArzRecordData *set_data = asset_get_dbr(bi->path);
  char markup[32768];
  BufWriter w;

  buf_init(&w, markup, sizeof(markup));

  // Set name.
  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='#40FF40'><b>%s</b></span>\n", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  // Members, each in its own rarity color.
  TQVariable *members = set_data ? arz_record_get_var(set_data, INT_setMembers)
                                 : NULL;

  int member_count = 0;

  if(members && members->type == TQ_VAR_STRING)
  {
    buf_write(&w, "\n<span color='#B0B0B0'>Set Members:</span>\n");

    for(uint32_t m = 0; m < members->count; m++)
    {
      const char *mp = members->value.str[m];
      const char *mname = db_set_member_name(st, mp);

      if(!mname)
        continue;

      member_count++;

      const char *color = get_item_color(mp, NULL, NULL);
      char *e_member = escape_markup(mname);

      buf_write(&w, "<span color='%s'>    %s</span>\n",
                color ? color : "white", e_member ? e_member : "");
      if(e_member)
        free(e_member);
    }
  }

  // Tiered set bonuses.  The stat arrays are indexed by (set items - 1); a set
  // with only scalar bonuses applies them to the full set, so the piece count
  // falls back to the member count.  Each tier reuses add_stats_from_record at
  // shard_index = (pieces - 1); empty tiers and ones identical to the tier
  // below (nothing new granted) are skipped.
  int depth = db_set_tier_depth(set_data);
  int full_pieces = (depth > 1) ? depth : member_count;
  bool any_tier = false;
  char prev_buf[8192] = "";

  for(int p = 2; p <= full_pieces; p++)
  {
    char tier_buf[8192];
    BufWriter tw;

    buf_init(&tw, tier_buf, sizeof(tier_buf));
    add_stats_from_record(bi->path, tr, &tw, "#40FF40", p - 1);

    if(tier_buf[0] == '\0' || strcmp(tier_buf, prev_buf) == 0)
      continue;  // no bonus, or nothing new granted at this many pieces

    memcpy(prev_buf, tier_buf, sizeof(prev_buf));

    if(!any_tier)
    {
      buf_write(&w, "\n<span color='#B0B0B0'>Set Bonuses:</span>\n");
      any_tier = true;
    }

    buf_write(&w, "\n<span color='#9F9F9F'>Required Set Items: %d</span>\n", p);
    buf_write(&w, "%s", tier_buf);
  }

  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, bi->icon_path, 0);
}

// -- Relic / charm / artifact extra detail (browser-only, tq-db style) ------

// Resolve a best-effort display name for an item record (translated
// description or itemNameTag, else a path-derived name).  Returns a malloc'd
// string the caller frees with free().
static char *
db_consumable_name(DbBrowserState *st, const char *path)
{
  const char *tag = dbr_get_string(path, "description");

  if(!tag || !tag[0])
    tag = dbr_get_string(path, "itemNameTag");

  if(tag && tag[0])
  {
    const char *name = translation_get(st->widgets->translations, tag);

    if(name && name[0] && strcmp(name, tag) != 0)
      return(strdup(name));
  }

  return(pretty_name_from_path(path));
}

// Find the completion-bonus randomizer table for a relic/charm (its own
// `bonusTableName`) or an artifact (its formula's `artifactBonusTableName`).
// Mirrors get_bonus_table_path() in ui_context_menu.c.  Returns an internal
// pointer (do not free), or NULL.
static const char *
db_bonus_table_path(const char *item_path)
{
  if(item_is_artifact(item_path))
  {
    const char *sep = strrchr(item_path, '\\');

    if(!sep)
      return(NULL);

    int dir_len = (int)(sep - item_path);
    const char *fname = sep + 1;
    int name_len = (int)strlen(fname);

    if(name_len > 4 && strcasecmp(fname + name_len - 4, ".dbr") == 0)
      name_len -= 4;

    char formula[1024];

    snprintf(formula, sizeof(formula), "%.*s\\arcaneformulae\\%.*s_formula.dbr",
             dir_len, item_path, name_len, fname);

    return(dbr_get_string(formula, "artifactBonusTableName"));
  }

  return(dbr_get_string(item_path, "bonusTableName"));
}

// True for one-shot scroll items.
static bool
db_is_scroll(const char *path)
{
  const char *cls = dbr_get_string(path, "Class");

  return(cls && strcasecmp(cls, "OneShot_Scroll") == 0);
}

// Append the full list of possible completion bonuses (each with its drop
// chance) for a relic/charm/artifact.  Enumerates the randomizer table's
// SPARSE randomizerName/Weight pairs, dedupes by path (summing weights) and
// renders each as "<chance>%  <name>" sorted most-likely first.
static void
db_append_bonus_table(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  const char *table_path = db_bonus_table_path(item_path);

  if(!table_path || !table_path[0])
    return;

  TQArzRecordData *table = asset_get_dbr(table_path);

  if(!table)
    return;

  const char *paths[256] = { 0 };
  float weights[256] = { 0 };

  for(uint32_t v = 0; v < table->num_vars; v++)
  {
    TQVariable *var = &table->vars[v];

    if(!var->name)
      continue;

    if(strncasecmp(var->name, "randomizerName", 14) == 0 &&
       var->type == TQ_VAR_STRING && var->count > 0 && var->value.str &&
       var->value.str[0] && var->value.str[0][0])
    {
      int idx = atoi(var->name + 14);

      if(idx >= 0 && idx < 256)
        paths[idx] = var->value.str[0];
    }
    else if(strncasecmp(var->name, "randomizerWeight", 16) == 0)
    {
      int idx = atoi(var->name + 16);
      float wt = 0;

      if(var->type == TQ_VAR_INT && var->count > 0 && var->value.i32)
        wt = (float)var->value.i32[0];
      else if(var->type == TQ_VAR_FLOAT && var->count > 0 && var->value.f32)
        wt = var->value.f32[0];

      if(idx >= 0 && idx < 256)
        weights[idx] = wt;
    }
  }

  // Dedup by path (sum weights); accumulate the grand total for percentages.
  const char *uniq_path[256];
  float uniq_w[256];
  int n = 0;
  float total = 0;

  for(int i = 0; i < 256; i++)
  {
    if(!paths[i] || weights[i] <= 0)
      continue;

    total += weights[i];

    int found = -1;

    for(int j = 0; j < n; j++)
      if(strcasecmp(uniq_path[j], paths[i]) == 0)
      {
        found = j;
        break;
      }

    if(found >= 0)
      uniq_w[found] += weights[i];
    else
    {
      uniq_path[n] = paths[i];
      uniq_w[n] = weights[i];
      n++;
    }
  }

  if(n == 0)
    return;

  // Sort by descending weight (most likely first).
  for(int i = 0; i < n - 1; i++)
    for(int j = i + 1; j < n; j++)
      if(uniq_w[j] > uniq_w[i])
      {
        const char *tp = uniq_path[i]; uniq_path[i] = uniq_path[j]; uniq_path[j] = tp;
        float tw = uniq_w[i]; uniq_w[i] = uniq_w[j]; uniq_w[j] = tw;
      }

  buf_write(w, "\n<span color='#B0B0B0'>Possible Completion Bonuses:</span>\n");

  for(int i = 0; i < n; i++)
  {
    float pct = total > 0 ? uniq_w[i] / total * 100.0f : 0;
    char *name = NULL;
    const char *desc = dbr_get_string(uniq_path[i], "description");

    if(desc && desc[0])
    {
      const char *t = translation_get(st->widgets->translations, desc);

      if(t && t[0] && strcmp(t, desc) != 0)
        name = strdup(t);
    }

    if(!name)
      name = item_bonus_stat_summary(uniq_path[i], st->widgets->translations);
    if(!name)
      name = pretty_name_from_path(uniq_path[i]);

    char *e = escape_markup(name ? name : "");

    buf_write(w, "<span color='#C1A472'>    %.1f%%  %s</span>\n", pct, e ? e : "");
    if(e)
      free(e);
    free(name);
  }
}

// Append an artifact's arcane-formula reagents (the recipe).
static void
db_append_artifact_formula(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  const char *sep = strrchr(item_path, '\\');

  if(!sep)
    return;

  int dir_len = (int)(sep - item_path);
  const char *fname = sep + 1;
  int name_len = (int)strlen(fname);

  if(name_len > 4 && strcasecmp(fname + name_len - 4, ".dbr") == 0)
    name_len -= 4;

  char formula[1024];

  snprintf(formula, sizeof(formula), "%.*s\\arcaneformulae\\%.*s_formula.dbr",
           dir_len, item_path, name_len, fname);

  TQArzRecordData *fd = asset_get_dbr(formula);

  if(!fd)
    return;

  const char *keys[] = { "reagent1BaseName", "reagent2BaseName", "reagent3BaseName" };
  bool header = false;

  for(int i = 0; i < 3; i++)
  {
    const char *rp = record_get_string_fast(fd, arz_intern(keys[i]));

    if(!rp || !rp[0])
      continue;

    if(!header)
    {
      buf_write(w, "\n<span color='#B0B0B0'>Arcane Formula — Reagents:</span>\n");
      header = true;
    }

    char *name = db_consumable_name(st, rp);
    char *e = escape_markup(name ? name : "");

    buf_write(w, "<span color='#C1A472'>    %s</span>\n", e ? e : "");
    if(e)
      free(e);
    free(name);
  }
}

// Append the browser-only, tq-db-style extra detail for a relic/charm
// (per-shard progression + completion-bonus table) or artifact (formula
// reagents + completion-bonus table).
static void
db_append_consumable_detail(DbBrowserState *st, const char *path, BufWriter *w)
{
  TQTranslation *tr = st->widgets->translations;

  if(item_is_relic_or_charm(path))
  {
    // Per-shard progression: the relic's stat arrays are indexed by shard-1.
    int max_shards = relic_max_shards(path);

    if(max_shards > 1)
    {
      char prev[4096] = "";
      bool header = false;

      for(int s = 1; s <= max_shards; s++)
      {
        char sb[4096];
        BufWriter sw;

        buf_init(&sw, sb, sizeof(sb));
        add_stats_from_record(path, tr, &sw, "#C1A472", s - 1);

        if(sb[0] == '\0' || strcmp(sb, prev) == 0)
          continue;

        memcpy(prev, sb, sizeof(prev));

        if(!header)
        {
          buf_write(w, "\n<span color='#B0B0B0'>Shard Progression:</span>\n");
          header = true;
        }

        buf_write(w, "\n<span color='#9F9F9F'>%d Shard%s:</span>\n", s, s > 1 ? "s" : "");
        buf_write(w, "%s", sb);
      }
    }

    db_append_bonus_table(st, path, w);
  }
  else if(item_is_artifact(path))
  {
    db_append_artifact_formula(st, path, w);
    db_append_bonus_table(st, path, w);
  }
  else if(db_is_scroll(path))
  {
    // The granted-skill section of the shared tooltip keys off itemSkillName;
    // scrolls use skillName, so render the granted skill here (name +
    // description + first-tier properties), mirroring that logic.
    const char *skill = dbr_get_string(path, "skillName");

    if(skill && skill[0])
    {
      TQArzRecordData *sd = asset_get_dbr(skill);
      const char *buff = sd ? record_get_string_fast(sd, INT_buffSkillName) : NULL;
      const char *effect = (buff && buff[0]) ? buff : skill;
      TQArzRecordData *ed = asset_get_dbr(effect);

      const char *ntag = ed ? record_get_string_fast(ed, INT_skillDisplayName) : NULL;

      if(!ntag && sd)
        ntag = record_get_string_fast(sd, INT_skillDisplayName);

      buf_write(w, "\n<span color='#B0B0B0'>Grants Skill:</span>\n");

      const char *sname = ntag ? translation_get(tr, ntag) : NULL;

      if(sname && sname[0])
      {
        char *e = escape_markup(sname);

        buf_write(w, "<span color='#DAA520'>%s</span>\n", e ? e : "");
        if(e)
          free(e);
      }

      const char *dtag = ed ? record_get_string_fast(ed, INT_skillBaseDescription) : NULL;

      if(!dtag && sd)
        dtag = record_get_string_fast(sd, INT_skillBaseDescription);

      if(dtag)
      {
        const char *dt = translation_get(tr, dtag);

        if(dt && dt[0])
        {
          char *e = escape_markup(dt);

          buf_write(w, "<span color='#DAA520'>%s</span>\n", e ? e : "");
          if(e)
            free(e);
        }
      }

      add_stats_from_record(effect, tr, w, "#DAA520", 0);
    }
  }
}

// Replace every occurrence of `from` with `to`.  Returns a newly g_malloc'd
// string (caller g_free's).  Used for the skill-tooltip "Intelligence" ->
// "Intellect" swap (skills phrase that attribute differently from item stats).
static char *
db_str_replace(const char *s, const char *from, const char *to)
{
  GString *g = g_string_new(NULL);
  size_t fl = strlen(from);

  for(const char *p = s; *p;)
  {
    if(strncmp(p, from, fl) == 0)
    {
      g_string_append(g, to);
      p += fl;
    }
    else
    {
      g_string_append_c(g, *p++);
    }
  }

  return(g_string_free(g, FALSE));
}

// Append a skill's leveled properties to the detail buffer.  Stat arrays are
// indexed by (level - 1); each distinct, non-empty tier is shown via
// add_stats_from_record (which follows buff/pet/skill refs).  Masteries scale
// linearly across many levels, so only their first and max tiers are shown;
// regular skills show every distinct tier.  eff_path is the description-bearing
// record (db_skill_effective_path).
static void
db_append_skill_levels(DbBrowserState *st, const char *eff_path, int max_level,
                       bool is_mastery, BufWriter *w)
{
  TQTranslation *tr = st->widgets->translations;
  const char *WHITE = "#E0E0E0";

  if(max_level < 1)
    max_level = 1;

  // Masteries: just the two endpoints (the bonus is uniform between them).
  int levels[64];
  int nlevels = 0;

  if(is_mastery && max_level > 2)
  {
    levels[nlevels++] = 1;
    levels[nlevels++] = max_level;
  }
  else
  {
    for(int L = 1; L <= max_level && nlevels < 64; L++)
      levels[nlevels++] = L;
  }

  char prev[8192] = "";
  bool any = false;

  for(int i = 0; i < nlevels; i++)
  {
    int L = levels[i];
    char lb[8192];
    BufWriter lw;

    buf_init(&lw, lb, sizeof(lb));
    add_stats_from_record(eff_path, tr, &lw, WHITE, L - 1);

    if(lb[0] == '\0' || strcmp(lb, prev) == 0)
      continue;  // no properties, or unchanged from the previous tier

    memcpy(prev, lb, sizeof(prev));

    if(!any)
    {
      buf_write(w, "\n<span color='#B0B0B0'>Properties by Level:</span>\n");
      any = true;
    }

    buf_write(w, "\n<span color='#FFD200'>Level %d:</span>\n", L);
    buf_write(w, "%s", lb);
  }
}

// Rebuild the detail pane for a mastery/skill: its name (skill gold),
// description, max level + mastery-level requirement, and its leveled
// properties.  Reuses the same DBR machinery as the visual skill manager
// (add_stats_from_record over the effective skill record).  Icon is the
// skill's .tex bitmap (loaded via texture_load, not the item loader).
static void
update_detail_skill(DbBrowserState *st, DbBrowseItem *bi)
{
  TQTranslation *tr = st->widgets->translations;
  char markup[32768];
  BufWriter w;

  buf_init(&w, markup, sizeof(markup));

  // Name.
  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='%s'><b>%s</b></span>\n",
            bi->color ? bi->color : "#DAA520", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  const char *eff = db_skill_effective_path(bi->path, 0);

  if(!eff)
    eff = bi->path;

  // Description (the buff/pet/toggle shell keeps it in the effective record).
  const char *dtag = dbr_get_string(eff, "skillBaseDescription");

  if(dtag && dtag[0])
  {
    const char *d = translation_get(tr, dtag);

    if(d && d[0] && strcmp(d, dtag) != 0)
    {
      char *e = escape_markup(d);

      buf_write(&w, "<span color='#C8C8C8'>%s</span>\n", e ? e : "");
      if(e)
        free(e);
    }
  }

  // Max level + mastery-level requirement.
  int max_level = db_skill_max_level(bi->path, 0);

  if(max_level < 1)
    max_level = 1;

  bool is_mastery = strcasestr(bi->path, "mastery") != NULL;

  buf_write(&w, "\n<span color='#9F9F9F'>Max Level: %d</span>\n", max_level);

  TQArzRecordData *sd = asset_get_dbr(bi->path);
  int mreq = sd ? arz_record_get_int(sd, "skillMasteryLevelRequired", 0, NULL) : 0;

  if(mreq > 0)
    buf_write(&w, "<span color='#9F9F9F'>Required Mastery Level: %d</span>\n", mreq);

  // Leveled properties.
  db_append_skill_levels(st, eff, max_level, is_mastery, &w);

  // Skills say "Intellect" where item-stat formatting says "Intelligence".
  char *fixed = db_str_replace(markup, "Intelligence", "Intellect");

  gtk_label_set_markup(GTK_LABEL(st->detail_label), fixed);
  g_free(fixed);

  // Icon: the skill's .tex bitmap.
  GdkPixbuf *pb = bi->icon_path ? texture_load(bi->icon_path) : NULL;

  db_detail_set_pixbuf(st, pb);
  if(pb)
    g_object_unref(pb);
}

// Rebuild the detail pane for a prefix/suffix affix: its name (in rarity
// color), prefix/suffix kind + classification, level requirement, the granted
// properties (via add_stats_from_record, which also follows pet/skill bonuses)
// and the equipment types it can roll on.  Stat-less affixes (the Tinkerer's
// extra relic slot) fall back to their special text.  Affixes have no icon.
static void
update_detail_affix(DbBrowserState *st, DbBrowseItem *bi)
{
  TQTranslation *tr = st->widgets->translations;
  char markup[32768];
  BufWriter w;

  buf_init(&w, markup, sizeof(markup));

  // Name in its rarity color.
  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='%s'><b>%s</b></span>\n",
            bi->color ? bi->color : "white", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  // Kind (prefix/suffix) + classification, derived from the record.
  const char *kind = path_contains_ci(bi->path, "\\suffix\\") ? "Suffix" : "Prefix";
  const char *cls = dbr_get_string(bi->path, "itemClassification");

  if(cls && cls[0])
    buf_write(&w, "<span color='#9F9F9F'>%s — %s</span>\n", kind, cls);
  else
    buf_write(&w, "<span color='#9F9F9F'>%s</span>\n", kind);

  // Level requirement, if any.
  int req[4];

  item_record_requirements(bi->path, req);
  if(req[0] > 0)
    buf_write(&w, "<span color='#9F9F9F'>Required Level: %d</span>\n", req[0]);

  // Properties: one block per distinct variant roll (a merged affix such as
  // "Allfather's" carries several).  add_stats_from_record follows petBonusName
  // and skill grants.  Distinct blocks are separated as alternatives.
  buf_write(&w, "\n<span color='#B0B0B0'>Properties:</span>\n");

  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  int shown = 0;

  for(char **vp = bi->variants; vp && *vp; vp++)
  {
    char props[8192];
    BufWriter pw;

    buf_init(&pw, props, sizeof(props));
    add_stats_from_record(*vp, tr, &pw, "#00A3FF", 0);

    if(props[0] == '\0' || g_hash_table_contains(seen, props))
      continue;  // empty, or identical to a variant already shown

    g_hash_table_add(seen, g_strdup(props));

    if(shown > 0)
      buf_write(&w, "<span color='#707070'>   — or —</span>\n");
    buf_write(&w, "%s", props);
    shown++;
  }

  g_hash_table_destroy(seen);

  if(shown == 0)
  {
    // Stat-less affix (e.g. "of the Tinkerer": engine-level extra relic slot).
    const char *rtag = dbr_get_string(bi->path, "lootRandomizerName");
    const char *special = (rtag && strcasecmp(rtag, "x3tagSuffix01") == 0)
                          ? translation_get(tr, "x3tagExtraRelic") : NULL;

    if(special && special[0])
    {
      char *e = escape_markup(special);

      buf_write(&w, "<span color='#00A3FF'>%s</span>\n", e ? e : "");
      if(e)
        free(e);
    }
    else
    {
      buf_write(&w, "<span color='#808080'>(no direct properties)</span>\n");
    }
  }

  // Equipment types the affix can roll on.
  if(bi->equip_types && bi->equip_types[0])
  {
    char *e = escape_markup(bi->equip_types);

    buf_write(&w, "\n<span color='#B0B0B0'>Can appear on:</span>\n");
    buf_write(&w, "<span color='#C8C8C8'>%s</span>\n", e ? e : "");
    if(e)
      free(e);
  }

  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, NULL, 0);
}

// -- Creature / quest detail + cross-reference (Phase 6) --------------------

// Format a per-difficulty chance triple compactly into out (e.g.
// "N 12.30%  E 4.50%"), showing only the non-zero tiers.
static void
db_fmt_chances(const double ch[3], char *out, size_t outsz)
{
  static const char *D[3] = { "N", "E", "L" };
  BufWriter w;

  buf_init(&w, out, outsz);
  for(int i = 0; i < 3; i++)
    if(ch[i] > 0.0)
      buf_write(&w, "%s%s %.2f%%", (w.pos > 0 ? "  " : ""), D[i], ch[i]);

  if(out[0] == '\0')
    snprintf(out, outsz, "—");
}

// Append a list of item rows (name in rarity color + chances) to w, capped at
// `limit` entries with a "+N more" note.  `paths`/`chances` are parallel.
static void
db_append_item_rows(DbBrowserState *st, BufWriter *w, GPtrArray *paths,
                    int limit)
{
  guint n = paths->len;
  guint shown = (n > (guint)limit) ? (guint)limit : n;

  for(guint i = 0; i < shown; i++)
  {
    DbItemChance *ic = g_ptr_array_index(paths, i);
    char *name = db_item_display_name(st, ic->item_lc);
    const char *color = get_item_color(ic->item_lc, NULL, NULL);
    char *e = escape_markup(name ? name : ic->item_lc);
    char chbuf[96];

    db_fmt_chances(ic->chance, chbuf, sizeof(chbuf));
    buf_write(w, "<span color='%s'>    %s</span> "
                 "<span color='#808080'>%s</span>\n",
              color ? color : "white", e ? e : "", chbuf);
    if(e)
      free(e);
    free(name);
  }

  if(n > shown)
    buf_write(w, "<span color='#808080'>    … +%u more</span>\n", n - shown);
}

// Detail pane for a creature: name, classification/race/levels, and the items
// it can drop (per-difficulty chance).
static void
update_detail_creature(DbBrowserState *st, DbBrowseItem *bi)
{
  DbCreature *c = g_ptr_array_index(st->creatures->creatures, bi->src_idx);
  char markup[32768];
  BufWriter w;

  buf_init(&w, markup, sizeof(markup));

  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='%s'><b>%s</b></span>\n",
            bi->color ? bi->color : "white", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  buf_write(&w, "<span color='#9F9F9F'>%s%s%s — Levels %d / %d / %d</span>\n",
            c->classification ? c->classification : "",
            c->race ? " · " : "", c->race ? c->race : "",
            c->level[0], c->level[1], c->level[2]);

  buf_write(&w, "\n<span color='#B0B0B0'>Drops (%u):</span>\n", c->drops->len);
  db_append_item_rows(st, &w, c->drops, 80);

  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, NULL, 0);
}

// Detail pane for a quest: name and its item rewards per difficulty.
static void
update_detail_quest(DbBrowserState *st, DbBrowseItem *bi)
{
  DbQuest *q = g_ptr_array_index(st->quests->quests, bi->src_idx);
  char markup[32768];
  BufWriter w;

  buf_init(&w, markup, sizeof(markup));

  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='#DAA520'><b>%s</b></span>\n", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  static const char *DIFF[3] = { "Normal", "Epic", "Legendary" };

  for(int d = 0; d < 3; d++)
  {
    if(!q->rewards[d] || g_hash_table_size(q->rewards[d]) == 0)
      continue;

    // Collect this difficulty's rewards into a sortable list.
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter it;
    gpointer k, v;

    g_hash_table_iter_init(&it, q->rewards[d]);
    while(g_hash_table_iter_next(&it, &k, &v))
    {
      DbItemChance *ic = g_new0(DbItemChance, 1);
      ic->item_lc = (char *)k;           // borrowed; not freed by us
      ic->chance[d] = *(double *)v;
      g_ptr_array_add(rows, ic);
    }

    buf_write(&w, "\n<span color='#B0B0B0'>%s rewards:</span>\n", DIFF[d]);
    db_append_item_rows(st, &w, rows, 40);
    g_ptr_array_free(rows, TRUE);  // frees the DbItemChance shells, not item_lc
  }

  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, NULL, 0);
}

// Append the "Dropped by" cross-reference for an item: the creatures that can
// drop it, best chance first, capped.
static void
db_append_dropped_by(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  if(!st->creatures)
    return;

  GPtrArray *drops = db_creature_drops_for_item(st->creatures, item_path);

  if(!drops || drops->len == 0)
    return;

  buf_write(w, "\n<span color='#B0B0B0'>Dropped by:</span>\n");

  guint limit = 20;
  guint shown = (drops->len > limit) ? limit : drops->len;

  for(guint i = 0; i < shown; i++)
  {
    DbDrop *d = g_ptr_array_index(drops, i);
    DbCreature *c = g_ptr_array_index(st->creatures->creatures, d->creature_idx);
    char *name = db_creature_display_name(st, c);
    char *e = escape_markup(name);
    char chbuf[96];

    db_fmt_chances(d->chance, chbuf, sizeof(chbuf));
    buf_write(w, "<span color='%s'>    %s</span> "
                 "<span color='#808080'>%s</span>\n",
              db_creature_color(c->classification), e ? e : "", chbuf);
    if(e)
      free(e);
    g_free(name);
  }

  if(drops->len > shown)
    buf_write(w, "<span color='#808080'>    … +%u more</span>\n",
              drops->len - shown);
}

// Append the "Reward from" cross-reference for an item: the quests that grant
// it (with the best per-difficulty percentage).
static void
db_append_reward_from(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  if(!st->quests)
    return;

  GPtrArray *rewards = db_quest_rewards_for_item(st->quests, item_path);

  if(!rewards || rewards->len == 0)
    return;

  buf_write(w, "\n<span color='#B0B0B0'>Quest reward from:</span>\n");

  for(guint i = 0; i < rewards->len && i < 20; i++)
  {
    DbQuestReward *rw = g_ptr_array_index(rewards, i);
    DbQuest *q = g_ptr_array_index(st->quests->quests, rw->quest_idx);
    const char *name = translation_get(st->widgets->translations, q->title_tag);
    char *e = escape_markup((name && name[0]) ? name : q->title_tag);
    char chbuf[96];

    db_fmt_chances(rw->percent, chbuf, sizeof(chbuf));
    buf_write(w, "<span color='#DAA520'>    %s</span> "
                 "<span color='#808080'>%s</span>\n", e ? e : "", chbuf);
    if(e)
      free(e);
  }
}

// Rebuild the right-hand detail pane for the selected item: large icon plus
// the full formatted tooltip (built from a synthetic vault item, the same
// path the in-app tooltips use).  Relics/charms/artifacts get extra tq-db-style
// sections appended (shard progression, completion bonuses, formula reagents);
// every item gets "Dropped by" / "Quest reward from" cross-references.
static void
update_detail(DbBrowserState *st, DbBrowseItem *bi)
{
  if(!bi)
  {
    gtk_picture_set_paintable(GTK_PICTURE(st->detail_pic), NULL);
    gtk_label_set_markup(GTK_LABEL(st->detail_label), "");
    return;
  }

  if(bi->is_set)
  {
    update_detail_set(st, bi);
    return;
  }

  if(bi->is_affix)
  {
    update_detail_affix(st, bi);
    return;
  }

  if(bi->is_skill)
  {
    update_detail_skill(st, bi);
    return;
  }

  if(bi->is_creature)
  {
    update_detail_creature(st, bi);
    return;
  }

  if(bi->is_quest)
  {
    update_detail_quest(st, bi);
    return;
  }

  // Full tooltip via the shared formatter (reads base_name only).
  TQVaultItem vi;

  memset(&vi, 0, sizeof(vi));
  vi.base_name  = (char *)bi->path;  // formatter only reads it
  vi.var1       = bi->var1;
  vi.stack_size = 1;

  char markup[32768];

  vault_item_format_stats(&vi, st->widgets->translations, markup, sizeof(markup));

  size_t len = strlen(markup);
  BufWriter w;

  buf_init(&w, markup + len, sizeof(markup) - len);

  // Append browser-only detail for relics/charms/artifacts/scrolls.
  if(item_is_relic_or_charm(bi->path) || item_is_artifact(bi->path) ||
     db_is_scroll(bi->path))
    db_append_consumable_detail(st, bi->path, &w);

  // Cross-references: which creatures drop it and which quests grant it.
  db_append_dropped_by(st, bi->path, &w);
  db_append_reward_from(st, bi->path, &w);

  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, bi->path, bi->var1);
}

static void
on_grid_selection_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
  (void)pspec;

  DbBrowserState *st = data;
  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(obj);
  // selected-item is the DbBrowseItem (passthrough model); transfer-none.
  DbBrowseItem *bi = gtk_single_selection_get_selected_item(sel);

  update_detail(st, bi);
}

// -- Search filter ----------------------------------------------------------

// GtkCustomFilter callback: keep items whose lowercased name contains the
// current needle.  O(1) per row (precomputed name_lc).
static gboolean
filter_match(gpointer item, gpointer data)
{
  DbBrowserState *st = data;

  if(st->search_lc[0] == '\0')
    return(TRUE);

  DbBrowseItem *bi = DB_BROWSE_ITEM(item);

  return(bi->name_lc && strstr(bi->name_lc, st->search_lc) != NULL);
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer data)
{
  DbBrowserState *st = data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  size_t len = strlen(text);

  if(len >= sizeof(st->search_lc))
    len = sizeof(st->search_lc) - 1;

  for(size_t i = 0; i < len; i++)
    st->search_lc[i] = (char)tolower((unsigned char)text[i]);

  st->search_lc[len] = '\0';

  gtk_filter_changed(GTK_FILTER(st->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
}

// -- Grid factory -----------------------------------------------------------

// setup: a vertical box holding a fixed-size GtkPicture (icon, aspect kept)
// and a wrapping colored GtkLabel (name).
static void
grid_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *li,
                   gpointer data)
{
  (void)factory;
  (void)data;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  gtk_widget_set_size_request(box, 96, 96);

  GtkWidget *pic = gtk_picture_new();

  gtk_widget_set_size_request(pic, 64, 64);
  gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(box), pic);

  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 14);
  gtk_widget_set_valign(label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), label);

  // Remember the two children so bind doesn't have to walk the box.
  g_object_set_data(G_OBJECT(box), "pic", pic);
  g_object_set_data(G_OBJECT(box), "label", label);

  gtk_list_item_set_child(li, box);
}

static void
grid_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *li,
                  gpointer data)
{
  (void)factory;

  DbBrowserState *st = data;
  GtkWidget *box = gtk_list_item_get_child(li);
  DbBrowseItem *bi = DB_BROWSE_ITEM(gtk_list_item_get_item(li));
  GtkWidget *pic = g_object_get_data(G_OBJECT(box), "pic");
  GtkWidget *label = g_object_get_data(G_OBJECT(box), "label");

  // Stash the item so right-click pickup can find it.
  g_object_set_data(G_OBJECT(box), "dbitem", bi);

  char *esc = escape_markup(bi->name);
  char *markup = g_strdup_printf("<span foreground='%s'>%s</span>",
                                  bi->color ? bi->color : "white",
                                  esc ? esc : "");

  gtk_label_set_markup(GTK_LABEL(label), markup);
  g_free(markup);
  if(esc)
    free(esc);

  // Sets have no bitmap of their own; draw their first member's icon.  Skills
  // carry a .tex bitmap path rather than an item record, so load it directly.
  GdkPixbuf *pb;

  if(bi->is_skill)
    pb = bi->icon_path ? texture_load(bi->icon_path) : NULL;
  else
  {
    const char *icon = bi->icon_path ? bi->icon_path : bi->path;

    pb = load_item_texture(st->widgets, icon, bi->var1);
  }

  if(pb)
  {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    G_GNUC_END_IGNORE_DEPRECATIONS

    gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(tex));
    g_object_unref(tex);
    g_object_unref(pb);
  }
  else
  {
    gtk_picture_set_paintable(GTK_PICTURE(pic), NULL);
  }
}

static void
grid_factory_unbind(GtkSignalListItemFactory *factory, GtkListItem *li,
                    gpointer data)
{
  (void)factory;
  (void)data;

  GtkWidget *box = gtk_list_item_get_child(li);

  g_object_set_data(G_OBJECT(box), "dbitem", NULL);
}

// -- Sidebar ----------------------------------------------------------------

// Switch the center grid to show the given category's items.
static void
show_category(DbBrowserState *st, int cat)
{
  if(cat < 0 || cat >= CAT_COUNT)
    return;

  gtk_filter_list_model_set_model(st->filter_model,
                                   G_LIST_MODEL(st->cat_stores[cat]));

  char buf[64];

  snprintf(buf, sizeof(buf), "%s — %d items",
           CAT_INFO[cat].leaf, st->cat_counts[cat]);
  gtk_label_set_text(GTK_LABEL(st->count_label), buf);

  update_detail(st, NULL);
}

// A selectable sidebar row carries its category index as data.
static void
on_sidebar_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  (void)box;

  DbBrowserState *st = data;

  if(!row)
    return;

  int cat = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "cat"));

  show_category(st, cat);
}

// Build the left sidebar: a GtkListBox with a non-selectable header row per
// group and a selectable row per leaf category (showing its item count).
static GtkWidget *
build_sidebar(DbBrowserState *st)
{
  GtkWidget *list = gtk_list_box_new();

  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);

  const char *cur_group = NULL;

  for(int cat = 0; cat < CAT_COUNT; cat++)
  {
    // Group header when the group label changes.
    if(!cur_group || strcmp(cur_group, CAT_INFO[cat].group) != 0)
    {
      cur_group = CAT_INFO[cat].group;

      GtkWidget *hrow = gtk_list_box_row_new();
      GtkWidget *hlbl = gtk_label_new(NULL);
      char *m = g_strdup_printf("<b>%s</b>", cur_group);

      gtk_label_set_markup(GTK_LABEL(hlbl), m);
      g_free(m);
      gtk_label_set_xalign(GTK_LABEL(hlbl), 0.0f);
      gtk_widget_set_margin_top(hlbl, 6);
      gtk_widget_set_margin_start(hlbl, 4);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(hrow), hlbl);
      gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(hrow), FALSE);
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(hrow), FALSE);
      gtk_list_box_append(GTK_LIST_BOX(list), hrow);
    }

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *lbl = gtk_label_new(NULL);
    char *txt = g_strdup_printf("%s  (%d)", CAT_INFO[cat].leaf,
                                 st->cat_counts[cat]);

    gtk_label_set_text(GTK_LABEL(lbl), txt);
    g_free(txt);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_margin_start(lbl, 16);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
    g_object_set_data(G_OBJECT(row), "cat", GINT_TO_POINTER(cat));
    gtk_list_box_append(GTK_LIST_BOX(list), row);
  }

  g_signal_connect(list, "row-selected",
                   G_CALLBACK(on_sidebar_row_selected), st);

  return(list);
}

// -- Show the database browser ----------------------------------------------

// The browser's data load (database parse + six category indexes) takes a
// noticeable moment on slower machines.  Rather than freeze the UI, we pop up
// a small modal progress dialog and drive the load one phase at a time from the
// main loop's idle handler, repainting the bar between phases.  Everything
// stays on the main thread, so there are no threading hazards around the
// GListStores, the GObject items, or the global intern/asset caches.

// Phase 0: get the database handle.  The app pre-loads every .arz at startup,
// so we reuse the shared cached handle rather than parse a second in-memory
// copy of the string + record tables (which would also re-pound the global
// intern table).  Falls back to loading our own copy only if the cache somehow
// doesn't have it.  Wrapped (rather than inlined in the phase table) to match
// the void(DbBrowserState*) shape the index builders already have.
static void
db_load_arz(DbBrowserState *st)
{
  st->arz = asset_get_database_arz();
  st->owns_arz = false;

  if(!st->arz)
  {
    char arz_path[1024];

    snprintf(arz_path, sizeof(arz_path), "%s/Database/database.arz",
             global_config.game_folder);
    st->arz = arz_load(arz_path);
    st->owns_arz = true;
  }
}

// The ordered load phases: a label shown while each runs, and the worker that
// does it.  The build_* functions match the signature directly.
static const struct
{
  const char *label;
  void      (*fn)(DbBrowserState *st);
}
DB_LOAD_PHASES[] =
{
  { "Loading game database...",  db_load_arz },
  { "Indexing equipment...",     build_category_index },
  { "Indexing item sets...",     build_set_index },
  { "Indexing affixes...",       build_affix_index },
  { "Indexing skills...",        build_skill_index },
  { "Indexing creatures...",     build_creature_index_grid },
  { "Indexing quest rewards...", build_quest_index_grid },
};

#define DB_LOAD_NPHASES ((int)G_N_ELEMENTS(DB_LOAD_PHASES))

// Transient loader state, alive only for the duration of the load.
typedef struct
{
  DbBrowserState *st;
  GtkWidget      *win;       // progress dialog
  GtkWidget      *bar;       // GtkProgressBar
  GtkWidget      *status;    // status label
  int             phase;     // next phase to run
  gboolean        announced; // current phase's label has been shown
} DbLoader;

static void db_browser_build_ui(DbBrowserState *st);

// Run one slice of the load per idle invocation.  Each phase is handled in two
// passes: first we show its label and yield (so the dialog repaints the bar and
// label), then we do the blocking work.  This keeps the user informed about
// which step is in progress even while that step holds the main loop.
static gboolean
db_browser_load_step(gpointer data)
{
  DbLoader *ld = data;
  DbBrowserState *st = ld->st;

  // All phases done: drop the modal progress dialog, then build and present
  // the real browser window.
  if(ld->phase >= DB_LOAD_NPHASES)
  {
    gtk_window_destroy(GTK_WINDOW(ld->win));
    db_browser_build_ui(st);
    g_free(ld);
    return(G_SOURCE_REMOVE);
  }

  // First pass for this phase: announce it, then yield so the bar repaints
  // before the (blocking) work below runs.
  if(!ld->announced)
  {
    gtk_label_set_text(GTK_LABEL(ld->status), DB_LOAD_PHASES[ld->phase].label);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ld->bar),
                                   (double)ld->phase / DB_LOAD_NPHASES);
    ld->announced = TRUE;
    return(G_SOURCE_CONTINUE);
  }

  // Second pass: do the work for this phase.
  DB_LOAD_PHASES[ld->phase].fn(st);

  // Phase 0 loads the database; if that failed there is nothing to show.
  if(!st->arz)
  {
    gtk_window_destroy(GTK_WINDOW(ld->win));
    db_browser_state_free(st);
    g_free(ld);
    return(G_SOURCE_REMOVE);
  }

  ld->phase++;
  ld->announced = FALSE;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ld->bar),
                                 (double)ld->phase / DB_LOAD_NPHASES);
  return(G_SOURCE_CONTINUE);
}

// Build and present the small modal "loading" dialog.
static void
db_browser_show_progress(DbLoader *ld, AppWidgets *widgets)
{
  ld->win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(ld->win), "Database Browser");
  gtk_window_set_default_size(GTK_WINDOW(ld->win), 380, -1);
  gtk_window_set_resizable(GTK_WINDOW(ld->win), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(ld->win),
                                GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(ld->win), TRUE);
  gtk_window_set_deletable(GTK_WINDOW(ld->win), FALSE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);

  GtkWidget *title = gtk_label_new(NULL);

  gtk_label_set_markup(GTK_LABEL(title), "<b>Loading Database...</b>");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(box), title);

  ld->status = gtk_label_new("Preparing...");
  gtk_label_set_xalign(GTK_LABEL(ld->status), 0.0f);
  gtk_widget_add_css_class(ld->status, "dim-label");
  gtk_box_append(GTK_BOX(box), ld->status);

  ld->bar = gtk_progress_bar_new();
  gtk_box_append(GTK_BOX(box), ld->bar);

  gtk_window_set_child(GTK_WINDOW(ld->win), box);
  gtk_window_present(GTK_WINDOW(ld->win));
}

void
show_db_browser_dialog(AppWidgets *widgets)
{
  if(!global_config.game_folder)
    return;

  DbBrowserState *st = g_new0(DbBrowserState, 1);

  st->widgets = widgets;
  st->dlg_cursor_x = -10000.0;
  st->dlg_cursor_y = -10000.0;

  // Cheap to create; populated by the indexing phases below.
  for(int i = 0; i < CAT_COUNT; i++)
    st->cat_stores[i] = g_list_store_new(DB_TYPE_BROWSE_ITEM);

  // Show the progress dialog immediately, then load in phases from the idle
  // loop so the UI never appears frozen while the database is indexed.
  DbLoader *ld = g_new0(DbLoader, 1);

  ld->st = st;
  db_browser_show_progress(ld, widgets);
  g_idle_add(db_browser_load_step, ld);
}

// Build the full browser window once all data is indexed.
static void
db_browser_build_ui(DbBrowserState *st)
{
  AppWidgets *widgets = st->widgets;

  // -- Window + held-item overlay --
  st->dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(st->dialog), "Database Browser");
  gtk_window_set_default_size(GTK_WINDOW(st->dialog), 1100, 760);
  gtk_window_set_transient_for(GTK_WINDOW(st->dialog),
                                GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(st->dialog), FALSE);
  g_object_set_data_full(G_OBJECT(st->dialog), "state", st,
                          db_browser_state_free);

  GtkWidget *overlay = gtk_overlay_new();

  gtk_widget_set_hexpand(overlay, TRUE);
  gtk_widget_set_vexpand(overlay, TRUE);
  gtk_window_set_child(GTK_WINDOW(st->dialog), overlay);

  st->dlg_held_overlay = gtk_drawing_area_new();
  gtk_widget_set_can_target(st->dlg_held_overlay, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(st->dlg_held_overlay),
                                  dlg_held_overlay_draw_cb, st, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), st->dlg_held_overlay);

  GtkEventController *dlg_motion = gtk_event_controller_motion_new();

  gtk_event_controller_set_propagation_phase(dlg_motion, GTK_PHASE_CAPTURE);
  g_signal_connect(dlg_motion, "motion", G_CALLBACK(on_dlg_overlay_motion), st);
  g_signal_connect(dlg_motion, "leave", G_CALLBACK(on_dlg_overlay_leave), st);
  gtk_widget_add_controller(overlay, dlg_motion);

  // -- Three-pane layout: sidebar | grid | detail --
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

  gtk_paned_set_position(GTK_PANED(paned), 220);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), paned);

  // Left: category sidebar.
  GtkWidget *side_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll),
                                 build_sidebar(st));
  gtk_paned_set_start_child(GTK_PANED(paned), side_scroll);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

  // Right side: a second paned splits the grid from the detail pane.
  GtkWidget *paned2 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

  gtk_paned_set_position(GTK_PANED(paned2), 540);
  gtk_paned_set_end_child(GTK_PANED(paned), paned2);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);

  // Center: search entry above the icon grid.
  GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_margin_start(center, 4);
  gtk_widget_set_margin_end(center, 4);
  gtk_widget_set_margin_top(center, 4);
  gtk_widget_set_margin_bottom(center, 4);

  GtkWidget *search = gtk_search_entry_new();

  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
                                         "Filter by name...");
  g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed), st);
  gtk_box_append(GTK_BOX(center), search);

  st->count_label = gtk_label_new("Select a category");
  gtk_label_set_xalign(GTK_LABEL(st->count_label), 0.0f);
  gtk_box_append(GTK_BOX(center), st->count_label);

  // Filter model over the (initially empty) active category store.
  st->custom_filter = gtk_custom_filter_new(filter_match, st, NULL);
  g_object_ref(st->custom_filter);

  st->filter_model = gtk_filter_list_model_new(NULL, GTK_FILTER(st->custom_filter));
  // transfer-full of the filter on the line above.

  st->selection = gtk_single_selection_new(G_LIST_MODEL(st->filter_model));
  gtk_single_selection_set_autoselect(st->selection, FALSE);
  gtk_single_selection_set_can_unselect(st->selection, TRUE);
  g_signal_connect(st->selection, "notify::selected-item",
                   G_CALLBACK(on_grid_selection_changed), st);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  g_signal_connect(factory, "setup",  G_CALLBACK(grid_factory_setup),  st);
  g_signal_connect(factory, "bind",   G_CALLBACK(grid_factory_bind),   st);
  g_signal_connect(factory, "unbind", G_CALLBACK(grid_factory_unbind), st);

  st->grid_view = gtk_grid_view_new(GTK_SELECTION_MODEL(st->selection), factory);
  gtk_grid_view_set_max_columns(GTK_GRID_VIEW(st->grid_view), 16);
  gtk_grid_view_set_min_columns(GTK_GRID_VIEW(st->grid_view), 2);

  GtkGesture *grid_rc = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(grid_rc), GDK_BUTTON_SECONDARY);
  g_signal_connect(grid_rc, "pressed", G_CALLBACK(on_grid_right_click), st);
  gtk_widget_add_controller(st->grid_view, GTK_EVENT_CONTROLLER(grid_rc));

  GtkWidget *grid_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(grid_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(grid_scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(grid_scroll), st->grid_view);
  gtk_box_append(GTK_BOX(center), grid_scroll);

  gtk_paned_set_start_child(GTK_PANED(paned2), center);
  gtk_paned_set_resize_start_child(GTK_PANED(paned2), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned2), FALSE);

  // Right: detail pane (large icon + full tooltip).
  GtkWidget *detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

  gtk_widget_set_margin_start(detail, 8);
  gtk_widget_set_margin_end(detail, 8);
  gtk_widget_set_margin_top(detail, 8);
  gtk_widget_set_margin_bottom(detail, 8);

  st->detail_pic = gtk_picture_new();
  gtk_widget_set_size_request(st->detail_pic, 128, 128);
  gtk_picture_set_content_fit(GTK_PICTURE(st->detail_pic), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_halign(st->detail_pic, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(detail), st->detail_pic);

  st->detail_label = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(st->detail_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(st->detail_label), 0.0f);
  gtk_label_set_yalign(GTK_LABEL(st->detail_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(st->detail_label), TRUE);
  gtk_widget_add_css_class(st->detail_label, "item-tooltip");
  gtk_box_append(GTK_BOX(detail), st->detail_label);

  GtkWidget *detail_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detail_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(detail_scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detail_scroll), detail);
  gtk_paned_set_end_child(GTK_PANED(paned2), detail_scroll);
  gtk_paned_set_resize_end_child(GTK_PANED(paned2), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned2), FALSE);

  gtk_window_present(GTK_WINDOW(st->dialog));
}
