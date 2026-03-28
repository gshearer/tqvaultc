#include "arz.h"
#include "platform_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

// ── string intern table ───────────────────────────────────────────

// lowercase string -> canonical pointer
static GHashTable *g_intern_table = NULL;

// arz_intern_init -- initialize the string interning system.
// creates the global hash table if it does not already exist.
void
arz_intern_init(void)
{
  if(g_intern_table)
    return;

  g_intern_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

// arz_intern -- intern a variable name string (case-insensitive).
// name: variable name to intern.
// returns: canonical lowercased pointer for this name, or NULL if name is NULL.
const char *
arz_intern(const char *name)
{
  if(!name)
    return(NULL);

  if(!g_intern_table)
    arz_intern_init();

  // Lowercase the name for canonical form
  size_t len = strlen(name);
  char *lower = malloc(len + 1);

  if(!lower)
    return(NULL);

  for(size_t i = 0; i <= len; i++)
    lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];

  const char *existing = g_hash_table_lookup(g_intern_table, lower);

  if(existing)
  {
    free(lower);
    return(existing);
  }

  // lower becomes the canonical copy owned by the hash table
  g_hash_table_insert(g_intern_table, lower, lower);
  return(lower);
}

// arz_intern_free -- free the string interning system.
// destroys the global hash table and sets it to NULL.
void
arz_intern_free(void)
{
  if(g_intern_table)
  {
    g_hash_table_destroy(g_intern_table);
    g_intern_table = NULL;
  }
}

// ── var_index building ──────────────────────────────────────────

// arz_record_build_var_index -- build the var_index hash table for a record.
// data: record data to index (NULL is safe, already-indexed is a no-op).
void
arz_record_build_var_index(TQArzRecordData *data)
{
  if(!data || data->var_index)
    return;

  data->var_index = g_hash_table_new(g_direct_hash, g_direct_equal);

  for(uint32_t i = 0; i < data->num_vars; i++)
  {
    if(!data->vars[i].name)
      continue;

    const char *interned = arz_intern(data->vars[i].name);

    // Only insert the first occurrence (matches original linear scan behavior)
    if(!g_hash_table_contains(data->var_index, interned))
      g_hash_table_insert(data->var_index, (gpointer)interned, &data->vars[i]);
  }
}

// arz_record_get_var -- O(1) variable lookup by interned name pointer.
// data: record data (must have var_index built).
// interned_name: MUST be an interned pointer from arz_intern().
// returns: pointer to the TQVariable, or NULL if not found.
TQVariable *
arz_record_get_var(TQArzRecordData *data, const char *interned_name)
{
  if(!data || !interned_name)
    return(NULL);

  if(!data->var_index)
    return(NULL);

  return(g_hash_table_lookup(data->var_index, interned_name));
}

// ── low-level readers ─────────────────────────────────────────────

// read_u32 -- read a little-endian uint32 from a byte buffer.
// data: source buffer.
// offset: byte offset to read from.
// returns: the uint32 value.
static uint32_t
read_u32(const uint8_t *data, size_t offset)
{
  uint32_t val;

  memcpy(&val, data + offset, 4);
  return(val);
}

// read_u16 -- read a little-endian uint16 from a byte buffer.
// data: source buffer.
// offset: byte offset to read from.
// returns: the uint16 value.
static uint16_t
read_u16(const uint8_t *data, size_t offset)
{
  uint16_t val;

  memcpy(&val, data + offset, 2);
  return(val);
}

// read_string_prefixed -- read a length-prefixed string from a byte buffer.
// data: source buffer.
// offset: byte offset where the 4-byte length prefix starts.
// next_offset: if non-NULL, set to the offset after the string.
// returns: malloc'd null-terminated string.
static char *
read_string_prefixed(const uint8_t *data, size_t offset, size_t *next_offset)
{
  uint32_t len = read_u32(data, offset);
  char *str = malloc(len + 1);

  if(!str)
  {
    if(next_offset)
      *next_offset = offset + 4 + len;
    return(NULL);
  }

  memcpy(str, data + offset + 4, len);
  str[len] = '\0';

  if(next_offset)
    *next_offset = offset + 4 + len;

  return(str);
}

