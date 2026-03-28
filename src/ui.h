#ifndef UI_H
#define UI_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include "character.h"
#include "vault.h"
#include "stash.h"
#include "translation.h"

// ── Shared enums ──────────────────────────────────────────────────────────

typedef enum {
    CONTAINER_NONE, CONTAINER_VAULT, CONTAINER_INV, CONTAINER_BAG, CONTAINER_EQUIP,
    CONTAINER_TRANSFER, CONTAINER_PLAYER_STASH, CONTAINER_RELIC_VAULT
} ContainerType;

// Gear type flags for equipment classification and comparison
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

  GEAR_JEWELLERY   = (1 << 4) | (1 << 5),
  GEAR_ALL_ARMOR   = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3),
  GEAR_ALL_WEAPONS = (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10)
                   | (1 << 11) | (1 << 12) | (1 << 13),
};

// ── Held item (click-to-move) ─────────────────────────────────────────────

typedef struct {
    TQVaultItem item;        // deep copy of picked-up item
    ContainerType source;
    int source_sack_idx;     // vault sack or char bag index
    int source_equip_slot;
    GdkPixbuf *texture;      // cached for cursor drawing
    int item_w, item_h;      // cell dimensions
    bool is_copy;            // true = created by Copy/Duplicate, discard on cancel
} HeldItem;

// ── Equipment slot layout ─────────────────────────────────────────────────

// Pixels reserved below each slot box for the slot name label
#define EQUIP_LABEL_H   14.0
// Horizontal gap between the three columns (px)
#define EQUIP_COL_GAP    4.0
// Vertical gap between slots within the same column (px)
#define EQUIP_SLOT_GAP   4.0

typedef struct {
    int        slot_idx;
    const char *label;
    int        box_w;   // slot box width  in cells
    int        box_h;   // slot box height in cells
} EquipSlot;

extern const EquipSlot COL_LEFT[3];
extern const EquipSlot COL_CENTER[4];
extern const EquipSlot RING_SLOTS[2];
extern const EquipSlot COL_RIGHT[3];

// ── Global grid sizing ────────────────────────────────────────────────────

// Vertical overhead in the vault column (combo + bag buttons + spacing).
#define VAULT_V_OVERHEAD 90
// Horizontal gaps: vault/char margin + inv_bag grid column gap.
#define LAYOUT_H_OVERHEAD 20
// Vault grid dimensions
#define VAULT_COLS 18
#define VAULT_ROWS 20

// Bag button state indices (used by ui.c and ui_io.c)
enum { BAG_DOWN = 0, BAG_UP = 1, BAG_OVER = 2 };

