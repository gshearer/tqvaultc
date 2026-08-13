// ui_skills_tree.c -- visual skill-tree editor: dialog, model and allocation.
//
// Two side-by-side mastery panes.  Each pane draws its mastery's skills as a
// 2D tree of icons (cairo on a GtkDrawingArea) with connecting dependency
// lines and an "X / Y" current/max counter under each icon, mirroring the
// in-game / calculator skill window.  Left-click adds a point, right-click
// removes one, Shift maxes/zeroes.  Hovering shows an in-game-style tooltip.
// Changes are written to the save on "Apply".
//
// The editor is split across three translation units that share
// ui_skills_tree_internal.h (the SkillsState/MasteryPane/SkillNode model):
//   * ui_skills_tree.c -- this file: the dialog + widgets, the per-mastery
//                         model build, the auto/DB layout, and the
//                         point-allocation backend (tier gating, parent/child
//                         dependency, cascading zeroing, point-pool accounting).
//   * ui_skills_data.c -- skill + equipment-bonus DBR resolution.
//   * ui_skills_draw.c -- cairo rendering of the canvas and tooltips.
// The layout is auto-derived from the skill DBR data (tier -> row, base-skill
// branches -> columns) and can be overridden per mastery via ui_skills_layout.

#include "ui_skills_tree_internal.h"

#include "asset_lookup.h"
#include "ui_skills_layout.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>

// Shared mastery + tier tables (declared extern in the internal header).
// Tier -> minimum mastery level thresholds.  These are the game's canonical
// tier levels (records/game/gameengine.dbr: skillMasteryTierLevel).
const int tier_mastery_req[] = {
  [0] = 0, [1] = 1, [2] = 4, [3] = 10, [4] = 16, [5] = 24, [6] = 32, [7] = 40,
};

const MasteryDef mastery_defs[] = {
  { "Defense",  "records\\skills\\defensive\\defensivemastery.dbr",
                "records\\skills\\defensive\\",
                "records/skills/defensive/defensiveskilltree.dbr" },
  { "Earth",    "records\\skills\\earth\\earthmastery.dbr",
                "records\\skills\\earth\\",
                "records/skills/earth/earthskilltree.dbr" },
  { "Hunting",  "records\\skills\\hunting\\huntingmastery.dbr",
                "records\\skills\\hunting\\",
                "records/skills/hunting/huntingskilltree.dbr" },
  { "Nature",   "records\\skills\\nature\\naturemastery.dbr",
                "records\\skills\\nature\\",
                "records/skills/nature/natureskilltree.dbr" },
  { "Spirit",   "records\\skills\\spirit\\spiritmastery.dbr",
                "records\\skills\\spirit\\",
                "records/skills/spirit/spiritskilltree.dbr" },
  { "Storm",    "records\\skills\\storm\\stormmastery.dbr",
                "records\\skills\\storm\\",
                "records/skills/storm/stormskilltree.dbr" },
  { "Warfare",  "records\\skills\\warfare\\warfaremastery.dbr",
                "records\\skills\\warfare\\",
                "records/skills/warfare/warfareskilltree.dbr" },
  { "Dream",    "records\\xpack\\skills\\dream\\dreammastery.dbr",
                "records\\xpack\\skills\\dream\\",
                "records/xpack/skills/dream/dreamskilltree.dbr" },
  { "Rune",     "records\\xpack2\\skills\\runemaster\\runemaster_mastery.dbr",
                "records\\xpack2\\skills\\runemaster\\",
                "records/xpack2/skills/runemaster/runemaster_skilltree.dbr" },
  { "Rogue",    "records\\skills\\stealth\\stealthmastery.dbr",
                "records\\skills\\stealth\\",
                "records/skills/stealth/stealthskilltree.dbr" },
  { "Neidan",   "records\\xpack4\\skills\\neidan\\neidanmastery.dbr",
                "records\\xpack4\\skills\\neidan\\",
                "records/xpack4/skills/neidan/neidanskilltree.dbr" },
};

// ── Forward declarations ────────────────────────────────────────────────

static void build_mastery_model(SkillsState *st, int pane_idx);
static void refresh_chrome(SkillsState *st);
static void apply_canvas_size(MasteryPane *mp);

static int
compute_avail(SkillsState *st)
{
  int spent = 0;

  for(int i = 0; i < st->num_chr_skills; i++)
    spent += (int)st->work_levels[i];

  // Brand-new skills (not yet in the save: chr_skill_idx < 0) keep their pending
  // level only on the node, not in work_levels, so add those points here. A
  // skill belongs to a single mastery, so it appears in at most one pane.
  for(int p = 0; p < 2; p++)
  {
    MasteryPane *mp = &st->panes[p];

    for(int i = 0; i < mp->num_nodes; i++)
      if(mp->nodes[i].chr_skill_idx < 0)
        spent += mp->nodes[i].cur_level;
  }

  return(st->total_skill_points - spent);
}

static bool
has_changes(SkillsState *st)
{
  TQCharacter *chr = st->widgets->current_character;

  for(int p = 0; p < 2; p++)
  {
    if(st->panes[p].mastery_def_idx != st->panes[p].initial_mastery_def_idx)
      return(true);
  }

  for(int i = 0; i < st->num_chr_skills; i++)
  {
    if(st->work_levels[i] != chr->skills[i].skill_level)
      return(true);
  }

  // Any points allocated to a brand-new skill (not yet in the save) is a change.
  for(int p = 0; p < 2; p++)
  {
    MasteryPane *mp = &st->panes[p];

    for(int i = 0; i < mp->num_nodes; i++)
      if(mp->nodes[i].chr_skill_idx < 0 && mp->nodes[i].cur_level > 0)
        return(true);
  }

  return(false);
}

