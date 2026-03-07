/**
 * ui_stats_dialog.c — Attributes dialog (+/- buttons for str/dex/int/health/mana)
 *
 * Each row shows points spent; available points displayed at top.
 */

#include "ui.h"
#include <stdio.h>
#include <math.h>

/* ── Game constants ───────────────────────────────────────────────────── */

#define MIN_STR   50.0f
#define MIN_DEX   50.0f
#define MIN_INT   50.0f
#define MIN_HP   300.0f
#define MIN_MP   300.0f

#define INC_ATTR   4    /* str/dex/int gain per attribute point spent */
#define INC_HPMP  40    /* health/mana gain per attribute point spent */

#define NUM_ATTRS 5

/* ── Dialog state ─────────────────────────────────────────────────────── */

typedef struct {
    AppWidgets *widgets;
    GtkWidget *dialog;

    /* Original values (for cancel) */
    float orig_str, orig_dex, orig_int, orig_hp, orig_mp;
    uint32_t orig_modifier_points;

    /* Total earned attribute points (immutable) */
    int total_earned;

    /* Current points spent per stat */
    int pts[NUM_ATTRS];  /* 0=str, 1=dex, 2=int, 3=hp, 4=mp */

    /* Widgets per stat row */
    GtkWidget *val_labels[NUM_ATTRS];
    GtkWidget *minus_btns[NUM_ATTRS];
    GtkWidget *plus_btns[NUM_ATTRS];

    /* Available points display */
    GtkWidget *avail_label;
} StatsDialogState;

static void stats_state_free(gpointer data) {
    g_free(data);
}

/* ── Compute available points ─────────────────────────────────────────── */

static int compute_avail(StatsDialogState *st) {
    int spent = 0;
    for (int i = 0; i < NUM_ATTRS; i++)
        spent += st->pts[i];
    return st->total_earned - spent;
}

/* ── Refresh all labels and button sensitivity ────────────────────────── */

static void refresh_display(StatsDialogState *st) {
    int avail = compute_avail(st);

    char buf[64];
    snprintf(buf, sizeof(buf), "Available Attribute Points: %d", avail);
    gtk_label_set_text(GTK_LABEL(st->avail_label), buf);

    for (int i = 0; i < NUM_ATTRS; i++) {
        snprintf(buf, sizeof(buf), "%d", st->pts[i]);
        gtk_label_set_text(GTK_LABEL(st->val_labels[i]), buf);

        gtk_widget_set_sensitive(st->minus_btns[i], st->pts[i] > 0);
        gtk_widget_set_sensitive(st->plus_btns[i], avail > 0);
    }
}

/* ── Plus/minus button callbacks ──────────────────────────────────────── */

static void on_plus_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    StatsDialogState *st = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "attr-idx"));
    if (compute_avail(st) > 0) {
        st->pts[idx]++;
        refresh_display(st);
    }
}

static void on_minus_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    StatsDialogState *st = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "attr-idx"));
    if (st->pts[idx] > 0) {
        st->pts[idx]--;
        refresh_display(st);
    }
}

/* ── Apply: convert points back to stat values and save ───────────────── */

static void on_apply_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    StatsDialogState *st = user_data;
    TQCharacter *chr = st->widgets->current_character;
    if (!chr) return;

    chr->strength     = MIN_STR + (float)(st->pts[0] * INC_ATTR);
    chr->dexterity    = MIN_DEX + (float)(st->pts[1] * INC_ATTR);
    chr->intelligence = MIN_INT + (float)(st->pts[2] * INC_ATTR);
    chr->health       = MIN_HP  + (float)(st->pts[3] * INC_HPMP);
    chr->mana         = MIN_MP  + (float)(st->pts[4] * INC_HPMP);
    chr->modifier_points = (uint32_t)compute_avail(st);

    if (character_save_stats(chr) == 0) {
        update_ui(st->widgets, chr);
    } else {
        fprintf(stderr, "Attributes: failed to save stats\n");
    }

    gtk_window_close(GTK_WINDOW(st->dialog));
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    StatsDialogState *st = user_data;
    gtk_window_close(GTK_WINDOW(st->dialog));
}

/* ── Build a single attribute row: Label  [-] [number] [+] ───────────── */

