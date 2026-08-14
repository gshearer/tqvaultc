#include "ui.h"
#include "ui_internal.h"
#include "config.h"
#include "texture.h"
#include "arc.h"
#include "arz.h"
#include "asset_lookup.h"
#include "item_stats.h"
#include "affix_table.h"
#include "prefetch.h"
#include "version.h"
#include "build_number.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// Resolve the in-game bitmap an item draws (before the ".tex" swap): "bitmap"/
// "artifactBitmap" for normal items, or the shard vs complete relic/charm
// bitmap chosen from var1.  See ui.h for the single-piece rule.
//   base_name - DBR record path for the item
//   var1      - shard count (only affects multi-piece relics/charms)
// Returns a malloc'd bitmap path (caller frees), or NULL.
char*
item_resolve_bitmap(const char *base_name, uint32_t var1)
{
  if(!base_name)
    return(NULL);

  TQArzRecordData *data = asset_get_dbr(base_name);

  if(!data)
    return(NULL);

  char *bitmap = arz_record_get_string(data, "bitmap", NULL);

  if(bitmap)
    return(bitmap);

  bitmap = arz_record_get_string(data, "artifactBitmap", NULL);
  if(bitmap)
    return(bitmap);

  // Artifact formulae (recipes -- Class ItemArtifactFormula) keep their icon in
  // artifactFormulaBitmapName rather than "bitmap"/"artifactBitmap".  Without
  // this the recipe items render as the purple no-texture placeholder.
  bitmap = arz_record_get_string(data, "artifactFormulaBitmapName", NULL);
  if(bitmap)
    return(bitmap);

  // For relics/charms: use shardBitmap when incomplete, relicBitmap when complete.
  // completedRelicLevel from the DBR tells us how many shards are needed.
  char *relic_bmp = arz_record_get_string(data, "relicBitmap", NULL);
  char *shard_bmp = arz_record_get_string(data, "shardBitmap", NULL);

  if(relic_bmp && shard_bmp)
  {
    int max_shards = arz_record_get_int(data, "completedRelicLevel", 0, NULL);

    // Single-piece relics/charms (completedRelicLevel <= 1) are always awarded
    // complete, so they always use the full relicBitmap and never the shard
    // image -- only genuinely multi-piece items can be partial.
    bool incomplete = (max_shards > 1 && var1 < (uint32_t)max_shards);

    if(incomplete)
    {
      free(relic_bmp);
      return(shard_bmp);
    }

    free(shard_bmp);
    return(relic_bmp);
  }

  if(relic_bmp)
    return(relic_bmp);

  return(shard_bmp);   // may be NULL
}

// Load the texture for a vault item, using a cache keyed by base_name:var1.
// Handles relics/charms (shard vs complete bitmap), artifacts, and normal items.
//   widgets   - app state (owns the texture cache)
//   base_name - DBR record path for the item
//   var1      - shard count (used to pick shard vs complete relic texture)
// Returns a new GdkPixbuf ref (caller must unref), or NULL on failure.
GdkPixbuf*
load_item_texture(AppWidgets *widgets, const char *base_name, uint32_t var1)
{
  if(!base_name)
    return(NULL);
  if(!global_config.game_folder)
    return(NULL);

  // Cache key includes shard count so incomplete and complete relics/charms
  // get different textures even though they share the same base_name.
  char cache_key[1200];

  snprintf(cache_key, sizeof(cache_key), "%s:%u", base_name, var1);

  GdkPixbuf *cached = g_hash_table_lookup(widgets->texture_cache, cache_key);

  if(cached)
    return(g_object_ref(cached));

  char *bitmap_path = item_resolve_bitmap(base_name, var1);

  char tex_path[1024];
  const char *source = bitmap_path ? bitmap_path : base_name;
  const char *dot = strrchr(source, '.');
  int base_len = dot ? (int)(dot - source) : (int)strlen(source);

  // Replace any extension on source with ".tex" in one bounded write.  source
  // comes from on-disk DBR data and can be arbitrarily long, so snprintf's
  // truncation is what keeps the fixed buffer safe.
  snprintf(tex_path, sizeof(tex_path), "%.*s.tex", base_len, source);

  if(bitmap_path)
    free(bitmap_path);

  GdkPixbuf *pixbuf = texture_load(tex_path);

  if(pixbuf)
    g_hash_table_insert(widgets->texture_cache, strdup(cache_key), g_object_ref(pixbuf));

  return(pixbuf);
}

// Check whether base_name refers to a standalone relic or charm item.
//   base_name - DBR record path
// Returns true if the item is a relic or charm.
bool
item_is_relic_or_charm(const char *base_name)
{
  if(!base_name)
    return(false);

  // Case-insensitive, separator-agnostic path checks (matching the C# reference)
  if(path_contains_ci(base_name, "animalrelics") ||
     path_contains_ci(base_name, "\\relics\\") ||
     path_contains_ci(base_name, "\\charms\\"))
    return(true);

  // Fallback: check DBR Class for items in non-standard directories (e.g. HCDUNGEON)
  const char *cls = dbr_get_string(base_name, "Class");

  if(cls && (strcasecmp(cls, "ItemRelic") == 0 ||
             strcasecmp(cls, "ItemCharm") == 0))
    return(true);

  return(false);
}

