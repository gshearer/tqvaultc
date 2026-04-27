// ui_manage.c -- Vault & character management dialogs
//               (create, duplicate, rename, delete).
//
// Extracted from ui.c for maintainability.

#include "ui.h"
#include "config.h"
#include "vault.h"
#include "character.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib/gstdio.h>

// -- Directory copy helper --------------------------------------------------

// Recursively copy a directory tree from src to dst.
// src: source directory path
// dst: destination directory path (created if needed)
// returns: 0 on success, -1 on failure
static int
copy_directory_recursive(const char *src, const char *dst)
{
  if(!g_file_test(src, G_FILE_TEST_IS_DIR))
    return(-1);

  if(g_mkdir_with_parents(dst, 0755) != 0)
    return(-1);

  GDir *d = g_dir_open(src, 0, NULL);

  if(!d)
    return(-1);

  int ret = 0;
  const gchar *name;

  while((name = g_dir_read_name(d)) != NULL)
  {
    char *src_path = g_build_filename(src, name, NULL);
    char *dst_path = g_build_filename(dst, name, NULL);

    if(g_file_test(src_path, G_FILE_TEST_IS_DIR))
    {
      if(copy_directory_recursive(src_path, dst_path) != 0)
        ret = -1;
    }
    else
    {
      gchar *contents = NULL;
      gsize length = 0;

      if(g_file_get_contents(src_path, &contents, &length, NULL))
      {
        if(!g_file_set_contents(dst_path, contents, (gssize)length, NULL))
          ret = -1;

        g_free(contents);
      }
      else
      {
        ret = -1;
      }
    }

    g_free(src_path);
    g_free(dst_path);

    if(ret != 0)
      break;
  }

  g_dir_close(d);
  return(ret);
}

// -- Recursive directory removal helper -------------------------------------

// Recursively remove a directory and all its contents.
// path: directory to remove
// returns: 0 on success, -1 on failure
static int
remove_directory_recursive(const char *path)
{
  GDir *d = g_dir_open(path, 0, NULL);

  if(!d)
    return(-1);

  const gchar *name;

  while((name = g_dir_read_name(d)) != NULL)
  {
    char *child = g_build_filename(path, name, NULL);

    if(g_file_test(child, G_FILE_TEST_IS_DIR))
      remove_directory_recursive(child);
    else
      g_unlink(child);

    g_free(child);
  }

  g_dir_close(d);
  return(g_rmdir(path));
}

// -- Name validation helper -------------------------------------------------

// Validate a vault or character name for filesystem safety.
// text: name to validate
// returns: error message string if invalid, or NULL if valid
static const char *
validate_name(const char *text)
{
  if(!text || !text[0])
    return("Name cannot be empty.");

  // Reject leading/trailing whitespace
  if(isspace((unsigned char)text[0]) || isspace((unsigned char)text[strlen(text) - 1]))
    return("Name cannot start or end with spaces.");

  // Characters illegal in filenames on Linux and Windows
  static const char illegal[] = "/\\:*?\"<>|";

  for(const char *p = text; *p; p++)
  {
    unsigned char c = (unsigned char)*p;

    if(c < 0x20 || strchr(illegal, *p))
      return("Name contains illegal characters.\n"
             "Avoid: / \\ : * ? \" < > |");
  }

  // Reject . and .. and names that are all dots
  int all_dots = 1;

  for(const char *p = text; *p; p++)
    if(*p != '.')
    {
      all_dots = 0;
      break;
    }

  if(all_dots)
    return("Name cannot be only dots.");

  return(NULL); // valid
}

// Show a modal error dialog with a validation message.
// parent: parent window for transient-for
// msg: error message to display
static void
show_validation_error(GtkWidget *parent, const char *msg)
{
  GtkWidget *err = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(err), "Invalid Name");
  gtk_window_set_transient_for(GTK_WINDOW(err), GTK_WINDOW(parent));
  gtk_window_set_modal(GTK_WINDOW(err), TRUE);

  GtkWidget *lbl = gtk_label_new(msg);

  gtk_widget_set_margin_start(lbl, 20);
  gtk_widget_set_margin_end(lbl, 20);
  gtk_widget_set_margin_top(lbl, 20);
  gtk_widget_set_margin_bottom(lbl, 20);
  gtk_window_set_child(GTK_WINDOW(err), lbl);
  gtk_window_present(GTK_WINDOW(err));
}

// -- Patch myPlayerName in Player.chr ---------------------------------------

