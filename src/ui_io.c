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
#include <glib/gstdio.h>

// ── Unsaved character confirmation dialog ─────────────────────────────
typedef struct {
  GMainLoop *loop;
  ConfirmUnsaved result;
} ConfirmData;

// Callback: Save button clicked in the unsaved-changes dialog.
//   btn       - the button (unused)
//   user_data - ConfirmData*
static void
on_confirm_save(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  ConfirmData *cd = user_data;

  cd->result = CONFIRM_SAVE;
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

  cd->result = CONFIRM_DISCARD;
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

  cd->result = CONFIRM_CANCEL;
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

  cd->result = CONFIRM_CANCEL;  // treat window close as Cancel
  g_main_loop_quit(cd->loop);
  return(TRUE);
}

// Show a modal dialog asking the user to Save, Discard, or Cancel unsaved changes.
//   widgets - app state (for character name and parent window)
ConfirmUnsaved
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

  // Tailor the prompt to what is actually unsaved: character data, the storage
  // vaults (transfer / player / relic stash tabs), or both.
  bool stash_dirty = (widgets->transfer_stash && widgets->transfer_stash->dirty) ||
                     (widgets->player_stash   && widgets->player_stash->dirty) ||
                     (widgets->relic_vault    && widgets->relic_vault->dirty);

  char msg[512];

  if(widgets->char_dirty && stash_dirty)
    snprintf(msg, sizeof(msg), "Save changes to %s and the storage vaults?", char_name);
  else if(stash_dirty)
    snprintf(msg, sizeof(msg), "Save changes to the storage vaults?");
  else
    snprintf(msg, sizeof(msg), "Save changes to %s?", char_name);

  GtkWidget *label = gtk_label_new(msg);

  gtk_box_append(GTK_BOX(vbox), label);

  GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_halign(btnbox, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(vbox), btnbox);

  ConfirmData cd = { .loop = g_main_loop_new(NULL, FALSE), .result = CONFIRM_CANCEL };

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

// Compare two string pointers for qsort (forward decl; defined below).
static int compare_strings(const void *a, const void *b);

// Build the "📱 …" combo markup for an open mobile char (forward decl).
static char *build_mobile_display_markup(const TQCharacter *chr, TQTranslation *tr);

// Build the character's "type" string from its masteries. See header for details.
void
character_type_string(const TQCharacter *chr, TQTranslation *tr,
                      bool with_masteries, char *out, size_t outsz)
{
  if(!out || outsz == 0)
    return;

  out[0] = '\0';
  if(!chr)
    return;

  if(chr->mastery1 && chr->mastery2)
  {
    char m1[64], m2[64];

    character_mastery_display_name(chr->mastery1, m1, sizeof(m1));
    character_mastery_display_name(chr->mastery2, m2, sizeof(m2));

    // Dual mastery: prefer the translated class title from the save's tag.
    const char *cls = (tr && chr->class_tag) ? translation_get(tr, chr->class_tag) : NULL;

    if(cls && cls[0])
    {
      if(with_masteries)
        g_snprintf(out, outsz, "%s (%s + %s)", cls, m1, m2);
      else
        g_strlcpy(out, cls, outsz);
    }
    else
    {
      // Fallback (e.g. no game files loaded): just combine the mastery names.
      g_snprintf(out, outsz, "%s + %s", m1, m2);
    }
  }
  else if(chr->mastery1)
  {
    character_mastery_display_name(chr->mastery1, out, outsz);
  }
}

// Build the dropdown display label (Pango markup) for one character folder.
// Format: "name <dim>- Type . Lv N</dim>", with the leading '_' stripped.
//   folder - save folder name (e.g. "_wu")
//   chr    - loaded character (may be NULL on load failure)
//   tr     - translation table (may be NULL)
// Returns a newly-allocated markup string (caller frees).
static char *
build_char_display_markup(const char *folder, const TQCharacter *chr, TQTranslation *tr)
{
  const char *display_name = (folder[0] == '_') ? folder + 1 : folder;
  char *name_esc = g_markup_escape_text(display_name, -1);

  if(!chr)
  {
    char *out = g_strdup(name_esc);

    g_free(name_esc);
    return(out);
  }

  char type[96];

  character_type_string(chr, tr, false, type, sizeof(type));

  char *type_esc = g_markup_escape_text(type, -1);
  char *detail;

  if(type[0])
    detail = g_strdup_printf(" \xe2\x80\x94 %s \xc2\xb7 Lv %u", type_esc, chr->level);
  else
    detail = g_strdup_printf(" \xc2\xb7 Lv %u", chr->level);

  char *out = g_strdup_printf("%s<span alpha='55%%'>%s</span>", name_esc, detail);

  g_free(name_esc);
  g_free(type_esc);
  g_free(detail);
  return(out);
}

