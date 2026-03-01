#include "ui.h"
#include "texture.h"
#include "arz.h"
#include "asset_lookup.h"
#include "item_stats.h"
#include <string.h>
#include <strings.h>

/* Load the relic/charm overlay icon (cached via the texture cache).
 * The game texture has a solid black background, so we key it out to
 * transparent so the overlay composites cleanly over item textures. */
static GdkPixbuf* load_relic_overlay(AppWidgets *widgets) {
    static const char *overlay_path = "Items\\Relic\\ItemRelicOverlay.tex";
    GdkPixbuf *cached = g_hash_table_lookup(widgets->texture_cache, overlay_path);
    if (cached) return g_object_ref(cached);
    GdkPixbuf *raw = texture_load(overlay_path);
    if (!raw) return NULL;
    /* Make a writable copy and key out black pixels */
    GdkPixbuf *pixbuf = gdk_pixbuf_copy(raw);
    g_object_unref(raw);
    if (pixbuf && gdk_pixbuf_get_has_alpha(pixbuf)) {
        int w = gdk_pixbuf_get_width(pixbuf);
        int h = gdk_pixbuf_get_height(pixbuf);
        int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
        guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
        for (int y = 0; y < h; y++) {
            guchar *row = pixels + y * rowstride;
            for (int x = 0; x < w; x++) {
                guchar *px = row + x * 4;
                /* Treat near-black pixels as background → fully transparent */
                if (px[0] < 8 && px[1] < 8 && px[2] < 8)
                    px[3] = 0;
            }
        }
    }
    if (pixbuf)
        g_hash_table_insert(widgets->texture_cache, strdup(overlay_path), g_object_ref(pixbuf));
    return pixbuf;
}

/* Draw the relic overlay icon in the bottom-right cell of an item.
 * x,y is the pixel origin of the item; item_w/item_h are item size in cells. */
static void draw_relic_overlay(cairo_t *cr, AppWidgets *widgets,
                                double x, double y,
                                int item_w, int item_h, double cell_size)
{
    GdkPixbuf *overlay = load_relic_overlay(widgets);
    if (!overlay) return;
    int ow = gdk_pixbuf_get_width(overlay);
    int oh = gdk_pixbuf_get_height(overlay);
    /* Scale overlay proportionally: 32 native pixels = 1 cell (TQVaultAE standard).
     * This keeps the overlay at its natural size relative to the cell rather than
     * stretching it to fill the whole cell — important for small 1×1 items. */
    double draw_w = ((double)ow / 32.0) * cell_size;
    double draw_h = ((double)oh / 32.0) * cell_size;
    /* Position: bottom-right corner of the item's bottom-right cell */
    double ox = x + (double)item_w * cell_size - draw_w;
    double oy = y + (double)item_h * cell_size - draw_h;
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, draw_w / (double)ow, draw_h / (double)oh);
    gdk_cairo_set_source_pixbuf(cr, overlay, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    g_object_unref(overlay);
}

/* Returns the completedRelicLevel from the DBR, or 0 if not found. */
static int item_completed_relic_level(const char *base_name) {
    TQArzRecordData *data = asset_get_dbr(base_name);
    if (!data) return 0;
    return arz_record_get_int(data, "completedRelicLevel", 0, NULL);
}

/* equipment[] order: Head=0, Neck=1, Chest=2, Legs=3, Arms=4,
 *                    Ring1=5, Ring2=6, Wep1=7, Shld1=8, Wep2=9, Shld2=10, Artifact=11
 *
 * Column arrangement mirrors TQVaultAE (SackCollection.cs offsets):
 *   Left   col (x=1): Right (2×5), Artifact (2×2), Left (2×5)
 *   Centre col (x=4): Head (2×2), Neck (2×1), Chest (2×3), Legs (2×2), Ring1+Ring2 (1×1 each)
 *   Right  col (x=7): AltRight (2×5), Arms (2×2), AltLeft (2×5)
 */
const EquipSlot COL_LEFT[] = {
    { 7,  "Right",   2, 5 },
    { 11, "Artifact", 2, 2 },
    { 9,  "AltRight",  2, 5 },
};
const EquipSlot COL_CENTER[] = {
    { 0, "Head",  2, 2 },
    { 1, "Neck",  2, 1 },
    { 2, "Chest", 2, 3 },
    { 3, "Legs",  2, 2 },
};
/* Ring1 and Ring2 occupy the bottom of the centre column side-by-side (1×1 each) */
const EquipSlot RING_SLOTS[2] = {
    { 5, "Ring 1", 1, 1 },
    { 6, "Ring 2", 1, 1 },
};
const EquipSlot COL_RIGHT[] = {
    { 8,  "Left",   2, 5 },
    { 4,  "Arms",     2, 2 },
    { 10, "AltLeft", 2, 5 },
};

