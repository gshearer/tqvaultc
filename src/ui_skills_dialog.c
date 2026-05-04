// ui_skills_dialog.c -- Skills dialog for managing mastery and skill point allocation
//
// Two panes (one per mastery). Each pane shows mastery level +/-, child skills
// with +/- buttons, tier-based unlocking. Mastery dropdown allows switching.
// Changes are applied to the save file only on "Apply".

#include "ui.h"
#include "arz.h"
#include "asset_lookup.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

// ── Constants ───────────────────────────────────────────────────────────

#define MAX_MASTERY_LEVEL 40
#define MAX_SKILLS_PER_MASTERY 64
#define MAX_TREE_ENTRIES 32

// Tier -> minimum mastery level thresholds (standard TQ values)
static const int tier_mastery_req[] = {
  [0] = 0,
  [1] = 1,
  [2] = 4,
  [3] = 8,
  [4] = 16,
  [5] = 24,
  [6] = 32,
  [7] = 40,
};
#define MAX_TIER 7

// ── Mastery definitions ─────────────────────────────────────────────────

typedef struct {
  const char *name;       // display name
  const char *dbr_path;   // DBR record path
  const char *dir_prefix; // directory prefix for child skill matching
  const char *tree_path;  // skill tree DBR path
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

// XPack3 AllMasteries skill prefix mapping
static const struct { const char *prefix; int mastery_idx; } allmastery_map[] = {
  { "defense_",  0 },
  { "earth_",    1 },
  { "hunting_",  2 },
  { "nature_",   3 },
  { "spirit_",   4 },
  { "storm_",    5 },
  { "warfare_",  6 },
  { "dream_",    7 },
  { "rune_",     8 },
  { "stealth_",  9 },
  { "neidan_",  10 },
};
#define NUM_ALLMASTERY_MAP 11

// ── Skill tree entry (loaded from DBR) ──────────────────────────────────

typedef struct {
  char path[256];     // normalized lowercase path with backslashes
  char basename[128]; // filename without extension, lowercase
  int parent_idx;     // index into tree_entries[], -1 if root
} TreeEntry;

// ── Skill UI entry ──────────────────────────────────────────────────────

typedef struct {
  int chr_skill_idx;      // index into TQCharacter.skills[]
  char skill_path[256];   // normalized path for tree lookup
  char display_name[128]; // resolved display name
  int skill_tier;         // skillTier from DBR (1-7), 0 if unknown
  int max_level;          // skillMaxLevel from DBR
  int cur_level;          // working copy of skill level
  int parent_idx;         // index into pane skills[], -1 if no parent
} SkillEntry;

// ── Per-mastery pane state ──────────────────────────────────────────────

typedef struct {
  int mastery_def_idx;           // index into mastery_defs[], -1 = none
  int initial_mastery_def_idx;   // value at dialog open; for change detection
  int mastery_chr_skill_idx;     // index into TQCharacter.skills[] for mastery itself
  int mastery_level;             // working copy of mastery level

  SkillEntry skills[MAX_SKILLS_PER_MASTERY];
  int num_skills;

  // Widgets
  GtkWidget *frame;
  GtkWidget *mastery_dropdown;
  GtkWidget *mastery_label;
  GtkWidget *mastery_val_label;
  GtkWidget *mastery_plus_btn;
  GtkWidget *mastery_minus_btn;
  GtkWidget *skills_box;         // vbox containing skill rows

  // Per-skill widgets
  GtkWidget *skill_rows[MAX_SKILLS_PER_MASTERY];
  GtkWidget *skill_val_labels[MAX_SKILLS_PER_MASTERY];
  GtkWidget *skill_plus_btns[MAX_SKILLS_PER_MASTERY];
  GtkWidget *skill_minus_btns[MAX_SKILLS_PER_MASTERY];
} MasteryPane;

// ── Dialog state ────────────────────────────────────────────────────────

typedef struct {
  AppWidgets *widgets;
  GtkWidget *dialog;
  GtkWidget *avail_label;
  GtkWidget *apply_btn;

  int total_skill_points;  // immutable total (available + all spent)
  int orig_skill_points;   // original available for detecting changes

  // Working copies of all skill levels (indexed by chr skill idx)
  uint32_t *work_levels;   // [num_skills]
  int num_chr_skills;

  bool building;           // suppress dropdown signals during construction
  MasteryPane panes[2];
} SkillsDialogState;

// ── Forward declarations ────────────────────────────────────────────────

static void refresh_display(SkillsDialogState *st);
static void populate_mastery_pane(SkillsDialogState *st, int pane_idx);

// ── Helpers ─────────────────────────────────────────────────────────────

// Compute the number of available (unspent) skill points.
//
// st: dialog state
// returns: available points
static int
compute_avail(SkillsDialogState *st)
{
  int spent = 0;

  for(int i = 0; i < st->num_chr_skills; i++)
    spent += (int)st->work_levels[i];

  return(st->total_skill_points - spent);
}

// Check if any skill levels differ from the original character data.
//
// st: dialog state
// returns: true if there are unsaved changes
static bool
has_changes(SkillsDialogState *st)
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

// Find which mastery_defs[] index a skill path belongs to.
//
// skill_path: normalized skill path to look up
// returns: mastery index, or -1 if none
static int
find_mastery_for_skill(const char *skill_path)
{
  if(!skill_path)
    return(-1);

  // Check main mastery directories
  for(int m = 0; m < NUM_MASTERIES; m++)
  {
    if(strncasecmp(skill_path, mastery_defs[m].dir_prefix,
                   strlen(mastery_defs[m].dir_prefix)) == 0)
      return(m);
  }

  // Check XPack3 AllMasteries: e.g. "records\xpack3\skills\allmasteries\nature_earthbind.dbr"
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

// Check if a skill path is a mastery record (not a child skill).
//
// path: skill path to check
// returns: true if path contains "mastery.dbr"
static bool
is_mastery_record(const char *path)
{
  if(!path)
    return(false);

  return(strcasestr(path, "mastery.dbr") != NULL);
}

// Normalize a path to lowercase with backslashes for comparison.
//
// src: source path string
// dst: destination buffer
// dst_size: size of destination buffer
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

// Extract lowercase basename (filename without extension) from a path.
//
// path: source path
// out: destination buffer
// out_size: size of destination buffer
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

// Try to resolve a display name tag from a DBR.
//
// widgets: application widget state for translation context
// dbr: the DBR record data to read from
// out: destination buffer for the resolved name
// out_size: size of destination buffer
// returns: true on success
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

  // tag itself might be a readable name (some records use raw text)
  if(tag[0] && strncmp(tag, "tag", 3) != 0)
  {
    snprintf(out, out_size, "%s", tag);
    free(tag);
    return(true);
  }

  free(tag);
  return(false);
}

// Resolve display name for a skill from database + translation.
// Buff/toggle skills store their display name in the referenced buffSkillName record.
//
// widgets: application widget state for translation context
// skill_path: DBR path to the skill
// out: destination buffer for the resolved name
// out_size: size of destination buffer
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

    // Follow buffSkillName / petSkillName for display name
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

  // Fallback: extract name from path
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

// Try to get tier + maxLevel from a referenced skill record.
//
// ref_field: name of the DBR field containing the reference path
// dbr: the parent DBR record data
// skill_tier: output pointer for the skill tier
// max_level: output pointer for the max level
// returns: true if skillTier was found
static bool
get_tier_from_ref(const char *ref_field, TQArzRecordData *dbr,
                  int *skill_tier, int *max_level)
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
    int bm = arz_record_get_int(ref_dbr, "skillMaxLevel", 0, NULL);

    if(bm > 0)
      *max_level = bm;

    return(true);
  }

  return(false);
}

