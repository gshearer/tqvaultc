/**
 * ui_bag_menu.c — Right-click context menu on vault/character bag buttons.
 *
 * Provides bulk operations: move/copy all items, empty bag, auto-arrange,
 * clipboard export/import.
 */
#include "ui.h"
#include "arz.h"
#include "asset_lookup.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern int tqvc_debug;

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Mark the appropriate dirty flag for a container. */
static void mark_bag_dirty(AppWidgets *widgets, ContainerType ct) {
    if (ct == CONTAINER_VAULT)
        widgets->vault_dirty = true;
    else
        widgets->char_dirty = true;
}

/* Resolve (container_type, idx) → sack pointer, cols, rows.
 * Returns NULL if the container/index is invalid or not loaded. */
static TQVaultSack *resolve_sack(AppWidgets *widgets, ContainerType ct, int idx,
                                  int *out_cols, int *out_rows) {
    if (ct == CONTAINER_VAULT) {
        if (!widgets->current_vault) return NULL;
        if (idx < 0 || idx >= widgets->current_vault->num_sacks) return NULL;
        *out_cols = VAULT_COLS;
        *out_rows = VAULT_ROWS;
        return &widgets->current_vault->sacks[idx];
    }
    if (ct == CONTAINER_INV) {
        if (!widgets->current_character) return NULL;
        if (widgets->current_character->num_inv_sacks < 1) return NULL;
        *out_cols = 12;
        *out_rows = 5;
        return &widgets->current_character->inv_sacks[0];
    }
    if (ct == CONTAINER_BAG) {
        if (!widgets->current_character) return NULL;
        int si = 1 + idx;
        if (si >= widgets->current_character->num_inv_sacks) return NULL;
        *out_cols = 8;
        *out_rows = 5;
        return &widgets->current_character->inv_sacks[si];
    }
    return NULL;
}

/* Parse a destination string like "v:3", "i:0", "b:2" into container type + index. */
static bool parse_dest(const char *s, ContainerType *ct, int *idx) {
    if (!s || strlen(s) < 3 || s[1] != ':') return false;
    *idx = atoi(s + 2);
    switch (s[0]) {
    case 'v': *ct = CONTAINER_VAULT; return true;
    case 'i': *ct = CONTAINER_INV;   return true;
    case 'b': *ct = CONTAINER_BAG;   return true;
    default:  return false;
    }
}

/* Find first free spot in occupancy grid for item of given dimensions.
 * Scans top-to-bottom, left-to-right. Returns true if found. */
static bool find_free_spot(const bool *grid, int cols, int rows,
                           int item_w, int item_h, int *out_x, int *out_y) {
    for (int y = 0; y <= rows - item_h; y++) {
        for (int x = 0; x <= cols - item_w; x++) {
            if (can_place_item(grid, cols, rows, x, y, item_w, item_h)) {
                *out_x = x;
                *out_y = y;
                return true;
            }
        }
    }
    return false;
}

/* Mark cells occupied by an item in the grid. */
static void mark_grid(bool *grid, int cols, int x, int y, int w, int h) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            grid[(y + dy) * cols + (x + dx)] = true;
}

/* ── Destination submenu builder ───────────────────────────────────────── */

static GMenu *build_dest_submenu(AppWidgets *widgets, const char *action_prefix,
                                  ContainerType src_ct, int src_idx) {
    GMenu *menu = g_menu_new();

    /* Vault bags */
    if (widgets->current_vault) {
        int n = widgets->current_vault->num_sacks;
        if (n > 12) n = 12;
        for (int i = 0; i < n; i++) {
            if (src_ct == CONTAINER_VAULT && i == src_idx) continue;
            char label[32], action[64];
            snprintf(label, sizeof(label), "Vault Bag %d", i + 1);
            snprintf(action, sizeof(action), "app.%s::v:%d", action_prefix, i);
            g_menu_append(menu, label, action);
        }
    }

    /* Character inventory and bags */
    if (widgets->current_character) {
        if (!(src_ct == CONTAINER_INV)) {
            g_menu_append(menu, "Main Inventory",
                          g_strdup_printf("app.%s::i:0", action_prefix));
        }
        int n_bags = widgets->current_character->num_inv_sacks - 1;
        if (n_bags > 3) n_bags = 3;
        for (int i = 0; i < n_bags; i++) {
            if (src_ct == CONTAINER_BAG && i == src_idx) continue;
            char label[32], action[64];
            snprintf(label, sizeof(label), "Character Bag %d", i + 1);
            snprintf(action, sizeof(action), "app.%s::b:%d", action_prefix, i);
            g_menu_append(menu, label, action);
        }
    }

    return menu;
}