static void
zero_skill_and_dependents(SkillsState *st, MasteryPane *mp, int idx)
{
  SkillNode *n = &mp->nodes[idx];

  if(n->cur_level > 0)
  {
    n->cur_level = 0;

    if(n->chr_skill_idx >= 0)
      st->work_levels[n->chr_skill_idx] = 0;
  }

  for(int i = 0; i < mp->num_nodes; i++)
  {
    if(mp->nodes[i].parent_idx == idx && mp->nodes[i].cur_level > 0)
      zero_skill_and_dependents(st, mp, i);
  }
}

// ── Mutators (no widgets) ────────────────────────────────────────────────

static bool
skill_inc(SkillsState *st, MasteryPane *mp, int idx)
{
  SkillNode *n = &mp->nodes[idx];

  if(!skill_is_accessible(mp, idx))
    return(false);
  if(n->cur_level >= n->max_level)
    return(false);
  // Refuse points that can't raise the skill: once equipment +skill bonuses have
  // already pushed the effective level to the ultimate cap, further invested
  // points do nothing (mirrors the tooltip's "no Next Level" rule). Without this
  // a skill with strong gear lets you spend points past the cap for no effect.
  if(effective_level(n->cur_level, n->gear_all + n->gear_mastery + n->gear_skill,
                     n->ultimate_level) >= n->ultimate_level)
    return(false);
  if(compute_avail(st) <= 0)
    return(false);

  n->cur_level++;

  // Existing skills mirror their level into work_levels (saved in-place); new
  // skills (chr_skill_idx < 0) keep it only on the node and are spliced into the
  // save at apply time. Either way compute_avail() counts the point.
  if(n->chr_skill_idx >= 0)
    st->work_levels[n->chr_skill_idx] = (uint32_t)n->cur_level;
  return(true);
}

static void
skill_dec(SkillsState *st, MasteryPane *mp, int idx)
{
  SkillNode *n = &mp->nodes[idx];

  if(n->cur_level <= 0)
    return;

  n->cur_level--;
  if(n->chr_skill_idx >= 0)
    st->work_levels[n->chr_skill_idx] = (uint32_t)n->cur_level;

  if(n->cur_level == 0)
  {
    for(int i = 0; i < mp->num_nodes; i++)
    {
      if(mp->nodes[i].parent_idx == idx && mp->nodes[i].cur_level > 0)
        zero_skill_and_dependents(st, mp, i);
    }
  }
}

static bool
mastery_inc(SkillsState *st, MasteryPane *mp)
{
  if(mp->mastery_chr_skill_idx < 0)
    return(false);
  if(mp->mastery_level >= MAX_MASTERY_LEVEL)
    return(false);
  if(compute_avail(st) <= 0)
    return(false);

  mp->mastery_level++;
  st->work_levels[mp->mastery_chr_skill_idx] = (uint32_t)mp->mastery_level;
  return(true);
}

static void
mastery_dec(SkillsState *st, MasteryPane *mp)
{
  if(mp->mastery_chr_skill_idx < 0 || mp->mastery_level <= 0)
    return;

  mp->mastery_level--;
  st->work_levels[mp->mastery_chr_skill_idx] = (uint32_t)mp->mastery_level;

  for(int i = 0; i < mp->num_nodes; i++)
  {
    SkillNode *n = &mp->nodes[i];
    int tier = n->skill_tier;

    if(tier >= 1 && tier <= MAX_TIER &&
       mp->mastery_level < tier_mastery_req[tier] && n->cur_level > 0)
      zero_skill_and_dependents(st, mp, i);
  }
}

// Full refund: zero every skill (and the mastery) for this pane's mastery.
static void
mastery_reset(SkillsState *st, MasteryPane *mp)
{
  TQCharacter *chr = st->widgets->current_character;
  int def = mp->mastery_def_idx;

  if(def < 0 || !chr)
    return;

  for(int i = 0; i < chr->num_skills; i++)
  {
    const char *path = chr->skills[i].skill_name;

    if(path && path[0] && find_mastery_for_skill(path) == def)
      st->work_levels[i] = 0;
  }

  mp->mastery_level = 0;

  for(int i = 0; i < mp->num_nodes; i++)
    mp->nodes[i].cur_level = 0;
}

// ── Visual model build ───────────────────────────────────────────────────

// Resolve a tree-entry parent index (which may point at a filtered/hidden
// entry) to the nearest visible node index, or -1.
static int
map_visible_parent(const TreeEntry *tree, const int *tree_to_node, int t)
{
  int pt = tree[t].parent_idx;

  while(pt >= 0)
  {
    if(tree_to_node[pt] >= 0)
      return(tree_to_node[pt]);

    pt = tree[pt].parent_idx;
  }

  return(-1);
}

