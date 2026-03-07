// ui_database_dialog.c — Database Explorer dialog
//
// Lets users browse the game's database.arz file in a tree view,
// with variable inspection for selected records.

#include "ui.h"
#include "arz.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// Tree store columns
enum {
  COL_NAME,       // display name (directory or filename)
  COL_FULL_PATH,  // full record path (only for leaf nodes)
  COL_IS_LEAF,    // TRUE for .dbr records, FALSE for directories
  TREE_N_COLS
};

// Variable list store columns
enum {
  VCOL_NAME,
  VCOL_TYPE,
  VCOL_VALUE,
  VAR_N_COLS
};

// ── Dialog state ────────────────────────────────────────────────────────

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  GtkWidget *tree_view;
  GtkTreeStore *tree_store;
  GtkTreeModel *filter_model;
  GtkWidget *var_tree_view;
  GtkListStore *var_store;
  GtkWidget *path_label;
  GtkWidget *search_entry;
  char search_text[256];
  TQArzFile *arz;
  bool arz_owned;  // true if we loaded it ourselves (must free)
} DatabaseDialogState;

static void database_state_free(gpointer data) {
  DatabaseDialogState *st = data;
  if (st->arz_owned && st->arz)
    arz_free(st->arz);
  g_free(st);
}

// ── Type name for display ────────────────────────────────────────────────

static const char *type_name(TQVarType t) {
  switch (t) {
    case TQ_VAR_INT:    return "int";
    case TQ_VAR_FLOAT:  return "float";
    case TQ_VAR_STRING: return "string";
    default:            return "bool";
  }
}

// ── Format a variable value as a string ─────────────────────────────────

static char *format_variable(const TQVariable *var) {
  if (!var || var->count == 0)
    return strdup("");

  // arz parser doesn't handle type=3 (bool), leaving value pointer NULL
  if (!var->value.i32)
    return strdup("0");

  // Estimate buffer size
  size_t bufsz = var->count * 64 + 1;
  char *buf = malloc(bufsz);
  if (!buf) return strdup("");

  buf[0] = '\0';
  size_t pos = 0;

  for (uint32_t i = 0; i < var->count; i++) {
    if (i > 0 && pos < bufsz - 2) {
      buf[pos++] = ';';
      buf[pos] = '\0';
    }

    char tmp[256];
    switch (var->type) {
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
    if (pos + tlen < bufsz) {
      memcpy(buf + pos, tmp, tlen + 1);
      pos += tlen;
    }
  }

  return buf;
}

// ── Sort comparator for record indices ──────────────────────────────────

static TQArzRecord *s_sort_records;  // qsort context (no user data in qsort)

static int cmp_record_indices(const void *a, const void *b) {
  const char *pa = s_sort_records[*(const uint32_t *)a].path;
  const char *pb = s_sort_records[*(const uint32_t *)b].path;
  if (!pa && !pb) return 0;
  if (!pa) return 1;
  if (!pb) return -1;
  return strcasecmp(pa, pb);
}

// ── Build the tree from arz records ─────────────────────────────────────

static void build_database_tree(DatabaseDialogState *st) {
  TQArzFile *arz = st->arz;
  if (!arz) return;

  // Sort record indices by path for alphabetical tree order
  uint32_t *sorted = malloc(arz->num_records * sizeof(uint32_t));
  if (!sorted) return;
  for (uint32_t i = 0; i < arz->num_records; i++)
    sorted[i] = i;

  s_sort_records = arz->records;
  qsort(sorted, arz->num_records, sizeof(uint32_t), cmp_record_indices);

  // Map path prefix -> GtkTreeIter for O(1) parent lookups
  GHashTable *dir_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);

  for (uint32_t ri = 0; ri < arz->num_records; ri++) {
    uint32_t r = sorted[ri];
    const char *path = arz->records[r].path;
    if (!path) continue;

    // Split path by backslash
    // e.g. "records\creature\monster\skeleton\am_minion_02.dbr"
    GtkTreeIter parent_iter;
    bool has_parent = false;

    // Walk each component, creating directories as needed
    const char *p = path;
    char prefix[1024];
    prefix[0] = '\0';
    size_t prefix_len = 0;

    while (*p) {
      // Find next separator
      const char *sep = strchr(p, '\\');
      if (!sep) {
        // This is the leaf (filename)
        GtkTreeIter leaf_iter;
        gtk_tree_store_append(st->tree_store, &leaf_iter,
                              has_parent ? &parent_iter : NULL);
        gtk_tree_store_set(st->tree_store, &leaf_iter,
                           COL_NAME, p,
                           COL_FULL_PATH, path,
                           COL_IS_LEAF, TRUE,
                           -1);
        break;
      }

      // Directory component
      size_t comp_len = (size_t)(sep - p);
      if (prefix_len + comp_len + 1 >= sizeof(prefix)) break;

      if (prefix_len > 0)
        prefix[prefix_len++] = '\\';
      memcpy(prefix + prefix_len, p, comp_len);
      prefix_len += comp_len;
      prefix[prefix_len] = '\0';

      // Check if this directory already exists
      GtkTreeIter *cached = g_hash_table_lookup(dir_map, prefix);
      if (cached) {
        parent_iter = *cached;
        has_parent = true;
      } else {
        // Create directory node
        GtkTreeIter dir_iter;
        gtk_tree_store_append(st->tree_store, &dir_iter,
                              has_parent ? &parent_iter : NULL);

        // Extract just the directory name for display
        char comp_name[256];
        if (comp_len >= sizeof(comp_name)) comp_len = sizeof(comp_name) - 1;
        memcpy(comp_name, p, comp_len);
        comp_name[comp_len] = '\0';

        gtk_tree_store_set(st->tree_store, &dir_iter,
                           COL_NAME, comp_name,
                           COL_FULL_PATH, NULL,
                           COL_IS_LEAF, FALSE,
                           -1);

        // Cache this iterator
        GtkTreeIter *stored = g_new(GtkTreeIter, 1);
        *stored = dir_iter;
        g_hash_table_insert(dir_map, g_strdup(prefix), stored);

        parent_iter = dir_iter;
        has_parent = true;
      }

      p = sep + 1;
    }
  }

  g_hash_table_destroy(dir_map);
  free(sorted);
}