/* ── Transfer items (move/copy) ────────────────────────────────────────── */

static void do_transfer(AppWidgets *widgets, const char *dest_str, bool is_move) {
    ContainerType src_ct = widgets->bag_menu_source;
    int src_idx = widgets->bag_menu_sack_idx;
    ContainerType dst_ct;
    int dst_idx;
    if (!parse_dest(dest_str, &dst_ct, &dst_idx)) return;

    int src_cols, src_rows, dst_cols, dst_rows;
    TQVaultSack *src = resolve_sack(widgets, src_ct, src_idx, &src_cols, &src_rows);
    TQVaultSack *dst = resolve_sack(widgets, dst_ct, dst_idx, &dst_cols, &dst_rows);
    if (!src || !dst || src->num_items == 0) return;

    /* Build destination occupancy grid */
    bool *grid = build_occupancy_grid(widgets, dst, dst_cols, dst_rows, NULL);
    if (!grid) return;

    /* Track which source items were placed (for move removal) */
    bool *placed = calloc((size_t)src->num_items, sizeof(bool));

    for (int i = 0; i < src->num_items; i++) {
        TQVaultItem *si = &src->items[i];
        if (!si->base_name) continue;

        int iw, ih;
        get_item_dims(widgets, si, &iw, &ih);

        int px, py;
        /* Try to preserve original position first */
        if (si->point_x >= 0 && si->point_y >= 0 &&
            si->point_x + iw <= dst_cols && si->point_y + ih <= dst_rows &&
            can_place_item(grid, dst_cols, dst_rows, si->point_x, si->point_y, iw, ih)) {
            px = si->point_x;
            py = si->point_y;
        } else if (!find_free_spot(grid, dst_cols, dst_rows, iw, ih, &px, &py)) {
            continue;  /* no room — skip */
        }

        TQVaultItem copy;
        vault_item_deep_copy(&copy, si);
        copy.point_x = px;
        copy.point_y = py;
        sack_add_item(dst, &copy);
        mark_grid(grid, dst_cols, px, py, iw, ih);
        placed[i] = true;
    }

    free(grid);

    /* Remove placed items from source (reverse order to keep indices stable) */
    if (is_move) {
        for (int i = src->num_items - 1; i >= 0; i--) {
            if (!placed[i]) continue;
            vault_item_free_strings(&src->items[i]);
            if (i < src->num_items - 1)
                memmove(&src->items[i], &src->items[i + 1],
                        (size_t)(src->num_items - 1 - i) * sizeof(TQVaultItem));
            src->num_items--;
        }
        if (src->num_items == 0) {
            free(src->items);
            src->items = NULL;
        }
        mark_bag_dirty(widgets, src_ct);
    }

    free(placed);
    mark_bag_dirty(widgets, dst_ct);
    invalidate_tooltips(widgets);
    queue_redraw_all(widgets);
}

/* ── Action callbacks ──────────────────────────────────────────────────── */

static void on_bag_move_to(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data) {
    (void)action;
    AppWidgets *widgets = user_data;
    const char *dest = g_variant_get_string(parameter, NULL);
    do_transfer(widgets, dest, true);
}

static void on_bag_copy_to(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data) {
    (void)action;
    AppWidgets *widgets = user_data;
    const char *dest = g_variant_get_string(parameter, NULL);
    do_transfer(widgets, dest, false);
}

static void on_bag_empty(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data) {
    (void)action; (void)parameter;
    AppWidgets *widgets = user_data;

    int cols, rows;
    TQVaultSack *sack = resolve_sack(widgets, widgets->bag_menu_source,
                                      widgets->bag_menu_sack_idx, &cols, &rows);
    if (!sack || sack->num_items == 0) return;

    for (int i = 0; i < sack->num_items; i++)
        vault_item_free_strings(&sack->items[i]);
    free(sack->items);
    sack->items = NULL;
    sack->num_items = 0;

    mark_bag_dirty(widgets, widgets->bag_menu_source);
    invalidate_tooltips(widgets);
    queue_redraw_all(widgets);
}

/* Comparison function for auto-arrange: sort by area descending, then height, then width. */
static int cmp_items_by_area(const void *a, const void *b) {
    const int *ia = a, *ib = b;
    /* ia[0] = index, ia[1] = width, ia[2] = height, ia[3] = area */
    if (ib[3] != ia[3]) return ib[3] - ia[3];  /* area descending */
    if (ib[2] != ia[2]) return ib[2] - ia[2];  /* height descending */
    return ib[1] - ia[1];                       /* width descending */
}

