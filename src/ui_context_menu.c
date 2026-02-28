/**
 * ui_context_menu.c — Right-click context menu, item action handlers,
 *                      bonus submenus, and quantity dialog.
 *
 * Extracted from ui.c for maintainability.
 */
#include "ui.h"
#include "arz.h"
#include "asset_lookup.h"
#include "item_stats.h"
#include "affix_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Relic type label ──────────────────────────────────────────────────── */

/* Determine if a relic path refers to a charm or a relic. */
static const char* relic_type_label_ui(const char *path) {
    if (strcasestr(path, "charm")) return "Charm";
    if (strcasestr(path, "animalrelic")) return "Charm";
    return "Relic";
}

/* ── Bonus table path lookup ───────────────────────────────────────────── */

/* Get the bonus table DBR path for a relic, charm, or artifact.
 * For relics/charms: reads "bonusTableName" from the item's own DBR.
 * For artifacts: constructs the formula path and reads "artifactBonusTableName".
 * Returns an internal pointer (do not free), or NULL. */
static const char* get_bonus_table_path(const char *item_path) {
    if (!item_path || !item_path[0]) return NULL;

    if (item_is_artifact(item_path)) {
        /* Artifact: find the formula record.
         * E.g. records/xpack/items/artifacts/foo.dbr ->
         *      records/xpack/items/artifacts/arcaneformulae/foo_formula.dbr */
        char formula_path[1024];
        const char *last_sep = strrchr(item_path, '\\');
        if (!last_sep) return NULL;
        int dir_len = (int)(last_sep - item_path);
        const char *fname = last_sep + 1;
        /* Strip .dbr extension from filename */
        int name_len = (int)strlen(fname);
        if (name_len > 4 && strcasecmp(fname + name_len - 4, ".dbr") == 0)
            name_len -= 4;

        snprintf(formula_path, sizeof(formula_path),
                 "%.*s\\arcaneformulae\\%.*s_formula.dbr",
                 dir_len, item_path, name_len, fname);

        TQArzRecordData *formula_dbr = asset_get_dbr(formula_path);
        if (formula_dbr) {
            /* Standard artifact bonus table */
            for (uint32_t i = 0; i < formula_dbr->num_vars; i++) {
                if (formula_dbr->vars[i].name &&
                    strcasecmp(formula_dbr->vars[i].name, "artifactBonusTableName") == 0 &&
                    formula_dbr->vars[i].type == TQ_VAR_STRING &&
                    formula_dbr->vars[i].count > 0 &&
                    formula_dbr->vars[i].value.str[0] &&
                    formula_dbr->vars[i].value.str[0][0]) {
                    return formula_dbr->vars[i].value.str[0];
                }
            }
        }
        return NULL;
    }

    /* Relic/Charm: read bonusTableName from the item DBR */
    TQArzRecordData *dbr = asset_get_dbr(item_path);
    if (!dbr) return NULL;
    for (uint32_t i = 0; i < dbr->num_vars; i++) {
        if (dbr->vars[i].name &&
            strcasecmp(dbr->vars[i].name, "bonusTableName") == 0 &&
            dbr->vars[i].type == TQ_VAR_STRING &&
            dbr->vars[i].count > 0 &&
            dbr->vars[i].value.str[0] &&
            dbr->vars[i].value.str[0][0]) {
            return dbr->vars[i].value.str[0];
        }
    }
    return NULL;
}

/* ── Build bonus submenu ───────────────────────────────────────────────── */

/* Build a "Completion Bonus" GMenu from a bonus randomizer table.
 * Returns a new GMenu (caller owns), or NULL if the table has no entries. */
