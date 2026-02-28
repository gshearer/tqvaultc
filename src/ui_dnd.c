#include "ui.h"
#include "arz.h"
#include "asset_lookup.h"
#include "item_stats.h"
#include "affix_table.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Click-to-move helpers ──────────────────────────────────────────────── */

void vault_item_deep_copy(TQVaultItem *dst, const TQVaultItem *src) {
    *dst = *src;
    dst->base_name   = safe_strdup(src->base_name);
    dst->prefix_name = safe_strdup(src->prefix_name);
    dst->suffix_name = safe_strdup(src->suffix_name);
    dst->relic_name  = safe_strdup(src->relic_name);
    dst->relic_bonus = safe_strdup(src->relic_bonus);
    dst->relic_name2 = safe_strdup(src->relic_name2);
    dst->relic_bonus2= safe_strdup(src->relic_bonus2);
}

void sack_add_item(TQVaultSack *sack, const TQVaultItem *item) {
    sack->items = realloc(sack->items, (size_t)(sack->num_items + 1) * sizeof(TQVaultItem));
    sack->items[sack->num_items] = *item;
    sack->num_items++;
}

static bool str_empty(const char *s) { return !s || !s[0]; }

bool items_stackable(const TQVaultItem *a, const TQVaultItem *b) {
    if (!a->base_name || !b->base_name) return false;
    if (strcasecmp(a->base_name, b->base_name) != 0) return false;
    /* Stackable items are always plain (no prefix/suffix/relic) */
    if (!str_empty(a->prefix_name) || !str_empty(b->prefix_name)) return false;
    if (!str_empty(a->suffix_name) || !str_empty(b->suffix_name)) return false;
    if (!str_empty(a->relic_name)  || !str_empty(b->relic_name))  return false;
    if (!str_empty(a->relic_name2) || !str_empty(b->relic_name2)) return false;
    return true;
}

void equip_to_vault_item(TQVaultItem *vi, const TQItem *eq) {
    memset(vi, 0, sizeof(*vi));
    vi->seed        = eq->seed;
    vi->base_name   = safe_strdup(eq->base_name);
    vi->prefix_name = safe_strdup(eq->prefix_name);
    vi->suffix_name = safe_strdup(eq->suffix_name);
    vi->relic_name  = safe_strdup(eq->relic_name);
    vi->relic_bonus = safe_strdup(eq->relic_bonus);
    vi->relic_name2 = safe_strdup(eq->relic_name2);
    vi->relic_bonus2= safe_strdup(eq->relic_bonus2);
    vi->var1        = eq->var1;
    vi->var2        = eq->var2;
    vi->stack_size  = 1;
}

void vault_item_to_equip(TQItem *eq, const TQVaultItem *vi) {
    memset(eq, 0, sizeof(*eq));
    eq->seed        = vi->seed;
    eq->base_name   = safe_strdup(vi->base_name);
    eq->prefix_name = safe_strdup(vi->prefix_name);
    eq->suffix_name = safe_strdup(vi->suffix_name);
    eq->relic_name  = safe_strdup(vi->relic_name);
    eq->relic_bonus = safe_strdup(vi->relic_bonus);
    eq->relic_name2 = safe_strdup(vi->relic_name2);
    eq->relic_bonus2= safe_strdup(vi->relic_bonus2);
    eq->var1        = vi->var1;
    eq->var2        = vi->var2;
}

/* Build a boolean occupancy grid for a sack, excluding a specific item pointer
 * (so we can test placement where the held item used to be). */
bool* build_occupancy_grid(AppWidgets *widgets, TQVaultSack *sack,
                                   int cols, int rows, TQVaultItem *exclude) {
    bool *grid = calloc((size_t)(cols * rows), sizeof(bool));
    if (!sack) return grid;
    for (int i = 0; i < sack->num_items; i++) {
        TQVaultItem *it = &sack->items[i];
        if (it == exclude || !it->base_name) continue;
        int w = it->width > 0 ? it->width : 1;
        int h = it->height > 0 ? it->height : 1;
        GdkPixbuf *pixbuf = load_item_texture(widgets, it->base_name, it->var1);
        if (pixbuf) {
            w = gdk_pixbuf_get_width(pixbuf) / 32;
            h = gdk_pixbuf_get_height(pixbuf) / 32;
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            g_object_unref(pixbuf);
        }
        for (int dy = 0; dy < h; dy++)
            for (int dx = 0; dx < w; dx++) {
                int gx = it->point_x + dx, gy = it->point_y + dy;
                if (gx >= 0 && gx < cols && gy >= 0 && gy < rows)
                    grid[gy * cols + gx] = true;
            }
    }
    return grid;
}