// arz_load -- load and parse an ARZ database file.
// filepath: path to the .arz file.
// returns: parsed TQArzFile, or NULL on failure.
TQArzFile *
arz_load(const char *filepath)
{
  size_t file_size = 0;
  uint8_t *data = platform_mmap_readonly(filepath, &file_size);

  if(!data)
    return(NULL);

  uint32_t magic = read_u32(data, 0);

  if(magic != 0x0052415a && magic != 0x00030004)
  {
    platform_munmap(data, file_size);
    return(NULL);
  }

  TQArzFile *arz = calloc(1, sizeof(TQArzFile));

  if(!arz)
  {
    platform_munmap(data, file_size);
    return(NULL);
  }

  arz->filepath = strdup(filepath);
  arz->raw_data = data;
  arz->data_size = file_size;

  uint32_t record_start = read_u32(data, 4);
  uint32_t record_count = read_u32(data, 12);
  uint32_t string_start = read_u32(data, 16);

  arz->num_strings = read_u32(data, string_start);
  arz->string_table = calloc(arz->num_strings, sizeof(char *));

  if(!arz->string_table)
  {
    free(arz->filepath);
    platform_munmap(data, file_size);
    free(arz);
    return(NULL);
  }

  size_t s_off = string_start + 4;

  for(uint32_t i = 0; i < arz->num_strings; i++)
    arz->string_table[i] = read_string_prefixed(data, s_off, &s_off);

  arz->num_records = record_count;
  arz->records = calloc(record_count, sizeof(TQArzRecord));

  if(!arz->records)
  {
    for(uint32_t i = 0; i < arz->num_strings; i++)
      free(arz->string_table[i]);
    free(arz->string_table);
    free(arz->filepath);
    platform_munmap(data, file_size);
    free(arz);
    return(NULL);
  }

  size_t r_off = record_start;

  for(uint32_t i = 0; i < record_count; i++)
  {
    uint32_t name_idx = read_u32(data, r_off);
    uint32_t type_len = read_u32(data, r_off + 4);

    r_off += 8 + type_len;

    arz->records[i].path = (name_idx < arz->num_strings) ? arz->string_table[name_idx] : NULL;
    arz->records[i].offset = read_u32(data, r_off) + 24;
    arz->records[i].compressed_size = read_u32(data, r_off + 4);
    r_off += 16;
  }

  // Pre-intern all string table entries so var_index lookups work
  for(uint32_t i = 0; i < arz->num_strings; i++)
  {
    if(arz->string_table[i])
      arz_intern(arz->string_table[i]);
  }

  return(arz);
}

// arz_read_record_at -- read and decompress a record at a specific offset.
// arz: the database file.
// offset: byte offset into the raw data.
// compressed_size: size of the compressed record data.
// returns: parsed TQArzRecordData with var_index built, or NULL on failure.
TQArzRecordData *
arz_read_record_at(TQArzFile *arz, uint32_t offset, uint32_t compressed_size)
{
  if(!arz || offset + compressed_size > arz->data_size)
    return(NULL);

  uLong uncompressed_size = 1024 * 1024; // 1MB initial
  uint8_t *uncompressed = malloc(uncompressed_size);

  if(!uncompressed)
    return(NULL);

  int res = uncompress(uncompressed, &uncompressed_size, arz->raw_data + offset, compressed_size);

  if(res == Z_BUF_ERROR)
  {
    uncompressed_size = 4 * 1024 * 1024; // 4MB
    uint8_t *new_buf = realloc(uncompressed, uncompressed_size);

    if(!new_buf)
    {
      free(uncompressed);
      return(NULL);
    }

    uncompressed = new_buf;
    res = uncompress(uncompressed, &uncompressed_size, arz->raw_data + offset, compressed_size);
  }

  if(res != Z_OK)
  {
    free(uncompressed);
    return(NULL);
  }

  uint32_t num_vars = 0;
  size_t off = 0;
  size_t data_pool_size = 0;

  while(off + 8 <= (size_t)uncompressed_size)
  {
    uint16_t count = read_u16(uncompressed, off + 2);

    data_pool_size += count * 8;
    off += 8 + 4 * (size_t)count;
    num_vars++;
  }

  TQArzRecordData *data = calloc(1, sizeof(TQArzRecordData));

  if(!data)
  {
    free(uncompressed);
    return(NULL);
  }

  data->num_vars = num_vars;
  data->vars = calloc(num_vars, sizeof(TQVariable));
  data->buffer_to_free = uncompressed;
  data->pool_to_free = malloc(data_pool_size);
  data->var_index = NULL;

  if(!data->vars || (data_pool_size > 0 && !data->pool_to_free))
  {
    free(data->vars);
    free(data->pool_to_free);
    free(uncompressed);
    free(data);
    return(NULL);
  }

  size_t pool_off = 0;

  off = 0;

  for(uint32_t i = 0; i < num_vars; i++)
  {
    uint16_t type = read_u16(uncompressed, off);
    uint16_t count = read_u16(uncompressed, off + 2);
    uint32_t key_idx = read_u32(uncompressed, off + 4);

    off += 8;

    data->vars[i].name = (key_idx < arz->num_strings) ? arz->string_table[key_idx] : NULL;
    data->vars[i].count = count;

    if(type == 0 || type == 1)
    {
      data->vars[i].type = (type == 0) ? TQ_VAR_INT : TQ_VAR_FLOAT;
      data->vars[i].value.i32 = (int32_t *)(data->pool_to_free + pool_off);
      memcpy(data->vars[i].value.i32, uncompressed + off, (size_t)count * 4);
      pool_off += (size_t)count * 4;
    }
    else if(type == 2)
    {
      data->vars[i].type = TQ_VAR_STRING;
      data->vars[i].value.str = (const char **)(data->pool_to_free + pool_off);

      for(uint32_t j = 0; j < count; j++)
      {
        uint32_t val_idx = read_u32(uncompressed, off + 4 * j);

        data->vars[i].value.str[j] = (val_idx < arz->num_strings) ? arz->string_table[val_idx] : NULL;
      }

      pool_off += (size_t)count * sizeof(char *);
    }

    off += 4 * (size_t)count;
  }

  // Build var_index for O(1) lookups
  arz_record_build_var_index(data);

  return(data);
}

