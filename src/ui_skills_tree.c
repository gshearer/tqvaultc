// ui_skills_tree.c -- visual skill-tree editor (replaces ui_skills_dialog.c)
//
// Two side-by-side mastery panes.  Each pane draws its mastery's skills as a
// 2D tree of icons (cairo on a GtkDrawingArea) with connecting dependency
// lines and an "X / Y" current/max counter under each icon, mirroring the
// in-game / calculator skill window.  Left-click adds a point, right-click
// removes one, Shift maxes/zeroes.  Hovering shows an in-game-style tooltip.
// Changes are written to the save on "Apply".
//
// The point-allocation backend (tier gating, parent/child dependency,
// cascading zeroing, point-pool accounting) is ported from the former
// text-based ui_skills_dialog.c.  The layout is auto-derived from the skill
// DBR data (tier -> row, base-skill branches -> columns) and can be overridden
// per mastery via ui_skills_layout.{h,c}.

#include "ui.h"
#include "arz.h"
#include "asset_lookup.h"
#include "texture.h"
#include "translation.h"
#include "item_stats.h"
#include "ui_skills_layout.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

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

// Tier -> minimum mastery level thresholds.  These are the game's canonical
// tier levels (records/game/gameengine.dbr: skillMasteryTierLevel).
static const int tier_mastery_req[] = {
  [0] = 0, [1] = 1, [2] = 4, [3] = 10, [4] = 16, [5] = 24, [6] = 32, [7] = 40,
};

// ── Mastery definitions ─────────────────────────────────────────────────

typedef struct {
  const char *name;
  const char *dbr_path;
  const char *dir_prefix;
  const char *tree_path;
} MasteryDef;