static void on_bag_auto_arrange(GSimpleAction *action, GVariant *parameter,
                                gpointer user_data) {
    (void)action; (void)parameter;
    AppWidgets *widgets = user_data;

    int cols, rows;
    TQVaultSack *sack = resolve_sack(widgets, widgets->bag_menu_source,
                                      widgets->bag_menu_sack_idx, &cols, &rows);
    if (!sack || sack->num_items == 0) return;

    int n = sack->num_items;

    /* Gather dimensions and sort indices by area descending */
    int (*info)[4] = malloc((size_t)n * sizeof(int[4]));
    for (int i = 0; i < n; i++) {
        int iw, ih;
        get_item_dims(widgets, &sack->items[i], &iw, &ih);
        info[i][0] = i;
        info[i][1] = iw;
        info[i][2] = ih;
        info[i][3] = iw * ih;
    }
    qsort(info, (size_t)n, sizeof(int[4]), cmp_items_by_area);

    /* Fresh empty grid */
    bool *grid = calloc((size_t)(cols * rows), sizeof(bool));

    for (int k = 0; k < n; k++) {
        int idx = info[k][0];
        int iw  = info[k][1];
        int ih  = info[k][2];
        int px, py;
        if (find_free_spot(grid, cols, rows, iw, ih, &px, &py)) {
            sack->items[idx].point_x = px;
            sack->items[idx].point_y = py;
            mark_grid(grid, cols, px, py, iw, ih);
        }
        /* else: keep old position (shouldn't happen since items already fit) */
    }

    free(grid);
    free(info);

    mark_bag_dirty(widgets, widgets->bag_menu_source);
    invalidate_tooltips(widgets);
    queue_redraw_all(widgets);
}

static void on_bag_export(GSimpleAction *action, GVariant *parameter,
                          gpointer user_data) {
    (void)action; (void)parameter;
    AppWidgets *widgets = user_data;

    int cols, rows;
    TQVaultSack *sack = resolve_sack(widgets, widgets->bag_menu_source,
                                      widgets->bag_menu_sack_idx, &cols, &rows);
    if (!sack || sack->num_items == 0) return;

    struct json_object *root = json_object_new_object();
    struct json_object *items_arr = json_object_new_array();

    for (int i = 0; i < sack->num_items; i++) {
        TQVaultItem *it = &sack->items[i];
        struct json_object *obj = json_object_new_object();
        json_object_object_add(obj, "stackSize",
            json_object_new_int(it->stack_size > 0 ? it->stack_size : 1));
        json_object_object_add(obj, "seed", json_object_new_int((int32_t)it->seed));
        json_object_object_add(obj, "baseName",
            json_object_new_string(it->base_name ? it->base_name : ""));
        json_object_object_add(obj, "prefixName",
            json_object_new_string(it->prefix_name ? it->prefix_name : ""));
        json_object_object_add(obj, "suffixName",
            json_object_new_string(it->suffix_name ? it->suffix_name : ""));
        json_object_object_add(obj, "relicName",
            json_object_new_string(it->relic_name ? it->relic_name : ""));
        json_object_object_add(obj, "relicBonus",
            json_object_new_string(it->relic_bonus ? it->relic_bonus : ""));
        json_object_object_add(obj, "var1", json_object_new_int((int32_t)it->var1));
        json_object_object_add(obj, "relicName2",
            json_object_new_string(it->relic_name2 ? it->relic_name2 : ""));
        json_object_object_add(obj, "relicBonus2",
            json_object_new_string(it->relic_bonus2 ? it->relic_bonus2 : ""));
        json_object_object_add(obj, "var2", json_object_new_int((int32_t)it->var2));
        json_object_object_add(obj, "pointX", json_object_new_int(it->point_x));
        json_object_object_add(obj, "pointY", json_object_new_int(it->point_y));
        json_object_array_add(items_arr, obj);
    }

    json_object_object_add(root, "items", items_arr);
    const char *json_str = json_object_to_json_string_ext(root,
        JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);

    GdkDisplay *display = gdk_display_get_default();
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    gdk_clipboard_set_text(clipboard, json_str);

    json_object_put(root);
}

/* ── Import (async clipboard read) ─────────────────────────────────────── */