// GtkSignalListItemFactory "setup": create the row's label widget.
static void
char_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
  (void)factory; (void)user_data;
  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
  gtk_list_item_set_child(item, label);
}

// GtkSignalListItemFactory "bind": fill the label from the display map,
// keyed by the folder name held in the model's GtkStringObject.
static void
char_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
  (void)factory;
  AppWidgets *widgets = (AppWidgets *)user_data;
  GtkWidget *label = gtk_list_item_get_child(item);
  GtkStringObject *obj = gtk_list_item_get_item(item);

  if(!label || !obj)
    return;

  const char *folder = gtk_string_object_get_string(obj);
  const char *markup = folder ? g_hash_table_lookup(widgets->char_display_map, folder) : NULL;

  if(markup)
    gtk_label_set_markup(GTK_LABEL(label), markup);
  else
    gtk_label_set_text(GTK_LABEL(label), folder ? folder : "");
}

void
install_character_combo_factory(AppWidgets *widgets)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  g_signal_connect(factory, "setup", G_CALLBACK(char_item_setup), widgets);
  g_signal_connect(factory, "bind", G_CALLBACK(char_item_bind), widgets);
  gtk_drop_down_set_factory(GTK_DROP_DOWN(widgets->character_combo), factory);
  g_object_unref(factory);
}

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

  // Collect character folders, then sort alphabetically before inserting.
  const gchar *name;
  char **names = NULL;
  int count = 0, cap = 0;

  while((name = g_dir_read_name(d)) != NULL)
  {
    if(name[0] != '_')
      continue;

    if(count >= cap)
    {
      int newcap = cap ? cap * 2 : 16;
      char **grown = realloc(names, (size_t)newcap * sizeof(char *));

      // On OOM keep the names gathered so far (grown==NULL would orphan them and
      // leave qsort() below with a NULL base and count>0 — undefined behaviour).
      if(!grown)
        break;

      names = grown;
      cap = newcap;
    }
    names[count++] = strdup(name);
  }
  g_dir_close(d);
  qsort(names, (size_t)count, sizeof(char *), compare_strings);

  // Rebuild the display-name map (folder -> pretty markup) and the model.
  g_hash_table_remove_all(widgets->char_display_map);

  for(int i = 0; i < count; i++)
  {
    char chr_path[1024];

    snprintf(chr_path, sizeof(chr_path), "%s/SaveData/Main/%s/Player.chr",
             global_config.save_folder, names[i]);

    TQCharacter *chr = character_load(chr_path);
    char *markup = build_char_display_markup(names[i], chr, widgets->translations);

    g_hash_table_insert(widgets->char_display_map, strdup(names[i]), markup);
    if(chr)
      character_free(chr);

    gtk_string_list_append(sl, names[i]);
    free(names[i]);
  }
  free(names);

  // Preserve the open external (iOS mobile) entry across a rebuild — the scan
  // above only covers SaveData/Main, and remove_all() cleared its display map
  // entry. Re-add it so switching back to the mobile char keeps working.
  if(widgets->mobile_folder)
  {
    char mob_chr[1024];

    snprintf(mob_chr, sizeof(mob_chr), "%s/Player.chr", widgets->mobile_folder);

    TQCharacter *mob = character_load(mob_chr);

    if(mob)
    {
      g_hash_table_insert(widgets->char_display_map, g_strdup(widgets->mobile_folder),
                          build_mobile_display_markup(mob, widgets->translations));
      gtk_string_list_append(sl, widgets->mobile_folder);
      character_free(mob);
    }
    else
    {
      // The folder vanished (e.g. unmounted) — forget the stale entry.
      g_free(widgets->mobile_folder);
      widgets->mobile_folder = NULL;
    }
  }

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

// ── External (iOS mobile) save folder ───────────────────────────────────
//
// A copied iOS save is opened from an arbitrary folder rather than the
// SaveData/Main scan. The chosen folder path doubles as the character combo's
// entry string; on_character_changed routes any (re)load of that entry to
// <folder>/Player.chr + <folder>/0winsys.dxb. See open_external_character_folder.