bool can_place_item(const bool *grid, int cols, int rows,
                            int x, int y, int w, int h) {
    if (x < 0 || y < 0 || x + w > cols || y + h > rows) return false;
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            if (grid[(y + dy) * cols + (x + dx)]) return false;
    return true;
}

void free_held_item(AppWidgets *widgets) {
    if (!widgets->held_item) return;
    vault_item_free_strings(&widgets->held_item->item);
    if (widgets->held_item->texture)
        g_object_unref(widgets->held_item->texture);
    free(widgets->held_item);
    widgets->held_item = NULL;
}

/* Return held item to its source container (or discard if it's a copy). */
void cancel_held_item(AppWidgets *widgets) {
    if (!widgets->held_item) return;
    HeldItem *hi = widgets->held_item;
    if (hi->is_copy) {
        free_held_item(widgets);
        invalidate_tooltips(widgets);
        queue_redraw_all(widgets);
        return;
    }
    switch (hi->source) {
    case CONTAINER_VAULT:
        if (widgets->current_vault &&
            hi->source_sack_idx >= 0 &&
            hi->source_sack_idx < widgets->current_vault->num_sacks) {
            TQVaultItem copy;
            vault_item_deep_copy(&copy, &hi->item);
            sack_add_item(&widgets->current_vault->sacks[hi->source_sack_idx], &copy);
        }
        break;
    case CONTAINER_INV:
        if (widgets->current_character && widgets->current_character->num_inv_sacks > 0) {
            TQVaultItem copy;
            vault_item_deep_copy(&copy, &hi->item);
            sack_add_item(&widgets->current_character->inv_sacks[0], &copy);
        }
        break;
    case CONTAINER_BAG:
        if (widgets->current_character) {
            int idx = 1 + hi->source_sack_idx;
            if (idx < widgets->current_character->num_inv_sacks) {
                TQVaultItem copy;
                vault_item_deep_copy(&copy, &hi->item);
                sack_add_item(&widgets->current_character->inv_sacks[idx], &copy);
            }
        }
        break;
    case CONTAINER_EQUIP:
        if (widgets->current_character &&
            hi->source_equip_slot >= 0 && hi->source_equip_slot < 12 &&
            !widgets->current_character->equipment[hi->source_equip_slot]) {
            TQItem *eq = calloc(1, sizeof(TQItem));
            vault_item_to_equip(eq, &hi->item);
            widgets->current_character->equipment[hi->source_equip_slot] = eq;
        }
        break;
    default:
        break;
    }
    free_held_item(widgets);
    invalidate_tooltips(widgets);
    queue_redraw_all(widgets);
}

/* Gear type flags for relic/charm equipment compatibility */
enum {
    GEAR_HEAD     = 1 << 0,
    GEAR_TORSO    = 1 << 1,
    GEAR_ARM      = 1 << 2,
    GEAR_LEG      = 1 << 3,
    GEAR_RING     = 1 << 4,
    GEAR_AMULET   = 1 << 5,
    GEAR_SHIELD   = 1 << 6,
    GEAR_SWORD    = 1 << 7,
    GEAR_AXE      = 1 << 8,
    GEAR_MACE     = 1 << 9,
    GEAR_SPEAR    = 1 << 10,
    GEAR_BOW      = 1 << 11,
    GEAR_STAFF    = 1 << 12,
    GEAR_THROWN   = 1 << 13,

    GEAR_JEWELLERY   = (1 << 4) | (1 << 5),              /* RING | AMULET */
    GEAR_ALL_ARMOR   = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3),
    GEAR_ALL_WEAPONS = (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10)
                     | (1 << 11) | (1 << 12) | (1 << 13),
};

