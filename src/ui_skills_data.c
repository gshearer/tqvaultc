// Visual skill tree: skill + equipment-bonus DBR resolution.
// Split from ui_skills_tree.c; shared types in ui_skills_tree_internal.h.

#include "ui_skills_tree_internal.h"

#include "asset_lookup.h"
#include "translation.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

// XPack3 AllMasteries skill prefix -> owning mastery index.
static const struct { const char *prefix; int mastery_idx; } allmastery_map[] = {
  { "defense_", 0 }, { "earth_", 1 }, { "hunting_", 2 }, { "nature_", 3 },
  { "spirit_", 4 },  { "storm_", 5 }, { "warfare_", 6 }, { "dream_", 7 },
  { "rune_", 8 },    { "stealth_", 9 }, { "neidan_", 10 },
};
#define NUM_ALLMASTERY_MAP 11

// ── Path helpers ────────────────────────────────────────────────────────

void
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

int
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

bool
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

void
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
void
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
void
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

// Walk a skill's buffSkillName/petSkillName chain looking for the record that
// carries the icon bitmap.  Some skills are several hops deep (e.g. the Wolf
// "Strength of the Pack" pet modifier: PetModifier -> petSkillName -> PetSkill
// -> buffSkillName -> PetSkillBuff, where only the last record has a bitmap), so
// follow the refs recursively with a depth guard against cycles.
static bool
resolve_bitmaps_rec(const char *skill_path, int depth,
                    char *up, size_t up_sz, char *down, size_t down_sz)
{
  if(!skill_path || !skill_path[0] || depth > 8)
    return(false);

  TQArzRecordData *dbr = asset_get_dbr(skill_path);

  if(!dbr)
    return(false);

  read_bitmap_field(dbr, "skillUpBitmapName", up, up_sz);
  read_bitmap_field(dbr, "skillDownBitmapName", down, down_sz);

  if(up[0])
    return(true);

  static const char *ref_fields[] = { "buffSkillName", "petSkillName", NULL };

  for(int r = 0; ref_fields[r]; r++)
  {
    char *ref = arz_record_get_string(dbr, ref_fields[r], NULL);

    if(!ref)
      continue;

    bool got = resolve_bitmaps_rec(ref, depth + 1, up, up_sz, down, down_sz);

    free(ref);

    if(got)
      return(true);
  }

  return(false);
}

// Resolve a skill's up/down icon paths.  Buff/toggle and pet-modifier skills
// store no bitmap on their own record -- the icon lives in the referenced
// buffSkillName/petSkillName record (same place get_skill_dbr_info reads tier),
// which may itself only point at a deeper record, so the lookup recurses.
void
resolve_skill_bitmaps(const char *skill_path, char *up, size_t up_sz,
                      char *down, size_t down_sz)
{
  up[0] = '\0';
  down[0] = '\0';

  resolve_bitmaps_rec(skill_path, 0, up, up_sz, down, down_sz);
}

// Resolve the record that actually carries a skill's description and stats.
// Buff/toggle/aura skills (e.g. Deathchill Aura) are a thin shell that the skill
// tree points at: the description, mana cost and effect stats live in the
// referenced buffSkillName/petSkillName record.  When the main record has no
// skillBaseDescription, follow those refs to the record that does.  Falls back
// to skill_path (the common case: the skill describes itself).
void
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

int
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

  // Authoritative parent links: the game's explicit skillDependancy prerequisite
  // (note the in-game misspelling).  The basename-prefix and Class heuristics
  // above miss links where the parent's name is unrelated -- e.g. Neidan's
  // "Echoes of the Ancestors" (deathbomb) requires "Shen Pao", and "Spreading
  // Rot" requires "Consequences".  skillDependancy is the field the game itself
  // uses to draw the tree's prerequisite arrows, so prefer it whenever it
  // resolves to another skill in this same tree.  Run as a second pass so a
  // dependency that points forward (parent listed after the child) still binds.
  for(int i = 0; i < count; i++)
  {
    TQArzRecordData *sdbr = asset_get_dbr(entries[i].path);

    if(!sdbr)
      continue;

    char *dep = arz_record_get_string(sdbr, "skillDependancy", NULL);

    if(dep && dep[0])
    {
      char depnorm[256];

      normalize_path(dep, depnorm, sizeof(depnorm));

      for(int j = 0; j < count; j++)
      {
        if(j == i || strcmp(entries[j].path, depnorm) != 0)
          continue;

        // Guard against a dependency cycle, which would spin the layout and
        // accessibility recursion: only accept the link if j is not already a
        // descendant of i.
        int a = entries[j].parent_idx, steps = 0;

        while(a >= 0 && a != i && steps++ < count)
          a = entries[a].parent_idx;

        if(a != i)
          entries[i].parent_idx = j;

        break;
      }
    }

    free(dep);
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
void
compute_gear_bonuses(TQCharacter *chr, GearBonuses *g)
{
  memset(g, 0, sizeof(*g));

  if(!chr)
    return;

  for(int i = 0; i < 12; i++)
    gear_scan_item(chr->equipment[i], g);
}

// Skill-specific bonus for a normalized skill path (0 if none).
int
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
int
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

// True if a skill node can currently receive points: its tier's mastery-level
// requirement is met and every ancestor on its path has at least one point.
// Shared by the allocation backend (ui_skills_tree.c) and the renderer
// (ui_skills_draw.c, which greys out inaccessible skills).
bool
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
