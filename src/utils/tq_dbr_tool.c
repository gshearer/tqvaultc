// tq_dbr_tool.c -- Universal DBR/ARC inspection tool for TQVaultC development.
//
// Works directly against testdata/database.arz and game arc files without
// requiring the resource index or game installation path.
//
// Usage:
//   tq-dbr-tool <command> [options]
//
// Commands:
//   dump    <arz> <record_path>            Dump all variables from a DBR record
//   search  <arz> <pattern>                List records matching a path pattern
//   fields  <arz> <pattern> <field,...>     Show specific fields for matching records
//   stats   <arz> <pattern>                Show non-zero numeric variables for matching records
//   arctxt  <arc> <search_term>            Search for text in arc text files (UTF-16 aware)
//   arcls   <arc>                          List all files in an arc archive
//   archex  <arc> <file_pattern>           Extract and hex-dump a file from an arc archive
//   bonus   <arz> <item_path>              Follow bonus table chain for a relic/charm/artifact

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <math.h>
#include <glib.h>
#include "../compat.h"  // portable strcasestr (mingw)
#include "../arz.h"
#include "../arc.h"
#include "../parse_num.h"
#include "../db_loot.h"
#include "../db_creatures.h"
#include "../db_quests.h"
#include "../texture.h"
#include "../mesh.h"
#include "../mesh_render.h"
#include "../mesh_skin.h"
#include "../anm.h"

// Normalizes a path by lowercasing and converting forward slashes to
// backslashes (arz paths use backslashes).
// input: the path string to normalize.
// Returns a newly allocated normalized string (free with g_free).
static char *
normalize_path(const char *input)
{
  char *out = g_ascii_strdown(input, -1);

  for(char *p = out; *p; p++)
    if(*p == '/')
      *p = '\\';

  return(out);
}

// Prints usage information for all commands to stderr.
// prog: the program name (argv[0]).
static void
usage(const char *prog)
{
  fprintf(stderr,
    "Usage: %s <command> [options]\n"
    "\n"
    "Commands:\n"
    "  dump    <arz> <record_path>          Dump all variables from a DBR record\n"
    "  search  <arz> <pattern>              List records matching path substring\n"
    "  fields  <arz> <pattern> <field,...>   Show specific fields for matching records\n"
    "  stats   <arz> <pattern>              Show non-zero numeric vars for matching records\n"
    "  arctxt  <arc> <search_term>          Search text in arc files (UTF-16 aware)\n"
    "  arcls   <arc>                        List all files in an arc archive\n"
    "  archex  <arc> <file_pattern>         Extract and hex-dump a file from an arc archive\n"
    "  bonus   <arz> <item_path>            Follow bonus table chain for relic/charm/artifact\n"
    "  coverage <arz> [path_substr]         Sorted list of all vars with non-zero values\n"
    "  categories <arz>                     Count items per Database Browser category\n"
    "  sets <arz>                           List item sets, members and bonus tiers\n"
    "  affixes <arz>                        List prefixes/suffixes, their gear and stats\n"
    "  skills <arz>                         List masteries and their skills (max level, tier)\n"
    "  loot <arz> <table> [level]           Flatten a loot table to its items + chances\n"
    "  creatures <arz>                      Summarize boss/hero loot index ('dropped by')\n"
    "  droppedby <arz> <item>               List creatures that drop an item, per difficulty\n"
    "  quests <arz> <resources_dir>         Summarize quest item-reward index\n"
    "\n"
    "Examples:\n"
    "  %s dump testdata/database.arz records/xpack4/item/relics/x4_relic05.dbr\n"
    "  %s search testdata/database.arz xpack4/item/relics/\n"
    "  %s fields testdata/database.arz xpack4/item/lootmagicalaffixes/ description,lootRandomizerName,FileDescription\n"
    "  %s stats testdata/database.arz xpack4/item/lootmagicalaffixes/x4_relic05\n"
    "  %s arctxt /path/to/Text_EN.arc x4tagU_Relic\n"
    "  %s arcls /path/to/Text_EN.arc\n"
    "  %s archex testdata/gamefiles/Resources/Items.arc items/equipmenthead\n"
    "  %s bonus testdata/database.arz records/xpack4/item/relics/x4_relic05.dbr\n"
    "  %s categories testdata/gamefiles/Database/database.arz\n"
    "  %s sets testdata/gamefiles/Database/database.arz\n"
    "  %s affixes testdata/gamefiles/Database/database.arz\n"
    "  %s skills testdata/gamefiles/Database/database.arz\n",
    prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
    prog);
}