/* Parse the relic/charm's "itemText" translation to determine which gear types
 * it can enchant.  Returns a bitmask of GEAR_* flags.
 * Returns 0xFFFFFFFF (all bits set) if the field is missing or unparseable,
 * so that unknown relics fail-open rather than blocking legitimate socketing. */
static uint32_t relic_allowed_gear(const char *relic_base_name, TQTranslation *tr) {
    if (!relic_base_name || !tr) return 0xFFFFFFFF;

    const char *tag = dbr_get_string(relic_base_name, "itemText");
    if (!tag || !tag[0]) return 0xFFFFFFFF;

    const char *text = translation_get(tr, tag);
    if (!text || !text[0]) return 0xFFFFFFFF;

    /* Find "Can enchant" or "Can enhance" (case-insensitive) */
    const char *p = text;
    const char *found = NULL;
    while (*p) {
        if (strncasecmp(p, "Can enchant", 11) == 0) { found = p + 11; break; }
        if (strncasecmp(p, "Can enhance", 11) == 0) { found = p + 11; break; }
        p++;
    }
    if (!found) return 0xFFFFFFFF;

    /* Keyword table: substring → gear flag(s).
     * Order matters: longer/more-specific keywords before shorter ones to avoid
     * partial matches (e.g. "armband" before "arm"). */
    static const struct { const char *kw; uint32_t flags; } kw_table[] = {
        { "weapon",    GEAR_ALL_WEAPONS },
        { "armband",   GEAR_ARM },
        { "bracelet",  GEAR_ARM },
        { "forearm",   GEAR_ARM },
        { "armor",     GEAR_ALL_ARMOR },
        { "armour",    GEAR_ALL_ARMOR },
        { "jewellery", GEAR_JEWELLERY },
        { "jewelry",   GEAR_JEWELLERY },
        { "head",      GEAR_HEAD },
        { "helm",      GEAR_HEAD },
        { "torso",     GEAR_TORSO },
        { "leg",       GEAR_LEG },
        { "greave",    GEAR_LEG },
        { "boot",      GEAR_LEG },
        { "legging",   GEAR_LEG },
        { "ring",      GEAR_RING },
        { "amulet",    GEAR_AMULET },
        { "necklace",  GEAR_AMULET },
        { "pendant",   GEAR_AMULET },
        { "shield",    GEAR_SHIELD },
        { "sword",     GEAR_SWORD },
        { "blade",     GEAR_SWORD },
        { "axe",       GEAR_AXE },
        { "mace",      GEAR_MACE },
        { "club",      GEAR_MACE },
        { "spear",     GEAR_SPEAR },
        { "lance",     GEAR_SPEAR },
        { "bow",       GEAR_BOW },
        { "staff",     GEAR_STAFF },
        { "stave",     GEAR_STAFF },
        { "thrown",    GEAR_THROWN },
        { "piercing",  GEAR_BOW | GEAR_SPEAR | GEAR_SWORD | GEAR_THROWN },
    };

    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof(kw_table) / sizeof(kw_table[0]); i++) {
        if (strcasestr(found, kw_table[i].kw))
            mask |= kw_table[i].flags;
    }

    return mask ? mask : 0xFFFFFFFF;
}

/* Map an item's DBR "Class" field to a single GEAR_* flag.
 * Returns 0 if the class is unknown (will pass any compatibility check). */
static uint32_t item_gear_type(const char *base_name) {
    const char *cls = dbr_get_string(base_name, "Class");
    if (!cls) return 0;

    static const struct { const char *cls; uint32_t flag; } class_map[] = {
        { "ArmorProtective_Head",      GEAR_HEAD },
        { "ArmorProtective_UpperBody", GEAR_TORSO },
        { "ArmorProtective_Forearm",   GEAR_ARM },
        { "ArmorProtective_LowerBody", GEAR_LEG },
        { "ArmorJewelry_Ring",         GEAR_RING },
        { "ArmorJewelry_Amulet",       GEAR_AMULET },
        { "WeaponArmor_Shield",        GEAR_SHIELD },
        { "WeaponMelee_Sword",         GEAR_SWORD },
        { "WeaponMelee_Axe",           GEAR_AXE },
        { "WeaponMelee_Mace",          GEAR_MACE },
        { "WeaponHunting_Spear",       GEAR_SPEAR },
        { "WeaponHunting_Bow",         GEAR_BOW },
        { "WeaponMagical_Staff",       GEAR_STAFF },
        { "WeaponHunting_RangedOneHand", GEAR_THROWN },
    };

    for (size_t i = 0; i < sizeof(class_map) / sizeof(class_map[0]); i++) {
        if (strcasecmp(cls, class_map[i].cls) == 0)
            return class_map[i].flag;
    }
    return 0;
}

