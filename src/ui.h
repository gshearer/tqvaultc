#ifndef UI_H
#define UI_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include "character.h"
#include "vault.h"
#include "translation.h"

/* ── Shared enums ──────────────────────────────────────────────────────── */

typedef enum {
    CONTAINER_NONE, CONTAINER_VAULT, CONTAINER_INV, CONTAINER_BAG, CONTAINER_EQUIP
} ContainerType;

/* ── Held item (click-to-move) ─────────────────────────────────────────── */

typedef struct {
    TQVaultItem item;        /* deep copy of picked-up item */
    ContainerType source;
    int source_sack_idx;     /* vault sack or char bag index */
    int source_equip_slot;
    GdkPixbuf *texture;      /* cached for cursor drawing */
    int item_w, item_h;      /* cell dimensions */
    bool is_copy;            /* true = created by Copy/Duplicate, discard on cancel */
} HeldItem;

/* ── Equipment slot layout ─────────────────────────────────────────────── */

/* Pixels reserved below each slot box for the slot name label */
#define EQUIP_LABEL_H   14.0
/* Horizontal gap between the three columns (px) */
#define EQUIP_COL_GAP    4.0
/* Vertical gap between slots within the same column (px) */
#define EQUIP_SLOT_GAP   4.0

typedef struct {
    int        slot_idx;
    const char *label;
    int        box_w;   /* slot box width  in cells */
    int        box_h;   /* slot box height in cells */
} EquipSlot;

extern const EquipSlot COL_LEFT[3];
extern const EquipSlot COL_CENTER[4];
extern const EquipSlot RING_SLOTS[2];
extern const EquipSlot COL_RIGHT[3];

/* ── Global grid sizing ────────────────────────────────────────────────── */

/* Vertical overhead in the vault column (combo + bag buttons + spacing). */
#define VAULT_V_OVERHEAD 90
/* Horizontal gaps: vault/char margin + inv_bag grid column gap. */
#define LAYOUT_H_OVERHEAD 20
/* Vault grid dimensions */
#define VAULT_COLS 18
#define VAULT_ROWS 20

/* ── Main application widget state ─────────────────────────────────────── */