// ── Main application widget state ─────────────────────────────────────────

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
    GtkWidget *inv_drawing_area;      // 12x5 character main inventory
    GtkWidget *bag_drawing_area;      // 8x5 extra bag (current_char_bag)
    int current_char_bag;             // 0-2 which extra bag is shown
    TQCharacter *current_character;
    TQVault *current_vault;
    TQTranslation *translations;
    GHashTable *texture_cache;

    // Tooltip caches
    TQVaultItem *last_tooltip_item;
    char last_tooltip_markup[16384];

    TQVaultItem *last_inv_tooltip_item;
    char last_inv_tooltip_markup[16384];

    TQVaultItem *last_bag_tooltip_item;
    char last_bag_tooltip_markup[16384];

    int last_equip_tooltip_slot;      // -1 = none
    char last_equip_tooltip_markup[16384];

    // Resistance table
    GtkWidget *resist_grid;
    GtkWidget *resist_cells[14][9]; // rows: 12 slots + 2 totals; cols: 9 resistance types

    // Secondary Resistances table -- 8 columns
    GtkWidget *secresist_grid;
    GtkWidget *secresist_cells[14][8];

    // Direct Damage table -- 8 columns
    GtkWidget *fdmg_grid;
    GtkWidget *fdmg_cells[14][8];

    // Bonus Damage table -- 11 columns (percentage only)
    GtkWidget *bdmg_grid;
    GtkWidget *bdmg_cells[14][11];

    // DOT Damage table -- 8 columns
    GtkWidget *dotdmg_grid;
    GtkWidget *dotdmg_cells[14][8];

    // Pet Bonuses table -- 11 columns (percentage only)
    GtkWidget *petdmg_grid;
    GtkWidget *petdmg_cells[14][11];

    // Bonus Speed table -- 6 columns (percentage only)
    GtkWidget *bspd_grid;
    GtkWidget *bspd_cells[14][6];

    // Health / Energy Bonuses table -- 7 columns
    GtkWidget *hea_grid;
    GtkWidget *hea_cells[14][7];

    // Ability Bonuses table -- 4 columns
    GtkWidget *abil_grid;
    GtkWidget *abil_cells[14][4];

    GtkWidget *main_hbox;   // top-level horizontal split, for cell size computation

    // Bag button textures: pre-rendered numbered pixbufs for each state
    GdkPixbuf *vault_bag_pix[3][12];  // [state][bag_idx] -- 0=down, 1=up, 2=over
    GtkWidget *vault_bag_btns[12];
    GdkPixbuf *char_bag_pix[3][3];    // [state][bag_idx]
    GtkWidget *char_bag_btns[3];

    // Click-to-move state
    HeldItem *held_item;
    double cursor_x, cursor_y;
    GtkWidget *cursor_widget;   // which drawing area the cursor is over
    GtkWidget *held_overlay;    // transparent overlay for held item between panes
    double win_cursor_x, win_cursor_y;  // cursor pos in overlay coordinates
    bool vault_dirty;
    bool char_dirty;

    // Right-click context menu
    GMenu *context_menu_model;       // GMenu rebuilt before each show
    GtkWidget *context_menu;         // GtkPopoverMenu for right-click
    TQVaultItem *context_item;       // item under the right-click (pointer into sack)
    TQItem *context_equip_item;      // equipment item under right-click
    ContainerType context_source;    // which container the item is in
    int context_sack_idx;            // sack index
    int context_equip_slot;          // equipment slot (-1 if not equipment)
    GtkWidget *context_parent;       // drawing area the menu is attached to

    // Bag context menu (right-click on bag buttons)
    GMenu *bag_menu_model;
    GtkWidget *bag_menu;
    GtkWidget *bag_menu_parent;
    ContainerType bag_menu_source;   // CONTAINER_VAULT, CONTAINER_INV, or CONTAINER_BAG
    int bag_menu_sack_idx;           // 0-11 for vault, 0 for inv, 0-2 for bag

    // Instant tooltip (replaces GTK4 built-in 500ms delayed tooltips)
    GtkWidget *tooltip_popover;
    GtkWidget *tooltip_label;
    GtkWidget *tooltip_parent;       // current parent drawing area

    // Item comparison state (Ctrl+click to mark, hover to compare)
    TQVaultItem    compare_item;           // deep copy of marked item
    bool           compare_active;         // true when compare is set
    ContainerType  compare_source;         // container type
    int            compare_sack_idx;       // sack index
    int            compare_equip_slot;     // equip slot (-1 if N/A)
    char           compare_markup[16384];  // pre-rendered tooltip markup

    // Compare tooltip (inside main tooltip popover, shown/hidden as needed)
    GtkWidget     *compare_scroll;         // scrolled window to show/hide
    GtkWidget     *compare_label;
    GtkWidget     *compare_separator;      // vertical separator between tooltips

    // Save/Refresh/Checklist/Stats/Skills buttons
    GtkWidget *save_char_btn;
    GtkWidget *checklist_btn;
    GtkWidget *stats_btn;
    GtkWidget *skills_btn;

    // Search
    GtkWidget *search_entry;
    char search_text[256];          // lowercased search term, empty = no search
    bool vault_sack_match[12];      // per-sack: any items match?
    bool char_sack_match[4];        // per-char-inv-sack: any items match?

    // Caravan Driver stash tabs
    GtkWidget *stash_notebook;
    GtkWidget *stash_transfer_da;
    GtkWidget *stash_player_da;
    GtkWidget *stash_relic_da;
    TQStash *transfer_stash;
    TQStash *player_stash;
    TQStash *relic_vault;

    // Stash tooltip caches
    TQVaultItem *last_transfer_tooltip_item;
    char last_transfer_tooltip_markup[16384];
    TQVaultItem *last_player_tooltip_item;
    char last_player_tooltip_markup[16384];
    TQVaultItem *last_relic_tooltip_item;
    char last_relic_tooltip_markup[16384];
} AppWidgets;