/* Returns the first available relic socket slot (1 or 2) for a sack/inventory
 * item, or 0 if the item cannot accept a relic/charm.
 * relic_base_name and tr are used for gear-type compatibility checking. */
int item_can_accept_relic_sack(const TQVaultItem *it,
                                      const char *relic_base_name,
                                      TQTranslation *tr) {
    if (!it || !it->base_name) return 0;
    /* Relics/charms/artifacts cannot receive other relics */
    if (item_is_relic_or_charm(it->base_name)) return 0;
    if (item_is_artifact(it->base_name)) return 0;
    /* Epic/Legendary items are non-moddable */
    if (!item_can_modify_affixes(it->base_name)) return 0;
    /* Gear-type compatibility: relic must allow this item's equipment type */
    uint32_t allowed = relic_allowed_gear(relic_base_name, tr);
    uint32_t target_type = item_gear_type(it->base_name);
    if (target_type && !(allowed & target_type)) return 0;
    /* Check slots */
    if (str_empty(it->relic_name)) return 1;
    if (item_has_two_relic_slots(it->suffix_name) && str_empty(it->relic_name2)) {
        /* Slot 2: reject if the same relic/charm is already in slot 1.
         * Different tiers (e.g. 02_ vs 03_) of the same relic are allowed. */
        if (relic_base_name && strcasecmp(it->relic_name, relic_base_name) == 0)
            return 0;
        return 2;
    }
    return 0;
}

/* Same check for equipment-panel items (TQItem). */
int item_can_accept_relic_equip(const TQItem *eq,
                                       const char *relic_base_name,
                                       TQTranslation *tr) {
    if (!eq || !eq->base_name) return 0;
    if (item_is_relic_or_charm(eq->base_name)) return 0;
    if (item_is_artifact(eq->base_name)) return 0;
    if (!item_can_modify_affixes(eq->base_name)) return 0;
    /* Gear-type compatibility */
    uint32_t allowed = relic_allowed_gear(relic_base_name, tr);
    uint32_t target_type = item_gear_type(eq->base_name);
    if (target_type && !(allowed & target_type)) return 0;
    if (!eq->relic_name || !eq->relic_name[0]) return 1;
    if (item_has_two_relic_slots(eq->suffix_name) &&
        (!eq->relic_name2 || !eq->relic_name2[0])) {
        /* Reject duplicate relic/charm in slot 2 (same path = same item) */
        if (relic_base_name && strcasecmp(eq->relic_name, relic_base_name) == 0)
            return 0;
        return 2;
    }
    return 0;
}

/* ── Click-to-move: pick up / place item in a sack grid ─────────────────── */

/* Hit-test a sack grid and return the item at pixel (px, py), or NULL.
 * If out_idx is non-NULL, stores the index into sack->items[]. */
TQVaultItem *find_item_at_cell(AppWidgets *widgets, TQVaultSack *sack,
                                       int cols, int rows, double cell,
                                       double px, double py, int *out_idx) {
    int col = (int)(px / cell);
    int row = (int)(py / cell);
    if (col < 0 || col >= cols || row < 0 || row >= rows) return NULL;
    for (int i = 0; i < sack->num_items; i++) {
        TQVaultItem *it = &sack->items[i];
        if (!it->base_name) continue;
        int w, h;
        get_item_dims(widgets, it, &w, &h);
        if (col >= it->point_x && col < it->point_x + w &&
            row >= it->point_y && row < it->point_y + h) {
            if (out_idx) *out_idx = i;
            return it;
        }
    }
    return NULL;
}

