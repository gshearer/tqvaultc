// tq-dbr-tool: item-facing reports -- relic/charm/artifact bonus chains,
// Database Browser category buckets and item sets.

#include "tq_dbr_tool.h"
#include "../parse_num.h"

// prints all bonus entries with their weights and stats.
// arz_path: path to the .arz database file.
// item_path: DBR record path for the item to inspect.
// Returns 0 on success, 1 on failure.
int
cmd_bonus(const char *arz_path, const char *item_path)
{
  TQArzFile *arz = arz_load(arz_path);
  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  TQArzRecordData *item_data = arz_read_record(arz, item_path);
  if(!item_data)
  {
    fprintf(stderr, "Item record not found: %s\n", item_path);
    arz_free(arz);
    return(1);
  }

  // Try bonusTableName (relics/charms) first
  char *table_path = arz_record_get_string(item_data, "bonusTableName", NULL);

  // If not found, try artifact formula path
  if(!table_path || !table_path[0])
  {
    free(table_path);
    table_path = NULL;

    // Construct formula path: replace filename with arcaneformulae/<name>_formula.dbr
    char path_buf[512];

    snprintf(path_buf, sizeof(path_buf), "%s", item_path);
    char *last_slash = strrchr(path_buf, '/');

    if(!last_slash)
      last_slash = strrchr(path_buf, '\\');

    if(last_slash)
    {
      // Extract basename without extension
      char basename[256];
      const char *fname = last_slash + 1;
      const char *dot = strrchr(fname, '.');

      if(dot)
        snprintf(basename, sizeof(basename), "%.*s", (int)(dot - fname), fname);
      else
        snprintf(basename, sizeof(basename), "%s", fname);

      // Try <dir>/arcaneformulae/<name>_formula.dbr
      snprintf(last_slash + 1, sizeof(path_buf) - (last_slash + 1 - path_buf),
               "arcaneformulae/%s_formula.dbr", basename);

      printf("Trying formula path: %s\n", path_buf);
      TQArzRecordData *formula = arz_read_record(arz, path_buf);

      if(formula)
      {
        table_path = arz_record_get_string(formula, "artifactBonusTableName", NULL);
        arz_record_data_free(formula);
      }
    }
  }

  if(!table_path || !table_path[0])
  {
    printf("No bonus table found for: %s\n", item_path);
    printf("\nItem fields:\n");

    // Show name-related fields
    const char *name_fields[] = {"description", "itemNameTag", "lootRandomizerName",
                                 "FileDescription", "bonusTableName", "Class", NULL};

    for(int f = 0; name_fields[f]; f++)
    {
      for(uint32_t v = 0; v < item_data->num_vars; v++)
      {
        if(strcasecmp(item_data->vars[v].name, name_fields[f]) == 0)
        {
          print_variable(&item_data->vars[v]);
          break;
        }
      }
    }

    free(table_path);
    arz_record_data_free(item_data);
    arz_free(arz);
    return(1);
  }

  printf("Item: %s\n", item_path);
  printf("Bonus table: %s\n\n", table_path);

  // Load the bonus table
  TQArzRecordData *table = arz_read_record(arz, table_path);
  if(!table)
  {
    fprintf(stderr, "Failed to load bonus table: %s\n", table_path);
    free(table_path);
    arz_record_data_free(item_data);
    arz_free(arz);
    return(1);
  }

  // Collect randomizerName[N] / randomizerWeight[N] pairs.  Indices are
  // SPARSE (e.g. randomizerName10, 13, 16, ...), so scan every variable and
  // bucket by the trailing number instead of assuming a contiguous 1..N range.
#define MAX_BONUS_IDX 256
  const char *bp_path[MAX_BONUS_IDX] = { 0 };
  float bp_weight[MAX_BONUS_IDX] = { 0 };
  float total_weight = 0;

  for(uint32_t v = 0; v < table->num_vars; v++)
  {
    TQVariable *var = &table->vars[v];

    if(!var->name)
      continue;

    if(strncasecmp(var->name, "randomizerName", 14) == 0 &&
       var->type == TQ_VAR_STRING && var->count > 0 && var->value.str &&
       var->value.str[0] && var->value.str[0][0])
    {
      int idx = 0;

      if(parse_int(var->name + 14, &idx) && idx >= 0 && idx < MAX_BONUS_IDX)
        bp_path[idx] = var->value.str[0];
    }
    else if(strncasecmp(var->name, "randomizerWeight", 16) == 0)
    {
      int idx = 0;
      float w = 0;

      if(!parse_int(var->name + 16, &idx))
        continue;

      if(var->type == TQ_VAR_INT && var->count > 0 && var->value.i32)
        w = (float)var->value.i32[0];
      else if(var->type == TQ_VAR_FLOAT && var->count > 0 && var->value.f32)
        w = var->value.f32[0];

      if(idx >= 0 && idx < MAX_BONUS_IDX)
        bp_weight[idx] = w;
    }
  }

  for(int i = 0; i < MAX_BONUS_IDX; i++)
    if(bp_path[i] && bp_weight[i] > 0)
      total_weight += bp_weight[i];

  int n_bonuses = 0;

  for(int i = 0; i < MAX_BONUS_IDX; i++)
  {
    const char *bonus_path = bp_path[i];

    if(!bonus_path || bp_weight[i] <= 0)
      continue;

    float pct = total_weight > 0 ? bp_weight[i] / total_weight * 100.0f : 0;

    n_bonuses++;
    printf("Bonus %d (weight %.0f, %.2f%%): %s\n", i, bp_weight[i], pct, bonus_path);

    // Load the bonus record and show its name fields + non-zero stats
    TQArzRecordData *bonus = arz_read_record(arz, bonus_path);

    if(bonus)
    {
      // Show name fields
      const char *nf[] = {"description", "lootRandomizerName", "FileDescription", NULL};

      for(int f = 0; nf[f]; f++)
      {
        for(uint32_t v = 0; v < bonus->num_vars; v++)
        {
          if(strcasecmp(bonus->vars[v].name, nf[f]) == 0 &&
              bonus->vars[v].type == TQ_VAR_STRING &&
              bonus->vars[v].value.str &&
              bonus->vars[v].value.str[0] &&
              bonus->vars[v].value.str[0][0])
            printf("  %-30s %s\n", nf[f], bonus->vars[v].value.str[0]);
        }
      }

      // Show non-zero numeric stats
      for(uint32_t v = 0; v < bonus->num_vars; v++)
      {
        TQVariable *var = &bonus->vars[v];

        // Skip metadata fields
        if(strcasecmp(var->name, "Class") == 0 ||
            strcasecmp(var->name, "templateName") == 0 ||
            strcasecmp(var->name, "FileDescription") == 0 ||
            strcasecmp(var->name, "description") == 0 ||
            strcasecmp(var->name, "lootRandomizerName") == 0 ||
            strcasecmp(var->name, "itemClassification") == 0)
          continue;

        if(var->type == TQ_VAR_FLOAT && var->value.f32)
        {
          for(uint32_t j = 0; j < var->count; j++)
          {
            if(fabsf(var->value.f32[j]) > 0.0001f)
            {
              printf("  %-30s %.2f\n", var->name, var->value.f32[j]);
              break;
            }
          }
        }
        else if(var->type == TQ_VAR_INT && var->value.i32)
        {
          for(uint32_t j = 0; j < var->count; j++)
          {
            if(var->value.i32[j] != 0)
            {
              printf("  %-30s %d\n", var->name, var->value.i32[j]);
              break;
            }
          }
        }
      }

      arz_record_data_free(bonus);
    }

    printf("\n");
  }

  printf("%d completion bonuses, total weight %.0f.\n\n", n_bonuses, total_weight);

  arz_record_data_free(table);
  free(table_path);
  arz_record_data_free(item_data);
  arz_free(arz);
  return(0);
}