// Get skillTier and skillMaxLevel from database.
// Buff/toggle skills store tier in buffSkillName, pet modifiers in petSkillName.
//
// skill_path: DBR path to the skill
// skill_tier: output pointer for the skill tier
// max_level: output pointer for the max level
static void
get_skill_dbr_info(const char *skill_path, int *skill_tier, int *max_level)
{
  *skill_tier = 0;
  *max_level = 1;

  TQArzRecordData *dbr = asset_get_dbr(skill_path);

  if(!dbr)
    return;

  *skill_tier = arz_record_get_int(dbr, "skillTier", 0, NULL);
  *max_level = arz_record_get_int(dbr, "skillMaxLevel", 1, NULL);

  if(*skill_tier == 0)
  {
    if(!get_tier_from_ref("buffSkillName", dbr, skill_tier, max_level))
      get_tier_from_ref("petSkillName", dbr, skill_tier, max_level);
  }
}

// ── Skill tree loading ──────────────────────────────────────────────────

// Load the skill tree DBR and build tree entries with parent relationships.
//
// tree_dbr_path: DBR path to the skill tree record
// entries: output array of TreeEntry
// max_entries: maximum number of entries to load
// returns: number of entries loaded
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

    // Skip the mastery entry itself
    if(strcasestr(te->basename, "mastery") != NULL)
      continue;

    // Determine parent: look backward for a tree entry whose basename is a
    // prefix of this entry's basename, or if this is a modifier/secondary
    // (indicated by the parent's basename appearing in ours), chain to it.
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

    // If no prefix match found, check if this is a modifier/secondary type
    // that should chain to the immediately preceding entry
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

