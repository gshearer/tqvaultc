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

// Resolution helpers (ui_db_browser_index.c) reused by the detail renderers.
char *db_item_display_name(DbBrowserState *st, const char *path);
const char *db_skill_effective_path(const char *path, int depth);
int db_skill_max_level(const char *path, int depth);
char *db_creature_display_name(DbBrowserState *st, DbCreature *c);
const char *db_set_member_name(DbBrowserState *st, const char *member_path);
const char *db_creature_color(const char *classification);

// Detail renderer (ui_db_browser_detail.c).
void update_detail(DbBrowserState *st, DbBrowseItem *bi);

#endif
