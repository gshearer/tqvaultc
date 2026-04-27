// ui_database_dialog.c -- Database Explorer dialog
//
// Lets users browse the game's database.arz file in a tree view,
// with variable inspection for selected records.

#include "ui.h"
#include "arz.h"
#include "config.h"
#include "item_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// -- DbRecordItem: GObject wrapping a record-tree node in the left pane ----

#define DB_TYPE_RECORD_ITEM (db_record_item_get_type())
G_DECLARE_FINAL_TYPE(DbRecordItem, db_record_item, DB, RECORD_ITEM, GObject)

struct _DbRecordItem {
  GObject parent_instance;
  char *name;            // display name (directory component or filename)
  char *full_path;       // full record path; NULL for directories
  gboolean is_leaf;      // TRUE for .dbr records, FALSE for directories
  GListStore *children;  // GListStore<DbRecordItem>; NULL for leaves
  gboolean matches_cache;  // result of latest filter recomputation
};

G_DEFINE_FINAL_TYPE(DbRecordItem, db_record_item, G_TYPE_OBJECT)

static void
db_record_item_finalize(GObject *object)
{
  DbRecordItem *self = DB_RECORD_ITEM(object);

  g_free(self->name);
  g_free(self->full_path);
  g_clear_object(&self->children);
  G_OBJECT_CLASS(db_record_item_parent_class)->finalize(object);
}

static void
db_record_item_class_init(DbRecordItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = db_record_item_finalize;
}

static void
db_record_item_init(DbRecordItem *self)
{
  (void)self;
}

// Construct a new DbRecordItem.  Directory items get an empty children store;
// leaves get NULL children.
//
// name:      display string (component or filename)
// full_path: full path for leaves; NULL for directories
// is_leaf:   TRUE for .dbr files
// returns: new DbRecordItem with refcount 1
static DbRecordItem *
db_record_item_new(const char *name, const char *full_path, gboolean is_leaf)
{
  DbRecordItem *r = g_object_new(DB_TYPE_RECORD_ITEM, NULL);

  r->name = g_strdup(name ? name : "");
  r->full_path = full_path ? g_strdup(full_path) : NULL;
  r->is_leaf = is_leaf;
  r->children = is_leaf ? NULL : g_list_store_new(DB_TYPE_RECORD_ITEM);
  r->matches_cache = TRUE;
  return(r);
}

// -- VarItem: GObject wrapping a single variable row in the right pane ------

#define VAR_TYPE_ITEM (var_item_get_type())
G_DECLARE_FINAL_TYPE(VarItem, var_item, VAR, ITEM, GObject)

struct _VarItem {
  GObject parent_instance;
  char *name;
  char *type;
  char *value;
};

G_DEFINE_FINAL_TYPE(VarItem, var_item, G_TYPE_OBJECT)

static void
var_item_finalize(GObject *object)
{
  VarItem *self = VAR_ITEM(object);

  g_free(self->name);
  g_free(self->type);
  g_free(self->value);
  G_OBJECT_CLASS(var_item_parent_class)->finalize(object);
}

static void
var_item_class_init(VarItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = var_item_finalize;
}

static void
var_item_init(VarItem *self)
{
  (void)self;
}

// Construct a new VarItem with copies of the three strings.
//
// name:  variable name (NULL treated as empty)
// type:  type name (NULL treated as empty)
// value: formatted value string (NULL treated as empty)
// returns: new VarItem with refcount 1
static VarItem *
var_item_new(const char *name, const char *type, const char *value)
{
  VarItem *vi = g_object_new(VAR_TYPE_ITEM, NULL);

  vi->name  = g_strdup(name  ? name  : "");
  vi->type  = g_strdup(type  ? type  : "");
  vi->value = g_strdup(value ? value : "");
  return(vi);
}

// -- Dialog state -----------------------------------------------------------

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  GtkWidget *list_view;
  GListStore *root_store;          // GListStore<DbRecordItem>
  GtkTreeListModel *tree_list_model;
  GtkFilterListModel *filter_model;
  GtkCustomFilter *custom_filter;
  GtkSingleSelection *selection;
  GtkWidget *var_column_view;
  GListStore *var_store;
  GtkWidget *path_label;
  GtkWidget *search_entry;
  char search_text[256];
  TQArzFile *arz;
  bool arz_owned;  // true if we loaded it ourselves (must free)

  // Held-item rendering inside the dialog: the main window's held_overlay
  // can't draw the cursor-attached item while the cursor is over this
  // separate window, so we mirror that overlay locally.
  GtkWidget *dlg_held_overlay;
  double dlg_cursor_x, dlg_cursor_y;  // off-screen until cursor enters
} DatabaseDialogState;