// --- categories: report the Database Browser's category buckets -----------
//
// Mirrors db_categorize() in src/ui_db_browser.c so the in-app browser's
// categorization can be validated headlessly (the GUI is never run here).
// Kept self-contained: this tool links only arz.c/arc.c (no GTK/item_stats),
// so the Class->category mapping is duplicated rather than shared.

// Leaf categories in display order; CAT_GROUP/CAT_LEAF index together.
static const char *CAT_GROUP[] = {
  "Weapons", "Weapons", "Weapons", "Weapons", "Weapons", "Weapons", "Weapons",
  "Armor", "Armor", "Armor", "Armor", "Armor",
  "Jewelry", "Jewelry",
  "Relics", "Charms", "Artifacts", "Scrolls",
};
static const char *CAT_LEAF[] = {
  "Sword", "Axe", "Mace", "Spear", "Bow", "Staff", "Throwing",
  "Head", "Torso", "Arm", "Leg", "Shield",
  "Ring", "Amulet",
  "Relics", "Charms", "Artifacts", "Scrolls",
};
#define NCAT 18

// Equipment Class -> leaf-category index (matches item_gear_type's class_map).
static const struct { const char *cls; int cat; } GEAR_CAT[] = {
  { "WeaponMelee_Sword", 0 },   { "WeaponMelee_Axe", 1 },
  { "WeaponMelee_Mace", 2 },    { "WeaponHunting_Spear", 3 },
  { "WeaponHunting_Bow", 4 },   { "WeaponMagical_Staff", 5 },
  { "WeaponHunting_RangedOneHand", 6 },
  { "ArmorProtective_Head", 7 },     { "ArmorProtective_UpperBody", 8 },
  { "ArmorProtective_Forearm", 9 },  { "ArmorProtective_LowerBody", 10 },
  { "WeaponArmor_Shield", 11 },
  { "ArmorJewelry_Ring", 12 },       { "ArmorJewelry_Amulet", 13 },
};

