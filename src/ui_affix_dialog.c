// ui_affix_dialog.c -- Affix modification dialog (three-pane prefix/suffix picker)
//
// Extracted from ui.c for maintainability.

#include "ui.h"
#include "item_stats.h"
#include "affix_table.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// Check if the family at entries[idx] has more than one member.
//
// entries: array of affix entries
// count: total number of entries
// idx: index to check
// returns: true if the family has siblings
static bool
family_has_siblings(const TQAffixEntry *entries, int count, int idx)
{
  const char *f = entries[idx].effect_family;

  if(!f)
    return(false);

  if(idx > 0 && entries[idx-1].effect_family &&
     strcasecmp(f, entries[idx-1].effect_family) == 0)
    return(true);

  if(idx + 1 < count && entries[idx+1].effect_family &&
     strcasecmp(f, entries[idx+1].effect_family) == 0)
    return(true);

  return(false);
}

// Check if entries[idx] is the highest tier in its family group.
// Since entries are sorted by family then tier ascending, the last entry
// in a family run has the highest tier.
//
// entries: array of affix entries
// count: total number of entries
// idx: index to check
// returns: true if this is the max tier in the family
static bool
is_max_tier_in_family(const TQAffixEntry *entries, int count, int idx)
{
  const char *f = entries[idx].effect_family;

  if(!f)
    return(true);

  if(idx + 1 < count && entries[idx+1].effect_family &&
     strcasecmp(f, entries[idx+1].effect_family) == 0)
    return(false);

  return(true);
}

// ── Affix dialog state ──────────────────────────────────────────────────

typedef struct {
  AppWidgets *widgets;
  GtkWidget  *dialog;
  TQItemAffixes *affixes;          // affix data
  bool owns_affixes;               // true if we should free affixes on destroy
  GtkListBox *prefix_listbox;
  GtkListBox *suffix_listbox;
  GtkWidget  *prefix_search;
  GtkWidget  *suffix_search;
  GtkLabel   *preview_label;
  char *orig_prefix;               // strdup'd originals for cancel
  char *orig_suffix;
  char *selected_prefix;           // current dialog selection (strdup'd or NULL)
  char *selected_suffix;
  // Back-pointers to the real item (exactly one is non-NULL)
  TQVaultItem *vault_item;
  TQItem      *equip_item;
  ContainerType source;
  // Item fields needed for preview
  uint32_t seed;
  char *base_name;
  char *relic_name;
  char *relic_bonus;
  uint32_t var1;
  char *relic_name2;
  char *relic_bonus2;
  uint32_t var2;
} AffixDialogState;

// Free the affix dialog state when the dialog is destroyed.
//
// data: AffixDialogState pointer
static void
affix_dialog_state_free(gpointer data)
{
  AffixDialogState *st = data;

  free(st->orig_prefix);
  free(st->orig_suffix);
  free(st->selected_prefix);
  free(st->selected_suffix);

  // If affixes came from the internal cache (affix_table_get), don't free.
  // If they came from an override (e.g. forge), we own them and must free.
  if(st->owns_affixes)
    affix_result_free(st->affixes);

  g_free(st);
}

// Build a temporary TQVaultItem from dialog state and render tooltip markup.
//
// st: affix dialog state
static void
update_affix_preview(AffixDialogState *st)
{
  TQVaultItem tmp = {
    .seed = st->seed,
    .base_name = st->base_name,
    .prefix_name = st->selected_prefix,
    .suffix_name = st->selected_suffix,
    .relic_name = st->relic_name,
    .relic_bonus = st->relic_bonus,
    .var1 = st->var1,
    .relic_name2 = st->relic_name2,
    .relic_bonus2 = st->relic_bonus2,
    .var2 = st->var2,
  };

  if(!st->preview_label)
    return;  // dialog being torn down

  char buf[16384];

  buf[0] = '\0';
  vault_item_format_stats(&tmp, st->widgets->translations, buf, sizeof(buf));
  gtk_label_set_markup(st->preview_label, buf);
}

// Retrieve the affix-path string stored on a listbox row (may be NULL for "(None)").
//
// row: the listbox row
// returns: affix path string or NULL
static const char *
row_get_affix_path(GtkListBoxRow *row)
{
  return(g_object_get_data(G_OBJECT(row), "affix-path"));
}

