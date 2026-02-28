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
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

GdkPixbuf* load_item_texture(AppWidgets *widgets, const char *base_name, uint32_t var1) {
    if (!base_name) return NULL;
    if (!global_config.game_folder) return NULL;

    /* Cache key includes shard count so incomplete and complete relics/charms
     * get different textures even though they share the same base_name. */
    char cache_key[1200];
    snprintf(cache_key, sizeof(cache_key), "%s:%u", base_name, var1);

    GdkPixbuf *cached = g_hash_table_lookup(widgets->texture_cache, cache_key);
    if (cached) return g_object_ref(cached);

    char *bitmap_path = NULL;
    TQArzRecordData *data = asset_get_dbr(base_name);
    if (data) {
        bitmap_path = arz_record_get_string(data, "bitmap", NULL);
        if (!bitmap_path) bitmap_path = arz_record_get_string(data, "artifactBitmap", NULL);
        if (!bitmap_path) {
            /* For relics/charms: use shardBitmap when incomplete, relicBitmap when complete.
             * completedRelicLevel from the DBR tells us how many shards are needed. */
            char *relic_bmp = arz_record_get_string(data, "relicBitmap", NULL);
            char *shard_bmp = arz_record_get_string(data, "shardBitmap", NULL);
            if (relic_bmp && shard_bmp) {
                int max_shards = arz_record_get_int(data, "completedRelicLevel", 0, NULL);
                if (max_shards > 0 && var1 < (uint32_t)max_shards) {
                    bitmap_path = shard_bmp;
                    free(relic_bmp);
                } else {
                    bitmap_path = relic_bmp;
                    free(shard_bmp);
                }
            } else if (relic_bmp) {
                bitmap_path = relic_bmp;
            } else if (shard_bmp) {
                bitmap_path = shard_bmp;
            }
        }
    }

    char tex_path[1024];
    const char *source = bitmap_path ? bitmap_path : base_name;
    strncpy(tex_path, source, sizeof(tex_path));
    tex_path[sizeof(tex_path)-1] = '\0';
    if (bitmap_path) free(bitmap_path);

    char *dot = strrchr(tex_path, '.');
    if (dot) strcpy(dot, ".tex");
    else strcat(tex_path, ".tex");

    GdkPixbuf *pixbuf = texture_load(tex_path);
    if (pixbuf) {
        g_hash_table_insert(widgets->texture_cache, strdup(cache_key), g_object_ref(pixbuf));
    }
    return pixbuf;
}

/* Returns true if base_name refers to a standalone relic or charm item. */
bool item_is_relic_or_charm(const char *base_name) {
    if (!base_name) return false;
    /* Case-insensitive substring checks matching the C# reference logic */
    for (const char *p = base_name; *p; p++) {
        if (strncasecmp(p, "animalrelics", 12) == 0) return true;
        if (strncasecmp(p, "\\relics\\", 8) == 0) return true;
        if (strncasecmp(p, "\\charms\\", 8) == 0) return true;
    }
    return false;
}

char* safe_strdup(const char *s) { return s ? strdup(s) : NULL; }

/* ── GtkDropDown helpers (replacing deprecated GtkComboBoxText) ───────── */

char *dropdown_get_selected_text(GtkWidget *dd) {
    GtkStringObject *obj = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dd));
    if (!obj) return NULL;
    return g_strdup(gtk_string_object_get_string(obj));
}

guint dropdown_select_by_name(GtkWidget *dd, const char *name) {
    GtkStringList *sl = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(dd)));
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));
    for (guint i = 0; i < n; i++) {
        if (strcmp(gtk_string_list_get_string(sl, i), name) == 0) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i);
            return i;
        }
    }
    return GTK_INVALID_LIST_POSITION;
}

bool item_is_stackable_type(const TQVaultItem *a) {
    if (!a || !a->base_name) return false;
    if (a->prefix_name && a->prefix_name[0]) return false;
    if (a->suffix_name && a->suffix_name[0]) return false;
    if (a->relic_name && a->relic_name[0])   return false;
    if (a->relic_name2 && a->relic_name2[0]) return false;
    /* Only relics, charms, potions, and scrolls are stackable */
    const char *b = a->base_name;
    if (strcasestr(b, "\\relics\\"))    return true;
    if (strcasestr(b, "\\charms\\"))    return true;
    if (strcasestr(b, "\\animalrelic")) return true;
    if (strcasestr(b, "\\oneshot\\"))   return true;
    if (strcasestr(b, "\\scrolls\\"))   return true;
    return false;
}

/* Returns true if path refers to an artifact (but not an arcane formula). */
bool item_is_artifact(const char *base_name) {
    if (!base_name) return false;
    if (strcasestr(base_name, "\\artifacts\\") == NULL) return false;
    if (strcasestr(base_name, "\\arcaneformulae\\") != NULL) return false;
    return true;
}

/* Returns true if the suffix path grants a second relic/charm socket slot. */
bool item_has_two_relic_slots(const char *suffix_name) {
    if (!suffix_name || !suffix_name[0]) return false;
    return strcasestr(suffix_name, "RARE_EXTRARELIC_01.DBR") != NULL;
}

/* Helper: look up a string variable from a DBR record (internal pointer, don't free). */
const char* dbr_get_string(const char *record_path, const char *var_name) {
    if (!record_path || !record_path[0]) return NULL;
    TQArzRecordData *data = asset_get_dbr(record_path);
    if (!data) return NULL;
    const char *interned = arz_intern(var_name);
    TQVariable *v = arz_record_get_var(data, interned);
    if (v && v->type == TQ_VAR_STRING && v->count > 0)
        return v->value.str[0];
    return NULL;
}

/* Save vault if dirty */
void save_vault_if_dirty(AppWidgets *widgets) {
    if (widgets->vault_dirty && widgets->current_vault && widgets->current_vault->vault_name) {
        vault_save_json(widgets->current_vault, widgets->current_vault->vault_name);
        widgets->vault_dirty = false;
        if (tqvc_debug)
            printf("vault saved: %s\n", widgets->current_vault->vault_name);
    }
}