/* Hit-test the equipment panel at pixel (px,py) given cell_size.
 * Fills out_idx (slot index), out_x, out_y, out_bw, out_bh (pixel rect).
 * Returns true if a slot was found. */
bool equip_hit_test(double px, double py, double cell_size,
                            int *out_idx, double *out_x, double *out_y,
                            double *out_bw, double *out_bh)
{
    double cx0 = 0.0;
    double cx1 = 2.0 * cell_size + EQUIP_COL_GAP;
    double cx2 = 4.0 * cell_size + 2.0 * EQUIP_COL_GAP;

    typedef struct { double ex; const EquipSlot *slots; int n; } ColDef;
    ColDef cdefs[3] = {
        { cx0, COL_LEFT,   (int)(sizeof COL_LEFT   / sizeof COL_LEFT[0])   },
        { cx1, COL_CENTER, (int)(sizeof COL_CENTER / sizeof COL_CENTER[0]) },
        { cx2, COL_RIGHT,  (int)(sizeof COL_RIGHT  / sizeof COL_RIGHT[0])  },
    };

    for (int ci = 0; ci < 3; ci++) {
        double cy = 0.0;
        for (int si = 0; si < cdefs[ci].n; si++) {
            const EquipSlot *sl = &cdefs[ci].slots[si];
            double bw = (double)sl->box_w * cell_size;
            double bh = (double)sl->box_h * cell_size;
            if (px >= cdefs[ci].ex && px < cdefs[ci].ex + bw &&
                py >= cy             && py < cy + bh) {
                if (out_idx) *out_idx = sl->slot_idx;
                if (out_x)   *out_x   = cdefs[ci].ex;
                if (out_y)   *out_y   = cy;
                if (out_bw)  *out_bw  = bw;
                if (out_bh)  *out_bh  = bh;
                return true;
            }
            cy += bh + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
        }
        if (ci == 1) {
            double cy2 = 0.0;
            for (int si = 0; si < cdefs[1].n; si++)
                cy2 += (double)cdefs[1].slots[si].box_h * cell_size + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
            for (int ri = 0; ri < 2; ri++) {
                double rx = cx1 + (double)ri * (cell_size + EQUIP_COL_GAP / 2.0);
                double bw = (double)RING_SLOTS[ri].box_w * cell_size;
                double bh = (double)RING_SLOTS[ri].box_h * cell_size;
                if (px >= rx && px < rx + bw && py >= cy2 && py < cy2 + bh) {
                    if (out_idx) *out_idx = RING_SLOTS[ri].slot_idx;
                    if (out_x)   *out_x   = rx;
                    if (out_y)   *out_y   = cy2;
                    if (out_bw)  *out_bw  = bw;
                    if (out_bh)  *out_bh  = bh;
                    return true;
                }
            }
        }
    }
    return false;
}

/* Draw one slot box: background, border, item texture centred inside, label below.
 * x/y are the top-left pixel origin of the box; cell_size is shared with vault. */