// arz_read_record -- read a record by its path from the database.
// arz: the database file.
// record_path: path of the record to read (case-insensitive, / or \ separators).
// returns: parsed TQArzRecordData, or NULL if not found.
TQArzRecordData *
arz_read_record(TQArzFile *arz, const char *record_path)
{
  if(!arz || !record_path)
    return(NULL);

  char normalized_path[1024];

  strncpy(normalized_path, record_path, sizeof(normalized_path));
  normalized_path[sizeof(normalized_path) - 1] = '\0';

  for(int i = 0; normalized_path[i]; i++)
  {
    if(normalized_path[i] == '/')
      normalized_path[i] = '\\';
  }

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(arz->records[i].path && strcasecmp(arz->records[i].path, normalized_path) == 0)
      return(arz_read_record_at(arz, arz->records[i].offset, arz->records[i].compressed_size));
  }

  return(NULL);
}

// arz_record_get_string -- get a string variable value from a record.
// data: record data.
// var_name: variable name to look up (case-insensitive).
// found: optional, set to true if the variable exists.
// returns: strdup'd string (caller must free), or NULL.
char *
arz_record_get_string(TQArzRecordData *data, const char *var_name, bool *found)
{
  if(found)
    *found = false;

  if(!data || !var_name)
    return(NULL);

  // Try fast path via var_index with interned name
  if(data->var_index)
  {
    const char *interned = arz_intern(var_name);
    TQVariable *v = g_hash_table_lookup(data->var_index, interned);

    if(v && v->type == TQ_VAR_STRING && v->count > 0)
    {
      if(found)
        *found = true;
      return(v->value.str[0] ? strdup(v->value.str[0]) : NULL);
    }

    return(NULL);
  }

  // Fallback: linear scan
  for(uint32_t i = 0; i < data->num_vars; i++)
  {
    if(data->vars[i].name && strcasecmp(data->vars[i].name, var_name) == 0)
    {
      if(data->vars[i].type == TQ_VAR_STRING && data->vars[i].count > 0)
      {
        if(found)
          *found = true;
        return(data->vars[i].value.str[0] ? strdup(data->vars[i].value.str[0]) : NULL);
      }
      break;
    }
  }

  return(NULL);
}

// arz_record_get_int -- get an integer variable value from a record.
// data: record data.
// var_name: variable name to look up (case-insensitive).
// default_val: value to return if the variable is not found.
// found: optional, set to true if the variable exists.
// returns: integer value, or default_val if not found.
int
arz_record_get_int(TQArzRecordData *data, const char *var_name, int default_val,
                   bool *found)
{
  if(found)
    *found = false;

  if(!data || !var_name)
    return(default_val);

  // Try fast path via var_index
  if(data->var_index)
  {
    const char *interned = arz_intern(var_name);
    TQVariable *v = g_hash_table_lookup(data->var_index, interned);

    if(v && v->type == TQ_VAR_INT && v->count > 0)
    {
      if(found)
        *found = true;
      return(v->value.i32[0]);
    }

    return(default_val);
  }

  // Fallback: linear scan
  for(uint32_t i = 0; i < data->num_vars; i++)
  {
    if(data->vars[i].name && strcasecmp(data->vars[i].name, var_name) == 0)
    {
      if(data->vars[i].type == TQ_VAR_INT && data->vars[i].count > 0)
      {
        if(found)
          *found = true;
        return(data->vars[i].value.i32[0]);
      }
      break;
    }
  }

  return(default_val);
}

// arz_record_data_free -- free a parsed record and all its resources.
// data: record data to free (NULL is safe).
void
arz_record_data_free(TQArzRecordData *data)
{
  if(!data)
    return;

  if(data->var_index)
    g_hash_table_destroy(data->var_index);

  free(data->vars);
  free(data->pool_to_free);
  free(data->buffer_to_free);
  free(data);
}

// arz_free -- free all resources associated with an ARZ database.
// arz: database to free (NULL is safe).
void
arz_free(TQArzFile *arz)
{
  if(!arz)
    return;

  free(arz->filepath);

  if(arz->raw_data)
    platform_munmap(arz->raw_data, arz->data_size);

  if(arz->string_table)
  {
    for(uint32_t i = 0; i < arz->num_strings; i++)
      free(arz->string_table[i]);

    free(arz->string_table);
  }

  free(arz->records);
  free(arz);
}