// Recursive column assignment: leaves consume one slot, parents center over
// their children.  cursor is the running leaf position.
static void
assign_columns(MasteryPane *mp, int idx, double *cursor)
{
  int first = -1, last = -1;
  int child_count = 0;

  for(int i = 0; i < mp->num_nodes; i++)
  {
    if(mp->nodes[i].parent_idx != idx)
      continue;

    assign_columns(mp, i, cursor);

    if(first < 0)
      first = i;

    last = i;
    child_count++;
  }

  if(child_count == 0)
  {
    mp->nodes[idx].colf = *cursor;
    *cursor += 1.0;
  }
  else
  {
    mp->nodes[idx].colf = (mp->nodes[first].colf + mp->nodes[last].colf) / 2.0;
  }
}

// Lay out the pane from the in-game (DB-authored) icon positions.  The skill
// columns occupy the canvas to the right of a fixed left gutter; that gutter
// holds the mastery level bar + tier labels (drawn separately) with the mastery
// icon at its base.  Horizontal bounds are over the skill nodes only (the
// mastery icon's far-left native X would otherwise open a wide empty gap);
// vertical bounds include the mastery icon, which sits below the lowest row.
static void
layout_pane_db(MasteryPane *mp)
{
  int min_x = 0, max_x = 0;
  int min_y = mp->mastery_pos_y, max_y = mp->mastery_pos_y;
  bool have_x = false;

  for(int i = 0; i < mp->num_nodes; i++)
  {
    if(!mp->nodes[i].has_pos)
      continue;

    if(!have_x)
    {
      min_x = max_x = mp->nodes[i].pos_x;
      have_x = true;
    }
    else
    {
      if(mp->nodes[i].pos_x < min_x) min_x = mp->nodes[i].pos_x;
      if(mp->nodes[i].pos_x > max_x) max_x = mp->nodes[i].pos_x;
    }

    if(mp->nodes[i].pos_y < min_y) min_y = mp->nodes[i].pos_y;
    if(mp->nodes[i].pos_y > max_y) max_y = mp->nodes[i].pos_y;
  }

  if(!have_x)
    min_x = max_x = mp->mastery_pos_x;

  double left = SK_MARGIN_X + SK_BAR_GUTTER;

  for(int i = 0; i < mp->num_nodes; i++)
  {
    int x = mp->nodes[i].has_pos ? mp->nodes[i].pos_x : min_x;
    int y = mp->nodes[i].has_pos ? mp->nodes[i].pos_y : min_y;

    mp->nodes[i].x = left + SK_ICON / 2.0 + (x - min_x) * SK_DB_SCALE;
    mp->nodes[i].y = SK_MARGIN_Y + SK_ICON / 2.0 + (y - min_y) * SK_DB_SCALE;
  }

  // Mastery icon: base of the gutter (under the level bar), at the bottom.
  mp->mastery_x = SK_MARGIN_X + SK_BAR_GUTTER / 2.0;
  mp->mastery_y = SK_MARGIN_Y + SK_ICON / 2.0 + (mp->mastery_pos_y - min_y) * SK_DB_SCALE;

  mp->content_w = left + SK_ICON + (max_x - min_x) * SK_DB_SCALE + SK_MARGIN_X;
  mp->content_h = SK_MARGIN_Y * 2.0 + SK_ICON + (max_y - min_y) * SK_DB_SCALE
                  + SK_COUNTER_DY + 6.0;
}

// Compute (row, colf, x, y) for every node and the content extents.
static void
layout_pane(MasteryPane *mp)
{
  // Preferred: lay out exactly as the in-game skill window does.
  if(mp->use_db_layout)
  {
    layout_pane_db(mp);
    return;
  }

  // Fallback (no DB positions, e.g. a modded mastery): derive a tree grid.

  // Rows from tier (tier 1 at the bottom, tier 7 at the top).
  for(int i = 0; i < mp->num_nodes; i++)
  {
    int tier = mp->nodes[i].skill_tier;

    if(tier < 1)
      tier = 1;
    if(tier > MAX_TIER)
      tier = MAX_TIER;

    mp->nodes[i].row = MAX_TIER - tier;
  }

  // Tier-less sub-skills (e.g. pet/elemental modifiers with no skillTier) sit
  // just above their parent rather than collapsing to the bottom row.  Parents
  // precede children in tree order, so a single forward pass resolves chains.
  for(int i = 0; i < mp->num_nodes; i++)
  {
    if(mp->nodes[i].skill_tier <= 0 && mp->nodes[i].parent_idx >= 0)
    {
      int pr = mp->nodes[mp->nodes[i].parent_idx].row;

      mp->nodes[i].row = (pr > 0) ? pr - 1 : 0;
    }
  }

  // Columns: auto tree layout (leaves consume slots, parents center over them).
  double cursor = 0.0;

  for(int i = 0; i < mp->num_nodes; i++)
  {
    if(!mp->nodes[i].is_base)
      continue;

    assign_columns(mp, i, &cursor);
  }

  // Pixel centers + extents.
  double max_colf = 0.0;
  int max_row = MAX_TIER; // reserve the mastery row at the bottom

  for(int i = 0; i < mp->num_nodes; i++)
  {
    mp->nodes[i].x = SK_MARGIN_X + SK_ICON / 2.0 + mp->nodes[i].colf * SK_COL_W;
    mp->nodes[i].y = SK_MARGIN_Y + SK_ICON / 2.0 + mp->nodes[i].row * SK_ROW_H;

    if(mp->nodes[i].colf > max_colf)
      max_colf = mp->nodes[i].colf;
    if(mp->nodes[i].row > max_row)
      max_row = mp->nodes[i].row;
  }

  // Mastery node: bottom-left, one row below the lowest tier.
  mp->mastery_x = SK_MARGIN_X + SK_ICON / 2.0;
  mp->mastery_y = SK_MARGIN_Y + SK_ICON / 2.0 + (MAX_TIER) * SK_ROW_H;

  mp->content_w = SK_MARGIN_X * 2.0 + SK_ICON + max_colf * SK_COL_W;
  mp->content_h = SK_MARGIN_Y * 2.0 + SK_ICON + max_row * SK_ROW_H + SK_COUNTER_DY + 6.0;
}