static GMenu* build_bonus_submenu(const char *table_path, const char *current_bonus,
                                   const char *action_name, TQTranslation *tr) {
    if (!table_path || !table_path[0]) return NULL;

    TQArzRecordData *dbr = asset_get_dbr(table_path);
    if (!dbr) return NULL;

    /* Collect randomizerName[N] / randomizerWeight[N] pairs */
    typedef struct { char *path; float weight; } BonusPair;
    GHashTable *pairs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    for (uint32_t i = 0; i < dbr->num_vars; i++) {
        if (!dbr->vars[i].name) continue;
        const char *vname = dbr->vars[i].name;

        if (strncasecmp(vname, "randomizerName", 14) == 0 &&
            dbr->vars[i].type == TQ_VAR_STRING && dbr->vars[i].count > 0) {
            const char *val = dbr->vars[i].value.str[0];
            if (!val || !val[0]) continue;
            const char *num = vname + 14;
            char *key = g_strdup(num);
            BonusPair *bp = g_hash_table_lookup(pairs, key);
            if (!bp) {
                bp = g_new0(BonusPair, 1);
                g_hash_table_insert(pairs, key, bp);
            } else {
                g_free(key);
            }
            free(bp->path);
            bp->path = strdup(val);
        } else if (strncasecmp(vname, "randomizerWeight", 16) == 0) {
            const char *num = vname + 16;
            float w = 0;
            if (dbr->vars[i].type == TQ_VAR_INT && dbr->vars[i].count > 0)
                w = (float)dbr->vars[i].value.i32[0];
            else if (dbr->vars[i].type == TQ_VAR_FLOAT && dbr->vars[i].count > 0)
                w = dbr->vars[i].value.f32[0];
            if (w <= 0) continue;

            char *key = g_strdup(num);
            BonusPair *bp = g_hash_table_lookup(pairs, key);
            if (!bp) {
                bp = g_new0(BonusPair, 1);
                g_hash_table_insert(pairs, key, bp);
            } else {
                g_free(key);
            }
            bp->weight = w;
        }
    }

    /* Collect valid entries into an array */
    typedef struct { char *path; char *translation; float weight; } BonusEntry;
    int capacity = g_hash_table_size(pairs);
    BonusEntry *entries = calloc(capacity, sizeof(BonusEntry));
    int count = 0;
    float total_weight = 0;

    GHashTableIter iter;
    gpointer gkey, gval;
    g_hash_table_iter_init(&iter, pairs);
    while (g_hash_table_iter_next(&iter, &gkey, &gval)) {
        BonusPair *bp = gval;
        if (!bp->path || bp->weight <= 0) {
            free(bp->path);
            bp->path = NULL;
            continue;
        }

        /* Deduplicate: sum weights for same path */
        bool found_dup = false;
        for (int j = 0; j < count; j++) {
            if (strcasecmp(entries[j].path, bp->path) == 0) {
                entries[j].weight += bp->weight;
                total_weight += bp->weight;
                found_dup = true;
                break;
            }
        }
        if (found_dup) {
            free(bp->path);
            bp->path = NULL;
            continue;
        }

        /* Resolve display name */
        char *translation = NULL;
        TQArzRecordData *bonus_dbr = asset_get_dbr(bp->path);
        if (bonus_dbr) {
            char *tag = arz_record_get_string(bonus_dbr, "description", NULL);
            if (tag && tag[0] && tr) {
                const char *trans = translation_get(tr, tag);
                if (trans && trans[0]) translation = strdup(trans);
            }
            free(tag);
            if (!translation) {
                char *tag2 = arz_record_get_string(bonus_dbr, "lootRandomizerName", NULL);
                if (tag2 && tag2[0] && tr) {
                    const char *trans = translation_get(tr, tag2);
                    if (trans && trans[0]) translation = strdup(trans);
                }
                free(tag2);
            }
            if (!translation) {
                char *fdesc = arz_record_get_string(bonus_dbr, "FileDescription", NULL);
                if (fdesc && fdesc[0])
                    translation = fdesc;
                else
                    free(fdesc);
            }
        }
        if (!translation) {
            /* Fallback: generate stat summary from the bonus record */
            translation = item_bonus_stat_summary(bp->path);
        }
        if (!translation) {
            /* Last resort: extract filename */
            const char *last_sep = strrchr(bp->path, '\\');
            const char *fname = last_sep ? last_sep + 1 : bp->path;
            translation = strdup(fname);
        }

        entries[count].path = bp->path;
        entries[count].translation = translation;
        entries[count].weight = bp->weight;
        total_weight += bp->weight;
        bp->path = NULL;  /* ownership transferred */
        count++;
    }

    /* Clean up remaining paths */
    g_hash_table_iter_init(&iter, pairs);
    while (g_hash_table_iter_next(&iter, &gkey, &gval)) {
        BonusPair *bp = gval;
        free(bp->path);
        bp->path = NULL;
    }
    g_hash_table_destroy(pairs);

    if (count == 0) {
        free(entries);
        return NULL;
    }

    /* Sort by translation name */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcasecmp(entries[i].translation, entries[j].translation) > 0) {
                BonusEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    /* Build the GMenu */
    GMenu *menu = g_menu_new();
    for (int i = 0; i < count; i++) {
        bool is_current = current_bonus && strcasecmp(current_bonus, entries[i].path) == 0;
        float pct = total_weight > 0 ? (entries[i].weight / total_weight) * 100.0f : 0;

        /* Check for siblings with same translation */
        bool has_sibling =
            (i > 0 && strcasecmp(entries[i].translation, entries[i-1].translation) == 0) ||
            (i + 1 < count && strcasecmp(entries[i].translation, entries[i+1].translation) == 0);

        char label[256];
        if (has_sibling) {
            const char *last_sep = strrchr(entries[i].path, '\\');
            const char *fname = last_sep ? last_sep + 1 : entries[i].path;
            int flen = (int)strlen(fname);
            if (flen > 4) flen -= 4;  /* strip .dbr */
            snprintf(label, sizeof(label), "%s%s [%.*s] (%.0f%%)",
                     is_current ? "\u2022 " : "", entries[i].translation, flen, fname, pct);
        } else {
            snprintf(label, sizeof(label), "%s%s (%.0f%%)",
                     is_current ? "\u2022 " : "", entries[i].translation, pct);
        }

        char action[512];
        snprintf(action, sizeof(action), "%s::%s", action_name, entries[i].path);
        g_menu_append(menu, label, action);

        free(entries[i].path);
        free(entries[i].translation);
    }
    free(entries);
    return menu;
}

