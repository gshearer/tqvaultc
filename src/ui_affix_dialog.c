/**
 * ui_affix_dialog.c — Affix modification dialog (three-pane prefix/suffix picker)
 *
 * Extracted from ui.c for maintainability.
 */
#include "ui.h"
#include "item_stats.h"
#include "affix_table.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ── Affix dialog state ────────────────────────────────────────────────── */

typedef struct {
    AppWidgets *widgets;
    GtkWidget  *dialog;
    TQItemAffixes *affixes;          /* owned — freed on destroy */
    GtkListBox *prefix_listbox;
    GtkListBox *suffix_listbox;
    GtkWidget  *prefix_search;
    GtkWidget  *suffix_search;
    GtkLabel   *preview_label;
    char *orig_prefix;               /* strdup'd originals for cancel */
    char *orig_suffix;
    char *selected_prefix;           /* current dialog selection (strdup'd or NULL) */
    char *selected_suffix;
    /* Back-pointers to the real item (exactly one is non-NULL) */
    TQVaultItem *vault_item;
    TQItem      *equip_item;
    ContainerType source;
    /* Item fields needed for preview */
    uint32_t seed;
    char *base_name;
    char *relic_name;
    char *relic_bonus;
    uint32_t var1;
    char *relic_name2;
    char *relic_bonus2;
    uint32_t var2;
} AffixDialogState;

static void affix_dialog_state_free(gpointer data) {
    AffixDialogState *st = data;
    free(st->orig_prefix);
    free(st->orig_suffix);
    free(st->selected_prefix);
    free(st->selected_suffix);
    /* Do NOT call affix_result_free(st->affixes) — the result is owned by
     * affix_table's internal cache and will be reused on subsequent calls. */
    g_free(st);
}

/* Build a temporary TQVaultItem from dialog state and render tooltip markup */
static void update_affix_preview(AffixDialogState *st) {
    TQVaultItem tmp = {
        .seed = st->seed,
        .base_name = st->base_name,
        .prefix_name = st->selected_prefix,
        .suffix_name = st->selected_suffix,
        .relic_name = st->relic_name,
        .relic_bonus = st->relic_bonus,
        .var1 = st->var1,
        .relic_name2 = st->relic_name2,
        .relic_bonus2 = st->relic_bonus2,
        .var2 = st->var2,
    };
    if (!st->preview_label) return;  /* dialog being torn down */
    char buf[16384];
    buf[0] = '\0';
    vault_item_format_stats(&tmp, st->widgets->translations, buf, sizeof(buf));
    gtk_label_set_markup(st->preview_label, buf);
}

/* Retrieve the affix-path string stored on a listbox row (may be NULL for "(None)") */
static const char *row_get_affix_path(GtkListBoxRow *row) {
    return g_object_get_data(G_OBJECT(row), "affix-path");
}

/* Retrieve the display label stored on a listbox row for filtering */
static const char *row_get_label_text(GtkListBoxRow *row) {
    return g_object_get_data(G_OBJECT(row), "label-text");
}

static void on_prefix_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    AffixDialogState *st = data;
    if (!st->preview_label) return;  /* dialog being torn down */
    free(st->selected_prefix);
    if (row) {
        const char *p = row_get_affix_path(row);
        st->selected_prefix = p ? strdup(p) : NULL;
    } else {
        st->selected_prefix = st->orig_prefix ? strdup(st->orig_prefix) : NULL;
    }
    update_affix_preview(st);
}

static void on_suffix_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    AffixDialogState *st = data;
    if (!st->preview_label) return;  /* dialog being torn down */
    free(st->selected_suffix);
    if (row) {
        const char *p = row_get_affix_path(row);
        st->selected_suffix = p ? strdup(p) : NULL;
    } else {
        st->selected_suffix = st->orig_suffix ? strdup(st->orig_suffix) : NULL;
    }
    update_affix_preview(st);
}

static gboolean affix_filter_func(GtkListBoxRow *row, gpointer data) {
    GtkSearchEntry *search = GTK_SEARCH_ENTRY(data);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(search));
    if (!query || !query[0]) return TRUE;  /* no filter — show all */

    const char *label = row_get_label_text(row);
    if (!label) return TRUE;  /* "(None)" row always visible */

    /* Case-insensitive substring match */
    for (const char *s = label; *s; s++) {
        const char *a = s, *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++; b++;
        }
        if (!*b) return TRUE;
    }
    return FALSE;
}

static void on_prefix_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    AffixDialogState *st = data;
    gtk_list_box_invalidate_filter(st->prefix_listbox);
}

static void on_suffix_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    AffixDialogState *st = data;
    gtk_list_box_invalidate_filter(st->suffix_listbox);
}