static GtkWidget *make_attr_row(const char *label_text, int idx,
                                 StatsDialogState *st) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0);
    gtk_widget_set_size_request(label, 100, -1);
    gtk_box_append(GTK_BOX(row), label);

    /* Spacer to push controls right */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), spacer);

    GtkWidget *minus_btn = gtk_button_new_with_label("\u2212"); /* minus sign */
    g_object_set_data(G_OBJECT(minus_btn), "attr-idx", GINT_TO_POINTER(idx));
    g_signal_connect(minus_btn, "clicked", G_CALLBACK(on_minus_clicked), st);
    gtk_box_append(GTK_BOX(row), minus_btn);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", st->pts[idx]);
    GtkWidget *val_label = gtk_label_new(buf);
    gtk_label_set_xalign(GTK_LABEL(val_label), 0.5);
    gtk_widget_set_size_request(val_label, 40, -1);
    gtk_box_append(GTK_BOX(row), val_label);

    GtkWidget *plus_btn = gtk_button_new_with_label("+");
    g_object_set_data(G_OBJECT(plus_btn), "attr-idx", GINT_TO_POINTER(idx));
    g_signal_connect(plus_btn, "clicked", G_CALLBACK(on_plus_clicked), st);
    gtk_box_append(GTK_BOX(row), plus_btn);

    st->val_labels[idx] = val_label;
    st->minus_btns[idx] = minus_btn;
    st->plus_btns[idx]  = plus_btn;

    return row;
}

/* ── Public entry point ───────────────────────────────────────────────── */

void show_stats_dialog(AppWidgets *widgets) {
    TQCharacter *chr = widgets->current_character;
    if (!chr) return;

    if (!chr->off_strength || !chr->off_modifier_points) {
        fprintf(stderr, "Attributes: character stat offsets not available\n");
        return;
    }

    StatsDialogState *st = g_new0(StatsDialogState, 1);
    st->widgets = widgets;

    /* Save originals for cancel */
    st->orig_str = chr->strength;
    st->orig_dex = chr->dexterity;
    st->orig_int = chr->intelligence;
    st->orig_hp  = chr->health;
    st->orig_mp  = chr->mana;
    st->orig_modifier_points = chr->modifier_points;

    /* Convert raw stat values to points spent */
    st->pts[0] = (int)round((chr->strength     - MIN_STR) / INC_ATTR);
    st->pts[1] = (int)round((chr->dexterity    - MIN_DEX) / INC_ATTR);
    st->pts[2] = (int)round((chr->intelligence - MIN_INT) / INC_ATTR);
    st->pts[3] = (int)round((chr->health       - MIN_HP)  / INC_HPMP);
    st->pts[4] = (int)round((chr->mana         - MIN_MP)  / INC_HPMP);

    int spent = 0;
    for (int i = 0; i < NUM_ATTRS; i++) spent += st->pts[i];
    st->total_earned = (int)chr->modifier_points + spent;

    /* Build dialog */
    GtkWidget *dialog = gtk_window_new();
    st->dialog = dialog;

    char title[256];
    snprintf(title, sizeof(title), "Attributes \u2014 %s",
             chr->character_name ? chr->character_name : "Character");
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    g_object_set_data_full(G_OBJECT(dialog), "stats-state", st, stats_state_free);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    /* Available points label */
    st->avail_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(st->avail_label), 0.0);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.1));
    gtk_label_set_attributes(GTK_LABEL(st->avail_label), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_append(GTK_BOX(vbox), st->avail_label);

    static const char *attr_names[] = {
        "Strength:", "Dexterity:", "Intelligence:", "Health:", "Energy:"
    };
    for (int i = 0; i < NUM_ATTRS; i++)
        gtk_box_append(GTK_BOX(vbox), make_attr_row(attr_names[i], i, st));

    /* ── Button bar ───────────────────────────────────────────────── */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    GtkWidget *apply_btn = gtk_button_new_with_label("Apply");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_clicked), st);
    gtk_box_append(GTK_BOX(btn_box), apply_btn);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), st);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);

    /* Initial display */
    refresh_display(st);

    gtk_window_present(GTK_WINDOW(dialog));
}