// ── Tree selection changed: show record variables ───────────────────────

static void on_tree_selection_changed(GtkTreeSelection *sel, gpointer data) {
  DatabaseDialogState *st = data;
  gtk_list_store_clear(st->var_store);
  gtk_label_set_text(GTK_LABEL(st->path_label), "");

  GtkTreeModel *model;
  GtkTreeIter iter;
  if (!gtk_tree_selection_get_selected(sel, &model, &iter))
    return;

  gboolean is_leaf = FALSE;
  char *full_path = NULL;
  gtk_tree_model_get(model, &iter,
                     COL_FULL_PATH, &full_path,
                     COL_IS_LEAF, &is_leaf,
                     -1);

  if (!is_leaf || !full_path) {
    g_free(full_path);
    return;
  }

  // Show path in status bar
  gtk_label_set_text(GTK_LABEL(st->path_label), full_path);

  // Read and display record variables
  TQArzRecordData *rec = arz_read_record(st->arz, full_path);
  if (!rec) {
    g_free(full_path);
    return;
  }

  for (uint32_t i = 0; i < rec->num_vars; i++) {
    const TQVariable *var = &rec->vars[i];
    char *val_str = format_variable(var);

    GtkTreeIter row;
    gtk_list_store_append(st->var_store, &row);
    gtk_list_store_set(st->var_store, &row,
                       VCOL_NAME, var->name ? var->name : "",
                       VCOL_TYPE, type_name(var->type),
                       VCOL_VALUE, val_str,
                       -1);
    free(val_str);
  }

  arz_record_data_free(rec);
  g_free(full_path);
}

// ── Search / filter ─────────────────────────────────────────────────────

// Check if a node or any descendant matches the search text
static gboolean filter_visible_func(GtkTreeModel *model, GtkTreeIter *iter,
                                     gpointer data) {
  DatabaseDialogState *st = data;
  if (st->search_text[0] == '\0')
    return TRUE;

  // For leaf nodes, check if full path contains search text
  gboolean is_leaf = FALSE;
  char *full_path = NULL;
  gtk_tree_model_get(model, iter,
                     COL_FULL_PATH, &full_path,
                     COL_IS_LEAF, &is_leaf,
                     -1);

  if (is_leaf && full_path) {
    // Case-insensitive substring match
    char lower_path[1024];
    size_t len = strlen(full_path);
    if (len >= sizeof(lower_path)) len = sizeof(lower_path) - 1;
    for (size_t i = 0; i < len; i++)
      lower_path[i] = (char)tolower((unsigned char)full_path[i]);
    lower_path[len] = '\0';
    g_free(full_path);
    return strstr(lower_path, st->search_text) != NULL;
  }
  g_free(full_path);

  // For directory nodes, check if any child is visible
  GtkTreeIter child;
  if (gtk_tree_model_iter_children(model, &child, iter)) {
    do {
      if (filter_visible_func(model, &child, data))
        return TRUE;
    } while (gtk_tree_model_iter_next(model, &child));
  }

  return FALSE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
  DatabaseDialogState *st = data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

  // Lowercase the search text
  size_t len = strlen(text);
  if (len >= sizeof(st->search_text)) len = sizeof(st->search_text) - 1;
  for (size_t i = 0; i < len; i++)
    st->search_text[i] = (char)tolower((unsigned char)text[i]);
  st->search_text[len] = '\0';

  gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(st->filter_model));
}

