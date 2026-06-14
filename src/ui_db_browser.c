// ui_db_browser.c -- tq-db-style Database Browser: core, dialog and grid.
//
// A curated, categorized item browser modeled on tq-db.net.  Unlike the raw
// Database Explorer (ui_database_dialog.c, a path-tree + variable inspector),
// this presents the game's items grouped by type, each rendered with its icon,
// rarity-colored name and full in-game-style tooltip -- and lets the user
// right-click an item to pick it up and drop it into a vault.  Browsable
// groups: item bases (weapons/armor/jewelry/relics/charms/artifacts/scrolls),
// Sets, Affixes (Prefixes/Suffixes), Skills, and creature / quest-reward views.
//
// The browser is split across three translation units that share
// ui_db_browser_internal.h (the DbBrowseItem GObject + DbBrowserState):
//   * ui_db_browser.c        -- this file: the DbBrowseItem GObject impl, the
//                               loading dialog, the held-item overlay, and the
//                               grid / search / sidebar wiring.
//   * ui_db_browser_index.c  -- categorization + the per-category index builders.
//   * ui_db_browser_detail.c -- the detail-pane renderers.
//
// All formatting reuses the existing C engine (item_stats.c, get_item_color,
// load_item_texture); nothing is pre-baked.  See AGENTS.md "Database Browser".

#include "ui_db_browser_internal.h"
#include "db_browser_cache.h"

#include "config.h"
#include "asset_lookup.h"
#include "affix_table.h"
#include "texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// -- Category model ---------------------------------------------------------

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

// -- DbBrowseItem: GObject wrapping one item shown in the center grid -------
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


// -- Grid selection -> detail ----------------------------------------------

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
  st->sidebar = list;

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
    st->sidebar_rows[cat] = row;
  }

  g_signal_connect(list, "row-selected",
                   G_CALLBACK(on_sidebar_row_selected), st);

  return(list);
}

// -- Cross-pane navigation (detail-pane links) ------------------------------
//
// Cross-reference rows in the detail pane are rendered as Pango <a href> links
// whose URI encodes the jump target: "i:<dbr-path>" -> an item (searched across
// all category stores), "c:<idx>" -> a creature row, "q:<idx>" -> a quest row.
// Clicking one switches category, clears the filter and selects/scrolls to it.

// Find the grid item in category `cat` whose creature/quest source index equals
// `src_idx`.  Returns a new reference (caller g_object_unref's), or NULL.
static DbBrowseItem *
db_find_grid_item_by_src(DbBrowserState *st, int cat, int src_idx)
{
  GListModel *m = G_LIST_MODEL(st->cat_stores[cat]);
  guint n = g_list_model_get_n_items(m);

  for(guint i = 0; i < n; i++)
  {
    DbBrowseItem *bi = g_list_model_get_item(m, i);  // owns a ref

    if(bi->src_idx == src_idx)
      return(bi);  // transfer the ref to the caller
    g_object_unref(bi);
  }

  return(NULL);
}

// Find the browse item carrying the given DBR path across every category store
// (item paths are unique to one store).  Returns a new reference and writes the
// owning category to *out_cat, or NULL if no browsable item has that path (e.g.
// a set member or drop that didn't pass categorization).
static DbBrowseItem *
db_find_item_by_path(DbBrowserState *st, const char *path, int *out_cat)
{
  for(int c = 0; c < CAT_COUNT; c++)
  {
    GListModel *m = G_LIST_MODEL(st->cat_stores[c]);
    guint n = g_list_model_get_n_items(m);

    for(guint i = 0; i < n; i++)
    {
      DbBrowseItem *bi = g_list_model_get_item(m, i);  // owns a ref

      if(bi->path && g_ascii_strcasecmp(bi->path, path) == 0)
      {
        *out_cat = c;
        return(bi);  // transfer the ref to the caller
      }
      g_object_unref(bi);
    }
  }

  return(NULL);
}