// Prints a single TQVariable's name and all its values to stdout.
// v: pointer to the variable to print.
static void
print_variable(TQVariable *v)
{
  printf("  %-40s ", v->name);

  if(v->count == 0)
  {
    printf("(empty)\n");
    return;
  }

  for(uint32_t j = 0; j < v->count; j++)
  {
    if(v->type == TQ_VAR_INT)
    {
      if(v->value.i32)
        printf("%d", v->value.i32[j]);
      else
        printf("(null)");
    }
    else if(v->type == TQ_VAR_FLOAT)
    {
      if(v->value.f32)
        printf("%.4f", v->value.f32[j]);
      else
        printf("(null)");
    }
    else if(v->type == TQ_VAR_STRING)
    {
      if(v->value.str)
        printf("%s", v->value.str[j] ? v->value.str[j] : "(null)");
      else
        printf("(null)");
    }
    else
    {
      printf("(unknown type %d)", v->type);
    }

    if(j < v->count - 1)
      printf(", ");
  }

  printf("\n");
}

// Dumps all variables from a single ARZ record.
// arz_path: path to the .arz database file.
// record_path: DBR record path within the database.
// Returns 0 on success, 1 on failure.
static int
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
static int
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
static int
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
static int
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

static int
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

// Searches for text in an arc text file, with UTF-16 awareness.
// arc_path: path to the .arc archive file.
// search_term: text to search for (case-insensitive).
// Returns 0 on success, 1 on failure.
static int
cmd_arctxt(const char *arc_path, const char *search_term)
{
  TQArcFile *arc = arc_load(arc_path);
  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  char *lower_search = g_ascii_strdown(search_term, -1);
  int total_matches = 0;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    size_t size;
    uint8_t *data = arc_extract_file(arc, i, &size);

    if(!data)
      continue;

    // Convert to UTF-8 if UTF-16LE BOM detected
    char *content = NULL;
    bool content_is_glib = false;

    if(size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
      GError *err = NULL;
      gsize bw;

      content = g_convert((const gchar*)(data+2), size-2,
                          "UTF-8", "UTF-16LE", NULL, &bw, &err);

      if(!content)
      {
        if(err)
        {
          fprintf(stderr, "Warning: encoding error in %s: %s\n",
                  arc->entries[i].path, err->message);
          g_error_free(err);
        }

        free(data);
        continue;
      }

      content_is_glib = true;
    }
    else
    {
      content = malloc(size + 1);
      if(!content)
      {
        free(data);
        continue;
      }

      memcpy(content, data, size);
      content[size] = '\0';
    }

    free(data);

    // Case-insensitive search
    char *lower = g_ascii_strdown(content, -1);
    char *pos = lower;

    while((pos = strstr(pos, lower_search)) != NULL)
    {
      int offset = pos - lower;

      // Find line boundaries in original content
      int ls = offset;

      while(ls > 0 && content[ls-1] != '\n')
        ls--;

      int le = offset;

      while(content[le] && content[le] != '\n' && content[le] != '\r')
        le++;

      printf("[%s] %.*s\n", arc->entries[i].path, le - ls, content + ls);
      total_matches++;
      pos++;
    }

    g_free(lower);

    if(content_is_glib)
      g_free(content);
    else
      free(content);
  }

  printf("\n%d matches found.\n", total_matches);

  g_free(lower_search);
  arc_free(arc);
  return(0);
}

// Lists all files in an arc archive with their sizes.
// arc_path: path to the .arc archive file.
// Returns 0 on success, 1 on failure.
static int
cmd_arcls(const char *arc_path)
{
  TQArcFile *arc = arc_load(arc_path);
  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  for(uint32_t i = 0; i < arc->num_files; i++)
    printf("%s (%u bytes)\n", arc->entries[i].path, arc->entries[i].real_size);

  printf("\n%u files total.\n", arc->num_files);
  arc_free(arc);
  return(0);
}

