// ui_checklist_dialog.c -- Read-only completion checklist dialog
//
// Shows quest completion and non-quest achievements (boss chests, exploration,
// NPCs, misc) organized by category tabs, with checkboxes reflecting the
// character's current QuestToken.myw state.

#include "ui.h"
#include "quest_tokens.h"
#include <stdlib.h>
#include <string.h>

// -- Dialog state -----------------------------------------------------------

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  GtkWidget *diff_buttons[NUM_DIFFICULTIES];
  QuestTokenSet sets[NUM_DIFFICULTIES];
  bool sets_loaded[NUM_DIFFICULTIES];
  QuestDifficulty current_diff;

  GtkWidget **check_buttons;   // one per checklist extra entry
  int extra_count;

  GtkWidget **quest_check_buttons;  // one per quest def entry
  int quest_count;
} ChecklistDialogState;

// Free the checklist dialog state, releasing token sets and button arrays.
//
// data: pointer to ChecklistDialogState
static void
checklist_state_free(gpointer data)
{
  ChecklistDialogState *st = data;

  for(int d = 0; d < NUM_DIFFICULTIES; d++)
  {
    if(st->sets_loaded[d])
      quest_token_set_free(&st->sets[d]);
  }

  free(st->check_buttons);
  free(st->quest_check_buttons);
  g_free(st);
}

// -- Load token sets for all available difficulties -------------------------

// Attempt to load QuestToken.myw for each difficulty level.
// Populates st->sets[] and st->sets_loaded[] accordingly.
//
// st: dialog state with current character info
static void
load_all_difficulties(ChecklistDialogState *st)
{
  if(!st->widgets->current_character)
    return;

  const char *filepath = st->widgets->current_character->filepath;

  for(int d = 0; d < NUM_DIFFICULTIES; d++)
  {
    char *path = quest_token_path(filepath, (QuestDifficulty)d);

    if(!path)
      continue;

    if(g_file_test(path, G_FILE_TEST_EXISTS))
    {
      if(quest_tokens_load(path, &st->sets[d]) == 0)
        st->sets_loaded[d] = true;
    }

    free(path);
  }
}

// -- Resolve the token to check for a checklist extra entry -----------------

// Return the appropriate token string for a checklist entry at the given
// difficulty.  Epic/Legendary entries may have a different token than Normal.
//
// e: the checklist extra definition
// d: the difficulty level
// returns: token string, or NULL if not applicable
static const char *
extra_token_for_diff(const ChecklistExtraDef *e, QuestDifficulty d)
{
  if(d != DIFF_NORMAL && e->token_epic)
    return(e->token_epic);

  return(e->token);  // may be NULL for IT-only bosses on Normal
}

// -- Update all checkbox states from current difficulty's token set ----------

// Refresh every checkbox (both extras and quests) to reflect the current
// difficulty's token set.
//
// st: dialog state with loaded token sets and checkbox widget arrays
static void
update_checkboxes(ChecklistDialogState *st)
{
  QuestDifficulty d = st->current_diff;
  bool loaded = st->sets_loaded[d];

  int ecount;
  const ChecklistExtraDef *extras = checklist_get_extras(&ecount);

  for(int i = 0; i < ecount && i < st->extra_count; i++)
  {
    GtkWidget *cb = st->check_buttons[i];

    if(!cb)
      continue;

    if(!loaded)
    {
      gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), FALSE);
      continue;
    }

    const char *tok = extra_token_for_diff(&extras[i], d);

    if(!tok)
      gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), FALSE);
    else
    {
      bool found = quest_token_set_contains(&st->sets[d], tok);
      gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), found);
    }
  }

  // Update quest checkboxes
  int qcount;
  const QuestDef *qdefs = quest_get_defs(&qcount);

  for(int i = 0; i < qcount && i < st->quest_count; i++)
  {
    GtkWidget *cb = st->quest_check_buttons[i];

    if(!cb)
      continue;

    if(!loaded)
      gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), FALSE);
    else
    {
      bool found = quest_token_set_contains(&st->sets[d], qdefs[i].completion_token);
      gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), found);
    }
  }
}

// -- Difficulty radio button toggled ----------------------------------------

// Called when a difficulty radio button is toggled.  Updates the current
// difficulty and refreshes all checkboxes.
//
// btn:  the toggled check button
// data: ChecklistDialogState pointer
static void
on_diff_toggled(GtkCheckButton *btn, gpointer data)
{
  ChecklistDialogState *st = data;

  if(!gtk_check_button_get_active(btn))
    return;

  for(int d = 0; d < NUM_DIFFICULTIES; d++)
  {
    if(GTK_WIDGET(btn) == st->diff_buttons[d])
    {
      st->current_diff = (QuestDifficulty)d;
      break;
    }
  }

  update_checkboxes(st);
}