// Jump the browser to `target` in category `cat`: switch the sidebar/grid to
// that category, clear any active name filter so the target is visible, then
// select it and scroll it into view (which fires the detail-pane update).
static void
db_jump_select(DbBrowserState *st, int cat, DbBrowseItem *target)
{
  // Clear the name filter so the target can't be filtered out.  Reset the
  // needle and refresh the filter directly -- GtkSearchEntry's "search-changed"
  // is debounced, so editing the entry alone wouldn't take effect in time.
  st->search_lc[0] = '\0';
  if(st->search_entry)
    gtk_editable_set_text(GTK_EDITABLE(st->search_entry), "");
  gtk_filter_changed(GTK_FILTER(st->custom_filter), GTK_FILTER_CHANGE_LESS_STRICT);

  // Switch category via the sidebar so its highlight stays in sync; selecting
  // the row fires on_sidebar_row_selected -> show_category synchronously.  If
  // that category is already selected, switch the grid model directly.
  GtkListBoxRow *want = GTK_LIST_BOX_ROW(st->sidebar_rows[cat]);

  if(st->sidebar && want &&
     gtk_list_box_get_selected_row(GTK_LIST_BOX(st->sidebar)) != want)
    gtk_list_box_select_row(GTK_LIST_BOX(st->sidebar), want);
  else
    show_category(st, cat);

  // Locate the target in the (now unfiltered) filter model and select it.
  GListModel *m = G_LIST_MODEL(st->filter_model);
  guint n = g_list_model_get_n_items(m);

  for(guint i = 0; i < n; i++)
  {
    DbBrowseItem *bi = g_list_model_get_item(m, i);

    g_object_unref(bi);  // borrow; the store keeps it alive
    if(bi == target)
    {
      gtk_single_selection_set_selected(st->selection, i);
      gtk_grid_view_scroll_to(GTK_GRID_VIEW(st->grid_view), i,
                              GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS,
                              NULL);
      return;
    }
  }
}