static void pick_up_from_sack(AppWidgets *widgets, TQVaultSack *sack,
                               ContainerType ctype, int sack_idx,
                               int cols, int rows, double cell, double px, double py) {
    int hit_idx = -1;
    TQVaultItem *hit = find_item_at_cell(widgets, sack, cols, rows, cell, px, py, &hit_idx);
    if (!hit) return;

    /* Create held item */
    HeldItem *hi = calloc(1, sizeof(HeldItem));
    vault_item_deep_copy(&hi->item, hit);
    hi->source = ctype;
    hi->source_sack_idx = sack_idx;
    get_item_dims(widgets, hit, &hi->item_w, &hi->item_h);
    hi->texture = load_item_texture(widgets, hit->base_name, hit->var1);

    /* Remove from sack (shift items, don't free strings since we copied them) */
    if (hit_idx < sack->num_items - 1)
        memmove(&sack->items[hit_idx], &sack->items[hit_idx + 1],
                (size_t)(sack->num_items - 1 - hit_idx) * sizeof(TQVaultItem));
    sack->num_items--;

    widgets->held_item = hi;
    invalidate_tooltips(widgets);
    queue_redraw_all(widgets);
}

static void place_in_sack(AppWidgets *widgets, TQVaultSack *sack,
                           ContainerType ctype, int sack_idx,
                           int cols, int rows, double cell, double px, double py) {
    HeldItem *hi = widgets->held_item;
    ContainerType held_source = hi->source; /* capture before potential free */
    int col = (int)(px / cell);
    int row = (int)(py / cell);
    /* Centre item on clicked cell */
    int place_x = col - hi->item_w / 2;
    int place_y = row - hi->item_h / 2;

    /* Check if we're clicking on an existing item → swap */
    TQVaultItem *target = NULL;
    int target_idx = -1;
    for (int i = 0; i < sack->num_items; i++) {
        TQVaultItem *it = &sack->items[i];
        if (!it->base_name) continue;
        int w, h;
        get_item_dims(widgets, it, &w, &h);
        if (col >= it->point_x && col < it->point_x + w &&
            row >= it->point_y && row < it->point_y + h) {
            target = it;
            target_idx = i;
            break;
        }
    }

    if (target) {
        /* Socket relic/charm into target item if compatible */
        if (item_is_relic_or_charm(hi->item.base_name)) {
            int slot = item_can_accept_relic_sack(target, hi->item.base_name, widgets->translations);
            if (slot != 0) {
                if (slot == 1) {
                    free(target->relic_name);
                    free(target->relic_bonus);
                    target->relic_name  = safe_strdup(hi->item.base_name);
                    target->relic_bonus = safe_strdup(hi->item.relic_bonus);
                    target->var1        = hi->item.var1;
                } else {
                    free(target->relic_name2);
                    free(target->relic_bonus2);
                    target->relic_name2  = safe_strdup(hi->item.base_name);
                    target->relic_bonus2 = safe_strdup(hi->item.relic_bonus);
                    target->var2         = hi->item.var1;
                }
                free_held_item(widgets);
                goto done;
            }
        }

        /* Stack merge: if held item and target are stackable-compatible, merge */
        if (items_stackable(&hi->item, target)) {
            if (item_is_relic_or_charm(target->base_name)) {
                int max_s = relic_max_shards(target->base_name);
                int combined = (int)target->var1 + (int)hi->item.var1;
                if (combined > max_s) {
                    target->var1 = (uint32_t)max_s;
                    hi->item.var1 = (uint32_t)(combined - max_s);
                    /* Keep remainder on cursor — don't free */
                } else {
                    target->var1 = (uint32_t)combined;
                    free_held_item(widgets);
                }
            } else {
                target->stack_size += hi->item.stack_size;
                free_held_item(widgets);
            }
            goto done;
        }
        /* Swap: pick up target, place held at target's position */
        TQVaultItem target_copy;
        vault_item_deep_copy(&target_copy, target);
        int tw, th;
        get_item_dims(widgets, target, &tw, &th);

        /* Remove target from sack */
        if (target_idx < sack->num_items - 1)
            memmove(&sack->items[target_idx], &sack->items[target_idx + 1],
                    (size_t)(sack->num_items - 1 - target_idx) * sizeof(TQVaultItem));
        sack->num_items--;

        /* Place held item at target's position */
        TQVaultItem place_copy;
        vault_item_deep_copy(&place_copy, &hi->item);
        place_copy.point_x = target_copy.point_x;
        place_copy.point_y = target_copy.point_y;
        sack_add_item(sack, &place_copy);

        /* Update held item to be the swapped target */
        vault_item_free_strings(&hi->item);
        vault_item_deep_copy(&hi->item, &target_copy);
        hi->item_w = tw;
        hi->item_h = th;
        if (hi->texture) g_object_unref(hi->texture);
        hi->texture = load_item_texture(widgets, target_copy.base_name, target_copy.var1);
        hi->source = ctype;
        hi->source_sack_idx = sack_idx;
        vault_item_free_strings(&target_copy);
    } else {
        /* Check occupancy for placement */
        bool *grid = build_occupancy_grid(widgets, sack, cols, rows, NULL);
        bool valid = can_place_item(grid, cols, rows, place_x, place_y, hi->item_w, hi->item_h);
        free(grid);
        if (!valid) return;

        TQVaultItem place_copy;
        vault_item_deep_copy(&place_copy, &hi->item);
        place_copy.point_x = place_x;
        place_copy.point_y = place_y;
        place_copy.width = hi->item_w;
        place_copy.height = hi->item_h;
        sack_add_item(sack, &place_copy);
        free_held_item(widgets);
    }

done:
    /* Mark vault dirty if this container is a vault or item came from vault */
    if (ctype == CONTAINER_VAULT || held_source == CONTAINER_VAULT)
        widgets->vault_dirty = true;

    /* Mark character dirty if source or destination is character inventory/bag */
    if (ctype == CONTAINER_INV || ctype == CONTAINER_BAG ||
        held_source == CONTAINER_INV || held_source == CONTAINER_BAG ||
        held_source == CONTAINER_EQUIP) {
        widgets->char_dirty = true;
        update_save_button_sensitivity(widgets);
    }

    invalidate_tooltips(widgets);
    queue_redraw_all(widgets);
}