// -- Build a category tab (Boss Chests / Exploration / NPCs / Misc) ---------

// Create a scrolled list of checkboxes for a single non-quest category,
// grouped by act with header labels.
//
// st:  dialog state with checkbox widget array to populate
// cat: which category to build
// returns: the scrolled window widget
static GtkWidget *
build_category_tab(ChecklistDialogState *st, ChecklistCategory cat)
{
  int ecount;
  const ChecklistExtraDef *extras = checklist_get_extras(&ecount);

  GtkWidget *scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *listbox = gtk_list_box_new();

  gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);

  QuestAct last_act = -1;
  bool first = true;

  for(int i = 0; i < ecount; i++)
  {
    if(extras[i].category != cat)
      continue;

    // Act header when act changes
    if(extras[i].act != last_act)
    {
      last_act = extras[i].act;
      char markup[128];

      snprintf(markup, sizeof(markup), "<b>%s</b>", quest_act_name(last_act));
      GtkWidget *lbl = gtk_label_new(NULL);

      gtk_label_set_markup(GTK_LABEL(lbl), markup);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      gtk_widget_set_margin_start(lbl, 8);
      gtk_widget_set_margin_top(lbl, first ? 4 : 10);
      gtk_widget_set_margin_bottom(lbl, 2);
      gtk_list_box_append(GTK_LIST_BOX(listbox), lbl);
      first = false;
    }

    GtkWidget *cb = gtk_check_button_new_with_label(extras[i].name);

    gtk_widget_set_margin_start(cb, 24);
    gtk_widget_set_sensitive(cb, FALSE);  // read-only

    // Tooltip: show the token name
    const char *tok = extras[i].token ? extras[i].token : extras[i].token_epic;

    if(tok)
      gtk_widget_set_tooltip_text(cb, tok);

    st->check_buttons[i] = cb;
    gtk_list_box_append(GTK_LIST_BOX(listbox), cb);
  }

  return(scroll);
}

// -- Build a quest tab (Main Quests or Side Quests) -------------------------

// Create a scrolled list of checkboxes for main or side quests, grouped
// by act (and area for side quests) with header labels.
//
// st:      dialog state with quest checkbox widget array to populate
// is_main: true for main quests, false for side quests
// returns: the scrolled window widget
static GtkWidget *
build_quest_tab(ChecklistDialogState *st, bool is_main)
{
  int qcount;
  const QuestDef *qdefs = quest_get_defs(&qcount);

  GtkWidget *scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *listbox = gtk_list_box_new();

  gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);

  QuestAct last_act = -1;
  const char *last_area = NULL;
  bool first = true;

  for(int i = 0; i < qcount; i++)
  {
    if(qdefs[i].is_main != is_main)
      continue;

    // Act header when act changes
    if(qdefs[i].act != last_act)
    {
      last_act = qdefs[i].act;
      last_area = NULL;
      char markup[128];

      snprintf(markup, sizeof(markup), "<b>%s</b>", quest_act_name(last_act));
      GtkWidget *lbl = gtk_label_new(NULL);

      gtk_label_set_markup(GTK_LABEL(lbl), markup);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      gtk_widget_set_margin_start(lbl, 8);
      gtk_widget_set_margin_top(lbl, first ? 4 : 10);
      gtk_widget_set_margin_bottom(lbl, 2);
      gtk_list_box_append(GTK_LIST_BOX(listbox), lbl);
      first = false;
    }

    // Area sub-header for side quests
    if(!is_main && qdefs[i].area &&
       (!last_area || strcmp(qdefs[i].area, last_area) != 0))
    {
      last_area = qdefs[i].area;
      char markup[128];

      snprintf(markup, sizeof(markup), "<i>%s</i>", last_area);
      GtkWidget *lbl = gtk_label_new(NULL);

      gtk_label_set_markup(GTK_LABEL(lbl), markup);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      gtk_widget_set_margin_start(lbl, 16);
      gtk_widget_set_margin_top(lbl, 4);
      gtk_list_box_append(GTK_LIST_BOX(listbox), lbl);
    }

    GtkWidget *cb = gtk_check_button_new_with_label(qdefs[i].name);

    gtk_widget_set_margin_start(cb, is_main ? 24 : 32);
    gtk_widget_set_sensitive(cb, FALSE);  // read-only

    if(qdefs[i].completion_token)
      gtk_widget_set_tooltip_text(cb, qdefs[i].completion_token);

    st->quest_check_buttons[i] = cb;
    gtk_list_box_append(GTK_LIST_BOX(listbox), cb);
  }

  return(scroll);
}