/* ── Context menu display ──────────────────────────────────────────────── */

/* Show the right-click context menu on a drawing area at (x, y). */
void show_item_context_menu(AppWidgets *widgets, GtkWidget *drawing_area,
                            TQVaultItem *item, TQItem *equip_item,
                            ContainerType source, int sack_idx,
                            int equip_slot, double x, double y) {
    widgets->context_item = item;
    widgets->context_equip_item = equip_item;
    widgets->context_source = source;
    widgets->context_sack_idx = sack_idx;
    widgets->context_equip_slot = equip_slot;

    /* Rebuild menu model: clear all items then re-add */
    GMenu *model = widgets->context_menu_model;
    while (g_menu_model_get_n_items(G_MENU_MODEL(model)) > 0)
        g_menu_remove(model, 0);

    {
        GMenuItem *mi;
        mi = g_menu_item_new("Copy", "app.item-copy");
        g_menu_item_set_attribute(mi, "accel", "s", "c");
        g_menu_append_item(model, mi);
        g_object_unref(mi);

        mi = g_menu_item_new("Duplicate", "app.item-duplicate");
        g_menu_item_set_attribute(mi, "accel", "s", "d");
        g_menu_append_item(model, mi);
        g_object_unref(mi);

        mi = g_menu_item_new("Delete", "app.item-delete");
        g_menu_item_set_attribute(mi, "accel", "s", "<Shift>d");
        g_menu_append_item(model, mi);
        g_object_unref(mi);
    }

    if (item && item_is_stackable_type(item))
        g_menu_append(model, "Set Quantity...", "app.set-stack-quantity");

    /* Check for relic slot 1 */
    const char *rn1 = equip_item ? equip_item->relic_name
                                 : (item ? item->relic_name : NULL);
    if (rn1 && rn1[0]) {
        const char *type = relic_type_label_ui(rn1);
        char label[64];
        snprintf(label, sizeof(label), "Remove %s", type);
        g_menu_append(model, label, "app.item-remove-relic");
    }

    /* Check for relic slot 2 */
    const char *rn2 = equip_item ? equip_item->relic_name2
                                 : (item ? item->relic_name2 : NULL);
    if (rn2 && rn2[0]) {
        const char *type = relic_type_label_ui(rn2);
        char label[64];
        snprintf(label, sizeof(label), "Remove Second %s", type);
        g_menu_append(model, label, "app.item-remove-relic2");
    }

    /* Affix modification dialog for eligible items */
    const char *base = equip_item ? equip_item->base_name
                                  : (item ? item->base_name : NULL);
    if (base && item_can_modify_affixes(base))
        g_menu_append(model, "Modify Affixes\u2026", "app.modify-affixes");

    /* Completion Bonus submenus */
    {
        /* Case 1: Standalone relic/charm/artifact -- base_name IS the relic/charm/artifact */
        if (base && (item_is_relic_or_charm(base) || item_is_artifact(base))) {
            const char *cur_bonus = equip_item ? equip_item->relic_bonus
                                               : (item ? item->relic_bonus : NULL);
            uint32_t shard_count = equip_item ? equip_item->var1
                                              : (item ? item->var1 : 0);
            /* Artifacts are always "complete" -- they don't have shard mechanics.
             * Relics/charms require var1 >= max shards to be considered complete. */
            bool is_complete = item_is_artifact(base) ||
                               (int)shard_count >= relic_max_shards(base);
            if (is_complete) {
                const char *table = get_bonus_table_path(base);
                GMenu *bonus_menu = build_bonus_submenu(table, cur_bonus,
                                                         "app.set-relic-bonus",
                                                         widgets->translations);
                if (bonus_menu) {
                    g_menu_append_submenu(model, "Completion Bonus",
                                          G_MENU_MODEL(bonus_menu));
                    g_object_unref(bonus_menu);
                }
            }
        }

        /* Case 2: Equipped relic/charm in slot 1 */
        if (rn1 && rn1[0] && (item_is_relic_or_charm(rn1) || item_is_artifact(rn1))) {
            const char *cur_bonus1 = equip_item ? equip_item->relic_bonus
                                                : (item ? item->relic_bonus : NULL);
            uint32_t shard1 = equip_item ? equip_item->var1
                                         : (item ? item->var1 : 0);
            bool slot1_complete = (cur_bonus1 && cur_bonus1[0]) ||
                                  (int)shard1 >= relic_max_shards(rn1);
            /* Only show for socketed relics, not standalone (already handled above) */
            if (slot1_complete && base && !item_is_relic_or_charm(base) && !item_is_artifact(base)) {
                const char *table1 = get_bonus_table_path(rn1);
                GMenu *bonus_menu1 = build_bonus_submenu(table1, cur_bonus1,
                                                          "app.set-relic-bonus",
                                                          widgets->translations);
                if (bonus_menu1) {
                    g_menu_append_submenu(model, "Completion Bonus",
                                          G_MENU_MODEL(bonus_menu1));
                    g_object_unref(bonus_menu1);
                }
            }
        }

        /* Case 3: Equipped relic/charm in slot 2 */
        if (rn2 && rn2[0] && (item_is_relic_or_charm(rn2) || item_is_artifact(rn2))) {
            const char *cur_bonus2 = equip_item ? equip_item->relic_bonus2
                                                : (item ? item->relic_bonus2 : NULL);
            uint32_t shard2 = equip_item ? equip_item->var2
                                         : (item ? item->var2 : 0);
            bool slot2_complete = (cur_bonus2 && cur_bonus2[0]) ||
                                  (int)shard2 >= relic_max_shards(rn2);
            if (slot2_complete && base && !item_is_relic_or_charm(base) && !item_is_artifact(base)) {
                const char *table2 = get_bonus_table_path(rn2);
                GMenu *bonus_menu2 = build_bonus_submenu(table2, cur_bonus2,
                                                          "app.set-relic-bonus2",
                                                          widgets->translations);
                if (bonus_menu2) {
                    g_menu_append_submenu(model, "Second Completion Bonus",
                                          G_MENU_MODEL(bonus_menu2));
                    g_object_unref(bonus_menu2);
                }
            }
        }
    }

    /* Copy DBR Path option */
    if (base && base[0])
        g_menu_append(model, "Copy DBR Path", "app.copy-dbr-path");

    /* Re-parent the popover to the clicked drawing area */
    if (widgets->context_parent != drawing_area) {
        if (widgets->context_parent)
            gtk_widget_unparent(widgets->context_menu);
        gtk_widget_set_parent(widgets->context_menu, drawing_area);
        widgets->context_parent = drawing_area;
    }

    if (widgets->tooltip_popover)
        gtk_widget_set_visible(widgets->tooltip_popover, FALSE);

    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(widgets->context_menu), &rect);
    gtk_popover_popup(GTK_POPOVER(widgets->context_menu));
}