/* Update Save Character button sensitivity based on char_dirty */
void update_save_button_sensitivity(AppWidgets *widgets) {
    if (widgets->save_char_btn)
        gtk_widget_set_sensitive(widgets->save_char_btn, widgets->char_dirty);
}

/* Save character if dirty */
void save_character_if_dirty(AppWidgets *widgets) {
    if (widgets->char_dirty && widgets->current_character && widgets->current_character->filepath) {
        if (character_save(widgets->current_character, widgets->current_character->filepath) == 0) {
            widgets->char_dirty = false;
            update_save_button_sensitivity(widgets);
            if (tqvc_debug)
                printf("character saved: %s\n", widgets->current_character->filepath);
        } else {
            fprintf(stderr, "character save failed: %s\n", widgets->current_character->filepath);
        }
    }
}

/* Get item dimensions (texture-based if available, else from struct fields) */
void get_item_dims(AppWidgets *widgets, TQVaultItem *item, int *w, int *h) {
    GdkPixbuf *pixbuf = load_item_texture(widgets, item->base_name, item->var1);
    if (pixbuf) {
        *w = gdk_pixbuf_get_width(pixbuf) / 32;
        *h = gdk_pixbuf_get_height(pixbuf) / 32;
        if (*w < 1) *w = 1;
        if (*h < 1) *h = 1;
        g_object_unref(pixbuf);
    } else {
        *w = item->width  > 0 ? item->width  : 1;
        *h = item->height > 0 ? item->height : 1;
    }
}

/* Strip Pango/HTML markup tags from a string, producing plain text.
 * Decodes common XML entities (&amp; &lt; &gt; &apos; &quot;).
 * dst must be at least as large as src. */
static void strip_pango_markup(char *dst, size_t dst_size, const char *src) {
    size_t di = 0;
    bool in_tag = false;
    for (const char *p = src; *p && di + 1 < dst_size; p++) {
        if (*p == '<') { in_tag = true; continue; }
        if (*p == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (*p == '&') {
            if (strncmp(p, "&amp;", 5) == 0)       { dst[di++] = '&'; p += 4; }
            else if (strncmp(p, "&lt;", 4) == 0)    { dst[di++] = '<'; p += 3; }
            else if (strncmp(p, "&gt;", 4) == 0)    { dst[di++] = '>'; p += 3; }
            else if (strncmp(p, "&apos;", 6) == 0)  { dst[di++] = '\''; p += 5; }
            else if (strncmp(p, "&quot;", 6) == 0)   { dst[di++] = '"'; p += 5; }
            else dst[di++] = *p;
        } else {
            dst[di++] = *p;
        }
    }
    dst[di] = '\0';
}

/* Invalidate all tooltip caches */
void invalidate_tooltips(AppWidgets *widgets) {
    widgets->last_tooltip_item     = NULL;
    widgets->last_inv_tooltip_item = NULL;
    widgets->last_bag_tooltip_item = NULL;
    widgets->last_equip_tooltip_slot = -1;
    if (widgets->tooltip_popover)
        gtk_widget_set_visible(widgets->tooltip_popover, FALSE);
}

void queue_redraw_equip(AppWidgets *widgets) {
    gtk_widget_queue_draw(widgets->vault_drawing_area);
    gtk_widget_queue_draw(widgets->inv_drawing_area);
    gtk_widget_queue_draw(widgets->bag_drawing_area);
    gtk_widget_queue_draw(widgets->equip_drawing_area);
    update_resist_damage_tables(widgets, widgets->current_character);
}

void queue_redraw_all(AppWidgets *widgets) {
    gtk_widget_queue_draw(widgets->vault_drawing_area);
    gtk_widget_queue_draw(widgets->inv_drawing_area);
    gtk_widget_queue_draw(widgets->bag_drawing_area);
    gtk_widget_queue_draw(widgets->equip_drawing_area);
}

/* ── Search logic ────────────────────────────────────────────────────────── */

/* Match search text against the full tooltip (name + stats + everything).
 * This matches TQVaultAE behavior: searching "Lightning" will find items
 * with Lightning stats even if the word isn't in the item name. */
bool item_matches_search(AppWidgets *widgets, TQVaultItem *item) {
    if (!widgets->search_text[0] || !item || !item->base_name) return false;
    char markup[16384];
    char plain[16384];
    markup[0] = '\0';
    vault_item_format_stats(item, widgets->translations, markup, sizeof(markup));
    strip_pango_markup(plain, sizeof(plain), markup);
    for (char *p = plain; *p; p++) *p = (char)tolower((unsigned char)*p);
    bool match = strstr(plain, widgets->search_text) != NULL;
    if (tqvc_debug && match) {
        printf("SEARCH MATCH [%s] in '%s'\n", widgets->search_text, item->base_name);
    }
    return match;
}

/* Check if any item in a sack matches the current search text. */
static bool sack_has_match(AppWidgets *widgets, TQVaultSack *sack) {
    if (!sack || !widgets->search_text[0]) return false;
    for (int i = 0; i < sack->num_items; i++) {
        if (item_matches_search(widgets, &sack->items[i])) return true;
    }
    return false;
}

static void run_search(AppWidgets *widgets) {
    /* Evaluate vault sacks */
    memset(widgets->vault_sack_match, 0, sizeof(widgets->vault_sack_match));
    if (widgets->current_vault) {
        for (int i = 0; i < widgets->current_vault->num_sacks && i < 12; i++)
            widgets->vault_sack_match[i] = sack_has_match(widgets, &widgets->current_vault->sacks[i]);
    }

    /* Evaluate character inventory sacks (sack 0 = main inv, 1-3 = bags) */
    memset(widgets->char_sack_match, 0, sizeof(widgets->char_sack_match));
    if (widgets->current_character) {
        for (int i = 0; i < widgets->current_character->num_inv_sacks && i < 4; i++)
            widgets->char_sack_match[i] = sack_has_match(widgets, &widgets->current_character->inv_sacks[i]);
    }

    /* Update vault bag button CSS classes */
    for (int i = 0; i < 12; i++) {
        if (!widgets->vault_bag_btns[i]) continue;
        if (widgets->search_text[0] && widgets->vault_sack_match[i])
            gtk_widget_add_css_class(widgets->vault_bag_btns[i], "bag-button-search-match");
        else
            gtk_widget_remove_css_class(widgets->vault_bag_btns[i], "bag-button-search-match");
    }

    /* Update char bag button CSS classes (char_bag_btns[0-2] map to inv_sacks[1-3]) */
    for (int i = 0; i < 3; i++) {
        if (!widgets->char_bag_btns[i]) continue;
        if (widgets->search_text[0] && widgets->char_sack_match[i + 1])
            gtk_widget_add_css_class(widgets->char_bag_btns[i], "bag-button-search-match");
        else
            gtk_widget_remove_css_class(widgets->char_bag_btns[i], "bag-button-search-match");
    }

    queue_redraw_all(widgets);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    AppWidgets *widgets = (AppWidgets *)user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text && text[0]) {
        size_t len = strlen(text);
        if (len >= sizeof(widgets->search_text)) len = sizeof(widgets->search_text) - 1;
        memcpy(widgets->search_text, text, len);
        widgets->search_text[len] = '\0';
        /* lowercase */
        for (char *p = widgets->search_text; *p; p++) *p = (char)tolower((unsigned char)*p);
    } else {
        widgets->search_text[0] = '\0';
    }
    run_search(widgets);
}