typedef struct {
    GtkWidget *main_window;
    GtkWidget *name_label;
    GtkWidget *level_label;
    GtkWidget *mastery1_label;
    GtkWidget *mastery2_label;
    GtkWidget *strength_label;
    GtkWidget *dexterity_label;
    GtkWidget *intelligence_label;
    GtkWidget *health_label;
    GtkWidget *mana_label;
    GtkWidget *deaths_label;
    GtkWidget *kills_label;
    GtkWidget *vault_drawing_area;
    GtkWidget *character_combo;
    GtkWidget *vault_combo;
    gulong vault_combo_handler;
    gulong char_combo_handler;
    int current_sack;
    GtkWidget *equip_drawing_area;
    GtkWidget *inv_drawing_area;      /* 12x5 character main inventory */
    GtkWidget *bag_drawing_area;      /* 8x5 extra bag (current_char_bag) */
    int current_char_bag;             /* 0-2 which extra bag is shown */
    TQCharacter *current_character;
    TQVault *current_vault;
    TQTranslation *translations;
    GHashTable *texture_cache;

    /* Tooltip caches */
    TQVaultItem *last_tooltip_item;
    char last_tooltip_markup[16384];

    TQVaultItem *last_inv_tooltip_item;
    char last_inv_tooltip_markup[16384];

    TQVaultItem *last_bag_tooltip_item;
    char last_bag_tooltip_markup[16384];

    int last_equip_tooltip_slot;      /* -1 = none */
    char last_equip_tooltip_markup[16384];

    /* Resistance table */
    GtkWidget *resist_grid;
    GtkWidget *resist_cells[14][9]; /* rows: 12 slots + 2 totals; cols: 9 resistance types */

    /* Secondary Resistances table — 8 columns */
    GtkWidget *secresist_grid;
    GtkWidget *secresist_cells[14][8];

    /* Bonus Damage table — 11 columns (percentage only) */
    GtkWidget *bdmg_grid;
    GtkWidget *bdmg_cells[14][11];

    /* Pet Bonus Damage table — 10 columns (percentage only) */
    GtkWidget *petdmg_grid;
    GtkWidget *petdmg_cells[14][10];

    /* Bonus Speed table — 7 columns (percentage only) */
    GtkWidget *bspd_grid;
    GtkWidget *bspd_cells[14][7];

    /* Health / Energy / Ability table — 10 columns */
    GtkWidget *hea_grid;
    GtkWidget *hea_cells[14][10];

    GtkWidget *main_hbox;   /* top-level horizontal split, for cell size computation */

    /* Bag button textures: pre-rendered numbered pixbufs for each state */
    GdkPixbuf *vault_bag_pix[3][12];  /* [state][bag_idx] -- 0=down, 1=up, 2=over */
    GtkWidget *vault_bag_btns[12];
    GdkPixbuf *char_bag_pix[3][3];    /* [state][bag_idx] */
    GtkWidget *char_bag_btns[3];

    /* Click-to-move state */
    HeldItem *held_item;
    double cursor_x, cursor_y;
    GtkWidget *cursor_widget;   /* which drawing area the cursor is over */
    bool vault_dirty;
    bool char_dirty;

    /* Right-click context menu */
    GMenu *context_menu_model;       /* GMenu rebuilt before each show */
    GtkWidget *context_menu;         /* GtkPopoverMenu for right-click */
    TQVaultItem *context_item;       /* item under the right-click (pointer into sack) */
    TQItem *context_equip_item;      /* equipment item under right-click */
    ContainerType context_source;    /* which container the item is in */
    int context_sack_idx;            /* sack index */
    int context_equip_slot;          /* equipment slot (-1 if not equipment) */
    GtkWidget *context_parent;       /* drawing area the menu is attached to */

    /* Instant tooltip (replaces GTK4 built-in 500ms delayed tooltips) */
    GtkWidget *tooltip_popover;
    GtkWidget *tooltip_label;
    GtkWidget *tooltip_parent;       /* current parent drawing area */

    /* Save/Refresh buttons */
    GtkWidget *save_char_btn;

    /* Search */
    GtkWidget *search_entry;
    char search_text[256];          /* lowercased search term, empty = no search */
    bool vault_sack_match[12];      /* per-sack: any items match? */
    bool char_sack_match[4];        /* per-char-inv-sack: any items match? */
} AppWidgets;

/* ── Functions shared across ui modules (defined in ui.c) ──────────────── */

void invalidate_tooltips(AppWidgets *widgets);
void queue_redraw_all(AppWidgets *widgets);
void queue_redraw_equip(AppWidgets *widgets);
void save_vault_if_dirty(AppWidgets *widgets);
void save_character_if_dirty(AppWidgets *widgets);
void update_save_button_sensitivity(AppWidgets *widgets);
void repopulate_vault_combo(AppWidgets *widgets, const char *select_name);
void repopulate_character_combo(AppWidgets *widgets, const char *select_name);
void copy_item_to_cursor(AppWidgets *widgets, TQVaultItem *src, bool is_copy);
void copy_equip_to_cursor(AppWidgets *widgets, TQItem *eq, bool is_copy);

bool item_is_relic_or_charm(const char *base_name);
bool item_is_artifact(const char *base_name);
bool item_has_two_relic_slots(const char *suffix_name);
bool item_is_stackable_type(const TQVaultItem *a);
const char *dbr_get_string(const char *record_path, const char *var_name);
char *safe_strdup(const char *s);
void get_item_dims(AppWidgets *widgets, TQVaultItem *item, int *w, int *h);
GdkPixbuf *load_item_texture(AppWidgets *widgets, const char *base_name, uint32_t var1);
bool item_matches_search(AppWidgets *widgets, TQVaultItem *item);
char *dropdown_get_selected_text(GtkWidget *dd);
guint dropdown_select_by_name(GtkWidget *dd, const char *name);

