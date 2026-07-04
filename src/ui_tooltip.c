// ui_tooltip.c -- instant tooltip system (extracted from ui.c)

#include "ui.h"
#include "item_stats.h"
#include <string.h>
#include <strings.h>

// Choose tooltip position so it doesn't clip off the top or bottom of the
// window.  rect is in widget-local coordinates; we translate to window
// coords to decide.
//
// popover: the GtkPopover to position
// parent:  the widget that owns the popover
// rect:    pointing-to rectangle in parent-local coordinates
static void
tooltip_set_position(GtkWidget *popover, GtkWidget *parent,
                     const GdkRectangle *rect)
{
  graphene_point_t src = GRAPHENE_POINT_INIT(rect->x, rect->y);
  graphene_point_t dst;
  GtkWidget *win = GTK_WIDGET(gtk_widget_get_root(parent));

  if(!gtk_widget_compute_point(parent, win, &src, &dst))
    dst = src;

  double wy = dst.y;
  int win_h = gtk_widget_get_height(win);
  GtkPositionType pos;

  if(wy < win_h * 0.25)
    pos = GTK_POS_BOTTOM;
  else if(wy > win_h * 0.75)
    pos = GTK_POS_TOP;
  else
    pos = GTK_POS_RIGHT;

  gtk_popover_set_position(GTK_POPOVER(popover), pos);
}

// Show or hide the compare column inside the tooltip popover.
// When the hovered item is in the same comparison group as the marked item
// (same gear sub-type, or both relics/charms, or both artifacts), the
// compare_scroll and compare_separator are made visible so both tooltips appear
// side-by-side in a single popover.
//
// widgets:      application state
// hovered_item: the item currently being hovered
static void
show_compare_tooltip(AppWidgets *widgets, const TQVaultItem *hovered_item)
{
  if(!widgets->compare_active || !hovered_item->base_name)
  {
    gtk_widget_set_visible(widgets->compare_scroll, FALSE);
    gtk_widget_set_visible(widgets->compare_separator, FALSE);
    return;
  }

  // Don't compare an item with itself
  if(widgets->compare_item.base_name &&
     strcasecmp(hovered_item->base_name, widgets->compare_item.base_name) == 0 &&
     hovered_item->point_x == widgets->compare_item.point_x &&
     hovered_item->point_y == widgets->compare_item.point_y &&
     hovered_item->seed == widgets->compare_item.seed)
  {
    gtk_widget_set_visible(widgets->compare_scroll, FALSE);
    gtk_widget_set_visible(widgets->compare_separator, FALSE);
    return;
  }

  // Type compatibility check: both must be the same comparison group (e.g. sword
  // vs sword, charm/relic vs charm/relic, artifact vs artifact).
  uint32_t hover_gt = item_compare_group(hovered_item->base_name);
  uint32_t cmp_gt = item_compare_group(widgets->compare_item.base_name);

  if(!hover_gt || !cmp_gt || hover_gt != cmp_gt)
  {
    gtk_widget_set_visible(widgets->compare_scroll, FALSE);
    gtk_widget_set_visible(widgets->compare_separator, FALSE);
    return;
  }

  gtk_label_set_markup(GTK_LABEL(widgets->compare_label), widgets->compare_markup);
  gtk_widget_set_visible(widgets->compare_separator, TRUE);
  gtk_widget_set_visible(widgets->compare_scroll, TRUE);
}