static void draw_equip_slot(cairo_t *cr, AppWidgets *widgets,
                             const EquipSlot *slot,
                             double x, double y, double cell_size)
{
    double box_w = (double)slot->box_w * cell_size;
    double box_h = (double)slot->box_h * cell_size;

    /* Slot background */
    cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
    cairo_rectangle(cr, x + 1, y + 1, box_w - 2, box_h - 2);
    cairo_fill(cr);

    /* Slot border */
    cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 1, y + 1, box_w - 2, box_h - 2);
    cairo_stroke(cr);

    /* Item texture, centred inside the box at natural cell scale */
    if (widgets->current_character) {
        TQItem *item = widgets->current_character->equipment[slot->slot_idx];
        if (item) {
            GdkPixbuf *pixbuf = load_item_texture(widgets, item->base_name, item->var1);
            if (pixbuf) {
                int pw = gdk_pixbuf_get_width(pixbuf);
                int ph = gdk_pixbuf_get_height(pixbuf);
                /* Cell count from texture (32 px per cell — TQVaultAE standard) */
                int iw = pw / 32; if (iw < 1) iw = 1;
                int ih = ph / 32; if (ih < 1) ih = 1;
                double draw_w = (double)iw * cell_size;
                double draw_h = (double)ih * cell_size;
                double dx = x + (box_w - draw_w) / 2.0;
                double dy = y + (box_h - draw_h) / 2.0;
                cairo_save(cr);
                cairo_translate(cr, dx, dy);
                cairo_scale(cr, draw_w / (double)pw, draw_h / (double)ph);
                gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
                g_object_unref(pixbuf);
                /* Relic/charm overlay icon */
                if ((item->relic_name && item->relic_name[0]) ||
                    (item->relic_name2 && item->relic_name2[0])) {
                    draw_relic_overlay(cr, widgets, dx, dy, iw, ih, cell_size);
                }
            }
        }
    }

    /* Slot label centred below the box */
    cairo_text_extents_t te;
    cairo_text_extents(cr, slot->label, &te);
    double tx = x + (box_w - te.width) / 2.0 - te.x_bearing;
    double ty = y + box_h + te.height + 2.0;
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, slot->label);
}

void equip_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
                           int width, int height, gpointer user_data)
{
    (void)drawing_area; (void)width; (void)height;
    AppWidgets *widgets = (AppWidgets *)user_data;

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_paint(cr);

    if (!widgets->current_character) return;

    /* Use the same shared cell size as the vault grid */
    double cell_size = compute_cell_size(widgets);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);

    /* Column x origins: each column is 2 cells wide */
    double cx0 = 0.0;
    double cx1 = 2.0 * cell_size + EQUIP_COL_GAP;
    double cx2 = 4.0 * cell_size + 2.0 * EQUIP_COL_GAP;

    /* Left column */
    double y = 0.0;
    for (int i = 0; i < (int)(sizeof COL_LEFT / sizeof COL_LEFT[0]); i++) {
        draw_equip_slot(cr, widgets, &COL_LEFT[i], cx0, y, cell_size);
        y += (double)COL_LEFT[i].box_h * cell_size + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
    }

    /* Centre column */
    y = 0.0;
    for (int i = 0; i < (int)(sizeof COL_CENTER / sizeof COL_CENTER[0]); i++) {
        draw_equip_slot(cr, widgets, &COL_CENTER[i], cx1, y, cell_size);
        y += (double)COL_CENTER[i].box_h * cell_size + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
    }
    /* Ring1 and Ring2 side-by-side at the bottom of the centre column */
    for (int i = 0; i < 2; i++) {
        double rx = cx1 + (double)i * (cell_size + EQUIP_COL_GAP / 2.0);
        draw_equip_slot(cr, widgets, &RING_SLOTS[i], rx, y, cell_size);
    }

    /* Right column */
    y = 0.0;
    for (int i = 0; i < (int)(sizeof COL_RIGHT / sizeof COL_RIGHT[0]); i++) {
        draw_equip_slot(cr, widgets, &COL_RIGHT[i], cx2, y, cell_size);
        y += (double)COL_RIGHT[i].box_h * cell_size + EQUIP_LABEL_H + EQUIP_SLOT_GAP;
    }

    /* Held-item overlay on equip area: draw at cursor if hovering here */
    if (widgets->held_item && widgets->cursor_widget == widgets->equip_drawing_area) {
        HeldItem *hi = widgets->held_item;

        /* When holding a relic/charm, highlight the slot under the cursor */
        if (item_is_relic_or_charm(hi->item.base_name) && widgets->current_character) {
            int slot_idx = -1;
            double sx = 0.0, sy = 0.0, sbw = 0.0, sbh = 0.0;
            if (equip_hit_test(widgets->cursor_x, widgets->cursor_y, cell_size,
                               &slot_idx, &sx, &sy, &sbw, &sbh) &&
                slot_idx >= 0 && slot_idx < 12) {
                TQItem *eq = widgets->current_character->equipment[slot_idx];
                if (eq && eq->base_name) {
                    bool can_socket = item_can_accept_relic_equip(eq, hi->item.base_name, widgets->translations) != 0;
                    cairo_set_source_rgba(cr,
                        can_socket ? 0.0 : 0.8,
                        can_socket ? 0.8 : 0.0,
                        0.0, 0.35);
                    cairo_rectangle(cr, sx, sy, sbw, sbh);
                    cairo_fill(cr);
                }
            }
        }

        if (hi->texture) {
            int pw = gdk_pixbuf_get_width(hi->texture);
            int ph = gdk_pixbuf_get_height(hi->texture);
            double rw = (double)hi->item_w * cell_size;
            double rh = (double)hi->item_h * cell_size;
            double ix = widgets->cursor_x - rw / 2.0;
            double iy = widgets->cursor_y - rh / 2.0;
            cairo_save(cr);
            cairo_translate(cr, ix, iy);
            cairo_scale(cr, rw / (double)pw, rh / (double)ph);
            gdk_cairo_set_source_pixbuf(cr, hi->texture, 0, 0);
            cairo_paint_with_alpha(cr, 0.7);
            cairo_restore(cr);
        }
    }
}