// Free the dialog state, releasing the arz file if we own it.
//
// data: pointer to DatabaseDialogState
static void
database_state_free(gpointer data)
{
  DatabaseDialogState *st = data;

  // Release left-pane model refs we kept for cache walks / filter changes.
  // The widgets themselves hold separate refs and clean up via GTK destroy.
  g_clear_object(&st->root_store);
  g_clear_object(&st->custom_filter);

  // var_store: we kept an extra ref so we could still mutate it after handing
  // ownership to GtkNoSelection. Drop it now.
  g_clear_object(&st->var_store);

  if(st->arz_owned && st->arz)
    arz_free(st->arz);

  g_free(st);
}

// -- Type name for display --------------------------------------------------

// Return a human-readable type name string for a TQVarType.
//
// t: the variable type enum
// returns: static string ("int", "float", "string", or "bool")
static const char *
type_name(TQVarType t)
{
  switch(t)
  {
    case TQ_VAR_INT:    return("int");
    case TQ_VAR_FLOAT:  return("float");
    case TQ_VAR_STRING: return("string");
    default:            return("bool");
  }
}

// -- Format a variable value as a string ------------------------------------

// Format a TQVariable's value(s) into a semicolon-separated string.
// Caller must free() the returned string.
//
// var: the variable to format
// returns: heap-allocated string (empty string on failure)
static char *
format_variable(const TQVariable *var)
{
  if(!var || var->count == 0)
    return(strdup(""));

  // arz parser doesn't handle type=3 (bool), leaving value pointer NULL
  if(!var->value.i32)
    return(strdup("0"));

  // Estimate buffer size
  size_t bufsz = var->count * 64 + 1;
  char *buf = malloc(bufsz);

  if(!buf)
    return(strdup(""));

  buf[0] = '\0';
  size_t pos = 0;

  for(uint32_t i = 0; i < var->count; i++)
  {
    if(i > 0 && pos < bufsz - 2)
    {
      buf[pos++] = ';';
      buf[pos] = '\0';
    }

    char tmp[256];

    switch(var->type)
    {
      case TQ_VAR_INT:
        snprintf(tmp, sizeof(tmp), "%d", var->value.i32[i]);
        break;
      case TQ_VAR_FLOAT:
        snprintf(tmp, sizeof(tmp), "%.6g", (double)var->value.f32[i]);
        break;
      case TQ_VAR_STRING:
        snprintf(tmp, sizeof(tmp), "%s",
                 var->value.str[i] ? var->value.str[i] : "");
        break;
      default:
        snprintf(tmp, sizeof(tmp), "?");
        break;
    }

    size_t tlen = strlen(tmp);

    if(pos + tlen < bufsz)
    {
      memcpy(buf + pos, tmp, tlen + 1);
      pos += tlen;
    }
  }

  return(buf);
}

// -- Sort comparator for record indices -------------------------------------

static TQArzRecord *s_sort_records;  // qsort context (no user data in qsort)

// Compare two record indices by their path strings (case-insensitive).
// Used as qsort comparator.
//
// a, b: pointers to uint32_t record indices
// returns: negative/zero/positive for ordering
static int
cmp_record_indices(const void *a, const void *b)
{
  const char *pa = s_sort_records[*(const uint32_t *)a].path;
  const char *pb = s_sort_records[*(const uint32_t *)b].path;

  if(!pa && !pb)
    return(0);

  if(!pa)
    return(1);

  if(!pb)
    return(-1);

  return(strcasecmp(pa, pb));
}

// -- Build the tree from arz records ----------------------------------------

