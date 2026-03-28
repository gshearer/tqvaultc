// ui_io.c -- character/vault loading lifecycle, bag button callbacks,
//            and the unsaved-changes confirmation dialog.
//
// Extracted from ui.c to keep individual source files under ~1500 lines.

#include "ui.h"
#include "config.h"
#include "prefetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Unsaved character confirmation dialog ─────────────────────────────
// Returns: 0=Save, 1=Discard, 2=Cancel
typedef struct {
  GMainLoop *loop;
  int result;
} ConfirmData;

// Callback: Save button clicked in the unsaved-changes dialog.
//   btn       - the button (unused)
//   user_data - ConfirmData*
static void
on_confirm_save(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  ConfirmData *cd = user_data;

  cd->result = 0;
  g_main_loop_quit(cd->loop);
}

// Callback: Discard button clicked in the unsaved-changes dialog.
//   btn       - the button (unused)
//   user_data - ConfirmData*
static void
on_confirm_discard(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  ConfirmData *cd = user_data;

  cd->result = 1;
  g_main_loop_quit(cd->loop);
}

// Callback: Cancel button clicked in the unsaved-changes dialog.
//   btn       - the button (unused)
//   user_data - ConfirmData*
static void
on_confirm_cancel(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  ConfirmData *cd = user_data;

  cd->result = 2;
  g_main_loop_quit(cd->loop);
}

// Callback: dialog window close-request (treated as Cancel).
//   win       - the dialog window (unused)
//   user_data - ConfirmData*
// Returns TRUE to prevent default close (loop handles it).
static gboolean
on_confirm_close(GtkWindow *win, gpointer user_data)
{
  (void)win;
  ConfirmData *cd = user_data;

  cd->result = 2;  // treat window close as Cancel
  g_main_loop_quit(cd->loop);
  return(TRUE);
}

// Show a modal dialog asking the user to Save, Discard, or Cancel unsaved changes.
//   widgets - app state (for character name and parent window)
// Returns 0=Save, 1=Discard, 2=Cancel.
int
confirm_unsaved_character(AppWidgets *widgets)
{
  const char *char_name = widgets->current_character
    ? widgets->current_character->character_name : "character";

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Unsaved Changes");
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               GTK_WINDOW(widgets->main_window));
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  char msg[512];

  snprintf(msg, sizeof(msg), "Save changes to %s?", char_name);
  GtkWidget *label = gtk_label_new(msg);

  gtk_box_append(GTK_BOX(vbox), label);

  GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_halign(btnbox, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(vbox), btnbox);

  ConfirmData cd = { .loop = g_main_loop_new(NULL, FALSE), .result = 2 };

  GtkWidget *save_btn = gtk_button_new_with_label("Save");

  g_signal_connect(save_btn, "clicked", G_CALLBACK(on_confirm_save), &cd);
  gtk_box_append(GTK_BOX(btnbox), save_btn);

  GtkWidget *discard_btn = gtk_button_new_with_label("Discard");

  g_signal_connect(discard_btn, "clicked", G_CALLBACK(on_confirm_discard), &cd);
  gtk_box_append(GTK_BOX(btnbox), discard_btn);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_confirm_cancel), &cd);
  gtk_box_append(GTK_BOX(btnbox), cancel_btn);

  g_signal_connect(dialog, "close-request", G_CALLBACK(on_confirm_close), &cd);

  gtk_window_present(GTK_WINDOW(dialog));
  g_main_loop_run(cd.loop);
  g_main_loop_unref(cd.loop);
  gtk_window_destroy(GTK_WINDOW(dialog));

  return(cd.result);
}

// ── Character combo repopulation helper ──────────────────────────────

