// ui_settings.c -- settings dialog, about dialog, first-run setup, view build

#include "ui.h"
#include "config.h"
#include "translation.h"
#include "item_stats.h"
#include "asset_lookup.h"
#include "arz.h"
#include "affix_table.h"
#include "version.h"
#include "build_number.h"
#include <stdio.h>
#include <string.h>

// ── Folder validation ───────────────────────────────────────────────────

// Validate a candidate game folder. Returns NULL on success, or a
// caller-owned error message describing what's wrong (free with g_free).
static char *
validate_game_folder(const char *path)
{
  if(!path || !*path)
    return(g_strdup("Game folder is empty. Please choose your Titan Quest install directory."));

  if(!g_file_test(path, G_FILE_TEST_IS_DIR))
    return(g_strdup_printf("Game folder does not exist:\n%s", path));

  // The install must contain Database/database.arz and Resources/.
  // We check for those two specifically because they're the inputs
  // asset_manager_init walks; if either is missing the index will be empty.
  char *db = g_build_filename(path, "Database", "database.arz", NULL);
  char *res = g_build_filename(path, "Resources", NULL);
  bool db_ok = g_file_test(db, G_FILE_TEST_IS_REGULAR);
  bool res_ok = g_file_test(res, G_FILE_TEST_IS_DIR);
  char *err = NULL;

  if(!db_ok || !res_ok)
  {
    err = g_strdup_printf(
      "This doesn't look like a Titan Quest install folder:\n"
      "%s\n\n"
      "Expected to find:\n"
      "  • %s%s\n"
      "  • %s%s\n\n"
      "Pick the folder that contains the Database and Resources subfolders "
      "(typically \"…/steamapps/common/Titan Quest Anniversary Edition\").",
      path,
      "Database\\database.arz", db_ok ? " ✓" : " ✗ missing",
      "Resources\\",            res_ok ? " ✓" : " ✗ missing");
  }

  g_free(db);
  g_free(res);
  return(err);
}

// Validate a candidate save folder. Returns NULL on success, or a
// caller-owned error message (free with g_free).
static char *
validate_save_folder(const char *path)
{
  if(!path || !*path)
    return(g_strdup("Save folder is empty. Please choose your Titan Quest save directory."));

  if(!g_file_test(path, G_FILE_TEST_IS_DIR))
    return(g_strdup_printf("Save folder does not exist:\n%s", path));

  // TQ saves live under <save_folder>/SaveData/Main/_<charname>/.
  char *save_data = g_build_filename(path, "SaveData", NULL);
  bool ok = g_file_test(save_data, G_FILE_TEST_IS_DIR);

  g_free(save_data);
  if(!ok)
  {
    return(g_strdup_printf(
      "This doesn't look like a Titan Quest save folder:\n"
      "%s\n\n"
      "Expected to find a SaveData subfolder. Pick the folder that contains "
      "SaveData (typically \"…/Documents/My Games/Titan Quest - Immortal Throne\").",
      path));
  }
  return(NULL);
}

// Show a modal error dialog with the given message and run the optional
// callback when the user dismisses it. Used to keep folder-picker wizards
// open after a validation failure.
static void
show_error(GtkWindow *parent, const char *message)
{
  GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", message);

  gtk_alert_dialog_set_modal(dlg, TRUE);
  gtk_alert_dialog_show(dlg, parent);
  g_object_unref(dlg);
}

// ── Browse folder helpers (shared by settings + first-run) ──────────────

// Callback for folder selection dialog completion.
//
// source: the GtkFileDialog
// result: async result
// user_data: GtkEntry to populate with the selected path
static void
on_browse_folder_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  GtkEntry *entry = GTK_ENTRY(user_data);
  GFile *folder = gtk_file_dialog_select_folder_finish(dlg, result, NULL);

  if(folder)
  {
    char *path = g_file_get_path(folder);

    if(path)
    {
      gtk_editable_set_text(GTK_EDITABLE(entry), path);
      g_free(path);
    }

    g_object_unref(folder);
  }
}

