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
#include "../arz.h"
#include "../arc.h"

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
    "\n"
    "Examples:\n"
    "  %s dump testdata/database.arz records/xpack4/item/relics/x4_relic05.dbr\n"
    "  %s search testdata/database.arz xpack4/item/relics/\n"
    "  %s fields testdata/database.arz xpack4/item/lootmagicalaffixes/ description,lootRandomizerName,FileDescription\n"
    "  %s stats testdata/database.arz xpack4/item/lootmagicalaffixes/x4_relic05\n"
    "  %s arctxt /path/to/Text_EN.arc x4tagU_Relic\n"
    "  %s arcls /path/to/Text_EN.arc\n"
    "  %s archex testdata/gamefiles/Resources/Items.arc items/equipmenthead\n"
    "  %s bonus testdata/database.arz records/xpack4/item/relics/x4_relic05.dbr\n",
    prog, prog, prog, prog, prog, prog, prog, prog, prog);
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
  char *tok = strtok(fields_copy, ",");

  while(tok && num_fields < 64)
  {
    field_names[num_fields++] = tok;
    tok = strtok(NULL, ",");
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

  // Parse randomizerName[N] / randomizerWeight[N] pairs
  for(int n = 1; n <= 50; n++)
  {
    char name_key[64], weight_key[64];

    snprintf(name_key, sizeof(name_key), "randomizerName%d", n);
    snprintf(weight_key, sizeof(weight_key), "randomizerWeight%d", n);

    char *bonus_path = arz_record_get_string(table, name_key, NULL);

    if(!bonus_path || !bonus_path[0])
    {
      free(bonus_path);
      break;
    }

    // Get weight
    int weight = arz_record_get_int(table, weight_key, 0, NULL);

    printf("Bonus %d (weight %d): %s\n", n, weight, bonus_path);

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
    free(bonus_path);
  }

  arz_record_data_free(table);
  free(table_path);
  arz_record_data_free(item_data);
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

  fprintf(stderr, "Unknown command: %s\n", cmd);
  usage(argv[0]);
  return(1);
}