/* ── Action callbacks ──────────────────────────────────────────────────── */

static void on_copy_dbr_path(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;
    const char *base = NULL;
    if (widgets->context_equip_item)
        base = widgets->context_equip_item->base_name;
    else if (widgets->context_item)
        base = widgets->context_item->base_name;
    if (base && base[0]) {
        char *copy = strdup(base);
        for (char *p = copy; *p; p++)
            if (*p == '\\') *p = '/';
        GdkDisplay *display = gdk_display_get_default();
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        gdk_clipboard_set_text(clipboard, copy);
        free(copy);
    }
}

static void on_item_copy(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;
    if (widgets->held_item) return;
    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        copy_equip_to_cursor(widgets, widgets->context_equip_item, true);
    } else if (widgets->context_item) {
        copy_item_to_cursor(widgets, widgets->context_item, true);
    }
}

static void on_item_duplicate(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;
    if (widgets->held_item) return;
    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        copy_equip_to_cursor(widgets, widgets->context_equip_item, false);
    } else if (widgets->context_item) {
        copy_item_to_cursor(widgets, widgets->context_item, false);
    }
}

static void on_item_delete(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;
    if (widgets->held_item) return;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        /* Delete equipment item */
        int slot = widgets->context_equip_slot;
        if (widgets->current_character && slot >= 0 && slot < 12 &&
            widgets->current_character->equipment[slot]) {
            TQItem *eq = widgets->current_character->equipment[slot];
            free(eq->base_name);
            free(eq->prefix_name);
            free(eq->suffix_name);
            free(eq->relic_name);
            free(eq->relic_bonus);
            free(eq->relic_name2);
            free(eq->relic_bonus2);
            free(eq);
            widgets->current_character->equipment[slot] = NULL;
            widgets->char_dirty = true;
        }
    } else if (widgets->context_item) {
        /* Delete sack item: find it in the sack and remove */
        TQVaultSack *sack = NULL;
        if (widgets->context_source == CONTAINER_VAULT &&
            widgets->current_vault &&
            widgets->context_sack_idx >= 0 &&
            widgets->context_sack_idx < widgets->current_vault->num_sacks) {
            sack = &widgets->current_vault->sacks[widgets->context_sack_idx];
            widgets->vault_dirty = true;
        } else if (widgets->context_source == CONTAINER_INV &&
                   widgets->current_character &&
                   widgets->current_character->num_inv_sacks > 0) {
            sack = &widgets->current_character->inv_sacks[0];
            widgets->char_dirty = true;
        } else if (widgets->context_source == CONTAINER_BAG &&
                   widgets->current_character) {
            int idx = 1 + widgets->context_sack_idx;
            if (idx < widgets->current_character->num_inv_sacks) {
                sack = &widgets->current_character->inv_sacks[idx];
                widgets->char_dirty = true;
            }
        }
        if (sack) {
            for (int i = 0; i < sack->num_items; i++) {
                if (&sack->items[i] == widgets->context_item) {
                    vault_item_free_strings(&sack->items[i]);
                    if (i < sack->num_items - 1)
                        memmove(&sack->items[i], &sack->items[i + 1],
                                (size_t)(sack->num_items - 1 - i) * sizeof(TQVaultItem));
                    sack->num_items--;
                    break;
                }
            }
        }
    }
    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Remove relic from slot 1 of the right-clicked item and place it on the cursor. */
