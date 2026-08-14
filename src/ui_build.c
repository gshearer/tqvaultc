#include "ui.h"
#include "ui_internal.h"
#include "config.h"
#include "texture.h"
#include "item_stats.h"
#include "version.h"
#include "build_number.h"
#include <stdio.h>
#include <string.h>

// Load and scale the three bag-button state textures (down/up/over).
// Returns false when any is missing; the caller then falls back to numeric
// labels.  base[] is filled in either way and must be passed to
// bag_button_textures_free.
static bool
bag_button_textures_load(GdkPixbuf *base[3])
{
  static const char *tex_paths[3] = {
    "InGameUI\\characterscreen\\inventorybagdown01.tex",
    "InGameUI\\characterscreen\\inventorybagup01.tex",
    "InGameUI\\characterscreen\\inventorybagover01.tex",
  };

  for(int s = 0; s < 3; s++)
  {
    GdkPixbuf *raw = texture_load(tex_paths[s]);

    base[s] = NULL;

    if(raw)
    {
      base[s] = gdk_pixbuf_scale_simple(raw, 40, 36, GDK_INTERP_BILINEAR);
      g_object_unref(raw);
    }
  }

  return(base[0] && base[1] && base[2]);
}

// Drop the scaled state textures once the per-bag numbered copies are made.
static void
bag_button_textures_free(GdkPixbuf *base[3])
{
  for(int s = 0; s < 3; s++)
  {
    if(base[s])
      g_object_unref(base[s]);
  }
}

// The right-click item menu and the bag menu, each owned by us so that
// unparenting the popover does not destroy it.
static void
build_context_menus(GtkApplication *app, AppWidgets *widgets)
{
  // Right-click context menu: actions + popover
  register_context_actions(app, widgets);

  GMenu *ctx_menu = g_menu_new();

  widgets->context_menu_model = ctx_menu;  // kept alive for dynamic rebuild

  widgets->context_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(ctx_menu));
  g_object_ref_sink(widgets->context_menu);  // own the popover so unparent won't destroy it
  gtk_popover_set_has_arrow(GTK_POPOVER(widgets->context_menu), FALSE);
  gtk_widget_set_halign(widgets->context_menu, GTK_ALIGN_START);
  g_signal_connect(widgets->context_menu, "closed",
                   G_CALLBACK(on_context_menu_closed), widgets);

  // Bag context menu: actions + popover
  register_bag_menu_actions(app, widgets);
  widgets->bag_menu_model = g_menu_new();
  widgets->bag_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(widgets->bag_menu_model));
  g_object_ref_sink(widgets->bag_menu);
  gtk_popover_set_has_arrow(GTK_POPOVER(widgets->bag_menu), FALSE);
  gtk_widget_set_halign(widgets->bag_menu, GTK_ALIGN_START);
  g_signal_connect(widgets->bag_menu, "closed",
                   G_CALLBACK(on_context_menu_closed), widgets);
}

// The zero-delay item tooltip: a non-interactive popover holding the main
// card, a separator, and the Ctrl+click compare card (both hidden until used).
static void
build_instant_tooltip(AppWidgets *widgets)
{
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

  // Cap the tooltip height so a giant card can't run off the monitor, but keep
  // it high enough that tall artifact/set cards (base stats + granted skill +
  // full summoned-pet ability list + seed/expansion/required-level footer) fit
  // without clipping.  The old 800px cap cut the last few footer lines off long
  // cards, and because the tooltip is non-interactive (can_target/can_focus
  // FALSE) the resulting scrollbar could never be used to reach them.  A popover
  // is clamped to the monitor, not the parent window, so this may exceed the
  // window height.
  const int tooltip_max_h = 1000;

  GtkWidget *tip_scroll = gtk_scrolled_window_new();

  widgets->tooltip_scroll = tip_scroll;
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tip_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(tip_scroll), tooltip_max_h);
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
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(widgets->compare_scroll), tooltip_max_h);
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
}

