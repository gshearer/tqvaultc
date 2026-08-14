// tq-dbr-tool: the Database Browser Prefix/Suffix affix buckets.

#include "tq_dbr_tool.h"

// --- affixes: report the Database Browser's Prefix/Suffix buckets ----------
//
// Mirrors build_affix_index() in src/ui_db_browser.c (a headless port of
// tqdb's parse_affixes, main.py): walk every affix randomizer table, map each
// `randomizerName*` affix to the equipment type(s) whose tables reference it,
// classify prefix vs suffix by the affix record's own path, and list them.
// Kept self-contained: this tool links only arz.c/arc.c (no GTK/item_stats/
// translation), so the glob, gear-label mapping and stat count are duplicated
// here, and names use FileDescription rather than the translation tag.

// Map an affix-table filename prefix to a gear label, or NULL if unknown.
// Mirrors db_affix_gear_label() / tqdb get_affix_table_type().
static const char *
affix_gear_label(const char *file_prefix)
{
  static const struct { const char *pfx; const char *label; } MAP[] = {
    { "armmage", "Arm Armor (Caster)" },   { "armsmage", "Arm Armor (Caster)" },
    { "armmelee", "Arm Armor (Fighter)" }, { "armsmelee", "Arm Armor (Fighter)" },
    { "headmage", "Head Armor (Caster)" }, { "headmelee", "Head Armor (Fighter)" },
    { "legmage", "Leg Armor (Caster)" },   { "legsmage", "Leg Armor (Caster)" },
    { "legmelee", "Leg Armor (Fighter)" }, { "legsmelee", "Leg Armor (Fighter)" },
    { "torsomage", "Torso Armor (Caster)" }, { "torsomelee", "Torso Armor (Fighter)" },
    { "amulet", "Amulet" }, { "ring", "Ring" }, { "shield", "Shield" },
    { "axe", "Axe" }, { "bow", "Bow" }, { "club", "Mace" }, { "spear", "Spear" },
    { "staff", "Staff" }, { "sword", "Sword" }, { "roh", "Throwing Weapon" },
  };

  for(size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
    if(strncmp(file_prefix, MAP[i].pfx, strlen(MAP[i].pfx)) == 0)
      return(MAP[i].label);

  return(NULL);
}

// If lp (lowercased, backslash-separated) is an affix randomizer table, return
// a g_strdup'd equipment-type label (caller frees) and else NULL.  Mirrors
// db_affix_table_label() in ui_db_browser.c.
static char *
affix_table_label(const char *lp)
{
  if(strncmp(lp, "records\\", 8) != 0)
    return(NULL);

  const char *marker = strstr(lp, "\\lootmagicalaffixes\\");

  if(!marker)
    return(NULL);

  const char *seg_a = marker + strlen("\\lootmagicalaffixes\\");
  const char *s1 = strchr(seg_a, '\\');

  if(!s1)
    return(NULL);

  size_t alen = (size_t)(s1 - seg_a);

  if(!(alen == 6 && (strncmp(seg_a, "prefix", 6) == 0 ||
                     strncmp(seg_a, "suffix", 6) == 0)))
    return(NULL);

  const char *seg_b = s1 + 1;
  const char *s2 = strchr(seg_b, '\\');

  if(!s2 || strncmp(seg_b, "tables", 6) != 0)
    return(NULL);

  const char *file = s2 + 1;

  if(strchr(file, '\\'))
    return(NULL);

  char token[64];
  size_t t = 0;

  for(const char *p = file; *p && *p != '_' && *p != '.' && t < sizeof(token) - 1; p++)
    token[t++] = *p;
  token[t] = '\0';

  const char *label = affix_gear_label(token);

  if(label)
    return(g_strdup(label));

  char *cap = g_strdup(token);

  if(cap[0])
    cap[0] = g_ascii_toupper(cap[0]);
  return(cap);
}

// One affix DBR's resolved attributes (read once, cached by normalized path).
typedef struct {
  char *tag;      // lootRandomizerName (may be NULL)
  char *name;     // FileDescription (may be NULL)
  char *classif;  // itemClassification (may be NULL)
  int stat_count; // non-zero numeric stats (properties proxy)
  int kind;       // 0 == prefix, 1 == suffix, -1 == neither
} PInfo;

static void
pinfo_free(gpointer d)
{
  PInfo *p = d;

  if(!p)
    return;
  free(p->tag);
  free(p->name);
  free(p->classif);
  free(p);
}

// One logical (merged-by-tag) affix.  Mirrors build_affix_index in
// ui_db_browser.c: same-tag records collapse into one entry, accumulating the
// gear types of every referencing table and counting the distinct variants.
typedef struct {
  char *name;          // FileDescription or tag (representative = first variant)
  char *classif;       // representative itemClassification
  int stat_count;      // representative non-zero stat count
  int kind;            // 0 == prefix, 1 == suffix
  int variants;        // number of distinct DBR records under this tag
  GHashTable *types;   // set<char*> of gear labels
} MAffix;

static void
maffix_free(gpointer d)
{
  MAffix *m = d;

  if(!m)
    return;
  free(m->name);
  free(m->classif);
  if(m->types)
    g_hash_table_destroy(m->types);
  free(m);
}

// Count a record's non-zero numeric stats, excluding metadata and the int
// routing flags (offensive*Global / *XOR) — a rough "has properties" proxy.
static int
affix_stat_count(TQArzRecordData *dbr)
{
  int n = 0;

  for(uint32_t v = 0; v < dbr->num_vars; v++)
  {
    TQVariable *var = &dbr->vars[v];

    if(!var->name)
      continue;

    size_t len = strlen(var->name);

    if((len >= 6 && strcasecmp(var->name + len - 6, "Global") == 0) ||
       (len >= 3 && strcasecmp(var->name + len - 3, "XOR") == 0))
      continue;
    if(strcasecmp(var->name, "levelRequirement") == 0 ||
       strcasecmp(var->name, "lootRandomizerCost") == 0 ||
       strcasecmp(var->name, "lootRandomizerJitter") == 0 ||
       strcasecmp(var->name, "marketAdjustmentPercent") == 0)
      continue;

    if(var->type == TQ_VAR_FLOAT && var->value.f32)
    {
      for(uint32_t j = 0; j < var->count; j++)
        if(fabsf(var->value.f32[j]) > 0.0001f)
        {
          n++;
          break;
        }
    }
    else if(var->type == TQ_VAR_INT && var->value.i32)
    {
      for(uint32_t j = 0; j < var->count; j++)
        if(var->value.i32[j] != 0)
        {
          n++;
          break;
        }
    }
  }

  return(n);
}

static int
affix_name_cmp(const void *a, const void *b)
{
  const MAffix *x = *(MAffix * const *)a;
  const MAffix *y = *(MAffix * const *)b;

  return(g_ascii_strcasecmp(x->name ? x->name : "", y->name ? y->name : ""));
}

// qsort helper: order char* by strcmp (for sorting gear-label arrays).
static int
str_ptr_cmp(const void *a, const void *b)
{
  return(strcmp(*(const char * const *)a, *(const char * const *)b));
}

int
cmd_affixes(const char *arz_path)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  // Resolve each referenced affix DBR once (cached by normalized path).
  GHashTable *pinfo = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, pinfo_free);
  // "P|<tag>" / "S|<tag>" (tag-less keyed by "<kind>|@<path>") -> MAffix*
  GHashTable *merged = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, maffix_free);
  long tables = 0, refs = 0;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(!arz->records[i].path)
      continue;

    char *lp = g_ascii_strdown(arz->records[i].path, -1);

    for(char *p = lp; *p; p++)
      if(*p == '/')
        *p = '\\';

    char *label = affix_table_label(lp);

    g_free(lp);

    if(!label)
      continue;

    tables++;

    TQArzRecordData *table = arz_read_record(arz, arz->records[i].path);

    if(!table)
    {
      g_free(label);
      continue;
    }

    for(uint32_t v = 0; v < table->num_vars; v++)
    {
      TQVariable *var = &table->vars[v];

      if(!var->name ||
         strncasecmp(var->name, "randomizerName", 14) != 0 ||
         var->type != TQ_VAR_STRING || var->count == 0 ||
         !var->value.str || !var->value.str[0] || !var->value.str[0][0])
        continue;

      const char *affix_path = var->value.str[0];
      size_t plen = strlen(affix_path);

      // Skip malformed randomizer entries (numeric placeholders); mirrors the
      // .dbr/existence filter in ui_db_browser.c and tqdb's affix_dbr.exists().
      if(plen < 4 || strcasecmp(affix_path + plen - 4, ".dbr") != 0)
        continue;

      // Resolve this affix DBR's attributes once.
      char *npath = normalize_path(affix_path);
      PInfo *pi = g_hash_table_lookup(pinfo, npath);

      if(!pi)
      {
        pi = g_malloc0(sizeof(*pi));
        pi->kind = strstr(npath, "\\suffix\\") ? 1
                 : (strstr(npath, "\\prefix\\") ? 0 : -1);

        TQArzRecordData *dbr = arz_read_record(arz, affix_path);

        if(dbr)
        {
          pi->tag = arz_record_get_string(dbr, "lootRandomizerName", NULL);
          pi->name = arz_record_get_string(dbr, "FileDescription", NULL);
          pi->classif = arz_record_get_string(dbr, "itemClassification", NULL);
          pi->stat_count = affix_stat_count(dbr);
          arz_record_data_free(dbr);
        }

        g_hash_table_insert(pinfo, npath, pi);  // takes ownership of npath
      }
      else
        g_free(npath);

      if(pi->kind < 0)
        continue;  // not a standard prefix/suffix record

      refs++;

      // Merge key: translation tag, else the path (so tag-less stay distinct).
      char *key;

      if(pi->tag && pi->tag[0])
        key = g_strdup_printf("%c|%s", pi->kind ? 'S' : 'P', pi->tag);
      else
      {
        char *np2 = normalize_path(affix_path);

        key = g_strdup_printf("%c|@%s", pi->kind ? 'S' : 'P', np2);
        g_free(np2);
      }

      MAffix *m = g_hash_table_lookup(merged, key);

      if(!m)
      {
        m = g_malloc0(sizeof(*m));
        m->kind = pi->kind;
        m->stat_count = pi->stat_count;
        m->name = (pi->name && pi->name[0]) ? strdup(pi->name)
                : (pi->tag ? strdup(pi->tag) : strdup(affix_path));
        m->classif = pi->classif ? strdup(pi->classif) : NULL;
        m->types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        m->variants = 1;
        g_hash_table_insert(merged, key, m);  // takes ownership of key
      }
      else
        g_free(key);

      if(!g_hash_table_contains(m->types, label))
        g_hash_table_add(m->types, g_strdup(label));
    }

    arz_record_data_free(table);
    g_free(label);
  }

  // Bucket merged affixes; tally variants as the distinct affix DBRs per tag.
  GPtrArray *pre = g_ptr_array_new();
  GPtrArray *suf = g_ptr_array_new();
  GHashTableIter it;
  gpointer k, val;

  // Reset per-tag variant counts, then count one per distinct affix DBR.
  g_hash_table_iter_init(&it, merged);
  while(g_hash_table_iter_next(&it, &k, &val))
    ((MAffix *)val)->variants = 0;

  {
    GHashTableIter pit;
    gpointer pk, pv;

    g_hash_table_iter_init(&pit, pinfo);
    while(g_hash_table_iter_next(&pit, &pk, &pv))
    {
      PInfo *pi = pv;

      if(pi->kind < 0)
        continue;

      char keybuf[320];

      if(pi->tag && pi->tag[0])
        snprintf(keybuf, sizeof(keybuf), "%c|%s", pi->kind ? 'S' : 'P', pi->tag);
      else
        snprintf(keybuf, sizeof(keybuf), "%c|@%s", pi->kind ? 'S' : 'P', (char *)pk);

      MAffix *m = g_hash_table_lookup(merged, keybuf);

      if(m)
        m->variants++;
    }
  }

  g_hash_table_iter_init(&it, merged);
  while(g_hash_table_iter_next(&it, &k, &val))
  {
    MAffix *m = val;

    g_ptr_array_add(m->kind ? suf : pre, m);
  }

  g_ptr_array_sort(pre, affix_name_cmp);
  g_ptr_array_sort(suf, affix_name_cmp);

  printf("Database Browser affixes — %s\n", arz_path);

  GPtrArray *buckets[2] = { pre, suf };
  const char *titles[2] = { "PREFIXES", "SUFFIXES" };

  for(int b = 0; b < 2; b++)
  {
    printf("\n=== %s (%u) ===\n", titles[b], buckets[b]->len);

    for(guint j = 0; j < buckets[b]->len; j++)
    {
      MAffix *m = g_ptr_array_index(buckets[b], j);

      // Sorted, comma-joined gear labels.
      guint nt = g_hash_table_size(m->types);
      const char **arr = g_new(const char *, nt ? nt : 1);
      GHashTableIter ti;
      gpointer tk, tv;
      guint ai = 0;

      g_hash_table_iter_init(&ti, m->types);
      while(g_hash_table_iter_next(&ti, &tk, &tv))
        arr[ai++] = tk;
      qsort(arr, nt, sizeof(char *), str_ptr_cmp);

      printf("  %-26s [%-9s] stats:%-2d rolls:%-2d gear:", m->name,
             m->classif && m->classif[0] ? m->classif : "?",
             m->stat_count, m->variants);
      for(guint t = 0; t < nt; t++)
        printf("%s%s", t ? ", " : " ", arr[t]);
      printf("\n");

      g_free(arr);
    }
  }

  printf("\nTOTAL: %u prefixes, %u suffixes   "
         "(%u merged affixes / %ld table refs from %ld affix tables, "
         "scanned %u records)\n",
         pre->len, suf->len, g_hash_table_size(merged), refs, tables,
         arz->num_records);

  g_ptr_array_free(pre, TRUE);
  g_ptr_array_free(suf, TRUE);
  g_hash_table_destroy(merged);
  g_hash_table_destroy(pinfo);
  arz_free(arz);
  return(0);
}