// ── Functions shared across ui modules (defined in ui.c) ──────────────────

// Invalidate all cached tooltip data, forcing regeneration on next hover.
// widgets: the application widget state.
void
invalidate_tooltips(AppWidgets *widgets);

// Queue a redraw of all drawing areas (vault, inv, bag, equip, stashes).
// widgets: the application widget state.
void
queue_redraw_all(AppWidgets *widgets);

// Queue a redraw of the equipment drawing area only.
// widgets: the application widget state.
void
queue_redraw_equip(AppWidgets *widgets);

// Save the current vault to disk if it has been modified.
// widgets: the application widget state.
void
save_vault_if_dirty(AppWidgets *widgets);

// Save the current character to disk if it has been modified.
// widgets: the application widget state.
void
save_character_if_dirty(AppWidgets *widgets);

// Save all stash files to disk if any have been modified.
// widgets: the application widget state.
void
save_stashes_if_dirty(AppWidgets *widgets);

// Update the save button sensitivity based on dirty state.
// widgets: the application widget state.
void
update_save_button_sensitivity(AppWidgets *widgets);

// Repopulate the vault dropdown combo box.
// widgets: the application widget state.
// select_name: vault name to select after repopulation, or NULL.
void
repopulate_vault_combo(AppWidgets *widgets, const char *select_name);

// Repopulate the character dropdown combo box.
// widgets: the application widget state.
// select_name: character name to select after repopulation, or NULL.
void
repopulate_character_combo(AppWidgets *widgets, const char *select_name);

// Pick up a vault item and attach it to the cursor for click-to-move.
// widgets: the application widget state.
// src: the vault item to pick up.
// is_copy: true if this is a copy operation (original stays).
void
copy_item_to_cursor(AppWidgets *widgets, TQVaultItem *src, bool is_copy);

// Pick up an equipment item and attach it to the cursor for click-to-move.
// widgets: the application widget state.
// eq: the equipment item to pick up.
// is_copy: true if this is a copy operation (original stays).
void
copy_equip_to_cursor(AppWidgets *widgets, TQItem *eq, bool is_copy);

// Map an item's DBR "Class" field to a single GEAR_* flag.
// base_name: DBR path of the item.
// Returns: GEAR_* flag for the item's class, or 0 if unknown.
uint32_t
item_gear_type(const char *base_name);

// Clear the compare item state and hide the compare popover.
// widgets: the application widget state.
void
clear_compare_item(AppWidgets *widgets);

// Check if an item base_name is a relic or charm.
// base_name: DBR path of the item.
// Returns: true if item is a relic or charm.
bool
item_is_relic_or_charm(const char *base_name);

// Check if an item base_name is an artifact.
// base_name: DBR path of the item.
// Returns: true if item is an artifact.
bool
item_is_artifact(const char *base_name);

// Check if a suffix indicates the item has two relic slots.
// suffix_name: the item's suffix DBR path.
// Returns: true if the item has two relic slots.
bool
item_has_two_relic_slots(const char *suffix_name);

// Check if a vault item is a stackable type (potion, scroll, relic shard).
// a: the vault item to check.
// Returns: true if the item can stack.
bool
item_is_stackable_type(const TQVaultItem *a);

// Look up a string variable from a DBR record.
// record_path: DBR path to the record.
// var_name: variable name to look up.
// Returns: static string value, or NULL if not found.
const char *
dbr_get_string(const char *record_path, const char *var_name);

// Duplicate a string safely (returns NULL for NULL input).
// s: the string to duplicate.
// Returns: malloc'd copy of s, or NULL.
char *
safe_strdup(const char *s);

