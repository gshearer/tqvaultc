#include "ui.h"
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

  char *bitmap_path = NULL;
  TQArzRecordData *data = asset_get_dbr(base_name);

  if(data)
  {
    bitmap_path = arz_record_get_string(data, "bitmap", NULL);
    if(!bitmap_path)
      bitmap_path = arz_record_get_string(data, "artifactBitmap", NULL);

    if(!bitmap_path)
    {
      // For relics/charms: use shardBitmap when incomplete, relicBitmap when complete.
      // completedRelicLevel from the DBR tells us how many shards are needed.
      char *relic_bmp = arz_record_get_string(data, "relicBitmap", NULL);
      char *shard_bmp = arz_record_get_string(data, "shardBitmap", NULL);

      if(relic_bmp && shard_bmp)
      {
        int max_shards = arz_record_get_int(data, "completedRelicLevel", 0, NULL);

        if(max_shards > 0 && var1 < (uint32_t)max_shards)
        {
          bitmap_path = shard_bmp;
          free(relic_bmp);
        }
        else
        {
          bitmap_path = relic_bmp;
          free(shard_bmp);
        }
      }
      else if(relic_bmp)
        bitmap_path = relic_bmp;
      else if(shard_bmp)
        bitmap_path = shard_bmp;
    }
  }

  char tex_path[1024];
  const char *source = bitmap_path ? bitmap_path : base_name;

  strncpy(tex_path, source, sizeof(tex_path));
  tex_path[sizeof(tex_path)-1] = '\0';
  if(bitmap_path)
    free(bitmap_path);

  char *dot = strrchr(tex_path, '.');

  if(dot)
    strcpy(dot, ".tex");
  else
    strcat(tex_path, ".tex");

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

  // Case-insensitive substring checks matching the C# reference logic
  for(const char *p = base_name; *p; p++)
  {
    if(strncasecmp(p, "animalrelics", 12) == 0)
      return(true);
    if(strncasecmp(p, "\\relics\\", 8) == 0)
      return(true);
    if(strncasecmp(p, "\\charms\\", 8) == 0)
      return(true);
  }

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

  // Only relics, charms, potions, and scrolls are stackable
  const char *b = a->base_name;

  if(strcasestr(b, "\\relics\\"))
    return(true);
  if(strcasestr(b, "\\charms\\"))
    return(true);
  if(strcasestr(b, "\\animalrelic"))
    return(true);
  if(strcasestr(b, "\\oneshot\\"))
    return(true);
  if(strcasestr(b, "\\scrolls\\"))
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
  if(strcasestr(base_name, "\\artifacts\\") == NULL)
    return(false);
  if(strcasestr(base_name, "\\arcaneformulae\\") != NULL)
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

// Update the Save Character button's sensitivity based on char_dirty.
//   widgets - app state
void
update_save_button_sensitivity(AppWidgets *widgets)
{
  if(widgets->save_char_btn)
    gtk_widget_set_sensitive(widgets->save_char_btn, widgets->char_dirty);
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

// Strip Pango/HTML markup tags from a string, producing plain text.
// Decodes common XML entities (&amp; &lt; &gt; &apos; &quot;).
//   dst      - output buffer
//   dst_size - size of dst (must be at least as large as src)
//   src      - input markup string
static void
strip_pango_markup(char *dst, size_t dst_size, const char *src)
{
  size_t di = 0;
  bool in_tag = false;

  for(const char *p = src; *p && di + 1 < dst_size; p++)
  {
    if(*p == '<')
    {
      in_tag = true;
      continue;
    }

    if(*p == '>')
    {
      in_tag = false;
      continue;
    }

    if(in_tag)
      continue;

    if(*p == '&')
    {
      if(strncmp(p, "&amp;", 5) == 0)
      {
        dst[di++] = '&';
        p += 4;
      }
      else if(strncmp(p, "&lt;", 4) == 0)
      {
        dst[di++] = '<';
        p += 3;
      }
      else if(strncmp(p, "&gt;", 4) == 0)
      {
        dst[di++] = '>';
        p += 3;
      }
      else if(strncmp(p, "&apos;", 6) == 0)
      {
        dst[di++] = '\'';
        p += 5;
      }
      else if(strncmp(p, "&quot;", 6) == 0)
      {
        dst[di++] = '"';
        p += 5;
      }
      else
      {
        dst[di++] = *p;
      }
    }
    else
    {
      dst[di++] = *p;
    }
  }
  dst[di] = '\0';
}

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

  bool match = strstr(plain, widgets->search_text) != NULL;

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

// Callback: fired when the search entry text changes.
//   entry     - the GtkSearchEntry
//   user_data - AppWidgets*
static void
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
    // Release focus from the search box when text is cleared (e.g. via
    // the clear button) so keyboard shortcuts become accessible again.
    gtk_widget_grab_focus(widgets->vault_drawing_area);
  }

  run_search(widgets);
}