// Build the distinctive combo markup for an open mobile character:
// "📱 name <dim>— Type · Lv N · mobile</dim>". Mirrors build_char_display_markup
// but flags the entry as an external save.
//   chr - the loaded mobile character (non-NULL)
//   tr  - translation table for the class title
// Returns newly-allocated Pango markup (caller g_free's).
static char *
build_mobile_display_markup(const TQCharacter *chr, TQTranslation *tr)
{
  const char *nm = chr->character_name ? chr->character_name : "mobile";
  char *name_esc = g_markup_escape_text(nm, -1);

  char type[96];

  character_type_string(chr, tr, false, type, sizeof(type));

  char *type_esc = g_markup_escape_text(type, -1);
  char *detail;

  if(type[0])
    detail = g_strdup_printf(" \xe2\x80\x94 %s \xc2\xb7 Lv %u \xc2\xb7 mobile", type_esc, chr->level);
  else
    detail = g_strdup_printf(" \xc2\xb7 Lv %u \xc2\xb7 mobile", chr->level);

  char *out = g_strdup_printf("\xf0\x9f\x93\xb1 %s<span alpha='55%%'>%s</span>",
                              name_esc, detail);

  g_free(name_esc);
  g_free(type_esc);
  g_free(detail);
  return(out);
}

// Return the index of `s` in the character combo's string-list model, or -1.
static int
char_combo_find(AppWidgets *widgets, const char *s)
{
  GtkStringList *sl = GTK_STRING_LIST(
    gtk_drop_down_get_model(GTK_DROP_DOWN(widgets->character_combo)));
  guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));

  for(guint i = 0; i < n; i++)
    if(strcmp(gtk_string_list_get_string(sl, i), s) == 0)
      return((int)i);

  return(-1);
}

// Show a modal error alert anchored to the main window.
static void
mobile_error(AppWidgets *widgets, const char *msg)
{
  GtkAlertDialog *e = gtk_alert_dialog_new("%s", msg);

  gtk_alert_dialog_set_modal(e, TRUE);
  gtk_alert_dialog_show(e, GTK_WINDOW(widgets->main_window));
  g_object_unref(e);
}

bool
open_external_character_folder(AppWidgets *widgets, const char *folder)
{
  if(!folder || !folder[0])
    return(false);

  char chr_path[1024];

  snprintf(chr_path, sizeof(chr_path), "%s/Player.chr", folder);

  if(!g_file_test(chr_path, G_FILE_TEST_EXISTS))
  {
    mobile_error(widgets,
      "No Player.chr in that folder.\n\n"
      "Pick the character's save folder — the one that holds Player.chr.");
    return(false);
  }

  // Probe to validate it's a mobile save and to build the combo label. The
  // actual load happens via on_character_changed once the entry is selected.
  TQCharacter *probe = character_load(chr_path);

  if(!probe)
  {
    mobile_error(widgets, "Could not read Player.chr in that folder.");
    return(false);
  }

  if(!character_is_mobile(probe))
  {
    character_free(probe);
    mobile_error(widgets,
      "That isn't a mobile (iOS) save.\n\n"
      "Desktop characters open from the normal character list.");
    return(false);
  }

  char *markup = build_mobile_display_markup(probe, widgets->translations);

  character_free(probe);

  GtkStringList *sl = GTK_STRING_LIST(
    gtk_drop_down_get_model(GTK_DROP_DOWN(widgets->character_combo)));

  // Mutate the model with the change handler blocked, then fire it once
  // explicitly (mirrors repopulate_character_combo).
  g_signal_handler_block(widgets->character_combo, widgets->char_combo_handler);

  // Single external slot: drop any previously-open mobile entry first.
  if(widgets->mobile_folder && strcmp(widgets->mobile_folder, folder) != 0)
  {
    int old = char_combo_find(widgets, widgets->mobile_folder);

    if(old >= 0)
      gtk_string_list_remove(sl, (guint)old);

    g_hash_table_remove(widgets->char_display_map, widgets->mobile_folder);
  }
  g_free(widgets->mobile_folder);
  widgets->mobile_folder = g_strdup(folder);

  // Combo string = folder path; display map -> the "📱 …" markup.
  g_hash_table_insert(widgets->char_display_map, g_strdup(folder), markup);
  if(char_combo_find(widgets, folder) < 0)
    gtk_string_list_append(sl, folder);

  dropdown_select_by_name(widgets->character_combo, folder);
  g_signal_handler_unblock(widgets->character_combo, widgets->char_combo_handler);

  // Load the character + page-0 stash now (handler was blocked during select).
  on_character_changed(G_OBJECT(widgets->character_combo), NULL, widgets);
  return(true);
}