// Open a folder selection dialog pre-populated with the entry's current path.
//
// btn: the Browse button
// user_data: GtkEntry to read current path from and write result to
static void
on_browse_clicked(GtkButton *btn, gpointer user_data)
{
  GtkEntry *entry = GTK_ENTRY(user_data);
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_WINDOW));
  GtkFileDialog *dlg = gtk_file_dialog_new();

  gtk_file_dialog_set_title(dlg, "Select Folder");

  // Pre-select current path if set
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(entry));

  if(cur && cur[0])
  {
    GFile *initial = g_file_new_for_path(cur);

    gtk_file_dialog_set_initial_folder(dlg, initial);
    g_object_unref(initial);
  }

  gtk_file_dialog_select_folder(dlg, parent, NULL, on_browse_folder_ready, entry);
}

// ── Settings dialog ─────────────────────────────────────────────────────

typedef struct {
  GtkEntry *save_folder_entry;
  GtkEntry *game_folder_entry;
  AppWidgets *app_widgets;
} SettingsWidgets;

// Save settings, close the dialog, and reload translations/combos.
//
// btn: the Save & Close button (unused)
// user_data: SettingsWidgets pointer
static void
on_settings_close(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SettingsWidgets *sw = (SettingsWidgets *)user_data;
  GtkWidget *window = gtk_widget_get_ancestor(GTK_WIDGET(sw->save_folder_entry), GTK_TYPE_WINDOW);

  const char *game = gtk_editable_get_text(GTK_EDITABLE(sw->game_folder_entry));
  const char *save = gtk_editable_get_text(GTK_EDITABLE(sw->save_folder_entry));

  char *err = validate_game_folder(game);
  if(!err)
    err = validate_save_folder(save);
  if(err)
  {
    show_error(GTK_WINDOW(window), err);
    g_free(err);
    return;  // keep settings open
  }

  config_set_save_folder(save);
  config_set_game_folder(game);
  config_save();

  gtk_window_destroy(GTK_WINDOW(window));

  // Reload translations and repopulate combos after settings change
  AppWidgets *widgets = sw->app_widgets;

  if(widgets)
  {
    if(global_config.game_folder && !widgets->translations)
    {
      widgets->translations = translation_init();
      char trans_path[1024];

      snprintf(trans_path, sizeof(trans_path), "%s/Text/Text_EN.arc",
               global_config.game_folder);
      translation_load_from_arc(widgets->translations, trans_path);
    }

    if(global_config.save_folder)
    {
      repopulate_vault_combo(widgets, NULL);
      repopulate_character_combo(widgets, NULL);
    }
  }
}

// ── "View Build" window ─────────────────────────────────────────────────

// 12 equipment slots arranged in 2 rows x 6 columns for the build overview.
// Row 0: Right, Left, Head, Neck, Chest, Legs
// Row 1: AltRight, AltLeft, Arms, Ring 1, Ring 2, Artifact
static const struct { int idx; const char *label; } build_grid[2][6] = {
  {{ 7, "Right"  }, { 8, "Left"  }, { 0, "Head" }, { 1, "Neck"  }, { 2, "Chest" }, { 3, "Legs"     }},
  {{ 9, "AltRight" }, {10, "AltLeft"}, { 4, "Arms" }, { 5, "Ring 1"}, { 6, "Ring 2"}, {11, "Artifact" }},
};