// ── Dependency helpers ──────────────────────────────────────────────────

// Check if a skill is accessible: tier unlocked AND parent (if any) has points.
//
// mp: mastery pane state
// skill_idx: index into the pane's skills array
// returns: true if the skill can be modified
static bool
skill_is_accessible(MasteryPane *mp, int skill_idx)
{
  SkillEntry *se = &mp->skills[skill_idx];

  // Check tier unlock
  int tier = se->skill_tier;

  if(tier >= 1 && tier <= MAX_TIER)
  {
    if(mp->mastery_level < tier_mastery_req[tier])
      return(false);
  }

  // Check parent dependency
  if(se->parent_idx >= 0 && se->parent_idx < mp->num_skills)
  {
    if(mp->skills[se->parent_idx].cur_level <= 0)
      return(false);

    // Recursively check parent chain
    if(!skill_is_accessible(mp, se->parent_idx))
      return(false);
  }

  return(true);
}

// Zero out a skill and all skills that depend on it.
//
// st: dialog state
// mp: mastery pane state
// skill_idx: index of the skill to zero
static void
zero_skill_and_dependents(SkillsDialogState *st, MasteryPane *mp, int skill_idx)
{
  SkillEntry *se = &mp->skills[skill_idx];

  if(se->cur_level > 0)
  {
    se->cur_level = 0;

    if(se->chr_skill_idx >= 0)
      st->work_levels[se->chr_skill_idx] = 0;
  }

  // Find and zero any skills that depend on this one
  for(int i = 0; i < mp->num_skills; i++)
  {
    if(mp->skills[i].parent_idx == skill_idx && mp->skills[i].cur_level > 0)
      zero_skill_and_dependents(st, mp, i);
  }
}

// ── Skill row +/- callbacks ─────────────────────────────────────────────

// Increment a skill's level by one point.
//
// btn: the plus button
// user_data: SkillsDialogState pointer
static void
on_skill_plus(GtkButton *btn, gpointer user_data)
{
  SkillsDialogState *st = user_data;
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pane"));
  int sidx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "sidx"));
  MasteryPane *mp = &st->panes[pane];

  if(sidx < 0 || sidx >= mp->num_skills)
    return;

  SkillEntry *se = &mp->skills[sidx];

  if(se->chr_skill_idx < 0)
    return; // not in save

  if(se->cur_level >= se->max_level)
    return;

  if(compute_avail(st) <= 0)
    return;

  se->cur_level++;
  st->work_levels[se->chr_skill_idx] = (uint32_t)se->cur_level;
  refresh_display(st);
}

// Decrement a skill's level by one point, zeroing dependents if needed.
//
// btn: the minus button
// user_data: SkillsDialogState pointer
static void
on_skill_minus(GtkButton *btn, gpointer user_data)
{
  SkillsDialogState *st = user_data;
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pane"));
  int sidx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "sidx"));
  MasteryPane *mp = &st->panes[pane];

  if(sidx < 0 || sidx >= mp->num_skills)
    return;

  SkillEntry *se = &mp->skills[sidx];

  if(se->chr_skill_idx < 0)
    return; // not in save

  if(se->cur_level <= 0)
    return;

  se->cur_level--;
  st->work_levels[se->chr_skill_idx] = (uint32_t)se->cur_level;

  // If skill went to 0, zero out all dependents
  if(se->cur_level == 0)
  {
    for(int i = 0; i < mp->num_skills; i++)
    {
      if(mp->skills[i].parent_idx == sidx && mp->skills[i].cur_level > 0)
        zero_skill_and_dependents(st, mp, i);
    }
  }

  refresh_display(st);
}

// ── Mastery level +/- callbacks ─────────────────────────────────────────

// Increment mastery level by one point.
//
// btn: the plus button
// user_data: SkillsDialogState pointer
static void
on_mastery_plus(GtkButton *btn, gpointer user_data)
{
  SkillsDialogState *st = user_data;
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pane"));
  MasteryPane *mp = &st->panes[pane];

  if(mp->mastery_level >= MAX_MASTERY_LEVEL)
    return;

  if(compute_avail(st) <= 0)
    return;

  mp->mastery_level++;

  if(mp->mastery_chr_skill_idx >= 0)
    st->work_levels[mp->mastery_chr_skill_idx] = (uint32_t)mp->mastery_level;

  refresh_display(st);
}