// Force Adwaita's dark variant and load our stylesheet.  On Linux the system
// theme would usually do, but Windows typically has no GTK theme installed and
// would otherwise render our dark inventory grid against light defaults.
static void
apply_dark_theme_and_css(void)
{
  g_object_set(gtk_settings_get_default(),
               "gtk-application-prefer-dark-theme", TRUE,
               NULL);

  GtkCssProvider *provider = gtk_css_provider_new();

  gtk_css_provider_load_from_resource(provider, "/org/tqvaultc/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

// The application window, its key controller, and the translation table.
static GtkWidget *
build_main_window(GtkApplication *app, AppWidgets *widgets)
{
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

  return(window);
}

// The header bar: settings/about, the Database, Manage Vaults and Manage
// Characters dropdowns, the per-character buttons, and the search entry.
// Installs itself as the window's titlebar.
static void
build_header_bar(AppWidgets *widgets, GtkWidget *window)
{
  GtkWidget *header = gtk_header_bar_new();

  GtkWidget *settings_btn = gtk_button_new_with_label("Settings");

  g_signal_connect(settings_btn, "clicked", G_CALLBACK(on_settings_btn_clicked), widgets);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), settings_btn);

  GtkWidget *about_btn = gtk_button_new_with_label("About");

  g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_btn_clicked), widgets);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), about_btn);

  // ── Database dropdown (curated Browser + raw Explorer) ──
  GSimpleAction *db_browser_action = g_simple_action_new("db-browser", NULL);

  g_signal_connect(db_browser_action, "activate", G_CALLBACK(on_db_browser_action), widgets);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(db_browser_action));

  GSimpleAction *db_explorer_action = g_simple_action_new("db-explorer", NULL);

  g_signal_connect(db_explorer_action, "activate", G_CALLBACK(on_db_explorer_action), widgets);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(db_explorer_action));

  GMenu *database_menu = g_menu_new();

  g_menu_append(database_menu, "Database Browser", "win.db-browser");
  g_menu_append(database_menu, "Database Explorer (raw)", "win.db-explorer");

  GtkWidget *database_btn = gtk_menu_button_new();

  gtk_menu_button_set_label(GTK_MENU_BUTTON(database_btn), "Database");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(database_btn),
                                  G_MENU_MODEL(database_menu));
  g_object_unref(database_menu);
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
  g_menu_append(char_menu, "Open Mobile Save Folder\xe2\x80\xa6", "win.open-mobile-save");

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
  gtk_widget_set_tooltip_text(widgets->skills_btn, "Skill Manager (s)");
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

  // Capture phase: intercept Enter/Escape before the entry's internal GtkText
  // does -- Return is bound to "activate" there and would otherwise be
  // consumed before it could bubble out to this controller.
  gtk_event_controller_set_propagation_phase(search_key, GTK_PHASE_CAPTURE);
  g_signal_connect(search_key, "key-pressed", G_CALLBACK(on_search_key), widgets);
  gtk_widget_add_controller(widgets->search_entry, search_key);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->search_entry);

  gtk_window_set_titlebar(GTK_WINDOW(window), header);
}

// The window's content: a scrolled main_hbox with the transparent held-item
// layer and the toast label overlaid on top.  Leaves main_hbox in widgets.
static void
build_content_overlay(AppWidgets *widgets, GtkWidget *window)
{
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

  // Wrap the whole content in a scrolled window: it fills the window normally
  // (the cell scales to fit), but once the cell would drop below MIN_CELL the
  // grids hold their minimum size and scrollbars appear instead of shrinking
  // into unusable squares -- so tiny windows and handhelds stay usable.
  GtkWidget *content_scroll = gtk_scrolled_window_new();

  // Vertical only: the cell's width term always fits the window (the header bar
  // keeps the window wider than the grids ever need), so a horizontal bar would
  // never engage -- NEVER avoids a dead scrollbar and width fighting.
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(content_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  // Non-overlay so the outer bar takes its own gutter at the window edge instead
  // of floating over (and stealing events from) the stash notebook's own
  // scrollbar, which sits just inside it.
  gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(content_scroll), FALSE);
  // Floor the vertical shrink at something usable: without this the scrolled
  // window happily collapses to ~10px tall, leaving the grids invisible behind
  // the scrollbar. 300px keeps a couple of grid rows on screen before the bar
  // takes over.
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(content_scroll), 300);
  gtk_widget_set_hexpand(content_scroll, TRUE);
  gtk_widget_set_vexpand(content_scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(content_scroll), main_hbox);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), content_scroll);

  // Transparent overlay drawing area: renders held item between panes
  widgets->held_overlay = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->held_overlay, TRUE);
  gtk_widget_set_vexpand(widgets->held_overlay, TRUE);
  gtk_widget_set_can_target(widgets->held_overlay, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widgets->held_overlay),
                                 held_overlay_draw_cb, widgets, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), widgets->held_overlay);

  // Transient toast: a small label pinned to the bottom-centre, hidden until
  // show_toast() reveals it.  Non-targetable so it never steals clicks.
  widgets->toast_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(widgets->toast_label, "toast");
  gtk_widget_set_halign(widgets->toast_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(widgets->toast_label, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(widgets->toast_label, 24);
  gtk_widget_set_can_target(widgets->toast_label, FALSE);
  gtk_widget_set_visible(widgets->toast_label, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), widgets->toast_label);

  // Capture-phase motion on the overlay: tracks cursor globally
  GtkEventController *overlay_motion = gtk_event_controller_motion_new();

  gtk_event_controller_set_propagation_phase(overlay_motion, GTK_PHASE_CAPTURE);
  g_signal_connect(overlay_motion, "motion", G_CALLBACK(on_overlay_motion), widgets);
  gtk_widget_add_controller(overlay, overlay_motion);
}