// Retrieve the display label stored on a listbox row for filtering.
//
// row: the listbox row
// returns: label text string or NULL
static const char *
row_get_label_text(GtkListBoxRow *row)
{
  return(g_object_get_data(G_OBJECT(row), "label-text"));
}

// Handle prefix listbox row selection changes.
//
// box: the listbox (unused)
// row: newly selected row, or NULL
// data: AffixDialogState pointer
static void
on_prefix_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  (void)box;
  AffixDialogState *st = data;

  if(!st->preview_label)
    return;  // dialog being torn down

  free(st->selected_prefix);

  if(row)
  {
    const char *p = row_get_affix_path(row);

    st->selected_prefix = p ? strdup(p) : NULL;
  }

  else
  {
    st->selected_prefix = st->orig_prefix ? strdup(st->orig_prefix) : NULL;
  }

  update_affix_preview(st);
}

// Handle suffix listbox row selection changes.
//
// box: the listbox (unused)
// row: newly selected row, or NULL
// data: AffixDialogState pointer
static void
on_suffix_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  (void)box;
  AffixDialogState *st = data;

  if(!st->preview_label)
    return;  // dialog being torn down

  free(st->selected_suffix);

  if(row)
  {
    const char *p = row_get_affix_path(row);

    st->selected_suffix = p ? strdup(p) : NULL;
  }

  else
  {
    st->selected_suffix = st->orig_suffix ? strdup(st->orig_suffix) : NULL;
  }

  update_affix_preview(st);
}

// Case-insensitive substring search.
//
// haystack: string to search in
// needle: string to search for
// returns: true if needle is found in haystack
static bool
ci_contains(const char *haystack, const char *needle)
{
  if(!haystack || !needle)
    return(false);

  for(const char *s = haystack; *s; s++)
  {
    const char *a = s, *b = needle;

    while(*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
    {
      a++; b++;
    }

    if(!*b)
      return(true);
  }

  return(false);
}

// Filter function for affix listbox rows based on search text.
//
// row: the listbox row to test
// data: GtkSearchEntry pointer
// returns: TRUE if the row should be visible
static gboolean
affix_filter_func(GtkListBoxRow *row, gpointer data)
{
  GtkSearchEntry *search = GTK_SEARCH_ENTRY(data);
  const char *query = gtk_editable_get_text(GTK_EDITABLE(search));

  if(!query || !query[0])
    return(TRUE);  // no filter -- show all

  // Group header rows: always show for context
  if(g_object_get_data(G_OBJECT(row), "is-header"))
    return(TRUE);

  const char *label = row_get_label_text(row);

  if(!label)
    return(TRUE);  // "(None)" row always visible

  // Match against display name or stat summary
  if(ci_contains(label, query))
    return(TRUE);

  const char *summary = g_object_get_data(G_OBJECT(row), "stat-summary");

  if(ci_contains(summary, query))
    return(TRUE);

  return(FALSE);
}

// Invalidate prefix listbox filter when search text changes.
//
// entry: the search entry (unused)
// data: AffixDialogState pointer
static void
on_prefix_search_changed(GtkSearchEntry *entry, gpointer data)
{
  (void)entry;
  AffixDialogState *st = data;

  gtk_list_box_invalidate_filter(st->prefix_listbox);
}

// Invalidate suffix listbox filter when search text changes.
//
// entry: the search entry (unused)
// data: AffixDialogState pointer
static void
on_suffix_search_changed(GtkSearchEntry *entry, gpointer data)
{
  (void)entry;
  AffixDialogState *st = data;

  gtk_list_box_invalidate_filter(st->suffix_listbox);
}

// Apply selected affixes to the item and close the dialog.
//
// btn: the Apply button (unused)
// data: AffixDialogState pointer
static void
on_affix_dialog_apply(GtkButton *btn, gpointer data)
{
  (void)btn;
  AffixDialogState *st = data;
  AppWidgets *w = st->widgets;

  if(st->equip_item)
  {
    TQItem *eq = st->equip_item;

    free(eq->prefix_name);
    eq->prefix_name = st->selected_prefix ? strdup(st->selected_prefix) : NULL;
    free(eq->suffix_name);
    eq->suffix_name = st->selected_suffix ? strdup(st->selected_suffix) : NULL;
    w->char_dirty = true;
  }

  else if(st->vault_item)
  {
    TQVaultItem *it = st->vault_item;

    free(it->prefix_name);
    it->prefix_name = st->selected_prefix ? strdup(st->selected_prefix) : NULL;
    free(it->suffix_name);
    it->suffix_name = st->selected_suffix ? strdup(st->selected_suffix) : NULL;

    if(st->source == CONTAINER_VAULT)
      w->vault_dirty = true;
    else
      w->char_dirty = true;
  }

  invalidate_tooltips(w);
  queue_redraw_equip(w);
  update_save_button_sensitivity(w);
  st->preview_label = NULL;  // prevent row-deselect signals from using destroyed widget
  gtk_window_destroy(GTK_WINDOW(st->dialog));
}

// Cancel and close the affix dialog without applying changes.
//
// btn: the Cancel button (unused)
// data: AffixDialogState pointer
static void
on_affix_dialog_cancel(GtkButton *btn, gpointer data)
{
  (void)btn;
  AffixDialogState *st = data;

  st->preview_label = NULL;
  gtk_window_destroy(GTK_WINDOW(st->dialog));
}

// Create a non-selectable group header row for the affix listbox.
//
// stat_summary: text to display as the group header
// returns: the header row widget
static GtkWidget *
make_group_header(const char *stat_summary)
{
  GtkWidget *row = gtk_list_box_row_new();

  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

  GtkWidget *label = gtk_label_new(stat_summary);

  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_margin_start(label, 4);
  gtk_widget_set_margin_top(label, 6);
  gtk_widget_set_margin_bottom(label, 2);
  gtk_widget_add_css_class(label, "affix-group-header");
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);

  g_object_set_data(G_OBJECT(row), "is-header", GINT_TO_POINTER(1));

  return(row);
}