// Get the grid dimensions (width x height in cells) of a vault item.
// widgets: the application widget state (for database lookups).
// item: the vault item to measure.
// w: receives the width in cells.
// h: receives the height in cells.
void
get_item_dims(AppWidgets *widgets, TQVaultItem *item, int *w, int *h);

// Load or retrieve from cache the texture for an item.
// widgets: the application widget state (for texture cache).
// base_name: DBR path of the item.
// var1: item variant (used for relic shard count).
// Returns: GdkPixbuf texture, or NULL if not found.
GdkPixbuf *
load_item_texture(AppWidgets *widgets, const char *base_name, uint32_t var1);

// Check if a vault item matches the current search text.
// widgets: the application widget state (for search_text).
// item: the vault item to check.
// Returns: true if the item matches (or no search active).
bool
item_matches_search(AppWidgets *widgets, TQVaultItem *item);

// Run a search across all vault sacks and character inventory sacks,
// updating bag button CSS classes and redrawing.
// widgets: the application widget state.
void
run_search(AppWidgets *widgets);

// Get the currently selected text from a GtkDropDown.
// dd: the dropdown widget.
// Returns: malloc'd string with the selected text. Caller must free.
char *
dropdown_get_selected_text(GtkWidget *dd);

// Select an entry in a GtkDropDown by display name.
// dd: the dropdown widget.
// name: the name to select.
// Returns: the index of the selected entry, or GTK_INVALID_LIST_POSITION.
guint
dropdown_select_by_name(GtkWidget *dd, const char *name);

// ── Entry points in ui_draw.c ─────────────────────────────────────────────

// Compute the cell size in pixels based on available window space.
// widgets: the application widget state.
// Returns: cell size in pixels.
double
compute_cell_size(AppWidgets *widgets);

// Hit-test a point against items in a sack grid.
// widgets: the application widget state.
// sack: the sack to test against.
// cols: grid column count.
// rows: grid row count.
// w: drawing area width in pixels.
// h: drawing area height in pixels.
// x: click x coordinate.
// y: click y coordinate.
// Returns: the item under the point, or NULL.
TQVaultItem *
sack_hit_test(AppWidgets *widgets, TQVaultSack *sack,
              int cols, int rows, int w, int h, int x, int y);

// Hit-test a point against equipment slots.
// px: x coordinate in pixels.
// py: y coordinate in pixels.
// cell_size: current cell size.
// out_idx: receives the slot index if hit.
// out_x: receives the slot origin x.
// out_y: receives the slot origin y.
// out_bw: receives the slot box width.
// out_bh: receives the slot box height.
// Returns: true if an equipment slot was hit.
bool
equip_hit_test(double px, double py, double cell_size,
               int *out_idx, double *out_x, double *out_y,
               double *out_bw, double *out_bh);

// Draw callback for the equipment panel.
void
equip_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
              int width, int height, gpointer user_data);

// Draw callback for the vault grid.
void
vault_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
              int width, int height, gpointer user_data);

// Draw callback for the character main inventory grid.
void
inv_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
            int width, int height, gpointer user_data);

// Draw callback for the extra bag grid.
void
bag_draw_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
            int width, int height, gpointer user_data);

// Resize callback for the vault drawing area.
void
on_vault_resize(GtkDrawingArea *area, int width, int height, gpointer user_data);

// Draw items in a sack onto a cairo context.
// cr: cairo drawing context.
// widgets: the application widget state.
// sack: the sack whose items to draw.
// cols: grid column count.
// rows: grid row count.
// width: drawing area width.
// height: drawing area height.
// forced_cell: cell size override (0 = auto).
// this_widget: the drawing area widget being drawn.
void
draw_sack_items(cairo_t *cr, AppWidgets *widgets,
                TQVaultSack *sack, int cols, int rows,
                int width, int height, double forced_cell,
                GtkWidget *this_widget);

// Draw callback for the transfer stash.
void
stash_transfer_draw_cb(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer ud);

// Draw callback for the player stash.
void
stash_player_draw_cb(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer ud);

// Draw callback for the relic vault stash.
void
stash_relic_draw_cb(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer ud);

// Draw callback for the held item overlay.
void
held_overlay_draw_cb(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer ud);