static void on_affix_dialog_apply(GtkButton *btn, gpointer data) {
    (void)btn;
    AffixDialogState *st = data;
    AppWidgets *w = st->widgets;

    if (st->equip_item) {
        TQItem *eq = st->equip_item;
        free(eq->prefix_name);
        eq->prefix_name = st->selected_prefix ? strdup(st->selected_prefix) : NULL;
        free(eq->suffix_name);
        eq->suffix_name = st->selected_suffix ? strdup(st->selected_suffix) : NULL;
        w->char_dirty = true;
    } else if (st->vault_item) {
        TQVaultItem *it = st->vault_item;
        free(it->prefix_name);
        it->prefix_name = st->selected_prefix ? strdup(st->selected_prefix) : NULL;
        free(it->suffix_name);
        it->suffix_name = st->selected_suffix ? strdup(st->selected_suffix) : NULL;
        if (st->source == CONTAINER_VAULT)
            w->vault_dirty = true;
        else
            w->char_dirty = true;
    }
    invalidate_tooltips(w);
    queue_redraw_equip(w);
    update_save_button_sensitivity(w);
    st->preview_label = NULL;  /* prevent row-deselect signals from using destroyed widget */
    gtk_window_destroy(GTK_WINDOW(st->dialog));
}

static void on_affix_dialog_cancel(GtkButton *btn, gpointer data) {
    (void)btn;
    AffixDialogState *st = data;
    st->preview_label = NULL;
    gtk_window_destroy(GTK_WINDOW(st->dialog));
}

/* Create a single listbox row for an affix entry */
static GtkWidget *make_affix_row(const char *name, const char *affix_path,
                                  float pct, bool is_current,
                                  bool has_sibling, const char *affix_path_raw) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(hbox, 4);
    gtk_widget_set_margin_end(hbox, 4);
    gtk_widget_set_margin_top(hbox, 2);
    gtk_widget_set_margin_bottom(hbox, 2);

    char label_buf[256];
    if (name) {
        if (has_sibling && affix_path_raw) {
            const char *last_sep = strrchr(affix_path_raw, '\\');
            const char *fname = last_sep ? last_sep + 1 : affix_path_raw;
            int flen = (int)strlen(fname);
            if (flen > 4) flen -= 4;  /* strip .dbr */
            snprintf(label_buf, sizeof(label_buf), "%s [%.*s]", name, flen, fname);
        } else {
            snprintf(label_buf, sizeof(label_buf), "%s", name);
        }
    } else {
        snprintf(label_buf, sizeof(label_buf), "(None)");
    }

    GtkWidget *name_label = gtk_label_new(label_buf);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.1f);
    gtk_widget_set_hexpand(name_label, TRUE);
    if (is_current)
        gtk_widget_add_css_class(name_label, "affix-current");
    gtk_box_append(GTK_BOX(hbox), name_label);

    if (name && pct > 0) {
        char wt[32];
        snprintf(wt, sizeof(wt), "%.1f%%", pct);
        GtkWidget *wt_label = gtk_label_new(wt);
        gtk_widget_add_css_class(wt_label, "dim-label");
        gtk_box_append(GTK_BOX(hbox), wt_label);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    /* Store data on the row */
    if (affix_path)
        g_object_set_data_full(G_OBJECT(row), "affix-path", g_strdup(affix_path), g_free);
    /* label-text for filtering (NULL for "(None)" — always shown) */
    if (name)
        g_object_set_data_full(G_OBJECT(row), "label-text", g_strdup(label_buf), g_free);

    return row;
}

