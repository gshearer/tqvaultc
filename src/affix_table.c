#include "affix_table.h"
#include "arz.h"
#include "asset_lookup.h"
#include "config.h"
#include "item_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <glib.h>
#include <time.h>

// -- Internal types --

// A single affix-table pair extracted from a LootItemTable record
typedef struct {
  char *prefix_table;     // path to LootRandomizerTable for prefixes
  char *suffix_table;     // path to LootRandomizerTable for suffixes
} AffixTablePair;

// Per-item list of affix table pairs
typedef struct {
  AffixTablePair *pairs;
  int count;
} AffixTableList;

// Global map: normalized item path -> AffixTableList
static GHashTable *g_affix_map = NULL;

// Cache of resolved affix results: normalized item path -> TQItemAffixes*
static GHashTable *g_affix_cache = NULL;


// -- Helpers --

// Normalize a file path to lowercase with backslash separators.
// @param path  input path string
// @return newly-allocated normalized string, or NULL if path is NULL
static char *
normalize_path(const char *path)
{
  if(!path)
    return(NULL);

  char *norm = g_ascii_strdown(path, -1);

  for(char *p = norm; *p; p++)
    if(*p == '/')
      *p = '\\';

  return(norm);
}

// Extract a human-readable name from a file path for fallback display.
// Strips directory, extension, replaces underscores, capitalizes first letter.
// @param path  file path to extract name from
// @return newly-allocated display name string (caller must free)
static char *
pretty_filename(const char *path)
{
  if(!path)
    return(strdup("???"));

  const char *last_sep = strrchr(path, '\\');

  if(!last_sep)
    last_sep = strrchr(path, '/');

  const char *name = last_sep ? last_sep + 1 : path;
  char *copy = strdup(name);

  if(!copy)
    return(NULL);

  // Strip .dbr extension
  char *dot = strrchr(copy, '.');

  if(dot)
    *dot = '\0';

  // Replace underscores with spaces and capitalize first letter
  for(char *p = copy; *p; p++)
    if(*p == '_')
      *p = ' ';

  if(copy[0] >= 'a' && copy[0] <= 'z')
    copy[0] -= 32;

  return(copy);
}

// Extract effect family and tier from an affix DBR path.
// E.g. "records/.../character_attributestrength_05.dbr"
//    -> family "character attributestrength", tier 5
// If no trailing _NN, tier=0 and family is the full basename.
// @param path        affix DBR path
// @param out_family  receives newly-allocated family string (caller must free)
// @param out_tier    receives integer tier value
static void
extract_affix_family(const char *path, char **out_family, int *out_tier)
{
  *out_family = NULL;
  *out_tier = 0;

  if(!path)
    return;

  const char *last_sep = strrchr(path, '\\');

  if(!last_sep)
    last_sep = strrchr(path, '/');

  const char *name = last_sep ? last_sep + 1 : path;
  char *copy = strdup(name);

  if(!copy)
    return;

  // Strip .dbr extension
  char *dot = strrchr(copy, '.');

  if(dot)
    *dot = '\0';

  // Check for trailing _NN (1-3 digits)
  char *last_us = strrchr(copy, '_');

  if(last_us && last_us[1] >= '0' && last_us[1] <= '9')
  {
    char *endp;
    long tier = strtol(last_us + 1, &endp, 10);

    if(*endp == '\0')
    {
      *out_tier = (int)tier;
      *last_us = '\0';  // truncate the _NN
    }
  }

  // Replace underscores with spaces, lowercase
  for(char *p = copy; *p; p++)
  {
    if(*p == '_')
      *p = ' ';

    if(*p >= 'A' && *p <= 'Z')
      *p += 32;
  }

  *out_family = copy;
}