static const MasteryDef mastery_defs[] = {
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
#define NUM_MASTERIES 11

// XPack3 AllMasteries skill prefix -> owning mastery index.
static const struct { const char *prefix; int mastery_idx; } allmastery_map[] = {
  { "defense_", 0 }, { "earth_", 1 }, { "hunting_", 2 }, { "nature_", 3 },
  { "spirit_", 4 },  { "storm_", 5 }, { "warfare_", 6 }, { "dream_", 7 },
  { "rune_", 8 },    { "stealth_", 9 }, { "neidan_", 10 },
};
#define NUM_ALLMASTERY_MAP 11

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

// ── Forward declarations ────────────────────────────────────────────────

static void build_mastery_model(SkillsState *st, int pane_idx);
static void refresh_chrome(SkillsState *st);
static void apply_canvas_size(MasteryPane *mp);

// ── Path helpers ────────────────────────────────────────────────────────

static void
normalize_path(const char *src, char *dst, size_t dst_size)
{
  size_t i = 0;

  for(; src[i] && i < dst_size - 1; i++)
  {
    char c = src[i];

    if(c == '/')
      c = '\\';

    dst[i] = (char)tolower((unsigned char)c);
  }

  dst[i] = '\0';
}

static void
extract_basename(const char *path, char *out, size_t out_size)
{
  const char *base = path;

  for(const char *p = path; *p; p++)
    if(*p == '/' || *p == '\\')
      base = p + 1;

  const char *dot = strrchr(base, '.');
  size_t len = dot ? (size_t)(dot - base) : strlen(base);

  if(len >= out_size)
    len = out_size - 1;

  for(size_t i = 0; i < len; i++)
    out[i] = (char)tolower((unsigned char)base[i]);

  out[len] = '\0';
}

static int
find_mastery_for_skill(const char *skill_path)
{
  if(!skill_path)
    return(-1);

  for(int m = 0; m < NUM_MASTERIES; m++)
  {
    if(strncasecmp(skill_path, mastery_defs[m].dir_prefix,
                   strlen(mastery_defs[m].dir_prefix)) == 0)
      return(m);
  }

  const char *allm = "records\\xpack3\\skills\\allmasteries\\";
  size_t allm_len = strlen(allm);

  if(strncasecmp(skill_path, allm, allm_len) == 0)
  {
    const char *filename = skill_path + allm_len;

    for(int i = 0; i < NUM_ALLMASTERY_MAP; i++)
    {
      if(strncasecmp(filename, allmastery_map[i].prefix,
                     strlen(allmastery_map[i].prefix)) == 0)
        return(allmastery_map[i].mastery_idx);
    }
  }

  return(-1);
}

static bool
is_mastery_record(const char *path)
{
  if(!path)
    return(false);

  return(strcasestr(path, "mastery.dbr") != NULL);
}

// ── DBR-backed name / tier / icon resolution ─────────────────────────────

static bool
resolve_display_tag(AppWidgets *widgets, TQArzRecordData *dbr,
                    char *out, size_t out_size)
{
  char *tag = arz_record_get_string(dbr, "skillDisplayName", NULL);

  if(!tag)
    return(false);

  const char *translated = translation_get(widgets->translations, tag);

  if(translated && translated[0])
  {
    snprintf(out, out_size, "%s", translated);
    free(tag);
    return(true);
  }

  if(tag[0] && strncmp(tag, "tag", 3) != 0)
  {
    snprintf(out, out_size, "%s", tag);
    free(tag);
    return(true);
  }

  free(tag);
  return(false);
}

static void
resolve_skill_name(AppWidgets *widgets, const char *skill_path,
                   char *out, size_t out_size)
{
  out[0] = '\0';

  TQArzRecordData *dbr = asset_get_dbr(skill_path);

  if(dbr)
  {
    if(resolve_display_tag(widgets, dbr, out, out_size))
      return;

    static const char *ref_fields[] = { "buffSkillName", "petSkillName", NULL };

    for(int r = 0; ref_fields[r]; r++)
    {
      char *ref_path = arz_record_get_string(dbr, ref_fields[r], NULL);

      if(ref_path)
      {
        TQArzRecordData *ref_dbr = asset_get_dbr(ref_path);

        if(ref_dbr && resolve_display_tag(widgets, ref_dbr, out, out_size))
        {
          free(ref_path);
          return;
        }

        free(ref_path);
      }
    }
  }

  const char *base = skill_path;

  for(const char *p = skill_path; *p; p++)
    if(*p == '/' || *p == '\\')
      base = p + 1;

  const char *dot = strrchr(base, '.');
  size_t len = dot ? (size_t)(dot - base) : strlen(base);

  if(len >= out_size)
    len = out_size - 1;

  memcpy(out, base, len);
  out[len] = '\0';
}

static bool
get_tier_from_ref(const char *ref_field, TQArzRecordData *dbr, int *skill_tier)
{
  char *ref_path = arz_record_get_string(dbr, ref_field, NULL);

  if(!ref_path)
    return(false);

  TQArzRecordData *ref_dbr = asset_get_dbr(ref_path);

  free(ref_path);

  if(!ref_dbr)
    return(false);

  int bt = arz_record_get_int(ref_dbr, "skillTier", 0, NULL);

  if(bt > 0)
  {
    *skill_tier = bt;
    return(true);
  }

  return(false);
}

// Find skillMaxLevel + skillUltimateLevel for a skill, following the pet/buff
// ref chain when the record itself doesn't carry them.  The level data can sit
// two records deep (e.g. a pet modifier -> its pet skill -> that skill's buff),
// so this recurses.  Returns true once a record carrying skillMaxLevel is hit.
static bool
resolve_skill_levels(const char *path, int depth, int *max_level, int *ultimate_level)
{
  if(!path || !path[0] || depth > 4)
    return(false);

  TQArzRecordData *d = asset_get_dbr(path);

  if(!d)
    return(false);

  TQVariable *mv = arz_record_get_var(d, arz_intern("skillMaxLevel"));

  if(mv && mv->count > 0)
  {
    *max_level = arz_record_get_int(d, "skillMaxLevel", 1, NULL);
    *ultimate_level = arz_record_get_int(d, "skillUltimateLevel", *max_level, NULL);
    return(true);
  }

  static const char *refs[] = { "petSkillName", "buffSkillName", NULL };

  for(int r = 0; refs[r]; r++)
  {
    char *rp = arz_record_get_string(d, refs[r], NULL);

    if(rp)
    {
      bool got = resolve_skill_levels(rp, depth + 1, max_level, ultimate_level);

      free(rp);

      if(got)
        return(true);
    }
  }

  return(false);
}

// Read skillTier + skillMaxLevel + skillUltimateLevel.  Tier resolution follows
// a single buff/pet ref when absent (unchanged -- it drives gating/layout);
// max/ultimate follow the ref chain to whatever depth holds them.
// ultimate_level is the cap a skill reaches with equipment bonuses (usually
// max_level + 4); it defaults to max_level when not set.
static void
get_skill_dbr_info(const char *skill_path, int *skill_tier, int *max_level,
                   int *ultimate_level)
{
  *skill_tier = 0;
  *max_level = 1;
  *ultimate_level = 1;

  TQArzRecordData *dbr = asset_get_dbr(skill_path);

  if(!dbr)
    return;

  *skill_tier = arz_record_get_int(dbr, "skillTier", 0, NULL);

  resolve_skill_levels(skill_path, 0, max_level, ultimate_level);

  if(*skill_tier == 0)
  {
    if(!get_tier_from_ref("buffSkillName", dbr, skill_tier))
      get_tier_from_ref("petSkillName", dbr, skill_tier);
  }
}

// Copy a bitmap field value into out, appending ".tex" if it has no extension.
static void
read_bitmap_field(TQArzRecordData *dbr, const char *field, char *out, size_t out_size)
{
  out[0] = '\0';

  if(!dbr)
    return;

  char *v = arz_record_get_string(dbr, field, NULL);

  if(!v)
    return;

  if(v[0])
  {
    if(strrchr(v, '.'))
      snprintf(out, out_size, "%s", v);
    else
      snprintf(out, out_size, "%s.tex", v);
  }

  free(v);
}

// Resolve a skill's up/down icon paths.  Buff/toggle and pet-modifier skills
// store no bitmap on their own record -- the icon lives in the referenced
// buffSkillName/petSkillName record (same place get_skill_dbr_info reads tier).
static void
resolve_skill_bitmaps(const char *skill_path, char *up, size_t up_sz,
                      char *down, size_t down_sz)
{
  up[0] = '\0';
  down[0] = '\0';

  TQArzRecordData *dbr = asset_get_dbr(skill_path);

  if(!dbr)
    return;

  read_bitmap_field(dbr, "skillUpBitmapName", up, up_sz);
  read_bitmap_field(dbr, "skillDownBitmapName", down, down_sz);

  if(up[0])
    return;

  static const char *ref_fields[] = { "buffSkillName", "petSkillName", NULL };

  for(int r = 0; ref_fields[r]; r++)
  {
    char *ref = arz_record_get_string(dbr, ref_fields[r], NULL);

    if(!ref)
      continue;

    TQArzRecordData *rd = asset_get_dbr(ref);

    free(ref);

    if(rd)
    {
      read_bitmap_field(rd, "skillUpBitmapName", up, up_sz);
      read_bitmap_field(rd, "skillDownBitmapName", down, down_sz);

      if(up[0])
        return;
    }
  }
}

// Resolve the record that actually carries a skill's description and stats.
// Buff/toggle/aura skills (e.g. Deathchill Aura) are a thin shell that the skill
// tree points at: the description, mana cost and effect stats live in the
// referenced buffSkillName/petSkillName record.  When the main record has no
// skillBaseDescription, follow those refs to the record that does.  Falls back
// to skill_path (the common case: the skill describes itself).
static void
resolve_effective_skill_path(const char *skill_path, char *out, size_t out_sz)
{
  snprintf(out, out_sz, "%s", skill_path);

  TQArzRecordData *dbr = asset_get_dbr(skill_path);

  if(!dbr)
    return;

  char *desc = arz_record_get_string(dbr, "skillBaseDescription", NULL);
  bool self_described = desc && desc[0];

  free(desc);

  if(self_described)
    return;

  static const char *ref_fields[] = { "buffSkillName", "petSkillName", NULL };

  for(int r = 0; ref_fields[r]; r++)
  {
    char *ref = arz_record_get_string(dbr, ref_fields[r], NULL);

    if(!ref)
      continue;

    TQArzRecordData *rd = asset_get_dbr(ref);

    if(rd)
    {
      char *rdesc = arz_record_get_string(rd, "skillBaseDescription", NULL);
      bool has = rdesc && rdesc[0];

      free(rdesc);

      if(has)
      {
        snprintf(out, out_sz, "%s", ref);
        free(ref);
        return;
      }
    }

    free(ref);
  }
}

// ── Skill tree loading ──────────────────────────────────────────────────

static int
load_skill_tree(const char *tree_dbr_path, TreeEntry *entries, int max_entries)
{
  TQArzRecordData *dbr = asset_get_dbr(tree_dbr_path);

  if(!dbr)
    return(0);

  int count = 0;

  for(int n = 1; n <= 30 && count < max_entries; n++)
  {
    char key[32];

    snprintf(key, sizeof(key), "skillName%d", n);
    char *path = arz_record_get_string(dbr, key, NULL);

    if(!path)
      continue;

    TreeEntry *te = &entries[count];

    normalize_path(path, te->path, sizeof(te->path));
    extract_basename(path, te->basename, sizeof(te->basename));
    te->parent_idx = -1;
    free(path);

    if(strcasestr(te->basename, "mastery") != NULL)
      continue;

    for(int j = count - 1; j >= 0; j--)
    {
      size_t plen = strlen(entries[j].basename);

      if(strncmp(te->basename, entries[j].basename, plen) == 0 &&
         (te->basename[plen] == '_' || te->basename[plen] == '\0'))
      {
        te->parent_idx = j;
        break;
      }
    }

    if(te->parent_idx < 0 && count > 0)
    {
      TQArzRecordData *sdbr = asset_get_dbr(te->path);

      if(sdbr)
      {
        char *cls = arz_record_get_string(sdbr, "Class", NULL);

        if(cls)
        {
          if(strcasestr(cls, "Modifier") || strcasestr(cls, "Secondary"))
            te->parent_idx = count - 1;

          free(cls);
        }
      }
    }

    count++;
  }

  return(count);
}

// ── Equipment +skill bonus accounting ─────────────────────────────────────

// Read an augment level (single- or multi-valued) at index 0, rounded to int.
static int
aug_int_value(const TQVariable *v)
{
  if(!v || v->count == 0)
    return(0);

  if(v->type == TQ_VAR_FLOAT && v->value.f32)
    return((int)(v->value.f32[0] + (v->value.f32[0] < 0 ? -0.5f : 0.5f)));

  if(v->type == TQ_VAR_INT && v->value.i32)
    return(v->value.i32[0]);

  return(0);
}

// Record a +N bonus to a specific skill (by normalized DBR path).
static void
gear_add_skill(GearBonuses *g, const char *raw_path, int bonus)
{
  if(!raw_path || !raw_path[0] || bonus == 0)
    return;

  char norm[256];

  normalize_path(raw_path, norm, sizeof(norm));

  for(int i = 0; i < g->num_skill; i++)
  {
    if(strcmp(g->skill[i].path, norm) == 0)
    {
      g->skill[i].bonus += bonus;
      return;
    }
  }

  if(g->num_skill < MAX_GEAR_SKILL_AUG)
  {
    snprintf(g->skill[g->num_skill].path, sizeof(g->skill[0].path), "%s", norm);
    g->skill[g->num_skill].bonus = bonus;
    g->num_skill++;
  }
}

// Accumulate augmentAllLevel / augmentMastery* / augmentSkill* from one DBR
// component (base, prefix, suffix, relic, completion bonus) into the table.
// Mirrors the field layout item_stats.c reads for tooltips.
static void
gear_scan_record(const char *path, GearBonuses *g)
{
  if(!path || !path[0])
    return;

  TQArzRecordData *d = asset_get_dbr(path);

  if(!d)
    return;

  for(uint32_t i = 0; i < d->num_vars; i++)
  {
    const char *name = d->vars[i].name;

    if(!name)
      continue;

    if(strcasecmp(name, "augmentAllLevel") == 0)
    {
      g->all_bonus += aug_int_value(&d->vars[i]);
      continue;
    }

    if(strncasecmp(name, "augmentMasteryLevel", 19) == 0)
    {
      int lvl = aug_int_value(&d->vars[i]);

      if(lvl == 0)
        continue;

      char nm[64];

      snprintf(nm, sizeof(nm), "augmentMasteryName%s", name + 19);
      TQVariable *mv = arz_record_get_var(d, arz_intern(nm));
      const char *mpath =
        (mv && mv->type == TQ_VAR_STRING && mv->count > 0) ? mv->value.str[0] : NULL;

      if(!mpath || !mpath[0])
        continue;

      char mnorm[256];

      normalize_path(mpath, mnorm, sizeof(mnorm));
      int mdef = find_mastery_for_skill(mnorm);

      if(mdef >= 0 && mdef < NUM_MASTERIES)
        g->mastery_bonus[mdef] += lvl;

      continue;
    }

    if(strncasecmp(name, "augmentSkillLevel", 17) == 0)
    {
      int lvl = aug_int_value(&d->vars[i]);

      if(lvl == 0)
        continue;

      char nm[64];

      snprintf(nm, sizeof(nm), "augmentSkillName%s", name + 17);
      TQVariable *sv = arz_record_get_var(d, arz_intern(nm));
      const char *spath =
        (sv && sv->type == TQ_VAR_STRING && sv->count > 0) ? sv->value.str[0] : NULL;

      gear_add_skill(g, spath, lvl);
    }
  }
}

// Scan every component of one equipped item.
static void
gear_scan_item(TQItem *it, GearBonuses *g)
{
  if(!it)
    return;

  gear_scan_record(it->base_name, g);
  gear_scan_record(it->prefix_name, g);
  gear_scan_record(it->suffix_name, g);
  gear_scan_record(it->relic_name, g);
  gear_scan_record(it->relic_bonus, g);
  gear_scan_record(it->relic_name2, g);
  gear_scan_record(it->relic_bonus2, g);
}

// Build the character's full +skill bonus table from all equipped items.
static void
compute_gear_bonuses(TQCharacter *chr, GearBonuses *g)
{
  memset(g, 0, sizeof(*g));

  if(!chr)
    return;

  for(int i = 0; i < 12; i++)
    gear_scan_item(chr->equipment[i], g);
}

// Skill-specific bonus for a normalized skill path (0 if none).
static int
gear_skill_bonus(const GearBonuses *g, const char *norm_path)
{
  for(int i = 0; i < g->num_skill; i++)
    if(strcmp(g->skill[i].path, norm_path) == 0)
      return(g->skill[i].bonus);

  return(0);
}

// Effective level of a skill at `alloc` allocated points: equipment bonuses
// apply only once at least one point is spent, and the total is clamped to the
// skill's ultimate cap.
static int
effective_level(int alloc, int raw_bonus, int ultimate)
{
  if(alloc < 1)
    return(alloc);

  int e = alloc + (raw_bonus > 0 ? raw_bonus : 0);

  // Clamp to the ultimate cap, but never below what is actually allocated
  // (guards against an unreadable/under-reported cap yielding a negative bonus).
  if(e > ultimate)
    e = ultimate;
  if(e < alloc)
    e = alloc;

  return(e);
}

// ── Accounting / gating / cascade ────────────────────────────────────────

static int
compute_avail(SkillsState *st)
{
  int spent = 0;

  for(int i = 0; i < st->num_chr_skills; i++)
    spent += (int)st->work_levels[i];

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

  return(false);
}

static bool
skill_is_accessible(MasteryPane *mp, int idx)
{
  SkillNode *n = &mp->nodes[idx];
  int tier = n->skill_tier;

  if(tier >= 1 && tier <= MAX_TIER)
  {
    if(mp->mastery_level < tier_mastery_req[tier])
      return(false);
  }

  if(n->parent_idx >= 0 && n->parent_idx < mp->num_nodes)
  {
    if(mp->nodes[n->parent_idx].cur_level <= 0)
      return(false);

    if(!skill_is_accessible(mp, n->parent_idx))
      return(false);
  }

  return(true);
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

  if(n->chr_skill_idx < 0)
    return(false);
  if(!skill_is_accessible(mp, idx))
    return(false);
  if(n->cur_level >= n->max_level)
    return(false);
  if(compute_avail(st) <= 0)
    return(false);

  n->cur_level++;
  st->work_levels[n->chr_skill_idx] = (uint32_t)n->cur_level;
  return(true);
}

static void
skill_dec(SkillsState *st, MasteryPane *mp, int idx)
{
  SkillNode *n = &mp->nodes[idx];

  if(n->chr_skill_idx < 0 || n->cur_level <= 0)
    return;

  n->cur_level--;
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
    strncpy(n->skill_path, tree[t].path, sizeof(n->skill_path) - 1);
    strncpy(n->basename, tree[t].basename, sizeof(n->basename) - 1);
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

static GdkPixbuf *
skill_icon(AppWidgets *w, const char *tex)
{
  if(!tex || !tex[0])
    return(NULL);

  GdkPixbuf *cached = g_hash_table_lookup(w->texture_cache, tex);

  if(cached)
    return(g_object_ref(cached));

  GdkPixbuf *pb = texture_load(tex);

  if(pb)
    g_hash_table_insert(w->texture_cache, strdup(tex), g_object_ref(pb));

  return(pb);
}

// ── Tooltip composer ─────────────────────────────────────────────────────

// Replace every occurrence of `from` with `to`, returning a new string.
static char *
str_replace_all(const char *s, const char *from, const char *to)
{
  GString *g = g_string_new(NULL);
  size_t fl = strlen(from);

  for(const char *p = s; *p;)
  {
    if(strncmp(p, from, fl) == 0)
    {
      g_string_append(g, to);
      p += fl;
    }
    else
    {
      g_string_append_c(g, *p++);
    }
  }

  return(g_string_free(g, FALSE));
}

// Build in-game-style Pango markup for a skill (or the mastery) at `level`.
// The gear_* arguments carry the character's equipment +skill bonuses for this
// skill (0 for the mastery node); when present, the level lines show the
// boosted total, the stat previews reflect the effective level, and a breakdown
// of the bonus sources is appended.  mastery_name labels the mastery-wide
// bonus (may be NULL).
static char *
skill_tooltip_markup(AppWidgets *w, const char *skill_path, int level,
                     int max_level, int ultimate_level, int gear_all,
                     int gear_mastery, int gear_skill, const char *mastery_name)
{
  char raw[16384];
  BufWriter bw;

  buf_init(&bw, raw, sizeof(raw));

  char name[128];

  resolve_skill_name(w, skill_path, name, sizeof(name));
  char *ename = escape_markup(name);

  buf_write(&bw, "<b>%s</b>\n", ename ? ename : name);
  free(ename);

  // Buff/toggle/aura skills keep their description + stats in the referenced
  // record, not the shell the tree points at.  Resolve that record for both.
  char eff_path[256];

  resolve_effective_skill_path(skill_path, eff_path, sizeof(eff_path));

  TQArzRecordData *dbr = asset_get_dbr(eff_path);

  if(dbr)
  {
    char *dtag = arz_record_get_string(dbr, "skillBaseDescription", NULL);

    if(dtag)
    {
      const char *d = translation_get(w->translations, dtag);

      if(d && d[0])
      {
        char *ed = escape_markup(d);

        buf_write(&bw, "%s\n", ed ? ed : d);
        free(ed);
      }

      free(dtag);
    }
  }

  buf_write(&bw, "\n");

  const char *WHITE = "#E0E0E0";
  int raw_bonus = gear_all + gear_mastery + gear_skill;

  if(level > 0)
  {
    int eff = effective_level(level, raw_bonus, ultimate_level);
    int shown = eff - level;

    if(shown > 0)
      buf_write(&bw, "<span color='#FFD200'>Current Level: %d "
                     "<span color='#5FE85F'>(+%d = %d)</span></span>\n",
                level, shown, eff);
    else
      buf_write(&bw, "<span color='#FFD200'>Current Level: %d</span>\n", level);

    add_stats_from_record(eff_path, w->translations, &bw, WHITE, eff - 1);
  }

  if(level < max_level)
  {
    if(level > 0)
      buf_write(&bw, "\n");

    int nxt = level + 1;
    int eff = effective_level(nxt, raw_bonus, ultimate_level);
    int shown = eff - nxt;

    if(shown > 0)
      buf_write(&bw, "<span color='#FFD200'>Next Level: %d "
                     "<span color='#5FE85F'>(+%d = %d)</span></span>\n",
                nxt, shown, eff);
    else
      buf_write(&bw, "<span color='#FFD200'>Next Level: %d</span>\n", nxt);

    add_stats_from_record(eff_path, w->translations, &bw, WHITE, eff - 1);
  }

  // Equipment +skill bonus breakdown (which gear sources feed this skill, and a
  // reminder that a point must be spent for them to take effect).
  if(raw_bonus > 0)
  {
    buf_write(&bw, "\n<span color='#5FE85F'>Equipment Bonus:</span>\n");

    if(gear_all > 0)
      buf_write(&bw, "<span color='%s'>  +%d to All Skills</span>\n", WHITE, gear_all);

    if(gear_mastery > 0)
      buf_write(&bw, "<span color='%s'>  +%d to %s Mastery Skills</span>\n",
                WHITE, gear_mastery, mastery_name ? mastery_name : "this");

    if(gear_skill > 0)
      buf_write(&bw, "<span color='%s'>  +%d to this Skill</span>\n", WHITE, gear_skill);

    if(level < 1)
      buf_write(&bw, "<span color='#AAAAAA'>  (spend at least 1 point to apply)</span>\n");
  }

  buf_write(&bw, "\n<span color='#40FF40'>Left click to add unused skill points. "
                 "Right click to remove.</span>");

  // Skill tooltips use "Intellect" where item stats say "Intelligence".
  return(str_replace_all(raw, "Intelligence", "Intellect"));
}

// ── Drawing ──────────────────────────────────────────────────────────────

static void
blit_icon(cairo_t *cr, GdkPixbuf *pb, double cx, double cy, double size, double alpha)
{
  if(!pb)
    return;

  int pw = gdk_pixbuf_get_width(pb);
  int ph = gdk_pixbuf_get_height(pb);

  if(pw < 1 || ph < 1)
    return;

  cairo_save(cr);
  cairo_translate(cr, cx - size / 2.0, cy - size / 2.0);
  cairo_scale(cr, size / (double)pw, size / (double)ph);
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
  G_GNUC_END_IGNORE_DEPRECATIONS

  if(alpha >= 1.0)
    cairo_paint(cr);
  else
    cairo_paint_with_alpha(cr, alpha);

  cairo_restore(cr);
}

// Draw a counter string centered in the given colour, with a dark outline for
// legibility.  Used for the mastery "cur / max" line and each skill's level.
static void
draw_counter(cairo_t *cr, const char *txt, double cx, double top_y,
             double r, double g, double b)
{
  cairo_save(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 12.0);

  cairo_text_extents_t te;

  cairo_text_extents(cr, txt, &te);
  double tx = cx - te.width / 2.0 - te.x_bearing;
  double ty = top_y + SK_COUNTER_DY;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.85);

  for(int dx = -1; dx <= 1; dx++)
    for(int dy = -1; dy <= 1; dy++)
    {
      if(dx == 0 && dy == 0)
        continue;

      cairo_move_to(cr, tx + dx, ty + dy);
      cairo_show_text(cr, txt);
    }

  cairo_set_source_rgb(cr, r, g, b);
  cairo_move_to(cr, tx, ty);
  cairo_show_text(cr, txt);
  cairo_restore(cr);
}

// Connector between a parent (lower) and a child (higher).  In-game, every
// skill in a group shares a column and the link runs as a vertical trunk in
// the gap just to the RIGHT of the column, tapped by a short horizontal stub
// into each icon -- so the line never passes through an unrelated icon that
// merely shares the column.  Stubs start a few px inside the icon edge; since
// connectors are drawn under the icons, that overlap is hidden and leaves no
// gap.  A cross-column link (rare; only from a parent-detection fallback) keeps
// the old orthogonal bracket.
static void
draw_edge(cairo_t *cr, double px, double py, double cx, double cy, bool on)
{
  if(on)
    cairo_set_source_rgb(cr, 0.85, 0.78, 0.45);
  else
    cairo_set_source_rgb(cr, 0.34, 0.34, 0.38);

  cairo_set_line_width(cr, on ? 2.5 : 1.5);

  if(fabs(px - cx) < 1.0)
  {
    double stub_in = px + SK_ICON / 2.0 - 4.0;  // a touch inside the icon edge
    double trunk_x = px + SK_ICON / 2.0 + SK_STUB;

    cairo_move_to(cr, stub_in, py);   // stub into the parent
    cairo_line_to(cr, trunk_x, py);
    cairo_move_to(cr, stub_in, cy);   // stub into the child
    cairo_line_to(cr, trunk_x, cy);
    cairo_move_to(cr, trunk_x, py);   // vertical trunk in the column's gap
    cairo_line_to(cr, trunk_x, cy);
    cairo_stroke(cr);
    return;
  }

  double py_top = py - SK_ICON / 2.0;
  double cy_bot = cy + SK_ICON / 2.0;
  double mid = (py_top + cy_bot) / 2.0;

  cairo_move_to(cr, px, py_top);
  cairo_line_to(cr, px, mid);
  cairo_line_to(cr, cx, mid);
  cairo_line_to(cr, cx, cy_bot);
  cairo_stroke(cr);
}

// Screen Y for a mastery level on the bar (level 0 at the bottom, MAX at top).
static double
bar_level_y(double bar_bottom, double span, int level)
{
  if(level < 0) level = 0;
  if(level > MAX_MASTERY_LEVEL) level = MAX_MASTERY_LEVEL;

  return(bar_bottom - ((double)level / MAX_MASTERY_LEVEL) * span);
}

// The mastery level bar in the left gutter: a vertical track filled from the
// bottom up to the current mastery level, with a tick + point-requirement label
// for each tier (records/game/gameengine.dbr skillMasteryTierLevel, mirrored in
// tier_mastery_req).  Mirrors the in-game skill window's left-edge bar.
static void
draw_mastery_bar(cairo_t *cr, MasteryPane *mp)
{
  if(mp->num_nodes <= 0)
    return;

  // Vertical span: top of the highest skill row down to just above the mastery
  // icon.  The bar is a linear level scale (0 at the bottom, MAX at the top).
  double top_y = mp->nodes[0].y, bot_y = mp->nodes[0].y;

  for(int i = 1; i < mp->num_nodes; i++)
  {
    if(mp->nodes[i].y < top_y) top_y = mp->nodes[i].y;
    if(mp->nodes[i].y > bot_y) bot_y = mp->nodes[i].y;
  }

  double bar_top = top_y - SK_ICON / 2.0;
  double bar_bottom = mp->mastery_y - SK_ICON / 2.0 - 4.0;

  if(bar_bottom <= bar_top + 4.0)
    return;

  double span = bar_bottom - bar_top;
  double bar_l = mp->mastery_x - SK_BAR_W / 2.0;

  // Track + fill + border.
  cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
  cairo_rectangle(cr, bar_l, bar_top, SK_BAR_W, span);
  cairo_fill(cr);

  double fill_y = bar_level_y(bar_bottom, span, mp->mastery_level);

  cairo_set_source_rgb(cr, 0.85, 0.72, 0.30);
  cairo_rectangle(cr, bar_l, fill_y, SK_BAR_W, bar_bottom - fill_y);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 0.45, 0.40, 0.25);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, bar_l, bar_top, SK_BAR_W, span);
  cairo_stroke(cr);

  // Tier ticks + point-requirement labels.
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 9.0);

  for(int t = 1; t <= MAX_TIER; t++)
  {
    int lvl = tier_mastery_req[t];
    double ty = bar_level_y(bar_bottom, span, lvl);
    bool reached = mp->mastery_level >= lvl;

    cairo_set_source_rgb(cr, 0.30, 0.28, 0.20);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, bar_l, ty);
    cairo_line_to(cr, bar_l + SK_BAR_W, ty);
    cairo_stroke(cr);

    char buf[8];
    cairo_text_extents_t ext;

    snprintf(buf, sizeof(buf), "%d", lvl);
    cairo_text_extents(cr, buf, &ext);

    if(reached)
      cairo_set_source_rgb(cr, 0.92, 0.85, 0.55);
    else
      cairo_set_source_rgb(cr, 0.50, 0.50, 0.50);

    cairo_move_to(cr, bar_l - 3.0 - ext.width, ty + ext.height / 2.0);
    cairo_show_text(cr, buf);
  }
}