// Patch the myPlayerName field in a Player.chr binary file.
// player_chr_path: path to the Player.chr file
// new_name: new character name (ASCII)
// returns: 0 on success, -1 on failure
static int
patch_player_name(const char *player_chr_path, const char *new_name)
{
  FILE *fp = fopen(player_chr_path, "rb");

  if(!fp)
    return(-1);

  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if(file_size <= 0)
  {
    fclose(fp);
    return(-1);
  }

  uint8_t *data = malloc((size_t)file_size);

  if(!data)
  {
    fclose(fp);
    return(-1);
  }

  if(fread(data, 1, (size_t)file_size, fp) != (size_t)file_size)
  {
    free(data);
    fclose(fp);
    return(-1);
  }

  fclose(fp);

  // Find "myPlayerName" key: [4-byte len] [key bytes] [4-byte strlen] [utf16 data]
  const char *needle = "myPlayerName";
  size_t needle_len = strlen(needle);
  ssize_t key_offset = -1;

  for(size_t i = 0; i + 4 + needle_len <= (size_t)file_size; i++)
  {
    uint32_t klen;

    memcpy(&klen, data + i, 4);

    if(klen == (uint32_t)needle_len &&
       i + 4 + klen <= (size_t)file_size &&
       memcmp(data + i + 4, needle, needle_len) == 0)
    {
      key_offset = (ssize_t)i;
      break;
    }
  }

  if(key_offset < 0)
  {
    free(data);
    return(-1);
  }

  // Offset to the value: after key_len(4) + key(needle_len)
  size_t val_offset = (size_t)key_offset + 4 + needle_len;

  if(val_offset + 4 > (size_t)file_size)
  {
    free(data);
    return(-1);
  }

  uint32_t old_str_len;

  memcpy(&old_str_len, data + val_offset, 4);

  size_t old_val_bytes = 4 + (size_t)old_str_len * 2;
  size_t new_name_len = strlen(new_name);
  size_t new_val_bytes = 4 + new_name_len * 2;
  size_t new_file_size = (size_t)file_size - old_val_bytes + new_val_bytes;

  uint8_t *out = malloc(new_file_size);

  if(!out)
  {
    free(data);
    return(-1);
  }

  // Copy everything before the value
  memcpy(out, data, val_offset);

  // Write new string length + UTF-16LE data
  uint32_t new_len32 = (uint32_t)new_name_len;

  memcpy(out + val_offset, &new_len32, 4);

  for(size_t i = 0; i < new_name_len; i++)
  {
    out[val_offset + 4 + i * 2]     = (uint8_t)new_name[i];
    out[val_offset + 4 + i * 2 + 1] = 0;
  }

  // Copy everything after the old value
  size_t after_offset = val_offset + old_val_bytes;

  memcpy(out + val_offset + new_val_bytes, data + after_offset,
         (size_t)file_size - after_offset);

  free(data);

  fp = fopen(player_chr_path, "wb");

  if(!fp)
  {
    free(out);
    return(-1);
  }

  fwrite(out, 1, new_file_size, fp);
  fclose(fp);
  free(out);
  return(0);
}

// -- New vault dialog -------------------------------------------------------

typedef struct {
  AppWidgets *widgets;
  GtkEntry *name_entry;
  GtkWidget *dialog;
} NewVaultWidgets;