// The left-hand vault pane: vault selector, twelve bag buttons, and the grid.
static void
build_vault_pane(AppWidgets *widgets)
{
  // ── Left: vault area ──
  GtkWidget *main_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_hexpand(main_area, TRUE);
  gtk_widget_set_vexpand(main_area, TRUE);
  gtk_box_append(GTK_BOX(widgets->main_hbox), main_area);

  widgets->vault_combo = gtk_drop_down_new_from_strings(NULL);
  gtk_box_append(GTK_BOX(main_area), widgets->vault_combo);
  widgets->vault_combo_handler = g_signal_connect(widgets->vault_combo,
    "notify::selected", G_CALLBACK(on_vault_changed), widgets);

  // Vault bag buttons -- three-state textures (down/up/over)
  GtkWidget *bag_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_box_append(GTK_BOX(main_area), bag_hbox);
  {
    GdkPixbuf *base[3];
    bool have_tex = bag_button_textures_load(base);

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

    bag_button_textures_free(base);
  }

  widgets->vault_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->vault_drawing_area, TRUE);
  gtk_widget_set_vexpand(widgets->vault_drawing_area, TRUE);
  // Minimum size = grid at MIN_CELL. vexpand grows it past this when there's
  // room; when there isn't, the main scrolled window scrolls instead.
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->vault_drawing_area),
                                     VAULT_COLS * MIN_CELL);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widgets->vault_drawing_area),
                                      VAULT_ROWS * MIN_CELL);
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
}

// The right-hand character pane down to the inventory/bag grids.  Returns the
// pane so the bottom section can be appended to it.
static GtkWidget *
build_character_pane(AppWidgets *widgets)
{
  // ── Right: character panel ──
  GtkWidget *char_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_hexpand(char_panel, TRUE);
  gtk_widget_set_vexpand(char_panel, TRUE);
  gtk_widget_set_margin_start(char_panel, 6);
  gtk_box_append(GTK_BOX(widgets->main_hbox), char_panel);

  widgets->character_combo = gtk_drop_down_new_from_strings(NULL);
  install_character_combo_factory(widgets);
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
    GdkPixbuf *cbase[3];
    bool have_tex = bag_button_textures_load(cbase);

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

    bag_button_textures_free(cbase);
  }

  // Row 1, col 0: main inventory 12x5.  hexpand FALSE so the grid allocates it
  // exactly its pinned content_width (12*cell, set in on_vault_resize) rather
  // than splitting surplus 50/50 with the bag column -- that uneven split is
  // what clipped the inventory's 12th column. The spacer column (below)
  // absorbs the leftover width instead.
  widgets->inv_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->inv_drawing_area, FALSE);
  gtk_widget_set_vexpand(widgets->inv_drawing_area, TRUE);
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->inv_drawing_area),
                                     CHAR_INV_COLS * MIN_CELL);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widgets->inv_drawing_area),
                                      CHAR_INV_ROWS * MIN_CELL);
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

  // Row 1, col 1: extra bag 8x5.  hexpand FALSE for the same reason as the
  // inventory above -- pinned to 8*cell in on_vault_resize.
  widgets->bag_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->bag_drawing_area, FALSE);
  gtk_widget_set_vexpand(widgets->bag_drawing_area, TRUE);
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->bag_drawing_area),
                                     CHAR_BAG_COLS * MIN_CELL);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widgets->bag_drawing_area),
                                      CHAR_BAG_ROWS * MIN_CELL);
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

  // Col 2: invisible expanding spacer. The inventory and bag are pinned to
  // exact cell-multiple widths (hexpand FALSE), so this column soaks up the
  // grid's leftover horizontal space at the right edge -- keeping the inv/bag
  // columns at exactly 12*cell / 8*cell instead of stretching them.
  GtkWidget *inv_bag_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_set_hexpand(inv_bag_spacer, TRUE);
  gtk_grid_attach(GTK_GRID(inv_bag_grid), inv_bag_spacer, 2, 0, 1, 2);

  return(char_panel);
}

