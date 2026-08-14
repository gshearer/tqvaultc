// tq-dbr-tool: the Database Browser per-mastery skill buckets.

#include "tq_dbr_tool.h"

// --- skills: report the Database Browser's per-mastery Skills buckets -------
//
// Mirrors build_skill_index() in src/ui_db_browser.c: for each of the 11
// masteries, walk its skill tree DBR (skillName1..N), skip the mastery record,
// dedup records resolving to the same display tag, and list each skill with its
// max level and tier.  Kept self-contained: this tool links only arz.c/arc.c
// (no GTK/item_stats/translation), so names use the path basename and the
// skillDisplayName tag rather than the translated name, and the icon/button
// visibility gate (which needs the UI database) is reported but not applied.

// The 11 base masteries, in sidebar order (mirrors DB_MASTERY[] in the browser).
static const struct {
  const char *name;
  const char *mastery_dbr;
  const char *tree_dbr;
} SKILL_MASTERY[] = {
  { "Defense", "records\\skills\\defensive\\defensivemastery.dbr",
    "records\\skills\\defensive\\defensiveskilltree.dbr" },
  { "Earth", "records\\skills\\earth\\earthmastery.dbr",
    "records\\skills\\earth\\earthskilltree.dbr" },
  { "Hunting", "records\\skills\\hunting\\huntingmastery.dbr",
    "records\\skills\\hunting\\huntingskilltree.dbr" },
  { "Nature", "records\\skills\\nature\\naturemastery.dbr",
    "records\\skills\\nature\\natureskilltree.dbr" },
  { "Spirit", "records\\skills\\spirit\\spiritmastery.dbr",
    "records\\skills\\spirit\\spiritskilltree.dbr" },
  { "Storm", "records\\skills\\storm\\stormmastery.dbr",
    "records\\skills\\storm\\stormskilltree.dbr" },
  { "Warfare", "records\\skills\\warfare\\warfaremastery.dbr",
    "records\\skills\\warfare\\warfareskilltree.dbr" },
  { "Dream", "records\\xpack\\skills\\dream\\dreammastery.dbr",
    "records\\xpack\\skills\\dream\\dreamskilltree.dbr" },
  { "Rune", "records\\xpack2\\skills\\runemaster\\runemaster_mastery.dbr",
    "records\\xpack2\\skills\\runemaster\\runemaster_skilltree.dbr" },
  { "Rogue", "records\\skills\\stealth\\stealthmastery.dbr",
    "records\\skills\\stealth\\stealthskilltree.dbr" },
  { "Neidan", "records\\xpack4\\skills\\neidan\\neidanmastery.dbr",
    "records\\xpack4\\skills\\neidan\\neidanskilltree.dbr" },
};
#define NUM_SKILL_MASTERY 11

// Read a skill's skillDisplayName tag, recursing through buff/pet refs when its
// own record carries none.  Returns malloc'd (caller frees) or NULL.
static char *
skill_display_tag(TQArzFile *arz, const char *path, int depth)
{
  if(!path || !path[0] || depth > 4)
    return(NULL);

  TQArzRecordData *d = arz_read_record(arz, path);

  if(!d)
    return(NULL);

  char *tag = arz_record_get_string(d, "skillDisplayName", NULL);

  if(tag && tag[0])
  {
    arz_record_data_free(d);
    return(tag);
  }

  free(tag);

  static const char *refs[] = { "buffSkillName", "petSkillName" };
  char *result = NULL;

  for(int r = 0; r < 2 && !result; r++)
  {
    char *ref = arz_record_get_string(d, refs[r], NULL);

    if(ref && ref[0])
      result = skill_display_tag(arz, ref, depth + 1);

    free(ref);
  }

  arz_record_data_free(d);
  return(result);
}