// Decrement mastery level by one point, zeroing inaccessible skills.
//
// btn: the minus button
// user_data: SkillsDialogState pointer
static void
on_mastery_minus(GtkButton *btn, gpointer user_data)
{
  SkillsDialogState *st = user_data;
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pane"));
  MasteryPane *mp = &st->panes[pane];

  if(mp->mastery_level <= 0)
    return;

  mp->mastery_level--;

  if(mp->mastery_chr_skill_idx >= 0)
    st->work_levels[mp->mastery_chr_skill_idx] = (uint32_t)mp->mastery_level;

  // Zero out any skills whose tier is no longer accessible
  for(int i = 0; i < mp->num_skills; i++)
  {
    SkillEntry *se = &mp->skills[i];
    int tier = se->skill_tier;

    if(tier >= 1 && tier <= MAX_TIER &&
       mp->mastery_level < tier_mastery_req[tier] && se->cur_level > 0)
      zero_skill_and_dependents(st, mp, i);
  }

  refresh_display(st);
}

// ── Mastery dropdown change ─────────────────────────────────────────────

// Handle mastery dropdown selection change.
//
// dd: the GtkDropDown widget
// pspec: property spec (unused)
// user_data: SkillsDialogState pointer
static void
on_mastery_changed(GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  SkillsDialogState *st = user_data;

  if(st->building)
    return;

  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dd), "pane"));
  MasteryPane *mp = &st->panes[pane];

  guint sel = gtk_drop_down_get_selected(dd);

  if(sel == GTK_INVALID_LIST_POSITION)
    return;

  GtkStringObject *obj = gtk_drop_down_get_selected_item(dd);

  if(!obj)
    return;

  const char *name = gtk_string_object_get_string(obj);

  // Find matching mastery def
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

  // Check: don't allow selecting the same mastery as the other pane
  int other = 1 - pane;

  if(st->panes[other].mastery_def_idx == new_def)
    return;

  // Zero out every chr->skills entry that belongs to the old mastery —
  // refund all spent points.  Walk the full skill list rather than the
  // currently-loaded tree so out-of-tree or expansion-shifted skills
  // don't leak through.
  TQCharacter *chr = st->widgets->current_character;
  int old_def = mp->mastery_def_idx;

  if(old_def >= 0 && chr)
  {
    for(int i = 0; i < chr->num_skills; i++)
    {
      const char *path = chr->skills[i].skill_name;

      if(!path || !path[0])
        continue;

      if(find_mastery_for_skill(path) == old_def)
        st->work_levels[i] = 0;
    }
  }

  mp->mastery_level = 0;
  mp->mastery_def_idx = new_def;

  // Rebuild the skills list for new mastery
  populate_mastery_pane(st, pane);
  refresh_display(st);
}

// ── Refresh all UI elements ─────────────────────────────────────────────

// Update all labels, buttons, and sensitivity states in the dialog.
//
// st: dialog state
static void
refresh_display(SkillsDialogState *st)
{
  int avail = compute_avail(st);
  char buf[128];

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

    // Mastery level display
    bool has_mastery_block = mp->mastery_chr_skill_idx >= 0;

    snprintf(buf, sizeof(buf), "%d / %d", mp->mastery_level, MAX_MASTERY_LEVEL);
    gtk_label_set_text(GTK_LABEL(mp->mastery_val_label), buf);
    gtk_widget_set_sensitive(mp->mastery_minus_btn,
                             has_mastery_block && mp->mastery_level > 0);
    gtk_widget_set_sensitive(mp->mastery_plus_btn,
                             has_mastery_block &&
                             mp->mastery_level < MAX_MASTERY_LEVEL && avail > 0);

    // Skill rows
    for(int i = 0; i < mp->num_skills; i++)
    {
      SkillEntry *se = &mp->skills[i];
      bool in_save = se->chr_skill_idx >= 0;
      bool accessible = in_save && skill_is_accessible(mp, i);

      snprintf(buf, sizeof(buf), "%d / %d", se->cur_level, se->max_level);
      gtk_label_set_text(GTK_LABEL(mp->skill_val_labels[i]), buf);

      gtk_widget_set_sensitive(mp->skill_minus_btns[i],
                               accessible && se->cur_level > 0);
      gtk_widget_set_sensitive(mp->skill_plus_btns[i],
                               accessible && se->cur_level < se->max_level
                               && avail > 0);

      // Dim locked/unavailable skills
      gtk_widget_set_sensitive(mp->skill_rows[i], accessible);
    }
  }
}

// ── Apply / Cancel ──────────────────────────────────────────────────────