// Extracts a file from an arc archive and prints a hex dump.
// arc_path: path to the .arc archive file.
// file_pattern: case-insensitive substring to match against file paths in the archive.
// Returns 0 on success, 1 if pattern matches zero or multiple files.
static int
cmd_archex(const char *arc_path, const char *file_pattern)
{
  TQArcFile *arc = arc_load(arc_path);
  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  // Case-insensitive substring match (normalize / to backslash for arc paths)
  char *lower_pattern = g_ascii_strdown(file_pattern, -1);

  for(char *p = lower_pattern; *p; p++)
    if(*p == '/')
      *p = '\\';

  int match_idx = -1;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    char *lower_path = g_ascii_strdown(arc->entries[i].path, -1);

    if(strstr(lower_path, lower_pattern))
    {
      if(match_idx >= 0)
      {
        fprintf(stderr, "Pattern '%s' matches multiple files:\n", file_pattern);
        // Print the previous match
        fprintf(stderr, "  %s\n", arc->entries[match_idx].path);
        // Print this match and all remaining
        fprintf(stderr, "  %s\n", arc->entries[i].path);
        g_free(lower_path);

        for(uint32_t j = i + 1; j < arc->num_files; j++)
        {
          char *lp = g_ascii_strdown(arc->entries[j].path, -1);

          if(strstr(lp, lower_pattern))
            fprintf(stderr, "  %s\n", arc->entries[j].path);

          g_free(lp);
        }

        fprintf(stderr, "Use a more specific pattern.\n");
        g_free(lower_pattern);
        arc_free(arc);
        return(1);
      }

      match_idx = i;
    }

    g_free(lower_path);
  }

  if(match_idx < 0)
  {
    fprintf(stderr, "No file matching '%s' in %s\n", file_pattern, arc_path);
    g_free(lower_pattern);
    arc_free(arc);
    return(1);
  }

  printf("File: %s (%u bytes)\n\n", arc->entries[match_idx].path,
         arc->entries[match_idx].real_size);

  size_t size;
  uint8_t *data = arc_extract_file(arc, match_idx, &size);

  if(!data)
  {
    fprintf(stderr, "Failed to extract file\n");
    g_free(lower_pattern);
    arc_free(arc);
    return(1);
  }

  // Hex dump: 16 bytes per line with ASCII sidebar
  for(size_t off = 0; off < size; off += 16)
  {
    printf("%08zx  ", off);
    size_t n = (size - off < 16) ? size - off : 16;

    for(size_t j = 0; j < 16; j++)
    {
      if(j < n)
        printf("%02x ", data[off + j]);
      else
        printf("   ");

      if(j == 7)
        printf(" ");
    }

    printf(" |");

    for(size_t j = 0; j < n; j++)
    {
      uint8_t c = data[off + j];
      printf("%c", (c >= 0x20 && c <= 0x7e) ? c : '.');
    }

    printf("|\n");
  }

  printf("\n%zu bytes total.\n", size);

  free(data);
  g_free(lower_pattern);
  arc_free(arc);
  return(0);
}

// arcextract: extract a single (uniquely-matched) raw file from an arc to disk.
// Mirrors cmd_archex's matching, but writes the raw bytes instead of hex.
static int
cmd_arcextract(const char *arc_path, const char *file_pattern, const char *out_path)
{
  TQArcFile *arc = arc_load(arc_path);

  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  char *lower_pattern = g_ascii_strdown(file_pattern, -1);

  for(char *p = lower_pattern; *p; p++)
    if(*p == '/')
      *p = '\\';

  int match_idx = -1;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    char *lower_path = g_ascii_strdown(arc->entries[i].path, -1);

    if(strstr(lower_path, lower_pattern))
    {
      if(match_idx >= 0)
      {
        fprintf(stderr, "Pattern '%s' matches multiple files; be more specific.\n",
                file_pattern);
        g_free(lower_path);
        g_free(lower_pattern);
        arc_free(arc);
        return(1);
      }
      match_idx = i;
    }

    g_free(lower_path);
  }

  g_free(lower_pattern);

  if(match_idx < 0)
  {
    fprintf(stderr, "No file matching '%s' in %s\n", file_pattern, arc_path);
    arc_free(arc);
    return(1);
  }

  size_t size;
  uint8_t *data = arc_extract_file(arc, (uint32_t)match_idx, &size);

  if(!data)
  {
    fprintf(stderr, "Failed to extract file\n");
    arc_free(arc);
    return(1);
  }

  FILE *f = fopen(out_path, "wb");

  if(!f || fwrite(data, 1, size, f) != size)
  {
    fprintf(stderr, "Failed to write %s\n", out_path);
    if(f)
      fclose(f);
    free(data);
    arc_free(arc);
    return(1);
  }

  fclose(f);
  printf("Wrote %s (%zu bytes) from %s\n", out_path, size,
         arc->entries[match_idx].path);
  free(data);
  arc_free(arc);
  return(0);
}