static void on_search_stop(GtkSearchEntry *entry, gpointer user_data) {
    (void)entry;
    AppWidgets *widgets = (AppWidgets *)user_data;
    widgets->search_text[0] = '\0';
    gtk_editable_set_text(GTK_EDITABLE(widgets->search_entry), "");
    run_search(widgets);
}

/* ── Copy/Duplicate helpers ──────────────────────────────────────────────── */

/* Create a HeldItem copy of a vault item without removing the original. */
void copy_item_to_cursor(AppWidgets *widgets, TQVaultItem *src,
                                 bool randomize_seed) {
    HeldItem *hi = calloc(1, sizeof(HeldItem));
    vault_item_deep_copy(&hi->item, src);
    if (randomize_seed)
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

/* Create a HeldItem copy of an equipped TQItem without removing the original. */
void copy_equip_to_cursor(AppWidgets *widgets, TQItem *eq,
                                  bool randomize_seed) {
    HeldItem *hi = calloc(1, sizeof(HeldItem));
    equip_to_vault_item(&hi->item, eq);
    if (randomize_seed)
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

/* ── Unsaved character confirmation dialog ───────────────────────────── */
/* Returns: 0=Save, 1=Discard, 2=Cancel */
typedef struct {
    GMainLoop *loop;
    int result;
} ConfirmData;

static void on_confirm_save(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConfirmData *cd = user_data;
    cd->result = 0;
    g_main_loop_quit(cd->loop);
}

static void on_confirm_discard(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConfirmData *cd = user_data;
    cd->result = 1;
    g_main_loop_quit(cd->loop);
}

static void on_confirm_cancel(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ConfirmData *cd = user_data;
    cd->result = 2;
    g_main_loop_quit(cd->loop);
}

static gboolean on_confirm_close(GtkWindow *win, gpointer user_data) {
    (void)win;
    ConfirmData *cd = user_data;
    cd->result = 2;  /* treat window close as Cancel */
    g_main_loop_quit(cd->loop);
    return TRUE;
}

static int confirm_unsaved_character(AppWidgets *widgets) {
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

    return cd.result;
}

/* ── Character combo repopulation helper ──────────────────────────────── */
void repopulate_character_combo(AppWidgets *widgets, const char *select_name) {
    GtkStringList *sl = GTK_STRING_LIST(
        gtk_drop_down_get_model(GTK_DROP_DOWN(widgets->character_combo)));
    guint old_n = g_list_model_get_n_items(G_LIST_MODEL(sl));
    gtk_string_list_splice(sl, 0, old_n, NULL);

    char main_path[1024];
    snprintf(main_path, sizeof(main_path), "%s/SaveData/Main", global_config.save_folder);
    DIR *d = opendir(main_path);
    if (!d) return;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] != '_') continue;
        gtk_string_list_append(sl, dir->d_name);
    }
    closedir(d);

    guint active_idx = 0;
    const char *target = select_name ? select_name : global_config.last_character_path;
    if (target) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));
        for (guint i = 0; i < n; i++) {
            if (strcmp(gtk_string_list_get_string(sl, i), target) == 0) {
                active_idx = i;
                break;
            }
        }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->character_combo), active_idx);
}

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ── Vault combo repopulation helper ─────────────────────────────────── */
void repopulate_vault_combo(AppWidgets *widgets, const char *select_name) {
    GtkStringList *sl = GTK_STRING_LIST(
        gtk_drop_down_get_model(GTK_DROP_DOWN(widgets->vault_combo)));
    guint old_n = g_list_model_get_n_items(G_LIST_MODEL(sl));
    gtk_string_list_splice(sl, 0, old_n, NULL);

    char vault_path[1024];
    snprintf(vault_path, sizeof(vault_path), "%s/TQVaultData", global_config.save_folder);
    DIR *d = opendir(vault_path);
    if (!d) return;

    struct dirent *dir;
    char **vault_names = NULL;
    int vault_count = 0, vault_cap = 0;
    const char *suffix = ".vault.json";
    size_t suffix_len = strlen(suffix);
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;
        size_t name_len = strlen(dir->d_name);
        if (name_len > suffix_len &&
            strcmp(dir->d_name + name_len - suffix_len, suffix) == 0) {
            size_t base_len = name_len - suffix_len;
            if (base_len > 255) base_len = 255;
            if (vault_count >= vault_cap) {
                vault_cap = vault_cap ? vault_cap * 2 : 16;
                vault_names = realloc(vault_names, (size_t)vault_cap * sizeof(char *));
            }
            vault_names[vault_count] = strndup(dir->d_name, base_len);
            vault_count++;
        }
    }
    closedir(d);
    qsort(vault_names, (size_t)vault_count, sizeof(char *), compare_strings);
    for (int i = 0; i < vault_count; i++) {
        gtk_string_list_append(sl, vault_names[i]);
        free(vault_names[i]);
    }
    free(vault_names);

    guint active_idx = 0;
    const char *target = select_name ? select_name : global_config.last_vault_name;
    if (target) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(sl));
        for (guint i = 0; i < n; i++) {
            if (strcmp(gtk_string_list_get_string(sl, i), target) == 0) {
                active_idx = i;
                break;
            }
        }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->vault_combo), active_idx);
}