// (Re)build the node model for a pane from the skill tree + character save.
static void
build_mastery_model(SkillsState *st, int pane_idx)
{
  MasteryPane *mp = &st->panes[pane_idx];
  TQCharacter *chr = st->widgets->current_character;

  mp->num_nodes = 0;
  mp->mastery_chr_skill_idx = -1;
  mp->mastery_up_tex[0] = '\0';
  mp->mastery_down_tex[0] = '\0';
  mp->use_db_layout = false;
  mp->mastery_has_pos = false;
  mp->mastery_pos_x = 0;
  mp->mastery_pos_y = 0;

  if(mp->mastery_def_idx < 0)
    return;

  const MasteryDef *mdef = &mastery_defs[mp->mastery_def_idx];

  // In-game icon positions: if the mastery has a skill-window button, lay the
  // whole pane out from the authored positions and treat the button set as the
  // authoritative list of visible skills.  Otherwise fall back to the grid.
  SkillIconPos mpos;

  mp->mastery_has_pos = skill_layout_lookup(mdef->dbr_path, &mpos);

  if(mp->mastery_has_pos)
  {
    mp->mastery_pos_x = mpos.pos_x;
    mp->mastery_pos_y = mpos.pos_y;
  }

  mp->use_db_layout = mp->mastery_has_pos;

  // Mastery record in the save + its icon.
  for(int i = 0; i < chr->num_skills; i++)
  {
    const char *path = chr->skills[i].skill_name;

    if(!path || !path[0])
      continue;

    if(find_mastery_for_skill(path) == mp->mastery_def_idx && is_mastery_record(path))
    {
      mp->mastery_chr_skill_idx = i;
      mp->mastery_level = (int)st->work_levels[i];
      break;
    }
  }

  {
    TQArzRecordData *mdbr = asset_get_dbr(mdef->dbr_path);

    read_bitmap_field(mdbr, "skillUpBitmapName", mp->mastery_up_tex, sizeof(mp->mastery_up_tex));
    read_bitmap_field(mdbr, "skillDownBitmapName", mp->mastery_down_tex, sizeof(mp->mastery_down_tex));
  }

  // Load tree, keep only skills shown in-game (have a button position) or, in
  // fallback mode, those with an icon; build nodes.
  TreeEntry tree[MAX_TREE_ENTRIES];
  int num_tree = load_skill_tree(mdef->tree_path, tree, MAX_TREE_ENTRIES);
  int tree_to_node[MAX_TREE_ENTRIES];
  int node_src[MAX_SKILLS_PER_MASTERY];
  char seen_tags[MAX_SKILLS_PER_MASTERY][128];
  int num_seen = 0;

  for(int t = 0; t < MAX_TREE_ENTRIES; t++)
    tree_to_node[t] = -1;

  for(int t = 0; t < num_tree && mp->num_nodes < MAX_SKILLS_PER_MASTERY; t++)
  {
    TQArzRecordData *dbr = asset_get_dbr(tree[t].path);
    char up[256], down[256];

    resolve_skill_bitmaps(tree[t].path, up, sizeof(up), down, sizeof(down));

    SkillIconPos ipos;
    bool node_has_pos = skill_layout_lookup(tree[t].path, &ipos);

    if(mp->use_db_layout)
    {
      // In-game layout: show exactly the skills that have a skill-window button.
      if(!node_has_pos)
        continue;
    }
    else
    {
      // Fallback: need an icon, and drop duplicate records for the same skill
      // (a passive and its self-buff, per-element damage payloads, ...).
      if(!up[0])
        continue;

      char *disp_tag = dbr ? arz_record_get_string(dbr, "skillDisplayName", NULL) : NULL;
      const char *dedup_key = (disp_tag && disp_tag[0]) ? disp_tag : tree[t].basename;
      bool dup = false;

      for(int s = 0; s < num_seen; s++)
        if(strcasecmp(seen_tags[s], dedup_key) == 0)
        {
          dup = true;
          break;
        }

      if(!dup && num_seen < MAX_SKILLS_PER_MASTERY)
        snprintf(seen_tags[num_seen++], sizeof(seen_tags[0]), "%s", dedup_key);

      free(disp_tag);

      if(dup)
        continue;
    }

    SkillNode *n = &mp->nodes[mp->num_nodes];

    memset(n, 0, sizeof(*n));
    n->chr_skill_idx = -1;
    n->parent_idx = -1;
    g_strlcpy(n->skill_path, tree[t].path, sizeof(n->skill_path));
    g_strlcpy(n->basename, tree[t].basename, sizeof(n->basename));
    snprintf(n->up_tex, sizeof(n->up_tex), "%s", up);
    snprintf(n->down_tex, sizeof(n->down_tex), "%s", down);

    n->has_pos = node_has_pos;

    if(node_has_pos)
    {
      n->pos_x = ipos.pos_x;
      n->pos_y = ipos.pos_y;
    }

    get_skill_dbr_info(tree[t].path, &n->skill_tier, &n->max_level, &n->ultimate_level);
    n->mastery_req = arz_record_get_int(dbr, "skillMasteryLevelRequired", 0, NULL);

    // Equipment +skill bonuses applicable to this skill: all-skills + this
    // mastery + this exact skill.  Equipment is fixed, so this is stable for
    // the dialog's lifetime; the effective (capped) total is computed at draw
    // time from the current allocation.
    n->gear_all = st->gear.all_bonus;
    n->gear_mastery = st->gear.mastery_bonus[mp->mastery_def_idx];
    n->gear_skill = gear_skill_bonus(&st->gear, n->skill_path);

    resolve_skill_name(st->widgets, tree[t].path, n->display_name, sizeof(n->display_name));

    for(int i = 0; i < chr->num_skills; i++)
    {
      if(!chr->skills[i].skill_name)
        continue;

      char norm[256];

      normalize_path(chr->skills[i].skill_name, norm, sizeof(norm));

      if(strcasecmp(norm, n->skill_path) == 0)
      {
        n->chr_skill_idx = i;
        n->cur_level = (int)st->work_levels[i];
        break;
      }
    }

    node_src[mp->num_nodes] = t;
    tree_to_node[t] = mp->num_nodes;
    mp->num_nodes++;
  }

  // Resolve parents among visible nodes.
  for(int k = 0; k < mp->num_nodes; k++)
  {
    int p = map_visible_parent(tree, tree_to_node, node_src[k]);

    mp->nodes[k].parent_idx = p;
    mp->nodes[k].is_base = (p < 0);
  }

  layout_pane(mp);
}