// Rebuild the character dropdown, scanning the SaveData/Main directory.
// If select_name is non-NULL, select that entry; otherwise use the config default.
//   widgets     - app state
//   select_name - character folder name to select (or NULL)
void
repopulate_character_combo(AppWidgets *widgets, const char *select_name)
{
  // Block the handler while rebuilding -- gtk_string_list_append can fire
  // notify::selected (index goes from INVALID->0 on first item), which
  // triggers on_character_changed and overwrites last_character_path.
  g_signal_handler_block(widgets->character_combo, widgets->char_combo_handler);

  GtkStringList *sl = GTK_STRING_LIST(
    gtk_drop_down_get_model(GTK_DROP_DOWN(widgets->character_combo)));
  guint old_n = g_list_model_get_n_items(G_LIST_MODEL(sl));

  gtk_string_list_splice(sl, 0, old_n, NULL);

  char *main_path = g_build_filename(global_config.save_folder, "SaveData", "Main", NULL);
  GDir *d = g_dir_open(main_path, 0, NULL);

  g_free(main_path);
  if(!d)
  {
    g_signal_handler_unblock(widgets->character_combo, widgets->char_combo_handler);
    return;
  }

  const gchar *name;

  while((name = g_dir_read_name(d)) != NULL)
  {
    if(name[0] != '_')
      continue;
    gtk_string_list_append(sl, name);
  }
  g_dir_close(d);

  guint active_idx = 0;
  const char *target = select_name ? select_name : global_config.last_character_path;

  if(target)
  {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));

    for(guint i = 0; i < n; i++)
    {
      if(strcmp(gtk_string_list_get_string(sl, i), target) == 0)
      {
        active_idx = i;
        break;
      }
    }
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->character_combo), active_idx);
  g_signal_handler_unblock(widgets->character_combo, widgets->char_combo_handler);

  // Manually trigger the handler now that the correct selection is set
  on_character_changed(G_OBJECT(widgets->character_combo), NULL, widgets);
}

// Compare two string pointers for qsort.
//   a - pointer to const char*
//   b - pointer to const char*
// Returns strcmp result.
static int
compare_strings(const void *a, const void *b)
{
  return(strcmp(*(const char **)a, *(const char **)b));
}

// ── Vault combo repopulation helper ─────────────────────────────────

// Rebuild the vault dropdown, scanning the TQVaultData directory for .vault.json files.
// If select_name is non-NULL, select that entry; otherwise use the config default.
//   widgets     - app state
//   select_name - vault base name to select (or NULL)
void
repopulate_vault_combo(AppWidgets *widgets, const char *select_name)
{
  g_signal_handler_block(widgets->vault_combo, widgets->vault_combo_handler);

  GtkStringList *sl = GTK_STRING_LIST(
    gtk_drop_down_get_model(GTK_DROP_DOWN(widgets->vault_combo)));
  guint old_n = g_list_model_get_n_items(G_LIST_MODEL(sl));

  gtk_string_list_splice(sl, 0, old_n, NULL);

  char *vault_path = g_build_filename(global_config.save_folder, "TQVaultData", NULL);
  GDir *d = g_dir_open(vault_path, 0, NULL);

  g_free(vault_path);
  if(!d)
  {
    g_signal_handler_unblock(widgets->vault_combo, widgets->vault_combo_handler);
    return;
  }

  const gchar *dir_name;
  char **vault_names = NULL;
  int vault_count = 0, vault_cap = 0;
  const char *suffix = ".vault.json";
  size_t suffix_len = strlen(suffix);

  while((dir_name = g_dir_read_name(d)) != NULL)
  {
    size_t name_len = strlen(dir_name);

    if(name_len > suffix_len &&
       strcmp(dir_name + name_len - suffix_len, suffix) == 0)
    {
      size_t base_len = name_len - suffix_len;

      if(base_len > 255)
        base_len = 255;

      if(vault_count >= vault_cap)
      {
        vault_cap = vault_cap ? vault_cap * 2 : 16;
        vault_names = realloc(vault_names, (size_t)vault_cap * sizeof(char *));
        if(!vault_names)
          break;
      }
      vault_names[vault_count] = strndup(dir_name, base_len);
      vault_count++;
    }
  }
  g_dir_close(d);
  qsort(vault_names, (size_t)vault_count, sizeof(char *), compare_strings);

  for(int i = 0; i < vault_count; i++)
  {
    gtk_string_list_append(sl, vault_names[i]);
    free(vault_names[i]);
  }
  free(vault_names);

  guint active_idx = 0;
  const char *target = select_name ? select_name : global_config.last_vault_name;

  if(target)
  {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));

    for(guint i = 0; i < n; i++)
    {
      if(strcmp(gtk_string_list_get_string(sl, i), target) == 0)
      {
        active_idx = i;
        break;
      }
    }
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->vault_combo), active_idx);
  g_signal_handler_unblock(widgets->vault_combo, widgets->vault_combo_handler);

  // Manually trigger the handler now that the correct selection is set
  on_vault_changed(G_OBJECT(widgets->vault_combo), NULL, widgets);
}