// GtkLabel::activate-link handler for the detail pane.  Routes the private URI
// schemes above; always returns TRUE so the URI is never handed to
// gtk_show_uri() (which would try to open it as an external link).
static gboolean
on_detail_link(GtkLabel *label, const char *uri, gpointer data)
{
  (void)label;

  DbBrowserState *st = data;

  if(!uri || !uri[0] || uri[1] != ':')
    return(TRUE);

  if(uri[0] == 'i')
  {
    int cat = -1;
    DbBrowseItem *t = db_find_item_by_path(st, uri + 2, &cat);

    if(t)
    {
      db_jump_select(st, cat, t);
      g_object_unref(t);
    }
  }
  else if(uri[0] == 'c')
  {
    DbBrowseItem *t = db_find_grid_item_by_src(st, CAT_CREATURE, atoi(uri + 2));

    if(t)
    {
      db_jump_select(st, CAT_CREATURE, t);
      g_object_unref(t);
    }
  }
  else if(uri[0] == 'q')
  {
    DbBrowseItem *t = db_find_grid_item_by_src(st, CAT_QUEST, atoi(uri + 2));

    if(t)
    {
      db_jump_select(st, CAT_QUEST, t);
      g_object_unref(t);
    }
  }

  return(TRUE);
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
  st->tr = widgets->translations;
  st->dlg_cursor_x = -10000.0;
  st->dlg_cursor_y = -10000.0;

  // Cheap to create; populated either from the disk cache (fast path) or by
  // the indexing phases below.
  for(int i = 0; i < CAT_COUNT; i++)
    st->cat_stores[i] = g_list_store_new(DB_TYPE_BROWSE_ITEM);

  // Fast path: a valid on-disk cache (built once at startup, see Step 4) lets
  // us skip the ~4s index build and the progress dialog entirely — populate
  // the stores from disk and present the window immediately.
  if(db_browser_cache_load(st, global_config.game_folder))
  {
    // Keep the shared database handle around for any record lookups the
    // UI/detail might do; it's never freed (owns_arz == false) and may be NULL
    // on installs where asset_get_database_arz() can't match (the cache build
    // path doesn't need it).
    st->arz = asset_get_database_arz();
    st->owns_arz = false;
    db_browser_build_ui(st);
    return;
  }

  // Fallback (missing/stale/corrupt cache): show the progress dialog and index
  // in phases from the idle loop so the UI never appears frozen.
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

  st->search_entry = search;
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
  g_signal_connect(st->detail_label, "activate-link",
                   G_CALLBACK(on_detail_link), st);
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

// ── First-run / cache-rebuild setup popup ─────────────────────────────────
//
// When the Database Browser cache is missing or stale we build it once at
// startup, bundled with the resource-index build, behind a single modal
// "Setting up TQVaultC…" progress bar.  After that the browser opens instantly
// (the fast path in show_db_browser_dialog).  Driven from the idle loop in the
// same two-pass (announce, then work) style as the in-browser loader, so the
// bar repaints between steps even though each step blocks the main loop.

typedef struct
{
  GtkApplication *app;
  GtkWidget      *win;       // modal progress window
  GtkWidget      *bar;       // GtkProgressBar
  GtkWidget      *status;    // status label
  DbBrowserState *st;        // throwaway build state (freed after save)
  TQTranslation  *tr;        // throwaway translations (freed after save)
  int             phase;     // next phase to run
  gboolean        announced; // current phase's label has been shown
} StartupLoader;

// Step A: the synchronous init tail (resource-index build happens inside
// asset_manager_init) plus a throwaway translation table + build state.
static void
startup_step_init(StartupLoader *sl)
{
  asset_manager_init(global_config.game_folder);
  arz_intern_init();
  item_stats_init();
  affix_table_init(NULL);

  // The index builders need a translation table; the real UI loads its own
  // later inside ui_app_activate, so this one is temporary.
  sl->tr = translation_init();
  if(sl->tr)
  {
    char p[1024];

    snprintf(p, sizeof(p), "%s/Text/Text_EN.arc", global_config.game_folder);
    translation_load_from_arc(sl->tr, p);
  }

  sl->st = g_new0(DbBrowserState, 1);
  sl->st->tr = sl->tr;
  // Mirror db_load_arz(): shared handle if available, else our own copy.
  sl->st->arz = asset_get_database_arz();
  sl->st->owns_arz = false;
  if(!sl->st->arz)
  {
    char arz_path[1024];

    snprintf(arz_path, sizeof(arz_path), "%s/Database/database.arz",
             global_config.game_folder);
    sl->st->arz = arz_load(arz_path);
    sl->st->owns_arz = true;
  }
  for(int i = 0; i < CAT_COUNT; i++)
    sl->st->cat_stores[i] = g_list_store_new(DB_TYPE_BROWSE_ITEM);
}

// Steps B: one index builder each (thin wrappers so the phase table can carry
// a uniform StartupLoader* signature).
static void startup_step_categories(StartupLoader *sl){ build_category_index(sl->st); }
static void startup_step_sets(StartupLoader *sl)      { build_set_index(sl->st); }
static void startup_step_affixes(StartupLoader *sl)   { build_affix_index(sl->st); }
static void startup_step_skills(StartupLoader *sl)    { build_skill_index(sl->st); }
static void startup_step_creatures(StartupLoader *sl) { build_creature_index_grid(sl->st); }
static void startup_step_quests(StartupLoader *sl)    { build_quest_index_grid(sl->st); }

// Step C: persist the cache, then drop the throwaway build state.
static void
startup_step_save(StartupLoader *sl)
{
  if(!db_browser_cache_save(sl->st, global_config.game_folder))
    g_warning("startup: failed to save Database Browser cache");

  db_browser_state_free(sl->st);   // unrefs stores, frees creatures/quests/arz
  sl->st = NULL;
  if(sl->tr)
  {
    translation_free(sl->tr);
    sl->tr = NULL;
  }
}

// Phase table: label shown while the step runs + the bar position to settle on
// once it finishes.  Step A is the resource-index build (the "fake" 0→10%),
// steps B animate 10→90%, step C (save) lands 90→100%.
static const struct
{
  const char *label;
  void      (*fn)(StartupLoader *sl);
  double      frac;
}
STARTUP_PHASES[] =
{
  { "Preparing game database…",   startup_step_init,      0.10 },
  { "Indexing equipment…",        startup_step_categories,0.25 },
  { "Indexing item sets…",        startup_step_sets,      0.35 },
  { "Indexing affixes…",          startup_step_affixes,   0.50 },
  { "Indexing skills…",           startup_step_skills,    0.65 },
  { "Indexing creatures…",        startup_step_creatures, 0.85 },
  { "Indexing quest rewards…",    startup_step_quests,    0.90 },
  { "Saving cache…",              startup_step_save,      1.00 },
};

#define STARTUP_NPHASES ((int)G_N_ELEMENTS(STARTUP_PHASES))

static gboolean
startup_build_step(gpointer data)
{
  StartupLoader *sl = data;

  // All phases done: bring up the real UI first (so the application keeps a
  // window), then drop the popup.
  if(sl->phase >= STARTUP_NPHASES)
  {
    GtkApplication *app = sl->app;
    GtkWidget      *win = sl->win;

    g_free(sl);
    ui_app_activate(app, NULL);
    gtk_window_destroy(GTK_WINDOW(win));
    return(G_SOURCE_REMOVE);
  }

  // First pass: show this phase's label, then yield so the bar repaints before
  // the blocking work below.
  if(!sl->announced)
  {
    gtk_label_set_text(GTK_LABEL(sl->status), STARTUP_PHASES[sl->phase].label);
    sl->announced = TRUE;
    return(G_SOURCE_CONTINUE);
  }

  // Second pass: do the work, advance the bar.
  STARTUP_PHASES[sl->phase].fn(sl);
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(sl->bar),
                                STARTUP_PHASES[sl->phase].frac);
  sl->phase++;
  sl->announced = FALSE;
  return(G_SOURCE_CONTINUE);
}

