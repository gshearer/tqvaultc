/**
 * ui_quest_dialog.c — Quest management dialog
 *
 * Allows toggling quest completion status per-difficulty.
 * Quest state is stored in QuestToken.myw files as a flat token bag.
 */

#include "ui.h"
#include "quest_tokens.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Dialog state ─────────────────────────────────────────────────────── */

typedef struct {
    AppWidgets *widgets;
    GtkWidget *dialog;
    GtkWidget *diff_buttons[NUM_DIFFICULTIES];
    GtkWidget *notebook;
    QuestTokenSet sets[NUM_DIFFICULTIES];
    bool sets_loaded[NUM_DIFFICULTIES];
    QuestDifficulty current_diff;

    /* Per-quest checkboxes indexed [quest_def_index] — only for current difficulty view */
    GtkWidget **check_buttons;  /* array of quest_count GtkCheckButtons */
    int quest_count;
} QuestDialogState;

static void quest_dialog_state_free(gpointer data) {
    QuestDialogState *st = data;
    for (int d = 0; d < NUM_DIFFICULTIES; d++) {
        if (st->sets_loaded[d])
            quest_token_set_free(&st->sets[d]);
    }
    free(st->check_buttons);
    g_free(st);
}

/* ── Load token sets for all available difficulties ───────────────────── */

static void load_all_difficulties(QuestDialogState *st) {
    if (!st->widgets->current_character) return;
    const char *filepath = st->widgets->current_character->filepath;

    for (int d = 0; d < NUM_DIFFICULTIES; d++) {
        char *path = quest_token_path(filepath, (QuestDifficulty)d);
        if (!path) continue;

        if (access(path, F_OK) == 0) {
            if (quest_tokens_load(path, &st->sets[d]) == 0) {
                st->sets_loaded[d] = true;
            }
        }
        free(path);
    }
}

/* ── Update checkbox states from token set ────────────────────────────── */

static void update_checkboxes(QuestDialogState *st) {
    int count;
    const QuestDef *defs = quest_get_defs(&count);
    QuestDifficulty d = st->current_diff;

    bool loaded = st->sets_loaded[d];

    for (int i = 0; i < count && i < st->quest_count; i++) {
        GtkWidget *cb = st->check_buttons[i];
        if (!cb) continue;

        if (!loaded) {
            gtk_widget_set_sensitive(cb, FALSE);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), FALSE);
        } else {
            gtk_widget_set_sensitive(cb, TRUE);
            bool complete = quest_token_set_contains(&st->sets[d], defs[i].completion_token);
            /* Block signal during programmatic update */
            g_signal_handlers_block_matched(cb, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, st);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), complete);
            g_signal_handlers_unblock_matched(cb, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, st);
        }
    }
}

/* ── Difficulty radio button toggled ──────────────────────────────────── */

static void on_diff_toggled(GtkCheckButton *btn, gpointer data) {
    QuestDialogState *st = data;
    if (!gtk_check_button_get_active(btn)) return; /* ignore deactivation */

    for (int d = 0; d < NUM_DIFFICULTIES; d++) {
        if (GTK_WIDGET(btn) == st->diff_buttons[d]) {
            st->current_diff = (QuestDifficulty)d;
            break;
        }
    }
    update_checkboxes(st);
}

/* ── Quest checkbox toggled ───────────────────────────────────────────── */

static void on_quest_toggled(GtkCheckButton *btn, gpointer data) {
    QuestDialogState *st = data;
    QuestDifficulty d = st->current_diff;
    if (!st->sets_loaded[d]) return;

    /* Find which quest this checkbox belongs to */
    int count;
    const QuestDef *defs = quest_get_defs(&count);
    int qi = -1;
    for (int i = 0; i < count; i++) {
        if (st->check_buttons[i] == GTK_WIDGET(btn)) {
            qi = i;
            break;
        }
    }
    if (qi < 0) return;

    bool checked = gtk_check_button_get_active(btn);
    const QuestDef *qd = &defs[qi];

    if (checked) {
        /* Add ALL tokens for this quest */
        for (const char **t = qd->tokens; *t; t++)
            quest_token_set_add(&st->sets[d], *t);
    } else {
        /* Remove ALL tokens for this quest */
        for (const char **t = qd->tokens; *t; t++)
            quest_token_set_remove(&st->sets[d], *t);
    }
}