// OK button callback for the "New Vault" dialog.
// btn: button widget (unused)
// user_data: NewVaultWidgets pointer
static void
on_new_vault_ok(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  NewVaultWidgets *nw = (NewVaultWidgets *)user_data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(nw->name_entry));
  const char *err = validate_name(text);

  if(err)
  {
    show_validation_error(nw->dialog, err);
    return;
  }

  char filepath[1024];

  snprintf(filepath, sizeof(filepath), "%s/TQVaultData/%s.vault.json",
           global_config.save_folder, text);

  if(g_file_test(filepath, G_FILE_TEST_EXISTS))
  {
    GtkWidget *err = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(err), "Error");
    gtk_window_set_transient_for(GTK_WINDOW(err), GTK_WINDOW(nw->dialog));
    gtk_window_set_modal(GTK_WINDOW(err), TRUE);

    GtkWidget *lbl = gtk_label_new("A vault with that name already exists.");

    gtk_widget_set_margin_start(lbl, 20);
    gtk_widget_set_margin_end(lbl, 20);
    gtk_widget_set_margin_top(lbl, 20);
    gtk_widget_set_margin_bottom(lbl, 20);
    gtk_window_set_child(GTK_WINDOW(err), lbl);
    gtk_window_present(GTK_WINDOW(err));
    return;
  }

  save_vault_if_dirty(nw->widgets);

  // Create empty vault with 12 sacks
  TQVault *vault = calloc(1, sizeof(TQVault));

  if(!vault)
  {
    gtk_window_destroy(GTK_WINDOW(nw->dialog));
    return;
  }

  vault->vault_name = strdup(filepath);
  vault->num_sacks = 12;
  vault->sacks = calloc(12, sizeof(TQVaultSack));

  if(vault_save_json(vault, filepath) != 0)
  {
    fprintf(stderr, "Failed to create vault: %s\n", filepath);
    vault_free(vault);
    gtk_window_destroy(GTK_WINDOW(nw->dialog));
    return;
  }

  vault_free(vault);

  // Combo entry is the bare name (repopulate strips .vault.json)
  char combo_name[256];

  snprintf(combo_name, sizeof(combo_name), "%s", text);

  gtk_window_destroy(GTK_WINDOW(nw->dialog));
  repopulate_vault_combo(nw->widgets, combo_name);
}

// Cancel button callback for the "New Vault" dialog.
// btn: button widget (unused)
// user_data: NewVaultWidgets pointer
static void
on_new_vault_cancel(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  NewVaultWidgets *nw = (NewVaultWidgets *)user_data;

  gtk_window_destroy(GTK_WINDOW(nw->dialog));
}

// Show the "New Vault" dialog.
// btn: button widget (unused)
// user_data: AppWidgets pointer
static void
on_new_vault_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!global_config.save_folder)
    return;

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "New Vault");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("New vault name:"));

  GtkWidget *entry = gtk_entry_new();

  gtk_box_append(GTK_BOX(vbox), entry);

  NewVaultWidgets *nw = g_malloc(sizeof(NewVaultWidgets));

  nw->widgets = widgets;
  nw->name_entry = GTK_ENTRY(entry);
  nw->dialog = dialog;

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 10);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_new_vault_cancel), nw);
  gtk_box_append(GTK_BOX(btn_box), cancel_btn);

  GtkWidget *ok_btn = gtk_button_new_with_label("OK");

  g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_new_vault_ok), nw);
  gtk_box_append(GTK_BOX(btn_box), ok_btn);

  g_object_set_data_full(G_OBJECT(dialog), "new-vault-widgets", nw, g_free);
  gtk_window_present(GTK_WINDOW(dialog));
}

// -- Duplicate character dialog ---------------------------------------------

typedef struct {
  AppWidgets *widgets;
  GtkEntry *name_entry;
  GtkWidget *dialog;
} DuplicateWidgets;

// OK button callback for the "Duplicate Character" dialog.
// btn: button widget (unused)
// user_data: DuplicateWidgets pointer
static void
on_duplicate_ok(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DuplicateWidgets *dw = (DuplicateWidgets *)user_data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(dw->name_entry));

  // Validate the name portion (skip leading _ if user typed one)
  const char *name_part = (text && text[0] == '_') ? text + 1 : text;
  const char *err = validate_name(name_part);

  if(err)
  {
    show_validation_error(dw->dialog, err);
    return;
  }

  char new_dir[256];

  if(text[0] == '_')
    snprintf(new_dir, sizeof(new_dir), "%s", text);
  else
    snprintf(new_dir, sizeof(new_dir), "_%s", text);

  char target[1024];

  snprintf(target, sizeof(target), "%s/SaveData/Main/%s",
           global_config.save_folder, new_dir);

  if(g_file_test(target, G_FILE_TEST_EXISTS))
  {
    // Target already exists
    GtkWidget *err = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(err), "Error");
    gtk_window_set_transient_for(GTK_WINDOW(err), GTK_WINDOW(dw->dialog));
    gtk_window_set_modal(GTK_WINDOW(err), TRUE);

    GtkWidget *lbl = gtk_label_new("A character with that name already exists.");

    gtk_widget_set_margin_start(lbl, 20);
    gtk_widget_set_margin_end(lbl, 20);
    gtk_widget_set_margin_top(lbl, 20);
    gtk_widget_set_margin_bottom(lbl, 20);
    gtk_window_set_child(GTK_WINDOW(err), lbl);
    gtk_window_present(GTK_WINDOW(err));
    return;
  }

  save_character_if_dirty(dw->widgets);

  // Get current character directory
  char *cur = dropdown_get_selected_text(dw->widgets->character_combo);

  if(!cur)
    return;

  char source[1024];

  snprintf(source, sizeof(source), "%s/SaveData/Main/%s",
           global_config.save_folder, cur);
  g_free(cur);

  if(copy_directory_recursive(source, target) != 0)
  {
    fprintf(stderr, "Failed to copy character directory\n");
    gtk_window_destroy(GTK_WINDOW(dw->dialog));
    return;
  }

  // Patch myPlayerName in the copied Player.chr so the game recognises it
  char chr_path[1100];

  snprintf(chr_path, sizeof(chr_path), "%s/Player.chr", target);

  const char *display_name = (new_dir[0] == '_') ? new_dir + 1 : new_dir;

  if(patch_player_name(chr_path, display_name) != 0)
    fprintf(stderr, "Warning: failed to patch myPlayerName in %s\n", chr_path);

  gtk_window_destroy(GTK_WINDOW(dw->dialog));
  repopulate_character_combo(dw->widgets, new_dir);
}