// ── Icon cache helper ────────────────────────────────────────────────────

static int
hit_test(MasteryPane *mp, double px, double py)
{
  double r = SK_ICON / 2.0 + 4.0;

  if(fabs(px - mp->mastery_x) <= r && fabs(py - mp->mastery_y) <= r)
    return(HIT_MASTERY);

  for(int i = 0; i < mp->num_nodes; i++)
  {
    if(fabs(px - mp->nodes[i].x) <= r && fabs(py - mp->nodes[i].y) <= r)
      return(i);
  }

  return(HIT_NONE);
}

static void
on_canvas_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
  (void)n_press;
  GtkWidget *canvas = user_data;
  SkillsState *st = g_object_get_data(G_OBJECT(canvas), "state");
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(canvas), "pane"));
  MasteryPane *mp = &st->panes[pane];

  int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

  if(button != 1 && button != 3)
    return;

  GdkModifierType state =
    gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  bool shift = (state & GDK_SHIFT_MASK) != 0;

  int idx = hit_test(mp, x, y);

  if(idx == HIT_NONE)
    return;

  if(idx == HIT_MASTERY)
  {
    if(button == 1)
    {
      if(shift)
        while(mastery_inc(st, mp))
          ;
      else
        mastery_inc(st, mp);
    }
    else
    {
      if(shift)
        while(mp->mastery_level > 0)
          mastery_dec(st, mp);
      else
        mastery_dec(st, mp);
    }
  }
  else
  {
    if(button == 1)
    {
      if(shift)
        while(skill_inc(st, mp, idx))
          ;
      else
        skill_inc(st, mp, idx);
    }
    else
    {
      if(shift)
        zero_skill_and_dependents(st, mp, idx);
      else
        skill_dec(st, mp, idx);
    }
  }

  gtk_widget_queue_draw(st->panes[0].canvas);
  gtk_widget_queue_draw(st->panes[1].canvas);
  refresh_chrome(st);
}

static gboolean
on_canvas_tooltip(GtkWidget *canvas, int x, int y, gboolean keyboard,
                  GtkTooltip *tooltip, gpointer user_data)
{
  (void)keyboard;
  (void)user_data;
  SkillsState *st = g_object_get_data(G_OBJECT(canvas), "state");
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(canvas), "pane"));
  MasteryPane *mp = &st->panes[pane];

  int idx = hit_test(mp, x, y);

  if(idx == HIT_NONE)
    return(FALSE);

  char *markup;

  if(idx == HIT_MASTERY)
  {
    if(mp->mastery_def_idx < 0)
      return(FALSE);

    // The mastery bar itself is not raised by +skill gear bonuses.
    markup = skill_tooltip_markup(st->widgets, mastery_defs[mp->mastery_def_idx].dbr_path,
                                  mp->mastery_level, MAX_MASTERY_LEVEL, MAX_MASTERY_LEVEL,
                                  0, 0, 0, NULL);
  }
  else
  {
    SkillNode *n = &mp->nodes[idx];
    const char *mname = mp->mastery_def_idx >= 0 ? mastery_defs[mp->mastery_def_idx].name : NULL;

    markup = skill_tooltip_markup(st->widgets, n->skill_path, n->cur_level, n->max_level,
                                  n->ultimate_level, n->gear_all, n->gear_mastery,
                                  n->gear_skill, mname);
  }

  gtk_tooltip_set_markup(tooltip, markup);
  g_free(markup);
  return(TRUE);
}