// Duplicate a string safely, returning NULL if s is NULL.
//   s - string to duplicate (may be NULL)
// Returns a malloc'd copy, or NULL.
char*
safe_strdup(const char *s)
{
  return(s ? strdup(s) : NULL);
}

// ── GtkDropDown helpers (replacing deprecated GtkComboBoxText) ─────────

// Get the currently selected text from a GtkDropDown.
//   dd - the GtkDropDown widget
// Returns a g_strdup'd string (caller must g_free), or NULL if nothing selected.
char*
dropdown_get_selected_text(GtkWidget *dd)
{
  GtkStringObject *obj = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dd));

  if(!obj)
    return(NULL);

  return(g_strdup(gtk_string_object_get_string(obj)));
}

// Select an item in a GtkDropDown by matching its string value.
//   dd   - the GtkDropDown widget
//   name - the string to match
// Returns the index of the matched item, or GTK_INVALID_LIST_POSITION.
guint
dropdown_select_by_name(GtkWidget *dd, const char *name)
{
  GtkStringList *sl = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(dd)));
  guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));

  for(guint i = 0; i < n; i++)
  {
    if(strcmp(gtk_string_list_get_string(sl, i), name) == 0)
    {
      gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i);
      return(i);
    }
  }

  return(GTK_INVALID_LIST_POSITION);
}

// Check whether a vault item is a stackable type (relics, charms, potions, scrolls).
// Items with affixes attached are never stackable.
//   a - the vault item to check
// Returns true if the item type supports stacking.
bool
item_is_stackable_type(const TQVaultItem *a)
{
  if(!a || !a->base_name)
    return(false);
  if(a->prefix_name && a->prefix_name[0])
    return(false);
  if(a->suffix_name && a->suffix_name[0])
    return(false);
  if(a->relic_name && a->relic_name[0])
    return(false);
  if(a->relic_name2 && a->relic_name2[0])
    return(false);

  // Only relics, charms, potions, and scrolls are stackable.  Single-piece
  // quest relics/charms (completedRelicLevel 1) are always awarded complete and
  // never exist as shards, so they have no quantity to set.
  const char *b = a->base_name;

  if(path_contains_ci(b, "\\relics\\") ||
     path_contains_ci(b, "\\charms\\") ||
     path_contains_ci(b, "\\animalrelic"))
    return(!relic_is_single_piece(b));
  if(path_contains_ci(b, "\\oneshot\\"))
    return(true);
  if(path_contains_ci(b, "\\scrolls\\"))
    return(true);

  // Fallback: items like HCDungeon potions live outside the above paths
  // but have Class=OneShot_Scroll or OneShot_Potion in their DBR.
  TQArzRecordData *data = asset_get_dbr(b);

  if(data)
  {
    char *cls = arz_record_get_string(data, "Class", NULL);

    if(cls)
    {
      bool is_oneshot = (strcasestr(cls, "OneShot_") != NULL);

      free(cls);
      if(is_oneshot)
        return(true);
    }
  }

  return(false);
}

// Check whether path refers to an artifact (but not an arcane formula).
//   base_name - DBR record path
// Returns true if the item is an artifact.
bool
item_is_artifact(const char *base_name)
{
  if(!base_name)
    return(false);
  if(!path_contains_ci(base_name, "\\artifacts\\"))
    return(false);
  if(path_contains_ci(base_name, "\\arcaneformulae\\"))
    return(false);

  return(true);
}

// Check whether the suffix path grants a second relic/charm socket slot.
//   suffix_name - suffix DBR path (may be NULL or empty)
// Returns true if the suffix adds an extra relic slot.
bool
item_has_two_relic_slots(const char *suffix_name)
{
  if(!suffix_name || !suffix_name[0])
    return(false);

  return(strcasestr(suffix_name, "RARE_EXTRARELIC_01.DBR") != NULL);
}

// Look up a string variable from a DBR record.
//   record_path - DBR record path
//   var_name    - variable name to look up
// Returns an internal pointer to the string (do not free), or NULL.
const char*
dbr_get_string(const char *record_path, const char *var_name)
{
  if(!record_path || !record_path[0])
    return(NULL);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(NULL);

  const char *interned = arz_intern(var_name);
  TQVariable *v = arz_record_get_var(data, interned);

  if(v && v->type == TQ_VAR_STRING && v->count > 0)
    return(v->value.str[0]);

  return(NULL);
}

// Save the current vault to disk if it has been modified.
//   widgets - app state
void
save_vault_if_dirty(AppWidgets *widgets)
{
  if(widgets->vault_dirty && widgets->current_vault && widgets->current_vault->vault_name)
  {
    vault_save_json(widgets->current_vault, widgets->current_vault->vault_name);
    widgets->vault_dirty = false;
    if(tqvc_debug)
      printf("vault saved: %s\n", widgets->current_vault->vault_name);
  }
}