// Create a single listbox row for an affix entry.
// Tiered rows: "[T5] Name - compact_values (Pct%)"
// Singleton rows: "Name - full_stat_summary"
//
// entry: affix entry data, or NULL for "(None)" row
// pct: weight percentage to display
// is_current: true if this is the currently assigned affix
// show_tier: true if tier badge should be shown
// is_lower_tier: true if this is not the highest tier in its family (dimmed)
// returns: the row widget
static GtkWidget *
make_affix_row(const TQAffixEntry *entry, float pct,
               bool is_current, bool show_tier,
               bool is_lower_tier)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  gtk_widget_set_margin_start(hbox, 4);
  gtk_widget_set_margin_end(hbox, 4);
  gtk_widget_set_margin_top(hbox, 2);
  gtk_widget_set_margin_bottom(hbox, 2);

  char label_buf[512];

  if(!entry)
  {
    snprintf(label_buf, sizeof(label_buf), "(None)");
  }

  else if(show_tier && entry->tier > 0)
  {
    // Tiered row: show compact values (stats are in the group header)
    if(entry->stat_values && entry->stat_values[0])
      snprintf(label_buf, sizeof(label_buf), "[T%d] %s - %s",
               entry->tier, entry->translation, entry->stat_values);
    else
      snprintf(label_buf, sizeof(label_buf), "[T%d] %s",
               entry->tier, entry->translation);
  }

  else
  {
    // Singleton row: show full stat summary
    if(entry->stat_summary && entry->stat_summary[0])
      snprintf(label_buf, sizeof(label_buf), "%s - %s",
               entry->translation, entry->stat_summary);
    else
      snprintf(label_buf, sizeof(label_buf), "%s", entry->translation);
  }

  GtkWidget *name_label = gtk_label_new(label_buf);

  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(name_label, TRUE);

  if(is_current)
    gtk_widget_add_css_class(name_label, "affix-current");

  if(is_lower_tier)
    gtk_widget_add_css_class(name_label, "affix-lower-tier");

  if(entry && show_tier && entry->tier > 0)
    gtk_widget_set_tooltip_text(name_label, "Higher tier = stronger variant");

  gtk_box_append(GTK_BOX(hbox), name_label);

  if(entry && pct > 0)
  {
    char wt[32];

    snprintf(wt, sizeof(wt), "%.1f%%", pct);
    GtkWidget *wt_label = gtk_label_new(wt);

    gtk_widget_add_css_class(wt_label, "dim-label");
    gtk_box_append(GTK_BOX(hbox), wt_label);
  }

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

  // Store data on the row
  if(entry)
  {
    g_object_set_data_full(G_OBJECT(row), "affix-path",
                           g_strdup(entry->affix_path), g_free);
    g_object_set_data_full(G_OBJECT(row), "label-text",
                           g_strdup(label_buf), g_free);

    if(entry->stat_summary)
      g_object_set_data_full(G_OBJECT(row), "stat-summary",
                             g_strdup(entry->stat_summary), g_free);
  }

  return(row);
}