/* ── Sack click handler: handles both pick-up and place ─────────────────── */
static void handle_sack_click(AppWidgets *widgets, GtkWidget *drawing_area,
                               TQVaultSack *sack,
                               ContainerType ctype, int sack_idx,
                               int cols, int rows, double cell,
                               double px, double py, int button) {
    if (!sack) return;
    if (button == 3) {
        if (widgets->held_item) {
            cancel_held_item(widgets);
        } else {
            TQVaultItem *hit = find_item_at_cell(widgets, sack, cols, rows,
                                                  cell, px, py, NULL);
            if (hit)
                show_item_context_menu(widgets, drawing_area, hit, NULL,
                                       ctype, sack_idx, -1, px, py);
        }
        return;
    }
    if (button != 1) return; /* only left-click */

    if (!widgets->held_item) {
        pick_up_from_sack(widgets, sack, ctype, sack_idx, cols, rows, cell, px, py);
    } else {
        place_in_sack(widgets, sack, ctype, sack_idx, cols, rows, cell, px, py);
    }
}

/* ── Click callbacks for each drawing area ──────────────────────────────── */

void on_vault_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press;
    AppWidgets *widgets = (AppWidgets *)user_data;
    if (!widgets->current_vault) return;
    int sack_idx = widgets->current_sack;
    if (sack_idx < 0 || sack_idx >= widgets->current_vault->num_sacks) return;
    TQVaultSack *sack = &widgets->current_vault->sacks[sack_idx];
    double cell = compute_cell_size(widgets);
    int w = gtk_widget_get_width(widgets->vault_drawing_area);
    if (cell <= 0.0) cell = (double)w / 18.0;
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    handle_sack_click(widgets, widgets->vault_drawing_area, sack, CONTAINER_VAULT, sack_idx, 18, 20, cell, x, y, button);
}