// Cancel button callback for the "Duplicate Character" dialog.
// btn: button widget (unused)
// user_data: DuplicateWidgets pointer
static void
on_duplicate_cancel(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DuplicateWidgets *dw = (DuplicateWidgets *)user_data;

  gtk_window_destroy(GTK_WINDOW(dw->dialog));
}

// Show the "Duplicate Character" dialog.
// btn: button widget (unused)
// user_data: AppWidgets pointer
static void
on_duplicate_btn_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!global_config.save_folder)
    return;

  char *cur = dropdown_get_selected_text(widgets->character_combo);

  if(!cur)
    return;

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Duplicate Character");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("New character name:"));

  GtkWidget *entry = gtk_entry_new();

  // Pre-fill with current name stripped of _ prefix
  const char *prefill = (cur[0] == '_') ? cur + 1 : cur;

  gtk_editable_set_text(GTK_EDITABLE(entry), prefill);
  gtk_box_append(GTK_BOX(vbox), entry);

  DuplicateWidgets *dw = g_malloc(sizeof(DuplicateWidgets));

  dw->widgets = widgets;
  dw->name_entry = GTK_ENTRY(entry);
  dw->dialog = dialog;

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 10);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_duplicate_cancel), dw);
  gtk_box_append(GTK_BOX(btn_box), cancel_btn);

  GtkWidget *ok_btn = gtk_button_new_with_label("OK");

  g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_duplicate_ok), dw);
  gtk_box_append(GTK_BOX(btn_box), ok_btn);

  g_object_set_data_full(G_OBJECT(dialog), "dup-widgets", dw, g_free);
  g_free(cur);
  gtk_window_present(GTK_WINDOW(dialog));
}

// -- Duplicate vault dialog -------------------------------------------------

// OK button callback for the "Duplicate Vault" dialog.
// btn: button widget (unused)
// user_data: NewVaultWidgets pointer
static void
on_dup_vault_ok(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  NewVaultWidgets *nw = (NewVaultWidgets *)user_data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(nw->name_entry));
  const char *err = validate_name(text);

  if(err)
  {
    show_validation_error(nw->dialog, err);
    return;
  }

  char filepath[1024];

  snprintf(filepath, sizeof(filepath), "%s/TQVaultData/%s.vault.json",
           global_config.save_folder, text);

  if(g_file_test(filepath, G_FILE_TEST_EXISTS))
  {
    show_validation_error(nw->dialog, "A vault with that name already exists.");
    return;
  }

  // Get current vault name
  char *cur = dropdown_get_selected_text(nw->widgets->vault_combo);

  if(!cur)
  {
    gtk_window_destroy(GTK_WINDOW(nw->dialog));
    return;
  }

  save_vault_if_dirty(nw->widgets);

  char source[1024];

  snprintf(source, sizeof(source), "%s/TQVaultData/%s.vault.json",
           global_config.save_folder, cur);
  g_free(cur);

  // Read source file and write copy
  FILE *fin = fopen(source, "rb");

  if(!fin)
  {
    show_validation_error(nw->dialog, "Failed to read current vault.");
    return;
  }

  fseek(fin, 0, SEEK_END);
  long sz = ftell(fin);
  fseek(fin, 0, SEEK_SET);

  char *buf = malloc((size_t)sz);

  if(!buf)
  {
    fclose(fin);
    show_validation_error(nw->dialog, "Failed to read current vault.");
    return;
  }

  if(fread(buf, 1, (size_t)sz, fin) != (size_t)sz)
  {
    free(buf);
    fclose(fin);
    show_validation_error(nw->dialog, "Failed to read current vault.");
    return;
  }

  fclose(fin);

  FILE *fout = fopen(filepath, "wb");

  if(!fout)
  {
    free(buf);
    show_validation_error(nw->dialog, "Failed to create vault file.");
    return;
  }

  fwrite(buf, 1, (size_t)sz, fout);
  fclose(fout);
  free(buf);

  gtk_window_destroy(GTK_WINDOW(nw->dialog));
  repopulate_vault_combo(nw->widgets, text);
}