/* ── Complete All / Clear All for current notebook tab ────────────────── */

static void batch_set_tab(QuestDialogState *st, QuestAct act, bool complete) {
    QuestDifficulty d = st->current_diff;
    if (!st->sets_loaded[d]) return;

    int count;
    const QuestDef *defs = quest_get_defs(&count);

    for (int i = 0; i < count; i++) {
        if (defs[i].act != act) continue;
        if (complete) {
            for (const char **t = defs[i].tokens; *t; t++)
                quest_token_set_add(&st->sets[d], *t);
        } else {
            for (const char **t = defs[i].tokens; *t; t++)
                quest_token_set_remove(&st->sets[d], *t);
        }
    }
    update_checkboxes(st);
}

static void on_complete_all(GtkButton *btn, gpointer data) {
    (void)btn;
    QuestDialogState *st = data;
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(st->notebook));
    if (page >= 0 && page < NUM_ACTS)
        batch_set_tab(st, (QuestAct)page, true);
}

static void on_clear_all(GtkButton *btn, gpointer data) {
    (void)btn;
    QuestDialogState *st = data;
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(st->notebook));
    if (page >= 0 && page < NUM_ACTS)
        batch_set_tab(st, (QuestAct)page, false);
}

/* ── Apply: save all dirty sets ───────────────────────────────────────── */

static void on_quest_apply(GtkButton *btn, gpointer data) {
    (void)btn;
    QuestDialogState *st = data;
    const char *filepath = st->widgets->current_character->filepath;
    int errors = 0;

    for (int d = 0; d < NUM_DIFFICULTIES; d++) {
        if (!st->sets_loaded[d] || !st->sets[d].dirty) continue;

        char *path = quest_token_path(filepath, (QuestDifficulty)d);
        if (!path) continue;

        if (quest_tokens_save(path, &st->sets[d]) == 0) {
            st->sets[d].dirty = false;
        } else {
            errors++;
            fprintf(stderr, "quest_tokens_save failed: %s\n", path);
        }
        free(path);
    }

    if (errors == 0) {
        gtk_window_destroy(GTK_WINDOW(st->dialog));
    }
}

static void on_quest_cancel(GtkButton *btn, gpointer data) {
    (void)btn;
    QuestDialogState *st = data;
    gtk_window_destroy(GTK_WINDOW(st->dialog));
}

/* ── Build one notebook tab for an act ────────────────────────────────── */

static GtkWidget *build_act_tab(QuestDialogState *st, QuestAct act) {
    int count;
    const QuestDef *defs = quest_get_defs(&count);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);

    /* Section headers + area headers + checkboxes */
    bool added_main_header = false;
    bool added_side_header = false;
    const char *last_area = NULL;

    for (int i = 0; i < count; i++) {
        if (defs[i].act != act) continue;

        if (defs[i].is_main && !added_main_header) {
            GtkWidget *lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(lbl), "<b>Main Quests</b>");
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_start(lbl, 8);
            gtk_widget_set_margin_top(lbl, 4);
            gtk_widget_set_margin_bottom(lbl, 2);
            gtk_list_box_append(GTK_LIST_BOX(listbox), lbl);
            added_main_header = true;
        }
        if (!defs[i].is_main && !added_side_header) {
            GtkWidget *lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(lbl), "<b>Side Quests</b>");
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_start(lbl, 8);
            gtk_widget_set_margin_top(lbl, 8);
            gtk_widget_set_margin_bottom(lbl, 2);
            gtk_list_box_append(GTK_LIST_BOX(listbox), lbl);
            added_side_header = true;
        }

        /* Area header for side quests — insert when area changes */
        if (!defs[i].is_main && defs[i].area &&
            (!last_area || strcmp(defs[i].area, last_area) != 0)) {
            last_area = defs[i].area;
            char markup[128];
            snprintf(markup, sizeof(markup), "<i>%s</i>", defs[i].area);
            GtkWidget *area_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(area_lbl), markup);
            gtk_widget_set_halign(area_lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_start(area_lbl, 12);
            gtk_widget_set_margin_top(area_lbl, 6);
            gtk_widget_set_margin_bottom(area_lbl, 1);
            gtk_list_box_append(GTK_LIST_BOX(listbox), area_lbl);
        }

        GtkWidget *cb = gtk_check_button_new_with_label(defs[i].name);
        gtk_widget_set_margin_start(cb, 24);
        /* Store tooltip showing the completion token for debugging */
        gtk_widget_set_tooltip_text(cb, defs[i].completion_token);
        st->check_buttons[i] = cb;
        g_signal_connect(cb, "toggled", G_CALLBACK(on_quest_toggled), st);
        gtk_list_box_append(GTK_LIST_BOX(listbox), cb);
    }

    return scroll;
}