// True if there are any unsaved modifications: character data, or any of the
// caravan stash tabs (transfer / player storage / relic vault).  The stashes
// are saved to separate files but, from the user's point of view, are part of
// "the character", so edits to them must drive the Save button and the
// close/switch confirmation the same way character edits do.
//   widgets - app state
bool
has_unsaved_changes(AppWidgets *widgets)
{
  return(widgets->char_dirty ||
         (widgets->transfer_stash && widgets->transfer_stash->dirty) ||
         (widgets->player_stash   && widgets->player_stash->dirty) ||
         (widgets->relic_vault    && widgets->relic_vault->dirty));
}

// Update the Save Character button's sensitivity to reflect unsaved changes
// (character data or any stash tab).
//   widgets - app state
void
update_save_button_sensitivity(AppWidgets *widgets)
{
  if(widgets->save_char_btn)
    gtk_widget_set_sensitive(widgets->save_char_btn, has_unsaved_changes(widgets));
}

// Save the current character to disk if it has been modified.
//   widgets - app state
void
save_character_if_dirty(AppWidgets *widgets)
{
  if(widgets->char_dirty && widgets->current_character && widgets->current_character->filepath)
  {
    if(character_save(widgets->current_character, widgets->current_character->filepath) == 0)
    {
      widgets->char_dirty = false;
      update_save_button_sensitivity(widgets);
      if(tqvc_debug)
        printf("character saved: %s\n", widgets->current_character->filepath);
    }
    else
    {
      fprintf(stderr, "character save failed: %s\n", widgets->current_character->filepath);
    }
  }
}

// Save all dirty stashes (transfer, player, relic vault) to disk.
//   widgets - app state
void
save_stashes_if_dirty(AppWidgets *widgets)
{
  if(widgets->transfer_stash && widgets->transfer_stash->dirty)
    stash_save(widgets->transfer_stash);

  if(widgets->player_stash && widgets->player_stash->dirty)
    stash_save(widgets->player_stash);

  if(widgets->relic_vault && widgets->relic_vault->dirty)
    stash_save(widgets->relic_vault);
}

// Get the cell dimensions of an item from its texture, falling back to struct fields.
//   widgets - app state (for texture loading)
//   item    - the vault item
//   w       - output: width in cells
//   h       - output: height in cells
void
get_item_dims(AppWidgets *widgets, TQVaultItem *item, int *w, int *h)
{
  GdkPixbuf *pixbuf = load_item_texture(widgets, item->base_name, item->var1);

  if(pixbuf)
  {
    *w = gdk_pixbuf_get_width(pixbuf) / 32;
    *h = gdk_pixbuf_get_height(pixbuf) / 32;
    if(*w < 1)
      *w = 1;
    if(*h < 1)
      *h = 1;
    g_object_unref(pixbuf);
  }
  else
  {
    *w = item->width  > 0 ? item->width  : 1;
    *h = item->height > 0 ? item->height : 1;
  }
}

// strip_pango_markup() lives in item_stats.c now (GTK-free, shared with the
// Database Browser's search-blob generation); declared in item_stats.h.

// Invalidate all tooltip caches and hide the tooltip popover.
//   widgets - app state
void
invalidate_tooltips(AppWidgets *widgets)
{
  widgets->last_tooltip_item     = NULL;
  widgets->last_inv_tooltip_item = NULL;
  widgets->last_bag_tooltip_item = NULL;
  widgets->last_equip_tooltip_slot = -1;
  widgets->last_transfer_tooltip_item = NULL;
  widgets->last_player_tooltip_item = NULL;
  widgets->last_relic_tooltip_item = NULL;
  if(widgets->tooltip_popover)
    gtk_widget_set_visible(widgets->tooltip_popover, FALSE);
  if(widgets->compare_scroll)
    gtk_widget_set_visible(widgets->compare_scroll, FALSE);
  if(widgets->compare_separator)
    gtk_widget_set_visible(widgets->compare_separator, FALSE);
}

// Queue a redraw on all equipment-related drawing areas and update stat tables.
//   widgets - app state
void
queue_redraw_equip(AppWidgets *widgets)
{
  gtk_widget_queue_draw(widgets->vault_drawing_area);
  gtk_widget_queue_draw(widgets->inv_drawing_area);
  gtk_widget_queue_draw(widgets->bag_drawing_area);
  gtk_widget_queue_draw(widgets->equip_drawing_area);

  // Equipping/unequipping changes the character's buffed attributes, which the
  // always-on equippability tint depends on for every sack -- including the
  // stashes -- so refresh those too (and to hide the tint when an item is picked
  // up out of an equipment slot).  No-op when the stash dialog is closed.
  if(widgets->stash_transfer_da)
    gtk_widget_queue_draw(widgets->stash_transfer_da);

  if(widgets->stash_player_da)
    gtk_widget_queue_draw(widgets->stash_player_da);

  if(widgets->stash_relic_da)
    gtk_widget_queue_draw(widgets->stash_relic_da);

  // The held-item overlay floats above the equip area; placing or swapping an
  // item changes the held-item state, so the overlay must be repainted too.
  // Otherwise its last frame (the held item at the drop position) lingers as a
  // ghost slightly offset from the slot, until some other action redraws it.
  if(widgets->held_overlay)
    gtk_widget_queue_draw(widgets->held_overlay);

  update_equip_summary_stats(widgets, widgets->current_character);
  update_resist_damage_tables(widgets, widgets->current_character);
}