// Update the instant tooltip based on current cursor position and widget.
// Driven by on_motion(), replaces GTK4's 500ms-delayed tooltips.
//
// widgets: application state containing cursor info, tooltip popover, etc.
static void
update_instant_tooltip(AppWidgets *widgets)
{
  GtkWidget *popover = widgets->tooltip_popover;

  if(!popover)
    return;

  // Hide if carrying an item or context menu is visible
  if(widgets->held_item ||
     (widgets->context_menu && gtk_widget_get_visible(widgets->context_menu)))
  {
    gtk_widget_set_visible(popover, FALSE);
    return;
  }

  GtkWidget *w = widgets->cursor_widget;

  if(!w)
  {
    gtk_widget_set_visible(popover, FALSE);
    return;
  }

  double x = widgets->cursor_x;
  double y = widgets->cursor_y;
  int pw = gtk_widget_get_width(w);
  int ph = gtk_widget_get_height(w);
  TQVaultItem *item = NULL;
  int equip_slot = -1;
  GdkRectangle rect = {0};

  // Helper: compute item rectangle from cell coordinates for sack items
  #define SACK_ITEM_RECT(it, fallback_cols) do {                          \
      double cell = compute_cell_size(widgets);                           \
      if(cell <= 0.0) cell = (double)pw / (fallback_cols);               \
      int iw, ih;                                                         \
      get_item_dims(widgets, (it), &iw, &ih);                            \
      rect.x      = (int)((it)->point_x * cell);                        \
      rect.y      = (int)((it)->point_y * cell);                        \
      rect.width  = (int)(iw * cell);                                    \
      rect.height = (int)(ih * cell);                                    \
  } while(0)

  if(w == widgets->vault_drawing_area)
  {
    // Vault sack
    if(!widgets->current_vault)
    {
      gtk_widget_set_visible(popover, FALSE);
      return;
    }

    int si = widgets->current_sack;

    if(si >= 0 && si < widgets->current_vault->num_sacks)
    {
      TQVaultSack *sack = &widgets->current_vault->sacks[si];
      item = sack_hit_test(widgets, sack, 18, 20, pw, ph, (int)x, (int)y);
    }

    if(item)
    {
      if(item == widgets->last_tooltip_item &&
         gtk_widget_get_visible(popover))
        return;

      widgets->last_tooltip_item = item;
      widgets->tooltip_item_base = item->base_name;
      widgets->tooltip_item_var1 = item->var1;
      widgets->last_tooltip_markup[0] = '\0';
      ItemReqReduction red;
      item_req_reduction(widgets->current_character, item->base_name, &red);
      vault_item_format_stats_ex(item, widgets->translations, &red,
                                 widgets->last_tooltip_markup,
                                 sizeof(widgets->last_tooltip_markup));

      if(widgets->tooltip_parent != w)
      {
        if(widgets->tooltip_parent)
          gtk_widget_unparent(popover);
        gtk_widget_set_parent(popover, w);
        widgets->tooltip_parent = w;
      }

      SACK_ITEM_RECT(item, 18);
      gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                           widgets->last_tooltip_markup);
      show_compare_tooltip(widgets, item);
      gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
      tooltip_set_position(popover, w, &rect);
      gtk_widget_set_visible(popover, TRUE);
      return;
    }

    widgets->last_tooltip_item = NULL;
    gtk_widget_set_visible(popover, FALSE);
    return;
  }

  if(w == widgets->inv_drawing_area)
  {
    if(!widgets->current_character || widgets->current_character->num_inv_sacks < 1)
    {
      gtk_widget_set_visible(popover, FALSE);
      return;
    }

    TQVaultSack *sack = &widgets->current_character->inv_sacks[0];
    item = sack_hit_test(widgets, sack, CHAR_INV_COLS, CHAR_INV_ROWS, pw, ph, (int)x, (int)y);

    if(item)
    {
      if(item == widgets->last_inv_tooltip_item &&
         gtk_widget_get_visible(popover))
        return;

      widgets->last_inv_tooltip_item = item;
      widgets->tooltip_item_base = item->base_name;
      widgets->tooltip_item_var1 = item->var1;
      widgets->last_inv_tooltip_markup[0] = '\0';
      ItemReqReduction red;
      item_req_reduction(widgets->current_character, item->base_name, &red);
      vault_item_format_stats_ex(item, widgets->translations, &red,
                                 widgets->last_inv_tooltip_markup,
                                 sizeof(widgets->last_inv_tooltip_markup));

      if(widgets->tooltip_parent != w)
      {
        if(widgets->tooltip_parent)
          gtk_widget_unparent(popover);
        gtk_widget_set_parent(popover, w);
        widgets->tooltip_parent = w;
      }

      SACK_ITEM_RECT(item, CHAR_INV_COLS);
      gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                           widgets->last_inv_tooltip_markup);
      show_compare_tooltip(widgets, item);
      gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
      tooltip_set_position(popover, w, &rect);
      gtk_widget_set_visible(popover, TRUE);
      return;
    }

    widgets->last_inv_tooltip_item = NULL;
    gtk_widget_set_visible(popover, FALSE);
    return;
  }

  if(w == widgets->bag_drawing_area)
  {
    int idx = 1 + widgets->current_char_bag;

    if(!widgets->current_character || idx >= widgets->current_character->num_inv_sacks)
    {
      gtk_widget_set_visible(popover, FALSE);
      return;
    }

    TQVaultSack *sack = &widgets->current_character->inv_sacks[idx];
    item = sack_hit_test(widgets, sack, CHAR_BAG_COLS, CHAR_BAG_ROWS, pw, ph, (int)x, (int)y);

    if(item)
    {
      if(item == widgets->last_bag_tooltip_item &&
         gtk_widget_get_visible(popover))
        return;

      widgets->last_bag_tooltip_item = item;
      widgets->tooltip_item_base = item->base_name;
      widgets->tooltip_item_var1 = item->var1;
      widgets->last_bag_tooltip_markup[0] = '\0';
      ItemReqReduction red;
      item_req_reduction(widgets->current_character, item->base_name, &red);
      vault_item_format_stats_ex(item, widgets->translations, &red,
                                 widgets->last_bag_tooltip_markup,
                                 sizeof(widgets->last_bag_tooltip_markup));

      if(widgets->tooltip_parent != w)
      {
        if(widgets->tooltip_parent)
          gtk_widget_unparent(popover);
        gtk_widget_set_parent(popover, w);
        widgets->tooltip_parent = w;
      }

      SACK_ITEM_RECT(item, CHAR_BAG_COLS);
      gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                           widgets->last_bag_tooltip_markup);
      show_compare_tooltip(widgets, item);
      gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
      tooltip_set_position(popover, w, &rect);
      gtk_widget_set_visible(popover, TRUE);
      return;
    }

    widgets->last_bag_tooltip_item = NULL;
    gtk_widget_set_visible(popover, FALSE);
    return;
  }

  #undef SACK_ITEM_RECT

  // Helper macro for stash tooltip item rects (cell size from stash dims)
  #define STASH_ITEM_RECT(it, stash_ptr) do {                             \
      double cell = (double)pw / (stash_ptr)->sack_width;                 \
      double cell_h2 = (double)ph / (stash_ptr)->sack_height;             \
      if(cell_h2 < cell) cell = cell_h2;                                  \
      if(cell <= 0.0) cell = 32.0;                                        \
      int iw, ih;                                                         \
      get_item_dims(widgets, (it), &iw, &ih);                            \
      rect.x      = (int)((it)->point_x * cell);                        \
      rect.y      = (int)((it)->point_y * cell);                        \
      rect.width  = (int)(iw * cell);                                    \
      rect.height = (int)(ih * cell);                                    \
  } while(0)

  // Stash tooltip helper: compute cell size, find item, show tooltip.
  // Uses find_item_at_cell() with the locally-computed cell size instead of
  // sack_hit_test() (which would use compute_cell_size() -- wrong for stashes).
  #define STASH_TOOLTIP(da_field, stash_field, cache_item, cache_markup, label) \
  if(w == widgets->da_field && widgets->stash_field) {                        \
      TQStash *st = widgets->stash_field;                                      \
      double scw = (double)pw / st->sack_width;                                \
      double sch = (double)ph / st->sack_height;                               \
      double scell = scw < sch ? scw : sch;                                    \
      if(scell <= 0.0) scell = 32.0;                                           \
      item = find_item_at_cell(widgets, &st->sack, st->sack_width,             \
                               st->sack_height, scell, x, y, NULL);            \
      if(item) {                                                               \
          if(item == widgets->cache_item &&                                     \
             gtk_widget_get_visible(popover)) return;                          \
          widgets->cache_item = item;                                          \
          widgets->tooltip_item_base = item->base_name;                        \
          widgets->tooltip_item_var1 = item->var1;                             \
          widgets->cache_markup[0] = '\0';                                     \
          ItemReqReduction red;                                                \
          item_req_reduction(widgets->current_character, item->base_name, &red); \
          vault_item_format_stats_ex(item, widgets->translations, &red,         \
                                  widgets->cache_markup,                        \
                                  sizeof(widgets->cache_markup));               \
          if(widgets->tooltip_parent != w) {                                   \
              if(widgets->tooltip_parent) gtk_widget_unparent(popover);        \
              gtk_widget_set_parent(popover, w);                               \
              widgets->tooltip_parent = w;                                     \
          }                                                                    \
          STASH_ITEM_RECT(item, st);                                           \
          gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),              \
                               widgets->cache_markup);                          \
          show_compare_tooltip(widgets, item);                                 \
          gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);            \
          tooltip_set_position(popover, w, &rect);                             \
          gtk_widget_set_visible(popover, TRUE);                               \
          return;                                                              \
      }                                                                        \
      widgets->cache_item = NULL;                                              \
      gtk_widget_set_visible(popover, FALSE);                                  \
      return;                                                                  \
  }

  STASH_TOOLTIP(stash_transfer_da, transfer_stash,
                last_transfer_tooltip_item, last_transfer_tooltip_markup, "Transfer")
  STASH_TOOLTIP(stash_player_da, player_stash,
                last_player_tooltip_item, last_player_tooltip_markup, "Storage")
  STASH_TOOLTIP(stash_relic_da, relic_vault,
                last_relic_tooltip_item, last_relic_tooltip_markup, "Relics")

  #undef STASH_TOOLTIP
  #undef STASH_ITEM_RECT

  if(w == widgets->equip_drawing_area)
  {
    if(!widgets->current_character)
    {
      gtk_widget_set_visible(popover, FALSE);
      return;
    }

    double cell_size = compute_cell_size(widgets);
    double sx, sy, sbw, sbh;
    int slot = -1;

    if(equip_hit_test(x, y, cell_size, &slot, &sx, &sy, &sbw, &sbh) &&
       slot >= 0 && slot < 12 &&
       widgets->current_character->equipment[slot])
    {
      equip_slot = slot;

      if(equip_slot == widgets->last_equip_tooltip_slot &&
         gtk_widget_get_visible(popover))
        return;

      widgets->last_equip_tooltip_slot = equip_slot;
      widgets->last_equip_tooltip_markup[0] = '\0';
      TQItem *eq = widgets->current_character->equipment[equip_slot];
      TQVaultItem vi = {0};

      vi.seed        = eq->seed;
      vi.base_name   = eq->base_name;
      vi.prefix_name = eq->prefix_name;
      vi.suffix_name = eq->suffix_name;
      vi.relic_name  = eq->relic_name;
      vi.relic_bonus = eq->relic_bonus;
      vi.relic_name2 = eq->relic_name2;
      vi.relic_bonus2= eq->relic_bonus2;
      vi.var1        = eq->var1;
      vi.var2        = eq->var2;
      widgets->tooltip_item_base = eq->base_name;
      widgets->tooltip_item_var1 = eq->var1;
      ItemReqReduction red;
      item_req_reduction(widgets->current_character, vi.base_name, &red);
      vault_item_format_stats_ex(&vi, widgets->translations, &red,
                                 widgets->last_equip_tooltip_markup,
                                 sizeof(widgets->last_equip_tooltip_markup));

      if(widgets->tooltip_parent != w)
      {
        if(widgets->tooltip_parent)
          gtk_widget_unparent(popover);
        gtk_widget_set_parent(popover, w);
        widgets->tooltip_parent = w;
      }

      rect.x = (int)sx;  rect.y = (int)sy;
      rect.width = (int)sbw;  rect.height = (int)sbh;
      gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                           widgets->last_equip_tooltip_markup);
      show_compare_tooltip(widgets, &vi);
      gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
      tooltip_set_position(popover, w, &rect);
      gtk_widget_set_visible(popover, TRUE);
      return;
    }

    widgets->last_equip_tooltip_slot = -1;
    gtk_widget_set_visible(popover, FALSE);
    return;
  }

  gtk_widget_set_visible(popover, FALSE);
}