// Apply skill changes to the character and close the dialog.
//
// btn: the Apply button (unused)
// user_data: SkillsDialogState pointer
static void
on_apply_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SkillsDialogState *st = user_data;
  TQCharacter *chr = st->widgets->current_character;

  if(!chr)
    return;

  // Copy working levels back to character
  for(int i = 0; i < st->num_chr_skills; i++)
    chr->skills[i].skill_level = st->work_levels[i];

  chr->skill_points = (uint32_t)compute_avail(st);

  // Mark character dirty so "Save Character" stays available even when
  // the only change is a mastery swap (no inventory/equipment edits).
  st->widgets->char_dirty = true;

  if(character_save_skills(chr) == 0)
    update_ui(st->widgets, chr);
  else
    fprintf(stderr, "Skills: failed to save\n");

  gtk_window_close(GTK_WINDOW(st->dialog));
}

// Cancel and close the dialog without applying changes.
//
// btn: the Cancel button (unused)
// user_data: SkillsDialogState pointer
static void
on_cancel_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SkillsDialogState *st = user_data;

  gtk_window_close(GTK_WINDOW(st->dialog));
}

// Free the dialog state when the dialog is destroyed.
//
// data: SkillsDialogState pointer
static void
skills_state_free(gpointer data)
{
  SkillsDialogState *st = data;

  free(st->work_levels);
  g_free(st);
}

// Sorting is done inline in populate_mastery_pane via index arrays

// ── Build skill row widget ──────────────────────────────────────────────

// Create a horizontal box widget for a single skill row with tier, name,
// +/- buttons, and value label.
//
// st: dialog state
// pane: pane index (0 or 1)
// sidx: skill index within the pane
// returns: the row widget
static GtkWidget *
make_skill_row(SkillsDialogState *st, int pane, int sidx)
{
  SkillEntry *se = &st->panes[pane].skills[sidx];
  MasteryPane *mp = &st->panes[pane];
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  // Tier indicator
  char tier_buf[16];

  if(se->skill_tier >= 1)
    snprintf(tier_buf, sizeof(tier_buf), "T%d", se->skill_tier);
  else
    snprintf(tier_buf, sizeof(tier_buf), " ");

  GtkWidget *tier_label = gtk_label_new(tier_buf);

  gtk_label_set_xalign(GTK_LABEL(tier_label), 1.0);
  gtk_widget_set_size_request(tier_label, 30, -1);
  PangoAttrList *tier_attrs = pango_attr_list_new();

  pango_attr_list_insert(tier_attrs, pango_attr_scale_new(0.8));
  gtk_label_set_attributes(GTK_LABEL(tier_label), tier_attrs);
  pango_attr_list_unref(tier_attrs);
  gtk_box_append(GTK_BOX(row), tier_label);

  // Skill name -- indent sub-skills
  char name_buf[160];

  if(se->parent_idx >= 0)
    snprintf(name_buf, sizeof(name_buf), "  %s", se->display_name);
  else
    snprintf(name_buf, sizeof(name_buf), "%s", se->display_name);

  GtkWidget *name_label = gtk_label_new(name_buf);

  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(name_label, TRUE);
  gtk_box_append(GTK_BOX(row), name_label);

  // Minus button
  GtkWidget *minus_btn = gtk_button_new_with_label("\u2212");

  g_object_set_data(G_OBJECT(minus_btn), "pane", GINT_TO_POINTER(pane));
  g_object_set_data(G_OBJECT(minus_btn), "sidx", GINT_TO_POINTER(sidx));
  g_signal_connect(minus_btn, "clicked", G_CALLBACK(on_skill_minus), st);
  gtk_box_append(GTK_BOX(row), minus_btn);

  // Value label
  char val_buf[32];

  snprintf(val_buf, sizeof(val_buf), "%d / %d", se->cur_level, se->max_level);
  GtkWidget *val_label = gtk_label_new(val_buf);

  gtk_label_set_xalign(GTK_LABEL(val_label), 0.5);
  gtk_widget_set_size_request(val_label, 55, -1);
  gtk_box_append(GTK_BOX(row), val_label);

  // Plus button
  GtkWidget *plus_btn = gtk_button_new_with_label("+");

  g_object_set_data(G_OBJECT(plus_btn), "pane", GINT_TO_POINTER(pane));
  g_object_set_data(G_OBJECT(plus_btn), "sidx", GINT_TO_POINTER(sidx));
  g_signal_connect(plus_btn, "clicked", G_CALLBACK(on_skill_plus), st);
  gtk_box_append(GTK_BOX(row), plus_btn);

  mp->skill_rows[sidx] = row;
  mp->skill_val_labels[sidx] = val_label;
  mp->skill_minus_btns[sidx] = minus_btn;
  mp->skill_plus_btns[sidx] = plus_btn;

  return(row);
}