// Queue a redraw on all drawing areas (vault, inventory, bag, equip, stashes, overlay).
//   widgets - app state
void
queue_redraw_all(AppWidgets *widgets)
{
  gtk_widget_queue_draw(widgets->vault_drawing_area);
  gtk_widget_queue_draw(widgets->inv_drawing_area);
  gtk_widget_queue_draw(widgets->bag_drawing_area);
  gtk_widget_queue_draw(widgets->equip_drawing_area);

  if(widgets->stash_transfer_da)
    gtk_widget_queue_draw(widgets->stash_transfer_da);

  if(widgets->stash_player_da)
    gtk_widget_queue_draw(widgets->stash_player_da);

  if(widgets->stash_relic_da)
    gtk_widget_queue_draw(widgets->stash_relic_da);

  if(widgets->held_overlay)
    gtk_widget_queue_draw(widgets->held_overlay);
}

// ── Search logic ────────────────────────────────────────────────────────

// Match search text against the full tooltip (name + stats + everything).
// This matches TQVaultAE behavior: searching "Lightning" will find items
// with Lightning stats even if the word isn't in the item name.
//   widgets - app state (provides search_text and translations)
//   item    - the vault item to test
// Returns true if the item's tooltip contains the search text.
bool
item_matches_search(AppWidgets *widgets, TQVaultItem *item)
{
  if(!widgets->search_text[0] || !item || !item->base_name)
    return(false);

  char markup[16384];
  char plain[16384];

  markup[0] = '\0';
  vault_item_format_stats(item, widgets->translations, markup, sizeof(markup));
  strip_pango_markup(plain, sizeof(plain), markup);
  for(char *p = plain; *p; p++)
    *p = (char)tolower((unsigned char)*p);

  // The compiled query auto-detects a grouping/class regex vs literal phrase(s):
  // whitespace is literal (contiguous phrase) and '|' OR's alternative phrases;
  // plain is already lowercased.
  bool match = search_query_match(widgets->search_query, plain);

  if(tqvc_debug && match)
    printf("SEARCH MATCH [%s] in '%s'\n", widgets->search_text, item->base_name);

  return(match);
}

// Check if any item in a sack matches the current search text.
//   widgets - app state
//   sack    - the vault sack to scan
// Returns true if at least one item matches.
static bool
sack_has_match(AppWidgets *widgets, TQVaultSack *sack)
{
  if(!sack || !widgets->search_text[0])
    return(false);

  for(int i = 0; i < sack->num_items; i++)
  {
    if(item_matches_search(widgets, &sack->items[i]))
      return(true);
  }

  return(false);
}

// Execute a search across all vault sacks and character inventory sacks,
// updating bag button CSS classes and redrawing.
//   widgets - app state
void
run_search(AppWidgets *widgets)
{
  // Evaluate vault sacks
  memset(widgets->vault_sack_match, 0, sizeof(widgets->vault_sack_match));
  if(widgets->current_vault)
  {
    for(int i = 0; i < widgets->current_vault->num_sacks && i < 12; i++)
      widgets->vault_sack_match[i] = sack_has_match(widgets, &widgets->current_vault->sacks[i]);
  }

  // Evaluate character inventory sacks (sack 0 = main inv, 1-3 = bags)
  memset(widgets->char_sack_match, 0, sizeof(widgets->char_sack_match));
  if(widgets->current_character)
  {
    for(int i = 0; i < widgets->current_character->num_inv_sacks && i < 4; i++)
      widgets->char_sack_match[i] = sack_has_match(widgets, &widgets->current_character->inv_sacks[i]);
  }

  // Update vault bag button CSS classes
  for(int i = 0; i < 12; i++)
  {
    if(!widgets->vault_bag_btns[i])
      continue;

    if(widgets->search_text[0] && widgets->vault_sack_match[i])
      gtk_widget_add_css_class(widgets->vault_bag_btns[i], "bag-button-search-match");
    else
      gtk_widget_remove_css_class(widgets->vault_bag_btns[i], "bag-button-search-match");
  }

  // Update char bag button CSS classes (char_bag_btns[0-2] map to inv_sacks[1-3])
  for(int i = 0; i < 3; i++)
  {
    if(!widgets->char_bag_btns[i])
      continue;

    if(widgets->search_text[0] && widgets->char_sack_match[i + 1])
      gtk_widget_add_css_class(widgets->char_bag_btns[i], "bag-button-search-match");
    else
      gtk_widget_remove_css_class(widgets->char_bag_btns[i], "bag-button-search-match");
  }

  queue_redraw_all(widgets);
}