// Split a stat summary string into category (names only) and compact values.
// E.g. "+33 Strength, +12% Dexterity" -> category="Strength, Dexterity", values="+33 / +12%"
// Handles "Pets: " prefix.
// @param summary       input stat summary string
// @param out_category  receives newly-allocated category string (caller must free)
// @param out_values    receives newly-allocated values string (caller must free)
static void
split_stat_summary(const char *summary, char **out_category, char **out_values)
{
  *out_category = NULL;
  *out_values = NULL;

  if(!summary || !summary[0])
    return;

  const char *prefix = "";
  const char *s = summary;

  if(strncasecmp(s, "Pets: ", 6) == 0)
  {
    prefix = "Pets: ";
    s += 6;
  }

  char names[3][128];
  char vals[3][128];
  int n = 0;

  while(*s && n < 3)
  {
    const char *comma = strstr(s, ", ");
    size_t partlen = comma ? (size_t)(comma - s) : strlen(s);
    char part[128];
    size_t clen = partlen < sizeof(part) - 1 ? partlen : sizeof(part) - 1;

    memcpy(part, s, clen);
    part[clen] = '\0';

    // Split "value name" at first space followed by alpha
    const char *p = part;

    while(*p)
    {
      if(*p == ' ' && p[1] && ((p[1] >= 'A' && p[1] <= 'Z') ||
                                 (p[1] >= 'a' && p[1] <= 'z')))
      {
        size_t vn = (size_t)(p - part);

        if(vn >= sizeof(vals[0]))
          vn = sizeof(vals[0]) - 1;

        memcpy(vals[n], part, vn);
        vals[n][vn] = '\0';
        snprintf(names[n], sizeof(names[0]), "%s", p + 1);
        break;
      }
      p++;
    }

    if(!*p)
    {
      // No split found -- entire part is the value
      snprintf(vals[n], sizeof(vals[0]), "%s", part);
      names[n][0] = '\0';
    }

    n++;

    if(comma)
      s = comma + 2;
    else
      break;
  }

  // Build category (deduplicated names)
  char cat[256];

  cat[0] = '\0';

  for(int i = 0; i < n; i++)
  {
    if(!names[i][0])
      continue;

    bool dup = false;

    for(int j = 0; j < i; j++)
      if(strcasecmp(names[i], names[j]) == 0)
      {
        dup = true;
        break;
      }

    if(dup)
      continue;

    if(cat[0])
    {
      size_t cl = strlen(cat);

      if(cl < sizeof(cat) - 3)
      {
        cat[cl] = ',';
        cat[cl+1] = ' ';
        cat[cl+2] = '\0';
      }
    }

    size_t cl = strlen(cat), nl = strlen(names[i]);

    if(cl + nl < sizeof(cat) - 1)
      memcpy(cat + cl, names[i], nl + 1);
  }

  // Build compact values
  char valstr[128];

  valstr[0] = '\0';

  for(int i = 0; i < n; i++)
  {
    if(i > 0)
    {
      size_t vl = strlen(valstr);

      if(vl < sizeof(valstr) - 4)
        memcpy(valstr + vl, " / ", 4);
    }

    size_t vl = strlen(valstr), pl = strlen(vals[i]);

    if(vl + pl < sizeof(valstr) - 1)
      memcpy(valstr + vl, vals[i], pl + 1);
  }

  if(prefix[0])
  {
    char tmp[256];

    snprintf(tmp, sizeof(tmp), "%s%s", prefix, cat);
    *out_category = strdup(tmp);
  }
  else
    *out_category = strdup(cat);

  *out_values = strdup(valstr);
}

// -- Free helpers --

// Free an AffixTableList and all its pairs.
// @param data  gpointer to AffixTableList (for GHashTable destroy callback)
static void
affix_table_list_free(gpointer data)
{
  AffixTableList *list = data;

  if(!list)
    return;

  for(int i = 0; i < list->count; i++)
  {
    free(list->pairs[i].prefix_table);
    free(list->pairs[i].suffix_table);
  }

  free(list->pairs);
  free(list);
}

// GHashTable value destroy callback that delegates to affix_result_free.
// @param data  gpointer to TQItemAffixes
static void
affix_result_free_internal(gpointer data)
{
  affix_result_free((TQItemAffixes *)data);
}

// -- Phase A: Build item->table map --