// Show the "View Build" window with a 2x6 grid of equipped item tooltips.
//
// btn: the View Build button (unused)
// user_data: AppWidgets pointer
void
on_view_build_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = user_data;

  if(!widgets->current_character)
    return;

  TQCharacter *ch = widgets->current_character;
  char title[256];

  snprintf(title, sizeof(title), "Build: %s", ch->character_name ? ch->character_name : "Unknown");

  GtkWidget *win = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(win), title);
  gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(widgets->main_window));
  gtk_window_set_default_size(GTK_WINDOW(win), 1600, 900);
  gtk_window_set_modal(GTK_WINDOW(win), FALSE);

  GtkWidget *scrolled = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_widget_set_hexpand(scrolled, TRUE);

  // 2-row x 6-column grid: each cell is a vertical box with header + tooltip
  GtkWidget *grid = gtk_grid_new();

  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  gtk_widget_set_margin_start(grid, 8);
  gtk_widget_set_margin_end(grid, 8);
  gtk_widget_set_margin_top(grid, 8);
  gtk_widget_set_margin_bottom(grid, 8);

  for(int row = 0; row < 2; row++)
  {
    for(int col = 0; col < 6; col++)
    {
      int slot = build_grid[row][col].idx;
      const char *slot_label = build_grid[row][col].label;
      GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

      gtk_widget_set_valign(cell, GTK_ALIGN_START);

      // Slot header -- always shown
      GtkWidget *hdr = gtk_label_new(NULL);
      char hdr_markup[128];

      snprintf(hdr_markup, sizeof(hdr_markup), "<b>%s</b>", slot_label);
      gtk_label_set_markup(GTK_LABEL(hdr), hdr_markup);
      gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
      gtk_box_append(GTK_BOX(cell), hdr);

      GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

      gtk_box_append(GTK_BOX(cell), sep);

      TQItem *eq = ch->equipment[slot];

      if(eq)
      {
        char markup[16384];

        markup[0] = '\0';
        item_format_stats(eq, widgets->translations, markup, sizeof(markup));

        GtkWidget *label = gtk_label_new(NULL);

        gtk_label_set_markup(GTK_LABEL(label), markup);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_box_append(GTK_BOX(cell), label);
      }

      else
      {
        GtkWidget *empty = gtk_label_new("(empty)");

        gtk_widget_set_opacity(empty, 0.5);
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0f);
        gtk_box_append(GTK_BOX(cell), empty);
      }

      gtk_grid_attach(GTK_GRID(grid), cell, col, row, 1, 1);
    }
  }

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), grid);
  gtk_window_set_child(GTK_WINDOW(win), scrolled);
  gtk_window_present(GTK_WINDOW(win));
}

// ── About dialog ────────────────────────────────────────────────────────

// Show the About dialog with version info and credits.
//
// btn: the About button (unused)
// user_data: AppWidgets pointer
void
on_about_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "About TQVaultC");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 450, 520);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

  gtk_widget_set_margin_start(vbox, 30);
  gtk_widget_set_margin_end(vbox, 30);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  GtkWidget *logo = gtk_image_new_from_resource("/org/tqvaultc/tqvaultc_logo_256.png");

  gtk_widget_set_size_request(logo, 300, 300);
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 300);
  gtk_box_append(GTK_BOX(vbox), logo);

  GtkWidget *name_label = gtk_label_new("Titan Quest Vault in C (TQVaultC)");
  PangoAttrList *attrs = pango_attr_list_new();

  pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  pango_attr_list_insert(attrs, pango_attr_scale_new(1.3));
  gtk_label_set_attributes(GTK_LABEL(name_label), attrs);
  pango_attr_list_unref(attrs);
  gtk_box_append(GTK_BOX(vbox), name_label);

  char ver_str[64];

  snprintf(ver_str, sizeof(ver_str), "Version %s  (Build #%d)", TQVAULTC_VERSION, TQVAULTC_BUILD_NUMBER);
  gtk_box_append(GTK_BOX(vbox), gtk_label_new(ver_str));

  gtk_box_append(GTK_BOX(vbox), gtk_label_new(""));
  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Designed by George Shearer"));
  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Written by Claude"));
  gtk_box_append(GTK_BOX(vbox), gtk_label_new("george@shearer.tech"));
  gtk_box_append(GTK_BOX(vbox), gtk_label_new("https://github.com/gshearer/tqvaultc"));

  GtkWidget *close_btn = gtk_button_new_with_label("Close");

  gtk_widget_set_margin_top(close_btn, 15);
  g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
  gtk_box_append(GTK_BOX(vbox), close_btn);

  gtk_window_present(GTK_WINDOW(dialog));
}

// ── Settings action (invoked from menu or button) ───────────────────────