void show_affix_dialog(AppWidgets *widgets) {
    const char *base = NULL;
    TQVaultItem *vault_item = widgets->context_item;
    TQItem *equip_item = widgets->context_equip_item;


    if (equip_item)
        base = equip_item->base_name;
    else if (vault_item)
        base = vault_item->base_name;
    if (!base) return;


    TQItemAffixes *affixes = affix_table_get(base, widgets->translations);
    if (!affixes) return;


    AffixDialogState *st = g_new0(AffixDialogState, 1);
    st->widgets = widgets;
    st->affixes = affixes;
    st->vault_item = vault_item;
    st->equip_item = equip_item;
    st->source = widgets->context_source;


    /* Copy item fields for preview */
    if (equip_item) {
        st->seed = equip_item->seed;
        st->base_name = equip_item->base_name;
        st->orig_prefix = equip_item->prefix_name ? strdup(equip_item->prefix_name) : NULL;
        st->orig_suffix = equip_item->suffix_name ? strdup(equip_item->suffix_name) : NULL;
        st->relic_name = equip_item->relic_name;
        st->relic_bonus = equip_item->relic_bonus;
        st->var1 = equip_item->var1;
        st->relic_name2 = equip_item->relic_name2;
        st->relic_bonus2 = equip_item->relic_bonus2;
        st->var2 = equip_item->var2;
    } else {
        st->seed = vault_item->seed;
        st->base_name = vault_item->base_name;
        st->orig_prefix = vault_item->prefix_name ? strdup(vault_item->prefix_name) : NULL;
        st->orig_suffix = vault_item->suffix_name ? strdup(vault_item->suffix_name) : NULL;
        st->relic_name = vault_item->relic_name;
        st->relic_bonus = vault_item->relic_bonus;
        st->var1 = vault_item->var1;
        st->relic_name2 = vault_item->relic_name2;
        st->relic_bonus2 = vault_item->relic_bonus2;
        st->var2 = vault_item->var2;
    }
    st->selected_prefix = st->orig_prefix ? strdup(st->orig_prefix) : NULL;
    st->selected_suffix = st->orig_suffix ? strdup(st->orig_suffix) : NULL;


    /* -- Build dialog window -- */
    GtkWidget *dialog = gtk_window_new();
    st->dialog = dialog;
    gtk_window_set_title(GTK_WINDOW(dialog), "Modify Affixes");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
    gtk_window_set_default_size(GTK_WINDOW(dialog), 1100, 650);
    gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);


    /* Attach state for cleanup on destroy */
    g_object_set_data_full(G_OBJECT(dialog), "affix-state", st, affix_dialog_state_free);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);


    /* -- Three-pane hbox -- */
    GtkWidget *panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_vexpand(panes, TRUE);
    gtk_widget_set_margin_start(panes, 8);
    gtk_widget_set_margin_end(panes, 8);
    gtk_widget_set_margin_top(panes, 8);
    gtk_box_append(GTK_BOX(vbox), panes);

    /* -- Left pane: Prefix -- */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(left_vbox, TRUE);
    gtk_box_append(GTK_BOX(panes), left_vbox);

    GtkWidget *prefix_label = gtk_label_new("Prefix");
    gtk_widget_set_halign(prefix_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(left_vbox), prefix_label);

    GtkWidget *prefix_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(prefix_search), "Filter...");
    st->prefix_search = prefix_search;
    gtk_box_append(GTK_BOX(left_vbox), prefix_search);

    GtkWidget *prefix_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(prefix_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(prefix_scroll, TRUE);
    gtk_box_append(GTK_BOX(left_vbox), prefix_scroll);


    GtkWidget *prefix_listbox = gtk_list_box_new();
    st->prefix_listbox = GTK_LIST_BOX(prefix_listbox);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(prefix_listbox), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(prefix_scroll), prefix_listbox);

    /* -- Center pane: Preview -- */
    GtkWidget *center_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(center_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(center_scroll, TRUE);
    gtk_widget_set_vexpand(center_scroll, TRUE);
    gtk_widget_set_size_request(center_scroll, 420, -1);
    gtk_widget_add_css_class(center_scroll, "affix-preview");
    gtk_box_append(GTK_BOX(panes), center_scroll);

    GtkWidget *preview_label = gtk_label_new("");
    st->preview_label = GTK_LABEL(preview_label);
    gtk_label_set_use_markup(GTK_LABEL(preview_label), TRUE);
    gtk_label_set_wrap(GTK_LABEL(preview_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(preview_label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(preview_label), 0.0f);
    gtk_widget_set_margin_start(preview_label, 12);
    gtk_widget_set_margin_end(preview_label, 12);
    gtk_widget_set_margin_top(preview_label, 12);
    gtk_widget_set_margin_bottom(preview_label, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(center_scroll), preview_label);


    /* -- Right pane: Suffix -- */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(right_vbox, TRUE);
    gtk_box_append(GTK_BOX(panes), right_vbox);

    GtkWidget *suffix_label = gtk_label_new("Suffix");
    gtk_widget_set_halign(suffix_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(right_vbox), suffix_label);

    GtkWidget *suffix_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(suffix_search), "Filter...");
    st->suffix_search = suffix_search;
    gtk_box_append(GTK_BOX(right_vbox), suffix_search);

    GtkWidget *suffix_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(suffix_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(suffix_scroll, TRUE);
    gtk_box_append(GTK_BOX(right_vbox), suffix_scroll);

    GtkWidget *suffix_listbox = gtk_list_box_new();
    st->suffix_listbox = GTK_LIST_BOX(suffix_listbox);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(suffix_listbox), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(suffix_scroll), suffix_listbox);


    /* -- Button bar -- */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(btn_box, 8);
    gtk_widget_set_margin_bottom(btn_box, 8);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    GtkWidget *apply_btn = gtk_button_new_with_label("Apply");
    gtk_widget_add_css_class(apply_btn, "suggested-action");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_affix_dialog_apply), st);
    gtk_box_append(GTK_BOX(btn_box), apply_btn);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_affix_dialog_cancel), st);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);


    /* -- Populate prefix listbox -- */
    float prefix_total_w = 0;
    for (int i = 0; i < affixes->prefixes.count; i++)
        prefix_total_w += affixes->prefixes.entries[i].weight;


    const char *cur_prefix = st->orig_prefix;
    bool none_is_current = !cur_prefix || !cur_prefix[0];
    GtkWidget *none_row = make_affix_row(NULL, NULL, 0, none_is_current, false, NULL);
    gtk_list_box_append(GTK_LIST_BOX(prefix_listbox), none_row);
    GtkWidget *prefix_select_row = none_is_current ? none_row : NULL;


    for (int i = 0; i < affixes->prefixes.count; i++) {
        TQAffixEntry *e = &affixes->prefixes.entries[i];
        bool is_cur = cur_prefix && strcasecmp(cur_prefix, e->affix_path) == 0;
        float pct = prefix_total_w > 0 ? (e->weight / prefix_total_w) * 100.0f : 0;

        bool has_sibling =
            (i > 0 && strcasecmp(e->translation,
                affixes->prefixes.entries[i-1].translation) == 0) ||
            (i + 1 < affixes->prefixes.count && strcasecmp(e->translation,
                affixes->prefixes.entries[i+1].translation) == 0);

        GtkWidget *r = make_affix_row(e->translation, e->affix_path, pct,
                                       is_cur, has_sibling, e->affix_path);
        gtk_list_box_append(GTK_LIST_BOX(prefix_listbox), r);
        if (is_cur) prefix_select_row = r;
    }


    /* -- Populate suffix listbox -- */
    float suffix_total_w = 0;
    for (int i = 0; i < affixes->suffixes.count; i++)
        suffix_total_w += affixes->suffixes.entries[i].weight;

    const char *cur_suffix = st->orig_suffix;
    none_is_current = !cur_suffix || !cur_suffix[0];
    none_row = make_affix_row(NULL, NULL, 0, none_is_current, false, NULL);
    gtk_list_box_append(GTK_LIST_BOX(suffix_listbox), none_row);
    GtkWidget *suffix_select_row = none_is_current ? none_row : NULL;


    for (int i = 0; i < affixes->suffixes.count; i++) {
        TQAffixEntry *e = &affixes->suffixes.entries[i];
        bool is_cur = cur_suffix && strcasecmp(cur_suffix, e->affix_path) == 0;
        float pct = suffix_total_w > 0 ? (e->weight / suffix_total_w) * 100.0f : 0;

        bool has_sibling =
            (i > 0 && strcasecmp(e->translation,
                affixes->suffixes.entries[i-1].translation) == 0) ||
            (i + 1 < affixes->suffixes.count && strcasecmp(e->translation,
                affixes->suffixes.entries[i+1].translation) == 0);

        GtkWidget *r = make_affix_row(e->translation, e->affix_path, pct,
                                       is_cur, has_sibling, e->affix_path);
        gtk_list_box_append(GTK_LIST_BOX(suffix_listbox), r);
        if (is_cur) suffix_select_row = r;
    }


    /* Connect signals AFTER populating to avoid spurious callbacks */
    g_signal_connect(prefix_listbox, "row-selected",
                     G_CALLBACK(on_prefix_row_selected), st);
    g_signal_connect(suffix_listbox, "row-selected",
                     G_CALLBACK(on_suffix_row_selected), st);

    gtk_list_box_set_filter_func(GTK_LIST_BOX(prefix_listbox),
                                  affix_filter_func, prefix_search, NULL);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(suffix_listbox),
                                  affix_filter_func, suffix_search, NULL);

    g_signal_connect(prefix_search, "search-changed",
                     G_CALLBACK(on_prefix_search_changed), st);
    g_signal_connect(suffix_search, "search-changed",
                     G_CALLBACK(on_suffix_search_changed), st);


    /* Pre-select current affix rows */
    if (prefix_select_row)
        gtk_list_box_select_row(GTK_LIST_BOX(prefix_listbox),
                                GTK_LIST_BOX_ROW(prefix_select_row));
    if (suffix_select_row)
        gtk_list_box_select_row(GTK_LIST_BOX(suffix_listbox),
                                GTK_LIST_BOX_ROW(suffix_select_row));


    /* Generate initial preview */
    update_affix_preview(st);


    gtk_window_present(GTK_WINDOW(dialog));
}