// Populate the root GListStore with directory/file nodes from arz records.
// Records are sorted alphabetically by path and inserted into a tree of
// nested GListStores: directories are parents, .dbr files are leaves.
//
// The dir_map hashtable maps each accumulated path prefix to a borrowed
// DbRecordItem* (parent owns the ref via its children GListStore), so we get
// O(1) parent lookup without re-walking the tree.
//
// st: dialog state containing st->root_store and arz file
static void
build_database_tree(DatabaseDialogState *st)
{
  TQArzFile *arz = st->arz;

  if(!arz || !st->root_store)
    return;

  // Sort record indices by path for alphabetical tree order
  uint32_t *sorted = malloc(arz->num_records * sizeof(uint32_t));

  if(!sorted)
    return;

  for(uint32_t i = 0; i < arz->num_records; i++)
    sorted[i] = i;

  s_sort_records = arz->records;
  qsort(sorted, arz->num_records, sizeof(uint32_t), cmp_record_indices);

  // Map path prefix -> borrowed DbRecordItem* (parent retains the ref).
  // Values are NOT owned by the hashtable (no value_destroy_func).
  GHashTable *dir_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);

  for(uint32_t ri = 0; ri < arz->num_records; ri++)
  {
    uint32_t r = sorted[ri];
    const char *path = arz->records[r].path;

    if(!path)
      continue;

    // Split path by backslash
    // e.g. "records\creature\monster\skeleton\am_minion_02.dbr"
    DbRecordItem *parent_item = NULL;  // NULL means "append to root_store"

    // Walk each component, creating directories as needed
    const char *p = path;
    char prefix[1024];

    prefix[0] = '\0';
    size_t prefix_len = 0;

    while(*p)
    {
      // Find next separator
      const char *sep = strchr(p, '\\');

      if(!sep)
      {
        // This is the leaf (filename)
        DbRecordItem *leaf = db_record_item_new(p, path, TRUE);
        GListStore *store = parent_item ? parent_item->children
                                         : st->root_store;

        g_list_store_append(store, leaf);
        g_object_unref(leaf);
        break;
      }

      // Directory component
      size_t comp_len = (size_t)(sep - p);

      if(prefix_len + comp_len + 1 >= sizeof(prefix))
        break;

      if(prefix_len > 0)
        prefix[prefix_len++] = '\\';

      memcpy(prefix + prefix_len, p, comp_len);
      prefix_len += comp_len;
      prefix[prefix_len] = '\0';

      // Check if this directory already exists
      DbRecordItem *cached = g_hash_table_lookup(dir_map, prefix);

      if(cached)
      {
        parent_item = cached;
      }
      else
      {
        // Extract just the directory name for display
        char comp_name[256];

        if(comp_len >= sizeof(comp_name))
          comp_len = sizeof(comp_name) - 1;

        memcpy(comp_name, p, comp_len);
        comp_name[comp_len] = '\0';

        DbRecordItem *dir = db_record_item_new(comp_name, NULL, FALSE);
        GListStore *store = parent_item ? parent_item->children
                                         : st->root_store;

        g_list_store_append(store, dir);
        g_object_unref(dir);  // store retains the ref

        // Cache borrowed pointer (parent's GListStore owns the ref)
        g_hash_table_insert(dir_map, g_strdup(prefix), dir);
        parent_item = dir;
      }

      p = sep + 1;
    }
  }

  g_hash_table_destroy(dir_map);
  free(sorted);
}

// -- Tree selection changed: show record variables --------------------------

// Called when GtkSingleSelection's selected-item property changes.  If a
// leaf node (.dbr record) is selected, reads its variables and populates the
// variable list store.
//
// obj:  the GtkSingleSelection (cast back below)
// pspec: ignored
// data: DatabaseDialogState pointer
static void
on_selection_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
  (void)pspec;

  DatabaseDialogState *st = data;
  GtkSingleSelection *selection = GTK_SINGLE_SELECTION(obj);

  g_list_store_remove_all(st->var_store);
  gtk_label_set_text(GTK_LABEL(st->path_label), "");

  // Selected item is the GtkTreeListRow* (passthrough=FALSE).  Not transferred.
  GObject *selected = gtk_single_selection_get_selected_item(selection);

  if(!selected)
    return;

  GtkTreeListRow *row = GTK_TREE_LIST_ROW(selected);
  // gtk_tree_list_row_get_item is transfer-full — must unref.
  DbRecordItem *r = DB_RECORD_ITEM(gtk_tree_list_row_get_item(row));

  if(!r)
    return;

  if(!r->is_leaf || !r->full_path)
  {
    g_object_unref(r);
    return;
  }

  // Show path in status bar
  gtk_label_set_text(GTK_LABEL(st->path_label), r->full_path);

  // Read and display record variables
  TQArzRecordData *rec = arz_read_record(st->arz, r->full_path);

  if(!rec)
  {
    g_object_unref(r);
    return;
  }

  for(uint32_t i = 0; i < rec->num_vars; i++)
  {
    const TQVariable *var = &rec->vars[i];
    char *val_str = format_variable(var);
    VarItem *vi = var_item_new(var->name, type_name(var->type), val_str);

    g_list_store_append(st->var_store, vi);
    g_object_unref(vi);
    free(val_str);
  }

  arz_record_data_free(rec);
  g_object_unref(r);
}

// -- Search / filter --------------------------------------------------------