// -- Public entry point -----------------------------------------------------

// Create and present the completion checklist dialog for the current
// character.  Shows quest and non-quest achievements organized by
// category tabs, with difficulty radio buttons.
//
// widgets: application state with current character loaded
void
show_checklist_dialog(AppWidgets *widgets)
{
  if(!widgets->current_character)
    return;

  ChecklistDialogState *st = g_new0(ChecklistDialogState, 1);

  st->widgets = widgets;

  int ecount;

  checklist_get_extras(&ecount);
  st->extra_count = ecount;
  st->check_buttons = calloc(ecount, sizeof(GtkWidget *));

  if(!st->check_buttons)
  {
    g_free(st);
    return;
  }

  int qcount;

  quest_get_defs(&qcount);
  st->quest_count = qcount;
  st->quest_check_buttons = calloc(qcount, sizeof(GtkWidget *));

  if(!st->quest_check_buttons)
  {
    free(st->check_buttons);
    g_free(st);
    return;
  }

  load_all_difficulties(st);

  // Default to highest loaded difficulty
  st->current_diff = DIFF_NORMAL;

  for(int d = NUM_DIFFICULTIES - 1; d >= 0; d--)
  {
    if(st->sets_loaded[d])
    {
      st->current_diff = (QuestDifficulty)d;
      break;
    }
  }

  // Build dialog
  GtkWidget *dialog = gtk_window_new();

  st->dialog = dialog;

  char title[256];

  snprintf(title, sizeof(title), "Completion Checklist \u2014 %s",
           widgets->current_character->character_name
           ? widgets->current_character->character_name : "Character");
  gtk_window_set_title(GTK_WINDOW(dialog), title);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 500, 450);
  gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);

  g_object_set_data_full(G_OBJECT(dialog), "checklist-state", st, checklist_state_free);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);
  gtk_window_set_child(GTK_WINDOW(dialog), vbox);

  // -- Difficulty selector (radio buttons) --
  GtkWidget *diff_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_box_append(GTK_BOX(vbox), diff_box);

  GtkWidget *diff_label = gtk_label_new("Difficulty:");

  gtk_box_append(GTK_BOX(diff_box), diff_label);

  GtkWidget *first_radio = NULL;

  for(int d = 0; d < NUM_DIFFICULTIES; d++)
  {
    GtkWidget *radio = gtk_check_button_new_with_label(
        quest_difficulty_name((QuestDifficulty)d));

    if(first_radio)
      gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(first_radio));
    else
      first_radio = radio;

    st->diff_buttons[d] = radio;
    gtk_box_append(GTK_BOX(diff_box), radio);

    if(!st->sets_loaded[d])
      gtk_widget_set_sensitive(radio, FALSE);

    if((QuestDifficulty)d == st->current_diff)
      gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);

    g_signal_connect(radio, "toggled", G_CALLBACK(on_diff_toggled), st);
  }

  // -- Notebook with category tabs --
  GtkWidget *notebook = gtk_notebook_new();

  gtk_widget_set_vexpand(notebook, TRUE);
  gtk_box_append(GTK_BOX(vbox), notebook);

  // Quest tabs first
  GtkWidget *main_tab = build_quest_tab(st, true);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), main_tab,
                            gtk_label_new("Main Quests"));

  GtkWidget *side_tab = build_quest_tab(st, false);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), side_tab,
                            gtk_label_new("Side Quests"));

  // Non-quest category tabs
  for(int c = 0; c < NUM_CHECK_CATEGORIES; c++)
  {
    GtkWidget *tab = build_category_tab(st, (ChecklistCategory)c);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab,
                              gtk_label_new(checklist_category_name((ChecklistCategory)c)));
  }

  // -- Bottom bar: just a Close button --
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_margin_top(btn_box, 8);
  gtk_box_append(GTK_BOX(vbox), btn_box);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(btn_box), spacer);

  GtkWidget *close_btn = gtk_button_new_with_label("Close");

  g_signal_connect_swapped(close_btn, "clicked",
                            G_CALLBACK(gtk_window_destroy), dialog);
  gtk_box_append(GTK_BOX(btn_box), close_btn);

  // Set initial checkbox states
  update_checkboxes(st);

  gtk_window_present(GTK_WINDOW(dialog));
}