// ── Populate a mastery pane with ALL skills from the skill tree ─────────

// Load the skill tree and populate the pane with skill entries and widgets.
//
// st: dialog state
// pane_idx: pane index (0 or 1)
static void
populate_mastery_pane(SkillsDialogState *st, int pane_idx)
{
  MasteryPane *mp = &st->panes[pane_idx];
  TQCharacter *chr = st->widgets->current_character;

  // Clear existing skill widgets
  if(mp->skills_box)
  {
    GtkWidget *child;

    while((child = gtk_widget_get_first_child(mp->skills_box)))
      gtk_box_remove(GTK_BOX(mp->skills_box), child);
  }

  mp->num_skills = 0;
  mp->mastery_chr_skill_idx = -1;

  if(mp->mastery_def_idx < 0)
    return;

  const MasteryDef *mdef = &mastery_defs[mp->mastery_def_idx];

  // Find the mastery record in the character save
  for(int i = 0; i < chr->num_skills; i++)
  {
    const char *path = chr->skills[i].skill_name;

    if(!path || !path[0])
      continue;

    int skill_mastery = find_mastery_for_skill(path);

    if(skill_mastery != mp->mastery_def_idx)
      continue;

    if(is_mastery_record(path))
    {
      mp->mastery_chr_skill_idx = i;
      mp->mastery_level = (int)st->work_levels[i];
      break;
    }
  }

  // Load the skill tree for this mastery
  TreeEntry tree_entries[MAX_TREE_ENTRIES];
  int num_tree = load_skill_tree(mdef->tree_path, tree_entries, MAX_TREE_ENTRIES);

  // Build skills from the tree -- one SkillEntry per tree entry
  for(int t = 0; t < num_tree && mp->num_skills < MAX_SKILLS_PER_MASTERY; t++)
  {
    SkillEntry *se = &mp->skills[mp->num_skills];

    memset(se, 0, sizeof(*se));
    se->chr_skill_idx = -1; // default: not in save
    se->parent_idx = -1;
    strncpy(se->skill_path, tree_entries[t].path, sizeof(se->skill_path) - 1);

    // Resolve display name and DBR info
    resolve_skill_name(st->widgets, tree_entries[t].path,
                       se->display_name, sizeof(se->display_name));
    get_skill_dbr_info(tree_entries[t].path, &se->skill_tier, &se->max_level);

    // Find this skill in the character save
    for(int i = 0; i < chr->num_skills; i++)
    {
      if(!chr->skills[i].skill_name)
        continue;

      char norm[256];

      normalize_path(chr->skills[i].skill_name, norm, sizeof(norm));

      if(strcasecmp(norm, se->skill_path) == 0)
      {
        se->chr_skill_idx = i;
        se->cur_level = (int)st->work_levels[i];
        break;
      }
    }

    // Store tree parent index -- resolve to pane index after all entries built
    if(tree_entries[t].parent_idx >= 0)
      se->parent_idx = -(tree_entries[t].parent_idx + 100);

    mp->num_skills++;
  }

  // Resolve parent indices: tree entry index -> pane skill index.
  // Tree entries and pane skills are 1:1 in the same order at this point.
  for(int i = 0; i < mp->num_skills; i++)
  {
    if(mp->skills[i].parent_idx >= -99)
      continue;

    int tree_parent_idx = -(mp->skills[i].parent_idx + 100);

    mp->skills[i].parent_idx = -1;

    if(tree_parent_idx >= 0 && tree_parent_idx < mp->num_skills)
      mp->skills[i].parent_idx = tree_parent_idx;
  }

  // Build display order: root skills sorted by tier (highest first),
  // with each root's children listed underneath in tree order.
  int display_order[MAX_SKILLS_PER_MASTERY];
  bool used[MAX_SKILLS_PER_MASTERY];

  memset(used, 0, sizeof(used));
  int num_display = 0;

  // Collect root skills (no parent) and sort by tier descending, then by name
  int roots[MAX_SKILLS_PER_MASTERY];
  int num_roots = 0;

  for(int i = 0; i < mp->num_skills; i++)
  {
    if(mp->skills[i].parent_idx < 0)
      roots[num_roots++] = i;
  }

  // Sort roots by tier descending, then alphabetically
  for(int i = 0; i < num_roots - 1; i++)
  {
    for(int j = i + 1; j < num_roots; j++)
    {
      SkillEntry *a = &mp->skills[roots[i]];
      SkillEntry *b = &mp->skills[roots[j]];
      bool swap = false;

      if(b->skill_tier > a->skill_tier)
        swap = true;
      else if(b->skill_tier == a->skill_tier &&
              strcmp(a->display_name, b->display_name) > 0)
        swap = true;

      if(swap)
      {
        int tmp = roots[i];

        roots[i] = roots[j];
        roots[j] = tmp;
      }
    }
  }

  // For each root, emit root then children in tree order
  for(int r = 0; r < num_roots; r++)
  {
    int ri = roots[r];

    display_order[num_display++] = ri;
    used[ri] = true;

    // Collect all descendants (direct + transitive) in tree order
    for(int i = 0; i < mp->num_skills; i++)
    {
      if(used[i])
        continue;

      // Check if i is a descendant of ri
      int p = mp->skills[i].parent_idx;

      while(p >= 0 && p != ri)
        p = mp->skills[p].parent_idx;

      if(p == ri)
      {
        display_order[num_display++] = i;
        used[i] = true;
      }
    }
  }

  // Any remaining (shouldn't happen, but safety)
  for(int i = 0; i < mp->num_skills; i++)
  {
    if(!used[i])
      display_order[num_display++] = i;
  }

  // Rearrange skills[] array to match display order and fix parent indices
  SkillEntry temp[MAX_SKILLS_PER_MASTERY];
  int old_to_new[MAX_SKILLS_PER_MASTERY];

  for(int i = 0; i < mp->num_skills; i++)
  {
    temp[i] = mp->skills[display_order[i]];
    old_to_new[display_order[i]] = i;
  }

  for(int i = 0; i < mp->num_skills; i++)
  {
    mp->skills[i] = temp[i];

    if(mp->skills[i].parent_idx >= 0)
      mp->skills[i].parent_idx = old_to_new[mp->skills[i].parent_idx];
  }

  // Create skill row widgets
  if(mp->skills_box)
  {
    if(mp->num_skills == 0 && mp->mastery_chr_skill_idx < 0)
    {
      GtkWidget *note = gtk_label_new(
        "No skill data in save.\n"
        "Select this mastery in-game first.");

      gtk_label_set_xalign(GTK_LABEL(note), 0.5);
      gtk_widget_set_margin_top(note, 20);
      gtk_box_append(GTK_BOX(mp->skills_box), note);
    }

    else
    {
      for(int i = 0; i < mp->num_skills; i++)
      {
        GtkWidget *row = make_skill_row(st, pane_idx, i);

        gtk_box_append(GTK_BOX(mp->skills_box), row);
      }
    }
  }
}

