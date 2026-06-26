#ifndef UI_DB_BROWSER_INTERNAL_H
#define UI_DB_BROWSER_INTERNAL_H

// Shared internals for the Database Browser, split across ui_db_browser.c
// (core/dialog/grid), ui_db_browser_index.c (categorization + index builders)
// and ui_db_browser_detail.c (detail-pane renderers).

#include <gtk/gtk.h>
#include <stdint.h>
#include <stdbool.h>
#include "ui.h"
#include "arz.h"
#include "db_creatures.h"
#include "db_quests.h"

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

// -- DbBrowseItem: GObject wrapping one item shown in the center grid -------

#define DB_TYPE_BROWSE_ITEM (db_browse_item_get_type())
G_DECLARE_FINAL_TYPE(DbBrowseItem, db_browse_item, DB, BROWSE_ITEM, GObject)

struct _DbBrowseItem {
  GObject parent_instance;
  char *path;        // full DBR path (also the item base_name)
  char *name;        // resolved display name (UTF-8)
  char *name_lc;     // lowercased name, for substring search
  char *search_blob; // lowercased plain-text of the whole detail card (name +
                     // every rendered stat), for keyword/content search; built
                     // by build_search_blobs(), persisted in the disk cache
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

// -- Dialog state -----------------------------------------------------------

typedef struct {
  AppWidgets *widgets;     // NULL on the window-less startup-build path
  TQTranslation *tr;       // translation table used by the index builders;
                           // == widgets->translations on the GUI path, or an
                           // independently-loaded table on the startup-build
                           // path (lets builders run without a window)
  GtkWidget *dialog;
  TQArzFile *arz;  // used only to enumerate record paths
  bool owns_arz;   // true only if we arz_load'd our own copy (fallback path)

  GListStore *cat_stores[CAT_COUNT];  // GListStore<DbBrowseItem> per category
  int cat_counts[CAT_COUNT];

  DbCreatureIndex *creatures;  // boss/hero loot index ("dropped by")
  DbQuestIndex    *quests;     // quest item-reward index ("reward from")

  GtkWidget *grid_view;
  GtkFilterListModel *filter_model;   // NULL-filter passthrough over the current
                                      // category store; the search highlights in
                                      // place rather than filtering, so this is
                                      // never used to hide rows
  GtkSingleSelection *selection;
  int  current_cat;                   // category shown in the grid (-1 == none
                                      // picked yet)
  SearchQuery *search_query;          // compiled matcher (regex/AND auto-detect)
                                      // over each item's search_blob; recompiled
                                      // on every search-changed, NULL/empty when
                                      // idle.  Drives the highlights (sidebar
                                      // category markers + per-cell outlines),
                                      // NOT filtering -- non-matches stay visible
  int  search_total_matches;          // # hits across all categories (search on)
  int  search_match_cats;             // # categories with >=1 hit (search on)

  // First time a creature category is opened we render+cache the whole bestiary
  // behind a progress popup (db_start_thumb_build_if_needed), since rendering on
  // the synchronous grid-bind path freezes the UI.  building_thumbs guards
  // against re-entry while it runs; thumbs_build_skipped is set if the user
  // cancels, so we don't re-prompt this session (cells then render lazily).
  bool building_thumbs;
  bool thumbs_build_skipped;

  // Sidebar + search, retained so detail-pane cross-reference links can switch
  // category, clear the filter and select the jump target (Phase 7).
  GtkWidget *sidebar;                  // GtkListBox of categories
  GtkWidget *sidebar_rows[CAT_COUNT];  // selectable leaf row per category
  GtkWidget *search_entry;             // the name-filter GtkSearchEntry

  GtkWidget *detail_pic;              // GtkPicture: large icon
  GtkWidget *detail_label;            // GtkLabel: full formatted tooltip
  GtkWidget *count_label;             // "<n> items"

  // Dialog-local held-item overlay (the main window can't render the
  // cursor-attached item while the cursor is over this separate window).
  GtkWidget *dlg_held_overlay;
  double dlg_cursor_x, dlg_cursor_y;
} DbBrowserState;

// -- Cross-file entry points ------------------------------------------------

// Index builders (ui_db_browser_index.c), driven by the loader phases.
void build_category_index(DbBrowserState *st);
void build_set_index(DbBrowserState *st);
void build_affix_index(DbBrowserState *st);
void build_skill_index(DbBrowserState *st);
void build_creature_index_grid(DbBrowserState *st);
void build_quest_index_grid(DbBrowserState *st);

// Sort every category store for display: alphabetical by name, with same-named
// difficulty variants ordered legendary->epic->normal.  Skill categories keep
// their in-game tree order.  Run once after all stores are populated, on both
// the fresh-index and disk-cache paths.
void db_browser_sort_stores(DbBrowserState *st);

// Affix drill-down (ui_db_browser_index.c): return the concrete item DBR paths
// of equipment type `gear_label` (e.g. "Spear") that can roll the affix whose
// variant records are `variants` (NULL-terminated), with `kind` 0=prefix /
// 1=suffix.  Result is a newly-allocated GPtrArray of g_strdup'd normalized
// paths (deduped, unsorted); free with g_ptr_array_free(arr, TRUE).  Never NULL.
GPtrArray *db_affix_items_for_type(char *const *variants, int kind,
                                   const char *gear_label);

// Difficulty-tier rank from an item DBR filename's "_n_/_e_/_l_" infix:
// legendary=3, epic=2, normal=1, untiered=0.  Exposed for --db-sort-selftest.
int db_item_tier_rank(const char *path);

// Resolution helpers (ui_db_browser_index.c) reused by the detail renderers.
char *db_item_display_name(DbBrowserState *st, const char *path);
const char *db_skill_effective_path(const char *path, int depth);
int db_skill_max_level(const char *path, int depth);
// Resolve a skill's display name (recurses the buff/pet ref chain), with a
// path-derived fallback so it never returns NULL.  Caller frees (g_free).
char *db_skill_display_name(DbBrowserState *st, const char *path);
char *db_creature_display_name(DbBrowserState *st, DbCreature *c);
const char *db_set_member_name(DbBrowserState *st, const char *member_path);
const char *db_creature_color(const char *classification);

// Detail renderer (ui_db_browser_detail.c).
void update_detail(DbBrowserState *st, DbBrowseItem *bi);

// Render the affix drill-down: the concrete items of equipment type
// `gear_label` that can roll affix `bi` (a prefix/suffix DbBrowseItem), each a
// clickable jump-to-item link, plus a Back link.  (ui_db_browser_detail.c)
void update_detail_affix_items(DbBrowserState *st, DbBrowseItem *bi,
                               const char *gear_label);

// Populate every indexed item's search_blob (lowercased plain text of its whole
// detail card).  Window-less (uses st->tr only); run after all category indexes
// are built, on both the GUI loader and the startup-build paths.
void build_search_blobs(DbBrowserState *st);

#endif