// Process a single LootItemTable record (FixedWeight or DynWeight),
// extracting item paths and their associated prefix/suffix randomizer tables.
// @param dbr         decompressed DBR record data
// @param class_name  the Class field value (FixedWeight or DynWeight)
static void
process_loot_item_table(TQArzRecordData *dbr, const char *class_name)
{
  // Collect loot names (item paths this table applies to)
  char *loot_names[64];
  int num_loot_names = 0;

  bool is_fixed = (strcasecmp(class_name, "LootItemTable_FixedWeight") == 0);

  if(is_fixed)
  {
    // FixedWeight: individual lootName, lootName01, lootName02, ... fields
    for(uint32_t i = 0; i < dbr->num_vars; i++)
    {
      if(!dbr->vars[i].name)
        continue;

      if(strncasecmp(dbr->vars[i].name, "lootName", 8) == 0 &&
         dbr->vars[i].type == TQ_VAR_STRING && dbr->vars[i].count > 0)
      {
        const char *val = dbr->vars[i].value.str[0];

        if(val && val[0] && num_loot_names < 64)
          loot_names[num_loot_names++] = strdup(val);
      }
    }
  }
  else
  {
    // DynWeight: itemNames array field
    for(uint32_t i = 0; i < dbr->num_vars; i++)
    {
      if(!dbr->vars[i].name)
        continue;

      if(strcasecmp(dbr->vars[i].name, "itemNames") == 0 &&
         dbr->vars[i].type == TQ_VAR_STRING)
      {
        for(uint32_t j = 0; j < dbr->vars[i].count && num_loot_names < 64; j++)
        {
          const char *val = dbr->vars[i].value.str[j];

          if(val && val[0])
            loot_names[num_loot_names++] = strdup(val);
        }
        break;
      }
    }
  }

  if(num_loot_names == 0)
    return;

  // Collect prefix/suffix randomizer table paths, grouped by numeric suffix.
  // Variables look like: prefixRandomizerName, prefixRandomizerName01, ...
  GHashTable *table_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  for(uint32_t i = 0; i < dbr->num_vars; i++)
  {
    if(!dbr->vars[i].name || dbr->vars[i].type != TQ_VAR_STRING ||
       dbr->vars[i].count == 0 || !dbr->vars[i].value.str[0] ||
       !dbr->vars[i].value.str[0][0])
      continue;

    const char *vname = dbr->vars[i].name;
    const char *val = dbr->vars[i].value.str[0];
    const char *num_suffix = NULL;
    bool is_prefix = false, is_suffix = false;

    if(strncasecmp(vname, "prefixRandomizerName", 20) == 0)
    {
      num_suffix = vname + 20;
      is_prefix = true;
    }
    else if(strncasecmp(vname, "suffixRandomizerName", 20) == 0)
    {
      num_suffix = vname + 20;
      is_suffix = true;
    }

    if(!is_prefix && !is_suffix)
      continue;

    // Use the numeric suffix as group key (empty string for base)
    char *key = g_strdup(num_suffix);

    // Two strings stored as: prefix_table\0suffix_table\0 in a struct
    typedef struct { char *prefix; char *suffix; } PairBuf;
    PairBuf *pair = g_hash_table_lookup(table_groups, key);

    if(!pair)
    {
      pair = g_new0(PairBuf, 1);
      g_hash_table_insert(table_groups, key, pair);
    }
    else
      g_free(key);

    if(is_prefix)
    {
      free(pair->prefix);
      pair->prefix = strdup(val);
    }

    if(is_suffix)
    {
      free(pair->suffix);
      pair->suffix = strdup(val);
    }
  }

  // Convert group hash to AffixTablePair array
  int num_pairs = g_hash_table_size(table_groups);

  if(num_pairs == 0)
  {
    g_hash_table_destroy(table_groups);

    for(int i = 0; i < num_loot_names; i++)
      free(loot_names[i]);

    return;
  }

  AffixTablePair *pairs = calloc(num_pairs, sizeof(AffixTablePair));

  if(!pairs)
  {
    g_hash_table_destroy(table_groups);

    for(int i = 0; i < num_loot_names; i++)
      free(loot_names[i]);

    return;
  }

  GHashTableIter iter;
  gpointer gkey, gval;
  int pair_idx = 0;

  g_hash_table_iter_init(&iter, table_groups);

  while(g_hash_table_iter_next(&iter, &gkey, &gval))
  {
    typedef struct { char *prefix; char *suffix; } PairBuf;
    PairBuf *pb = gval;

    pairs[pair_idx].prefix_table = pb->prefix;  // transfer ownership
    pairs[pair_idx].suffix_table = pb->suffix;
    pb->prefix = NULL;
    pb->suffix = NULL;
    pair_idx++;
  }

  // Don't free pair strings -- ownership transferred
  g_hash_table_destroy(table_groups);

  // Insert into global map for each loot name
  for(int i = 0; i < num_loot_names; i++)
  {
    char *norm_key = normalize_path(loot_names[i]);

    free(loot_names[i]);

    AffixTableList *existing = g_hash_table_lookup(g_affix_map, norm_key);

    if(existing)
    {
      // Append pairs to existing list
      int new_count = existing->count + num_pairs;
      AffixTablePair *new_pairs = realloc(existing->pairs, new_count * sizeof(AffixTablePair));

      if(!new_pairs)
      {
        g_free(norm_key);
        continue;
      }

      existing->pairs = new_pairs;

      for(int j = 0; j < num_pairs; j++)
      {
        existing->pairs[existing->count + j].prefix_table =
          pairs[j].prefix_table ? strdup(pairs[j].prefix_table) : NULL;
        existing->pairs[existing->count + j].suffix_table =
          pairs[j].suffix_table ? strdup(pairs[j].suffix_table) : NULL;
      }

      existing->count = new_count;
      g_free(norm_key);
    }
    else
    {
      AffixTableList *list = g_new0(AffixTableList, 1);

      if(i == num_loot_names - 1)
      {
        // Last item: transfer ownership of pairs array
        list->pairs = pairs;
        list->count = num_pairs;
        pairs = NULL;  // prevent double-free below
      }
      else
      {
        // Duplicate pairs for earlier items
        list->pairs = calloc(num_pairs, sizeof(AffixTablePair));

        if(!list->pairs)
        {
          g_free(list);
          g_free(norm_key);
          continue;
        }

        for(int j = 0; j < num_pairs; j++)
        {
          list->pairs[j].prefix_table =
            pairs[j].prefix_table ? strdup(pairs[j].prefix_table) : NULL;
          list->pairs[j].suffix_table =
            pairs[j].suffix_table ? strdup(pairs[j].suffix_table) : NULL;
        }

        list->count = num_pairs;
      }

      g_hash_table_insert(g_affix_map, norm_key, list);
    }
  }

  // Free pairs array if not transferred
  if(pairs)
  {
    for(int j = 0; j < num_pairs; j++)
    {
      free(pairs[j].prefix_table);
      free(pairs[j].suffix_table);
    }

    free(pairs);
  }
}