// Build and present the modal setup window, then start the idle build.
static void
startup_build_show(GtkApplication *app)
{
  StartupLoader *sl = g_new0(StartupLoader, 1);

  sl->app = app;
  sl->win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(sl->win), "TQVaultC");
  gtk_window_set_default_size(GTK_WINDOW(sl->win), 420, -1);
  gtk_window_set_resizable(GTK_WINDOW(sl->win), FALSE);
  gtk_window_set_deletable(GTK_WINDOW(sl->win), FALSE);
  // Register with the application so it's the live window keeping the app
  // running until the main window is created (no main window exists yet).
  gtk_window_set_application(GTK_WINDOW(sl->win), app);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);

  GtkWidget *title = gtk_label_new(NULL);

  gtk_label_set_markup(GTK_LABEL(title), "<b>Setting up TQVaultC…</b>");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *sub = gtk_label_new("First-time setup — this happens once.");

  gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
  gtk_widget_add_css_class(sub, "dim-label");
  gtk_box_append(GTK_BOX(box), sub);

  sl->status = gtk_label_new("Preparing…");
  gtk_label_set_xalign(GTK_LABEL(sl->status), 0.0f);
  gtk_widget_add_css_class(sl->status, "dim-label");
  gtk_box_append(GTK_BOX(box), sl->status);

  sl->bar = gtk_progress_bar_new();
  gtk_box_append(GTK_BOX(box), sl->bar);

  gtk_window_set_child(GTK_WINDOW(sl->win), box);
  gtk_window_present(GTK_WINDOW(sl->win));

  g_idle_add(startup_build_step, sl);
}

void
ui_startup_init_and_activate(GtkApplication *app)
{
  // No game folder: nothing to initialize (shouldn't normally happen here).
  if(!global_config.game_folder)
  {
    ui_app_activate(app, NULL);
    return;
  }

  // Fast path: a valid browser cache means init is cheap (just loads the
  // existing resource index) and the browser will open instantly — no popup.
  if(db_browser_cache_valid(global_config.game_folder))
  {
    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);
    ui_app_activate(app, NULL);
    return;
  }

  // Slow path (first run / game updated / format bumped): build the resource
  // index and the browser cache behind a one-time progress popup.
  startup_build_show(app);
}