// Cancel button callback for the "Duplicate Vault" dialog.
// btn: button widget (unused)
// user_data: NewVaultWidgets pointer
static void
on_dup_vault_cancel(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  NewVaultWidgets *nw = (NewVaultWidgets *)user_data;

  gtk_window_destroy(GTK_WINDOW(nw->dialog));
}

// Action handler to show the "Duplicate Vault" dialog.
// action: GSimpleAction (unused)
// param: action parameter (unused)
// user_data: AppWidgets pointer
static void
on_dup_vault_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void)action;
  (void)param;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!global_config.save_folder)
    return;

  char *cur = dropdown_get_selected_text(widgets->vault_combo);

  if(!cur)
    return;

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Duplicate Vault");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("New vault name:"));

  GtkWidget *entry = gtk_entry_new();

  gtk_editable_set_text(GTK_EDITABLE(entry), cur);
  gtk_box_append(GTK_BOX(vbox), entry);

  NewVaultWidgets *nw = g_malloc(sizeof(NewVaultWidgets));

  nw->widgets = widgets;
  nw->name_entry = GTK_ENTRY(entry);
  nw->dialog = dialog;

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 10);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_dup_vault_cancel), nw);
  gtk_box_append(GTK_BOX(btn_box), cancel_btn);

  GtkWidget *ok_btn = gtk_button_new_with_label("OK");

  g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_dup_vault_ok), nw);
  gtk_box_append(GTK_BOX(btn_box), ok_btn);

  g_object_set_data_full(G_OBJECT(dialog), "dup-vault-widgets", nw, g_free);
  g_free(cur);
  gtk_window_present(GTK_WINDOW(dialog));
}

// -- Rename vault dialog ----------------------------------------------------

// OK button callback for the "Rename Vault" dialog.
// btn: button widget (unused)
// user_data: NewVaultWidgets pointer
static void
on_rename_vault_ok(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  NewVaultWidgets *nw = (NewVaultWidgets *)user_data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(nw->name_entry));
  const char *err = validate_name(text);

  if(err)
  {
    show_validation_error(nw->dialog, err);
    return;
  }

  char *cur = dropdown_get_selected_text(nw->widgets->vault_combo);

  if(!cur)
  {
    gtk_window_destroy(GTK_WINDOW(nw->dialog));
    return;
  }

  if(strcmp(cur, text) == 0)
  {
    g_free(cur);
    gtk_window_destroy(GTK_WINDOW(nw->dialog));
    return;
  }

  char newpath[1024], oldpath[1024];

  snprintf(newpath, sizeof(newpath), "%s/TQVaultData/%s.vault.json",
           global_config.save_folder, text);
  snprintf(oldpath, sizeof(oldpath), "%s/TQVaultData/%s.vault.json",
           global_config.save_folder, cur);
  g_free(cur);

  if(g_file_test(newpath, G_FILE_TEST_EXISTS))
  {
    show_validation_error(nw->dialog, "A vault with that name already exists.");
    return;
  }

  save_vault_if_dirty(nw->widgets);

  if(rename(oldpath, newpath) != 0)
  {
    show_validation_error(nw->dialog, "Failed to rename vault file.");
    return;
  }

  gtk_window_destroy(GTK_WINDOW(nw->dialog));
  repopulate_vault_combo(nw->widgets, text);
}

// Cancel button callback for the "Rename Vault" dialog.
// btn: button widget (unused)
// user_data: NewVaultWidgets pointer
static void
on_rename_vault_cancel(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  NewVaultWidgets *nw = (NewVaultWidgets *)user_data;

  gtk_window_destroy(GTK_WINDOW(nw->dialog));
}