// Initialize the global affix table by scanning all LootItemTable records
// across all loaded .arz files. Call once after asset_manager_init().
// @param tr  translation table (unused at init time, reserved for future use)
void
affix_table_init(TQTranslation *tr)
{
  (void)tr;  // tr not needed at init time, only at resolve time

  if(g_affix_map)
    return;  // already initialized

  struct timespec t0;

  clock_gettime(CLOCK_MONOTONIC, &t0);

  g_affix_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, affix_table_list_free);
  g_affix_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, affix_result_free_internal);

  int num_files = asset_get_num_files();
  int records_scanned = 0;
  int tables_found = 0;

  for(int fid = 0; fid < num_files; fid++)
  {
    const char *fpath = asset_get_file_path((uint16_t)fid);

    if(!fpath)
      continue;

    // Only process .arz files
    const char *ext = strrchr(fpath, '.');

    if(!ext || strcasecmp(ext, ".arz") != 0)
      continue;

    TQArzFile *arz = asset_get_arz((uint16_t)fid);

    if(!arz)
      continue;

    for(uint32_t ri = 0; ri < arz->num_records; ri++)
    {
      const char *rpath = arz->records[ri].path;

      if(!rpath)
        continue;

      // Process LootItemTable records
      if(!path_contains_ci(rpath, "loottables") &&
         !path_contains_ci(rpath, "merchantloottables"))
        continue;

      records_scanned++;

      // Decompress and check Class field
      TQArzRecordData *dbr = arz_read_record_at(arz,
        arz->records[ri].offset, arz->records[ri].compressed_size);

      if(!dbr)
        continue;

      char *class_name_str = arz_record_get_string(dbr, "Class", NULL);

      if(class_name_str &&
         (strcasecmp(class_name_str, "LootItemTable_FixedWeight") == 0 ||
          strcasecmp(class_name_str, "LootItemTable_DynWeight") == 0))
      {
        process_loot_item_table(dbr, class_name_str);
        tables_found++;
      }

      free(class_name_str);
      arz_record_data_free(dbr);
    }
  }

  struct timespec t1;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

  if(tqvc_debug)
    fprintf(stderr, "Affix table init: scanned %d loot records, found %d tables, "
            "%d items mapped in %.1f ms\n",
            records_scanned, tables_found,
            g_hash_table_size(g_affix_map), ms);
}

// -- Phase B: Resolve affix list on demand --

// Compare two TQAffixEntry structs for qsort: by family, then tier, then translation.
// @param a  pointer to first TQAffixEntry
// @param b  pointer to second TQAffixEntry
// @return negative/zero/positive per strcmp convention
static int
compare_affix_entries(const void *a, const void *b)
{
  const TQAffixEntry *ea = a;
  const TQAffixEntry *eb = b;

  // Primary: effect_family (alphabetical)
  const char *fa = ea->effect_family ? ea->effect_family : "";
  const char *fb = eb->effect_family ? eb->effect_family : "";
  int cmp = strcasecmp(fa, fb);

  if(cmp != 0)
    return(cmp);

  // Secondary: tier (ascending)
  if(ea->tier != eb->tier)
    return(ea->tier - eb->tier);

  // Tertiary: translation
  return(strcasecmp(ea->translation, eb->translation));
}