static void on_item_remove_relic(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;
    if (widgets->held_item) return;

    const char *relic_name = NULL;
    const char *relic_bonus = NULL;
    uint32_t var1 = 0;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        if (!eq->relic_name || !eq->relic_name[0]) return;
        relic_name = eq->relic_name;
        relic_bonus = eq->relic_bonus;
        var1 = eq->var1;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        if (!it->relic_name || !it->relic_name[0]) return;
        relic_name = it->relic_name;
        relic_bonus = it->relic_bonus;
        var1 = it->var1;
    } else {
        return;
    }

    /* Build a HeldItem for the extracted relic */
    HeldItem *hi = calloc(1, sizeof(HeldItem));
    hi->item.base_name = strdup(relic_name);
    hi->item.relic_bonus = safe_strdup(relic_bonus);
    hi->item.seed = (uint32_t)(rand() % 0x7fff);
    hi->item.var1 = var1;
    hi->item.stack_size = 1;
    hi->source = widgets->context_source;
    hi->source_sack_idx = widgets->context_sack_idx;
    hi->source_equip_slot = -1;
    hi->is_copy = false;
    get_item_dims(widgets, &hi->item, &hi->item_w, &hi->item_h);
    hi->texture = load_item_texture(widgets, hi->item.base_name, hi->item.var1);
    widgets->held_item = hi;

    /* Clear relic slot 1 from the source item */
    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->relic_name);  eq->relic_name = NULL;
        free(eq->relic_bonus); eq->relic_bonus = NULL;
        eq->var1 = 0;
        widgets->char_dirty = true;
    } else {
        TQVaultItem *it = widgets->context_item;
        free(it->relic_name);  it->relic_name = NULL;
        free(it->relic_bonus); it->relic_bonus = NULL;
        it->var1 = 0;
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }

    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Remove relic from slot 2 of the right-clicked item and place it on the cursor. */