/* ── Public entry point ───────────────────────────────────────────────── */

void show_quest_dialog(AppWidgets *widgets) {
    if (!widgets->current_character) return;

    /* Allocate state */
    QuestDialogState *st = g_new0(QuestDialogState, 1);
    st->widgets = widgets;

    int count;
    quest_get_defs(&count);
    st->quest_count = count;
    st->check_buttons = calloc(count, sizeof(GtkWidget *));

    /* Load quest data for all difficulties */
    load_all_difficulties(st);

    /* Find default difficulty: highest loaded */
    st->current_diff = DIFF_NORMAL;
    for (int d = NUM_DIFFICULTIES - 1; d >= 0; d--) {
        if (st->sets_loaded[d]) {
            st->current_diff = (QuestDifficulty)d;
            break;
        }
    }

    /* Build dialog */
    GtkWidget *dialog = gtk_window_new();
    st->dialog = dialog;

    char title[256];
    snprintf(title, sizeof(title), "Manage Quests — %s",
             widgets->current_character->character_name
             ? widgets->current_character->character_name : "Character");
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
    gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 550);
    gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);

    g_object_set_data_full(G_OBJECT(dialog), "quest-state", st, quest_dialog_state_free);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    /* ── Difficulty selector (radio buttons) ── */
    GtkWidget *diff_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(vbox), diff_box);

    GtkWidget *diff_label = gtk_label_new("Difficulty:");
    gtk_box_append(GTK_BOX(diff_box), diff_label);

    GtkWidget *first_radio = NULL;
    for (int d = 0; d < NUM_DIFFICULTIES; d++) {
        GtkWidget *radio = gtk_check_button_new_with_label(quest_difficulty_name((QuestDifficulty)d));
        if (first_radio)
            gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(first_radio));
        else
            first_radio = radio;

        st->diff_buttons[d] = radio;
        gtk_box_append(GTK_BOX(diff_box), radio);

        if (!st->sets_loaded[d])
            gtk_widget_set_sensitive(radio, FALSE);

        if ((QuestDifficulty)d == st->current_diff)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);

        g_signal_connect(radio, "toggled", G_CALLBACK(on_diff_toggled), st);
    }

    /* ── Notebook with act tabs ── */
    GtkWidget *notebook = gtk_notebook_new();
    st->notebook = notebook;
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(vbox), notebook);

    for (int a = 0; a < NUM_ACTS; a++) {
        GtkWidget *tab = build_act_tab(st, (QuestAct)a);
        GtkWidget *tab_label = gtk_label_new(quest_act_name((QuestAct)a));
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab, tab_label);
    }

    /* ── Bottom button bar ── */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btn_box, 8);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    GtkWidget *complete_btn = gtk_button_new_with_label("Complete All");
    g_signal_connect(complete_btn, "clicked", G_CALLBACK(on_complete_all), st);
    gtk_box_append(GTK_BOX(btn_box), complete_btn);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_all), st);
    gtk_box_append(GTK_BOX(btn_box), clear_btn);

    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(btn_box), spacer);

    GtkWidget *apply_btn = gtk_button_new_with_label("Apply");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_quest_apply), st);
    gtk_box_append(GTK_BOX(btn_box), apply_btn);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_quest_cancel), st);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);

    /* Set initial checkbox states */
    update_checkboxes(st);

    gtk_window_present(GTK_WINDOW(dialog));
}