// Drop keyboard focus from the search entry so the window's global hotkeys
// (s/d/c/p, ...) work again.  Grabbing the vault drawing area is a no-op --
// a GtkDrawingArea isn't focusable -- so clear the window's focus widget
// outright; with nothing focused, key events bubble straight to the
// window-level key controller (on_key_pressed).
static void
search_release_focus(AppWidgets *widgets)
{
  gtk_window_set_focus(GTK_WINDOW(widgets->main_window), NULL);
}

// Release the main window's keyboard focus on the NEXT main-loop iteration.
// A GtkPopoverMenu restores/moves keyboard focus during the unmap that runs
// AFTER its "closed" signal — especially after a submenu activation (e.g.
// changing a relic/charm completion bonus) — so an immediate release is
// overwritten and the window-level hotkeys (on_key_pressed) stay dead until you
// click a focusable widget (the "switch bags fixes it" symptom).  Deferring to
// idle wins that race.  widgets is app-lifetime, so no source-removal is needed.
static gboolean
release_window_focus_idle(gpointer user_data)
{
  AppWidgets *widgets = user_data;

  gtk_window_set_focus(GTK_WINDOW(widgets->main_window), NULL);
  return(G_SOURCE_REMOVE);
}

void
ui_release_window_focus_deferred(AppWidgets *widgets)
{
  g_idle_add(release_window_focus_idle, widgets);
}

// Fired when the right-click context menu (a GtkPopoverMenu) closes.
void
on_context_menu_closed(GtkPopover *popover, gpointer user_data)
{
  (void)popover;
  ui_release_window_focus_deferred((AppWidgets *)user_data);
}

// Callback: fired when the search entry text changes.
//   entry     - the GtkSearchEntry
//   user_data - AppWidgets*
void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
  AppWidgets *widgets = (AppWidgets *)user_data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

  if(text && text[0])
  {
    size_t len = strlen(text);

    if(len >= sizeof(widgets->search_text))
      len = sizeof(widgets->search_text) - 1;
    memcpy(widgets->search_text, text, len);
    widgets->search_text[len] = '\0';
    // lowercase
    for(char *p = widgets->search_text; *p; p++)
      *p = (char)tolower((unsigned char)*p);
  }
  else
  {
    widgets->search_text[0] = '\0';
    // Keep focus in the entry while the user edits the query down to empty --
    // focus is released only on the explicit Enter/Escape gestures
    // (on_search_key), so backspacing to retype doesn't yank the cursor away.
  }

  // Recompile the matcher from the RAW entry text (regex needs the unfolded
  // metacharacters; case is handled inside).  search_text[0] stays the
  // "search active" flag for run_search / the draw stroke.
  g_clear_pointer(&widgets->search_query, search_query_free);
  widgets->search_query = search_query_compile(text);

  run_search(widgets);
}

// Callback: fired when the search entry emits stop-search (e.g. Escape).
//   entry     - the GtkSearchEntry
//   user_data - AppWidgets*
void
on_search_stop(GtkSearchEntry *entry, gpointer user_data)
{
  (void)entry;
  AppWidgets *widgets = (AppWidgets *)user_data;

  widgets->search_text[0] = '\0';
  gtk_editable_set_text(GTK_EDITABLE(widgets->search_entry), "");
  search_release_focus(widgets);
  run_search(widgets);
}

// Key handling for the search box (capture phase, so it sees these keys
// before the entry's internal GtkText does):
//   Enter  - drop focus, keeping the typed filter active.
//   Escape - clear the filter and drop focus, in a single keypress.
// Either way focus leaves the box so the window's global hotkeys work again.
//   ctrl      - key event controller
//   keyval    - the key that was pressed
//   keycode   - hardware keycode
//   state     - modifier state
//   user_data - AppWidgets*
// Returns TRUE if the event was handled (Enter/Escape), FALSE otherwise.
gboolean
on_search_key(GtkEventControllerKey *ctrl, guint keyval,
              guint keycode, GdkModifierType state, gpointer user_data)
{
  (void)ctrl; (void)keycode; (void)state;

  AppWidgets *widgets = (AppWidgets *)user_data;

  // Enter: keep the filter, just release focus.
  if(keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
     keyval == GDK_KEY_ISO_Enter)
  {
    search_release_focus(widgets);
    return(TRUE);
  }

  // Escape: clear the filter and release focus.
  if(keyval == GDK_KEY_Escape)
  {
    widgets->search_text[0] = '\0';
    gtk_editable_set_text(GTK_EDITABLE(widgets->search_entry), "");
    search_release_focus(widgets);
    run_search(widgets);
    return(TRUE);
  }

  return(FALSE);  // let normal typing through to the entry
}

// ── Copy/Duplicate helpers ──────────────────────────────────────────────