// Callback: fired when the search entry emits stop-search (e.g. Escape).
//   entry     - the GtkSearchEntry
//   user_data - AppWidgets*
static void
on_search_stop(GtkSearchEntry *entry, gpointer user_data)
{
  (void)entry;
  AppWidgets *widgets = (AppWidgets *)user_data;

  widgets->search_text[0] = '\0';
  gtk_editable_set_text(GTK_EDITABLE(widgets->search_entry), "");
  gtk_widget_grab_focus(widgets->vault_drawing_area);
  run_search(widgets);
}

// Escape in the search box: clear text + release focus in a single keypress.
//   ctrl      - key event controller
//   keyval    - the key that was pressed
//   keycode   - hardware keycode
//   state     - modifier state
//   user_data - AppWidgets*
// Returns TRUE if the event was handled (Escape), FALSE otherwise.
static gboolean
on_search_key(GtkEventControllerKey *ctrl, guint keyval,
              guint keycode, GdkModifierType state, gpointer user_data)
{
  (void)ctrl; (void)keycode; (void)state;

  if(keyval != GDK_KEY_Escape)
    return(FALSE);

  AppWidgets *widgets = (AppWidgets *)user_data;

  widgets->search_text[0] = '\0';
  gtk_editable_set_text(GTK_EDITABLE(widgets->search_entry), "");
  gtk_widget_grab_focus(widgets->vault_drawing_area);
  run_search(widgets);
  return(TRUE);  // stop further handling
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

  return(false);
}