// ── Entry points in ui_tooltip.c ──────────────────────────────────────────

// Motion event handler for tooltip display.
// ctrl: the motion event controller.
// x: cursor x position.
// y: cursor y position.
// user_data: AppWidgets pointer.
void
on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data);

// Motion-leave event handler to hide tooltips.
// ctrl: the motion event controller.
// user_data: AppWidgets pointer.
void
on_motion_leave(GtkEventControllerMotion *ctrl, gpointer user_data);

// ── Entry points in ui_dnd.c ──────────────────────────────────────────────

// Cancel the current held item, returning it to its source.
// widgets: the application widget state.
void
cancel_held_item(AppWidgets *widgets);

// Free the held item without returning it.
// widgets: the application widget state.
void
free_held_item(AppWidgets *widgets);

// Deep-copy a vault item (all strings are strdup'd).
// dst: destination item.
// src: source item.
void
vault_item_deep_copy(TQVaultItem *dst, const TQVaultItem *src);

// Add an item to a sack (appends to the item array).
// sack: the sack to add to.
// item: the item to add (deep copied).
void
sack_add_item(TQVaultSack *sack, const TQVaultItem *item);

// Check if two vault items can stack together.
// a: first item.
// b: second item.
// Returns: true if the items are stackable with each other.
bool
items_stackable(const TQVaultItem *a, const TQVaultItem *b);

// Convert an equipment item to a vault item.
// vi: destination vault item.
// eq: source equipment item.
void
equip_to_vault_item(TQVaultItem *vi, const TQItem *eq);

// Convert a vault item to an equipment item.
// eq: destination equipment item.
// vi: source vault item.
void
vault_item_to_equip(TQItem *eq, const TQVaultItem *vi);

// Build a boolean occupancy grid for a sack.
// widgets: the application widget state.
// sack: the sack to scan.
// cols: grid column count.
// rows: grid row count.
// exclude: item to exclude from occupancy (NULL for none).
// Returns: malloc'd bool array of size cols*rows. Caller must free.
bool *
build_occupancy_grid(AppWidgets *widgets, TQVaultSack *sack,
                     int cols, int rows, TQVaultItem *exclude);

// Check if an item can be placed at a position in the occupancy grid.
// grid: the occupancy grid.
// cols: grid column count.
// rows: grid row count.
// x: target column.
// y: target row.
// w: item width in cells.
// h: item height in cells.
// Returns: true if placement is valid.
bool
can_place_item(const bool *grid, int cols, int rows,
               int x, int y, int w, int h);

// Check if a sack item can accept a relic in one of its slots.
// it: the vault item to check.
// relic_base_name: DBR path of the relic.
// tr: translation table.
// Returns: slot number (1 or 2) that can accept the relic, or 0 if none.
int
item_can_accept_relic_sack(const TQVaultItem *it,
                           const char *relic_base_name, TQTranslation *tr);

// Check if an equipment item can accept a relic in one of its slots.
// eq: the equipment item to check.
// relic_base_name: DBR path of the relic.
// tr: translation table.
// Returns: slot number (1 or 2) that can accept the relic, or 0 if none.
int
item_can_accept_relic_equip(const TQItem *eq,
                            const char *relic_base_name, TQTranslation *tr);

// Find the item at a given cell position in a sack.
// widgets: the application widget state.
// sack: the sack to search.
// cols: grid column count.
// rows: grid row count.
// cell: cell size in pixels.
// px: x coordinate in pixels.
// py: y coordinate in pixels.
// out_idx: receives the item index in the sack.
// Returns: the item at that position, or NULL.
TQVaultItem *
find_item_at_cell(AppWidgets *widgets, TQVaultSack *sack,
                  int cols, int rows, double cell,
                  double px, double py, int *out_idx);

// Click handler for the vault drawing area.
void
on_vault_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

// Click handler for the main inventory drawing area.
void
on_inv_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

// Click handler for the extra bag drawing area.
void
on_bag_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

// Click handler for the equipment drawing area.
void
on_equip_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