// Create a HeldItem copy of a vault item without removing the original.
//   widgets        - app state
//   src            - source vault item to copy
//   randomize_seed - if true, assign a new random seed to the copy
void
copy_item_to_cursor(AppWidgets *widgets, TQVaultItem *src,
                    bool randomize_seed)
{
  HeldItem *hi = calloc(1, sizeof(HeldItem));

  if(!hi)
    return;

  vault_item_deep_copy(&hi->item, src);
  if(randomize_seed)
    hi->item.seed = (uint32_t)(rand() % 0x7fff);
  hi->source = CONTAINER_VAULT;
  hi->source_sack_idx = -1;
  hi->is_copy = true;
  get_item_dims(widgets, &hi->item, &hi->item_w, &hi->item_h);
  hi->texture = load_item_texture(widgets, hi->item.base_name, hi->item.var1);
  widgets->held_item = hi;
  invalidate_tooltips(widgets);
  queue_redraw_all(widgets);
}

// Create a HeldItem copy of an equipped TQItem without removing the original.
//   widgets        - app state
//   eq             - source equipped item to copy
//   randomize_seed - if true, assign a new random seed to the copy
void
copy_equip_to_cursor(AppWidgets *widgets, TQItem *eq,
                     bool randomize_seed)
{
  HeldItem *hi = calloc(1, sizeof(HeldItem));

  if(!hi)
    return;

  equip_to_vault_item(&hi->item, eq);
  if(randomize_seed)
    hi->item.seed = (uint32_t)(rand() % 0x7fff);
  hi->source = CONTAINER_VAULT;
  hi->source_sack_idx = -1;
  hi->is_copy = true;
  get_item_dims(widgets, &hi->item, &hi->item_w, &hi->item_h);
  hi->texture = load_item_texture(widgets, hi->item.base_name, hi->item.var1);
  widgets->held_item = hi;
  invalidate_tooltips(widgets);
  queue_redraw_all(widgets);
}

// ── Keyboard shortcuts ──────────────────────────────────────────────────