// ── Mastery dropdown change ──────────────────────────────────────────────

static void
on_mastery_changed(GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  SkillsState *st = user_data;

  if(st->building)
    return;

  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dd), "pane"));
  MasteryPane *mp = &st->panes[pane];

  GtkStringObject *obj = gtk_drop_down_get_selected_item(dd);

  if(!obj)
    return;

  const char *name = gtk_string_object_get_string(obj);
  int new_def = -1;

  for(int m = 0; m < NUM_MASTERIES; m++)
  {
    if(strcmp(mastery_defs[m].name, name) == 0)
    {
      new_def = m;
      break;
    }
  }

  if(new_def < 0 || new_def == mp->mastery_def_idx)
    return;

  if(st->panes[1 - pane].mastery_def_idx == new_def)
    return; // can't pick the other pane's mastery

  // Refund all points spent in the old mastery.
  TQCharacter *chr = st->widgets->current_character;
  int old_def = mp->mastery_def_idx;

  if(old_def >= 0 && chr)
  {
    for(int i = 0; i < chr->num_skills; i++)
    {
      const char *path = chr->skills[i].skill_name;

      if(path && path[0] && find_mastery_for_skill(path) == old_def)
        st->work_levels[i] = 0;
    }
  }

  mp->mastery_level = 0;
  mp->mastery_def_idx = new_def;

  build_mastery_model(st, pane);
  apply_canvas_size(mp);
  gtk_widget_queue_draw(mp->canvas);
  refresh_chrome(st);
}

static void
on_reset_clicked(GtkButton *btn, gpointer user_data)
{
  SkillsState *st = user_data;
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pane"));

  mastery_reset(st, &st->panes[pane]);
  gtk_widget_queue_draw(st->panes[0].canvas);
  gtk_widget_queue_draw(st->panes[1].canvas);
  refresh_chrome(st);
}

// ── Chrome refresh ───────────────────────────────────────────────────────

static int
mastery_points_spent(SkillsState *st, int def)
{
  TQCharacter *chr = st->widgets->current_character;
  int spent = 0;

  if(!chr)
    return(0);

  for(int i = 0; i < chr->num_skills; i++)
  {
    const char *path = chr->skills[i].skill_name;

    if(path && path[0] && find_mastery_for_skill(path) == def)
      spent += (int)st->work_levels[i];
  }

  return(spent);
}

static void
refresh_chrome(SkillsState *st)
{
  int avail = compute_avail(st);
  char buf[160];

  snprintf(buf, sizeof(buf), "Available Skill Points: %d", avail);
  gtk_label_set_text(GTK_LABEL(st->avail_label), buf);
  gtk_widget_set_sensitive(st->apply_btn, has_changes(st));

  for(int p = 0; p < 2; p++)
  {
    MasteryPane *mp = &st->panes[p];

    if(mp->mastery_def_idx < 0)
    {
      gtk_widget_set_visible(mp->frame, FALSE);
      continue;
    }

    gtk_widget_set_visible(mp->frame, TRUE);

    int spent = mastery_points_spent(st, mp->mastery_def_idx);

    snprintf(buf, sizeof(buf), "Mastery %d / %d   ·   %d points spent",
             mp->mastery_level, MAX_MASTERY_LEVEL, spent);
    gtk_label_set_text(GTK_LABEL(mp->summary_label), buf);
  }
}

// ── Apply / Cancel / free ────────────────────────────────────────────────

static void
on_apply_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SkillsState *st = user_data;
  TQCharacter *chr = st->widgets->current_character;

  if(!chr)
    return;

  for(int i = 0; i < st->num_chr_skills; i++)
    chr->skills[i].skill_level = st->work_levels[i];

  chr->skill_points = (uint32_t)compute_avail(st);

  // Collect brand-new skills (not yet in the save) that got points; they are
  // spliced into the skill list. Dedup by path defensively (a skill belongs to
  // one mastery, so duplicates across panes shouldn't occur).
  const char *new_paths[2 * MAX_SKILLS_PER_MASTERY];
  uint32_t    new_levels[2 * MAX_SKILLS_PER_MASTERY];
  int         n_new = 0;

  for(int p = 0; p < 2; p++)
  {
    MasteryPane *mp = &st->panes[p];

    for(int i = 0; i < mp->num_nodes; i++)
    {
      SkillNode *n = &mp->nodes[i];

      if(n->chr_skill_idx >= 0 || n->cur_level <= 0)
        continue;

      int dup = 0;

      for(int k = 0; k < n_new; k++)
        if(g_ascii_strcasecmp(new_paths[k], n->skill_path) == 0)
        {
          dup = 1;
          break;
        }

      if(!dup && n_new < (int)G_N_ELEMENTS(new_paths))
      {
        new_paths[n_new]  = n->skill_path;
        new_levels[n_new] = (uint32_t)n->cur_level;
        n_new++;
      }
    }
  }

  // Adding skills splices bytes into the prefix and forces a reload below, which
  // would otherwise discard unsaved inventory/equipment edits. Persist those
  // first: character_save re-encodes them and refreshes raw_data while leaving
  // the skill list (in the prefix) byte-identical, so the splice offsets hold.
  if(n_new > 0 && st->widgets->char_dirty)
  {
    if(character_save(chr, chr->filepath) != 0)
      fprintf(stderr, "Skills: failed to persist pending edits before add\n");
  }

  int rc = character_save_skills_ex(chr, new_paths, new_levels, n_new);

  if(rc != 0)
  {
    fprintf(stderr, "Skills: failed to save\n");
  }
  else
  {
    bool state_valid = true;

    if(n_new > 0)
    {
      // The splice shifted every offset past the skill list, so the in-memory
      // character is stale; reload it from disk (update_ui frees the old one).
      TQCharacter *fresh = character_load(chr->filepath);

      if(fresh)
      {
        update_ui(st->widgets, fresh);
      }
      else
      {
        // Could not re-read what we just wrote (disk error). The skills are on
        // disk, but chr's offsets are stale -- don't mark dirty, lest a full
        // re-save splice at the wrong offsets.
        fprintf(stderr, "Skills: reload after add failed\n");
        update_ui(st->widgets, chr);
        state_valid = false;
      }
    }
    else
    {
      update_ui(st->widgets, chr);
    }

    // Skill edits leave the character dirty so the main-window Save button
    // stays active (update_ui above cleared the flag).
    if(state_valid)
      st->widgets->char_dirty = true;
  }

  update_save_button_sensitivity(st->widgets);

  gtk_window_close(GTK_WINDOW(st->dialog));
}