// Timeout callback: hide the toast and clear the pending source id.
// user_data: AppWidgets pointer.
// Returns G_SOURCE_REMOVE so the timeout fires only once.
static gboolean
toast_hide_cb(gpointer user_data)
{
  AppWidgets *widgets = user_data;

  if(widgets->toast_label)
    gtk_widget_set_visible(widgets->toast_label, FALSE);

  widgets->toast_timeout_id = 0;
  return(G_SOURCE_REMOVE);
}

// Briefly show a transient toast message at the bottom of the window, then
// auto-hide it after ~2s.  Re-arming while one is showing replaces the text and
// restarts the timer.
//
// widgets: application state.
// message: text to display.
void
show_toast(AppWidgets *widgets, const char *message)
{
  if(!widgets->toast_label)
    return;

  gtk_label_set_text(GTK_LABEL(widgets->toast_label), message);
  gtk_widget_set_visible(widgets->toast_label, TRUE);

  if(widgets->toast_timeout_id)
    g_source_remove(widgets->toast_timeout_id);

  widgets->toast_timeout_id =
    g_timeout_add(2000, toast_hide_cb, widgets);
}

// Load an item's icon and wrap it in a GdkTexture for compositing into the
// clipboard image.  Item textures are small, so the drawn size is scaled up to
// ~2x with the longest edge capped at 128px; the texture itself keeps the
// natural pixel size (the renderer scales it into the drawn rectangle).
//
// widgets:        application state.
// base_name:      item DBR path used for icon resolution.
// var1:           shard count / variant for icon resolution.
// draw_w, draw_h: out parameters; receive the drawn size in pixels.
// Returns a new GdkTexture (caller unrefs), or NULL if no icon resolves.
static GdkTexture *
tooltip_icon_texture(AppWidgets *widgets, const char *base_name, uint32_t var1,
                     int *draw_w, int *draw_h)
{
  *draw_w = 0;
  *draw_h = 0;

  if(!base_name)
    return(NULL);

  GdkPixbuf *icon = load_item_texture(widgets, base_name, var1);

  if(!icon)
    return(NULL);

  int nw = gdk_pixbuf_get_width(icon);
  int nh = gdk_pixbuf_get_height(icon);
  GdkTexture *tex = NULL;

  if(nw > 0 && nh > 0)
  {
    double scale = 2.0;
    double longest = (nw > nh ? nw : nh) * scale;

    if(longest > 128.0)
      scale = 128.0 / (nw > nh ? nw : nh);

    *draw_w = (int)(nw * scale);
    *draw_h = (int)(nh * scale);

    // Build a texture from the pixbuf without the deprecated
    // gdk_texture_new_for_pixbuf (mirrors set_bag_btn_image in ui_io.c).
    GBytes *bytes = g_bytes_new(gdk_pixbuf_get_pixels(icon),
                                (gsize)nh * gdk_pixbuf_get_rowstride(icon));

    tex = gdk_memory_texture_new(
      nw, nh,
      gdk_pixbuf_get_has_alpha(icon) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
      bytes, gdk_pixbuf_get_rowstride(icon));
    g_bytes_unref(bytes);
  }

  g_object_unref(icon);
  return(tex);
}