// Populate context_* fields from the current cursor position, performing the
// same hit-test that right-click uses.
//   widgets - app state
// Returns true if an item was found under the cursor.
static bool
set_context_from_cursor(AppWidgets *widgets)
{
  widgets->context_item       = NULL;
  widgets->context_equip_item = NULL;
  widgets->context_source     = CONTAINER_NONE;
  widgets->context_sack_idx   = -1;
  widgets->context_equip_slot = -1;

  GtkWidget *cw = widgets->cursor_widget;

  if(!cw)
    return(false);

  double px   = widgets->cursor_x;
  double py   = widgets->cursor_y;
  double cell = compute_cell_size(widgets);

  // ── Vault ──
  if(cw == widgets->vault_drawing_area && widgets->current_vault)
  {
    int sack_idx = widgets->current_sack;

    if(sack_idx < 0 || sack_idx >= widgets->current_vault->num_sacks)
      return(false);

    TQVaultSack *sack = &widgets->current_vault->sacks[sack_idx];
    int w = gtk_widget_get_width(widgets->vault_drawing_area);
    double c = (cell > 0.0) ? cell : (double)w / 18.0;
    TQVaultItem *hit = find_item_at_cell(widgets, sack, 18, 20, c, px, py, NULL);

    if(!hit)
      return(false);

    widgets->context_item     = hit;
    widgets->context_source   = CONTAINER_VAULT;
    widgets->context_sack_idx = sack_idx;
    return(true);
  }

  // ── Main inventory ──
  if(cw == widgets->inv_drawing_area && widgets->current_character &&
     widgets->current_character->num_inv_sacks >= 1)
  {
    TQVaultSack *sack = &widgets->current_character->inv_sacks[0];
    int w = gtk_widget_get_width(widgets->inv_drawing_area);
    double c = (cell > 0.0) ? cell : (double)w / CHAR_INV_COLS;
    TQVaultItem *hit = find_item_at_cell(widgets, sack,
                                         CHAR_INV_COLS, CHAR_INV_ROWS, c, px, py, NULL);

    if(!hit)
      return(false);

    widgets->context_item     = hit;
    widgets->context_source   = CONTAINER_INV;
    widgets->context_sack_idx = 0;
    return(true);
  }

  // ── Extra bag ──
  if(cw == widgets->bag_drawing_area && widgets->current_character)
  {
    int idx = 1 + widgets->current_char_bag;

    if(idx >= widgets->current_character->num_inv_sacks)
      return(false);

    TQVaultSack *sack = &widgets->current_character->inv_sacks[idx];
    int w = gtk_widget_get_width(widgets->bag_drawing_area);
    double c = (cell > 0.0) ? cell : (double)w / CHAR_BAG_COLS;
    TQVaultItem *hit = find_item_at_cell(widgets, sack,
                                         CHAR_BAG_COLS, CHAR_BAG_ROWS, c, px, py, NULL);

    if(!hit)
      return(false);

    widgets->context_item     = hit;
    widgets->context_source   = CONTAINER_BAG;
    widgets->context_sack_idx = widgets->current_char_bag;
    return(true);
  }

  // ── Equipment ──
  if(cw == widgets->equip_drawing_area && widgets->current_character)
  {
    double cs    = compute_cell_size(widgets);
    double cx0   = 0.0;
    double cx1   = 2.0 * cs + EQUIP_COL_GAP;
    double cx2   = 4.0 * cs + 2.0 * EQUIP_COL_GAP;

    typedef struct { double cx; const EquipSlot *slots; int n; } ColDef;
    ColDef cols[3] = {
      { cx0, COL_LEFT,   (int)(sizeof COL_LEFT   / sizeof COL_LEFT[0])   },
      { cx1, COL_CENTER, (int)(sizeof COL_CENTER / sizeof COL_CENTER[0]) },
      { cx2, COL_RIGHT,  (int)(sizeof COL_RIGHT  / sizeof COL_RIGHT[0])  },
    };

    int hit_slot = -1;

    for(int ci = 0; ci < 3 && hit_slot < 0; ci++)
    {
      double cy = 0.0;

      for(int si = 0; si < cols[ci].n && hit_slot < 0; si++)
      {
        const EquipSlot *sl = &cols[ci].slots[si];
        double bw = (double)sl->box_w * cs;
        double bh = (double)sl->box_h * cs;

        if(px >= cols[ci].cx && px < cols[ci].cx + bw &&
           py >= cy          && py < cy + bh)
          hit_slot = sl->slot_idx;
        cy += bh + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
      }

      if(ci == 1)
      {
        double cy2 = 0.0;

        for(int si = 0; si < cols[1].n; si++)
          cy2 += (double)cols[1].slots[si].box_h * cs + EQUIP_LABEL_H + EQUIP_SLOT_GAP;

        for(int ri = 0; ri < 2 && hit_slot < 0; ri++)
        {
          double rx = cx1 + (double)ri * (cs + EQUIP_COL_GAP / 2.0);
          double bw = RING_SLOTS[ri].box_w * cs;
          double bh = RING_SLOTS[ri].box_h * cs;

          if(px >= rx && px < rx + bw && py >= cy2 && py < cy2 + bh)
            hit_slot = RING_SLOTS[ri].slot_idx;
        }
      }
    }

    if(hit_slot < 0 || hit_slot >= 12)
      return(false);

    TQItem *eq = widgets->current_character->equipment[hit_slot];

    if(!eq || !eq->base_name)
      return(false);

    widgets->context_equip_item = eq;
    widgets->context_source     = CONTAINER_EQUIP;
    widgets->context_equip_slot = hit_slot;
    return(true);
  }

  // ── Caravan stash tabs (transfer / player / relic) ──
  // Each tab is a single sack whose grid dimensions and cell size come from the
  // stash itself; mirrors the click handlers in ui_dnd.c so keyboard shortcuts
  // behave the same as the right-click context menu over these panes.
  {
    struct { GtkWidget *da; TQStash *stash; ContainerType src; } tabs[] = {
      { widgets->stash_transfer_da, widgets->transfer_stash, CONTAINER_TRANSFER },
      { widgets->stash_player_da,   widgets->player_stash,   CONTAINER_PLAYER_STASH },
      { widgets->stash_relic_da,    widgets->relic_vault,    CONTAINER_RELIC_VAULT },
    };

    for(int t = 0; t < (int)(sizeof tabs / sizeof tabs[0]); t++)
    {
      if(cw != tabs[t].da || !tabs[t].stash)
        continue;

      TQStash *stash = tabs[t].stash;
      double c = stash_cell_size(stash, tabs[t].da);
      TQVaultItem *hit = find_item_at_cell(widgets, &stash->sack,
                                           stash->sack_width, stash->sack_height,
                                           c, px, py, NULL);

      if(!hit)
        return(false);

      widgets->context_item     = hit;
      widgets->context_source   = tabs[t].src;
      widgets->context_sack_idx = 0;
      return(true);
    }
  }

  return(false);
}

// Handle keyboard shortcuts: s = open Skill Manager, d = duplicate, c = copy,
// D (Shift+d) = delete.
//   ctrl      - key event controller (unused)
//   keyval    - the key that was pressed
//   keycode   - hardware keycode (unused)
//   state     - modifier state
//   user_data - AppWidgets*
// Returns TRUE if the key was handled.
gboolean
on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
               guint keycode, GdkModifierType state,
               gpointer user_data)
{
  (void)ctrl; (void)keycode;
  AppWidgets *widgets = user_data;

  // Escape clears compare item (before modifier filter)
  if(keyval == GDK_KEY_Escape && widgets->compare_active)
  {
    clear_compare_item(widgets);
    return(TRUE);
  }

  // Ignore when a modifier other than Shift is held
  if(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
    return(FALSE);

  // 'p' (for "pic") copies the currently-shown tooltip to the clipboard as a
  // PNG image, so the item card can be pasted into Discord and other apps.
  if(keyval == GDK_KEY_p && !widgets->held_item)
  {
    if(tooltip_copy_to_clipboard(widgets))
      show_toast(widgets, "Tooltip image copied to clipboard");
    else
      show_toast(widgets, "Hover an item, then press P to copy its tooltip");
    return(TRUE);
  }

  // 's' opens the Skill Manager (when a character is loaded and we're not
  // mid-drag).  A focused text entry consumes the keystroke first, so this
  // never fires while typing in a search box.
  if(keyval == GDK_KEY_s && !widgets->held_item &&
     widgets->skills_btn && gtk_widget_get_sensitive(widgets->skills_btn))
  {
    show_skills_dialog(widgets);
    return(TRUE);
  }

  bool is_dup    = (keyval == GDK_KEY_d);
  bool is_copy   = (keyval == GDK_KEY_c);
  bool is_delete = (keyval == GDK_KEY_D);

  if(!is_dup && !is_copy && !is_delete)
    return(FALSE);

  if(widgets->held_item)
    return(FALSE);

  if(!set_context_from_cursor(widgets))
    return(FALSE);

  const char *action_name = is_dup  ? "item-duplicate"
                           : is_copy ? "item-copy"
                           :           "item-delete";

  g_action_group_activate_action(
    G_ACTION_GROUP(g_application_get_default()), action_name, NULL);

  return(TRUE);
}

// ── Window lifecycle ────────────────────────────────────────────────────

// Action callback: quit the application.
//   action    - the action (unused)
//   parameter - action parameter (unused)
//   user_data - GtkApplication*
void
on_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action; (void)parameter;
  GtkApplication *app = GTK_APPLICATION(user_data);

  g_application_quit(G_APPLICATION(app));
}