// Folder-picker completion for "Open Mobile Save Folder…".
//   source    - the GtkFileDialog
//   result    - async result
//   user_data - AppWidgets*
static void
on_mobile_save_folder_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  AppWidgets *widgets = (AppWidgets *)user_data;
  GFile *folder = gtk_file_dialog_select_folder_finish(dlg, result, NULL);

  if(!folder)
    return;  // cancelled

  char *dir = g_file_get_path(folder);

  g_object_unref(folder);
  if(!dir)
    return;

  open_external_character_folder(widgets, dir);
  g_free(dir);
}

void
open_mobile_save_choose_folder(AppWidgets *widgets)
{
  GtkFileDialog *dlg = gtk_file_dialog_new();

  gtk_file_dialog_set_title(dlg, "Open Mobile (iOS) Save Folder");

  const char *home = g_get_home_dir();

  if(home)
  {
    GFile *initial = g_file_new_for_path(home);

    gtk_file_dialog_set_initial_folder(dlg, initial);
    g_object_unref(initial);
  }
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(widgets->main_window), NULL,
                                on_mobile_save_folder_ready, widgets);
}

// ── Missing-vault-folder prompt ─────────────────────────────────────
//
// When the vault directory can't be found we don't fail silently: we ask the
// user whether to create a new vault there, point us at an existing vault
// folder, or cancel. The chosen folder is only remembered (persisted as
// vault_folder) when it actually holds vault data or the user creates a brand
// new vault in it -- never for a folder we rejected as empty.

// Carries state from the alert dialog to its async response callback.
//   widgets - app state
//   dir     - folder the "Create New Vault" action should create in (owned)
typedef struct {
  AppWidgets *widgets;
  char *dir;
} VaultPromptCtx;

static void vault_prompt_not_found(AppWidgets *widgets, const char *dir);

// Return TRUE if `dir` contains at least one *.vault.json file.
static gboolean
vault_dir_has_data(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);

  if(!d)
    return(FALSE);

  const char *suffix = ".vault.json";
  size_t slen = strlen(suffix);
  const gchar *nm;
  gboolean found = FALSE;

  while((nm = g_dir_read_name(d)) != NULL)
  {
    size_t l = strlen(nm);

    if(l > slen && strcmp(nm + l - slen, suffix) == 0)
    {
      found = TRUE;
      break;
    }
  }
  g_dir_close(d);
  return(found);
}

// Folder-picker completion: remember + load the chosen folder if it holds
// vault data, otherwise re-show the not-found prompt for that folder.
//   source    - the GtkFileDialog
//   result    - async result
//   user_data - AppWidgets*
static void
on_vault_choose_folder_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  AppWidgets *widgets = (AppWidgets *)user_data;
  GFile *folder = gtk_file_dialog_select_folder_finish(dlg, result, NULL);

  if(!folder)
    return;  // picker cancelled -> stop

  char *dir = g_file_get_path(folder);

  g_object_unref(folder);
  if(!dir)
    return;

  if(vault_dir_has_data(dir))
  {
    config_set_vault_folder(dir);
    config_save();
    repopulate_vault_combo(widgets, NULL);
  }
  else
  {
    vault_prompt_not_found(widgets, dir);  // empty folder -> ask again
  }
  g_free(dir);
}

// Open a folder picker (starting at the user's home dir) so the user can point
// us at their existing vault folder.
static void
vault_choose_folder(AppWidgets *widgets)
{
  GtkFileDialog *dlg = gtk_file_dialog_new();

  gtk_file_dialog_set_title(dlg, "Select Your Vault Folder");

  const char *home = g_get_home_dir();

  if(home)
  {
    GFile *initial = g_file_new_for_path(home);

    gtk_file_dialog_set_initial_folder(dlg, initial);
    g_object_unref(initial);
  }
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(widgets->main_window), NULL,
                                on_vault_choose_folder_ready, widgets);
}