// Find the unique arc entry whose path contains `pattern` (case-insensitive).
// Returns the entry index, or -1 if there is no match or more than one.
static int
arc_find_unique(TQArcFile *arc, const char *pattern)
{
  char *lower = g_ascii_strdown(pattern, -1);
  int match = -1;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    char *lp = g_ascii_strdown(arc->entries[i].path, -1);

    if(strstr(lp, lower))
      match = (match >= 0) ? -2 : (int)i;
    g_free(lp);
    if(match == -2)
      break;
  }

  g_free(lower);
  return(match < 0 ? -1 : match);
}

// meshrender: parse a .msh model from an arc and software-rasterize it (textured
// with a .tex from the same arc) into a PNG.  A prototype harness for the
// Database Browser creature-thumbnail feature.
static int
cmd_meshrender(int argc, char **argv)
{
  // argv: <arc> <mesh_substr> <tex_substr> <out.png> [size] [yaw] [pitch]
  //       [anm_substr|-] [frame]
  const char *arc_path = argv[2];
  const char *mesh_pat = argv[3];
  const char *tex_pat  = argv[4];
  const char *out_path = argv[5];
  int   size  = 256;
  // yaw: a number, or "auto" (the default) to orient from the bounding box.
  bool  auto_yaw = (argc <= 7) || strcmp(argv[7], "auto") == 0;
  float yaw   = 0.0f;
  float pitch = 12.0f;
  // Optional skeletal pose: an .anm substring + frame (0 if omitted).
  const char *anm_pat = (argc > 9) ? argv[9] : NULL;
  int   frame = 0;

  if((argc > 6  && !parse_int(argv[6], &size))     ||
     (!auto_yaw && !parse_float(argv[7], &yaw))    ||
     (argc > 8  && !parse_float(argv[8], &pitch))  ||
     (argc > 10 && !parse_int(argv[10], &frame)))
  {
    fprintf(stderr, "meshrender: size/yaw/pitch/frame must be numbers\n");
    return(1);
  }

  if(size < 8 || size > 2048)
    size = 256;

  TQArcFile *arc = arc_load(arc_path);

  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  int mi = arc_find_unique(arc, mesh_pat);

  if(mi < 0)
  {
    fprintf(stderr, "mesh pattern '%s' did not match exactly one file\n", mesh_pat);
    arc_free(arc);
    return(1);
  }

  size_t msize;
  uint8_t *mdata = arc_extract_file(arc, (uint32_t)mi, &msize);
  TQMesh *mesh = mdata ? tq_mesh_parse(mdata, msize) : NULL;

  free(mdata);

  if(!mesh)
  {
    fprintf(stderr, "Failed to parse mesh %s\n", arc->entries[mi].path);
    arc_free(arc);
    return(1);
  }

  // Optional natural pose: skin the mesh to a frame of an idle .anm from the
  // same arc (recomputes bounds, so it must run before auto-yaw + the print).
  if(anm_pat && anm_pat[0] && strcmp(anm_pat, "-") != 0)
  {
    int ai = arc_find_unique(arc, anm_pat);

    if(ai < 0)
      fprintf(stderr, "warning: anm '%s' not uniquely matched; bind pose\n", anm_pat);
    else if(!mesh->skin)
      fprintf(stderr, "warning: mesh has no skeleton/weights; bind pose\n");
    else
    {
      size_t asize;
      uint8_t *adata = arc_extract_file(arc, (uint32_t)ai, &asize);
      TQAnm *anm = adata ? tq_anm_parse(adata, asize) : NULL;

      free(adata);
      if(!anm)
        fprintf(stderr, "warning: failed to parse anm %s; bind pose\n",
                arc->entries[ai].path);
      else
      {
        if(tq_mesh_pose(mesh, anm, frame))
          printf("Posed with %s frame %d (of %d)\n", arc->entries[ai].path,
                 frame, tq_anm_num_frames(anm));
        tq_anm_free(anm);
      }
    }
  }

  if(auto_yaw)
    yaw = tq_mesh_suggest_yaw(mesh);

  printf("Mesh %s: %u verts, %u tris, bbox [%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
         arc->entries[mi].path, mesh->num_verts, mesh->num_tris,
         mesh->bbmin[0], mesh->bbmin[1], mesh->bbmin[2],
         mesh->bbmax[0], mesh->bbmax[1], mesh->bbmax[2]);

  GdkPixbuf *tex = NULL;

  // An explicit texture pattern is matched in the arc; "-" / omitted falls back
  // to the model's own material texture (chunk-7 baseTexture), mirroring how the
  // app textures creatures whose DBR provides no baseTexture override.
  bool tex_given = (tex_pat && tex_pat[0] && strcmp(tex_pat, "-") != 0);
  const char *tex_match = tex_pat;
  char *mat_base = NULL;

  if(!tex_given && mesh->material_texture)
  {
    // Match the material .tex in the arc by its basename (arc paths differ in
    // separator/case from the game path stored in the mesh).
    const char *b = strrchr(mesh->material_texture, '\\');
    const char *f = strrchr(mesh->material_texture, '/');

    if(f > b)
      b = f;
    mat_base = g_strdup(b ? b + 1 : mesh->material_texture);
    tex_match = mat_base;
    printf("No texture given; using mesh material texture %s\n",
           mesh->material_texture);
  }

  if(tex_match && tex_match[0] && strcmp(tex_match, "-") != 0)
  {
    int ti = arc_find_unique(arc, tex_match);

    if(ti < 0)
      fprintf(stderr, "warning: texture '%s' not uniquely matched; rendering untextured\n",
              tex_match);
    else
    {
      tex = texture_load_by_index(arc, (uint32_t)ti);
      if(!tex)
        fprintf(stderr, "warning: failed to decode texture %s\n", arc->entries[ti].path);
      else
        printf("Texture %s: %dx%d\n", arc->entries[ti].path,
               gdk_pixbuf_get_width(tex), gdk_pixbuf_get_height(tex));
    }
  }

  g_free(mat_base);

  GdkPixbuf *out = tq_mesh_render(mesh, tex, size, yaw, pitch);
  int rc = 0;

  if(!out)
  {
    fprintf(stderr, "Render failed\n");
    rc = 1;
  }
  else
  {
    GError *err = NULL;

    if(!gdk_pixbuf_save(out, out_path, "png", &err, NULL))
    {
      fprintf(stderr, "Failed to save %s: %s\n", out_path,
              err ? err->message : "?");
      if(err)
        g_error_free(err);
      rc = 1;
    }
    else
      printf("Wrote %s (%dx%d, yaw=%.0f pitch=%.0f)\n", out_path, size, size,
             yaw, pitch);
    g_object_unref(out);
  }

  if(tex)
    g_object_unref(tex);
  tq_mesh_free(mesh);
  arc_free(arc);
  return(rc);
}