/* ── Shared sack drawing helper ──────────────────────────────────────────── */

void draw_sack_items(cairo_t *cr, AppWidgets *widgets,
                     TQVaultSack *sack, int cols, int rows,
                     int width, int height, double forced_cell,
                     GtkWidget *this_widget)
{
    /* Use forced_cell when provided (>0); otherwise derive from dimensions. */
    double cell;
    if (forced_cell > 0.0) {
        cell = forced_cell;
    } else {
        double cw = (double)width  / cols;
        double ch = (double)height / rows;
        cell = cw < ch ? cw : ch;
    }
    double cell_width  = cell;
    double cell_height = cell;

    /* Background — only fill the grid area so any extra allocated space
     * remains transparent (no visible blank bar below the grid). */
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_rectangle(cr, 0, 0, (double)cols * cell_width, (double)rows * cell_height);
    cairo_fill(cr);

    /* Grid lines */
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= rows; i++) {
        cairo_move_to(cr, 0, (double)i * cell_height);
        cairo_line_to(cr, (double)cols * cell_width, (double)i * cell_height);
    }
    for (int j = 0; j <= cols; j++) {
        cairo_move_to(cr, (double)j * cell_width, 0);
        cairo_line_to(cr, (double)j * cell_width, (double)rows * cell_height);
    }
    cairo_stroke(cr);

    if (!sack) return;

    for (int i = 0; i < sack->num_items; i++) {
        TQVaultItem *item = &sack->items[i];

        GdkPixbuf *pixbuf = load_item_texture(widgets, item->base_name, item->var1);
        int w, h;
        if (pixbuf) {
            w = gdk_pixbuf_get_width(pixbuf) / 32;
            h = gdk_pixbuf_get_height(pixbuf) / 32;
            if (w < 1) w = 1;
            if (h < 1) h = 1;
        } else {
            w = item->width  > 0 ? item->width  : 1;
            h = item->height > 0 ? item->height : 1;
        }

        double x  = (double)item->point_x * cell_width;
        double y  = (double)item->point_y * cell_height;
        double rw = (double)w * cell_width;
        double rh = (double)h * cell_height;

        if (pixbuf) {
            int pw = gdk_pixbuf_get_width(pixbuf);
            int ph = gdk_pixbuf_get_height(pixbuf);
            cairo_save(cr);
            cairo_translate(cr, x + 2, y + 2);
            cairo_scale(cr, (rw - 4) / pw, (rh - 4) / ph);
            gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            g_object_unref(pixbuf);
        } else {
            cairo_set_source_rgb(cr, 0.5, 0.5, 0.8);
            cairo_rectangle(cr, x + 2, y + 2, rw - 4, rh - 4);
            cairo_fill(cr);
        }

        /* Relic/charm overlay icon in bottom-right cell */
        if ((item->relic_name && item->relic_name[0]) ||
            (item->relic_name2 && item->relic_name2[0])) {
            draw_relic_overlay(cr, widgets, x, y, w, h, cell_width);
        }

        /* Shard count number in bottom-right corner for incomplete relics/charms */
        if (item_is_relic_or_charm(item->base_name)) {
            int max_shards = item_completed_relic_level(item->base_name);
            uint32_t shard_count = item->var1 > 0 ? item->var1 : 1;
            if (max_shards > 0 && shard_count < (uint32_t)max_shards) {
                char num_text[4];
                snprintf(num_text, sizeof(num_text), "%u", shard_count);
                cairo_save(cr);
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                double font_size = rh * 0.35;
                if (font_size < 10) font_size = 10;
                cairo_set_font_size(cr, font_size);
                cairo_text_extents_t extents;
                cairo_text_extents(cr, num_text, &extents);
                double tx = x + rw - extents.width - 4;
                double ty = y + rh - 4;
                /* Shadow outline */
                cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        if (dx == 0 && dy == 0) continue;
                        cairo_move_to(cr, tx + dx, ty + dy);
                        cairo_show_text(cr, num_text);
                    }
                }
                /* White number */
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_move_to(cr, tx, ty);
                cairo_show_text(cr, num_text);
                cairo_restore(cr);
            }
        }

        /* Stack count in bottom-right corner for stacked items.
         * For relics/charms, show shard count (var1) only when incomplete. */
        bool is_rc = item_is_relic_or_charm(item->base_name);
        int display_qty = is_rc ? (int)item->var1 : item->stack_size;
        bool rc_complete = is_rc && display_qty >= relic_max_shards(item->base_name);
        if (display_qty > 1 && !rc_complete) {
            char num_text[8];
            snprintf(num_text, sizeof(num_text), "%d", display_qty);
            cairo_save(cr);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            double font_size = rh * 0.35;
            if (font_size < 10) font_size = 10;
            cairo_set_font_size(cr, font_size);
            cairo_text_extents_t extents;
            cairo_text_extents(cr, num_text, &extents);
            double tx = x + rw - extents.width - 4;
            double ty = y + rh - 4;
            /* Shadow outline */
            cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    cairo_move_to(cr, tx + dx, ty + dy);
                    cairo_show_text(cr, num_text);
                }
            }
            /* White number */
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_move_to(cr, tx, ty);
            cairo_show_text(cr, num_text);
            cairo_restore(cr);
        }

        /* Search highlight: red outline around matching items */
        if (widgets->search_text[0] && item_matches_search(widgets, item)) {
            cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.9);
            cairo_set_line_width(cr, 2.0);
            cairo_rectangle(cr, x + 1, y + 1, rw - 2, rh - 2);
            cairo_stroke(cr);
        }
    }

    /* ── Held-item placement preview + cursor overlay ───────────────────── */
    if (widgets->held_item && widgets->cursor_widget == this_widget) {
        HeldItem *hi = widgets->held_item;
        int cx = (int)(widgets->cursor_x / cell_width);
        int cy = (int)(widgets->cursor_y / cell_height);

        /* When holding a relic/charm: check if the cursor is over a socketable
         * target item.  If so, highlight that item instead of the placement grid. */
        bool held_is_relic = item_is_relic_or_charm(hi->item.base_name);
        TQVaultItem *relic_target = NULL;
        int relic_target_iw = 0, relic_target_ih = 0;
        if (held_is_relic && sack) {
            for (int i = 0; i < sack->num_items; i++) {
                TQVaultItem *it = &sack->items[i];
                if (!it->base_name) continue;
                int iw, ih;
                get_item_dims(widgets, it, &iw, &ih);
                if (cx >= it->point_x && cx < it->point_x + iw &&
                    cy >= it->point_y && cy < it->point_y + ih) {
                    relic_target = it;
                    relic_target_iw = iw;
                    relic_target_ih = ih;
                    break;
                }
            }
        }

        /* If the relic target is itself a stackable-compatible relic/charm,
         * fall through to the normal placement path (handles stack highlight). */
        if (relic_target && items_stackable(&hi->item, relic_target))
            relic_target = NULL;

        if (relic_target) {
            /* Hovering over a target item while holding a relic/charm:
             * green = has an empty socket, red = no available socket. */
            bool can_socket = item_can_accept_relic_sack(relic_target, hi->item.base_name, widgets->translations) != 0;
            cairo_set_source_rgba(cr,
                can_socket ? 0.0 : 0.8,
                can_socket ? 0.8 : 0.0,
                0.0, 0.35);
            cairo_rectangle(cr,
                (double)relic_target->point_x * cell_width,
                (double)relic_target->point_y * cell_height,
                (double)relic_target_iw * cell_width,
                (double)relic_target_ih * cell_height);
            cairo_fill(cr);
        } else {
            /* Normal placement preview (also used when holding a relic/charm
             * but not hovering over any existing item). */
            int px = cx - hi->item_w / 2;
            int py = cy - hi->item_h / 2;

            bool *grid = build_occupancy_grid(widgets, sack, cols, rows, NULL);
            bool valid = can_place_item(grid, cols, rows, px, py, hi->item_w, hi->item_h);
            free(grid);

            /* Also valid if hovering over a stackable target */
            if (!valid && sack) {
                for (int i = 0; i < sack->num_items; i++) {
                    TQVaultItem *it = &sack->items[i];
                    if (!it->base_name) continue;
                    int iw, ih;
                    get_item_dims(widgets, it, &iw, &ih);
                    if (cx >= it->point_x && cx < it->point_x + iw &&
                        cy >= it->point_y && cy < it->point_y + ih &&
                        items_stackable(&hi->item, it)) {
                        valid = true;
                        break;
                    }
                }
            }

            for (int dy = 0; dy < hi->item_h; dy++) {
                for (int dx = 0; dx < hi->item_w; dx++) {
                    int gx = px + dx, gy = py + dy;
                    if (gx < 0 || gx >= cols || gy < 0 || gy >= rows) continue;
                    if (valid)
                        cairo_set_source_rgba(cr, 0.0, 0.8, 0.0, 0.25);
                    else
                        cairo_set_source_rgba(cr, 0.8, 0.0, 0.0, 0.25);
                    cairo_rectangle(cr, (double)gx * cell_width, (double)gy * cell_height,
                                    cell_width, cell_height);
                    cairo_fill(cr);
                }
            }
        }

        /* Draw held item texture at cursor with transparency */
        if (hi->texture) {
            int pw = gdk_pixbuf_get_width(hi->texture);
            int ph = gdk_pixbuf_get_height(hi->texture);
            double rw = (double)hi->item_w * cell_width;
            double rh = (double)hi->item_h * cell_height;
            double ix = widgets->cursor_x - rw / 2.0;
            double iy = widgets->cursor_y - rh / 2.0;
            cairo_save(cr);
            cairo_translate(cr, ix, iy);
            cairo_scale(cr, rw / (double)pw, rh / (double)ph);
            gdk_cairo_set_source_pixbuf(cr, hi->texture, 0, 0);
            cairo_paint_with_alpha(cr, 0.7);
            cairo_restore(cr);
        }
    }
}

