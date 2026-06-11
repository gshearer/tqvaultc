#ifndef UI_SKILLS_TREE_INTERNAL_H
#define UI_SKILLS_TREE_INTERNAL_H

// Shared internals for the visual skill-tree dialog, split across
// ui_skills_tree.c (dialog/model/allocation), ui_skills_data.c (skill +
// gear DBR resolution) and ui_skills_draw.c (cairo rendering).

#include <gtk/gtk.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui.h"
#include "arz.h"

// ── Constants ───────────────────────────────────────────────────────────

#define MAX_MASTERY_LEVEL 40
#define MAX_SKILLS_PER_MASTERY 64
#define MAX_TREE_ENTRIES 32
#define MAX_TIER 7

// Drawing geometry (pixels).  Kept tight to fit whole trees without scrolling.
#define SK_ICON     44.0   // icon draw size
#define SK_COL_W    56.0   // horizontal grid pitch
#define SK_ROW_H    64.0   // vertical grid pitch (icon + counter + gap)
#define SK_MARGIN_X 16.0
#define SK_MARGIN_Y 12.0
#define SK_COUNTER_DY 14.0 // counter baseline offset below icon bottom
#define SK_STUB 10.0       // connector stub length past an icon's right edge
#define SK_BAR_GUTTER 52.0 // left strip reserved for the mastery level bar + labels
#define SK_BAR_W 12.0      // mastery level bar width

// Native skill-panel pixel -> screen pixel scale for the DB-authored layout.
// The game's column pitch is 100px and its row pitch ~62px; at 1.0 the rows
// land at ~62px (matching SK_ROW_H's icon+counter footprint) and columns stay
// at the game's wide spacing.  Lowering this would collide rows vertically.
#define SK_DB_SCALE 1.0

#define HIT_MASTERY (-2)
#define HIT_NONE    (-1)
#define NUM_MASTERIES 11

// ── Mastery definitions ─────────────────────────────────────────────────

typedef struct {
  const char *name;
  const char *dbr_path;
  const char *dir_prefix;
  const char *tree_path;
} MasteryDef;

// Defined once in ui_skills_tree.c; shared with the other units via extern.
extern const MasteryDef mastery_defs[];
extern const int tier_mastery_req[];

// ── Data model ──────────────────────────────────────────────────────────

typedef struct {
  char path[256];
  char basename[128];
  int parent_idx;     // index into tree entries, -1 if root
} TreeEntry;

typedef struct {
  int chr_skill_idx;        // index into TQCharacter.skills[] / work_levels[], -1
  char skill_path[256];
  char basename[128];
  char display_name[128];
  int skill_tier;           // 1..7 (0 unknown)
  int max_level;
  int ultimate_level;
  int mastery_req;
  int cur_level;
  int parent_idx;           // index into pane nodes[], -1 root
  bool is_base;
  // Equipment-granted +skill bonuses applicable to this skill (raw, uncapped).
  // Only apply when at least one point is allocated; the effective total is
  // clamped to ultimate_level (a skill can exceed max_level via gear).
  int gear_all;             // augmentAllLevel (+N to all skills)
  int gear_mastery;         // augmentMastery* for this skill's mastery
  int gear_skill;           // augmentSkill* targeting this exact skill
  char up_tex[256];
  char down_tex[256];
  // authored in-game icon position (native skill-panel pixels), when present
  bool has_pos;
  int pos_x, pos_y;
  // derived layout (icon center, pixels)
  int row;
  double colf;
  double x, y;
} SkillNode;

typedef struct {
  int mastery_def_idx;
  int initial_mastery_def_idx;
  int mastery_chr_skill_idx;
  int mastery_level;
  char mastery_up_tex[256];
  char mastery_down_tex[256];

  // In-game (DB-authored) layout: when this mastery's skills carry button
  // positions, the pane is laid out from them instead of the derived grid.
  bool use_db_layout;
  bool mastery_has_pos;
  int mastery_pos_x, mastery_pos_y;

  SkillNode nodes[MAX_SKILLS_PER_MASTERY];
  int num_nodes;

  double content_w, content_h;
  double mastery_x, mastery_y;

  // widgets
  GtkWidget *frame;
  GtkWidget *dropdown;
  GtkWidget *summary_label;
  GtkWidget *reset_btn;
  GtkWidget *scroll;
  GtkWidget *canvas;
} MasteryPane;

// Equipment-derived +skill bonuses for the loaded character.  Computed once
// when the dialog opens (the equipped gear is fixed while it is open) and then
// consulted per skill node.  Bonuses come in three flavours -- all-skills,
// mastery-wide and skill-specific -- which stack on a given skill.
#define MAX_GEAR_SKILL_AUG 96

typedef struct {
  char path[256];   // normalized skill DBR path the augment targets
  int  bonus;       // summed +levels to that specific skill
} GearSkillAug;

typedef struct {
  int all_bonus;                          // +N to all skills (augmentAllLevel)
  int mastery_bonus[NUM_MASTERIES];       // +N to all skills in a mastery
  GearSkillAug skill[MAX_GEAR_SKILL_AUG]; // +N to specific skills
  int num_skill;
} GearBonuses;

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  GtkWidget *avail_label;
  GtkWidget *apply_btn;

  int total_skill_points;
  int orig_skill_points;

  uint32_t *work_levels;
  int num_chr_skills;

  GearBonuses gear;

  bool building;
  MasteryPane panes[2];
} SkillsState;

// -- Cross-file entry points ------------------------------------------------

// Skill + gear resolution (ui_skills_data.c).
void normalize_path(const char *src, char *dst, size_t dst_size);
int find_mastery_for_skill(const char *skill_path);
bool is_mastery_record(const char *path);
void resolve_skill_name(AppWidgets *widgets, const char *skill_path,
                        char *out, size_t out_size);
void get_skill_dbr_info(const char *skill_path, int *skill_tier, int *max_level,
                        int *ultimate_level);
void read_bitmap_field(TQArzRecordData *dbr, const char *field, char *out, size_t out_size);
void resolve_skill_bitmaps(const char *skill_path, char *up, size_t up_sz,
                           char *down, size_t down_sz);
void resolve_effective_skill_path(const char *skill_path, char *out, size_t out_sz);
int load_skill_tree(const char *tree_dbr_path, TreeEntry *entries, int max_entries);
void compute_gear_bonuses(TQCharacter *chr, GearBonuses *g);
int gear_skill_bonus(const GearBonuses *g, const char *norm_path);
int effective_level(int alloc, int raw_bonus, int ultimate);
bool skill_is_accessible(MasteryPane *mp, int idx);

// Rendering (ui_skills_draw.c).
char *skill_tooltip_markup(AppWidgets *w, const char *skill_path, int level,
                           int max_level, int ultimate_level, int gear_all,
                           int gear_mastery, int gear_skill, const char *mastery_name);
void skill_canvas_draw_cb(GtkDrawingArea *da, cairo_t *cr, int width, int height, gpointer user_data);

#endif