// Recompute matches_cache on every node in the tree rooted at the given
// GListStore.  A leaf matches if its full_path contains the (lowercased)
// needle; a directory matches if any descendant matches.  Empty needle =>
// everything matches.
//
// Always walks the full tree (we must reset stale FALSE caches even when
// clearing the search).  ~10k records → comfortably under a millisecond.
//
// store: GListStore<DbRecordItem> to walk
// needle: lowercased substring to search for ("" treats all as matched)
// returns: TRUE if any item in this store (or its descendants) matches
static gboolean
recompute_match_cache(GListStore *store, const char *needle)
{
  gboolean any_match = FALSE;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));

  for(guint i = 0; i < n; i++)
  {
    // g_list_model_get_item is transfer-full
    DbRecordItem *r = DB_RECORD_ITEM(
        g_list_model_get_item(G_LIST_MODEL(store), i));

    if(r->is_leaf)
    {
      if(needle[0] == '\0')
      {
        r->matches_cache = TRUE;
      }
      else
      {
        // Case-insensitive substring match.  Paths are ASCII.
        char lower_path[1024];
        size_t len = strlen(r->full_path);

        if(len >= sizeof(lower_path))
          len = sizeof(lower_path) - 1;

        for(size_t k = 0; k < len; k++)
          lower_path[k] = (char)tolower((unsigned char)r->full_path[k]);

        lower_path[len] = '\0';
        r->matches_cache = (strstr(lower_path, needle) != NULL);
      }
    }
    else
    {
      r->matches_cache = recompute_match_cache(r->children, needle);
    }

    if(r->matches_cache)
      any_match = TRUE;

    g_object_unref(r);
  }

  return(any_match);
}

// GtkCustomFilter callback.  Reads the precomputed matches_cache flag set
// by recompute_match_cache.  O(1) per row.
//
// item: GtkTreeListRow* (passthrough=FALSE on the tree list model)
// data: unused
// returns: TRUE if the row should be visible
static gboolean
filter_match(gpointer item, gpointer data)
{
  (void)data;

  GtkTreeListRow *row = GTK_TREE_LIST_ROW(item);
  // gtk_tree_list_row_get_item is transfer-full
  DbRecordItem *r = DB_RECORD_ITEM(gtk_tree_list_row_get_item(row));
  gboolean keep = r->matches_cache;

  g_object_unref(r);
  return(keep);
}

// Called when the search entry text changes.  Lowercases the search text,
// recomputes the match cache, and asks the filter to re-evaluate every row.
//
// entry: the search entry widget
// data:  DatabaseDialogState pointer
static void
on_search_changed(GtkSearchEntry *entry, gpointer data)
{
  DatabaseDialogState *st = data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

  // Lowercase the search text
  size_t len = strlen(text);

  if(len >= sizeof(st->search_text))
    len = sizeof(st->search_text) - 1;

  for(size_t i = 0; i < len; i++)
    st->search_text[i] = (char)tolower((unsigned char)text[i]);

  st->search_text[len] = '\0';

  recompute_match_cache(st->root_store, st->search_text);
  gtk_filter_changed(GTK_FILTER(st->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
}

// -- Variable column factory callbacks --------------------------------------

// Per-column field selector encoded into the factory's user_data via
// GUINT_TO_POINTER so a single bind callback can serve all three columns.
typedef enum {
  VAR_FIELD_NAME = 0,
  VAR_FIELD_TYPE = 1,
  VAR_FIELD_VALUE = 2,
} VarField;

// Factory setup: install a left-aligned GtkLabel as the list item's child.
//
// factory: the GtkSignalListItemFactory (unused)
// li:      the GtkListItem being prepared
// data:    unused
static void
var_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *li,
                  gpointer data)
{
  (void)factory;
  (void)data;

  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_list_item_set_child(li, label);
}

// Factory bind: pull the matching field from the bound VarItem into the
// item's GtkLabel.
//
// factory: the GtkSignalListItemFactory (unused)
// li:      the GtkListItem being bound
// data:    GUINT_TO_POINTER(VarField) selecting which string to display
static void
var_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *li,
                 gpointer data)
{
  (void)factory;

  VarField field = (VarField)GPOINTER_TO_UINT(data);
  GtkWidget *label = gtk_list_item_get_child(li);
  VarItem *vi = VAR_ITEM(gtk_list_item_get_item(li));
  const char *text = "";

  switch(field)
  {
    case VAR_FIELD_NAME:  text = vi->name;  break;
    case VAR_FIELD_TYPE:  text = vi->type;  break;
    case VAR_FIELD_VALUE: text = vi->value; break;
  }

  gtk_label_set_text(GTK_LABEL(label), text);
}