// Show the affix modification dialog for the current context item.
//
// widgets: application widget state
// override_affixes: if non-NULL, use these affixes instead of looking up by item
//                   (caller transfers ownership)
// override_title: if non-NULL, use this as the dialog title
void
show_affix_dialog(AppWidgets *widgets, TQItemAffixes *override_affixes,
                  const char *override_title)
{
  const char *base = NULL;
  TQVaultItem *vault_item = widgets->context_item;
  TQItem *equip_item = widgets->context_equip_item;

  if(equip_item)
    base = equip_item->base_name;
  else if(vault_item)
    base = vault_item->base_name;

  if(!base)
    return;

  TQItemAffixes *affixes;
  bool owns_affixes = false;

  if(override_affixes)
  {
    affixes = override_affixes;
    owns_affixes = true;   // caller transfers ownership
  }

  else
  {
    affixes = affix_table_get(base, widgets->translations);
  }

  if(!affixes)
    return;

  AffixDialogState *st = g_new0(AffixDialogState, 1);

  st->widgets = widgets;
  st->affixes = affixes;
  st->owns_affixes = owns_affixes;
  st->vault_item = vault_item;
  st->equip_item = equip_item;
  st->source = widgets->context_source;

  // Copy item fields for preview
  if(equip_item)
  {
    st->seed = equip_item->seed;
    st->base_name = equip_item->base_name;
    st->orig_prefix = equip_item->prefix_name ? strdup(equip_item->prefix_name) : NULL;
    st->orig_suffix = equip_item->suffix_name ? strdup(equip_item->suffix_name) : NULL;
    st->relic_name = equip_item->relic_name;
    st->relic_bonus = equip_item->relic_bonus;
    st->var1 = equip_item->var1;
    st->relic_name2 = equip_item->relic_name2;
    st->relic_bonus2 = equip_item->relic_bonus2;
    st->var2 = equip_item->var2;
  }

  else
  {
    st->seed = vault_item->seed;
    st->base_name = vault_item->base_name;
    st->orig_prefix = vault_item->prefix_name ? strdup(vault_item->prefix_name) : NULL;
    st->orig_suffix = vault_item->suffix_name ? strdup(vault_item->suffix_name) : NULL;
    st->relic_name = vault_item->relic_name;
    st->relic_bonus = vault_item->relic_bonus;
    st->var1 = vault_item->var1;
    st->relic_name2 = vault_item->relic_name2;
    st->relic_bonus2 = vault_item->relic_bonus2;
    st->var2 = vault_item->var2;
  }

  st->selected_prefix = st->orig_prefix ? strdup(st->orig_prefix) : NULL;
  st->selected_suffix = st->orig_suffix ? strdup(st->orig_suffix) : NULL;

  // -- Build dialog window --
  GtkWidget *dialog = gtk_window_new();

  st->dialog = dialog;
  gtk_window_set_title(GTK_WINDOW(dialog), override_title ? override_title : "Modify Affixes");
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 1100, 650);
  gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);

  // Attach state for cleanup on destroy
  g_object_set_data_full(G_OBJECT(dialog), "affix-state", st, affix_dialog_state_free);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  // -- Three-pane hbox --
  GtkWidget *panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_vexpand(panes, TRUE);
  gtk_widget_set_margin_start(panes, 8);
  gtk_widget_set_margin_end(panes, 8);
  gtk_widget_set_margin_top(panes, 8);
  gtk_box_append(GTK_BOX(vbox), panes);

  // -- Left pane: Prefix --
  GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_hexpand(left_vbox, TRUE);
  gtk_box_append(GTK_BOX(panes), left_vbox);

  GtkWidget *prefix_label = gtk_label_new("Prefix");

  gtk_widget_set_halign(prefix_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(left_vbox), prefix_label);

  GtkWidget *prefix_search = gtk_search_entry_new();

  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(prefix_search), "Filter...");
  st->prefix_search = prefix_search;
  gtk_box_append(GTK_BOX(left_vbox), prefix_search);

  GtkWidget *prefix_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(prefix_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(prefix_scroll, TRUE);
  gtk_box_append(GTK_BOX(left_vbox), prefix_scroll);

  GtkWidget *prefix_listbox = gtk_list_box_new();

  st->prefix_listbox = GTK_LIST_BOX(prefix_listbox);
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(prefix_listbox), GTK_SELECTION_SINGLE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(prefix_scroll), prefix_listbox);

  // -- Center pane: Preview --
  GtkWidget *center_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(center_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(center_scroll, TRUE);
  gtk_widget_set_vexpand(center_scroll, TRUE);
  gtk_widget_set_size_request(center_scroll, 420, -1);
  gtk_widget_add_css_class(center_scroll, "affix-preview");
  gtk_box_append(GTK_BOX(panes), center_scroll);

  GtkWidget *preview_label = gtk_label_new("");

  st->preview_label = GTK_LABEL(preview_label);
  gtk_label_set_use_markup(GTK_LABEL(preview_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(preview_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(preview_label), 0.0f);
  gtk_label_set_yalign(GTK_LABEL(preview_label), 0.0f);
  gtk_widget_set_margin_start(preview_label, 12);
  gtk_widget_set_margin_end(preview_label, 12);
  gtk_widget_set_margin_top(preview_label, 12);
  gtk_widget_set_margin_bottom(preview_label, 12);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(center_scroll), preview_label);

  // -- Right pane: Suffix --
  GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_hexpand(right_vbox, TRUE);
  gtk_box_append(GTK_BOX(panes), right_vbox);

  GtkWidget *suffix_label = gtk_label_new("Suffix");

  gtk_widget_set_halign(suffix_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(right_vbox), suffix_label);

  GtkWidget *suffix_search = gtk_search_entry_new();

  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(suffix_search), "Filter...");
  st->suffix_search = suffix_search;
  gtk_box_append(GTK_BOX(right_vbox), suffix_search);

  GtkWidget *suffix_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(suffix_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(suffix_scroll, TRUE);
  gtk_box_append(GTK_BOX(right_vbox), suffix_scroll);

  GtkWidget *suffix_listbox = gtk_list_box_new();

  st->suffix_listbox = GTK_LIST_BOX(suffix_listbox);
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(suffix_listbox), GTK_SELECTION_SINGLE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(suffix_scroll), suffix_listbox);

  // -- Button bar --
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(btn_box, 8);
  gtk_widget_set_margin_bottom(btn_box, 8);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *apply_btn = gtk_button_new_with_label("Apply");

  gtk_widget_add_css_class(apply_btn, "suggested-action");
  g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_affix_dialog_apply), st);
  gtk_box_append(GTK_BOX(btn_box), apply_btn);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_affix_dialog_cancel), st);
  gtk_box_append(GTK_BOX(btn_box), cancel_btn);

  // -- Populate prefix listbox --
  float prefix_total_w = 0;

  for(int i = 0; i < affixes->prefixes.count; i++)
    prefix_total_w += affixes->prefixes.entries[i].weight;

  const char *cur_prefix = st->orig_prefix;
  bool none_is_current = !cur_prefix || !cur_prefix[0];
  GtkWidget *none_row = make_affix_row(NULL, 0, none_is_current, false, false);

  gtk_list_box_append(GTK_LIST_BOX(prefix_listbox), none_row);
  GtkWidget *prefix_select_row = none_is_current ? none_row : NULL;

  const char *prev_family = NULL;

  for(int i = 0; i < affixes->prefixes.count; i++)
  {
    TQAffixEntry *e = &affixes->prefixes.entries[i];
    bool new_family = !prev_family || !e->effect_family ||
                      strcasecmp(prev_family, e->effect_family) != 0;
    bool has_siblings = family_has_siblings(affixes->prefixes.entries,
                                            affixes->prefixes.count, i);

    // Insert group header when family changes and there are multiple tiers
    if(new_family && has_siblings)
    {
      const char *hdr_text = e->stat_category ? e->stat_category : e->stat_summary;

      if(hdr_text)
      {
        GtkWidget *hdr = make_group_header(hdr_text);

        gtk_list_box_append(GTK_LIST_BOX(prefix_listbox), hdr);
      }
    }

    prev_family = e->effect_family;

    bool is_cur = cur_prefix && strcasecmp(cur_prefix, e->affix_path) == 0;
    float pct = prefix_total_w > 0 ? (e->weight / prefix_total_w) * 100.0f : 0;
    bool lower = has_siblings && !is_max_tier_in_family(
      affixes->prefixes.entries, affixes->prefixes.count, i);

    GtkWidget *r = make_affix_row(e, pct, is_cur, has_siblings, lower);

    gtk_list_box_append(GTK_LIST_BOX(prefix_listbox), r);

    if(is_cur)
      prefix_select_row = r;
  }

  // -- Populate suffix listbox --
  float suffix_total_w = 0;

  for(int i = 0; i < affixes->suffixes.count; i++)
    suffix_total_w += affixes->suffixes.entries[i].weight;

  const char *cur_suffix = st->orig_suffix;

  none_is_current = !cur_suffix || !cur_suffix[0];
  none_row = make_affix_row(NULL, 0, none_is_current, false, false);
  gtk_list_box_append(GTK_LIST_BOX(suffix_listbox), none_row);
  GtkWidget *suffix_select_row = none_is_current ? none_row : NULL;

  prev_family = NULL;

  for(int i = 0; i < affixes->suffixes.count; i++)
  {
    TQAffixEntry *e = &affixes->suffixes.entries[i];
    bool new_family = !prev_family || !e->effect_family ||
                      strcasecmp(prev_family, e->effect_family) != 0;
    bool has_siblings = family_has_siblings(affixes->suffixes.entries,
                                            affixes->suffixes.count, i);

    if(new_family && has_siblings)
    {
      const char *hdr_text = e->stat_category ? e->stat_category : e->stat_summary;

      if(hdr_text)
      {
        GtkWidget *hdr = make_group_header(hdr_text);

        gtk_list_box_append(GTK_LIST_BOX(suffix_listbox), hdr);
      }
    }

    prev_family = e->effect_family;

    bool is_cur = cur_suffix && strcasecmp(cur_suffix, e->affix_path) == 0;
    float pct = suffix_total_w > 0 ? (e->weight / suffix_total_w) * 100.0f : 0;
    bool lower = has_siblings && !is_max_tier_in_family(
      affixes->suffixes.entries, affixes->suffixes.count, i);

    GtkWidget *r = make_affix_row(e, pct, is_cur, has_siblings, lower);

    gtk_list_box_append(GTK_LIST_BOX(suffix_listbox), r);

    if(is_cur)
      suffix_select_row = r;
  }

  // Connect signals AFTER populating to avoid spurious callbacks
  g_signal_connect(prefix_listbox, "row-selected",
                   G_CALLBACK(on_prefix_row_selected), st);
  g_signal_connect(suffix_listbox, "row-selected",
                   G_CALLBACK(on_suffix_row_selected), st);

  gtk_list_box_set_filter_func(GTK_LIST_BOX(prefix_listbox),
                               affix_filter_func, prefix_search, NULL);
  gtk_list_box_set_filter_func(GTK_LIST_BOX(suffix_listbox),
                               affix_filter_func, suffix_search, NULL);

  g_signal_connect(prefix_search, "search-changed",
                   G_CALLBACK(on_prefix_search_changed), st);
  g_signal_connect(suffix_search, "search-changed",
                   G_CALLBACK(on_suffix_search_changed), st);

  // Pre-select current affix rows
  if(prefix_select_row)
    gtk_list_box_select_row(GTK_LIST_BOX(prefix_listbox),
                            GTK_LIST_BOX_ROW(prefix_select_row));

  if(suffix_select_row)
    gtk_list_box_select_row(GTK_LIST_BOX(suffix_listbox),
                            GTK_LIST_BOX_ROW(suffix_select_row));

  // Generate initial preview
  update_affix_preview(st);

  gtk_window_present(GTK_WINDOW(dialog));
}