// Create the vault folder `dir` (if needed), seed it with the user's first
// vault ("Main Vault"), remember it, and refresh the combo.
static void
vault_create_in(AppWidgets *widgets, const char *dir)
{
  if(!dir || !dir[0])
    return;

  if(g_mkdir_with_parents(dir, 0755) != 0)
  {
    char *msg = g_strdup_printf(
      "Could not create the vault folder:\n\n%s\n\n"
      "Check that you have permission to write there.", dir);
    GtkAlertDialog *e = gtk_alert_dialog_new("%s", msg);

    gtk_alert_dialog_set_modal(e, TRUE);
    gtk_alert_dialog_show(e, GTK_WINDOW(widgets->main_window));
    g_object_unref(e);
    g_free(msg);
    return;
  }

  // Remember this as the vault folder, then seed it with a starter vault.
  config_set_vault_folder(dir);

  char *vpath = config_vault_file_new("Main Vault");

  if(vpath && !g_file_test(vpath, G_FILE_TEST_EXISTS))
  {
    TQVault *vault = calloc(1, sizeof(TQVault));

    if(vault)
    {
      vault->vault_name = strdup(vpath);
      vault->num_sacks = 12;
      vault->sacks = calloc(12, sizeof(TQVaultSack));

      if(vault_save_json(vault, vpath) != 0)
        fprintf(stderr, "Failed to create initial vault: %s\n", vpath);
      vault_free(vault);
    }
  }
  g_free(vpath);

  config_save();
  repopulate_vault_combo(widgets, "Main Vault");
}

// Alert-dialog response: 2=Create New Vault, 1=Choose a Different Folder,
// 0/-1=Cancel or dismissed.
//   source    - the GtkAlertDialog
//   result    - async result
//   user_data - VaultPromptCtx* (freed here)
static void
on_vault_notfound_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkAlertDialog *dlg = GTK_ALERT_DIALOG(source);
  VaultPromptCtx *ctx = (VaultPromptCtx *)user_data;
  int idx = gtk_alert_dialog_choose_finish(dlg, result, NULL);

  switch(idx)
  {
    case 2:  // Create New Vault
      vault_create_in(ctx->widgets, ctx->dir);
      break;
    case 1:  // Choose a Different Folder…
      vault_choose_folder(ctx->widgets);
      break;
    default: // Cancel / dismissed
      break;
  }
  g_free(ctx->dir);
  g_free(ctx);
}

// Show the "vault folder not found" prompt for the directory `dir`.
//   widgets - app state
//   dir     - directory we looked in (used in the message and as the create
//             target); may be NULL if we couldn't determine one
static void
vault_prompt_not_found(AppWidgets *widgets, const char *dir)
{
  char *msg;

  if(dir && dir[0])
    msg = g_strdup_printf(
      "TQVault could not find any vault files in:\n\n%s\n\n"
      "Would you like to create a new vault there, choose a different "
      "folder, or cancel?", dir);
  else
    msg = g_strdup(
      "TQVault could not find your vault folder.\n\n"
      "Would you like to create one, choose a different folder, or cancel?");

  GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
  const char *buttons[] = { "Cancel", "Choose a Different Folder…",
                            "Create New Vault", NULL };

  gtk_alert_dialog_set_buttons(dlg, buttons);
  gtk_alert_dialog_set_cancel_button(dlg, 0);
  gtk_alert_dialog_set_default_button(dlg, 2);
  gtk_alert_dialog_set_modal(dlg, TRUE);

  VaultPromptCtx *ctx = g_malloc0(sizeof(VaultPromptCtx));

  ctx->widgets = widgets;
  ctx->dir = g_strdup(dir);

  gtk_alert_dialog_choose(dlg, GTK_WINDOW(widgets->main_window), NULL,
                          on_vault_notfound_response, ctx);
  g_object_unref(dlg);
  g_free(msg);
}

// ensure_vault_folder - if the effective vault directory is missing, prompt the
// user to create one or point us at their existing vault data. No-op when the
// directory already exists.
void
ensure_vault_folder(AppWidgets *widgets)
{
  char *dir = config_vault_dir_new();

  if(!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR))
    vault_prompt_not_found(widgets, dir);
  g_free(dir);
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

  // Scan the effective vault directory (a user-chosen vault_folder, or the
  // default {save_folder}/TQVaultData). If it can't be opened we leave the
  // list empty here; ensure_vault_folder() handles prompting the user.
  char *vault_path = config_vault_dir_new();
  GDir *d = vault_path ? g_dir_open(vault_path, 0, NULL) : NULL;

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
        int newcap = vault_cap ? vault_cap * 2 : 16;
        char **grown = realloc(vault_names, (size_t)newcap * sizeof(char *));

        // Keep the names gathered so far on OOM (see the character loop above).
        if(!grown)
          break;

        vault_names = grown;
        vault_cap = newcap;
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
  save_stashes_if_dirty(widgets);
  update_save_button_sensitivity(widgets);
}