// ── Load callbacks ──────────────────────────────────────────────────────

// Callback: Save Character button clicked.
//   btn       - the button (unused)
//   user_data - AppWidgets*
void
on_save_char_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  save_character_if_dirty(widgets);
}

// Callback: Database button clicked.
//   btn       - the button (unused)
//   user_data - AppWidgets*
void
on_database_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  show_database_dialog(widgets);
}

// Callback: Checklist button clicked.
//   btn       - the button (unused)
//   user_data - AppWidgets*
void
on_checklist_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  show_checklist_dialog(widgets);
}

// Callback: Attributes (stats) button clicked.
//   btn       - the button (unused)
//   user_data - AppWidgets*
void
on_stats_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  show_stats_dialog(widgets);
}

// Callback: Skills button clicked.
//   btn       - the button (unused)
//   user_data - AppWidgets*
void
on_skills_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  show_skills_dialog(widgets);
}

// Callback: Refresh Character button clicked. Reloads from disk.
//   btn       - the button (unused)
//   user_data - AppWidgets*
void
on_refresh_char_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!widgets->current_character)
    return;

  if(widgets->char_dirty)
  {
    int choice = confirm_unsaved_character(widgets);

    if(choice == 0)       // Save
      save_character_if_dirty(widgets);
    else if(choice == 2)  // Cancel
      return;
    // Discard: fall through to reload
  }

  cancel_held_item(widgets);
  TQCharacter *chr = character_load(widgets->current_character->filepath);

  if(chr)
  {
    update_ui(widgets, chr);
    prefetch_for_character(chr);
    run_search(widgets);
  }
}

// Callback: character dropdown selection changed.
//   obj       - the GtkDropDown widget
//   pspec     - property spec (unused)
//   user_data - AppWidgets*
void
on_character_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GtkWidget *combo = GTK_WIDGET(obj);
  AppWidgets *widgets = (AppWidgets *)user_data;

  clear_compare_item(widgets);
  cancel_held_item(widgets);

  if(widgets->char_dirty)
  {
    int choice = confirm_unsaved_character(widgets);

    if(choice == 0)
    {
      save_character_if_dirty(widgets);
    }
    else if(choice == 2)
    {
      // Cancel -- revert combo to current character
      g_signal_handler_block(combo, widgets->char_combo_handler);
      // Find and select the current character's folder name
      if(widgets->current_character && widgets->current_character->filepath)
      {
        const char *fp = widgets->current_character->filepath;

        // filepath is .../SaveData/Main/_CharName/Player.chr -- extract _CharName
        char *dir_part = g_path_get_dirname(fp);
        char *char_name = g_path_get_basename(dir_part);

        dropdown_select_by_name(combo, char_name);
        g_free(char_name);
        g_free(dir_part);
      }
      g_signal_handler_unblock(combo, widgets->char_combo_handler);
      return;
    }
    // Discard: reset dirty flag, continue with switch
    widgets->char_dirty = false;
    update_save_button_sensitivity(widgets);
  }

  // Save dirty player stash before switching
  if(widgets->player_stash && widgets->player_stash->dirty)
    stash_save(widgets->player_stash);

  if(widgets->player_stash)
  {
    stash_free(widgets->player_stash);
    widgets->player_stash = NULL;
  }

  char *name = dropdown_get_selected_text(combo);

  if(!name)
    return;

  config_set_last_character(name);
  config_save();

  char path[1024];

  snprintf(path, sizeof(path), "%s/SaveData/Main/%s/Player.chr", global_config.save_folder, name);

  // Load per-character player stash
  char *ps_path = stash_build_path(STASH_PLAYER, name);

  if(ps_path)
  {
    widgets->player_stash = stash_load(ps_path);
    free(ps_path);
  }

  g_free(name);

  TQCharacter *chr = character_load(path);

  if(chr)
  {
    update_ui(widgets, chr);
    prefetch_for_character(chr);
    run_search(widgets);
  }
  queue_redraw_all(widgets);
}