/* ── Entry points in ui_draw.c ─────────────────────────────────────────── */

double compute_cell_size(AppWidgets *widgets);
TQVaultItem *sack_hit_test(AppWidgets *widgets, TQVaultSack *sack,
                            int cols, int rows, int w, int h, int x, int y);
bool equip_hit_test(double px, double py, double cell_size,
                    int *out_idx, double *out_x, double *out_y,
                    double *out_bw, double *out_bh);
void equip_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
                   int width, int height, gpointer user_data);
void vault_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
                   int width, int height, gpointer user_data);
void inv_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
                 int width, int height, gpointer user_data);
void bag_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
                 int width, int height, gpointer user_data);
void on_vault_resize(GtkDrawingArea *area, int width, int height, gpointer user_data);

/* ── Entry points in ui_tooltip.c ──────────────────────────────────────── */

void on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data);
void on_motion_leave(GtkEventControllerMotion *ctrl, gpointer user_data);

/* ── Entry points in ui_dnd.c ──────────────────────────────────────────── */

void cancel_held_item(AppWidgets *widgets);
void free_held_item(AppWidgets *widgets);
void vault_item_deep_copy(TQVaultItem *dst, const TQVaultItem *src);
void sack_add_item(TQVaultSack *sack, const TQVaultItem *item);
bool items_stackable(const TQVaultItem *a, const TQVaultItem *b);
void equip_to_vault_item(TQVaultItem *vi, const TQItem *eq);
void vault_item_to_equip(TQItem *eq, const TQVaultItem *vi);
bool *build_occupancy_grid(AppWidgets *widgets, TQVaultSack *sack,
                            int cols, int rows, TQVaultItem *exclude);
bool can_place_item(const bool *grid, int cols, int rows,
                    int x, int y, int w, int h);
int item_can_accept_relic_sack(const TQVaultItem *it,
                                const char *relic_base_name, TQTranslation *tr);
int item_can_accept_relic_equip(const TQItem *eq,
                                 const char *relic_base_name, TQTranslation *tr);
TQVaultItem *find_item_at_cell(AppWidgets *widgets, TQVaultSack *sack,
                                int cols, int rows, double cell,
                                double px, double py, int *out_idx);
void on_vault_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
void on_inv_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
void on_bag_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
void on_equip_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

/* ── Entry points in ui_stats.c ────────────────────────────────────────── */

void update_ui(AppWidgets *widgets, TQCharacter *chr);
void update_resist_damage_tables(AppWidgets *widgets, TQCharacter *chr);
void build_stat_tables(AppWidgets *widgets, GtkWidget *tables_inner);

/* ── Entry points in ui_settings.c ─────────────────────────────────────── */

void on_settings_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_about_btn_clicked(GtkButton *btn, gpointer user_data);
void on_view_build_clicked(GtkButton *btn, gpointer user_data);

/* ── Entry points in ui_affix_dialog.c ─────────────────────────────────── */

void show_affix_dialog(AppWidgets *widgets);

/* ── Entry points in ui_context_menu.c ─────────────────────────────────── */

void show_item_context_menu(AppWidgets *widgets, GtkWidget *drawing_area,
                            TQVaultItem *item, TQItem *equip_item,
                            ContainerType source, int sack_idx,
                            int equip_slot, double x, double y);
void register_context_actions(GtkApplication *app, AppWidgets *widgets);

/* ── Entry points in ui_manage.c ───────────────────────────────────────── */

void register_manage_actions(GtkWindow *window, AppWidgets *widgets);

/* ── Main entry points ─────────────────────────────────────────────────── */

void ui_first_run_setup(GtkApplication *app);
void ui_app_activate(GtkApplication *app, gpointer user_data);

#endif