static void on_item_remove_relic2(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;
    if (widgets->held_item) return;

    const char *relic_name = NULL;
    const char *relic_bonus = NULL;
    uint32_t var2 = 0;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        if (!eq->relic_name2 || !eq->relic_name2[0]) return;
        relic_name = eq->relic_name2;
        relic_bonus = eq->relic_bonus2;
        var2 = eq->var2;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        if (!it->relic_name2 || !it->relic_name2[0]) return;
        relic_name = it->relic_name2;
        relic_bonus = it->relic_bonus2;
        var2 = it->var2;
    } else {
        return;
    }

    /* Build a HeldItem for the extracted relic */
    HeldItem *hi = calloc(1, sizeof(HeldItem));
    hi->item.base_name = strdup(relic_name);
    hi->item.relic_bonus = safe_strdup(relic_bonus);
    hi->item.seed = (uint32_t)(rand() % 0x7fff);
    hi->item.var1 = var2;  /* shard count goes into var1 for the standalone item */
    hi->item.stack_size = 1;
    hi->source = widgets->context_source;
    hi->source_sack_idx = widgets->context_sack_idx;
    hi->source_equip_slot = -1;
    hi->is_copy = false;
    get_item_dims(widgets, &hi->item, &hi->item_w, &hi->item_h);
    hi->texture = load_item_texture(widgets, hi->item.base_name, hi->item.var1);
    widgets->held_item = hi;

    /* Clear relic slot 2 from the source item */
    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->relic_name2);  eq->relic_name2 = NULL;
        free(eq->relic_bonus2); eq->relic_bonus2 = NULL;
        eq->var2 = 0;
        widgets->char_dirty = true;
    } else {
        TQVaultItem *it = widgets->context_item;
        free(it->relic_name2);  it->relic_name2 = NULL;
        free(it->relic_bonus2); it->relic_bonus2 = NULL;
        it->var2 = 0;
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }

    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Set prefix on the right-clicked item */
static void on_set_prefix(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action;
    AppWidgets *widgets = data;
    const char *affix_path = g_variant_get_string(param, NULL);
    if (!affix_path || !affix_path[0]) return;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->prefix_name);
        eq->prefix_name = strdup(affix_path);
        widgets->char_dirty = true;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        free(it->prefix_name);
        it->prefix_name = strdup(affix_path);
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }
    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Remove prefix from the right-clicked item */
static void on_remove_prefix(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->prefix_name);
        eq->prefix_name = NULL;
        widgets->char_dirty = true;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        free(it->prefix_name);
        it->prefix_name = NULL;
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }
    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Set suffix on the right-clicked item */