static void on_import_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppWidgets *widgets = user_data;
    GdkClipboard *clipboard = GDK_CLIPBOARD(source);

    char *text = gdk_clipboard_read_text_finish(clipboard, result, NULL);
    if (!text || !text[0]) {
        g_free(text);
        return;
    }

    struct json_object *parsed = json_tokener_parse(text);
    g_free(text);
    if (!parsed) return;

    struct json_object *items_arr;
    if (!json_object_object_get_ex(parsed, "items", &items_arr)) {
        json_object_put(parsed);
        return;
    }

    int cols, rows;
    TQVaultSack *sack = resolve_sack(widgets, widgets->bag_menu_source,
                                      widgets->bag_menu_sack_idx, &cols, &rows);
    if (!sack) {
        json_object_put(parsed);
        return;
    }

    bool *grid = build_occupancy_grid(widgets, sack, cols, rows, NULL);
    if (!grid) {
        json_object_put(parsed);
        return;
    }

    int n = json_object_array_length(items_arr);
    for (int i = 0; i < n; i++) {
        struct json_object *obj = json_object_array_get_idx(items_arr, i);
        if (!obj) continue;

        TQVaultItem item;
        memset(&item, 0, sizeof(item));

        struct json_object *val;
        if (json_object_object_get_ex(obj, "seed", &val))
            item.seed = (uint32_t)json_object_get_int(val);
        if (json_object_object_get_ex(obj, "baseName", &val))
            item.base_name = safe_strdup(json_object_get_string(val));
        if (json_object_object_get_ex(obj, "prefixName", &val))
            item.prefix_name = safe_strdup(json_object_get_string(val));
        if (json_object_object_get_ex(obj, "suffixName", &val))
            item.suffix_name = safe_strdup(json_object_get_string(val));
        if (json_object_object_get_ex(obj, "relicName", &val))
            item.relic_name = safe_strdup(json_object_get_string(val));
        if (json_object_object_get_ex(obj, "relicBonus", &val))
            item.relic_bonus = safe_strdup(json_object_get_string(val));
        if (json_object_object_get_ex(obj, "relicName2", &val))
            item.relic_name2 = safe_strdup(json_object_get_string(val));
        if (json_object_object_get_ex(obj, "relicBonus2", &val))
            item.relic_bonus2 = safe_strdup(json_object_get_string(val));
        if (json_object_object_get_ex(obj, "var1", &val))
            item.var1 = (uint32_t)json_object_get_int(val);
        if (json_object_object_get_ex(obj, "var2", &val))
            item.var2 = (uint32_t)json_object_get_int(val);
        if (json_object_object_get_ex(obj, "stackSize", &val))
            item.stack_size = json_object_get_int(val);
        if (item.stack_size < 1) item.stack_size = 1;
        if (json_object_object_get_ex(obj, "pointX", &val))
            item.point_x = json_object_get_int(val);
        if (json_object_object_get_ex(obj, "pointY", &val))
            item.point_y = json_object_get_int(val);

        /* Determine dimensions from DBR Class (same logic as vault_load_json) */
        item.width = 1;
        item.height = 1;
        if (item.base_name) {
            TQArzRecordData *dbr = asset_get_dbr(item.base_name);
            if (dbr) {
                bool class_found;
                char *class_name = arz_record_get_string(dbr, "Class", &class_found);
                if (class_found && class_name) {
                    if      (strstr(class_name, "UpperBody"))       { item.width = 2; item.height = 4; }
                    else if (strstr(class_name, "LowerBody"))       { item.width = 2; item.height = 2; }
                    else if (strstr(class_name, "Head"))            { item.width = 2; item.height = 2; }
                    else if (strstr(class_name, "Forearm"))         { item.width = 2; item.height = 2; }
                    else if (strstr(class_name, "WeaponMelee"))     { item.width = 1; item.height = 3; }
                    else if (strstr(class_name, "WeaponHunting"))   { item.width = 2; item.height = 4; }
                    else if (strstr(class_name, "WeaponMagical"))   { item.width = 2; item.height = 4; }
                    else if (strstr(class_name, "Shield"))          { item.width = 2; item.height = 3; }
                    else if (strstr(class_name, "Amulet"))          { item.width = 1; item.height = 2; }
                    else if (strstr(class_name, "ItemArtifactFormula")) { item.width = 1; item.height = 2; }
                    else if (strstr(class_name, "ItemArtifact"))    { item.width = 2; item.height = 2; }
                    free(class_name);
                }
                bool iw_found, ih_found;
                int iw = arz_record_get_int(dbr, "ItemWidth",  0, &iw_found);
                int ih = arz_record_get_int(dbr, "ItemHeight", 0, &ih_found);
                if (iw_found && iw > 0) item.width  = iw;
                if (ih_found && ih > 0) item.height = ih;
            }
        }

        /* Use texture-derived dimensions for placement */
        int pw, ph;
        get_item_dims(widgets, &item, &pw, &ph);

        /* Try to preserve original position from the exported data */
        int px, py;
        int orig_x = item.point_x, orig_y = item.point_y;
        if (orig_x >= 0 && orig_y >= 0 &&
            orig_x + pw <= cols && orig_y + ph <= rows &&
            can_place_item(grid, cols, rows, orig_x, orig_y, pw, ph)) {
            px = orig_x;
            py = orig_y;
        } else if (!find_free_spot(grid, cols, rows, pw, ph, &px, &py)) {
            vault_item_free_strings(&item);
            continue;
        }
        item.point_x = px;
        item.point_y = py;
        sack_add_item(sack, &item);
        mark_grid(grid, cols, px, py, pw, ph);
    }

    free(grid);
    json_object_put(parsed);

    mark_bag_dirty(widgets, widgets->bag_menu_source);
    invalidate_tooltips(widgets);
    queue_redraw_all(widgets);
}