// Resolve a single randomizer table DBR to a list of affix entries,
// appending results to the provided output arrays.
// @param table_path   path to the LootRandomizerTable DBR
// @param tr           translation table for resolving affix display names
// @param out_entries  pointer to entry array (may be reallocated)
// @param out_count    pointer to current entry count (updated on return)
static void
resolve_randomizer_table(const char *table_path, TQTranslation *tr,
                         TQAffixEntry **out_entries, int *out_count)
{
  if(!table_path || !table_path[0])
    return;

  TQArzRecordData *dbr = asset_get_dbr(table_path);

  if(!dbr)
    return;

  // Collect randomizerName[N] / randomizerWeight[N] pairs
  GHashTable *pairs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  typedef struct { char *path; float weight; } RandPair;

  for(uint32_t i = 0; i < dbr->num_vars; i++)
  {
    if(!dbr->vars[i].name)
      continue;

    const char *vname = dbr->vars[i].name;

    if(strncasecmp(vname, "randomizerName", 14) == 0 &&
       dbr->vars[i].type == TQ_VAR_STRING && dbr->vars[i].count > 0)
    {
      const char *val = dbr->vars[i].value.str[0];

      if(!val || !val[0])
        continue;

      const char *num = vname + 14;
      char *key = g_strdup(num);
      RandPair *rp = g_hash_table_lookup(pairs, key);

      if(!rp)
      {
        rp = g_new0(RandPair, 1);
        g_hash_table_insert(pairs, key, rp);
      }
      else
        g_free(key);

      free(rp->path);
      rp->path = strdup(val);
    }
    else if(strncasecmp(vname, "randomizerWeight", 16) == 0)
    {
      const char *num = vname + 16;
      float w = 0;

      if(dbr->vars[i].type == TQ_VAR_INT && dbr->vars[i].count > 0)
        w = (float)dbr->vars[i].value.i32[0];
      else if(dbr->vars[i].type == TQ_VAR_FLOAT && dbr->vars[i].count > 0)
        w = dbr->vars[i].value.f32[0];

      if(w <= 0)
        continue;

      char *key = g_strdup(num);
      RandPair *rp = g_hash_table_lookup(pairs, key);

      if(!rp)
      {
        rp = g_new0(RandPair, 1);
        g_hash_table_insert(pairs, key, rp);
      }
      else
        g_free(key);

      rp->weight = w;
    }
  }

  // Convert valid pairs to affix entries
  int capacity = g_hash_table_size(pairs);
  TQAffixEntry *entries = *out_entries;
  int count = *out_count;

  entries = realloc(entries, (count + capacity) * sizeof(TQAffixEntry));

  if(!entries)
  {
    g_hash_table_destroy(pairs);
    return;
  }

  GHashTableIter iter;
  gpointer gkey, gval;

  g_hash_table_iter_init(&iter, pairs);

  while(g_hash_table_iter_next(&iter, &gkey, &gval))
  {
    RandPair *rp = gval;

    if(!rp->path || rp->weight <= 0)
    {
      free(rp->path);
      rp->path = NULL;
      continue;
    }

    // Check if this affix path already exists (deduplicate, sum weights)
    bool found_dup = false;

    for(int j = 0; j < count; j++)
    {
      if(strcasecmp(entries[j].affix_path, rp->path) == 0)
      {
        entries[j].weight += rp->weight;
        found_dup = true;
        break;
      }
    }

    if(found_dup)
    {
      free(rp->path);
      rp->path = NULL;
      continue;
    }

    // Resolve translation
    char *translation = NULL;
    TQArzRecordData *affix_dbr = asset_get_dbr(rp->path);

    if(affix_dbr)
    {
      char *tag = arz_record_get_string(affix_dbr, "lootRandomizerName", NULL);

      if(tag && tag[0] && tr)
      {
        const char *trans = translation_get(tr, tag);

        if(trans && trans[0])
          translation = strdup(trans);
      }

      free(tag);

      if(!translation)
      {
        char *fdesc = arz_record_get_string(affix_dbr, "FileDescription", NULL);

        if(fdesc && fdesc[0])
          translation = fdesc;
        else
          free(fdesc);
      }
    }

    if(!translation)
      translation = pretty_filename(rp->path);

    // Extract family/tier from affix path
    char *family = NULL;
    int tier = 0;

    extract_affix_family(rp->path, &family, &tier);

    // Build stat summary from the affix DBR
    char *summary = item_bonus_stat_summary(rp->path);

    if(!summary)
      summary = family ? strdup(family) : pretty_filename(rp->path);

    // Split summary into category (names) and compact values
    char *category = NULL, *values = NULL;

    split_stat_summary(summary, &category, &values);

    entries[count].affix_path = rp->path;
    entries[count].translation = translation;
    entries[count].weight = rp->weight;
    entries[count].effect_family = family;
    entries[count].tier = tier;
    entries[count].stat_summary = summary;
    entries[count].stat_category = category;
    entries[count].stat_values = values;
    rp->path = NULL;  // ownership transferred
    count++;
  }

  // Clean up any remaining rp->path that weren't transferred
  g_hash_table_iter_init(&iter, pairs);

  while(g_hash_table_iter_next(&iter, &gkey, &gval))
  {
    RandPair *rp = gval;

    free(rp->path);
    rp->path = NULL;
  }

  g_hash_table_destroy(pairs);

  *out_entries = entries;
  *out_count = count;
}