// Follows the bonus table chain for a relic, charm, or artifact and
// prints all bonus entries with their weights and stats.
// arz_path: path to the .arz database file.
// item_path: DBR record path for the item to inspect.
// Returns 0 on success, 1 on failure.
static int
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

static int
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

static int
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

static int
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

static int
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

// -- Creatures + Quests (Phase 6) -------------------------------------------
//
// These exercise the shared db_loot / db_creatures / db_quests modules headless
// (no GTK/translation), so names use FileDescription or the path basename.

// Resolve a readable name for a record path: FileDescription, else basename.
// Returns a g_strdup'd string the caller frees.
static char *
tool_record_name(TQArzFile *arz, const char *path)
{
  TQArzRecordData *d = arz_read_record(arz, path);

  if(d)
  {
    char *fd = arz_record_get_string(d, "FileDescription", NULL);
    arz_record_data_free(d);
    if(fd && fd[0])
      return(fd);
    free(fd);
  }

  // Basename without extension.
  const char *slash = strrchr(path, '\\');
  const char *base = slash ? slash + 1 : path;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", base);
  char *dot = strrchr(buf, '.');
  if(dot)
    *dot = '\0';

  return(g_strdup(buf));
}

// Sort helper: descending by a double* GHashTable value, keyed by string.
typedef struct { const char *key; double val; } KV;