// Render the currently-visible tooltip popover to a GdkTexture and copy it to
// the system clipboard, so the user can paste the item card into Discord and
// other apps.  Each visible card gets its item's icon composited above it:
// the hovered item above the main card and, when the Ctrl+click compare
// column is shown, the marked item above the compare card.  Icons are centred
// on their own card, all over the tooltip's dark background colour.
//
// widgets: application state.
// Returns TRUE if a tooltip was visible and an image was copied.
gboolean
tooltip_copy_to_clipboard(AppWidgets *widgets)
{
  GtkWidget *popover = widgets->tooltip_popover;

  if(!popover || !gtk_widget_get_visible(popover))
    return(FALSE);

  GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(popover));

  if(!content)
    return(FALSE);

  int cw = gtk_widget_get_width(content);
  int ch = gtk_widget_get_height(content);

  if(cw <= 0 || ch <= 0)
    return(FALSE);

  // One icon per visible card, each anchored to its card's horizontal centre.
  struct
  {
    GdkTexture *tex;
    int w, h;      // drawn size
    float cx;      // card centre x in content coordinates
  } icons[2];
  int n_icons = 0;
  const int icon_gap = 6;

  GtkWidget *cards[2] = { widgets->tooltip_scroll, NULL };
  const char *bases[2] = { widgets->tooltip_item_base, NULL };
  uint32_t vars[2] = { widgets->tooltip_item_var1, 0 };

  if(widgets->compare_active && widgets->compare_scroll &&
     gtk_widget_get_visible(widgets->compare_scroll))
  {
    cards[1] = widgets->compare_scroll;
    bases[1] = widgets->compare_item.base_name;
    vars[1] = widgets->compare_item.var1;
  }

  for(int i = 0; i < 2; i++)
  {
    if(!cards[i])
      continue;

    int iw, ih;
    GdkTexture *tex = tooltip_icon_texture(widgets, bases[i], vars[i], &iw, &ih);

    if(!tex)
      continue;

    // Centre the icon on its card.  Falls back to the content centre if the
    // bounds can't be computed (shouldn't happen while the popover is mapped).
    graphene_rect_t cb;
    float cx = cw / 2.0f;

    if(gtk_widget_compute_bounds(cards[i], content, &cb))
      cx = cb.origin.x + cb.size.width / 2.0f;

    icons[n_icons].tex = tex;
    icons[n_icons].w = iw;
    icons[n_icons].h = ih;
    icons[n_icons].cx = cx;
    n_icons++;
  }

  int band_h = 0;
  int max_icon_w = 0;

  for(int i = 0; i < n_icons; i++)
  {
    if(icons[i].h > band_h)
      band_h = icons[i].h;
    if(icons[i].w > max_icon_w)
      max_icon_w = icons[i].w;
  }

  // Pad around everything so the content isn't flush against the image edge,
  // matching the look of the on-screen popover.
  const int pad = 8;
  int top_band = band_h ? band_h + icon_gap : 0;
  int inner_w = cw > max_icon_w ? cw : max_icon_w;
  int tw = inner_w + pad * 2;
  int th = top_band + ch + pad * 2;

  GtkSnapshot *snapshot = gtk_snapshot_new();
  graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, tw, th);

  // Fill the tooltip's dark background (matches popover.item-tooltip in CSS) so
  // the pasted image isn't transparent.
  GdkRGBA bg;

  gdk_rgba_parse(&bg, "#1a1a2e");
  gtk_snapshot_append_color(snapshot, &bg, &full);

  // The card content is drawn centred horizontally at card_x, so icon centres
  // shift by the same amount.  Icons sit bottom-aligned in the top band so each
  // ends the same gap above its card (absolute coords, before any transform is
  // pushed for the content below).
  float card_x = (tw - cw) / 2.0f;

  for(int i = 0; i < n_icons; i++)
  {
    float ix = card_x + icons[i].cx - icons[i].w / 2.0f;

    if(ix < pad)
      ix = pad;
    if(ix + icons[i].w > tw - pad)
      ix = tw - pad - icons[i].w;

    graphene_rect_t irect =
      GRAPHENE_RECT_INIT(ix, pad + band_h - icons[i].h, icons[i].w, icons[i].h);

    gtk_snapshot_append_texture(snapshot, icons[i].tex, &irect);
  }

  // Card content, centred horizontally, below the icon band.
  graphene_point_t off = GRAPHENE_POINT_INIT(card_x, pad + top_band);

  gtk_snapshot_translate(snapshot, &off);

  GdkPaintable *paintable = gtk_widget_paintable_new(content);

  gdk_paintable_snapshot(paintable, snapshot, cw, ch);
  g_object_unref(paintable);

  GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);

  for(int i = 0; i < n_icons; i++)
    g_clear_object(&icons[i].tex);

  if(!node)
    return(FALSE);

  GtkNative *native = gtk_widget_get_native(content);
  GskRenderer *renderer = native ? gtk_native_get_renderer(native) : NULL;
  GdkTexture *texture = NULL;

  if(renderer)
    texture = gsk_renderer_render_texture(renderer, node, &full);

  gsk_render_node_unref(node);

  if(!texture)
    return(FALSE);

  GdkClipboard *clipboard = gtk_widget_get_clipboard(widgets->main_window);

  gdk_clipboard_set_texture(clipboard, texture);
  g_object_unref(texture);
  return(TRUE);
}