// Look up valid prefixes and suffixes for an item by its base_name.
// Returns cached results on subsequent calls for the same item.
// @param item_base_name  DBR path to the item record
// @param tr              translation table for resolving affix display names
// @return pointer to TQItemAffixes (owned by cache, do not free), or NULL
TQItemAffixes *
affix_table_get(const char *item_base_name, TQTranslation *tr)
{
  if(!item_base_name || !g_affix_map)
    return(NULL);

  char *norm = normalize_path(item_base_name);

  // Check cache
  TQItemAffixes *cached = g_hash_table_lookup(g_affix_cache, norm);

  if(cached)
  {
    g_free(norm);
    return(cached);
  }

  // Look up affix table list
  AffixTableList *tbl = g_hash_table_lookup(g_affix_map, norm);

  if(!tbl || tbl->count == 0)
  {
    g_free(norm);
    return(NULL);
  }

  TQItemAffixes *result = calloc(1, sizeof(TQItemAffixes));

  if(!result)
  {
    g_free(norm);
    return(NULL);
  }

  // Track which randomizer tables we've already resolved (avoid duplicates)
  GHashTable *resolved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  // Resolve prefix and suffix tables from direct loot table references
  for(int i = 0; i < tbl->count; i++)
  {
    const char *tables[2] = { tbl->pairs[i].prefix_table, tbl->pairs[i].suffix_table };
    TQAffixEntry **lists[2] = { &result->prefixes.entries, &result->suffixes.entries };
    int *counts[2] = { &result->prefixes.count, &result->suffixes.count };

    for(int t = 0; t < 2; t++)
    {
      if(!tables[t] || !tables[t][0])
        continue;

      char *tnorm = normalize_path(tables[t]);

      if(!g_hash_table_contains(resolved, tnorm))
      {
        g_hash_table_add(resolved, tnorm);
        resolve_randomizer_table(tables[t], tr, lists[t], counts[t]);
      }
      else
        g_free(tnorm);
    }
  }

  g_hash_table_destroy(resolved);

  // Sort by translation
  if(result->prefixes.count > 0)
    qsort(result->prefixes.entries, result->prefixes.count,
          sizeof(TQAffixEntry), compare_affix_entries);

  if(result->suffixes.count > 0)
    qsort(result->suffixes.entries, result->suffixes.count,
          sizeof(TQAffixEntry), compare_affix_entries);

  // If both are empty, return NULL
  if(result->prefixes.count == 0 && result->suffixes.count == 0)
  {
    free(result);
    g_free(norm);
    return(NULL);
  }

  // Cache the result (cache takes ownership of norm key)
  g_hash_table_insert(g_affix_cache, norm, result);
  return(result);
}

// -- Eligibility check --

// Check whether an item supports affix modification (non-Epic/Legendary equipment).
// @param base_name  DBR path to the item record
// @return true if the item is moddable equipment
bool
item_can_modify_affixes(const char *base_name)
{
  if(!base_name || !base_name[0])
    return(false);

  TQArzRecordData *dbr = asset_get_dbr(base_name);

  if(!dbr)
    return(false);

  // Check classification: Epic and Legendary are non-moddable
  char *classification = arz_record_get_string(dbr, "itemClassification", NULL);

  if(classification)
  {
    if(strcasecmp(classification, "Epic") == 0 ||
       strcasecmp(classification, "Legendary") == 0)
    {
      free(classification);
      return(false);
    }

    free(classification);
  }

  // Check Class field: must be equipment (armor/weapon/shield/jewelry)
  char *class_name = arz_record_get_string(dbr, "Class", NULL);

  if(!class_name)
    return(false);

  bool is_equipment =
    strstr(class_name, "UpperBody") ||
    strstr(class_name, "LowerBody") ||
    strstr(class_name, "Head") ||
    strstr(class_name, "Forearm") ||
    strstr(class_name, "WeaponMelee") ||
    strstr(class_name, "WeaponHunting") ||
    strstr(class_name, "WeaponMagical") ||
    strstr(class_name, "Shield") ||
    strstr(class_name, "Amulet") ||
    strstr(class_name, "Ring");

  free(class_name);
  return(is_equipment);
}