static gint
kv_cmp_desc(gconstpointer a, gconstpointer b)
{
  double da = ((const KV *)a)->val, db = ((const KV *)b)->val;
  return(da < db) ? 1 : (da > db) ? -1 : 0;
}

// loot <arz> <table> [level]  -- flatten one loot table and print its items.
static int
cmd_loot(const char *arz_path, const char *table, int level)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  GHashTable *out = db_loot_resolve(arz, table, level);

  if(!out)
  {
    printf("Not a loot table (or not found): %s\n", table);
    arz_free(arz);
    return(1);
  }

  GArray *rows = g_array_new(FALSE, FALSE, sizeof(KV));
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, out);
  while(g_hash_table_iter_next(&it, &k, &v))
  {
    KV row = { (const char *)k, *(double *)v };
    g_array_append_val(rows, row);
  }
  g_array_sort(rows, kv_cmp_desc);

  printf("Loot table: %s  (level %d)\n%u item(s):\n\n",
         table, level, rows->len);

  for(guint i = 0; i < rows->len; i++)
  {
    KV *row = &g_array_index(rows, KV, i);
    char *name = tool_record_name(arz, row->key);
    printf("  %8.4f  %-34s  %s\n", row->val, name, row->key);
    g_free(name);
  }

  g_array_free(rows, TRUE);
  g_hash_table_destroy(out);
  arz_free(arz);
  return(0);
}

// creatures <arz>  -- build the creature loot index and print a summary.
static int
cmd_creatures(const char *arz_path)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  DbCreatureIndex *idx = db_creature_index_build(arz);

  int total = (int)idx->creatures->len, with_drops = 0;
  int boss = 0, hero = 0, quest = 0;

  for(guint i = 0; i < idx->creatures->len; i++)
  {
    DbCreature *c = g_ptr_array_index(idx->creatures, i);
    if(c->has_drops)
      with_drops++;
    if(g_ascii_strcasecmp(c->classification, "Boss") == 0)
      boss++;
    else if(g_ascii_strcasecmp(c->classification, "Hero") == 0)
      hero++;
    else
      quest++;
  }

  printf("Database Browser creatures — %s\n\n", arz_path);
  printf("  %d creatures (Boss %d / Hero %d / Quest %d); %d drop indexable loot\n",
         total, boss, hero, quest, with_drops);
  printf("  %u distinct items have a 'dropped by' source\n\n",
         g_hash_table_size(idx->by_item));

  // Show a sample of bosses with their largest drop, to eyeball correctness.
  printf("Sample bosses (name [levels] -> drop count):\n");
  int shown = 0;
  for(guint i = 0; i < idx->creatures->len && shown < 20; i++)
  {
    DbCreature *c = g_ptr_array_index(idx->creatures, i);
    if(g_ascii_strcasecmp(c->classification, "Boss") != 0 || !c->has_drops)
      continue;

    char *name = tool_record_name(arz, c->path);
    printf("  %-44s [%d/%d/%d]  %u items\n", name,
           c->level[0], c->level[1], c->level[2], c->drops->len);
    g_free(name);
    shown++;
  }

  db_creature_index_free(idx);
  arz_free(arz);
  return(0);
}