/* ── Load callbacks ──────────────────────────────────────────────────────── */

static void on_save_char_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *widgets = (AppWidgets *)user_data;
    save_character_if_dirty(widgets);
}

static void on_refresh_char_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *widgets = (AppWidgets *)user_data;
    if (!widgets->current_character) return;

    if (widgets->char_dirty) {
        int choice = confirm_unsaved_character(widgets);
        if (choice == 0)       /* Save */
            save_character_if_dirty(widgets);
        else if (choice == 2)  /* Cancel */
            return;
        /* Discard: fall through to reload */
    }

    cancel_held_item(widgets);
    TQCharacter *chr = character_load(widgets->current_character->filepath);
    if (chr) {
        update_ui(widgets, chr);
        prefetch_for_character(chr);
        run_search(widgets);
    }
}

static void on_character_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GtkWidget *combo = GTK_WIDGET(obj);
    AppWidgets *widgets = (AppWidgets *)user_data;
    cancel_held_item(widgets);

    if (widgets->char_dirty) {
        int choice = confirm_unsaved_character(widgets);
        if (choice == 0) {
            save_character_if_dirty(widgets);
        } else if (choice == 2) {
            /* Cancel — revert combo to current character */
            g_signal_handler_block(combo, widgets->char_combo_handler);
            /* Find and select the current character's folder name */
            if (widgets->current_character && widgets->current_character->filepath) {
                const char *fp = widgets->current_character->filepath;
                /* filepath is .../SaveData/Main/_CharName/Player.chr — extract _CharName */
                const char *player = strrchr(fp, '/');
                if (player) {
                    char prev_name[256];
                    size_t dir_len = (size_t)(player - fp);
                    const char *dir_start = fp + dir_len;
                    /* Walk back to find the preceding '/' */
                    while (dir_start > fp && *(dir_start - 1) != '/') dir_start--;
                    size_t nlen = (size_t)(player - dir_start);
                    if (nlen >= sizeof(prev_name)) nlen = sizeof(prev_name) - 1;
                    memcpy(prev_name, dir_start, nlen);
                    prev_name[nlen] = '\0';
                    dropdown_select_by_name(combo, prev_name);
                }
            }
            g_signal_handler_unblock(combo, widgets->char_combo_handler);
            return;
        }
        /* Discard: reset dirty flag, continue with switch */
        widgets->char_dirty = false;
        update_save_button_sensitivity(widgets);
    }

    char *name = dropdown_get_selected_text(combo);
    if (!name) return;

    config_set_last_character(name);
    config_save();

    char path[1024];
    snprintf(path, sizeof(path), "%s/SaveData/Main/%s/Player.chr", global_config.save_folder, name);
    g_free(name);

    TQCharacter *chr = character_load(path);
    if (chr) {
        update_ui(widgets, chr);
        prefetch_for_character(chr);
        run_search(widgets);
    }
}

/* Bag button state indices */
enum { BAG_DOWN = 0, BAG_UP = 1, BAG_OVER = 2 };

static void set_bag_btn_image(GtkWidget *btn, GdkPixbuf *pixbuf) {
    if (!btn || !pixbuf) return;
    GtkWidget *child = gtk_button_get_child(GTK_BUTTON(btn));
    GBytes *bytes = g_bytes_new(gdk_pixbuf_get_pixels(pixbuf),
                                (gsize)gdk_pixbuf_get_height(pixbuf) * gdk_pixbuf_get_rowstride(pixbuf));
    GdkTexture *texture = gdk_memory_texture_new(
        gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
        gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        bytes, gdk_pixbuf_get_rowstride(pixbuf));
    g_bytes_unref(bytes);
    if (child && GTK_IS_PICTURE(child)) {
        /* Reuse existing GtkPicture to avoid destroying a widget that GTK
         * may still reference internally (causes gtk_widget_compute_point
         * assertion failures and use-after-free on shutdown). */
        gtk_picture_set_paintable(GTK_PICTURE(child), GDK_PAINTABLE(texture));
    } else {
        GtkWidget *img = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
        gtk_picture_set_content_fit(GTK_PICTURE(img), GTK_CONTENT_FIT_FILL);
        gtk_picture_set_can_shrink(GTK_PICTURE(img), FALSE);
        gtk_button_set_child(GTK_BUTTON(btn), img);
    }
    g_object_unref(texture);
}