static void on_set_suffix(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action;
    AppWidgets *widgets = data;
    const char *affix_path = g_variant_get_string(param, NULL);
    if (!affix_path || !affix_path[0]) return;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->suffix_name);
        eq->suffix_name = strdup(affix_path);
        widgets->char_dirty = true;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        free(it->suffix_name);
        it->suffix_name = strdup(affix_path);
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }
    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Remove suffix from the right-clicked item */
static void on_remove_suffix(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->suffix_name);
        eq->suffix_name = NULL;
        widgets->char_dirty = true;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        free(it->suffix_name);
        it->suffix_name = NULL;
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }
    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Set completion bonus on slot 1 (or standalone relic/charm/artifact) */
static void on_set_relic_bonus(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action;
    AppWidgets *widgets = data;
    const char *bonus_path = g_variant_get_string(param, NULL);
    if (!bonus_path || !bonus_path[0]) return;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->relic_bonus);
        eq->relic_bonus = strdup(bonus_path);
        widgets->char_dirty = true;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        free(it->relic_bonus);
        it->relic_bonus = strdup(bonus_path);
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }
    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* Set completion bonus on slot 2 */
static void on_set_relic_bonus2(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action;
    AppWidgets *widgets = data;
    const char *bonus_path = g_variant_get_string(param, NULL);
    if (!bonus_path || !bonus_path[0]) return;

    if (widgets->context_equip_slot >= 0 && widgets->context_equip_item) {
        TQItem *eq = widgets->context_equip_item;
        free(eq->relic_bonus2);
        eq->relic_bonus2 = strdup(bonus_path);
        widgets->char_dirty = true;
    } else if (widgets->context_item) {
        TQVaultItem *it = widgets->context_item;
        free(it->relic_bonus2);
        it->relic_bonus2 = strdup(bonus_path);
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
    }
    invalidate_tooltips(widgets);
    queue_redraw_equip(widgets);
}

/* ── Modify Affixes action (launches dialog) ───────────────────────────── */

static void on_modify_affixes(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)action; (void)param;
    AppWidgets *widgets = data;
    show_affix_dialog(widgets);
}

/* ── Set Quantity dialog ─────────────────────────────────────────────────── */

static void on_qty_scale_changed(GtkRange *range, gpointer user_data) {
    GtkWidget *label = GTK_WIDGET(user_data);
    GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(range), GTK_TYPE_WINDOW);
    int max_val  = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "max_val"));
    bool is_rel  = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "is_relic"));
    int val = (int)gtk_range_get_value(range);
    char buf[48];
    snprintf(buf, sizeof(buf), is_rel ? "Shards: %d / %d" : "Quantity: %d / %d",
             val, max_val);
    gtk_label_set_text(GTK_LABEL(label), buf);
}