// Shared click handler for sack-based containers.
// widgets: the application widget state.
// drawing_area: the drawing area that was clicked.
// sack: the sack being clicked in.
// ctype: which container type.
// sack_idx: index of the sack.
// cols: grid column count.
// rows: grid row count.
// cell: cell size in pixels.
// px: click x coordinate.
// py: click y coordinate.
// button: mouse button number (1=left, 3=right).
void
handle_sack_click(AppWidgets *widgets, GtkWidget *drawing_area,
                  TQVaultSack *sack,
                  ContainerType ctype, int sack_idx,
                  int cols, int rows, double cell,
                  double px, double py, int button);

// Click handler for the transfer stash drawing area.
void
on_stash_transfer_click(GtkGestureClick *g, int n, double x, double y, gpointer ud);

// Click handler for the player stash drawing area.
void
on_stash_player_click(GtkGestureClick *g, int n, double x, double y, gpointer ud);

// Click handler for the relic vault stash drawing area.
void
on_stash_relic_click(GtkGestureClick *g, int n, double x, double y, gpointer ud);

// ── Entry points in ui_stats.c ────────────────────────────────────────────

// Update UI labels and fields from character data.
// widgets: the application widget state.
// chr: the character to display.
void
update_ui(AppWidgets *widgets, TQCharacter *chr);

// Update all resistance and damage stat tables from character equipment.
// widgets: the application widget state.
// chr: the character whose equipment to scan.
void
update_resist_damage_tables(AppWidgets *widgets, TQCharacter *chr);

// Build the stat table grid widgets and add them to tables_inner.
// widgets: the application widget state.
// tables_inner: the container widget to add tables to.
void
build_stat_tables(AppWidgets *widgets, GtkWidget *tables_inner);

// ── Entry points in ui_io.c ────────────────────────────────────────────

// Show a modal Save/Discard/Cancel dialog for unsaved character changes.
// widgets: the application widget state.
// Returns: 0=Save, 1=Discard, 2=Cancel.
int
confirm_unsaved_character(AppWidgets *widgets);

// Set the image on a bag button from a GdkPixbuf.
// btn: the GtkButton.
// pixbuf: the pixbuf to display.
void
set_bag_btn_image(GtkWidget *btn, GdkPixbuf *pixbuf);

// Callback: Save Character button clicked.
void on_save_char_clicked(GtkButton *btn, gpointer user_data);

// Callback: Database button clicked.
void on_database_btn_clicked(GtkButton *btn, gpointer user_data);

// Callback: Checklist button clicked.
void on_checklist_btn_clicked(GtkButton *btn, gpointer user_data);

// Callback: Attributes (stats) button clicked.
void on_stats_btn_clicked(GtkButton *btn, gpointer user_data);

// Callback: Skills button clicked.
void on_skills_btn_clicked(GtkButton *btn, gpointer user_data);

// Callback: Refresh Character button clicked.
void on_refresh_char_clicked(GtkButton *btn, gpointer user_data);

// Callback: character dropdown selection changed.
void on_character_changed(GObject *obj, GParamSpec *pspec, gpointer user_data);

// Callback: vault dropdown selection changed.
void on_vault_changed(GObject *obj, GParamSpec *pspec, gpointer user_data);

// Callback: mouse enters a vault bag button.
void on_vault_bag_hover_enter(GtkEventControllerMotion *ctrl,
                              double x, double y, gpointer user_data);

// Callback: mouse leaves a vault bag button.
void on_vault_bag_hover_leave(GtkEventControllerMotion *ctrl,
                              gpointer user_data);

// Callback: mouse enters a character bag button.
void on_char_bag_hover_enter(GtkEventControllerMotion *ctrl,
                             double x, double y, gpointer user_data);

// Callback: mouse leaves a character bag button.
void on_char_bag_hover_leave(GtkEventControllerMotion *ctrl,
                             gpointer user_data);

// Callback: vault bag button clicked.
void on_bag_clicked(GtkButton *btn, gpointer user_data);

// Callback: character bag button clicked.
void on_char_bag_clicked(GtkButton *btn, gpointer user_data);

// Callback: right-click on a vault bag button.
void on_vault_bag_right_click(GtkGestureClick *gesture, int n_press,
                              double x, double y, gpointer user_data);