/* When the vault area resizes (e.g. window tiled by compositor), update the
 * equip drawing area's content size so GTK allocates enough space for the
 * current cell size, then queue a redraw.  Without this the equip area keeps
 * its initial (too-small) size and items overflow or use a stale scale. */
void on_vault_resize(GtkDrawingArea *area, int width, int height, gpointer user_data) {
    (void)area; (void)width; (void)height;
    AppWidgets *widgets = (AppWidgets *)user_data;
    double cell = compute_cell_size(widgets);
    if (cell > 0.0) {
        int ew = (int)(6.0 * cell + 2.0 * EQUIP_COL_GAP + 0.5);
        int eh = (int)(12.0 * cell + 3.0 * EQUIP_LABEL_H + 2.0 * EQUIP_SLOT_GAP + 0.5);
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widgets->equip_drawing_area), ew);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widgets->equip_drawing_area), eh);
    }
    gtk_widget_queue_draw(widgets->equip_drawing_area);
}

void vault_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)drawing_area;
    AppWidgets *widgets = (AppWidgets *)user_data;
    TQVaultSack *sack = NULL;
    if (widgets->current_vault &&
        widgets->current_sack >= 0 &&
        widgets->current_sack < widgets->current_vault->num_sacks) {
        sack = &widgets->current_vault->sacks[widgets->current_sack];
    }
    double cell = compute_cell_size(widgets);
    if (cell <= 0.0) cell = (double)width / 18.0;
    draw_sack_items(cr, widgets, sack, 18, 20, width, height, cell,
                    widgets->vault_drawing_area);
}