// droppedby <arz> <item>  -- list creatures that drop an item, by difficulty.
static int
cmd_droppedby(const char *arz_path, const char *item)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  DbCreatureIndex *idx = db_creature_index_build(arz);
  GPtrArray *drops = db_creature_drops_for_item(idx, item);

  char *iname = tool_record_name(arz, item);
  printf("Dropped by — %s (%s)\n\n", iname, item);
  g_free(iname);

  if(!drops || drops->len == 0)
  {
    printf("  (no creature drops this item)\n");
  }
  else
  {
    printf("  %-44s   Normal     Epic Legendary\n", "Creature");
    for(guint i = 0; i < drops->len; i++)
    {
      DbDrop *d = g_ptr_array_index(drops, i);
      DbCreature *c = g_ptr_array_index(idx->creatures, d->creature_idx);
      char *name = tool_record_name(arz, c->path);
      printf("  %-44s %8.4f %8.4f %8.4f\n", name,
             d->chance[0], d->chance[1], d->chance[2]);
      g_free(name);
    }
  }

  db_creature_index_free(idx);
  arz_free(arz);
  return(0);
}

// quests <arz> <resources_dir>  -- build the quest reward index and summarize.
static int
cmd_quests(const char *arz_path, const char *res_dir)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  // Candidate Quests.arc locations under the Resources directory.
  static const char *REL[] = {
    "Quests.arc", "xpack/Quests.arc", "XPack2/Quests.arc",
    "XPack3/Quests.arc", "XPack4/Quests.arc",
  };
  GPtrArray *arcs = g_ptr_array_new_with_free_func(g_free);
  for(size_t i = 0; i < sizeof(REL) / sizeof(REL[0]); i++)
  {
    char *p = g_build_filename(res_dir, REL[i], NULL);
    if(g_file_test(p, G_FILE_TEST_IS_REGULAR))
    {
      printf("  arc: %s\n", p);
      g_ptr_array_add(arcs, p);
    }
    else
      g_free(p);
  }

  if(arcs->len == 0)
    fprintf(stderr, "  (no Quests.arc found under %s)\n", res_dir);

  DbQuestIndex *idx = db_quest_index_build(arz,
      (const char *const *)arcs->pdata, (int)arcs->len);

  printf("\nDatabase Browser quests — %s\n\n", arz_path);
  printf("  %u quests grant item rewards; %u distinct reward items\n\n",
         idx->quests->len, g_hash_table_size(idx->by_item));

  int shown = 0;
  for(guint i = 0; i < idx->quests->len && shown < 20; i++)
  {
    DbQuest *q = g_ptr_array_index(idx->quests, i);
    int n0 = q->rewards[0] ? (int)g_hash_table_size(q->rewards[0]) : 0;
    int n1 = q->rewards[1] ? (int)g_hash_table_size(q->rewards[1]) : 0;
    int n2 = q->rewards[2] ? (int)g_hash_table_size(q->rewards[2]) : 0;
    printf("  %-40s  rewards N/E/L = %d/%d/%d\n", q->title_tag, n0, n1, n2);
    shown++;
  }

  db_quest_index_free(idx);
  g_ptr_array_free(arcs, TRUE);
  arz_free(arz);
  return(0);
}