// Set the image on a bag button from a GdkPixbuf.
//   btn    - the GtkButton
//   pixbuf - the pixbuf to display
void
set_bag_btn_image(GtkWidget *btn, GdkPixbuf *pixbuf)
{
  if(!btn || !pixbuf)
    return;

  GtkWidget *child = gtk_button_get_child(GTK_BUTTON(btn));
  GBytes *bytes = g_bytes_new(gdk_pixbuf_get_pixels(pixbuf),
                              (gsize)gdk_pixbuf_get_height(pixbuf) * gdk_pixbuf_get_rowstride(pixbuf));
  GdkTexture *texture = gdk_memory_texture_new(
    gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
    gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
    bytes, gdk_pixbuf_get_rowstride(pixbuf));

  g_bytes_unref(bytes);

  if(child && GTK_IS_PICTURE(child))
  {
    // Reuse existing GtkPicture to avoid destroying a widget that GTK
    // may still reference internally (causes gtk_widget_compute_point
    // assertion failures and use-after-free on shutdown).
    gtk_picture_set_paintable(GTK_PICTURE(child), GDK_PAINTABLE(texture));
  }
  else
  {
    GtkWidget *img = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));

    gtk_picture_set_content_fit(GTK_PICTURE(img), GTK_CONTENT_FIT_FILL);
    gtk_picture_set_can_shrink(GTK_PICTURE(img), FALSE);
    gtk_button_set_child(GTK_BUTTON(btn), img);
  }
  g_object_unref(texture);
}

// Callback: vault dropdown selection changed. Loads the selected vault file.
//   obj       - the GtkDropDown widget
//   pspec     - property spec (unused)
//   user_data - AppWidgets*
void
on_vault_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GtkWidget *combo = GTK_WIDGET(obj);
  AppWidgets *widgets = (AppWidgets *)user_data;

  clear_compare_item(widgets);
  cancel_held_item(widgets);
  save_vault_if_dirty(widgets);

  char *name = dropdown_get_selected_text(combo);

  if(!name)
    return;

  char path[1024];

  snprintf(path, sizeof(path), "%s/TQVaultData/%s.vault.json", global_config.save_folder, name);

  // Reset bag to 0 only when switching to a different vault
  bool same_vault = global_config.last_vault_name &&
                    strcmp(global_config.last_vault_name, name) == 0;

  config_set_last_vault(name);
  if(!same_vault)
    config_set_last_vault_bag(0);
  config_save();
  g_free(name);

  if(widgets->current_vault)
    vault_free(widgets->current_vault);

  widgets->current_vault = vault_load_json(path);
  if(widgets->current_vault)
    prefetch_for_vault(widgets->current_vault);

  // Restore last viewed bag, or default to bag 0
  int restore_bag = global_config.last_vault_bag;
  int num_sacks = widgets->current_vault ? widgets->current_vault->num_sacks : 0;

  if(restore_bag < 0 || restore_bag >= num_sacks)
    restore_bag = 0;

  for(int i = 0; i < 12; i++)
  {
    if(widgets->vault_bag_pix[BAG_DOWN][i])
      set_bag_btn_image(widgets->vault_bag_btns[i],
                        widgets->vault_bag_pix[i == restore_bag ? BAG_UP : BAG_DOWN][i]);
  }
  widgets->current_sack = restore_bag;
  gtk_widget_queue_draw(widgets->vault_drawing_area);
  run_search(widgets);
}

// Callback: mouse enters a vault bag button (hover highlight).
//   ctrl      - motion controller
//   x, y      - cursor position (unused)
//   user_data - AppWidgets*
void
on_vault_bag_hover_enter(GtkEventControllerMotion *ctrl,
                         double x, double y, gpointer user_data)
{
  (void)ctrl; (void)x; (void)y;
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  if(idx != widgets->current_sack)
    set_bag_btn_image(btn, widgets->vault_bag_pix[BAG_OVER][idx]);
}