static void on_vault_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GtkWidget *combo = GTK_WIDGET(obj);
    AppWidgets *widgets = (AppWidgets *)user_data;
    cancel_held_item(widgets);
    save_vault_if_dirty(widgets);

    if (widgets->char_dirty) {
        int choice = confirm_unsaved_character(widgets);
        if (choice == 0) {
            save_character_if_dirty(widgets);
        } else if (choice == 2) {
            /* Cancel — revert vault combo to current vault */
            g_signal_handler_block(combo, widgets->vault_combo_handler);
            if (widgets->current_vault && widgets->current_vault->vault_name) {
                dropdown_select_by_name(combo, widgets->current_vault->vault_name);
            }
            g_signal_handler_unblock(combo, widgets->vault_combo_handler);
            return;
        }
        /* Discard: reset dirty flag, continue */
        widgets->char_dirty = false;
        update_save_button_sensitivity(widgets);
    }

    char *name = dropdown_get_selected_text(combo);
    if (!name) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/TQVaultData/%s.vault.json", global_config.save_folder, name);

    config_set_last_vault(name);
    config_save();
    g_free(name);

    if (widgets->current_vault) vault_free(widgets->current_vault);
    widgets->current_vault = vault_load_json(path);
    if (widgets->current_vault)
        prefetch_for_vault(widgets->current_vault);
    /* Reset vault bag button visuals: bag 0 selected, rest unselected */
    for (int i = 0; i < 12; i++) {
        if (widgets->vault_bag_pix[BAG_DOWN][i])
            set_bag_btn_image(widgets->vault_bag_btns[i],
                              widgets->vault_bag_pix[i == 0 ? BAG_UP : BAG_DOWN][i]);
    }
    widgets->current_sack = 0;
    gtk_widget_queue_draw(widgets->vault_drawing_area);
    run_search(widgets);
}

static void on_vault_bag_hover_enter(GtkEventControllerMotion *ctrl,
                                     double x, double y, gpointer user_data) {
    (void)ctrl; (void)x; (void)y;
    AppWidgets *widgets = (AppWidgets *)user_data;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
    if (idx != widgets->current_sack)
        set_bag_btn_image(btn, widgets->vault_bag_pix[BAG_OVER][idx]);
}

static void on_vault_bag_hover_leave(GtkEventControllerMotion *ctrl,
                                     gpointer user_data) {
    (void)ctrl;
    AppWidgets *widgets = (AppWidgets *)user_data;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
    if (idx != widgets->current_sack)
        set_bag_btn_image(btn, widgets->vault_bag_pix[BAG_DOWN][idx]);
}

static void on_char_bag_hover_enter(GtkEventControllerMotion *ctrl,
                                    double x, double y, gpointer user_data) {
    (void)ctrl; (void)x; (void)y;
    AppWidgets *widgets = (AppWidgets *)user_data;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
    if (idx != widgets->current_char_bag)
        set_bag_btn_image(btn, widgets->char_bag_pix[BAG_OVER][idx]);
}

static void on_char_bag_hover_leave(GtkEventControllerMotion *ctrl,
                                    gpointer user_data) {
    (void)ctrl;
    AppWidgets *widgets = (AppWidgets *)user_data;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
    if (idx != widgets->current_char_bag)
        set_bag_btn_image(btn, widgets->char_bag_pix[BAG_DOWN][idx]);
}

static void on_bag_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *widgets = (AppWidgets *)user_data;
    cancel_held_item(widgets);
    int bag_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
    int prev = widgets->current_sack;
    if (prev != bag_idx) {
        set_bag_btn_image(widgets->vault_bag_btns[prev],
                          widgets->vault_bag_pix[BAG_DOWN][prev]);
        set_bag_btn_image(widgets->vault_bag_btns[bag_idx],
                          widgets->vault_bag_pix[BAG_UP][bag_idx]);
    }
    widgets->current_sack = bag_idx;
    gtk_widget_queue_draw(widgets->vault_drawing_area);
}

static void on_char_bag_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *widgets = (AppWidgets *)user_data;
    cancel_held_item(widgets);
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bag-index"));
    int prev = widgets->current_char_bag;
    if (prev != idx) {
        set_bag_btn_image(widgets->char_bag_btns[prev],
                          widgets->char_bag_pix[BAG_DOWN][prev]);
        set_bag_btn_image(widgets->char_bag_btns[idx],
                          widgets->char_bag_pix[BAG_UP][idx]);
    }
    widgets->current_char_bag = idx;
    gtk_widget_queue_draw(widgets->bag_drawing_area);
}

/* ── Keyboard shortcuts ──────────────────────────────────────────────────── */

/* Populate context_* fields from the current cursor position, performing the
 * same hit-test that right-click uses.  Returns true if an item was found. */