// Entry point. Dispatches to the appropriate subcommand handler.
// argc: argument count (must be >= 2).
// argv: argument vector; argv[1] is the command name.
// Returns 0 on success, 1 on failure or unknown command.
int
main(int argc, char **argv)
{
  if(argc < 2)
  {
    usage(argv[0]);
    return(1);
  }

  const char *cmd = argv[1];

  if(strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)
  {
    usage(argv[0]);
    return(0);
  }

  if(strcmp(cmd, "dump") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s dump <arz> <record_path>\n", argv[0]);
      return(1);
    }

    return(cmd_dump(argv[2], argv[3]));
  }

  if(strcmp(cmd, "search") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s search <arz> <pattern>\n", argv[0]);
      return(1);
    }

    return(cmd_search(argv[2], argv[3]));
  }

  if(strcmp(cmd, "fields") == 0)
  {
    if(argc < 5)
    {
      fprintf(stderr, "Usage: %s fields <arz> <pattern> <field,...>\n", argv[0]);
      return(1);
    }

    return(cmd_fields(argv[2], argv[3], argv[4]));
  }

  if(strcmp(cmd, "stats") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s stats <arz> <pattern>\n", argv[0]);
      return(1);
    }

    return(cmd_stats(argv[2], argv[3]));
  }

  if(strcmp(cmd, "arctxt") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s arctxt <arc> <search_term>\n", argv[0]);
      return(1);
    }

    return(cmd_arctxt(argv[2], argv[3]));
  }

  if(strcmp(cmd, "arcls") == 0)
  {
    if(argc < 3)
    {
      fprintf(stderr, "Usage: %s arcls <arc>\n", argv[0]);
      return(1);
    }

    return(cmd_arcls(argv[2]));
  }

  if(strcmp(cmd, "arcextract") == 0)
  {
    if(argc < 5)
    {
      fprintf(stderr, "Usage: %s arcextract <arc> <file_pattern> <out_path>\n", argv[0]);
      return(1);
    }
    return(cmd_arcextract(argv[2], argv[3], argv[4]));
  }

  if(strcmp(cmd, "meshrender") == 0)
  {
    if(argc < 6)
    {
      fprintf(stderr, "Usage: %s meshrender <arc> <mesh_substr> <tex_substr|-> "
              "<out.png> [size] [yaw] [pitch] [anm_substr|-] [frame]\n", argv[0]);
      return(1);
    }
    return(cmd_meshrender(argc, argv));
  }

  if(strcmp(cmd, "archex") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s archex <arc> <file_pattern>\n", argv[0]);
      return(1);
    }

    return(cmd_archex(argv[2], argv[3]));
  }

  if(strcmp(cmd, "bonus") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s bonus <arz> <item_path>\n", argv[0]);
      return(1);
    }

    return(cmd_bonus(argv[2], argv[3]));
  }

  if(strcmp(cmd, "coverage") == 0)
  {
    if(argc < 3)
    {
      fprintf(stderr, "Usage: %s coverage <arz> [path_substr]\n", argv[0]);
      return(1);
    }

    return(cmd_coverage(argv[2], argc >= 4 ? argv[3] : ""));
  }

  if(strcmp(cmd, "categories") == 0)
  {
    if(argc < 3)
    {
      fprintf(stderr, "Usage: %s categories <arz>\n", argv[0]);
      return(1);
    }

    return(cmd_categories(argv[2]));
  }

  if(strcmp(cmd, "sets") == 0)
  {
    if(argc < 3)
    {
      fprintf(stderr, "Usage: %s sets <arz>\n", argv[0]);
      return(1);
    }

    return(cmd_sets(argv[2]));
  }

  if(strcmp(cmd, "skills") == 0)
  {
    if(argc < 3)
    {
      fprintf(stderr, "Usage: %s skills <arz>\n", argv[0]);
      return(1);
    }

    return(cmd_skills(argv[2]));
  }

  if(strcmp(cmd, "affixes") == 0)
  {
    if(argc < 3)
    {
      fprintf(stderr, "Usage: %s affixes <arz>\n", argv[0]);
      return(1);
    }

    return(cmd_affixes(argv[2]));
  }

  if(strcmp(cmd, "loot") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s loot <arz> <table_path> [level]\n", argv[0]);
      return(1);
    }

    int level = 30;

    if(argc >= 5 && !parse_int(argv[4], &level))
    {
      fprintf(stderr, "loot: level must be a number\n");
      return(1);
    }

    return(cmd_loot(argv[2], argv[3], level));
  }

  if(strcmp(cmd, "creatures") == 0)
  {
    if(argc < 3)
    {
      fprintf(stderr, "Usage: %s creatures <arz>\n", argv[0]);
      return(1);
    }

    return(cmd_creatures(argv[2]));
  }

  if(strcmp(cmd, "droppedby") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s droppedby <arz> <item_path>\n", argv[0]);
      return(1);
    }

    return(cmd_droppedby(argv[2], argv[3]));
  }

  if(strcmp(cmd, "quests") == 0)
  {
    if(argc < 4)
    {
      fprintf(stderr, "Usage: %s quests <arz> <resources_dir>\n", argv[0]);
      return(1);
    }

    return(cmd_quests(argv[2], argv[3]));
  }

  fprintf(stderr, "Unknown command: %s\n", cmd);
  usage(argv[0]);
  return(1);
}