// Callback: mouse leaves a vault bag button (remove hover highlight).
//   ctrl      - motion controller
//   user_data - AppWidgets*
void
on_vault_bag_hover_leave(GtkEventControllerMotion *ctrl,
                         gpointer user_data)
{
  (void)ctrl;
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  if(idx != widgets->current_sack)
    set_bag_btn_image(btn, widgets->vault_bag_pix[BAG_DOWN][idx]);
}

// Callback: mouse enters a character bag button (hover highlight).
//   ctrl      - motion controller
//   x, y      - cursor position (unused)
//   user_data - AppWidgets*
void
on_char_bag_hover_enter(GtkEventControllerMotion *ctrl,
                        double x, double y, gpointer user_data)
{
  (void)ctrl; (void)x; (void)y;
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  if(idx != widgets->current_char_bag)
    set_bag_btn_image(btn, widgets->char_bag_pix[BAG_OVER][idx]);
}

// Callback: mouse leaves a character bag button (remove hover highlight).
//   ctrl      - motion controller
//   user_data - AppWidgets*
void
on_char_bag_hover_leave(GtkEventControllerMotion *ctrl,
                        gpointer user_data)
{
  (void)ctrl;
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  if(idx != widgets->current_char_bag)
    set_bag_btn_image(btn, widgets->char_bag_pix[BAG_DOWN][idx]);
}

// Callback: vault bag button clicked, switch to that sack.
//   btn       - the clicked bag button
//   user_data - AppWidgets*
void
on_bag_clicked(GtkButton *btn, gpointer user_data)
{
  AppWidgets *widgets = (AppWidgets *)user_data;

  cancel_held_item(widgets);
  int bag_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
  int prev = widgets->current_sack;

  if(prev != bag_idx)
  {
    set_bag_btn_image(widgets->vault_bag_btns[prev],
                      widgets->vault_bag_pix[BAG_DOWN][prev]);
    set_bag_btn_image(widgets->vault_bag_btns[bag_idx],
                      widgets->vault_bag_pix[BAG_UP][bag_idx]);
  }
  widgets->current_sack = bag_idx;
  config_set_last_vault_bag(bag_idx);
  config_save();
  gtk_widget_queue_draw(widgets->vault_drawing_area);
}

// Callback: character bag button clicked, switch to that bag.
//   btn       - the clicked bag button (unused for value, index from data)
//   user_data - AppWidgets*
void
on_char_bag_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  cancel_held_item(widgets);
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
  int prev = widgets->current_char_bag;

  if(prev != idx)
  {
    set_bag_btn_image(widgets->char_bag_btns[prev],
                      widgets->char_bag_pix[BAG_DOWN][prev]);
    set_bag_btn_image(widgets->char_bag_btns[idx],
                      widgets->char_bag_pix[BAG_UP][idx]);
  }
  widgets->current_char_bag = idx;
  gtk_widget_queue_draw(widgets->bag_drawing_area);
}

// ── Bag button right-click callbacks ─────────────────────────────────

// Callback: right-click on a vault bag button, show bag context menu.
//   gesture   - click gesture
//   n_press   - number of presses (unused)
//   x, y      - click position (unused)
//   user_data - AppWidgets*
void
on_vault_bag_right_click(GtkGestureClick *gesture, int n_press,
                         double x, double y, gpointer user_data)
{
  (void)n_press; (void)x; (void)y;
  AppWidgets *widgets = user_data;
  GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  show_bag_context_menu(widgets, btn, CONTAINER_VAULT, idx);
}

// Callback: right-click on a character bag button, show bag context menu.
//   gesture   - click gesture
//   n_press   - number of presses (unused)
//   x, y      - click position (unused)
//   user_data - AppWidgets*
void
on_char_bag_right_click(GtkGestureClick *gesture, int n_press,
                        double x, double y, gpointer user_data)
{
  (void)n_press; (void)x; (void)y;
  AppWidgets *widgets = user_data;
  GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  show_bag_context_menu(widgets, btn, CONTAINER_BAG, idx);
}