// Callback: window close-request. Saves dirty data and prompts for unsaved characters.
//   window    - the main window (unused)
//   user_data - AppWidgets*
// Returns FALSE to allow close, TRUE to block it.
gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
  (void)window;
  AppWidgets *widgets = (AppWidgets *)user_data;

  cancel_held_item(widgets);
  save_vault_if_dirty(widgets);

  // Drop any pending toast auto-hide so it can't fire after teardown.
  if(widgets->toast_timeout_id)
  {
    g_source_remove(widgets->toast_timeout_id);
    widgets->toast_timeout_id = 0;
  }

  // Character data and the stash tabs share one Save/Discard/Cancel prompt so
  // stash edits are no longer saved silently behind the user's back: on Save we
  // persist both; on Discard both are dropped.
  if(has_unsaved_changes(widgets))
  {
    ConfirmUnsaved choice = confirm_unsaved_character(widgets);

    if(choice == CONFIRM_SAVE)
    {
      save_character_if_dirty(widgets);
      save_stashes_if_dirty(widgets);
    }
    else if(choice == CONFIRM_CANCEL)
      return(TRUE);  // block close
    // Discard: don't save character or stashes, allow close
  }

  // Free stash data
  stash_free(widgets->transfer_stash);
  widgets->transfer_stash = NULL;
  stash_free(widgets->player_stash);
  widgets->player_stash = NULL;
  stash_free(widgets->relic_vault);
  widgets->relic_vault = NULL;

  // Unparent the context-menu popover so the drawing area it's attached to
  // doesn't complain about leftover children during finalization.
  if(widgets->context_menu)
  {
    if(widgets->context_parent)
      gtk_widget_unparent(widgets->context_menu);
    g_object_unref(widgets->context_menu);
    widgets->context_menu = NULL;
    widgets->context_parent = NULL;
  }

  if(widgets->bag_menu)
  {
    if(widgets->bag_menu_parent)
      gtk_widget_unparent(widgets->bag_menu);
    g_object_unref(widgets->bag_menu);
    widgets->bag_menu = NULL;
    widgets->bag_menu_parent = NULL;
  }

  if(widgets->tooltip_popover)
  {
    if(widgets->tooltip_parent)
      gtk_widget_unparent(widgets->tooltip_popover);
    g_object_unref(widgets->tooltip_popover);
    widgets->tooltip_popover = NULL;
    widgets->tooltip_parent = NULL;
  }

  if(widgets->compare_active)
    vault_item_free_strings(&widgets->compare_item);

  // Block combo handlers before window destruction -- GTK4 may fire
  // notify::selected as it tears down the dropdown models, which would
  // overwrite the saved last_character/last_vault config.
  if(widgets->char_combo_handler && widgets->character_combo)
    g_signal_handler_block(widgets->character_combo, widgets->char_combo_handler);

  if(widgets->vault_combo_handler && widgets->vault_combo)
    g_signal_handler_block(widgets->vault_combo, widgets->vault_combo_handler);

  return(FALSE); // allow default close behaviour
}

// Callback: Settings button clicked.
//   btn       - the button (unused)
//   user_data - AppWidgets*
void
on_settings_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  on_settings_action(NULL, NULL, user_data);
}

// Capture-phase motion on the overlay: tracks cursor in overlay coordinates
// so the held item stays visible even between drawing areas.
//   ctrl      - motion controller (unused)
//   x, y      - cursor position in overlay coordinates
//   user_data - AppWidgets*
void
on_overlay_motion(GtkEventControllerMotion *ctrl,
                  double x, double y, gpointer user_data)
{
  (void)ctrl;
  AppWidgets *widgets = (AppWidgets *)user_data;

  widgets->win_cursor_x = x;
  widgets->win_cursor_y = y;
  if(widgets->held_item && widgets->held_overlay)
    gtk_widget_queue_draw(widgets->held_overlay);
}

// ── Application window layout ──────────────────────────────────────────