// -- Forge eligibility check --

// Check whether an Epic/Legendary item can have forge affixes applied.
// @param base_name  DBR path to the item record
// @return true if the item is Epic/Legendary equipment eligible for forging
bool
item_can_forge_affixes(const char *base_name)
{
  if(!base_name || !base_name[0])
    return(false);

  TQArzRecordData *dbr = asset_get_dbr(base_name);

  if(!dbr)
    return(false);

  // Must be Epic or Legendary
  char *classification = arz_record_get_string(dbr, "itemClassification", NULL);

  if(!classification)
    return(false);

  bool is_epic_or_leg = (strcasecmp(classification, "Epic") == 0 ||
                         strcasecmp(classification, "Legendary") == 0);

  free(classification);

  if(!is_epic_or_leg)
    return(false);

  // Must be equipment
  char *class_name = arz_record_get_string(dbr, "Class", NULL);

  if(!class_name)
    return(false);

  bool is_equipment =
    strstr(class_name, "UpperBody") ||
    strstr(class_name, "LowerBody") ||
    strstr(class_name, "Head") ||
    strstr(class_name, "Forearm") ||
    strstr(class_name, "WeaponMelee") ||
    strstr(class_name, "WeaponHunting") ||
    strstr(class_name, "WeaponMagical") ||
    strstr(class_name, "Shield") ||
    strstr(class_name, "Amulet") ||
    strstr(class_name, "Ring");

  free(class_name);
  return(is_equipment);
}

// -- Dvergr Forge affix resolution --

// Read a float variable from a DBR record by name (linear scan).
// @param dbr       decompressed record data
// @param var_name  variable name to look up
// @return float value, or 0 if not found
static float
dbr_get_float_simple(TQArzRecordData *dbr, const char *var_name)
{
  if(!dbr)
    return(0);

  for(uint32_t i = 0; i < dbr->num_vars; i++)
  {
    if(!dbr->vars[i].name)
      continue;

    if(strcasecmp(dbr->vars[i].name, var_name) != 0)
      continue;

    if(dbr->vars[i].type == TQ_VAR_FLOAT && dbr->vars[i].count > 0 &&
       dbr->vars[i].value.f32)
      return(dbr->vars[i].value.f32[0]);

    if(dbr->vars[i].type == TQ_VAR_INT && dbr->vars[i].count > 0 &&
       dbr->vars[i].value.i32)
      return((float)dbr->vars[i].value.i32[0]);

    return(0);
  }

  return(0);
}

// Read a string variable from a DBR (returns internal pointer, do NOT free).
// @param dbr       decompressed record data
// @param var_name  variable name to look up
// @return internal string pointer, or NULL if not found
static const char *
dbr_get_string_internal(TQArzRecordData *dbr, const char *var_name)
{
  if(!dbr)
    return(NULL);

  for(uint32_t i = 0; i < dbr->num_vars; i++)
  {
    if(!dbr->vars[i].name)
      continue;

    if(strcasecmp(dbr->vars[i].name, var_name) != 0)
      continue;

    if(dbr->vars[i].type == TQ_VAR_STRING && dbr->vars[i].count > 0)
      return(dbr->vars[i].value.str[0]);

    return(NULL);
  }

  return(NULL);
}