static void
skill_canvas_draw_cb(GtkDrawingArea *da, cairo_t *cr, int width, int height, gpointer user_data)
{
  (void)width;
  (void)height;
  (void)user_data;
  SkillsState *st = g_object_get_data(G_OBJECT(da), "state");
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(da), "pane"));
  MasteryPane *mp = &st->panes[pane];
  AppWidgets *w = st->widgets;

  cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
  cairo_paint(cr);

  if(mp->mastery_def_idx < 0)
    return;

  if(mp->num_nodes == 0 && mp->mastery_chr_skill_idx < 0)
  {
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 16, 30);
    cairo_show_text(cr, "No skill data in save -- select this mastery in-game first.");
    return;
  }

  // Mastery level bar in the left gutter (behind everything else).
  if(mp->use_db_layout)
    draw_mastery_bar(cr, mp);

  // Connection lines (under icons).
  for(int i = 0; i < mp->num_nodes; i++)
  {
    int p = mp->nodes[i].parent_idx;

    if(p < 0)
      continue;

    draw_edge(cr, mp->nodes[p].x, mp->nodes[p].y,
              mp->nodes[i].x, mp->nodes[i].y, mp->nodes[p].cur_level > 0);
  }

  // Mastery node.
  {
    bool on = mp->mastery_level > 0;
    GdkPixbuf *pb = skill_icon(w, on ? mp->mastery_up_tex : mp->mastery_down_tex);

    if(!pb)
      pb = skill_icon(w, on ? mp->mastery_down_tex : mp->mastery_up_tex);

    blit_icon(cr, pb, mp->mastery_x, mp->mastery_y, SK_ICON, 1.0);

    if(pb)
      g_object_unref(pb);

    char buf[32];

    snprintf(buf, sizeof(buf), "%d / %d", mp->mastery_level, MAX_MASTERY_LEVEL);
    draw_counter(cr, buf, mp->mastery_x, mp->mastery_y + SK_ICON / 2.0,
                 0.92, 0.92, 0.92);
  }

  // Skill nodes.
  for(int i = 0; i < mp->num_nodes; i++)
  {
    SkillNode *n = &mp->nodes[i];
    bool accessible = n->chr_skill_idx >= 0 && skill_is_accessible(mp, i);
    bool on = n->cur_level > 0;
    GdkPixbuf *pb = skill_icon(w, on ? n->up_tex : n->down_tex);

    if(!pb)
      pb = skill_icon(w, n->up_tex);

    blit_icon(cr, pb, n->x, n->y, SK_ICON, accessible ? 1.0 : 0.4);

    if(pb)
      g_object_unref(pb);

    // In-game style: show the skill's effective level as a single number
    // (allocated points plus applicable equipment bonuses, applied only with a
    // point spent and clamped to the ultimate cap).  Colour it green only when
    // equipment pushed it above the skill's natural cap (skillMaxLevel);
    // otherwise leave it plain.
    int raw_bonus = n->gear_all + n->gear_mastery + n->gear_skill;
    int eff = effective_level(n->cur_level, raw_bonus, n->ultimate_level);
    double counter_y = n->y + SK_ICON / 2.0;
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", eff);

    if(eff > n->max_level)
      draw_counter(cr, buf, n->x, counter_y, 0.42, 0.95, 0.42);
    else
      draw_counter(cr, buf, n->x, counter_y, 0.92, 0.92, 0.92);
  }
}