// Action: open the curated tq-db-style Database Browser.
//   action, parameter - unused
//   user_data         - AppWidgets*
void
on_db_browser_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
  AppWidgets *widgets = (AppWidgets *)user_data;

  show_db_browser_dialog(widgets);
}

// Action: open the raw Database Explorer (path tree + variable inspector).
//   action, parameter - unused
//   user_data         - AppWidgets*
void
on_db_explorer_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void)action;
  (void)parameter;
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
    ConfirmUnsaved choice = confirm_unsaved_character(widgets);

    if(choice == CONFIRM_SAVE)
      save_character_if_dirty(widgets);
    else if(choice == CONFIRM_CANCEL)
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
    ConfirmUnsaved choice = confirm_unsaved_character(widgets);

    if(choice == CONFIRM_SAVE)
    {
      save_character_if_dirty(widgets);
      save_stashes_if_dirty(widgets);
    }
    else if(choice == CONFIRM_CANCEL)
    {
      // revert combo to current character
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

  char path[1024];

  if(widgets->mobile_folder && strcmp(name, widgets->mobile_folder) == 0)
  {
    // External iOS save: load the char + its page-0 Storage stash straight from
    // the chosen folder, bypassing the SaveData/Main scan. Don't record it as
    // last_character (it isn't a SaveData char). Interim: only 0winsys.dxb is
    // shown/editable; 1/2winsys.dxb are left untouched on disk.
    snprintf(path, sizeof(path), "%s/Player.chr", widgets->mobile_folder);

    char *ps_path = g_build_filename(widgets->mobile_folder, "0winsys.dxb", NULL);

    widgets->player_stash = stash_load(ps_path);
    g_free(ps_path);
  }
  else
  {
    config_set_last_character(name);
    config_save();

    snprintf(path, sizeof(path), "%s/SaveData/Main/%s/Player.chr", global_config.save_folder, name);

    // Load per-character player stash
    char *ps_path = stash_build_path(STASH_PLAYER, name);

    if(ps_path)
    {
      widgets->player_stash = stash_load(ps_path);
      free(ps_path);
    }
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
  char *vp = config_vault_file_new(name);

  snprintf(path, sizeof(path), "%s", vp ? vp : "");
  g_free(vp);

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

  char *verr = NULL;

  widgets->current_vault = vault_load_json_ex(path, &verr);
  if(widgets->current_vault)
  {
    prefetch_for_vault(widgets->current_vault);
  }
  else if(verr)
  {
    // Surface the reason so the user can report what's wrong instead of just
    // seeing blank squares with no explanation.
    char *m = g_strdup_printf(
      "This vault could not be loaded, so its items can't be shown:\n\n%s\n\n"
      "File:\n%s", verr, path);
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", m);

    gtk_alert_dialog_set_modal(dlg, TRUE);
    gtk_alert_dialog_show(dlg, GTK_WINDOW(widgets->main_window));
    g_object_unref(dlg);
    g_free(m);
  }
  free(verr);

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

  int bag_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  // Holding an item: drop it into the clicked bag rather than switching to its
  // view -- but only if the bag has room.  No room (or the sack doesn't exist)
  // falls through to switch the view, keeping the item on the cursor.
  if(widgets->held_item &&
     widgets->current_vault &&
     bag_idx >= 0 && bag_idx < widgets->current_vault->num_sacks &&
     drop_held_into_sack(widgets, &widgets->current_vault->sacks[bag_idx],
                         CONTAINER_VAULT, VAULT_COLS, VAULT_ROWS))
    return;

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
  AppWidgets *widgets = (AppWidgets *)user_data;

  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));

  // Holding an item: drop it into the clicked bag rather than switching to its
  // view -- but only if the bag has room.  Char bag idx 0-2 -> inv_sacks[1-3].
  if(widgets->held_item && widgets->current_character)
  {
    int sidx = 1 + idx;

    if(sidx < widgets->current_character->num_inv_sacks &&
       drop_held_into_sack(widgets,
                           &widgets->current_character->inv_sacks[sidx],
                           CONTAINER_BAG, CHAR_BAG_COLS, CHAR_BAG_ROWS))
      return;
  }

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