static void on_bag_import(GSimpleAction *action, GVariant *parameter,
                          gpointer user_data) {
    (void)action; (void)parameter;
    AppWidgets *widgets = user_data;

    GdkDisplay *display = gdk_display_get_default();
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    gdk_clipboard_read_text_async(clipboard, NULL, on_import_ready, widgets);
}

/* ── Menu display ──────────────────────────────────────────────────────── */

void show_bag_context_menu(AppWidgets *widgets, GtkWidget *parent_widget,
                           ContainerType source, int sack_idx) {
    cancel_held_item(widgets);

    widgets->bag_menu_source = source;
    widgets->bag_menu_sack_idx = sack_idx;

    /* Clear and rebuild the menu model */
    g_menu_remove_all(widgets->bag_menu_model);

    /* Move items to → submenu */
    GMenu *move_sub = build_dest_submenu(widgets, "bag-move-to", source, sack_idx);
    if (g_menu_model_get_n_items(G_MENU_MODEL(move_sub)) > 0)
        g_menu_append_submenu(widgets->bag_menu_model, "Move items to",
                              G_MENU_MODEL(move_sub));
    g_object_unref(move_sub);

    /* Copy items to → submenu */
    GMenu *copy_sub = build_dest_submenu(widgets, "bag-copy-to", source, sack_idx);
    if (g_menu_model_get_n_items(G_MENU_MODEL(copy_sub)) > 0)
        g_menu_append_submenu(widgets->bag_menu_model, "Copy items to",
                              G_MENU_MODEL(copy_sub));
    g_object_unref(copy_sub);

    /* Direct actions */
    g_menu_append(widgets->bag_menu_model, "Empty bag", "app.bag-empty");
    g_menu_append(widgets->bag_menu_model, "Auto-arrange", "app.bag-auto-arrange");
    g_menu_append(widgets->bag_menu_model, "Export to clipboard", "app.bag-export");
    g_menu_append(widgets->bag_menu_model, "Import from clipboard", "app.bag-import");

    /* Re-parent popover to the button */
    if (widgets->bag_menu_parent && widgets->bag_menu_parent != parent_widget)
        gtk_widget_unparent(widgets->bag_menu);
    if (widgets->bag_menu_parent != parent_widget) {
        gtk_widget_set_parent(widgets->bag_menu, parent_widget);
        widgets->bag_menu_parent = parent_widget;
    }

    /* Point to a small rect at the button center */
    int bw = gtk_widget_get_width(parent_widget);
    int bh = gtk_widget_get_height(parent_widget);
    GdkRectangle rect = { bw / 2, bh / 2, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(widgets->bag_menu), &rect);

    gtk_popover_popup(GTK_POPOVER(widgets->bag_menu));
}

/* ── Action registration ───────────────────────────────────────────────── */

void register_bag_menu_actions(GtkApplication *app, AppWidgets *widgets) {
    struct { const char *name; GCallback cb; const GVariantType *type; } acts[] = {
        { "bag-move-to",      G_CALLBACK(on_bag_move_to),      G_VARIANT_TYPE_STRING },
        { "bag-copy-to",      G_CALLBACK(on_bag_copy_to),      G_VARIANT_TYPE_STRING },
        { "bag-empty",        G_CALLBACK(on_bag_empty),        NULL },
        { "bag-export",       G_CALLBACK(on_bag_export),       NULL },
        { "bag-import",       G_CALLBACK(on_bag_import),       NULL },
        { "bag-auto-arrange", G_CALLBACK(on_bag_auto_arrange), NULL },
    };

    for (size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        GSimpleAction *a = g_simple_action_new(acts[i].name, acts[i].type);
        g_signal_connect(a, "activate", acts[i].cb, widgets);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(a));
        g_object_unref(a);
    }
}