void on_inv_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press;
    AppWidgets *widgets = (AppWidgets *)user_data;
    if (!widgets->current_character || widgets->current_character->num_inv_sacks < 1) return;
    TQVaultSack *sack = &widgets->current_character->inv_sacks[0];
    double cell = compute_cell_size(widgets);
    int w = gtk_widget_get_width(widgets->inv_drawing_area);
    if (cell <= 0.0) cell = (double)w / CHAR_INV_COLS;
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    handle_sack_click(widgets, widgets->inv_drawing_area, sack, CONTAINER_INV, 0, CHAR_INV_COLS, CHAR_INV_ROWS, cell, x, y, button);
}

void on_bag_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press;
    AppWidgets *widgets = (AppWidgets *)user_data;
    if (!widgets->current_character) return;
    int idx = 1 + widgets->current_char_bag;
    if (idx >= widgets->current_character->num_inv_sacks) return;
    TQVaultSack *sack = &widgets->current_character->inv_sacks[idx];
    double cell = compute_cell_size(widgets);
    int w = gtk_widget_get_width(widgets->bag_drawing_area);
    if (cell <= 0.0) cell = (double)w / CHAR_BAG_COLS;
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    handle_sack_click(widgets, widgets->bag_drawing_area, sack, CONTAINER_BAG, widgets->current_char_bag,
                      CHAR_BAG_COLS, CHAR_BAG_ROWS, cell, x, y, button);
}