// Build a GtkColumnViewColumn driven by var_factory_setup/var_factory_bind
// for the given field.
//
// title:    column header text
// field:    which VarItem field this column displays
// returns: a new GtkColumnViewColumn (transfer-full)
static GtkColumnViewColumn *
var_column_new(const char *title, VarField field)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  g_signal_connect(factory, "setup", G_CALLBACK(var_factory_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(var_factory_bind),
                   GUINT_TO_POINTER((guint)field));

  // gtk_column_view_column_new is transfer-full of the factory.
  GtkColumnViewColumn *col = gtk_column_view_column_new(title, factory);

  gtk_column_view_column_set_resizable(col, TRUE);
  return(col);
}

// -- Record-tree factory & child-model callbacks ---------------------------

// GtkTreeListModel child-model callback.  Returns a new ref to the child
// GListStore for directory items (the model takes the ref); NULL for leaves
// (no expander shown).
//
// item: pointer to the parent DbRecordItem
// data: unused
// returns: ref to GListModel for non-leaves, NULL for leaves
static GListModel *
create_child_model(gpointer item, gpointer data)
{
  (void)data;

  DbRecordItem *r = DB_RECORD_ITEM(item);

  if(r->is_leaf || !r->children)
    return(NULL);

  return(G_LIST_MODEL(g_object_ref(r->children)));
}

// List-view factory setup: install a GtkTreeExpander wrapping a GtkLabel.
//
// factory: unused
// li:      the GtkListItem being prepared
// data:    unused
static void
record_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *li,
                     gpointer data)
{
  (void)factory;
  (void)data;

  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);

  GtkWidget *expander = gtk_tree_expander_new();

  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), label);
  gtk_list_item_set_child(li, expander);
}

// List-view factory bind: hook expander to the row, pull display name from
// the underlying DbRecordItem.
//
// factory: unused
// li:      the GtkListItem being bound (item is GtkTreeListRow*)
// data:    unused
static void
record_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *li,
                    gpointer data)
{
  (void)factory;
  (void)data;

  GtkTreeExpander *expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(li));
  GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(li));

  gtk_tree_expander_set_list_row(expander, row);

  // gtk_tree_list_row_get_item is transfer-full
  DbRecordItem *r = DB_RECORD_ITEM(gtk_tree_list_row_get_item(row));
  GtkWidget *label = gtk_tree_expander_get_child(expander);

  gtk_label_set_text(GTK_LABEL(label), r->name);
  g_object_unref(r);
}

// List-view factory unbind: detach the row from the expander to drop refs.
//
// factory: unused
// li:      the GtkListItem being unbound
// data:    unused
static void
record_factory_unbind(GtkSignalListItemFactory *factory, GtkListItem *li,
                      gpointer data)
{
  (void)factory;
  (void)data;

  GtkTreeExpander *expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(li));

  gtk_tree_expander_set_list_row(expander, NULL);
}

// -- Dialog-local held-item overlay -----------------------------------------

// Draw the cursor-attached held item over the dialog content.  Mirrors
// held_overlay_draw_cb in ui_draw.c but uses dialog-local cursor coords so
// the item tracks the cursor while it's still over this window.
//
// da, w, h: standard GtkDrawingArea draw-func args (unused)
// cr:       cairo context
// ud:       DatabaseDialogState pointer
static void
dlg_held_overlay_draw_cb(GtkDrawingArea *da, cairo_t *cr,
                         int w, int h, gpointer ud)
{
  (void)da;
  (void)w;
  (void)h;

  DatabaseDialogState *st = ud;
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

// Capture-phase motion handler: tracks cursor in dialog overlay coordinates
// and queues a redraw of the held-item layer.
//
// ctrl:    motion controller (unused)
// x, y:    cursor position in overlay coordinates
// data:    DatabaseDialogState pointer
static void
on_dlg_overlay_motion(GtkEventControllerMotion *ctrl,
                      double x, double y, gpointer data)
{
  (void)ctrl;
  DatabaseDialogState *st = data;

  st->dlg_cursor_x = x;
  st->dlg_cursor_y = y;
  if(st->widgets->held_item && st->dlg_held_overlay)
    gtk_widget_queue_draw(st->dlg_held_overlay);
}

// Cursor left the dialog: park the dialog-overlay cursor off-screen so the
// held item stops rendering inside the (still-visible) dialog while the user
// drags it over the main window.
//
// ctrl: motion controller (unused)
// data: DatabaseDialogState pointer
static void
on_dlg_overlay_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
  (void)ctrl;
  DatabaseDialogState *st = data;

  st->dlg_cursor_x = -10000.0;
  st->dlg_cursor_y = -10000.0;
  if(st->widgets->held_item && st->dlg_held_overlay)
    gtk_widget_queue_draw(st->dlg_held_overlay);
}