// Show the Settings dialog for configuring save and game folders.
//
// action: the GSimpleAction (unused)
// parameter: action parameter (unused)
// user_data: AppWidgets pointer
void
on_settings_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action; (void)parameter;
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Settings");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 350);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  SettingsWidgets *sw = g_malloc(sizeof(SettingsWidgets));

  sw->app_widgets = widgets;
  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Character Save Folder:"));

  GtkWidget *hbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  gtk_box_append(GTK_BOX(vbox), hbox1);
  sw->save_folder_entry = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(sw->save_folder_entry), TRUE);

  if(global_config.save_folder)
    gtk_editable_set_text(GTK_EDITABLE(sw->save_folder_entry), global_config.save_folder);

  gtk_box_append(GTK_BOX(hbox1), GTK_WIDGET(sw->save_folder_entry));

  GtkWidget *browse_save_btn = gtk_button_new_with_label("Browse...");

  g_signal_connect(browse_save_btn, "clicked", G_CALLBACK(on_browse_clicked), sw->save_folder_entry);
  gtk_box_append(GTK_BOX(hbox1), browse_save_btn);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Game Installation Folder:"));

  GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  gtk_box_append(GTK_BOX(vbox), hbox2);
  sw->game_folder_entry = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(sw->game_folder_entry), TRUE);

  if(global_config.game_folder)
    gtk_editable_set_text(GTK_EDITABLE(sw->game_folder_entry), global_config.game_folder);

  gtk_box_append(GTK_BOX(hbox2), GTK_WIDGET(sw->game_folder_entry));

  GtkWidget *browse_game_btn = gtk_button_new_with_label("Browse...");

  g_signal_connect(browse_game_btn, "clicked", G_CALLBACK(on_browse_clicked), sw->game_folder_entry);
  gtk_box_append(GTK_BOX(hbox2), browse_game_btn);

  GtkWidget *close_button = gtk_button_new_with_label("Save & Close");

  gtk_widget_set_margin_top(close_button, 20);
  g_signal_connect(close_button, "clicked", G_CALLBACK(on_settings_close), sw);
  gtk_box_append(GTK_BOX(vbox), close_button);
  g_object_set_data_full(G_OBJECT(dialog), "settings-widgets", sw, g_free);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ── First-run setup flow ────────────────────────────────────────────────

typedef struct {
  GtkApplication *app;
  GtkEntry *save_folder_entry;
  GtkEntry *game_folder_entry;
} FirstRunData;

// Save first-run settings and launch the main UI.
//
// btn: the Save & Continue button (unused)
// user_data: FirstRunData pointer
static void
on_first_run_save(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  FirstRunData *fr = user_data;
  GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(fr->save_folder_entry), GTK_TYPE_WINDOW);

  const char *game = gtk_editable_get_text(GTK_EDITABLE(fr->game_folder_entry));
  const char *save = gtk_editable_get_text(GTK_EDITABLE(fr->save_folder_entry));

  char *err = validate_game_folder(game);
  if(!err)
    err = validate_save_folder(save);
  if(err)
  {
    show_error(GTK_WINDOW(win), err);
    g_free(err);
    return;  // keep the wizard open
  }

  config_set_save_folder(save);
  config_set_game_folder(game);
  config_save();

  GtkApplication *app = fr->app;

  gtk_window_destroy(GTK_WINDOW(win));

  // Run the same init sequence as on_activate(). Without this, the asset
  // manager (DBR cache, ARZ mmaps), intern table, item stats, and affix
  // tables stay uninitialized — which on first run produces missing item
  // textures and "g_hash_table_lookup: hash_table != NULL" spam.
  if(global_config.game_folder)
  {
    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);
  }

  ui_app_activate(app, NULL);
}