// Handle keyboard shortcuts: d = duplicate, c = copy, D (Shift+d) = delete.
//   ctrl      - key event controller (unused)
//   keyval    - the key that was pressed
//   keycode   - hardware keycode (unused)
//   state     - modifier state
//   user_data - AppWidgets*
// Returns TRUE if the key was handled.
static gboolean
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
static void
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
static gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
  (void)window;
  AppWidgets *widgets = (AppWidgets *)user_data;

  cancel_held_item(widgets);
  save_vault_if_dirty(widgets);
  save_stashes_if_dirty(widgets);

  if(widgets->char_dirty)
  {
    int choice = confirm_unsaved_character(widgets);

    if(choice == 0)
      save_character_if_dirty(widgets);
    else if(choice == 2)
      return(TRUE);  // Cancel -- block close
    // Discard: don't save, allow close
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
static void
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
static void
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

// Main application activate callback. Builds the entire UI layout.
//   app       - the GtkApplication
//   user_data - unused
void
ui_app_activate(GtkApplication *app, gpointer user_data)
{
  (void)user_data;
  AppWidgets *widgets = g_malloc0(sizeof(AppWidgets));

  widgets->texture_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  widgets->last_equip_tooltip_slot = -1;
  widgets->context_equip_slot = -1;

  // Right-click context menu: actions + popover
  register_context_actions(app, widgets);

  GMenu *ctx_menu = g_menu_new();

  widgets->context_menu_model = ctx_menu;  // kept alive for dynamic rebuild

  widgets->context_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(ctx_menu));
  g_object_ref_sink(widgets->context_menu);  // own the popover so unparent won't destroy it
  gtk_popover_set_has_arrow(GTK_POPOVER(widgets->context_menu), FALSE);
  gtk_widget_set_halign(widgets->context_menu, GTK_ALIGN_START);

  // Bag context menu: actions + popover
  register_bag_menu_actions(app, widgets);
  widgets->bag_menu_model = g_menu_new();
  widgets->bag_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(widgets->bag_menu_model));
  g_object_ref_sink(widgets->bag_menu);
  gtk_popover_set_has_arrow(GTK_POPOVER(widgets->bag_menu), FALSE);
  gtk_widget_set_halign(widgets->bag_menu, GTK_ALIGN_START);

  // Instant tooltip popover (zero-delay, replaces GTK4's 500ms tooltip)
  widgets->tooltip_popover = gtk_popover_new();
  g_object_ref_sink(widgets->tooltip_popover);
  gtk_popover_set_has_arrow(GTK_POPOVER(widgets->tooltip_popover), FALSE);
  gtk_popover_set_autohide(GTK_POPOVER(widgets->tooltip_popover), FALSE);
  gtk_widget_set_can_focus(widgets->tooltip_popover, FALSE);
  gtk_widget_set_can_target(widgets->tooltip_popover, FALSE);

  widgets->tooltip_label = gtk_label_new(NULL);
  gtk_label_set_use_markup(GTK_LABEL(widgets->tooltip_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(widgets->tooltip_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(widgets->tooltip_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(widgets->tooltip_label), 60);
  gtk_widget_set_margin_start(widgets->tooltip_label, 6);
  gtk_widget_set_margin_end(widgets->tooltip_label, 6);
  gtk_widget_set_margin_top(widgets->tooltip_label, 4);
  gtk_widget_set_margin_bottom(widgets->tooltip_label, 4);

  GtkWidget *tip_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tip_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(tip_scroll), 800);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(tip_scroll), TRUE);
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(tip_scroll), 350);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(tip_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tip_scroll), widgets->tooltip_label);

  // Compare tooltip label and scrolled window (shown/hidden inside same popover)
  widgets->compare_label = gtk_label_new(NULL);
  gtk_label_set_use_markup(GTK_LABEL(widgets->compare_label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(widgets->compare_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(widgets->compare_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(widgets->compare_label), 60);
  gtk_widget_set_margin_start(widgets->compare_label, 6);
  gtk_widget_set_margin_end(widgets->compare_label, 6);
  gtk_widget_set_margin_top(widgets->compare_label, 4);
  gtk_widget_set_margin_bottom(widgets->compare_label, 4);

  widgets->compare_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widgets->compare_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(widgets->compare_scroll), 800);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(widgets->compare_scroll), TRUE);
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(widgets->compare_scroll), 350);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(widgets->compare_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(widgets->compare_scroll), widgets->compare_label);

  widgets->compare_separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);

  // Pack main + separator + compare into an HBox inside the popover
  GtkWidget *tip_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(tip_hbox), tip_scroll);
  gtk_box_append(GTK_BOX(tip_hbox), widgets->compare_separator);
  gtk_box_append(GTK_BOX(tip_hbox), widgets->compare_scroll);
  gtk_popover_set_child(GTK_POPOVER(widgets->tooltip_popover), tip_hbox);
  gtk_widget_add_css_class(widgets->tooltip_popover, "item-tooltip");

  // Initially hide compare section
  gtk_widget_set_visible(widgets->compare_scroll, FALSE);
  gtk_widget_set_visible(widgets->compare_separator, FALSE);
  widgets->tooltip_parent = NULL;

  GdkPixbuf *test_relic = texture_load("Items\\AnimalRelics\\AnimalPart07B_L.tex");

  if(test_relic)
  {
    if(tqvc_debug)
      printf("DEBUG: AnimalPart07B_L.tex size: %dx%d\n", gdk_pixbuf_get_width(test_relic), gdk_pixbuf_get_height(test_relic));
    g_object_unref(test_relic);
  }

  // Force GTK to use Adwaita's dark variant. On Linux this honors the
  // user's system theme by default, but on Windows there's typically no
  // GTK theme installed and we'd otherwise inherit the bright Adwaita
  // light defaults which clash with our dark inventory grid CSS.
  g_object_set(gtk_settings_get_default(),
               "gtk-application-prefer-dark-theme", TRUE,
               NULL);

  GtkCssProvider *provider = gtk_css_provider_new();

  gtk_css_provider_load_from_resource(provider, "/org/tqvaultc/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  GtkWidget *window = gtk_application_window_new(app);

  widgets->main_window = window;
  char title[64];

  snprintf(title, sizeof(title), "TQVaultC v%s (Build #%d)", TQVAULTC_VERSION, TQVAULTC_BUILD_NUMBER);
  gtk_window_set_title(GTK_WINDOW(window), title);
  gtk_window_set_default_size(GTK_WINDOW(window), 1600, 900);

  GtkEventController *key_ctrl = gtk_event_controller_key_new();

  g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), widgets);
  gtk_widget_add_controller(window, key_ctrl);

  if(global_config.game_folder)
  {
    widgets->translations = translation_init();
    char trans_path[1024];

    snprintf(trans_path, sizeof(trans_path), "%s/Text/Text_EN.arc", global_config.game_folder);
    translation_load_from_arc(widgets->translations, trans_path);
  }

  GtkWidget *header = gtk_header_bar_new();

  GtkWidget *settings_btn = gtk_button_new_with_label("Settings");

  g_signal_connect(settings_btn, "clicked", G_CALLBACK(on_settings_btn_clicked), widgets);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), settings_btn);

  GtkWidget *about_btn = gtk_button_new_with_label("About");

  g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_btn_clicked), widgets);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), about_btn);

  GtkWidget *database_btn = gtk_button_new_with_label("Database");

  g_signal_connect(database_btn, "clicked", G_CALLBACK(on_database_btn_clicked), widgets);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), database_btn);

  // ── Manage Vaults dropdown ──
  GMenu *vault_menu = g_menu_new();

  g_menu_append(vault_menu, "Duplicate current vault", "win.dup-vault");
  g_menu_append(vault_menu, "Rename current vault", "win.rename-vault");
  g_menu_append(vault_menu, "Delete current vault", "win.delete-vault");
  g_menu_append(vault_menu, "Create new vault", "win.new-vault");

  GtkWidget *vault_menu_btn = gtk_menu_button_new();

  gtk_menu_button_set_label(GTK_MENU_BUTTON(vault_menu_btn), "Manage Vaults");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(vault_menu_btn), G_MENU_MODEL(vault_menu));
  g_object_unref(vault_menu);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), vault_menu_btn);

  // ── Manage Characters dropdown ──
  GMenu *char_menu = g_menu_new();

  g_menu_append(char_menu, "Duplicate current character", "win.dup-char");
  g_menu_append(char_menu, "Rename current character", "win.rename-char");
  g_menu_append(char_menu, "Delete current character", "win.delete-char");

  GtkWidget *char_menu_btn = gtk_menu_button_new();

  gtk_menu_button_set_label(GTK_MENU_BUTTON(char_menu_btn), "Manage Characters");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(char_menu_btn), G_MENU_MODEL(char_menu));
  g_object_unref(char_menu);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), char_menu_btn);

  GtkWidget *view_build_btn = gtk_button_new_with_label("View Build");

  g_signal_connect(view_build_btn, "clicked", G_CALLBACK(on_view_build_clicked), widgets);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), view_build_btn);

  widgets->checklist_btn = gtk_button_new_with_label("Checklist");
  g_signal_connect(widgets->checklist_btn, "clicked", G_CALLBACK(on_checklist_btn_clicked), widgets);
  gtk_widget_set_sensitive(widgets->checklist_btn, FALSE);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->checklist_btn);

  widgets->skills_btn = gtk_button_new_with_label("Skills");
  g_signal_connect(widgets->skills_btn, "clicked", G_CALLBACK(on_skills_btn_clicked), widgets);
  gtk_widget_set_sensitive(widgets->skills_btn, FALSE);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->skills_btn);

  widgets->stats_btn = gtk_button_new_with_label("Attributes");
  g_signal_connect(widgets->stats_btn, "clicked", G_CALLBACK(on_stats_btn_clicked), widgets);
  gtk_widget_set_sensitive(widgets->stats_btn, FALSE);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->stats_btn);

  // Save Character button -- grayed out when no unsaved changes
  widgets->save_char_btn = gtk_button_new_with_label("Save Character");
  g_signal_connect(widgets->save_char_btn, "clicked", G_CALLBACK(on_save_char_clicked), widgets);
  gtk_widget_set_sensitive(widgets->save_char_btn, FALSE);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->save_char_btn);

  GtkWidget *refresh_char_btn = gtk_button_new_with_label("Refresh Character");

  g_signal_connect(refresh_char_btn, "clicked", G_CALLBACK(on_refresh_char_clicked), widgets);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh_char_btn);

  // Search entry in the header bar -- avoids layout interference with grids
  widgets->search_entry = gtk_search_entry_new();
  gtk_widget_set_size_request(widgets->search_entry, 200, -1);
  g_signal_connect(widgets->search_entry, "search-changed", G_CALLBACK(on_search_changed), widgets);
  g_signal_connect(widgets->search_entry, "stop-search", G_CALLBACK(on_search_stop), widgets);

  GtkEventController *search_key = gtk_event_controller_key_new();

  g_signal_connect(search_key, "key-pressed", G_CALLBACK(on_search_key), widgets);
  gtk_widget_add_controller(widgets->search_entry, search_key);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->search_entry);

  gtk_window_set_titlebar(GTK_WINDOW(window), header);

  // ── Top-level overlay: holds main_hbox + transparent held-item layer ──
  GtkWidget *overlay = gtk_overlay_new();

  gtk_widget_set_hexpand(overlay, TRUE);
  gtk_widget_set_vexpand(overlay, TRUE);
  gtk_window_set_child(GTK_WINDOW(window), overlay);

  // ── Top-level horizontal split: vault (left) | char panel (right) ──
  GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  widgets->main_hbox = main_hbox;
  gtk_widget_set_hexpand(main_hbox, TRUE);
  gtk_widget_set_vexpand(main_hbox, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), main_hbox);

  // Transparent overlay drawing area: renders held item between panes
  widgets->held_overlay = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->held_overlay, TRUE);
  gtk_widget_set_vexpand(widgets->held_overlay, TRUE);
  gtk_widget_set_can_target(widgets->held_overlay, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widgets->held_overlay),
                                 held_overlay_draw_cb, widgets, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), widgets->held_overlay);

  // Capture-phase motion on the overlay: tracks cursor globally
  GtkEventController *overlay_motion = gtk_event_controller_motion_new();

  gtk_event_controller_set_propagation_phase(overlay_motion, GTK_PHASE_CAPTURE);
  g_signal_connect(overlay_motion, "motion", G_CALLBACK(on_overlay_motion), widgets);
  gtk_widget_add_controller(overlay, overlay_motion);

  // ── Left: vault area ──
  GtkWidget *main_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_hexpand(main_area, TRUE);
  gtk_widget_set_vexpand(main_area, TRUE);
  gtk_box_append(GTK_BOX(main_hbox), main_area);

  widgets->vault_combo = gtk_drop_down_new_from_strings(NULL);
  gtk_box_append(GTK_BOX(main_area), widgets->vault_combo);
  widgets->vault_combo_handler = g_signal_connect(widgets->vault_combo,
    "notify::selected", G_CALLBACK(on_vault_changed), widgets);

  // Vault bag buttons -- three-state textures (down/up/over)
  GtkWidget *bag_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_box_append(GTK_BOX(main_area), bag_hbox);
  {
    const char *tex_paths[3] = {
      "InGameUI\\characterscreen\\inventorybagdown01.tex",
      "InGameUI\\characterscreen\\inventorybagup01.tex",
      "InGameUI\\characterscreen\\inventorybagover01.tex",
    };
    GdkPixbuf *base[3] = {NULL, NULL, NULL};

    for(int s = 0; s < 3; s++)
    {
      GdkPixbuf *raw = texture_load(tex_paths[s]);

      if(raw)
      {
        base[s] = gdk_pixbuf_scale_simple(raw, 40, 36, GDK_INTERP_BILINEAR);
        g_object_unref(raw);
      }
    }

    bool have_tex = (base[0] && base[1] && base[2]);

    for(int i = 0; i < 12; i++)
    {
      GtkWidget *btn;

      if(have_tex)
      {
        for(int s = 0; s < 3; s++)
          widgets->vault_bag_pix[s][i] = texture_create_with_number(base[s], i + 1);

        int init_state = (i == 0) ? BAG_UP : BAG_DOWN;

        btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "bag-button");
        gtk_widget_set_size_request(btn, 40, 36);
        set_bag_btn_image(btn, widgets->vault_bag_pix[init_state][i]);
      }
      else
      {
        char label[4];

        snprintf(label, sizeof(label), "%d", i + 1);
        btn = gtk_button_new_with_label(label);
      }
      widgets->vault_bag_btns[i] = btn;
      g_object_set_data(G_OBJECT(btn), "bag-index", GINT_TO_POINTER(i));
      g_signal_connect(btn, "clicked", G_CALLBACK(on_bag_clicked), widgets);

      GtkEventControllerMotion *hover = GTK_EVENT_CONTROLLER_MOTION(gtk_event_controller_motion_new());

      g_signal_connect(hover, "enter", G_CALLBACK(on_vault_bag_hover_enter), widgets);
      g_signal_connect(hover, "leave", G_CALLBACK(on_vault_bag_hover_leave), widgets);
      gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(hover));

      // Right-click for bag context menu
      GtkGesture *rclick = gtk_gesture_click_new();

      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), 3);
      g_signal_connect(rclick, "pressed", G_CALLBACK(on_vault_bag_right_click), widgets);
      gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(rclick));
      gtk_box_append(GTK_BOX(bag_hbox), btn);
    }

    for(int s = 0; s < 3; s++)
    {
      if(base[s])
        g_object_unref(base[s]);
    }
  }

  widgets->vault_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->vault_drawing_area, TRUE);
  gtk_widget_set_vexpand(widgets->vault_drawing_area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widgets->vault_drawing_area), vault_draw_cb, widgets, NULL);
  g_signal_connect(widgets->vault_drawing_area, "resize", G_CALLBACK(on_vault_resize), widgets);

  // Click + motion for vault
  GtkGesture *vault_click = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(vault_click), 0);
  g_signal_connect(vault_click, "pressed", G_CALLBACK(on_vault_click), widgets);
  gtk_widget_add_controller(widgets->vault_drawing_area, GTK_EVENT_CONTROLLER(vault_click));

  GtkEventController *vault_motion = gtk_event_controller_motion_new();

  g_signal_connect(vault_motion, "motion", G_CALLBACK(on_motion), widgets);
  g_signal_connect(vault_motion, "leave", G_CALLBACK(on_motion_leave), widgets);
  gtk_widget_add_controller(widgets->vault_drawing_area, vault_motion);
  gtk_box_append(GTK_BOX(main_area), widgets->vault_drawing_area);

  // ── Right: character panel ──
  GtkWidget *char_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_hexpand(char_panel, TRUE);
  gtk_widget_set_vexpand(char_panel, TRUE);
  gtk_widget_set_margin_start(char_panel, 6);
  gtk_box_append(GTK_BOX(main_hbox), char_panel);

  widgets->character_combo = gtk_drop_down_new_from_strings(NULL);
  gtk_box_append(GTK_BOX(char_panel), widgets->character_combo);
  widgets->char_combo_handler = g_signal_connect(widgets->character_combo,
    "notify::selected", G_CALLBACK(on_character_changed), widgets);

  // Inventory + bag grid layout
  GtkWidget *inv_bag_grid = gtk_grid_new();

  gtk_grid_set_column_spacing(GTK_GRID(inv_bag_grid), 4);
  gtk_grid_set_row_spacing(GTK_GRID(inv_bag_grid), 10);
  gtk_widget_set_hexpand(inv_bag_grid, TRUE);
  gtk_widget_set_vexpand(inv_bag_grid, TRUE);
  gtk_box_append(GTK_BOX(char_panel), inv_bag_grid);

  // Row 0, col 1: bag icon buttons
  GtkWidget *char_bag_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_grid_attach(GTK_GRID(inv_bag_grid), char_bag_hbox, 1, 0, 1, 1);

  {
    const char *tex_paths[3] = {
      "InGameUI\\characterscreen\\inventorybagdown01.tex",
      "InGameUI\\characterscreen\\inventorybagup01.tex",
      "InGameUI\\characterscreen\\inventorybagover01.tex",
    };
    GdkPixbuf *cbase[3] = {NULL, NULL, NULL};

    for(int s = 0; s < 3; s++)
    {
      GdkPixbuf *raw = texture_load(tex_paths[s]);

      if(raw)
      {
        cbase[s] = gdk_pixbuf_scale_simple(raw, 40, 36, GDK_INTERP_BILINEAR);
        g_object_unref(raw);
      }
    }

    bool have_tex = (cbase[0] && cbase[1] && cbase[2]);

    for(int i = 0; i < 3; i++)
    {
      GtkWidget *btn;

      if(have_tex)
      {
        for(int s = 0; s < 3; s++)
          widgets->char_bag_pix[s][i] = texture_create_with_number(cbase[s], i + 1);

        int init_state = (i == 0) ? BAG_UP : BAG_DOWN;

        btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "bag-button");
        gtk_widget_set_size_request(btn, 40, 36);
        set_bag_btn_image(btn, widgets->char_bag_pix[init_state][i]);
      }
      else
      {
        char label[4];

        snprintf(label, sizeof(label), "%d", i + 1);
        btn = gtk_button_new_with_label(label);
      }
      widgets->char_bag_btns[i] = btn;
      g_object_set_data(G_OBJECT(btn), "bag-index", GINT_TO_POINTER(i));
      g_signal_connect(btn, "clicked", G_CALLBACK(on_char_bag_clicked), widgets);

      GtkEventControllerMotion *hover = GTK_EVENT_CONTROLLER_MOTION(gtk_event_controller_motion_new());

      g_signal_connect(hover, "enter", G_CALLBACK(on_char_bag_hover_enter), widgets);
      g_signal_connect(hover, "leave", G_CALLBACK(on_char_bag_hover_leave), widgets);
      gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(hover));

      // Right-click for bag context menu
      GtkGesture *crclick = gtk_gesture_click_new();

      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(crclick), 3);
      g_signal_connect(crclick, "pressed", G_CALLBACK(on_char_bag_right_click), widgets);
      gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(crclick));
      gtk_box_append(GTK_BOX(char_bag_hbox), btn);
    }

    for(int s = 0; s < 3; s++)
    {
      if(cbase[s])
        g_object_unref(cbase[s]);
    }
  }

  // Row 1, col 0: main inventory 12x5
  widgets->inv_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->inv_drawing_area, TRUE);
  gtk_widget_set_vexpand(widgets->inv_drawing_area, TRUE);
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->inv_drawing_area),
                                     CHAR_INV_COLS * 34);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widgets->inv_drawing_area),
                                 inv_draw_cb, widgets, NULL);

  GtkGesture *inv_click = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(inv_click), 0);
  g_signal_connect(inv_click, "pressed", G_CALLBACK(on_inv_click), widgets);
  gtk_widget_add_controller(widgets->inv_drawing_area, GTK_EVENT_CONTROLLER(inv_click));

  GtkEventController *inv_motion = gtk_event_controller_motion_new();

  g_signal_connect(inv_motion, "motion", G_CALLBACK(on_motion), widgets);
  g_signal_connect(inv_motion, "leave", G_CALLBACK(on_motion_leave), widgets);
  gtk_widget_add_controller(widgets->inv_drawing_area, inv_motion);
  gtk_grid_attach(GTK_GRID(inv_bag_grid), widgets->inv_drawing_area, 0, 1, 1, 1);

  // Row 1, col 1: extra bag 8x5
  widgets->bag_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->bag_drawing_area, TRUE);
  gtk_widget_set_vexpand(widgets->bag_drawing_area, TRUE);
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->bag_drawing_area),
                                     CHAR_BAG_COLS * 26);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widgets->bag_drawing_area),
                                 bag_draw_cb, widgets, NULL);

  GtkGesture *bag_click = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bag_click), 0);
  g_signal_connect(bag_click, "pressed", G_CALLBACK(on_bag_click), widgets);
  gtk_widget_add_controller(widgets->bag_drawing_area, GTK_EVENT_CONTROLLER(bag_click));

  GtkEventController *bag_motion = gtk_event_controller_motion_new();

  g_signal_connect(bag_motion, "motion", G_CALLBACK(on_motion), widgets);
  g_signal_connect(bag_motion, "leave", G_CALLBACK(on_motion_leave), widgets);
  gtk_widget_add_controller(widgets->bag_drawing_area, bag_motion);
  gtk_grid_attach(GTK_GRID(inv_bag_grid), widgets->bag_drawing_area, 1, 1, 1, 1);

  // Bottom section: equip+stats on left, tables stacked on right.
  GtkWidget *bottom_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_vexpand(bottom_hbox, FALSE);
  gtk_box_append(GTK_BOX(char_panel), bottom_hbox);

  // Left column: stats above equipment
  GtkWidget *equip_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_hexpand(equip_col, FALSE);
  gtk_widget_set_vexpand(equip_col, FALSE);
  gtk_box_append(GTK_BOX(bottom_hbox), equip_col);

  // Stats above equipment -- wide compact grid
  GtkWidget *stats_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_set_hexpand(stats_frame, FALSE);
  gtk_widget_set_vexpand(stats_frame, FALSE);
  gtk_widget_set_valign(stats_frame, GTK_ALIGN_START);
  gtk_widget_add_css_class(stats_frame, "stats-frame");
  gtk_box_append(GTK_BOX(equip_col), stats_frame);

  // name_label: kept in the widget hierarchy for ancestor lookups
  widgets->name_label = gtk_label_new("");
  gtk_widget_set_visible(widgets->name_label, FALSE);
  gtk_box_append(GTK_BOX(stats_frame), widgets->name_label);

  GtkWidget *stats_grid = gtk_grid_new();

  gtk_grid_set_column_spacing(GTK_GRID(stats_grid), 2);
  gtk_grid_set_row_spacing(GTK_GRID(stats_grid), 0);
  gtk_widget_add_css_class(stats_grid, "stats-grid");
  gtk_box_append(GTK_BOX(stats_frame), stats_grid);

  int sg_row = 0; // current grid row

  // Helper: place a key+value pair at (col, row) spanning 1 column
  #define STAT_CELL(col, key_text, val_ptr) do { \
    GtkWidget *_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3); \
    GtkWidget *_k = gtk_label_new(key_text); \
    gtk_widget_add_css_class(_k, "stats-cell-key"); \
    gtk_box_append(GTK_BOX(_box), _k); \
    *(val_ptr) = gtk_label_new("-"); \
    gtk_widget_add_css_class(*(val_ptr), "stats-cell-val"); \
    gtk_box_append(GTK_BOX(_box), *(val_ptr)); \
    gtk_grid_attach(GTK_GRID(stats_grid), _box, (col), sg_row, 1, 1); \
  } while(0)

  // Row 0: Level, Mastery 1, Mastery 2
  STAT_CELL(0, "Lv",  &widgets->level_label);
  STAT_CELL(1, "",     &widgets->mastery1_label);
  STAT_CELL(2, "",     &widgets->mastery2_label);
  sg_row++;

  // Row 1: Str, Dex, Int
  STAT_CELL(0, "Str",  &widgets->strength_label);
  STAT_CELL(1, "Dex",  &widgets->dexterity_label);
  STAT_CELL(2, "Int",  &widgets->intelligence_label);
  sg_row++;

  // Row 2: HP, MP, K/D
  STAT_CELL(0, "HP",   &widgets->health_label);
  STAT_CELL(1, "MP",   &widgets->mana_label);

  // Combined kills/deaths cell
  {
    GtkWidget *kdbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    GtkWidget *kk = gtk_label_new("K");

    gtk_widget_add_css_class(kk, "stats-cell-key");
    gtk_box_append(GTK_BOX(kdbox), kk);

    widgets->kills_label = gtk_label_new("-");
    gtk_widget_add_css_class(widgets->kills_label, "stats-cell-val");
    gtk_box_append(GTK_BOX(kdbox), widgets->kills_label);

    GtkWidget *dk = gtk_label_new("D");

    gtk_widget_add_css_class(dk, "stats-cell-key");
    gtk_box_append(GTK_BOX(kdbox), dk);

    widgets->deaths_label = gtk_label_new("-");
    gtk_widget_add_css_class(widgets->deaths_label, "stats-cell-val");
    gtk_box_append(GTK_BOX(kdbox), widgets->deaths_label);
    gtk_grid_attach(GTK_GRID(stats_grid), kdbox, 2, sg_row, 1, 1);
  }
  sg_row++;

  // Row 3: Armor (total from equipped items)
  STAT_CELL(0, "Armor", &widgets->armor_label);

  #undef STAT_CELL

  // Equipment drawing area
  widgets->equip_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->equip_drawing_area, FALSE);
  gtk_widget_set_vexpand(widgets->equip_drawing_area, FALSE);
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->equip_drawing_area),
                                     6 * 50 + 2 * (int)EQUIP_COL_GAP);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widgets->equip_drawing_area),
                                      12 * 50 +
                                      3 * (int)EQUIP_LABEL_H +
                                      2 * (int)EQUIP_SLOT_GAP);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widgets->equip_drawing_area),
                                 equip_draw_cb, widgets, NULL);

  GtkGesture *equip_click = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(equip_click), 0);
  g_signal_connect(equip_click, "pressed", G_CALLBACK(on_equip_click), widgets);
  gtk_widget_add_controller(widgets->equip_drawing_area, GTK_EVENT_CONTROLLER(equip_click));

  GtkEventController *equip_motion = gtk_event_controller_motion_new();

  g_signal_connect(equip_motion, "motion", G_CALLBACK(on_motion), widgets);
  g_signal_connect(equip_motion, "leave", G_CALLBACK(on_motion_leave), widgets);
  gtk_widget_add_controller(widgets->equip_drawing_area, equip_motion);
  gtk_box_append(GTK_BOX(equip_col), widgets->equip_drawing_area);

  // Right column: notebook with stats + stash tabs
  widgets->stash_notebook = gtk_notebook_new();
  gtk_widget_set_hexpand(widgets->stash_notebook, TRUE);
  gtk_widget_set_vexpand(widgets->stash_notebook, TRUE);
  gtk_box_append(GTK_BOX(bottom_hbox), widgets->stash_notebook);

  // Tab 0: Stats
  GtkWidget *tables_scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tables_scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(tables_scroll, TRUE);
  gtk_widget_set_vexpand(tables_scroll, TRUE);

  GtkWidget *tables_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tables_scroll), tables_inner);
  build_stat_tables(widgets, tables_inner);
  gtk_notebook_append_page(GTK_NOTEBOOK(widgets->stash_notebook),
    tables_scroll, gtk_label_new("Stats"));

  // Helper: create a stash tab with scrolled drawing area + event controllers
  #define MAKE_STASH_TAB(da_field, draw_cb, click_cb, tab_label) do { \
    GtkWidget *sw = gtk_scrolled_window_new();                      \
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),         \
      GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);                  \
    gtk_widget_set_hexpand(sw, TRUE);                               \
    gtk_widget_set_vexpand(sw, TRUE);                               \
    widgets->da_field = gtk_drawing_area_new();                     \
    gtk_widget_set_hexpand(widgets->da_field, TRUE);                \
    gtk_widget_set_vexpand(widgets->da_field, TRUE);                \
    gtk_drawing_area_set_draw_func(                                 \
      GTK_DRAWING_AREA(widgets->da_field), draw_cb, widgets, NULL); \
    GtkGesture *sc = gtk_gesture_click_new();                       \
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(sc), 0);      \
    g_signal_connect(sc, "pressed", G_CALLBACK(click_cb), widgets); \
    gtk_widget_add_controller(widgets->da_field,                    \
      GTK_EVENT_CONTROLLER(sc));                                    \
    GtkEventController *sm = gtk_event_controller_motion_new();     \
    g_signal_connect(sm, "motion", G_CALLBACK(on_motion), widgets); \
    g_signal_connect(sm, "leave", G_CALLBACK(on_motion_leave), widgets);\
    gtk_widget_add_controller(widgets->da_field, sm);               \
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw),          \
      widgets->da_field);                                           \
    gtk_notebook_append_page(GTK_NOTEBOOK(widgets->stash_notebook), \
      sw, gtk_label_new(tab_label));                                \
  } while(0)

  // Tab 1: Transfer Stash
  MAKE_STASH_TAB(stash_transfer_da, stash_transfer_draw_cb,
                 on_stash_transfer_click, "Transfer");

  // Tab 2: Player Stash
  MAKE_STASH_TAB(stash_player_da, stash_player_draw_cb,
                 on_stash_player_click, "Storage");

  // Tab 3: Relic Vault
  MAKE_STASH_TAB(stash_relic_da, stash_relic_draw_cb,
                 on_stash_relic_click, "Relics");

  #undef MAKE_STASH_TAB

  // ── Actions ──
  GSimpleAction *settings_action = g_simple_action_new("settings", NULL);

  g_signal_connect(settings_action, "activate", G_CALLBACK(on_settings_action), widgets);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(settings_action));

  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);

  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(quit_action));

  // Manage Vaults / Characters actions
  register_manage_actions(GTK_WINDOW(window), widgets);

  // ── Populate combo boxes ──
  if(global_config.save_folder)
  {
    repopulate_vault_combo(widgets, NULL);
    repopulate_character_combo(widgets, NULL);

    // Load global stashes (transfer + relic vault)
    char *tp = stash_build_path(STASH_TRANSFER, NULL);

    if(tp)
    {
      widgets->transfer_stash = stash_load(tp);
      free(tp);
    }

    char *rp = stash_build_path(STASH_RELIC_VAULT, NULL);

    if(rp)
    {
      widgets->relic_vault = stash_load(rp);
      free(rp);
    }
  }

  // Save vault on close
  g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), widgets);

  gtk_window_present(GTK_WINDOW(window));
}