// ── Build a mastery pane widget ─────────────────────────────────────────

// Create the widget tree for a single mastery pane (dropdown, mastery level,
// scrollable skill list).
//
// st: dialog state
// pane_idx: pane index (0 or 1)
// returns: the frame widget containing the pane
static GtkWidget *
build_mastery_pane(SkillsDialogState *st, int pane_idx)
{
  MasteryPane *mp = &st->panes[pane_idx];
  int other_pane = 1 - pane_idx;
  GtkWidget *frame = gtk_frame_new(NULL);

  mp->frame = frame;
  gtk_widget_set_vexpand(frame, TRUE);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);
  gtk_frame_set_child(GTK_FRAME(frame), vbox);

  // Mastery dropdown
  GtkStringList *model = gtk_string_list_new(NULL);
  int sel_idx = 0, model_idx = 0;

  for(int m = 0; m < NUM_MASTERIES; m++)
  {
    // Skip mastery used by the other pane
    if(st->panes[other_pane].mastery_def_idx == m)
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
  mp->mastery_dropdown = dd;
  gtk_box_append(GTK_BOX(vbox), dd);

  // Mastery level row
  GtkWidget *mrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  mp->mastery_label = gtk_label_new("Mastery Level:");
  gtk_label_set_xalign(GTK_LABEL(mp->mastery_label), 0.0);
  gtk_widget_set_hexpand(mp->mastery_label, TRUE);
  gtk_box_append(GTK_BOX(mrow), mp->mastery_label);

  mp->mastery_minus_btn = gtk_button_new_with_label("\u2212");
  g_object_set_data(G_OBJECT(mp->mastery_minus_btn), "pane", GINT_TO_POINTER(pane_idx));
  g_signal_connect(mp->mastery_minus_btn, "clicked", G_CALLBACK(on_mastery_minus), st);
  gtk_box_append(GTK_BOX(mrow), mp->mastery_minus_btn);

  mp->mastery_val_label = gtk_label_new("0 / 40");
  gtk_label_set_xalign(GTK_LABEL(mp->mastery_val_label), 0.5);
  gtk_widget_set_size_request(mp->mastery_val_label, 55, -1);
  gtk_box_append(GTK_BOX(mrow), mp->mastery_val_label);

  mp->mastery_plus_btn = gtk_button_new_with_label("+");
  g_object_set_data(G_OBJECT(mp->mastery_plus_btn), "pane", GINT_TO_POINTER(pane_idx));
  g_signal_connect(mp->mastery_plus_btn, "clicked", G_CALLBACK(on_mastery_plus), st);
  gtk_box_append(GTK_BOX(mrow), mp->mastery_plus_btn);

  gtk_box_append(GTK_BOX(vbox), mrow);

  // Separator
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  // Scrolled skill list
  GtkWidget *scroll = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 200);

  mp->skills_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), mp->skills_box);
  gtk_box_append(GTK_BOX(vbox), scroll);

  return(frame);
}