// Handle pointer motion over a drawing area.  Updates cursor tracking
// and refreshes the instant tooltip.
//
// ctrl:      the motion event controller
// x, y:     cursor coordinates in widget-local space
// user_data: AppWidgets pointer
void
on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data)
{
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));

  widgets->cursor_x = x;
  widgets->cursor_y = y;
  widgets->cursor_widget = w;

  // Redraw to track the held-item placement preview as it follows the cursor.
  // The equippability tint is now always-on (static, not cursor-driven), so
  // plain hovering no longer needs a per-frame grid redraw -- only the tooltip,
  // which update_instant_tooltip() handles below regardless.
  if(widgets->held_item)
    gtk_widget_queue_draw(w);

  update_instant_tooltip(widgets);
}

// Handle pointer leaving a drawing area.  Clears cursor tracking,
// redraws if carrying an item, and hides the tooltip.
//
// ctrl:      the motion event controller (unused)
// user_data: AppWidgets pointer
void
on_motion_leave(GtkEventControllerMotion *ctrl, gpointer user_data)
{
  (void)ctrl;
  AppWidgets *widgets = (AppWidgets *)user_data;

  widgets->cursor_widget = NULL;

  // Clear the held-item placement preview.  The equippability tint is always-on
  // and not cursor-driven, so it needs no clearing when the pointer leaves.
  if(widgets->held_item)
    queue_redraw_all(widgets);

  if(widgets->tooltip_popover)
    gtk_widget_set_visible(widgets->tooltip_popover, FALSE);
}