// -- Right-click pickup: spawn a held item from a DBR record ---------------

// Decide whether a record describes an item that a vault entry's base_name
// could legitimately point to (equipment, relic/charm, artifact, oneshot).
// Filters out affix tables, loot pools, monsters, skills, and other
// non-item DBRs by inspecting the record's "Class" field.
//
// record_path: full DBR path
// returns: true if the DBR is a usable item base
static bool
db_record_is_item(const char *record_path)
{
  if(!record_path)
    return(false);

  // All equipment classes route through item_gear_type.
  if(item_gear_type(record_path) != 0)
    return(true);

  const char *cls = dbr_get_string(record_path, "Class");

  if(!cls)
    return(false);

  if(strcasecmp(cls, "ItemRelic")           == 0) return(true);
  if(strcasecmp(cls, "ItemCharm")           == 0) return(true);
  if(strcasecmp(cls, "ItemArtifact")        == 0) return(true);
  if(strcasecmp(cls, "ItemArtifactFormula") == 0) return(true);

  // OneShot_Scroll, OneShot_Potion*, OneShot_TomeOfSkill, etc.
  if(strncasecmp(cls, "OneShot_", 8) == 0)
    return(true);

  return(false);
}

// Build a fresh HeldItem from a DBR path and attach it to the cursor.
// The item has no affixes, no relics, a single stack count, and a random
// seed.  is_copy=true so cancel_held_item discards instead of trying to
// return-to-source.
//
// st: dialog state (for widgets)
// dbr_path: DBR path to use as the new item's base_name
static void
db_record_pickup(DatabaseDialogState *st, const char *dbr_path)
{
  AppWidgets *widgets = st->widgets;

  if(!dbr_path)
    return;

  // If the user is already holding something, return it to source first
  // (or discard if it was itself a copy).
  if(widgets->held_item)
    cancel_held_item(widgets);

  HeldItem *hi = calloc(1, sizeof(HeldItem));

  if(!hi)
    return;

  // Relics/charms render different textures based on var1 (shard count).
  // Spawn them at full completion so the user sees the finished texture.
  uint32_t var1 = 0;

  if(item_is_relic_or_charm(dbr_path))
  {
    int max_shards = relic_max_shards(dbr_path);

    if(max_shards > 0)
      var1 = (uint32_t)max_shards;
  }

  hi->item.seed       = (uint32_t)(rand() % 0x7fff);
  hi->item.base_name  = safe_strdup(dbr_path);
  hi->item.var1       = var1;
  hi->item.stack_size = 1;

  hi->source           = CONTAINER_VAULT;
  hi->source_sack_idx  = -1;
  hi->source_equip_slot = -1;
  hi->is_copy = true;

  get_item_dims(widgets, &hi->item, &hi->item_w, &hi->item_h);
  hi->texture = load_item_texture(widgets, hi->item.base_name, hi->item.var1);

  widgets->held_item = hi;

  // The dialog is a separate window, so on_overlay_motion in the main window
  // hasn't seen the cursor — win_cursor_x/y still hold the last main-window
  // position (often inside a vault cell, which makes the held item appear
  // dropped there).  Park the main-window render off-screen until the cursor
  // crosses into it; meanwhile the dialog's own overlay handles rendering.
  widgets->win_cursor_x = -10000.0;
  widgets->win_cursor_y = -10000.0;

  invalidate_tooltips(widgets);
  queue_redraw_all(widgets);
  if(st->dlg_held_overlay)
    gtk_widget_queue_draw(st->dlg_held_overlay);
}

// Right-click on a row in the record tree: if the row is a leaf .dbr that
// describes an item, attach a fresh copy to the cursor for drop into an
// inventory.  Walks up from the picked widget to find the GtkTreeExpander,
// then resolves the bound DbRecordItem.
//
// gesture: the GtkGestureClick (right button)
// n_press: ignored
// x, y:    coordinates within list_view
// data:    DatabaseDialogState pointer
static void
on_list_right_click(GtkGestureClick *gesture, int n_press,
                    double x, double y, gpointer data)
{
  (void)n_press;

  DatabaseDialogState *st = data;
  GtkWidget *picked = gtk_widget_pick(st->list_view, x, y, GTK_PICK_DEFAULT);

  while(picked && !GTK_IS_TREE_EXPANDER(picked))
    picked = gtk_widget_get_parent(picked);

  if(!picked)
    return;

  GtkTreeListRow *row = gtk_tree_expander_get_list_row(GTK_TREE_EXPANDER(picked));

  if(!row)
    return;

  // gtk_tree_list_row_get_item is transfer-full
  DbRecordItem *r = DB_RECORD_ITEM(gtk_tree_list_row_get_item(row));

  if(!r)
    return;

  if(r->is_leaf && r->full_path && db_record_is_item(r->full_path))
  {
    db_record_pickup(st, r->full_path);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  }

  g_object_unref(r);
}