// Action handler to show the "Rename Vault" dialog.
// action: GSimpleAction (unused)
// param: action parameter (unused)
// user_data: AppWidgets pointer
static void
on_rename_vault_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void)action;
  (void)param;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!global_config.save_folder)
    return;

  char *cur = dropdown_get_selected_text(widgets->vault_combo);

  if(!cur)
    return;

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Rename Vault");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("New vault name:"));

  GtkWidget *entry = gtk_entry_new();

  gtk_editable_set_text(GTK_EDITABLE(entry), cur);
  gtk_box_append(GTK_BOX(vbox), entry);

  NewVaultWidgets *nw = g_malloc(sizeof(NewVaultWidgets));

  nw->widgets = widgets;
  nw->name_entry = GTK_ENTRY(entry);
  nw->dialog = dialog;

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 10);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_rename_vault_cancel), nw);
  gtk_box_append(GTK_BOX(btn_box), cancel_btn);

  GtkWidget *ok_btn = gtk_button_new_with_label("OK");

  g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_rename_vault_ok), nw);
  gtk_box_append(GTK_BOX(btn_box), ok_btn);

  g_object_set_data_full(G_OBJECT(dialog), "rename-vault-widgets", nw, g_free);
  g_free(cur);
  gtk_window_present(GTK_WINDOW(dialog));
}

// -- Delete vault confirmation ----------------------------------------------

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  char *vault_name;
} DeleteVaultWidgets;

// Yes/Delete button callback for the "Delete Vault" confirmation dialog.
// btn: button widget (unused)
// user_data: DeleteVaultWidgets pointer
static void
on_delete_vault_yes(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DeleteVaultWidgets *dvw = (DeleteVaultWidgets *)user_data;

  save_vault_if_dirty(dvw->widgets);

  char filepath[1024];

  snprintf(filepath, sizeof(filepath), "%s/TQVaultData/%s.vault.json",
           global_config.save_folder, dvw->vault_name);

  if(g_unlink(filepath) != 0)
    fprintf(stderr, "Failed to delete vault: %s\n", filepath);

  gtk_window_destroy(GTK_WINDOW(dvw->dialog));

  // Clear current vault so we don't write it back on combo change
  if(dvw->widgets->current_vault)
  {
    vault_free(dvw->widgets->current_vault);
    dvw->widgets->current_vault = NULL;
  }

  repopulate_vault_combo(dvw->widgets, NULL);
}

// Cancel button callback for the "Delete Vault" confirmation dialog.
// btn: button widget (unused)
// user_data: DeleteVaultWidgets pointer
static void
on_delete_vault_no(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DeleteVaultWidgets *dvw = (DeleteVaultWidgets *)user_data;

  gtk_window_destroy(GTK_WINDOW(dvw->dialog));
}

// Action handler to show the "Delete Vault" confirmation dialog.
// action: GSimpleAction (unused)
// param: action parameter (unused)
// user_data: AppWidgets pointer
static void
on_delete_vault_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void)action;
  (void)param;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!global_config.save_folder)
    return;

  char *cur = dropdown_get_selected_text(widgets->vault_combo);

  if(!cur)
    return;

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Delete Vault");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  char msg[512];

  snprintf(msg, sizeof(msg), "Delete vault \"%s\"?\nThis cannot be undone.", cur);

  GtkWidget *lbl = gtk_label_new(msg);

  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_box_append(GTK_BOX(vbox), lbl);

  DeleteVaultWidgets *dvw = g_malloc(sizeof(DeleteVaultWidgets));

  dvw->widgets = widgets;
  dvw->dialog = dialog;
  dvw->vault_name = g_strdup(cur);

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 10);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *no_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(no_btn, "clicked", G_CALLBACK(on_delete_vault_no), dvw);
  gtk_box_append(GTK_BOX(btn_box), no_btn);

  GtkWidget *yes_btn = gtk_button_new_with_label("Delete");

  g_signal_connect(yes_btn, "clicked", G_CALLBACK(on_delete_vault_yes), dvw);
  gtk_box_append(GTK_BOX(btn_box), yes_btn);

  g_object_set_data_full(G_OBJECT(dialog), "del-vault-widgets", dvw,
                         (GDestroyNotify)g_free);
  g_object_set_data_full(G_OBJECT(dialog), "del-vault-name", dvw->vault_name, g_free);
  g_free(cur);
  gtk_window_present(GTK_WINDOW(dialog));
}

// -- Create new vault (action wrapper) --------------------------------------