// The compact character stats grid (level, type, attributes, HP/MP, K/D, armor).
static void
build_stats_grid(AppWidgets *widgets, GtkWidget *equip_col)
{
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

  // Row 0: Level, then the character type spanning the remaining two columns
  // (e.g. "Warlock (Spirit + Rogue)").
  STAT_CELL(0, "Lv",  &widgets->level_label);
  {
    GtkWidget *_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);

    widgets->type_label = gtk_label_new("-");
    gtk_widget_add_css_class(widgets->type_label, "stats-cell-val");
    gtk_label_set_xalign(GTK_LABEL(widgets->type_label), 0.0f);
    gtk_box_append(GTK_BOX(_box), widgets->type_label);
    gtk_grid_attach(GTK_GRID(stats_grid), _box, 1, sg_row, 2, 1);
  }
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
}

// The equipment paper-doll drawing area.
static void
build_equipment_area(AppWidgets *widgets, GtkWidget *equip_col)
{
  // Equipment drawing area
  widgets->equip_drawing_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(widgets->equip_drawing_area, FALSE);
  gtk_widget_set_vexpand(widgets->equip_drawing_area, FALSE);
  // Initial minimum (MIN_CELL-based); on_vault_resize() rescales this to the
  // live cell size so the equipment never over-reserves and starves the
  // inventory, and shrinks with everything else on small windows.
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->equip_drawing_area),
                                     6 * MIN_CELL + 2 * (int)EQUIP_COL_GAP);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widgets->equip_drawing_area),
                                      12 * MIN_CELL +
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
}

// The notebook on the right of the bottom section: the stat tables, then one
// tab per stash (Transfer, Storage, Relics).
static void
build_side_notebook(AppWidgets *widgets, GtkWidget *bottom_hbox)
{
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
}

// The bottom section: stats above equipment on the left, the stash/stats
// notebook on the right.
static void
build_bottom_section(AppWidgets *widgets, GtkWidget *char_panel)
{
  // Bottom section: equip+stats on left, tables stacked on right.
  GtkWidget *bottom_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_vexpand(bottom_hbox, FALSE);
  gtk_box_append(GTK_BOX(char_panel), bottom_hbox);

  // Left column: stats above equipment
  GtkWidget *equip_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_hexpand(equip_col, FALSE);
  gtk_widget_set_vexpand(equip_col, FALSE);
  gtk_box_append(GTK_BOX(bottom_hbox), equip_col);
  build_stats_grid(widgets, equip_col);
  build_equipment_area(widgets, equip_col);
  build_side_notebook(widgets, bottom_hbox);
}

// Window-scoped actions: settings, quit, and the vault/character management set.
static void
register_window_actions(GtkApplication *app, AppWidgets *widgets, GtkWidget *window)
{
  // ── Actions ──
  GSimpleAction *settings_action = g_simple_action_new("settings", NULL);

  g_signal_connect(settings_action, "activate", G_CALLBACK(on_settings_action), widgets);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(settings_action));

  GSimpleAction *quit_action = g_simple_action_new("quit", NULL);

  g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(quit_action));

  // Manage Vaults / Characters actions
  register_manage_actions(GTK_WINDOW(window), widgets);
}

// Fill the vault and character selectors and load the two global stashes.
static void
load_initial_data(AppWidgets *widgets)
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

// Build the entire main window and present it.  Runs once, on GtkApplication's
// "activate"; every widget it creates is reachable from `widgets` afterwards.
void
ui_app_activate(GtkApplication *app, gpointer user_data)
{
  (void)user_data;
  AppWidgets *widgets = g_malloc0(sizeof(AppWidgets));

  widgets->texture_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  widgets->char_display_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  widgets->last_equip_tooltip_slot = -1;
  widgets->context_equip_slot = -1;

  build_context_menus(app, widgets);
  build_instant_tooltip(widgets);

  GdkPixbuf *test_relic = texture_load("Items\\AnimalRelics\\AnimalPart07B_L.tex");

  if(test_relic)
  {
    if(tqvc_debug)
      printf("DEBUG: AnimalPart07B_L.tex size: %dx%d\n", gdk_pixbuf_get_width(test_relic), gdk_pixbuf_get_height(test_relic));
    g_object_unref(test_relic);
  }

  apply_dark_theme_and_css();

  GtkWidget *window = build_main_window(app, widgets);

  build_header_bar(widgets, window);
  build_content_overlay(widgets, window);
  build_vault_pane(widgets);

  GtkWidget *char_panel = build_character_pane(widgets);

  build_bottom_section(widgets, char_panel);
  register_window_actions(app, widgets, window);

  if(global_config.save_folder)
    load_initial_data(widgets);

  // Save vault on close
  g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), widgets);

  gtk_window_present(GTK_WINDOW(window));

  // If the vault folder is missing, ask the user whether to create one or
  // point us at their existing vault data instead of silently showing nothing.
  if(global_config.save_folder)
    ensure_vault_folder(widgets);
}
