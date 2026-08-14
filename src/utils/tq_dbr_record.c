// tq-dbr-tool: generic ARZ record queries -- dump, search, fields, stats
// and per-class variable coverage.

#include "tq_dbr_tool.h"

// Dumps all variables from a single ARZ record.
// arz_path: path to the .arz database file.
// record_path: DBR record path within the database.
// Returns 0 on success, 1 on failure.
int
cmd_dump(const char *arz_path, const char *record_path)
{
  TQArzFile *arz = arz_load(arz_path);
  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  TQArzRecordData *data = arz_read_record(arz, record_path);
  if(!data)
  {
    fprintf(stderr, "Record not found: %s\n", record_path);
    arz_free(arz);
    return(1);
  }

  printf("Record: %s (%u variables)\n", record_path, data->num_vars);

  for(uint32_t i = 0; i < data->num_vars; i++)
    print_variable(&data->vars[i]);

  arz_record_data_free(data);
  arz_free(arz);
  return(0);
}

// Lists all records whose path contains the given substring (case-insensitive).
// arz_path: path to the .arz database file.
// pattern: substring to match against record paths.
// Returns 0 on success, 1 on failure.
int
cmd_search(const char *arz_path, const char *pattern)
{
  TQArzFile *arz = arz_load(arz_path);
  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  // Case-insensitive substring search, normalize / to backslash
  char *norm_pattern = normalize_path(pattern);
  int count = 0;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(!arz->records[i].path)
      continue;

    char *lower_path = g_ascii_strdown(arz->records[i].path, -1);

    if(strstr(lower_path, norm_pattern))
    {
      printf("%s\n", arz->records[i].path);
      count++;
    }

    g_free(lower_path);
  }

  printf("\n%d records matched.\n", count);

  g_free(norm_pattern);
  arz_free(arz);
  return(0);
}

// Shows specific fields for all records matching a path substring.
// arz_path: path to the .arz database file.
// pattern: substring to match against record paths.
// field_list: comma-separated list of field names to display.
// Returns 0 on success, 1 on failure.
int
cmd_fields(const char *arz_path, const char *pattern, const char *field_list)
{
  TQArzFile *arz = arz_load(arz_path);
  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  // Parse comma-separated field names
  char *fields_copy = strdup(field_list);
  if(!fields_copy)
  {
    fprintf(stderr, "Out of memory\n");
    arz_free(arz);
    return(1);
  }

  char *field_names[64];
  int num_fields = 0;
  char *save = NULL;
  char *tok = strtok_r(fields_copy, ",", &save);

  while(tok && num_fields < 64)
  {
    field_names[num_fields++] = tok;
    tok = strtok_r(NULL, ",", &save);
  }

  char *lower_pattern = normalize_path(pattern);
  int count = 0;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(!arz->records[i].path)
      continue;

    char *lower_path = g_ascii_strdown(arz->records[i].path, -1);

    if(!strstr(lower_path, lower_pattern))
    {
      g_free(lower_path);
      continue;
    }

    g_free(lower_path);

    TQArzRecordData *data = arz_read_record(arz, arz->records[i].path);
    if(!data)
      continue;

    printf("--- %s\n", arz->records[i].path);

    for(int f = 0; f < num_fields; f++)
    {
      bool found_field = false;

      for(uint32_t v = 0; v < data->num_vars; v++)
      {
        if(strcasecmp(data->vars[v].name, field_names[f]) == 0)
        {
          print_variable(&data->vars[v]);
          found_field = true;
          break;
        }
      }

      if(!found_field)
        printf("  %-40s (not present)\n", field_names[f]);
    }

    count++;
    arz_record_data_free(data);
  }

  printf("\n%d records matched.\n", count);

  free(fields_copy);
  g_free(lower_pattern);
  arz_free(arz);
  return(0);
}

// Shows non-zero numeric variables for all records matching a path substring.
// arz_path: path to the .arz database file.
// pattern: substring to match against record paths.
// Returns 0 on success, 1 on failure.
int
cmd_stats(const char *arz_path, const char *pattern)
{
  TQArzFile *arz = arz_load(arz_path);
  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  char *lower_pattern = normalize_path(pattern);
  int count = 0;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(!arz->records[i].path)
      continue;

    char *lower_path = g_ascii_strdown(arz->records[i].path, -1);

    if(!strstr(lower_path, lower_pattern))
    {
      g_free(lower_path);
      continue;
    }

    g_free(lower_path);

    TQArzRecordData *data = arz_read_record(arz, arz->records[i].path);
    if(!data)
      continue;

    bool header_printed = false;

    for(uint32_t v = 0; v < data->num_vars; v++)
    {
      TQVariable *var = &data->vars[v];
      bool has_value = false;

      if(var->type == TQ_VAR_INT && var->value.i32)
      {
        for(uint32_t j = 0; j < var->count; j++)
          if(var->value.i32[j] != 0)
          {
            has_value = true;
            break;
          }
      }
      else if(var->type == TQ_VAR_FLOAT && var->value.f32)
      {
        for(uint32_t j = 0; j < var->count; j++)
          if(fabsf(var->value.f32[j]) > 0.0001f)
          {
            has_value = true;
            break;
          }
      }
      else if(var->type == TQ_VAR_STRING && var->value.str)
      {
        for(uint32_t j = 0; j < var->count; j++)
          if(var->value.str[j] && var->value.str[j][0])
          {
            has_value = true;
            break;
          }
      }

      if(has_value)
      {
        if(!header_printed)
        {
          printf("--- %s\n", arz->records[i].path);
          header_printed = true;
        }

        print_variable(var);
      }
    }

    if(header_printed)
      count++;

    arz_record_data_free(data);
  }

  printf("\n%d records with non-zero values.\n", count);

  g_free(lower_pattern);
  arz_free(arz);
  return(0);
}