// Action handler wrapping on_new_vault_btn_clicked.
// action: GSimpleAction (unused)
// param: action parameter (unused)
// user_data: AppWidgets pointer
static void
on_new_vault_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void)action;
  (void)param;
  on_new_vault_btn_clicked(NULL, user_data);
}

// -- Duplicate character (action wrapper) -----------------------------------

// Action handler wrapping on_duplicate_btn_clicked.
// action: GSimpleAction (unused)
// param: action parameter (unused)
// user_data: AppWidgets pointer
static void
on_dup_char_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void)action;
  (void)param;
  on_duplicate_btn_clicked(NULL, user_data);
}

// -- Rename character dialog ------------------------------------------------

// OK button callback for the "Rename Character" dialog.
// btn: button widget (unused)
// user_data: DuplicateWidgets pointer
static void
on_rename_char_ok(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DuplicateWidgets *dw = (DuplicateWidgets *)user_data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(dw->name_entry));
  const char *name_part = (text && text[0] == '_') ? text + 1 : text;
  const char *err = validate_name(name_part);

  if(err)
  {
    show_validation_error(dw->dialog, err);
    return;
  }

  char new_dir[256];

  if(text[0] == '_')
    snprintf(new_dir, sizeof(new_dir), "%s", text);
  else
    snprintf(new_dir, sizeof(new_dir), "_%s", text);

  char *cur = dropdown_get_selected_text(dw->widgets->character_combo);

  if(!cur)
  {
    gtk_window_destroy(GTK_WINDOW(dw->dialog));
    return;
  }

  if(strcmp(cur, new_dir) == 0)
  {
    g_free(cur);
    gtk_window_destroy(GTK_WINDOW(dw->dialog));
    return;
  }

  char target[1024], source[1024];

  snprintf(target, sizeof(target), "%s/SaveData/Main/%s",
           global_config.save_folder, new_dir);
  snprintf(source, sizeof(source), "%s/SaveData/Main/%s",
           global_config.save_folder, cur);
  g_free(cur);

  if(g_file_test(target, G_FILE_TEST_EXISTS))
  {
    show_validation_error(dw->dialog, "A character with that name already exists.");
    return;
  }

  save_character_if_dirty(dw->widgets);

  if(rename(source, target) != 0)
  {
    show_validation_error(dw->dialog, "Failed to rename character directory.");
    return;
  }

  // Patch Player.chr with the new name
  char chr_path[1100];

  snprintf(chr_path, sizeof(chr_path), "%s/Player.chr", target);

  const char *display_name = (new_dir[0] == '_') ? new_dir + 1 : new_dir;

  if(patch_player_name(chr_path, display_name) != 0)
    fprintf(stderr, "Warning: failed to patch myPlayerName in %s\n", chr_path);

  gtk_window_destroy(GTK_WINDOW(dw->dialog));
  repopulate_character_combo(dw->widgets, new_dir);
}

// Cancel button callback for the "Rename Character" dialog.
// btn: button widget (unused)
// user_data: DuplicateWidgets pointer
static void
on_rename_char_cancel(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DuplicateWidgets *dw = (DuplicateWidgets *)user_data;

  gtk_window_destroy(GTK_WINDOW(dw->dialog));
}

// Action handler to show the "Rename Character" dialog.
// action: GSimpleAction (unused)
// param: action parameter (unused)
// user_data: AppWidgets pointer
static void
on_rename_char_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void)action;
  (void)param;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!global_config.save_folder)
    return;

  char *cur = dropdown_get_selected_text(widgets->character_combo);

  if(!cur)
    return;

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Rename Character");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("New character name:"));

  GtkWidget *entry = gtk_entry_new();
  const char *prefill = (cur[0] == '_') ? cur + 1 : cur;

  gtk_editable_set_text(GTK_EDITABLE(entry), prefill);
  gtk_box_append(GTK_BOX(vbox), entry);

  DuplicateWidgets *dw = g_malloc(sizeof(DuplicateWidgets));

  dw->widgets = widgets;
  dw->name_entry = GTK_ENTRY(entry);
  dw->dialog = dialog;

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 10);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_rename_char_cancel), dw);
  gtk_box_append(GTK_BOX(btn_box), cancel_btn);

  GtkWidget *ok_btn = gtk_button_new_with_label("OK");

  g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_rename_char_ok), dw);
  gtk_box_append(GTK_BOX(btn_box), ok_btn);

  g_object_set_data_full(G_OBJECT(dialog), "rename-char-widgets", dw, g_free);
  g_free(cur);
  gtk_window_present(GTK_WINDOW(dialog));
}