void inv_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)drawing_area;
    AppWidgets *widgets = (AppWidgets *)user_data;
    TQVaultSack *sack = NULL;
    if (widgets->current_character && widgets->current_character->num_inv_sacks > 0)
        sack = &widgets->current_character->inv_sacks[0];
    double cell = compute_cell_size(widgets);
    if (cell <= 0.0) cell = (double)width / CHAR_INV_COLS;
    draw_sack_items(cr, widgets, sack, CHAR_INV_COLS, CHAR_INV_ROWS, width, height, cell,
                    widgets->inv_drawing_area);
}

void bag_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)drawing_area;
    AppWidgets *widgets = (AppWidgets *)user_data;
    TQVaultSack *sack = NULL;
    int idx = 1 + widgets->current_char_bag;
    if (widgets->current_character && idx < widgets->current_character->num_inv_sacks)
        sack = &widgets->current_character->inv_sacks[idx];
    double cell = compute_cell_size(widgets);
    if (cell <= 0.0) cell = (double)width / CHAR_BAG_COLS;
    draw_sack_items(cr, widgets, sack, CHAR_BAG_COLS, CHAR_BAG_ROWS, width, height, cell,
                    widgets->bag_drawing_area);
}

/* Generic sack tooltip helper: hit-tests (x,y) in pixel space against a sack grid.
 * Returns the matching TQVaultItem* or NULL. */