static void on_set_qty_ok(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GtkWidget *dialog   = GTK_WIDGET(user_data);
    GtkWidget *scale    = g_object_get_data(G_OBJECT(dialog), "scale");
    AppWidgets *widgets = g_object_get_data(G_OBJECT(dialog), "widgets");
    bool is_relic       = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "is_relic"));

    int qty = (int)gtk_range_get_value(GTK_RANGE(scale));
    if (qty < 1) qty = 1;

    TQVaultItem *it = widgets->context_item;
    if (it && item_is_stackable_type(it)) {
        if (is_relic) {
            it->var1 = (uint32_t)qty;
            /* If no longer complete, strip the completion bonus */
            if (qty < relic_max_shards(it->base_name)) {
                free(it->relic_bonus);
                it->relic_bonus = NULL;
            }
        } else {
            it->stack_size = qty;
        }
        if (widgets->context_source == CONTAINER_VAULT)
            widgets->vault_dirty = true;
        else
            widgets->char_dirty = true;
        invalidate_tooltips(widgets);
        queue_redraw_equip(widgets);
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void on_set_stack_quantity(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action; (void)param;
    AppWidgets *widgets = user_data;
    TQVaultItem *it = widgets->context_item;
    if (!it || !item_is_stackable_type(it)) return;

    bool is_relic = item_is_relic_or_charm(it->base_name);
    int max_val   = is_relic ? relic_max_shards(it->base_name) : 99;
    int cur_val   = is_relic ? (int)it->var1 : it->stack_size;
    if (cur_val < 1)       cur_val = 1;
    if (cur_val > max_val) cur_val = max_val;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Set Quantity");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    /* Live value label: "Shards: 2 / 3" or "Quantity: 15 / 99" */
    char buf[48];
    snprintf(buf, sizeof(buf), is_relic ? "Shards: %d / %d" : "Quantity: %d / %d",
             cur_val, max_val);
    GtkWidget *label = gtk_label_new(buf);
    gtk_box_append(GTK_BOX(vbox), label);

    /* Slider */
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                1.0, (double)max_val, 1.0);
    gtk_range_set_value(GTK_RANGE(scale), (double)cur_val);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_size_request(scale, 220, -1);
    gtk_scale_add_mark(GTK_SCALE(scale), 1.0, GTK_POS_BOTTOM, "1");
    char mark_buf[8];
    snprintf(mark_buf, sizeof(mark_buf), "%d", max_val);
    gtk_scale_add_mark(GTK_SCALE(scale), (double)max_val, GTK_POS_BOTTOM, mark_buf);
    gtk_box_append(GTK_BOX(vbox), scale);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), hbox);

    GtkWidget *ok_btn     = gtk_button_new_with_label("OK");
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_box_append(GTK_BOX(hbox), ok_btn);
    gtk_box_append(GTK_BOX(hbox), cancel_btn);

    g_object_set_data(G_OBJECT(dialog), "scale",    scale);
    g_object_set_data(G_OBJECT(dialog), "widgets",  widgets);
    g_object_set_data(G_OBJECT(dialog), "is_relic", GINT_TO_POINTER(is_relic ? 1 : 0));
    g_object_set_data(G_OBJECT(dialog), "max_val",  GINT_TO_POINTER(max_val));

    g_signal_connect(scale, "value-changed", G_CALLBACK(on_qty_scale_changed), label);
    g_signal_connect_swapped(cancel_btn, "clicked",
                             G_CALLBACK(gtk_window_destroy), dialog);
    g_signal_connect(ok_btn, "clicked",
                     G_CALLBACK(on_set_qty_ok), dialog);

    gtk_window_present(GTK_WINDOW(dialog));
}

/* ── Action registration ───────────────────────────────────────────────── */

void register_context_actions(GtkApplication *app, AppWidgets *widgets) {
    struct { const char *name; GCallback cb; const GVariantType *type; } acts[] = {
        { "item-copy",            G_CALLBACK(on_item_copy),            NULL },
        { "item-duplicate",       G_CALLBACK(on_item_duplicate),       NULL },
        { "item-delete",          G_CALLBACK(on_item_delete),          NULL },
        { "item-remove-relic",    G_CALLBACK(on_item_remove_relic),    NULL },
        { "item-remove-relic2",   G_CALLBACK(on_item_remove_relic2),   NULL },
        { "set-prefix",           G_CALLBACK(on_set_prefix),           G_VARIANT_TYPE_STRING },
        { "remove-prefix",        G_CALLBACK(on_remove_prefix),        NULL },
        { "set-suffix",           G_CALLBACK(on_set_suffix),           G_VARIANT_TYPE_STRING },
        { "remove-suffix",        G_CALLBACK(on_remove_suffix),        NULL },
        { "modify-affixes",       G_CALLBACK(on_modify_affixes),       NULL },
        { "set-relic-bonus",      G_CALLBACK(on_set_relic_bonus),      G_VARIANT_TYPE_STRING },
        { "set-relic-bonus2",     G_CALLBACK(on_set_relic_bonus2),     G_VARIANT_TYPE_STRING },
        { "copy-dbr-path",        G_CALLBACK(on_copy_dbr_path),        NULL },
        { "set-stack-quantity",   G_CALLBACK(on_set_stack_quantity),    NULL },
    };

    for (size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        GSimpleAction *a = g_simple_action_new(acts[i].name, acts[i].type);
        g_signal_connect(a, "activate", acts[i].cb, widgets);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(a));
        g_object_unref(a);
    }
}