static void
on_cancel_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SkillsState *st = user_data;

  gtk_window_close(GTK_WINDOW(st->dialog));
}

static void
skills_state_free(gpointer data)
{
  SkillsState *st = data;

  free(st->work_levels);
  g_free(st);
}

// ── Pane construction ────────────────────────────────────────────────────

static void
apply_canvas_size(MasteryPane *mp)
{
  if(!mp->canvas)
    return;

  int cw = (int)(mp->content_w + 0.5);
  int ch = (int)(mp->content_h + 0.5);

  if(cw < 200)
    cw = 200;
  if(ch < 200)
    ch = 200;

  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(mp->canvas), cw);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(mp->canvas), ch);
}

static GtkWidget *
build_mastery_pane(SkillsState *st, int pane_idx)
{
  MasteryPane *mp = &st->panes[pane_idx];
  int other = 1 - pane_idx;
  GtkWidget *frame = gtk_frame_new(NULL);

  mp->frame = frame;
  gtk_widget_set_vexpand(frame, TRUE);
  gtk_widget_set_hexpand(frame, TRUE);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_margin_start(vbox, 6);
  gtk_widget_set_margin_end(vbox, 6);
  gtk_widget_set_margin_top(vbox, 6);
  gtk_widget_set_margin_bottom(vbox, 6);
  gtk_frame_set_child(GTK_FRAME(frame), vbox);

  // Header: mastery dropdown + reset button.
  GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkStringList *model = gtk_string_list_new(NULL);
  int sel_idx = 0, model_idx = 0;

  for(int m = 0; m < NUM_MASTERIES; m++)
  {
    if(st->panes[other].mastery_def_idx == m)
      continue;

    gtk_string_list_append(model, mastery_defs[m].name);

    if(m == mp->mastery_def_idx)
      sel_idx = model_idx;

    model_idx++;
  }

  GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(model), NULL);

  gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), (guint)sel_idx);
  g_object_set_data(G_OBJECT(dd), "pane", GINT_TO_POINTER(pane_idx));
  g_signal_connect(dd, "notify::selected", G_CALLBACK(on_mastery_changed), st);
  gtk_widget_set_hexpand(dd, TRUE);
  mp->dropdown = dd;
  gtk_box_append(GTK_BOX(hrow), dd);

  mp->reset_btn = gtk_button_new_with_label("Reset Mastery");
  g_object_set_data(G_OBJECT(mp->reset_btn), "pane", GINT_TO_POINTER(pane_idx));
  g_signal_connect(mp->reset_btn, "clicked", G_CALLBACK(on_reset_clicked), st);
  gtk_box_append(GTK_BOX(hrow), mp->reset_btn);

  gtk_box_append(GTK_BOX(vbox), hrow);

  // Summary line.
  mp->summary_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(mp->summary_label), 0.0);
  gtk_box_append(GTK_BOX(vbox), mp->summary_label);

  // Scrolled canvas.
  GtkWidget *scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_widget_set_hexpand(scroll, TRUE);
  mp->scroll = scroll;

  GtkWidget *canvas = gtk_drawing_area_new();

  mp->canvas = canvas;
  g_object_set_data(G_OBJECT(canvas), "state", st);
  g_object_set_data(G_OBJECT(canvas), "pane", GINT_TO_POINTER(pane_idx));
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(canvas), skill_canvas_draw_cb, st, NULL);

  GtkGesture *click = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  g_signal_connect(click, "pressed", G_CALLBACK(on_canvas_pressed), canvas);
  gtk_widget_add_controller(canvas, GTK_EVENT_CONTROLLER(click));

  gtk_widget_set_has_tooltip(canvas, TRUE);
  g_signal_connect(canvas, "query-tooltip", G_CALLBACK(on_canvas_tooltip), NULL);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), canvas);
  gtk_box_append(GTK_BOX(vbox), scroll);

  return(frame);
}

// ── Public entry point ───────────────────────────────────────────────────