// Get forge-specific affixes for an Epic/Legendary item from the Dvergr
// Master Forge (Durin Upgrader) tables.
// @param item_base_name  DBR path to the item record
// @param tr              translation table for resolving affix display names
// @return newly-allocated TQItemAffixes (caller must free), or NULL
TQItemAffixes *
affix_table_get_forge(const char *item_base_name, TQTranslation *tr)
{
  if(!item_base_name)
    return(NULL);

  // Load the item's DBR to determine category
  TQArzRecordData *item_dbr = asset_get_dbr(item_base_name);

  if(!item_dbr)
    return(NULL);

  char *class_name = arz_record_get_string(item_dbr, "Class", NULL);

  if(!class_name)
    return(NULL);

  // Determine Defensive vs Offensive
  bool is_defensive =
    strstr(class_name, "UpperBody") || strstr(class_name, "LowerBody") ||
    strstr(class_name, "Head") || strstr(class_name, "Forearm") ||
    strstr(class_name, "Shield") || strstr(class_name, "Amulet") ||
    strstr(class_name, "Ring");
  bool is_offensive =
    strstr(class_name, "WeaponMelee") || strstr(class_name, "WeaponHunting") ||
    strstr(class_name, "WeaponMagical");

  if(!is_defensive && !is_offensive)
  {
    free(class_name);
    return(NULL);
  }

  // Determine Int vs Str from stat requirements
  float int_req = dbr_get_float_simple(item_dbr, "intelligenceRequirement");
  float str_req = dbr_get_float_simple(item_dbr, "strengthRequirement");
  float dex_req = dbr_get_float_simple(item_dbr, "dexterityRequirement");

  bool is_int;

  if(int_req > 0 && int_req > (str_req + dex_req))
    is_int = true;
  else if(str_req > 0 || dex_req > 0)
    is_int = false;
  else
    // Fallback by Class: staves are Int, everything else is Str
    is_int = (strstr(class_name, "WeaponMagical_Staff") != NULL);

  free(class_name);

  // Load Durin Upgrader NPC record
  static const char *DURIN_PATH =
    "records/xpack2/creatures/npc/dvergr/speaking/durin_upgrader.dbr";
  TQArzRecordData *durin = asset_get_dbr(DURIN_PATH);

  if(!durin)
    return(NULL);

  // Build the 6 variable names we need (3 prefix + 3 suffix tiers)
  const char *stat_str = is_int ? "Int" : "Str";
  const char *slot_str = is_defensive ? "Defensive" : "Offensive";
  static const char * const tiers[] = { "Normal", "Epic", "Legendary" };

  TQItemAffixes *result = calloc(1, sizeof(TQItemAffixes));

  if(!result)
    return(NULL);

  for(int t = 0; t < 3; t++)
  {
    // Prefix table variable: e.g. "ItemTable_IntDefensivePrefixNormal"
    char prefix_var[80], suffix_var[80];

    snprintf(prefix_var, sizeof(prefix_var), "ItemTable_%s%sPrefix%s",
             stat_str, slot_str, tiers[t]);
    snprintf(suffix_var, sizeof(suffix_var), "ItemTable_%s%sSufix%s",
             stat_str, slot_str, tiers[t]);

    const char *prefix_table = dbr_get_string_internal(durin, prefix_var);
    const char *suffix_table = dbr_get_string_internal(durin, suffix_var);

    if(prefix_table && prefix_table[0])
      resolve_randomizer_table(prefix_table, tr,
                               &result->prefixes.entries, &result->prefixes.count);

    if(suffix_table && suffix_table[0])
      resolve_randomizer_table(suffix_table, tr,
                               &result->suffixes.entries, &result->suffixes.count);
  }

  // Sort by translation
  if(result->prefixes.count > 0)
    qsort(result->prefixes.entries, result->prefixes.count,
          sizeof(TQAffixEntry), compare_affix_entries);

  if(result->suffixes.count > 0)
    qsort(result->suffixes.entries, result->suffixes.count,
          sizeof(TQAffixEntry), compare_affix_entries);

  if(result->prefixes.count == 0 && result->suffixes.count == 0)
  {
    free(result);
    return(NULL);
  }

  return(result);
}

// -- Cleanup --

// Free a TQItemAffixes structure and all its entries.
// @param affixes  structure to free (NULL-safe)
void
affix_result_free(TQItemAffixes *affixes)
{
  if(!affixes)
    return;

  for(int i = 0; i < affixes->prefixes.count; i++)
  {
    free(affixes->prefixes.entries[i].affix_path);
    free(affixes->prefixes.entries[i].translation);
    free(affixes->prefixes.entries[i].effect_family);
    free(affixes->prefixes.entries[i].stat_summary);
    free(affixes->prefixes.entries[i].stat_category);
    free(affixes->prefixes.entries[i].stat_values);
  }

  free(affixes->prefixes.entries);

  for(int i = 0; i < affixes->suffixes.count; i++)
  {
    free(affixes->suffixes.entries[i].affix_path);
    free(affixes->suffixes.entries[i].translation);
    free(affixes->suffixes.entries[i].effect_family);
    free(affixes->suffixes.entries[i].stat_summary);
    free(affixes->suffixes.entries[i].stat_category);
    free(affixes->suffixes.entries[i].stat_values);
  }

  free(affixes->suffixes.entries);
  free(affixes);
}

// Destroy the global affix map and cache, freeing all memory.
void
affix_table_free(void)
{
  if(g_affix_cache)
  {
    g_hash_table_destroy(g_affix_cache);
    g_affix_cache = NULL;
  }

  if(g_affix_map)
  {
    g_hash_table_destroy(g_affix_map);
    g_affix_map = NULL;
  }
}