// ── Hit testing + interaction ────────────────────────────────────────────

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

  if(character_save_skills(chr) == 0)
    update_ui(st->widgets, chr);
  else
    fprintf(stderr, "Skills: failed to save\n");

  st->widgets->char_dirty = true;
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
  st->work_levels = malloc((size_t)chr->num_skills * sizeof(uint32_t));

  if(!st->work_levels)
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

// ── Headless verification ─────────────────────────────────────────────────

void
skills_debug_print_gear_bonuses(TQCharacter *chr)
{
  if(!chr)
  {
    printf("skill-bonuses: no character\n");
    return;
  }

  GearBonuses g;

  compute_gear_bonuses(chr, &g);

  printf("\n--- Equipment +skill bonuses: %s ---\n",
         chr->character_name ? chr->character_name : "(unknown)");
  printf("All skills: +%d\n", g.all_bonus);

  for(int m = 0; m < NUM_MASTERIES; m++)
    if(g.mastery_bonus[m] > 0)
      printf("Mastery %-9s: +%d\n", mastery_defs[m].name, g.mastery_bonus[m]);

  if(g.num_skill > 0)
  {
    printf("Skill-specific:\n");

    for(int i = 0; i < g.num_skill; i++)
      printf("  +%d  %s\n", g.skill[i].bonus, g.skill[i].path);
  }

  // Effective levels for every allocated skill that any bonus touches.
  printf("Effective levels (allocated skills receiving a bonus):\n");

  for(int i = 0; i < chr->num_skills; i++)
  {
    const char *path = chr->skills[i].skill_name;
    int level = (int)chr->skills[i].skill_level;

    if(!path || !path[0] || level < 1 || is_mastery_record(path))
      continue;

    // Quest-reward stat buffs are pseudo-skills that never appear in the tree
    // and don't receive +skill bonuses in game; skip them in this report.
    if(strcasestr(path, "\\quests\\rewards\\"))
      continue;

    int mdef = find_mastery_for_skill(path);
    int mb = (mdef >= 0 && mdef < NUM_MASTERIES) ? g.mastery_bonus[mdef] : 0;

    char norm[256];

    normalize_path(path, norm, sizeof(norm));
    int sb = gear_skill_bonus(&g, norm);
    int raw = g.all_bonus + mb + sb;

    if(raw <= 0)
      continue;

    int tier, maxl, ult;

    get_skill_dbr_info(path, &tier, &maxl, &ult);
    int eff = effective_level(level, raw, ult);

    printf("  %-56s %2d -> %2d  (+%d, cap %d)\n", norm, level, eff, eff - level, ult);
  }
}