// Deferred callback to show the About dialog once the settings window is mapped.
//
// user_data: the settings window widget
// returns: G_SOURCE_REMOVE to run only once
static gboolean
show_first_run_about(gpointer user_data)
{
  GtkWidget *settings_win = user_data;
  GtkWidget *about = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(about), "About TQVaultC");
  gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(settings_win));
  gtk_window_set_modal(GTK_WINDOW(about), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(about), 450, 520);

  GtkWidget *avbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

  gtk_widget_set_margin_start(avbox, 30);
  gtk_widget_set_margin_end(avbox, 30);
  gtk_widget_set_margin_top(avbox, 20);
  gtk_widget_set_margin_bottom(avbox, 20);
  gtk_widget_set_halign(avbox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(avbox, GTK_ALIGN_CENTER);
  gtk_window_set_child(GTK_WINDOW(about), avbox);

  GtkWidget *logo = gtk_image_new_from_resource("/org/tqvaultc/tqvaultc_logo_256.png");

  gtk_widget_set_size_request(logo, 300, 300);
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 300);
  gtk_box_append(GTK_BOX(avbox), logo);

  GtkWidget *name_label = gtk_label_new("Titan Quest Vault in C (TQVaultC)");
  PangoAttrList *attrs = pango_attr_list_new();

  pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  pango_attr_list_insert(attrs, pango_attr_scale_new(1.3));
  gtk_label_set_attributes(GTK_LABEL(name_label), attrs);
  pango_attr_list_unref(attrs);
  gtk_box_append(GTK_BOX(avbox), name_label);

  char ver_str[64];

  snprintf(ver_str, sizeof(ver_str), "Version %s  (Build #%d)",
           TQVAULTC_VERSION, TQVAULTC_BUILD_NUMBER);
  gtk_box_append(GTK_BOX(avbox), gtk_label_new(ver_str));

  gtk_box_append(GTK_BOX(avbox), gtk_label_new(""));
  gtk_box_append(GTK_BOX(avbox), gtk_label_new("Designed by George Shearer"));
  gtk_box_append(GTK_BOX(avbox), gtk_label_new("Written by Claude"));
  gtk_box_append(GTK_BOX(avbox), gtk_label_new("george@shearer.tech"));
  gtk_box_append(GTK_BOX(avbox), gtk_label_new("https://github.com/gshearer/tqvaultc"));

  GtkWidget *close_btn = gtk_button_new_with_label("Close");

  gtk_widget_set_margin_top(close_btn, 15);
  g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy), about);
  gtk_box_append(GTK_BOX(avbox), close_btn);

  gtk_window_present(GTK_WINDOW(about));
  return(G_SOURCE_REMOVE);
}

// Show the first-run setup window with save/game folder configuration.
//
// app: the GtkApplication instance
void
ui_first_run_setup(GtkApplication *app)
{
  // Settings window -- standalone, no transient parent
  GtkWidget *settings_win = gtk_application_window_new(app);

  gtk_window_set_title(GTK_WINDOW(settings_win), "TQVaultC \u2014 First-Run Setup");
  gtk_window_set_default_size(GTK_WINDOW(settings_win), 600, 250);
  gtk_window_set_resizable(GTK_WINDOW(settings_win), FALSE);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(settings_win), vbox);

  FirstRunData *fr = g_malloc(sizeof(FirstRunData));

  fr->app = app;

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Character Save Folder:"));

  GtkWidget *hbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  gtk_box_append(GTK_BOX(vbox), hbox1);
  fr->save_folder_entry = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(fr->save_folder_entry), TRUE);

  if(global_config.save_folder)
    gtk_editable_set_text(GTK_EDITABLE(fr->save_folder_entry), global_config.save_folder);

  gtk_box_append(GTK_BOX(hbox1), GTK_WIDGET(fr->save_folder_entry));

  GtkWidget *browse_save = gtk_button_new_with_label("Browse...");

  g_signal_connect(browse_save, "clicked", G_CALLBACK(on_browse_clicked), fr->save_folder_entry);
  gtk_box_append(GTK_BOX(hbox1), browse_save);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Game Installation Folder:"));

  GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  gtk_box_append(GTK_BOX(vbox), hbox2);
  fr->game_folder_entry = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(fr->game_folder_entry), TRUE);

  if(global_config.game_folder)
    gtk_editable_set_text(GTK_EDITABLE(fr->game_folder_entry), global_config.game_folder);

  gtk_box_append(GTK_BOX(hbox2), GTK_WIDGET(fr->game_folder_entry));

  GtkWidget *browse_game = gtk_button_new_with_label("Browse...");

  g_signal_connect(browse_game, "clicked", G_CALLBACK(on_browse_clicked), fr->game_folder_entry);
  gtk_box_append(GTK_BOX(hbox2), browse_game);

  GtkWidget *save_btn = gtk_button_new_with_label("Save & Continue");

  gtk_widget_set_margin_top(save_btn, 20);
  g_signal_connect(save_btn, "clicked", G_CALLBACK(on_first_run_save), fr);
  gtk_box_append(GTK_BOX(vbox), save_btn);
  g_object_set_data_full(G_OBJECT(settings_win), "first-run-data", fr, g_free);

  gtk_window_present(GTK_WINDOW(settings_win));

  // Defer about dialog until the settings window is fully mapped
  g_idle_add(show_first_run_about, settings_win);
}