// Decide a record's browse category from its Class/itemClassification, or -1.
// lower_path: the record path, already lowercased (backslash separators).
static int
categorize(const char *cls, const char *classif, const char *lower_path)
{
  if(!cls)
    return(-1);

  if(strstr(lower_path, "\\old\\") || strstr(lower_path, "\\default\\"))
    return(-1);

  for(size_t i = 0; i < sizeof(GEAR_CAT) / sizeof(GEAR_CAT[0]); i++)
    if(strcasecmp(cls, GEAR_CAT[i].cls) == 0)
    {
      // Gear is only included when it carries a real rarity.
      if(!classif)
        return(-1);
      if(strcasecmp(classif, "Magical")   == 0 ||
         strcasecmp(classif, "Rare")      == 0 ||
         strcasecmp(classif, "Epic")      == 0 ||
         strcasecmp(classif, "Legendary") == 0)
        return(GEAR_CAT[i].cat);
      return(-1);
    }

  if(strcasecmp(cls, "ItemRelic") == 0)
    return(14);
  if(strcasecmp(cls, "ItemCharm") == 0)
    return(15);
  if(strcasecmp(cls, "ItemArtifact") == 0)
    return(16);
  if(strcasecmp(cls, "OneShot_Scroll") == 0)
    return(17);

  return(-1);
}

int
cmd_categories(const char *arz_path)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  int counts[NCAT] = { 0 };
  uint32_t scanned = 0;
  long total = 0;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(!arz->records[i].path)
      continue;

    TQArzRecordData *data = arz_read_record(arz, arz->records[i].path);

    if(!data)
      continue;

    scanned++;

    char *cls = arz_record_get_string(data, "Class", NULL);
    char *classif = arz_record_get_string(data, "itemClassification", NULL);
    char *lower_path = g_ascii_strdown(arz->records[i].path, -1);

    int cat = categorize(cls, classif, lower_path);

    if(cat >= 0)
    {
      counts[cat]++;
      total++;
    }

    g_free(lower_path);
    free(cls);
    free(classif);
    arz_record_data_free(data);
  }

  printf("Database Browser categories — %s\n\n", arz_path);

  const char *cur_group = NULL;

  for(int c = 0; c < NCAT; c++)
  {
    if(!cur_group || strcmp(cur_group, CAT_GROUP[c]) != 0)
    {
      cur_group = CAT_GROUP[c];
      printf("%s\n", cur_group);
    }
    printf("  %-12s %6d\n", CAT_LEAF[c], counts[c]);
  }

  printf("\nTOTAL items: %ld   (scanned %u records)\n", total, scanned);

  arz_free(arz);
  return(0);
}

