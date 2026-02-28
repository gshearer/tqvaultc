/* ui_tooltip.c — instant tooltip system (extracted from ui.c) */

#include "ui.h"
#include "item_stats.h"
#include <string.h>

/* Choose tooltip position so it doesn't clip off the top or bottom of the window.
 * rect is in widget-local coordinates; we translate to window coords to decide. */
static void tooltip_set_position(GtkWidget *popover, GtkWidget *parent,
                                 const GdkRectangle *rect) {
    graphene_point_t src = GRAPHENE_POINT_INIT(rect->x, rect->y);
    graphene_point_t dst;
    GtkWidget *win = GTK_WIDGET(gtk_widget_get_root(parent));
    if (!gtk_widget_compute_point(parent, win, &src, &dst))
        dst = src;
    double wy = dst.y;
    int win_h = gtk_widget_get_height(win);
    GtkPositionType pos;
    if (wy < win_h * 0.25)
        pos = GTK_POS_BOTTOM;
    else if (wy > win_h * 0.75)
        pos = GTK_POS_TOP;
    else
        pos = GTK_POS_RIGHT;
    gtk_popover_set_position(GTK_POPOVER(popover), pos);
}

/* Instant tooltip: driven by on_motion, replaces GTK4's 500ms-delayed tooltips. */
static void update_instant_tooltip(AppWidgets *widgets) {
    GtkWidget *popover = widgets->tooltip_popover;
    if (!popover) return;

    /* Hide if carrying an item or context menu is visible */
    if (widgets->held_item ||
        (widgets->context_menu && gtk_widget_get_visible(widgets->context_menu))) {
        gtk_widget_set_visible(popover, FALSE);
        return;
    }

    GtkWidget *w = widgets->cursor_widget;
    if (!w) {
        gtk_widget_set_visible(popover, FALSE);
        return;
    }

    double x = widgets->cursor_x;
    double y = widgets->cursor_y;
    int pw = gtk_widget_get_width(w);
    int ph = gtk_widget_get_height(w);
    TQVaultItem *item = NULL;
    int equip_slot = -1;
    GdkRectangle rect = {0};

    /* Helper: compute item rectangle from cell coordinates for sack items */
    #define SACK_ITEM_RECT(it, fallback_cols) do {                          \
        double cell = compute_cell_size(widgets);                           \
        if (cell <= 0.0) cell = (double)pw / (fallback_cols);              \
        int iw, ih;                                                         \
        get_item_dims(widgets, (it), &iw, &ih);                            \
        rect.x      = (int)((it)->point_x * cell);                        \
        rect.y      = (int)((it)->point_y * cell);                        \
        rect.width  = (int)(iw * cell);                                    \
        rect.height = (int)(ih * cell);                                    \
    } while (0)

    if (w == widgets->vault_drawing_area) {
        /* Vault sack */
        if (!widgets->current_vault) { gtk_widget_set_visible(popover, FALSE); return; }
        int si = widgets->current_sack;
        if (si >= 0 && si < widgets->current_vault->num_sacks) {
            TQVaultSack *sack = &widgets->current_vault->sacks[si];
            item = sack_hit_test(widgets, sack, 18, 20, pw, ph, (int)x, (int)y);
        }
        if (item) {
            if (item == widgets->last_tooltip_item &&
                gtk_widget_get_visible(popover))
                return;
            widgets->last_tooltip_item = item;
            widgets->last_tooltip_markup[0] = '\0';
            vault_item_format_stats(item, widgets->translations,
                                    widgets->last_tooltip_markup,
                                    sizeof(widgets->last_tooltip_markup));
            if (widgets->tooltip_parent != w) {
                if (widgets->tooltip_parent)
                    gtk_widget_unparent(popover);
                gtk_widget_set_parent(popover, w);
                widgets->tooltip_parent = w;
            }
            SACK_ITEM_RECT(item, 18);
            gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                                 widgets->last_tooltip_markup);
            gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
            tooltip_set_position(popover, w, &rect);
            gtk_widget_set_visible(popover, TRUE);
            return;
        }
        widgets->last_tooltip_item = NULL;
        gtk_widget_set_visible(popover, FALSE);
        return;
    }

    if (w == widgets->inv_drawing_area) {
        if (!widgets->current_character || widgets->current_character->num_inv_sacks < 1) {
            gtk_widget_set_visible(popover, FALSE); return;
        }
        TQVaultSack *sack = &widgets->current_character->inv_sacks[0];
        item = sack_hit_test(widgets, sack, CHAR_INV_COLS, CHAR_INV_ROWS, pw, ph, (int)x, (int)y);
        if (item) {
            if (item == widgets->last_inv_tooltip_item &&
                gtk_widget_get_visible(popover))
                return;
            widgets->last_inv_tooltip_item = item;
            widgets->last_inv_tooltip_markup[0] = '\0';
            vault_item_format_stats(item, widgets->translations,
                                    widgets->last_inv_tooltip_markup,
                                    sizeof(widgets->last_inv_tooltip_markup));
            if (widgets->tooltip_parent != w) {
                if (widgets->tooltip_parent)
                    gtk_widget_unparent(popover);
                gtk_widget_set_parent(popover, w);
                widgets->tooltip_parent = w;
            }
            SACK_ITEM_RECT(item, CHAR_INV_COLS);
            gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                                 widgets->last_inv_tooltip_markup);
            gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
            tooltip_set_position(popover, w, &rect);
            gtk_widget_set_visible(popover, TRUE);
            return;
        }
        widgets->last_inv_tooltip_item = NULL;
        gtk_widget_set_visible(popover, FALSE);
        return;
    }

    if (w == widgets->bag_drawing_area) {
        int idx = 1 + widgets->current_char_bag;
        if (!widgets->current_character || idx >= widgets->current_character->num_inv_sacks) {
            gtk_widget_set_visible(popover, FALSE); return;
        }
        TQVaultSack *sack = &widgets->current_character->inv_sacks[idx];
        item = sack_hit_test(widgets, sack, CHAR_BAG_COLS, CHAR_BAG_ROWS, pw, ph, (int)x, (int)y);
        if (item) {
            if (item == widgets->last_bag_tooltip_item &&
                gtk_widget_get_visible(popover))
                return;
            widgets->last_bag_tooltip_item = item;
            widgets->last_bag_tooltip_markup[0] = '\0';
            vault_item_format_stats(item, widgets->translations,
                                    widgets->last_bag_tooltip_markup,
                                    sizeof(widgets->last_bag_tooltip_markup));
            if (widgets->tooltip_parent != w) {
                if (widgets->tooltip_parent)
                    gtk_widget_unparent(popover);
                gtk_widget_set_parent(popover, w);
                widgets->tooltip_parent = w;
            }
            SACK_ITEM_RECT(item, CHAR_BAG_COLS);
            gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                                 widgets->last_bag_tooltip_markup);
            gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
            tooltip_set_position(popover, w, &rect);
            gtk_widget_set_visible(popover, TRUE);
            return;
        }
        widgets->last_bag_tooltip_item = NULL;
        gtk_widget_set_visible(popover, FALSE);
        return;
    }

    #undef SACK_ITEM_RECT

    if (w == widgets->equip_drawing_area) {
        if (!widgets->current_character) { gtk_widget_set_visible(popover, FALSE); return; }
        double cell_size = compute_cell_size(widgets);
        double sx, sy, sbw, sbh;
        int slot = -1;
        if (equip_hit_test(x, y, cell_size, &slot, &sx, &sy, &sbw, &sbh) &&
            slot >= 0 && slot < 12 &&
            widgets->current_character->equipment[slot]) {
            equip_slot = slot;
            if (equip_slot == widgets->last_equip_tooltip_slot &&
                gtk_widget_get_visible(popover))
                return;
            widgets->last_equip_tooltip_slot = equip_slot;
            widgets->last_equip_tooltip_markup[0] = '\0';
            TQItem *eq = widgets->current_character->equipment[equip_slot];
            TQVaultItem vi = {0};
            vi.seed        = eq->seed;
            vi.base_name   = eq->base_name;
            vi.prefix_name = eq->prefix_name;
            vi.suffix_name = eq->suffix_name;
            vi.relic_name  = eq->relic_name;
            vi.relic_bonus = eq->relic_bonus;
            vi.relic_name2 = eq->relic_name2;
            vi.relic_bonus2= eq->relic_bonus2;
            vi.var1        = eq->var1;
            vi.var2        = eq->var2;
            vault_item_format_stats(&vi, widgets->translations,
                                    widgets->last_equip_tooltip_markup,
                                    sizeof(widgets->last_equip_tooltip_markup));
            if (widgets->tooltip_parent != w) {
                if (widgets->tooltip_parent)
                    gtk_widget_unparent(popover);
                gtk_widget_set_parent(popover, w);
                widgets->tooltip_parent = w;
            }
            rect.x = (int)sx;  rect.y = (int)sy;
            rect.width = (int)sbw;  rect.height = (int)sbh;
            gtk_label_set_markup(GTK_LABEL(widgets->tooltip_label),
                                 widgets->last_equip_tooltip_markup);
            gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
            tooltip_set_position(popover, w, &rect);
            gtk_widget_set_visible(popover, TRUE);
            return;
        }
        widgets->last_equip_tooltip_slot = -1;
        gtk_widget_set_visible(popover, FALSE);
        return;
    }

    gtk_widget_set_visible(popover, FALSE);
}

void on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data) {
    AppWidgets *widgets = (AppWidgets *)user_data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    widgets->cursor_x = x;
    widgets->cursor_y = y;
    widgets->cursor_widget = w;
    if (widgets->held_item)
        gtk_widget_queue_draw(w);
    update_instant_tooltip(widgets);
}

void on_motion_leave(GtkEventControllerMotion *ctrl, gpointer user_data) {
    (void)ctrl;
    AppWidgets *widgets = (AppWidgets *)user_data;
    widgets->cursor_widget = NULL;
    if (widgets->held_item)
        queue_redraw_all(widgets);
    if (widgets->tooltip_popover)
        gtk_widget_set_visible(widgets->tooltip_popover, FALSE);
}