// Walks every item-class record in the database, collects all variable
// names with at least one non-zero / non-null value, and prints a sorted
// summary (var_name <tab> record_count <tab> sample_value).  Used to find
// stat fields the tooltip code does not yet render.
//
// arz_path: path to the .arz database file.
// path_substr: only count records whose path contains this substring
//              (use "" to match all records, or "item" for items).
// Returns 0 on success, 1 on failure.
typedef struct {
  const char *name;
  uint32_t count;
  char sample[64];
} CovStat;

static int
cov_cmp(const void *a, const void *b)
{
  return(strcmp(((const CovStat *)a)->name, ((const CovStat *)b)->name));
}

int
cmd_coverage(const char *arz_path, const char *path_substr)
{
  TQArzFile *arz = arz_load(arz_path);
  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  char *lower_pattern = path_substr ? normalize_path(path_substr) : g_strdup("");
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  uint32_t records_scanned = 0;

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(!arz->records[i].path)
      continue;

    char *lower_path = g_ascii_strdown(arz->records[i].path, -1);
    bool match = !lower_pattern[0] || strstr(lower_path, lower_pattern);
    g_free(lower_path);

    if(!match)
      continue;

    TQArzRecordData *data = arz_read_record(arz, arz->records[i].path);
    if(!data)
      continue;

    records_scanned++;

    for(uint32_t v = 0; v < data->num_vars; v++)
    {
      TQVariable *var = &data->vars[v];
      bool has_value = false;
      char sample[64] = "";

      if(var->type == TQ_VAR_INT && var->value.i32)
      {
        for(uint32_t j = 0; j < var->count; j++)
          if(var->value.i32[j] != 0)
          {
            has_value = true;
            snprintf(sample, sizeof(sample), "%d", var->value.i32[j]);
            break;
          }
      }
      else if(var->type == TQ_VAR_FLOAT && var->value.f32)
      {
        for(uint32_t j = 0; j < var->count; j++)
          if(fabsf(var->value.f32[j]) > 0.0001f)
          {
            has_value = true;
            snprintf(sample, sizeof(sample), "%.2f", var->value.f32[j]);
            break;
          }
      }
      else if(var->type == TQ_VAR_STRING && var->value.str)
      {
        for(uint32_t j = 0; j < var->count; j++)
          if(var->value.str[j] && var->value.str[j][0])
          {
            has_value = true;
            snprintf(sample, sizeof(sample), "%.50s", var->value.str[j]);
            break;
          }
      }

      if(!has_value)
        continue;

      CovStat *cs = g_hash_table_lookup(seen, var->name);

      if(!cs)
      {
        cs = g_malloc0(sizeof(*cs));
        cs->name = g_strdup(var->name);
        g_hash_table_insert(seen, (gpointer)cs->name, cs);
      }

      cs->count++;

      if(!cs->sample[0])
        snprintf(cs->sample, sizeof(cs->sample), "%s", sample);
    }

    arz_record_data_free(data);
  }

  // Collect into array, sort, print
  uint32_t n = g_hash_table_size(seen);
  CovStat *arr = g_new0(CovStat, n);
  GHashTableIter iter;
  gpointer key, value;
  uint32_t idx = 0;

  g_hash_table_iter_init(&iter, seen);

  while(g_hash_table_iter_next(&iter, &key, &value))
    arr[idx++] = *(CovStat *)value;

  qsort(arr, n, sizeof(CovStat), cov_cmp);

  printf("# coverage: %u records scanned (filter: \"%s\")\n", records_scanned, path_substr ? path_substr : "");
  printf("# columns: count\tvar_name\tsample_value\n");

  for(uint32_t k = 0; k < n; k++)
    printf("%u\t%s\t%s\n", arr[k].count, arr[k].name, arr[k].sample);

  fprintf(stderr, "\n%u distinct variable names with at least one non-zero/non-null value.\n", n);

  g_hash_table_iter_init(&iter, seen);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    CovStat *cs = value;
    g_free((char *)cs->name);
    g_free(cs);
  }

  g_hash_table_destroy(seen);
  g_free(arr);
  g_free(lower_pattern);
  arz_free(arz);
  return(0);
}