void
show_skills_dialog(AppWidgets *widgets)
{
  TQCharacter *chr = widgets->current_character;

  if(!chr)
    return;

  if(!chr->off_skill_points)
  {
    fprintf(stderr, "Skills: skill offsets not available\n");
    return;
  }

  SkillsState *st = g_new0(SkillsState, 1);

  st->widgets = widgets;
  st->num_chr_skills = chr->num_skills;
  // A character with no skills is a valid edge (brand-new char): malloc(0) may
  // return NULL, which is not an error here — only bail on a real OOM (n>0).
  st->work_levels = chr->num_skills > 0
                    ? malloc((size_t)chr->num_skills * sizeof(uint32_t)) : NULL;

  if(chr->num_skills > 0 && !st->work_levels)
  {
    g_free(st);
    return;
  }

  int spent = 0;

  for(int i = 0; i < chr->num_skills; i++)
  {
    st->work_levels[i] = chr->skills[i].skill_level;
    spent += (int)chr->skills[i].skill_level;
  }

  st->total_skill_points = (int)chr->skill_points + spent;
  st->orig_skill_points = (int)chr->skill_points;

  // Equipment +skill bonuses are fixed while the dialog is open; compute once.
  compute_gear_bonuses(chr, &st->gear);

  for(int p = 0; p < 2; p++)
  {
    st->panes[p].mastery_def_idx = -1;
    st->panes[p].initial_mastery_def_idx = -1;
    st->panes[p].mastery_chr_skill_idx = -1;
  }

  // Identify the character's (up to two) masteries.
  for(int i = 0; i < chr->num_skills; i++)
  {
    if(!is_mastery_record(chr->skills[i].skill_name))
      continue;

    int mdef = find_mastery_for_skill(chr->skills[i].skill_name);

    if(mdef < 0)
      continue;

    for(int p = 0; p < 2; p++)
    {
      if(st->panes[p].mastery_def_idx < 0)
      {
        st->panes[p].mastery_def_idx = mdef;
        st->panes[p].initial_mastery_def_idx = mdef;
        st->panes[p].mastery_chr_skill_idx = i;
        st->panes[p].mastery_level = (int)st->work_levels[i];
        break;
      }
    }
  }

  GtkWidget *dialog = gtk_window_new();

  st->dialog = dialog;

  char title[256];

  snprintf(title, sizeof(title), "Skills — %s",
           chr->character_name ? chr->character_name : "Character");
  gtk_window_set_title(GTK_WINDOW(dialog), title);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  g_object_set_data_full(G_OBJECT(dialog), "skills-state", st, skills_state_free);

  GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

  gtk_widget_set_margin_start(main_vbox, 12);
  gtk_widget_set_margin_end(main_vbox, 12);
  gtk_widget_set_margin_top(main_vbox, 8);
  gtk_widget_set_margin_bottom(main_vbox, 8);
  gtk_window_set_child(GTK_WINDOW(dialog), main_vbox);

  st->avail_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(st->avail_label), 0.0);

  PangoAttrList *attrs = pango_attr_list_new();

  pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  pango_attr_list_insert(attrs, pango_attr_scale_new(1.1));
  gtk_label_set_attributes(GTK_LABEL(st->avail_label), attrs);
  pango_attr_list_unref(attrs);
  gtk_box_append(GTK_BOX(main_vbox), st->avail_label);

  GtkWidget *panes_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_vexpand(panes_box, TRUE);
  gtk_widget_set_hexpand(panes_box, TRUE);
  gtk_box_append(GTK_BOX(main_vbox), panes_box);

  st->building = true;

  for(int p = 0; p < 2; p++)
  {
    GtkWidget *pane = build_mastery_pane(st, p);

    gtk_box_append(GTK_BOX(panes_box), pane);
    build_mastery_model(st, p);
    apply_canvas_size(&st->panes[p]);
  }

  st->building = false;

  // Size the window to hold both whole trees without scrolling (capped so it
  // can't exceed a typical screen; scrollbars remain as a fallback).
  {
    double w0 = st->panes[0].mastery_def_idx >= 0 ? st->panes[0].content_w : 320.0;
    double w1 = st->panes[1].mastery_def_idx >= 0 ? st->panes[1].content_w : 320.0;
    double h0 = st->panes[0].mastery_def_idx >= 0 ? st->panes[0].content_h : 400.0;
    double h1 = st->panes[1].mastery_def_idx >= 0 ? st->panes[1].content_h : 400.0;
    double maxh = h0 > h1 ? h0 : h1;

    int win_w = (int)(w0 + w1 + 96.0);   // frame borders + pane gap + window margins
    int win_h = (int)(maxh + 196.0);     // avail label + header + summary + buttons + chrome

    if(win_w > 1850)
      win_w = 1850;
    if(win_w < 900)
      win_w = 900;
    if(win_h > 1040)
      win_h = 1040;
    if(win_h < 560)
      win_h = 560;

    gtk_window_set_default_size(GTK_WINDOW(dialog), win_w, win_h);
  }

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(main_vbox), btn_box);

  st->apply_btn = gtk_button_new_with_label("Apply");
  g_signal_connect(st->apply_btn, "clicked", G_CALLBACK(on_apply_clicked), st);
  gtk_widget_set_sensitive(st->apply_btn, FALSE);
  gtk_box_append(GTK_BOX(btn_box), st->apply_btn);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");

  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), st);
  gtk_box_append(GTK_BOX(btn_box), cancel_btn);

  refresh_chrome(st);
  gtk_window_present(GTK_WINDOW(dialog));
}