static bool set_context_from_cursor(AppWidgets *widgets)
{
    widgets->context_item       = NULL;
    widgets->context_equip_item = NULL;
    widgets->context_source     = CONTAINER_NONE;
    widgets->context_sack_idx   = -1;
    widgets->context_equip_slot = -1;

    GtkWidget *cw = widgets->cursor_widget;
    if (!cw) return false;

    double px   = widgets->cursor_x;
    double py   = widgets->cursor_y;
    double cell = compute_cell_size(widgets);

    /* ── Vault ── */
    if (cw == widgets->vault_drawing_area && widgets->current_vault) {
        int sack_idx = widgets->current_sack;
        if (sack_idx < 0 || sack_idx >= widgets->current_vault->num_sacks)
            return false;
        TQVaultSack *sack = &widgets->current_vault->sacks[sack_idx];
        int w = gtk_widget_get_width(widgets->vault_drawing_area);
        double c = (cell > 0.0) ? cell : (double)w / 18.0;
        TQVaultItem *hit = find_item_at_cell(widgets, sack, 18, 20, c, px, py, NULL);
        if (!hit) return false;
        widgets->context_item     = hit;
        widgets->context_source   = CONTAINER_VAULT;
        widgets->context_sack_idx = sack_idx;
        return true;
    }

    /* ── Main inventory ── */
    if (cw == widgets->inv_drawing_area && widgets->current_character &&
        widgets->current_character->num_inv_sacks >= 1) {
        TQVaultSack *sack = &widgets->current_character->inv_sacks[0];
        int w = gtk_widget_get_width(widgets->inv_drawing_area);
        double c = (cell > 0.0) ? cell : (double)w / CHAR_INV_COLS;
        TQVaultItem *hit = find_item_at_cell(widgets, sack,
                                             CHAR_INV_COLS, CHAR_INV_ROWS, c, px, py, NULL);
        if (!hit) return false;
        widgets->context_item     = hit;
        widgets->context_source   = CONTAINER_INV;
        widgets->context_sack_idx = 0;
        return true;
    }

    /* ── Extra bag ── */
    if (cw == widgets->bag_drawing_area && widgets->current_character) {
        int idx = 1 + widgets->current_char_bag;
        if (idx >= widgets->current_character->num_inv_sacks) return false;
        TQVaultSack *sack = &widgets->current_character->inv_sacks[idx];
        int w = gtk_widget_get_width(widgets->bag_drawing_area);
        double c = (cell > 0.0) ? cell : (double)w / CHAR_BAG_COLS;
        TQVaultItem *hit = find_item_at_cell(widgets, sack,
                                             CHAR_BAG_COLS, CHAR_BAG_ROWS, c, px, py, NULL);
        if (!hit) return false;
        widgets->context_item     = hit;
        widgets->context_source   = CONTAINER_BAG;
        widgets->context_sack_idx = widgets->current_char_bag;
        return true;
    }

    /* ── Equipment ── */
    if (cw == widgets->equip_drawing_area && widgets->current_character) {
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
        for (int ci = 0; ci < 3 && hit_slot < 0; ci++) {
            double cy = 0.0;
            for (int si = 0; si < cols[ci].n && hit_slot < 0; si++) {
                const EquipSlot *sl = &cols[ci].slots[si];
                double bw = (double)sl->box_w * cs;
                double bh = (double)sl->box_h * cs;
                if (px >= cols[ci].cx && px < cols[ci].cx + bw &&
                    py >= cy          && py < cy + bh)
                    hit_slot = sl->slot_idx;
                cy += bh + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
            }
            if (ci == 1) {
                double cy2 = 0.0;
                for (int si = 0; si < cols[1].n; si++)
                    cy2 += (double)cols[1].slots[si].box_h * cs + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
                for (int ri = 0; ri < 2 && hit_slot < 0; ri++) {
                    double rx = cx1 + (double)ri * (cs + EQUIP_COL_GAP / 2.0);
                    double bw = RING_SLOTS[ri].box_w * cs;
                    double bh = RING_SLOTS[ri].box_h * cs;
                    if (px >= rx && px < rx + bw && py >= cy2 && py < cy2 + bh)
                        hit_slot = RING_SLOTS[ri].slot_idx;
                }
            }
        }
        if (hit_slot < 0 || hit_slot >= 12) return false;
        TQItem *eq = widgets->current_character->equipment[hit_slot];
        if (!eq || !eq->base_name) return false;
        widgets->context_equip_item = eq;
        widgets->context_source     = CONTAINER_EQUIP;
        widgets->context_equip_slot = hit_slot;
        return true;
    }

    return false;
}

/* d = duplicate, c = copy, D (Shift+d) = delete */
static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state,
                                gpointer user_data)
{
    (void)ctrl; (void)keycode;
    AppWidgets *widgets = user_data;

    /* Ignore when a modifier other than Shift is held */
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
        return FALSE;

    bool is_dup    = (keyval == GDK_KEY_d);
    bool is_copy   = (keyval == GDK_KEY_c);
    bool is_delete = (keyval == GDK_KEY_D);
    if (!is_dup && !is_copy && !is_delete) return FALSE;

    if (widgets->held_item) return FALSE;
    if (!set_context_from_cursor(widgets)) return FALSE;

    const char *action_name = is_dup  ? "item-duplicate"
                             : is_copy ? "item-copy"
                             :           "item-delete";
    g_action_group_activate_action(
        G_ACTION_GROUP(g_application_get_default()), action_name, NULL);

    return TRUE;
}

/* ── Window lifecycle ────────────────────────────────────────────────────── */

static void on_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    GtkApplication *app = GTK_APPLICATION(user_data);
    g_application_quit(G_APPLICATION(app));
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
    (void)window;
    AppWidgets *widgets = (AppWidgets *)user_data;
    cancel_held_item(widgets);
    save_vault_if_dirty(widgets);

    if (widgets->char_dirty) {
        int choice = confirm_unsaved_character(widgets);
        if (choice == 0)
            save_character_if_dirty(widgets);
        else if (choice == 2)
            return TRUE;  /* Cancel — block close */
        /* Discard: don't save, allow close */
    }

    /* Unparent the context-menu popover so the drawing area it's attached to
     * doesn't complain about leftover children during finalization. */
    if (widgets->context_menu) {
        if (widgets->context_parent)
            gtk_widget_unparent(widgets->context_menu);
        g_object_unref(widgets->context_menu);
        widgets->context_menu = NULL;
        widgets->context_parent = NULL;
    }
    if (widgets->tooltip_popover) {
        if (widgets->tooltip_parent)
            gtk_widget_unparent(widgets->tooltip_popover);
        g_object_unref(widgets->tooltip_popover);
        widgets->tooltip_popover = NULL;
        widgets->tooltip_parent = NULL;
    }
    return FALSE; /* allow default close behaviour */
}

static void on_settings_btn_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    on_settings_action(NULL, NULL, user_data);
}

/* ── Application window layout ──────────────────────────────────────────── */