// Resolve a skill's max allocatable level, following pet/buff refs when the
// record itself lacks skillMaxLevel.  Mirrors db_skill_max_level().
static int
skill_max_level_t(TQArzFile *arz, const char *path, int depth)
{
  if(!path || !path[0] || depth > 4)
    return(0);

  TQArzRecordData *d = arz_read_record(arz, path);

  if(!d)
    return(0);

  bool found = false;
  int ml = arz_record_get_int(d, "skillMaxLevel", 0, &found);

  if(found && ml > 0)
  {
    arz_record_data_free(d);
    return(ml);
  }

  static const char *refs[] = { "petSkillName", "buffSkillName" };
  int result = 0;

  for(int r = 0; r < 2 && !result; r++)
  {
    char *ref = arz_record_get_string(d, refs[r], NULL);

    if(ref && ref[0])
      result = skill_max_level_t(arz, ref, depth + 1);

    free(ref);
  }

  arz_record_data_free(d);
  return(result);
}

// True if a skill resolves an up-icon bitmap (own record or, recursively, a
// buff/pet ref).
static bool
skill_has_icon_t(TQArzFile *arz, const char *path, int depth)
{
  if(!path || !path[0] || depth > 4)
    return(false);

  TQArzRecordData *d = arz_read_record(arz, path);

  if(!d)
    return(false);

  char *bmp = arz_record_get_string(d, "skillUpBitmapName", NULL);
  bool has = bmp && bmp[0];

  free(bmp);

  if(!has)
  {
    static const char *refs[] = { "buffSkillName", "petSkillName" };

    for(int r = 0; r < 2 && !has; r++)
    {
      char *ref = arz_record_get_string(d, refs[r], NULL);

      if(ref && ref[0])
        has = skill_has_icon_t(arz, ref, depth + 1);

      free(ref);
    }
  }

  arz_record_data_free(d);
  return(has);
}

// Lowercase path basename without extension (e.g. ".../Adrenaline.dbr" ->
// "adrenaline"); written into out.
static void
skill_basename(const char *path, char *out, size_t outsz)
{
  const char *base = path;

  for(const char *p = path; *p; p++)
    if(*p == '/' || *p == '\\')
      base = p + 1;

  const char *dot = strrchr(base, '.');
  size_t len = dot ? (size_t)(dot - base) : strlen(base);

  if(len >= outsz)
    len = outsz - 1;

  for(size_t i = 0; i < len; i++)
    out[i] = (char)tolower((unsigned char)base[i]);

  out[len] = '\0';
}

// Read one skill-window control pane and add every button's target skillName
// (normalized) to the set.  Mirrors add_pane()/add_button() in ui_skills_layout.c.
static void
sbtn_add_pane(TQArzFile *arz, const char *pane_path, GHashTable *set)
{
  TQArzRecordData *pane = arz_read_record(arz, pane_path);

  if(!pane)
    return;

  TQVariable *buttons = arz_record_get_var(pane, arz_intern("tabSkillButtons"));

  if(buttons && buttons->type == TQ_VAR_STRING)
    for(uint32_t j = 0; j < buttons->count; j++)
    {
      const char *bp = buttons->value.str[j];

      if(!bp || !bp[0])
        continue;

      TQArzRecordData *btn = arz_read_record(arz, bp);

      if(!btn)
        continue;

      char *skill = arz_record_get_string(btn, "skillName", NULL);

      if(skill && skill[0])
      {
        char *norm = normalize_path(skill);

        if(g_hash_table_contains(set, norm))
          g_free(norm);
        else
          g_hash_table_add(set, norm);  // takes ownership
      }

      free(skill);
      arz_record_data_free(btn);
    }

  arz_record_data_free(pane);
}

// Build the set of normalized skill paths that have an in-game skill-window
// button.  Mirrors load_map() in ui_skills_layout.c: take the newest available
// skillswindow.dbr, enumerate skillCtrlPane1..16, and union their buttons.
static GHashTable *
build_skill_button_set(TQArzFile *arz)
{
  static const char *WIN[] = {
    "records\\xpack4\\ui\\skills\\skillswindow.dbr",
    "records\\xpack3\\ui\\skills\\skillswindow.dbr",
    "records\\xpack\\ui\\skills\\skillswindow.dbr",
    "records\\ui\\skills\\skillswindow.dbr",
    NULL,
  };
  GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  TQArzRecordData *win = NULL;

  for(int i = 0; WIN[i]; i++)
    if((win = arz_read_record(arz, WIN[i])) != NULL)
      break;

  if(!win)
    return(set);

  for(int i = 1; i <= 16; i++)
  {
    char field[32];

    snprintf(field, sizeof(field), "skillCtrlPane%d", i);

    char *pane = arz_record_get_string(win, field, NULL);

    if(pane && pane[0])
    {
      char *norm = normalize_path(pane);

      sbtn_add_pane(arz, norm, set);
      g_free(norm);
    }

    free(pane);
  }

  arz_record_data_free(win);
  return(set);
}