// ── Public entry point ──────────────────────────────────────────────────

// Show the skills management dialog for the current character.
//
// widgets: application widget state
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

  SkillsDialogState *st = g_new0(SkillsDialogState, 1);

  st->widgets = widgets;
  st->num_chr_skills = chr->num_skills;

  // Create working copies of skill levels
  st->work_levels = malloc((size_t)chr->num_skills * sizeof(uint32_t));

  if(!st->work_levels)
  {
    g_free(st);
    return;
  }

  for(int i = 0; i < chr->num_skills; i++)
    st->work_levels[i] = chr->skills[i].skill_level;

  // Compute total skill points (available + all spent)
  int spent = 0;

  for(int i = 0; i < chr->num_skills; i++)
    spent += (int)chr->skills[i].skill_level;

  st->total_skill_points = (int)chr->skill_points + spent;
  st->orig_skill_points = (int)chr->skill_points;

  // Identify which masteries the character has
  for(int p = 0; p < 2; p++)
  {
    st->panes[p].mastery_def_idx = -1;
    st->panes[p].initial_mastery_def_idx = -1;
  }

  for(int i = 0; i < chr->num_skills; i++)
  {
    if(!is_mastery_record(chr->skills[i].skill_name))
      continue;

    int mdef = find_mastery_for_skill(chr->skills[i].skill_name);

    if(mdef < 0)
      continue;

    if(st->panes[0].mastery_def_idx < 0)
    {
      st->panes[0].mastery_def_idx = mdef;
      st->panes[0].initial_mastery_def_idx = mdef;
      st->panes[0].mastery_chr_skill_idx = i;
      st->panes[0].mastery_level = (int)st->work_levels[i];
    }

    else if(st->panes[1].mastery_def_idx < 0)
    {
      st->panes[1].mastery_def_idx = mdef;
      st->panes[1].initial_mastery_def_idx = mdef;
      st->panes[1].mastery_chr_skill_idx = i;
      st->panes[1].mastery_level = (int)st->work_levels[i];
    }
  }

  // Build dialog
  GtkWidget *dialog = gtk_window_new();

  st->dialog = dialog;

  char title[256];

  snprintf(title, sizeof(title), "Skills \u2014 %s",
           chr->character_name ? chr->character_name : "Character");
  gtk_window_set_title(GTK_WINDOW(dialog), title);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(widgets->main_window));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 550);

  g_object_set_data_full(G_OBJECT(dialog), "skills-state", st, skills_state_free);

  GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

  gtk_widget_set_margin_start(main_vbox, 12);
  gtk_widget_set_margin_end(main_vbox, 12);
  gtk_widget_set_margin_top(main_vbox, 8);
  gtk_widget_set_margin_bottom(main_vbox, 8);
  gtk_window_set_child(GTK_WINDOW(dialog), main_vbox);

  // Available skill points label
  st->avail_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(st->avail_label), 0.0);
  PangoAttrList *attrs = pango_attr_list_new();

  pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  pango_attr_list_insert(attrs, pango_attr_scale_new(1.1));
  gtk_label_set_attributes(GTK_LABEL(st->avail_label), attrs);
  pango_attr_list_unref(attrs);
  gtk_box_append(GTK_BOX(main_vbox), st->avail_label);

  // Two-pane horizontal layout
  GtkWidget *panes_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_vexpand(panes_box, TRUE);
  gtk_box_append(GTK_BOX(main_vbox), panes_box);

  st->building = true;

  for(int p = 0; p < 2; p++)
  {
    GtkWidget *pane = build_mastery_pane(st, p);

    gtk_widget_set_hexpand(pane, TRUE);
    gtk_box_append(GTK_BOX(panes_box), pane);

    // Populate skills
    populate_mastery_pane(st, p);
  }

  st->building = false;

  // Button bar
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

  refresh_display(st);
  gtk_window_present(GTK_WINDOW(dialog));
}