// ── Show the database explorer dialog ───────────────────────────────────

void show_database_dialog(AppWidgets *widgets) {
  if (!global_config.game_folder) return;

  DatabaseDialogState *st = g_new0(DatabaseDialogState, 1);
  st->widgets = widgets;

  // Load the arz — try asset cache first, fall back to direct load
  char arz_path[1024];
  snprintf(arz_path, sizeof(arz_path), "%s/Database/database.arz",
           global_config.game_folder);

  st->arz = arz_load(arz_path);
  if (!st->arz) {
    g_free(st);
    return;
  }
  st->arz_owned = true;

  // Create dialog window
  st->dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(st->dialog), "Database Explorer");
  gtk_window_set_default_size(GTK_WINDOW(st->dialog), 1000, 700);
  gtk_window_set_transient_for(GTK_WINDOW(st->dialog),
                                GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(st->dialog), FALSE);
  g_object_set_data_full(G_OBJECT(st->dialog), "state", st,
                          database_state_free);

  // Main vertical box
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(vbox, 6);
  gtk_widget_set_margin_end(vbox, 6);
  gtk_widget_set_margin_top(vbox, 6);
  gtk_widget_set_margin_bottom(vbox, 6);
  gtk_window_set_child(GTK_WINDOW(st->dialog), vbox);

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

  // ── Left pane: record tree ──

  st->tree_store = gtk_tree_store_new(TREE_N_COLS,
                                       G_TYPE_STRING,   // COL_NAME
                                       G_TYPE_STRING,   // COL_FULL_PATH
                                       G_TYPE_BOOLEAN); // COL_IS_LEAF

  // Build tree from arz records
  build_database_tree(st);

  // Create filter model
  st->filter_model = gtk_tree_model_filter_new(
      GTK_TREE_MODEL(st->tree_store), NULL);
  gtk_tree_model_filter_set_visible_func(
      GTK_TREE_MODEL_FILTER(st->filter_model),
      filter_visible_func, st, NULL);

  // Tree view
  st->tree_view = gtk_tree_view_new_with_model(st->filter_model);
  g_object_unref(st->filter_model);

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
      "Record", renderer, "text", COL_NAME, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(st->tree_view), col);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(st->tree_view), FALSE);

  // Selection
  GtkTreeSelection *sel = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(st->tree_view));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
  g_signal_connect(sel, "changed", G_CALLBACK(on_tree_selection_changed), st);

  GtkWidget *tree_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scroll),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree_scroll),
                                 st->tree_view);
  gtk_paned_set_start_child(GTK_PANED(paned), tree_scroll);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

  // ── Right pane: variable list ──

  st->var_store = gtk_list_store_new(VAR_N_COLS,
                                      G_TYPE_STRING,   // VCOL_NAME
                                      G_TYPE_STRING,   // VCOL_TYPE
                                      G_TYPE_STRING);  // VCOL_VALUE

  st->var_tree_view = gtk_tree_view_new_with_model(
      GTK_TREE_MODEL(st->var_store));
  g_object_unref(st->var_store);

  GtkCellRenderer *name_r = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
      "Variable", name_r, "text", VCOL_NAME, NULL);
  gtk_tree_view_column_set_resizable(name_col, TRUE);
  gtk_tree_view_column_set_min_width(name_col, 150);
  gtk_tree_view_append_column(GTK_TREE_VIEW(st->var_tree_view), name_col);

  GtkCellRenderer *type_r = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *type_col = gtk_tree_view_column_new_with_attributes(
      "Type", type_r, "text", VCOL_TYPE, NULL);
  gtk_tree_view_column_set_resizable(type_col, TRUE);
  gtk_tree_view_column_set_min_width(type_col, 50);
  gtk_tree_view_append_column(GTK_TREE_VIEW(st->var_tree_view), type_col);

  GtkCellRenderer *val_r = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *val_col = gtk_tree_view_column_new_with_attributes(
      "Value", val_r, "text", VCOL_VALUE, NULL);
  gtk_tree_view_column_set_resizable(val_col, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(st->var_tree_view), val_col);

  GtkWidget *var_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(var_scroll),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(var_scroll),
                                 st->var_tree_view);
  gtk_paned_set_end_child(GTK_PANED(paned), var_scroll);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

  // ── Bottom path label ──

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