void on_equip_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press;
    AppWidgets *widgets = (AppWidgets *)user_data;
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    if (button == 3 && widgets->held_item) { cancel_held_item(widgets); return; }
    if (button != 1 && button != 3) return;
    if (!widgets->current_character) return;

    /* Hit-test equipment slots */
    double cell_size = compute_cell_size(widgets);

    double cx0 = 0.0;
    double cx1 = 2.0 * cell_size + EQUIP_COL_GAP;
    double cx2 = 4.0 * cell_size + 2.0 * EQUIP_COL_GAP;

    typedef struct { double cx; const EquipSlot *slots; int n; } ColDef;
    ColDef cols[3] = {
        { cx0, COL_LEFT,   (int)(sizeof COL_LEFT   / sizeof COL_LEFT[0]) },
        { cx1, COL_CENTER, (int)(sizeof COL_CENTER / sizeof COL_CENTER[0]) },
        { cx2, COL_RIGHT,  (int)(sizeof COL_RIGHT  / sizeof COL_RIGHT[0]) },
    };

    int hit_slot = -1;
    for (int ci = 0; ci < 3 && hit_slot < 0; ci++) {
        double cy = 0.0;
        for (int si = 0; si < cols[ci].n && hit_slot < 0; si++) {
            const EquipSlot *sl = &cols[ci].slots[si];
            double bw = (double)sl->box_w * cell_size;
            double bh = (double)sl->box_h * cell_size;
            if (x >= cols[ci].cx && x < cols[ci].cx + bw &&
                y >= cy          && y < cy + bh) {
                hit_slot = sl->slot_idx;
            }
            cy += bh + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
        }
        if (ci == 1) {
            double cy2 = 0.0;
            for (int si = 0; si < cols[1].n; si++)
                cy2 += (double)cols[1].slots[si].box_h * cell_size + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
            for (int ri = 0; ri < 2 && hit_slot < 0; ri++) {
                double rx = cx1 + (double)ri * (cell_size + EQUIP_COL_GAP / 2.0);
                double bw = RING_SLOTS[ri].box_w * cell_size;
                double bh = RING_SLOTS[ri].box_h * cell_size;
                if (x >= rx && x < rx + bw && y >= cy2 && y < cy2 + bh)
                    hit_slot = RING_SLOTS[ri].slot_idx;
            }
        }
    }

    if (hit_slot < 0 || hit_slot >= 12) return;

    /* Right-click on an equipment item: show context menu */
    if (button == 3) {
        TQItem *eq = widgets->current_character->equipment[hit_slot];
        if (eq && eq->base_name)
            show_item_context_menu(widgets, widgets->equip_drawing_area,
                                   NULL, eq, CONTAINER_EQUIP, -1, hit_slot, x, y);
        return;
    }

    if (!widgets->held_item) {
        /* Pick up from equipment */
        TQItem *eq = widgets->current_character->equipment[hit_slot];
        if (!eq || !eq->base_name) return;

        HeldItem *hi = calloc(1, sizeof(HeldItem));
        equip_to_vault_item(&hi->item, eq);
        hi->source = CONTAINER_EQUIP;
        hi->source_equip_slot = hit_slot;
        hi->texture = load_item_texture(widgets, eq->base_name, eq->var1);

        /* Get dimensions */
        if (hi->texture) {
            hi->item_w = gdk_pixbuf_get_width(hi->texture) / 32;
            hi->item_h = gdk_pixbuf_get_height(hi->texture) / 32;
            if (hi->item_w < 1) hi->item_w = 1;
            if (hi->item_h < 1) hi->item_h = 1;
        } else {
            hi->item_w = 1; hi->item_h = 1;
        }

        /* Free the equipment slot */
        free(eq->base_name);
        free(eq->prefix_name);
        free(eq->suffix_name);
        free(eq->relic_name);
        free(eq->relic_bonus);
        free(eq->relic_name2);
        free(eq->relic_bonus2);
        free(eq);
        widgets->current_character->equipment[hit_slot] = NULL;

        widgets->held_item = hi;
        widgets->char_dirty = true;
        update_save_button_sensitivity(widgets);
        invalidate_tooltips(widgets);
        queue_redraw_equip(widgets);
    } else {
        /* Place into equipment slot */
        HeldItem *hi = widgets->held_item;
        TQItem *existing = widgets->current_character->equipment[hit_slot];

        /* Socket relic/charm into the existing equipment item if compatible */
        if (existing && item_is_relic_or_charm(hi->item.base_name)) {
            int slot = item_can_accept_relic_equip(existing, hi->item.base_name, widgets->translations);
            if (slot != 0) {
                if (slot == 1) {
                    free(existing->relic_name);
                    free(existing->relic_bonus);
                    existing->relic_name  = safe_strdup(hi->item.base_name);
                    existing->relic_bonus = safe_strdup(hi->item.relic_bonus);
                    existing->var1        = hi->item.var1;
                } else {
                    free(existing->relic_name2);
                    free(existing->relic_bonus2);
                    existing->relic_name2  = safe_strdup(hi->item.base_name);
                    existing->relic_bonus2 = safe_strdup(hi->item.relic_bonus);
                    existing->var2         = hi->item.var1;
                }
                free_held_item(widgets);
                widgets->char_dirty = true;
                update_save_button_sensitivity(widgets);
                invalidate_tooltips(widgets);
                queue_redraw_equip(widgets);
                return;
            }
        }

        if (existing) {
            /* Swap: pick up existing, place held */
            TQVaultItem old_vi;
            equip_to_vault_item(&old_vi, existing);
            GdkPixbuf *old_tex = load_item_texture(widgets, existing->base_name, existing->var1);
            int ow = 1, oh = 1;
            if (old_tex) {
                ow = gdk_pixbuf_get_width(old_tex) / 32;
                oh = gdk_pixbuf_get_height(old_tex) / 32;
                if (ow < 1) ow = 1;
                if (oh < 1) oh = 1;
            }

            /* Place held item into slot */
            vault_item_to_equip(existing, &hi->item);

            /* Update held to be the old item */
            vault_item_free_strings(&hi->item);
            vault_item_deep_copy(&hi->item, &old_vi);
            hi->item_w = ow;
            hi->item_h = oh;
            if (hi->texture) g_object_unref(hi->texture);
            hi->texture = old_tex;
            hi->source = CONTAINER_EQUIP;
            hi->source_equip_slot = hit_slot;
            vault_item_free_strings(&old_vi);
        } else {
            /* Place into empty slot */
            TQItem *neq = calloc(1, sizeof(TQItem));
            vault_item_to_equip(neq, &hi->item);
            widgets->current_character->equipment[hit_slot] = neq;
            free_held_item(widgets);
        }

        widgets->char_dirty = true;
        update_save_button_sensitivity(widgets);
        invalidate_tooltips(widgets);
        queue_redraw_equip(widgets);
    }
}