// --- sets: report the Database Browser's item-set buckets -----------------
//
// Mirrors the Sets view of src/ui_db_browser.c so the set enumeration,
// member validation and bonus tiering can be validated headlessly.  Kept
// self-contained: this tool links only arz.c/arc.c (no GTK/item_stats), so the
// set-path glob and tiering are duplicated rather than shared.
//
// Set discovery follows tqdb's resources.py SETS globs:
//   records\item\sets\*.dbr                (base game)
//   records\xpack*\item*\set*\*.dbr        (the four expansions)
// which naturally excludes dev/sandbox set trees.

// Compare a path segment (start of a '\'-delimited component) to a literal.
static bool
seg_eq(const char *seg, const char *lit)
{
  size_t len = strlen(lit);

  return(strncmp(seg, lit, len) == 0 && (seg[len] == '\\' || seg[len] == '\0'));
}

// True if a path segment starts with the given prefix (glob `prefix*`).
static bool
seg_prefix(const char *seg, const char *pfx)
{
  return(strncmp(seg, pfx, strlen(pfx)) == 0);
}

// Decide whether a lowercased, backslash-separated record path is an item set,
// matching the two SETS globs above (exact directory depth enforced).
static bool
is_set_path(const char *lp)
{
  size_t n = strlen(lp);

  if(n < 5 || strcmp(lp + n - 4, ".dbr") != 0)
    return(false);

  // Walk the first four segments: records \ s1 \ s2 \ s3...
  const char *p0 = lp;
  const char *p1 = strchr(p0, '\\');

  if(!seg_eq(p0, "records") || !p1)
    return(false);
  p1++;

  const char *p2 = strchr(p1, '\\');

  if(!p2)
    return(false);
  p2++;

  const char *p3 = strchr(p2, '\\');

  if(!p3)
    return(false);
  p3++;

  const char *p3end = strchr(p3, '\\');

  if(!p3end)
    // Exactly four segments -> glob1: records\item\sets\<file>.dbr
    return(seg_eq(p1, "item") && seg_eq(p2, "sets"));

  // Five+ segments: the file must sit directly under the set* directory.
  const char *p4 = p3end + 1;

  if(strchr(p4, '\\'))
    return(false);  // six or more segments -> not a SETS glob

  // glob2: records\xpack*\item*\set*\<file>.dbr
  return(seg_prefix(p1, "xpack") && seg_prefix(p2, "item") && seg_prefix(p3, "set"));
}

// True if a set member path resolves to a real, named item (matches tqdb's
// ItemEquipmentParser rule: a member must carry an itemNameTag).  Bare
// directory entries and "#" placeholders fail.
static bool
set_member_is_valid(TQArzFile *arz, const char *mpath)
{
  if(!mpath || !mpath[0] || strcmp(mpath, "#") == 0)
    return(false);

  size_t n = strlen(mpath);

  if(n < 4 || strcasecmp(mpath + n - 4, ".dbr") != 0)
    return(false);

  TQArzRecordData *md = arz_read_record(arz, mpath);

  if(!md)
    return(false);

  char *tag = arz_record_get_string(md, "itemNameTag", NULL);
  bool ok = tag && tag[0];

  free(tag);
  arz_record_data_free(md);
  return(ok);
}

// True for the int routing flags that aren't real stats (offensive*Global,
// *XOR), so they don't count as a bonus when scanning tiers.
static bool
is_routing_flag(const char *name)
{
  size_t n = strlen(name);

  return((n >= 6 && strcasecmp(name + n - 6, "Global") == 0) ||
         (n >= 3 && strcasecmp(name + n - 3, "XOR") == 0));
}