void ui_app_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AppWidgets *widgets = g_malloc0(sizeof(AppWidgets));
    widgets->texture_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    widgets->last_equip_tooltip_slot = -1;
    widgets->context_equip_slot = -1;

    /* Right-click context menu: actions + popover */
    register_context_actions(app, widgets);

    GMenu *ctx_menu = g_menu_new();
    widgets->context_menu_model = ctx_menu;  /* kept alive for dynamic rebuild */

    widgets->context_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(ctx_menu));
    g_object_ref_sink(widgets->context_menu);  /* own the popover so unparent won't destroy it */
    gtk_popover_set_has_arrow(GTK_POPOVER(widgets->context_menu), FALSE);
    gtk_widget_set_halign(widgets->context_menu, GTK_ALIGN_START);

    /* Instant tooltip popover (zero-delay, replaces GTK4's 500ms tooltip) */
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
    gtk_popover_set_child(GTK_POPOVER(widgets->tooltip_popover), widgets->tooltip_label);
    gtk_widget_add_css_class(widgets->tooltip_popover, "item-tooltip");
    widgets->tooltip_parent = NULL;

    GdkPixbuf *test_relic = texture_load("Items\\AnimalRelics\\AnimalPart07B_L.tex");
    if (test_relic) {
        if (tqvc_debug) printf("DEBUG: AnimalPart07B_L.tex size: %dx%d\n", gdk_pixbuf_get_width(test_relic), gdk_pixbuf_get_height(test_relic));
        g_object_unref(test_relic);
    }
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/org/tqvaultc/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    GtkWidget *window = gtk_application_window_new(app);
    widgets->main_window = window;
    char title[64];
    snprintf(title, sizeof(title), "TQVaultC v%s", TQVAULTC_VERSION);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 1600, 900);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), widgets);
    gtk_widget_add_controller(window, key_ctrl);

    if (global_config.game_folder) {
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

    /* ── Manage Vaults dropdown ── */
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

    /* ── Manage Characters dropdown ── */
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

    /* Save Character button — grayed out when no unsaved changes */
    widgets->save_char_btn = gtk_button_new_with_label("Save Character");
    g_signal_connect(widgets->save_char_btn, "clicked", G_CALLBACK(on_save_char_clicked), widgets);
    gtk_widget_set_sensitive(widgets->save_char_btn, FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->save_char_btn);

    GtkWidget *refresh_char_btn = gtk_button_new_with_label("Refresh Character");
    g_signal_connect(refresh_char_btn, "clicked", G_CALLBACK(on_refresh_char_clicked), widgets);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh_char_btn);

    /* Search entry in the header bar — avoids layout interference with grids */
    widgets->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(widgets->search_entry, 200, -1);
    g_signal_connect(widgets->search_entry, "search-changed", G_CALLBACK(on_search_changed), widgets);
    g_signal_connect(widgets->search_entry, "stop-search", G_CALLBACK(on_search_stop), widgets);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->search_entry);

    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    /* ── Top-level horizontal split: vault (left) | char panel (right) ── */
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    widgets->main_hbox = main_hbox;
    gtk_widget_set_hexpand(main_hbox, TRUE);
    gtk_widget_set_vexpand(main_hbox, TRUE);
    gtk_window_set_child(GTK_WINDOW(window), main_hbox);

    /* ── Left: vault area ── */
    GtkWidget *main_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_hexpand(main_area, TRUE);
    gtk_widget_set_vexpand(main_area, TRUE);
    gtk_box_append(GTK_BOX(main_hbox), main_area);

    widgets->vault_combo = gtk_drop_down_new_from_strings(NULL);
    gtk_box_append(GTK_BOX(main_area), widgets->vault_combo);
    widgets->vault_combo_handler = g_signal_connect(widgets->vault_combo,
        "notify::selected", G_CALLBACK(on_vault_changed), widgets);

    /* Vault bag buttons — three-state textures (down/up/over) */
    GtkWidget *bag_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(main_area), bag_hbox);
    {
        const char *tex_paths[3] = {
            "InGameUI\\characterscreen\\inventorybagdown01.tex",
            "InGameUI\\characterscreen\\inventorybagup01.tex",
            "InGameUI\\characterscreen\\inventorybagover01.tex",
        };
        GdkPixbuf *base[3] = {NULL, NULL, NULL};
        for (int s = 0; s < 3; s++) {
            GdkPixbuf *raw = texture_load(tex_paths[s]);
            if (raw) {
                base[s] = gdk_pixbuf_scale_simple(raw, 40, 36, GDK_INTERP_BILINEAR);
                g_object_unref(raw);
            }
        }
        bool have_tex = (base[0] && base[1] && base[2]);
        for (int i = 0; i < 12; i++) {
            GtkWidget *btn;
            if (have_tex) {
                for (int s = 0; s < 3; s++)
                    widgets->vault_bag_pix[s][i] = texture_create_with_number(base[s], i + 1);
                int init_state = (i == 0) ? BAG_UP : BAG_DOWN;
                btn = gtk_button_new();
                gtk_widget_add_css_class(btn, "bag-button");
                gtk_widget_set_size_request(btn, 40, 36);
                set_bag_btn_image(btn, widgets->vault_bag_pix[init_state][i]);
            } else {
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
            gtk_box_append(GTK_BOX(bag_hbox), btn);
        }
        for (int s = 0; s < 3; s++)
            if (base[s]) g_object_unref(base[s]);
    }

    widgets->vault_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(widgets->vault_drawing_area, TRUE);
    gtk_widget_set_vexpand(widgets->vault_drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widgets->vault_drawing_area), vault_draw_cb, widgets, NULL);
    g_signal_connect(widgets->vault_drawing_area, "resize", G_CALLBACK(on_vault_resize), widgets);
    /* Click + motion for vault */
    GtkGesture *vault_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(vault_click), 0);
    g_signal_connect(vault_click, "pressed", G_CALLBACK(on_vault_click), widgets);
    gtk_widget_add_controller(widgets->vault_drawing_area, GTK_EVENT_CONTROLLER(vault_click));
    GtkEventController *vault_motion = gtk_event_controller_motion_new();
    g_signal_connect(vault_motion, "motion", G_CALLBACK(on_motion), widgets);
    g_signal_connect(vault_motion, "leave", G_CALLBACK(on_motion_leave), widgets);
    gtk_widget_add_controller(widgets->vault_drawing_area, vault_motion);
    gtk_box_append(GTK_BOX(main_area), widgets->vault_drawing_area);

    /* ── Right: character panel ── */
    GtkWidget *char_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_hexpand(char_panel, TRUE);
    gtk_widget_set_vexpand(char_panel, TRUE);
    gtk_widget_set_margin_start(char_panel, 6);
    gtk_box_append(GTK_BOX(main_hbox), char_panel);

    widgets->character_combo = gtk_drop_down_new_from_strings(NULL);
    gtk_box_append(GTK_BOX(char_panel), widgets->character_combo);
    widgets->char_combo_handler = g_signal_connect(widgets->character_combo,
        "notify::selected", G_CALLBACK(on_character_changed), widgets);

    /* Inventory + bag grid layout */
    GtkWidget *inv_bag_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(inv_bag_grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(inv_bag_grid), 10);
    gtk_widget_set_hexpand(inv_bag_grid, TRUE);
    gtk_widget_set_vexpand(inv_bag_grid, TRUE);
    gtk_box_append(GTK_BOX(char_panel), inv_bag_grid);

    /* Row 0, col 1: bag icon buttons */
    GtkWidget *char_bag_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_grid_attach(GTK_GRID(inv_bag_grid), char_bag_hbox, 1, 0, 1, 1);

    {
        const char *tex_paths[3] = {
            "InGameUI\\characterscreen\\inventorybagdown01.tex",
            "InGameUI\\characterscreen\\inventorybagup01.tex",
            "InGameUI\\characterscreen\\inventorybagover01.tex",
        };
        GdkPixbuf *cbase[3] = {NULL, NULL, NULL};
        for (int s = 0; s < 3; s++) {
            GdkPixbuf *raw = texture_load(tex_paths[s]);
            if (raw) {
                cbase[s] = gdk_pixbuf_scale_simple(raw, 40, 36, GDK_INTERP_BILINEAR);
                g_object_unref(raw);
            }
        }
        bool have_tex = (cbase[0] && cbase[1] && cbase[2]);
        for (int i = 0; i < 3; i++) {
            GtkWidget *btn;
            if (have_tex) {
                for (int s = 0; s < 3; s++)
                    widgets->char_bag_pix[s][i] = texture_create_with_number(cbase[s], i + 1);
                int init_state = (i == 0) ? BAG_UP : BAG_DOWN;
                btn = gtk_button_new();
                gtk_widget_add_css_class(btn, "bag-button");
                gtk_widget_set_size_request(btn, 40, 36);
                set_bag_btn_image(btn, widgets->char_bag_pix[init_state][i]);
            } else {
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
            gtk_box_append(GTK_BOX(char_bag_hbox), btn);
        }
        for (int s = 0; s < 3; s++)
            if (cbase[s]) g_object_unref(cbase[s]);
    }

    /* Row 1, col 0: main inventory 12x5 */
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

    /* Row 1, col 1: extra bag 8x5 */
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

    /* Bottom section: equip+stats on left, tables stacked on right. */
    GtkWidget *bottom_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_vexpand(bottom_hbox, FALSE);
    gtk_box_append(GTK_BOX(char_panel), bottom_hbox);

    /* Left column: stats above equipment */
    GtkWidget *equip_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(equip_col, FALSE);
    gtk_widget_set_vexpand(equip_col, FALSE);
    gtk_box_append(GTK_BOX(bottom_hbox), equip_col);

    /* Stats above equipment — wide compact grid */
    GtkWidget *stats_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(stats_frame, FALSE);
    gtk_widget_set_vexpand(stats_frame, FALSE);
    gtk_widget_set_valign(stats_frame, GTK_ALIGN_START);
    gtk_widget_add_css_class(stats_frame, "stats-frame");
    gtk_box_append(GTK_BOX(equip_col), stats_frame);

    /* name_label: kept in the widget hierarchy for ancestor lookups */
    widgets->name_label = gtk_label_new("");
    gtk_widget_set_visible(widgets->name_label, FALSE);
    gtk_box_append(GTK_BOX(stats_frame), widgets->name_label);

    GtkWidget *stats_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(stats_grid), 2);
    gtk_grid_set_row_spacing(GTK_GRID(stats_grid), 0);
    gtk_widget_add_css_class(stats_grid, "stats-grid");
    gtk_box_append(GTK_BOX(stats_frame), stats_grid);

    int sg_row = 0; /* current grid row */

    /* Helper: place a key+value pair at (col, row) spanning 1 column */
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

    /* Row 0: Level, Mastery 1, Mastery 2 */
    STAT_CELL(0, "Lv",  &widgets->level_label);
    STAT_CELL(1, "",     &widgets->mastery1_label);
    STAT_CELL(2, "",     &widgets->mastery2_label);
    sg_row++;

    /* Row 1: Str, Dex, Int */
    STAT_CELL(0, "Str",  &widgets->strength_label);
    STAT_CELL(1, "Dex",  &widgets->dexterity_label);
    STAT_CELL(2, "Int",  &widgets->intelligence_label);
    sg_row++;

    /* Row 2: HP, MP, K/D */
    STAT_CELL(0, "HP",   &widgets->health_label);
    STAT_CELL(1, "MP",   &widgets->mana_label);
    /* Combined kills/deaths cell */
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

    #undef STAT_CELL

    /* Equipment drawing area */
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

    /* Right column: single scrollable pane with all stats tables */
    GtkWidget *tables_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(tables_col, TRUE);
    gtk_widget_set_vexpand(tables_col, TRUE);
    gtk_box_append(GTK_BOX(bottom_hbox), tables_col);

    GtkWidget *tables_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tables_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(tables_scroll, TRUE);
    gtk_widget_set_vexpand(tables_scroll, TRUE);
    gtk_box_append(GTK_BOX(tables_col), tables_scroll);

    GtkWidget *tables_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tables_scroll), tables_inner);

    /* Build all stat tables (resistances, damage, speed, health) */
    build_stat_tables(widgets, tables_inner);

    /* ── Actions ── */
    GSimpleAction *settings_action = g_simple_action_new("settings", NULL);
    g_signal_connect(settings_action, "activate", G_CALLBACK(on_settings_action), widgets);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(settings_action));
    GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
    g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(quit_action));

    /* Manage Vaults / Characters actions */
    register_manage_actions(GTK_WINDOW(window), widgets);

    /* ── Populate combo boxes ── */
    if (global_config.save_folder) {
        repopulate_vault_combo(widgets, NULL);
        repopulate_character_combo(widgets, NULL);
    }
    /* Save vault on close */
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), widgets);

    gtk_window_present(GTK_WINDOW(window));
}