// True if a skill path has an in-game skill-window button (set membership).
static bool
skill_has_button(GHashTable *set, const char *path)
{
  char *norm = normalize_path(path);
  bool has = g_hash_table_contains(set, norm);

  g_free(norm);
  return(has);
}

int
cmd_skills(const char *arz_path)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  printf("Database Browser skills — %s\n\n", arz_path);

  // The in-game skill-window button set gates visibility, exactly like the
  // visual skill manager (and build_skill_index in the browser).
  GHashTable *buttons = build_skill_button_set(arz);
  long grand_skills = 0, grand_noicon = 0;

  for(int m = 0; m < NUM_SKILL_MASTERY; m++)
  {
    int ml = skill_max_level_t(arz, SKILL_MASTERY[m].mastery_dbr, 0);
    bool use_db = skill_has_button(buttons, SKILL_MASTERY[m].mastery_dbr);

    printf("== %s Mastery ==   (mastery max level %d, %s)\n",
           SKILL_MASTERY[m].name, ml,
           use_db ? "button-gated" : "icon-gated fallback");

    TQArzRecordData *tree = arz_read_record(arz, SKILL_MASTERY[m].tree_dbr);

    if(!tree)
    {
      printf("  (no skill tree)\n\n");
      continue;
    }

    // In icon-gated fallback, dedup by display tag (the translated-name
    // equivalent the browser uses); button-gated mode needs no dedup.
    char seen[64][128];
    int num_seen = 0;
    int count = 0;

    for(int n = 1; n <= 32; n++)
    {
      char field[32];

      snprintf(field, sizeof(field), "skillName%d", n);

      char *sp = arz_record_get_string(tree, field, NULL);

      if(!sp || !sp[0])
      {
        free(sp);
        continue;
      }

      if(strcasestr(sp, "mastery"))
      {
        free(sp);
        continue;  // the mastery record itself
      }

      char *tag = skill_display_tag(arz, sp, 0);
      bool icon = skill_has_icon_t(arz, sp, 0);

      if(use_db)
      {
        if(!skill_has_button(buttons, sp))
        {
          free(tag);
          free(sp);
          continue;  // not shown in-game (auto-applied helper / pet modifier)
        }
      }
      else
      {
        if(!icon)
        {
          free(tag);
          free(sp);
          continue;
        }

        const char *dedup = (tag && tag[0]) ? tag : sp;
        bool dup = false;

        for(int s = 0; s < num_seen; s++)
          if(strcasecmp(seen[s], dedup) == 0)
          {
            dup = true;
            break;
          }

        if(dup)
        {
          free(tag);
          free(sp);
          continue;
        }

        if(num_seen < 64)
          snprintf(seen[num_seen++], sizeof(seen[0]), "%s", dedup);
      }

      int sml = skill_max_level_t(arz, sp, 0);
      char base[128];

      skill_basename(sp, base, sizeof(base));

      printf("  %-28s maxLvl=%-2d  tag=%-20s%s\n", base, sml,
             tag && tag[0] ? tag : "(none)", icon ? "" : "   [no-icon]");

      count++;
      grand_skills++;
      if(!icon)
        grand_noicon++;

      free(tag);
      free(sp);
    }

    printf("  -> %d skills\n\n", count);
    arz_record_data_free(tree);
  }

  printf("TOTAL: %ld skills across %d masteries "
         "(%ld shown without a resolvable icon)\n",
         grand_skills, NUM_SKILL_MASTERY, grand_noicon);

  g_hash_table_destroy(buttons);
  arz_free(arz);
  return(0);
}