// -- Delete character confirmation ------------------------------------------

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  char *char_dir_name;
} DeleteCharWidgets;

// Yes/Delete button callback for the "Delete Character" confirmation dialog.
// btn: button widget (unused)
// user_data: DeleteCharWidgets pointer
static void
on_delete_char_yes(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DeleteCharWidgets *dcw = (DeleteCharWidgets *)user_data;

  save_character_if_dirty(dcw->widgets);

  char dirpath[1024];

  snprintf(dirpath, sizeof(dirpath), "%s/SaveData/Main/%s",
           global_config.save_folder, dcw->char_dir_name);

  if(remove_directory_recursive(dirpath) != 0)
    fprintf(stderr, "Failed to delete character directory: %s\n", dirpath);

  gtk_window_destroy(GTK_WINDOW(dcw->dialog));

  // Clear current character state
  if(dcw->widgets->current_character)
  {
    character_free(dcw->widgets->current_character);
    dcw->widgets->current_character = NULL;
  }

  repopulate_character_combo(dcw->widgets, NULL);
}

// Cancel button callback for the "Delete Character" confirmation dialog.
// btn: button widget (unused)
// user_data: DeleteCharWidgets pointer
static void
on_delete_char_no(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  DeleteCharWidgets *dcw = (DeleteCharWidgets *)user_data;

  gtk_window_destroy(GTK_WINDOW(dcw->dialog));
}

// Action handler to show the "Delete Character" confirmation dialog.
// action: GSimpleAction (unused)
// param: action parameter (unused)
// user_data: AppWidgets pointer
static void
on_delete_char_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void)action;
  (void)param;
  AppWidgets *widgets = (AppWidgets *)user_data;

  if(!global_config.save_folder)
    return;

  char *cur = dropdown_get_selected_text(widgets->character_combo);

  if(!cur)
    return;

  GtkWidget *dialog = gtk_window_new();

  gtk_window_set_title(GTK_WINDOW(dialog), "Delete Character");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  const char *display = (cur[0] == '_') ? cur + 1 : cur;
  char msg[512];

  snprintf(msg, sizeof(msg),
           "Delete character \"%s\"?\nThis will remove all save data and cannot be undone.",
           display);

  GtkWidget *lbl = gtk_label_new(msg);

  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_box_append(GTK_BOX(vbox), lbl);

  DeleteCharWidgets *dcw = g_malloc(sizeof(DeleteCharWidgets));

  dcw->widgets = widgets;
  dcw->dialog = dialog;
  dcw->char_dir_name = g_strdup(cur);

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 10);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *no_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(no_btn, "clicked", G_CALLBACK(on_delete_char_no), dcw);
  gtk_box_append(GTK_BOX(btn_box), no_btn);

  GtkWidget *yes_btn = gtk_button_new_with_label("Delete");

  g_signal_connect(yes_btn, "clicked", G_CALLBACK(on_delete_char_yes), dcw);
  gtk_box_append(GTK_BOX(btn_box), yes_btn);

  g_object_set_data_full(G_OBJECT(dialog), "del-char-widgets", dcw,
                         (GDestroyNotify)g_free);
  g_object_set_data_full(G_OBJECT(dialog), "del-char-name", dcw->char_dir_name, g_free);
  g_free(cur);
  gtk_window_present(GTK_WINDOW(dialog));
}

// -- Action registration ----------------------------------------------------

// Register all vault/character management actions on the given window.
// window: GTK window to attach actions to
// widgets: app context passed as user_data to callbacks
void
register_manage_actions(GtkWindow *window, AppWidgets *widgets)
{
  struct { const char *name; GCallback cb; } acts[] = {
    { "dup-vault",    G_CALLBACK(on_dup_vault_action) },
    { "rename-vault", G_CALLBACK(on_rename_vault_action) },
    { "delete-vault", G_CALLBACK(on_delete_vault_action) },
    { "new-vault",    G_CALLBACK(on_new_vault_action) },
    { "dup-char",     G_CALLBACK(on_dup_char_action) },
    { "rename-char",  G_CALLBACK(on_rename_char_action) },
    { "delete-char",  G_CALLBACK(on_delete_char_action) },
  };

  for(size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++)
  {
    GSimpleAction *a = g_simple_action_new(acts[i].name, NULL);

    g_signal_connect(a, "activate", acts[i].cb, widgets);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(a));
    g_object_unref(a);
  }
}