// -- Show the database explorer dialog --------------------------------------

// Create and present the Database Explorer dialog window.  Loads the
// game's database.arz, builds a tree of records, and provides variable
// inspection and search filtering.
//
// widgets: application state
void
show_database_dialog(AppWidgets *widgets)
{
  if(!global_config.game_folder)
    return;

  DatabaseDialogState *st = g_new0(DatabaseDialogState, 1);

  st->widgets = widgets;

  // Load the arz -- try asset cache first, fall back to direct load
  char arz_path[1024];

  snprintf(arz_path, sizeof(arz_path), "%s/Database/database.arz",
           global_config.game_folder);

  st->arz = arz_load(arz_path);

  if(!st->arz)
  {
    g_free(st);
    return;
  }

  st->arz_owned = true;
  st->dlg_cursor_x = -10000.0;
  st->dlg_cursor_y = -10000.0;

  // Create dialog window
  st->dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(st->dialog), "Database Explorer");
  gtk_window_set_default_size(GTK_WINDOW(st->dialog), 1000, 700);
  gtk_window_set_transient_for(GTK_WINDOW(st->dialog),
                                GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(st->dialog), FALSE);
  g_object_set_data_full(G_OBJECT(st->dialog), "state", st,
                          database_state_free);

  // Top-level overlay holds the content + a transparent held-item layer so
  // an item picked up via right-click renders at the cursor while it's still
  // over this dialog window.
  GtkWidget *overlay = gtk_overlay_new();

  gtk_widget_set_hexpand(overlay, TRUE);
  gtk_widget_set_vexpand(overlay, TRUE);
  gtk_window_set_child(GTK_WINDOW(st->dialog), overlay);

  // Main vertical box
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_margin_start(vbox, 6);
  gtk_widget_set_margin_end(vbox, 6);
  gtk_widget_set_margin_top(vbox, 6);
  gtk_widget_set_margin_bottom(vbox, 6);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), vbox);

  // Transparent held-item drawing layer
  st->dlg_held_overlay = gtk_drawing_area_new();
  gtk_widget_set_hexpand(st->dlg_held_overlay, TRUE);
  gtk_widget_set_vexpand(st->dlg_held_overlay, TRUE);
  gtk_widget_set_can_target(st->dlg_held_overlay, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(st->dlg_held_overlay),
                                  dlg_held_overlay_draw_cb, st, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), st->dlg_held_overlay);

  // Capture-phase motion controller tracks the cursor for the held-item layer
  GtkEventController *dlg_motion = gtk_event_controller_motion_new();

  gtk_event_controller_set_propagation_phase(dlg_motion, GTK_PHASE_CAPTURE);
  g_signal_connect(dlg_motion, "motion",
                    G_CALLBACK(on_dlg_overlay_motion), st);
  g_signal_connect(dlg_motion, "leave",
                    G_CALLBACK(on_dlg_overlay_leave), st);
  gtk_widget_add_controller(overlay, dlg_motion);

  // Search entry
  st->search_entry = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(st->search_entry),
                                         "Filter records...");
  g_signal_connect(st->search_entry, "search-changed",
                   G_CALLBACK(on_search_changed), st);
  gtk_box_append(GTK_BOX(vbox), st->search_entry);

  // Horizontal paned: tree on left, variables on right
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

  gtk_paned_set_position(GTK_PANED(paned), 400);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_box_append(GTK_BOX(vbox), paned);

  // -- Left pane: record tree (modern GListStore + GtkListView) --

  st->root_store = g_list_store_new(DB_TYPE_RECORD_ITEM);

  // Build tree from arz records (populates st->root_store recursively)
  build_database_tree(st);

  // Tree-list model wraps the root store; passthrough=FALSE so items
  // downstream are GtkTreeListRow* (required by GtkTreeExpander).
  // gtk_tree_list_model_new is transfer-full of the root store — pass an
  // extra ref so we keep st->root_store valid for cache walks.
  st->tree_list_model = gtk_tree_list_model_new(
      G_LIST_MODEL(g_object_ref(st->root_store)),
      FALSE,  // passthrough
      FALSE,  // autoexpand
      create_child_model,
      st,
      NULL);

  // Custom filter reads matches_cache (precomputed in on_search_changed).
  // Keep our own ref so we can call gtk_filter_changed in on_search_changed.
  st->custom_filter = gtk_custom_filter_new(filter_match, st, NULL);
  g_object_ref(st->custom_filter);

  st->filter_model = gtk_filter_list_model_new(
      G_LIST_MODEL(st->tree_list_model),  // transfer-full
      GTK_FILTER(st->custom_filter));     // transfer-full

  st->selection = gtk_single_selection_new(
      G_LIST_MODEL(st->filter_model));    // transfer-full
  gtk_single_selection_set_autoselect(st->selection, FALSE);
  gtk_single_selection_set_can_unselect(st->selection, TRUE);

  // Plain g_signal_connect: when the dialog is destroyed, the GtkListView
  // (which owns the only ref to st->selection beyond ours-via-state-fields)
  // is disposed, which clears all signal handlers before database_state_free
  // runs to free `st`.  No use-after-free.
  g_signal_connect(st->selection, "notify::selected-item",
                   G_CALLBACK(on_selection_changed), st);

  // List view driven by selection.  Single GtkSignalListItemFactory with
  // setup/bind/unbind that drives a GtkTreeExpander wrapping a GtkLabel.
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  g_signal_connect(factory, "setup",   G_CALLBACK(record_factory_setup),  NULL);
  g_signal_connect(factory, "bind",    G_CALLBACK(record_factory_bind),   NULL);
  g_signal_connect(factory, "unbind",  G_CALLBACK(record_factory_unbind), NULL);

  st->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(st->selection),
                                     factory);
  // gtk_list_view_new is transfer-full of selection AND factory.

  // Right-click on a row spawns a held item for drop into an inventory.
  GtkGesture *list_right_click = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(list_right_click),
                                 GDK_BUTTON_SECONDARY);
  g_signal_connect(list_right_click, "pressed",
                   G_CALLBACK(on_list_right_click), st);
  gtk_widget_add_controller(st->list_view,
                             GTK_EVENT_CONTROLLER(list_right_click));

  GtkWidget *tree_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scroll),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree_scroll),
                                 st->list_view);
  gtk_paned_set_start_child(GTK_PANED(paned), tree_scroll);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

  // -- Right pane: variable list (modern GListStore + GtkColumnView) --

  st->var_store = g_list_store_new(VAR_TYPE_ITEM);

  // gtk_no_selection_new is transfer-full of the model — pass an extra ref so
  // st->var_store stays valid for our own remove_all/append calls.
  GtkNoSelection *var_sel = gtk_no_selection_new(
      G_LIST_MODEL(g_object_ref(st->var_store)));

  // gtk_column_view_new is transfer-full of the selection model.
  st->var_column_view = gtk_column_view_new(GTK_SELECTION_MODEL(var_sel));

  GtkColumnViewColumn *name_col = var_column_new("Variable", VAR_FIELD_NAME);
  GtkColumnViewColumn *type_col = var_column_new("Type", VAR_FIELD_TYPE);
  GtkColumnViewColumn *value_col = var_column_new("Value", VAR_FIELD_VALUE);

  gtk_column_view_column_set_fixed_width(name_col, 150);
  gtk_column_view_column_set_expand(value_col, TRUE);

  gtk_column_view_append_column(GTK_COLUMN_VIEW(st->var_column_view),
                                 name_col);
  gtk_column_view_append_column(GTK_COLUMN_VIEW(st->var_column_view),
                                 type_col);
  gtk_column_view_append_column(GTK_COLUMN_VIEW(st->var_column_view),
                                 value_col);
  g_object_unref(name_col);
  g_object_unref(type_col);
  g_object_unref(value_col);

  GtkWidget *var_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(var_scroll),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(var_scroll),
                                 st->var_column_view);
  gtk_paned_set_end_child(GTK_PANED(paned), var_scroll);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

  // -- Bottom path label --

  st->path_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(st->path_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(st->path_label), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(st->path_label), PANGO_ELLIPSIZE_START);
  gtk_box_append(GTK_BOX(vbox), st->path_label);

  // Record count in title
  char title[128];

  snprintf(title, sizeof(title), "Database Explorer (%u records)",
           st->arz->num_records);
  gtk_window_set_title(GTK_WINDOW(st->dialog), title);

  gtk_window_present(GTK_WINDOW(st->dialog));
}