int
cmd_sets(const char *arz_path)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  printf("Database Browser sets — %s\n\n", arz_path);

  long candidates = 0, valid_sets = 0;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(!arz->records[i].path)
      continue;

    char *lp = g_ascii_strdown(arz->records[i].path, -1);
    bool match = is_set_path(lp);

    g_free(lp);

    if(!match)
      continue;

    candidates++;

    TQArzRecordData *data = arz_read_record(arz, arz->records[i].path);

    if(!data)
      continue;

    char *set_name = arz_record_get_string(data, "setName", NULL);

    if(!set_name || !set_name[0])
    {
      free(set_name);
      arz_record_data_free(data);
      continue;  // no setName tag -> not a real, named set
    }

    // Collect valid members.
    TQVariable *members = arz_record_get_var(data, arz_intern("setMembers"));
    int member_count = 0;
    const char *member_paths[64];

    if(members && members->type == TQ_VAR_STRING)
      for(uint32_t m = 0; m < members->count; m++)
      {
        const char *mp = members->value.str[m];

        if(set_member_is_valid(arz, mp) && member_count < 64)
          member_paths[member_count++] = mp;
      }

    if(member_count == 0)
    {
      free(set_name);
      arz_record_data_free(data);
      continue;  // template / placeholder set with no real members
    }

    // Tier depth = longest numeric stat array (excludes routing flags). The
    // array is indexed by (set items - 1), so index P-1 holds the bonus for
    // wearing P pieces.  When the set carries only scalar (single-value)
    // bonuses they apply to the full set, so the piece count comes from the
    // member count instead (mirrors ItemSetParser placing scalars on the
    // top tier).
    int tier_depth = 0;

    for(uint32_t v = 0; v < data->num_vars; v++)
    {
      TQVariable *var = &data->vars[v];

      if((var->type == TQ_VAR_INT || var->type == TQ_VAR_FLOAT) &&
         !is_routing_flag(var->name) && (int)var->count > tier_depth)
        tier_depth = (int)var->count;
    }

    int full_pieces = (tier_depth > 1) ? tier_depth : member_count;

    // For each piece count (>= 2, since one piece never grants a set bonus),
    // clamp short arrays to their last element (matching the in-game indexing)
    // and record which set-item counts grant any bonus.
    int active_pieces[64];
    int active_count = 0;

    for(int p = 2; p <= full_pieces && p <= 64; p++)
    {
      bool has_bonus = false;

      for(uint32_t v = 0; v < data->num_vars && !has_bonus; v++)
      {
        TQVariable *var = &data->vars[v];

        if(var->type != TQ_VAR_INT && var->type != TQ_VAR_FLOAT)
          continue;
        if(is_routing_flag(var->name) || var->count == 0)
          continue;

        int idx = (p - 1 < (int)var->count) ? p - 1 : (int)var->count - 1;

        if(var->type == TQ_VAR_FLOAT && var->value.f32 &&
           fabsf(var->value.f32[idx]) > 0.0001f)
          has_bonus = true;
        else if(var->type == TQ_VAR_INT && var->value.i32 &&
                var->value.i32[idx] != 0)
          has_bonus = true;
      }

      if(has_bonus)
        active_pieces[active_count++] = p;
    }

    valid_sets++;

    printf("%s   \"%s\"\n", arz->records[i].path, set_name);

    for(int m = 0; m < member_count; m++)
      printf("    member  %s\n", member_paths[m]);

    printf("    bonus tiers: %d", active_count);

    if(active_count > 0)
    {
      printf("   (set items:");
      for(int a = 0; a < active_count; a++)
        printf(" %d", active_pieces[a]);
      printf(")");
    }

    printf("\n\n");

    free(set_name);
    arz_record_data_free(data);
  }

  printf("TOTAL sets: %ld   (from %ld glob-matched candidates, scanned %u records)\n",
         valid_sets, candidates, arz->num_records);

  arz_free(arz);
  return(0);
}