// Callback: right-click on a character bag button.
void on_char_bag_right_click(GtkGestureClick *gesture, int n_press,
                             double x, double y, gpointer user_data);

// ── Entry points in ui_settings.c ─────────────────────────────────────────

// Action handler for the settings menu.
void
on_settings_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);

// Click handler for the about button.
void
on_about_btn_clicked(GtkButton *btn, gpointer user_data);

// Click handler for the view-build button.
void
on_view_build_clicked(GtkButton *btn, gpointer user_data);

// ── Entry points in ui_affix_dialog.c ─────────────────────────────────────

struct TQItemAffixes_tag;  // forward declaration (defined in affix_table.h)

// Show the affix modification dialog. If override_affixes is non-NULL, use
// those instead of looking up via affix_table_get(). If override_title is
// non-NULL, use it as the dialog title instead of "Modify Affixes".
// widgets: the application widget state.
// override_affixes: custom affix list, or NULL for default lookup.
// override_title: custom dialog title, or NULL for default.
void
show_affix_dialog(AppWidgets *widgets, struct TQItemAffixes_tag *override_affixes,
                  const char *override_title);

// ── Entry points in ui_stats_dialog.c ──────────────────────────────────────

// Show the character stats editing dialog.
// widgets: the application widget state.
void
show_stats_dialog(AppWidgets *widgets);

// ── Entry points in ui_skills_dialog.c ─────────────────────────────────────

// Show the skill point editing dialog.
// widgets: the application widget state.
void
show_skills_dialog(AppWidgets *widgets);

// ── Entry points in ui_database_dialog.c ──────────────────────────────────

// Show the database browser dialog.
// widgets: the application widget state.
void
show_database_dialog(AppWidgets *widgets);

// ── Entry points in ui_checklist_dialog.c ─────────────────────────────────

// Show the quest checklist dialog.
// widgets: the application widget state.
void
show_checklist_dialog(AppWidgets *widgets);

// ── Entry points in ui_context_menu.c ─────────────────────────────────────

// Show the right-click context menu for an item.
// widgets: the application widget state.
// drawing_area: the drawing area the menu is attached to.
// item: the vault item under the cursor (or NULL for equipment).
// equip_item: the equipment item under the cursor (or NULL for vault).
// source: which container the item is in.
// sack_idx: sack index (for vault/bag containers).
// equip_slot: equipment slot index (-1 if not equipment).
// x: menu x position.
// y: menu y position.
void
show_item_context_menu(AppWidgets *widgets, GtkWidget *drawing_area,
                       TQVaultItem *item, TQItem *equip_item,
                       ContainerType source, int sack_idx,
                       int equip_slot, double x, double y);

// Register context menu GActions with the application.
// app: the GTK application.
// widgets: the application widget state.
void
register_context_actions(GtkApplication *app, AppWidgets *widgets);

// ── Entry points in ui_bag_menu.c ──────────────────────────────────────────

// Register bag context menu GActions with the application.
// app: the GTK application.
// widgets: the application widget state.
void
register_bag_menu_actions(GtkApplication *app, AppWidgets *widgets);

// Show the right-click context menu for a bag button.
// widgets: the application widget state.
// parent_widget: the bag button widget.
// source: container type (CONTAINER_VAULT, CONTAINER_INV, or CONTAINER_BAG).
// sack_idx: the sack index.
void
show_bag_context_menu(AppWidgets *widgets, GtkWidget *parent_widget,
                      ContainerType source, int sack_idx);

// ── Entry points in ui_manage.c ───────────────────────────────────────────

// Register vault/character management GActions.
// window: the main application window.
// widgets: the application widget state.
void
register_manage_actions(GtkWindow *window, AppWidgets *widgets);

// ── Main entry points ─────────────────────────────────────────────────────

// Run first-time setup (settings dialog if no game path configured).
// app: the GTK application.
void
ui_first_run_setup(GtkApplication *app);

// Application activate callback — builds and shows the main window.
// app: the GTK application.
// user_data: unused.
void
ui_app_activate(GtkApplication *app, gpointer user_data);

#endif