TQVaultItem* sack_hit_test(AppWidgets *widgets, TQVaultSack *sack,
                                   int cols, int rows, int w, int h, int x, int y) {
    if (!sack || w <= 0 || h <= 0) return NULL;
    double cell = compute_cell_size(widgets);
    if (cell <= 0.0) {
        double cw = (double)w / cols;
        double ch = (double)h / rows;
        cell = cw < ch ? cw : ch;
    }
    int col = (int)(x / cell);
    int row = (int)(y / cell);
    if (col < 0 || col >= cols || row < 0 || row >= rows) return NULL;

    for (int i = 0; i < sack->num_items; i++) {
        TQVaultItem *it = &sack->items[i];
        if (!it->base_name) continue;
        int iw, ih;
        get_item_dims(widgets, it, &iw, &ih);
        if (col >= it->point_x && col < it->point_x + iw &&
            row >= it->point_y && row < it->point_y + ih) {
            return it;
        }
    }
    return NULL;
}

/* Compute the shared cell size from the main_hbox dimensions.  Called from
 * every draw callback so the value is always fresh — no signal timing issues. */
double compute_cell_size(AppWidgets *widgets)
{
    int w = gtk_widget_get_width(widgets->main_hbox);
    int h = gtk_widget_get_height(widgets->main_hbox);
    if (w <= 0 || h <= 0) return 0.0;

    double cell_w = (double)(w - LAYOUT_H_OVERHEAD) /
                    (VAULT_COLS + CHAR_INV_COLS + CHAR_BAG_COLS);
    double cell_h = (double)(h - VAULT_V_OVERHEAD) / VAULT_ROWS;

    double cell = cell_w < cell_h ? cell_w : cell_h;
    if (cell < 1.0) cell = 1.0;

    return cell;
}

/* ── Stash draw callbacks ──────────────────────────────────────────────── */

static void draw_empty_stash_message(cairo_t *cr, int width, int height,
                                      const char *msg) {
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_paint(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, msg, &te);
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_move_to(cr, ((double)width - te.width) / 2.0 - te.x_bearing,
                      ((double)height - te.height) / 2.0 - te.y_bearing);
    cairo_show_text(cr, msg);
}

static void stash_draw_common(cairo_t *cr, AppWidgets *widgets,
                               TQStash *stash, GtkWidget *da,
                               int width, int height, const char *empty_msg) {
    if (!stash) {
        draw_empty_stash_message(cr, width, height, empty_msg);
        return;
    }
    double cw = (double)width  / stash->sack_width;
    double ch = (double)height / stash->sack_height;
    double cell = cw < ch ? cw : ch;
    if (cell < 1.0) cell = 1.0;
    draw_sack_items(cr, widgets, &stash->sack, stash->sack_width,
                    stash->sack_height, width, height, cell, da);
}

void stash_transfer_draw_cb(GtkDrawingArea *da, cairo_t *cr,
                             int w, int h, gpointer ud) {
    (void)da;
    AppWidgets *widgets = (AppWidgets *)ud;
    stash_draw_common(cr, widgets, widgets->transfer_stash,
                      widgets->stash_transfer_da, w, h,
                      "Transfer stash not found");
}

void stash_player_draw_cb(GtkDrawingArea *da, cairo_t *cr,
                           int w, int h, gpointer ud) {
    (void)da;
    AppWidgets *widgets = (AppWidgets *)ud;
    stash_draw_common(cr, widgets, widgets->player_stash,
                      widgets->stash_player_da, w, h,
                      "Player stash not found");
}

void stash_relic_draw_cb(GtkDrawingArea *da, cairo_t *cr,
                          int w, int h, gpointer ud) {
    (void)da;
    AppWidgets *widgets = (AppWidgets *)ud;
    stash_draw_common(cr, widgets, widgets->relic_vault,
                      widgets->stash_relic_da, w, h,
                      "Relic vault not found");
}
