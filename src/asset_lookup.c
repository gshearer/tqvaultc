#include "asset_lookup.h"
#include "platform_mmap.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <zlib.h>
#include <glib.h>

static char *g_game_path = NULL;
static TQArzFile **g_arz_cache = NULL;
static TQArcFile **g_arc_cache = NULL;
static GHashTable *g_dbr_cache = NULL;
static int g_num_files = 0;
static GMutex g_dbr_mutex;

static void *g_index_mmap = NULL;
static size_t g_index_size = 0;
static TQIndexHeader *g_index_header = NULL;
static TQAssetEntry *g_index_entries = NULL;
static const char **g_game_files = NULL;

// calculate_hash - compute a CRC32 hash of a normalized game path
// path: asset path to hash (backslash-normalized, lowercased)
// returns: CRC32 hash value, or 0 if path is NULL
static uint32_t
calculate_hash(const char *path)
{
  if(!path)
    return(0);

  char normalized[1024];
  size_t len = strlen(path);

  if(len >= sizeof(normalized))
    len = sizeof(normalized) - 1;

  memcpy(normalized, path, len);
  normalized[len] = '\0';

  for(int i = 0; normalized[i]; i++)
  {
    if(normalized[i] == '/')
      normalized[i] = '\\';

    if(normalized[i] >= 'A' && normalized[i] <= 'Z')
      normalized[i] += 32;
  }

  return((uint32_t)crc32(0, (const Bytef *)normalized, (uInt)strlen(normalized)));
}

// compare_entries - qsort comparator for TQAssetEntry by hash then file_id
// a: pointer to first TQAssetEntry
// b: pointer to second TQAssetEntry
// returns: negative if a < b, positive if a > b, 0 if equal
static int
compare_entries(const void *a, const void *b)
{
  const TQAssetEntry *e1 = (const TQAssetEntry *)a;
  const TQAssetEntry *e2 = (const TQAssetEntry *)b;

  if(e1->hash < e2->hash)
    return(-1);

  if(e1->hash > e2->hash)
    return(1);

  if(e1->file_id < e2->file_id)
    return(1);

  if(e1->file_id > e2->file_id)
    return(-1);

  return(0);
}

typedef struct {
  char *path;
} BuildGameFile;

typedef struct {
  TQAssetEntry *entries;
  int num_entries;
  int max_entries;
  BuildGameFile *files;
  int num_files;
  int max_files;
} IndexBuilder;

// builder_process_arz - scan an ARZ file and add its records to the index
// b: index builder to populate
// path: filesystem path to the ARZ file
// file_id: file table index for this ARZ
static void
builder_process_arz(IndexBuilder *b, const char *path, int file_id)
{
  FILE *fp = fopen(path, "rb");

  if(!fp)
    return;

  uint32_t header[5];

  if(fread(header, 4, 5, fp) != 5)
  {
    fclose(fp);
    return;
  }

  uint32_t record_start = header[1];
  uint32_t record_count = header[3];
  uint32_t string_start = header[4];

  if(record_count > 1000000)
  {
    fclose(fp);
    return;
  }

  fseek(fp, string_start, SEEK_SET);
  uint32_t num_strings;

  if(fread(&num_strings, 4, 1, fp) != 1)
  {
    fclose(fp);
    return;
  }

  char **strings = malloc(num_strings * sizeof(char *));

  if(!strings)
  {
    fclose(fp);
    return;
  }

  for(uint32_t i = 0; i < num_strings; i++)
  {
    uint32_t len;

    if(fread(&len, 4, 1, fp) != 1)
    {
      strings[i] = NULL;
      continue;
    }

    strings[i] = malloc(len + 1);

    if(!strings[i])
      continue;

    if(fread(strings[i], 1, len, fp) != len)
    {
      free(strings[i]);
      strings[i] = NULL;
      continue;
    }

    strings[i][len] = '\0';
  }

  fseek(fp, record_start, SEEK_SET);

  for(uint32_t i = 0; i < record_count; i++)
  {
    uint32_t name_idx;

    if(fread(&name_idx, 4, 1, fp) != 1)
      break;

    uint32_t type_len;

    if(fread(&type_len, 4, 1, fp) != 1)
      break;

    fseek(fp, type_len, SEEK_CUR);
    uint32_t offset, compressed_size;

    if(fread(&offset, 4, 1, fp) != 1)
      break;

    if(fread(&compressed_size, 4, 1, fp) != 1)
      break;

    fseek(fp, 8, SEEK_CUR);

    if(name_idx >= num_strings || !strings[name_idx])
      continue;

    if(b->num_entries >= b->max_entries)
    {
      b->max_entries *= 2;
      b->entries = realloc(b->entries, b->max_entries * sizeof(TQAssetEntry));

      if(!b->entries)
        break;
    }

    b->entries[b->num_entries].hash = calculate_hash(strings[name_idx]);
    b->entries[b->num_entries].file_id = (uint16_t)file_id;
    b->entries[b->num_entries].offset = offset + 24;
    b->entries[b->num_entries].size = compressed_size;
    b->entries[b->num_entries].real_size = 0;
    b->entries[b->num_entries].flags = 0;
    b->num_entries++;
  }

  for(uint32_t i = 0; i < num_strings; i++)
  {
    if(strings[i])
      free(strings[i]);
  }

  free(strings);
  fclose(fp);
}

// builder_process_arc - scan an ARC archive and add its entries to the index
// b: index builder to populate
// path: filesystem path to the ARC file
// rel_path: relative path within the game directory
// file_id: file table index for this ARC
static void
builder_process_arc(IndexBuilder *b, const char *path, const char *rel_path, int file_id)
{
  FILE *fp = fopen(path, "rb");

  if(!fp)
    return;

  char prefix[512] = "";
  const char *p = rel_path;

  // rel_path uses the OS-native separator (backslash on Windows) because
  // g_build_filename() produced it. Match either form when stripping the
  // Resources/ prefix so the on-disk-path -> in-archive-key derivation
  // gives the same result on both platforms.
  if(strncasecmp(p, "Resources/", 10) == 0 ||
     strncasecmp(p, "Resources\\", 10) == 0)
    p += 10;

  char *copy = strdup(p);
  char *dot = strrchr(copy, '.');

  if(dot)
    *dot = '\0';

  for(int i = 0; copy[i]; i++)
  {
    if(copy[i] == '/')
      copy[i] = '\\';
  }

  snprintf(prefix, sizeof(prefix), "%s\\", copy);
  free(copy);

  if(strncasecmp(prefix, "xpack\\", 6) == 0)
  {
    char temp[512];

    snprintf(temp, sizeof(temp), "XPack\\%s", prefix + 6);
    strncpy(prefix, temp, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
  }

  char magic[4];

  if(fread(magic, 1, 4, fp) != 4)
  {
    fclose(fp);
    return;
  }

  if(memcmp(magic, "ARC\0", 4) != 0)
  {
    fclose(fp);
    return;
  }

  uint32_t header[6];

  if(fread(header, 4, 6, fp) != 6)
  {
    fclose(fp);
    return;
  }

  uint32_t arc_num_files = header[1];
  uint32_t toc_offset = header[5];

  fseek(fp, toc_offset, SEEK_SET);
  fseek(fp, header[2] * 12, SEEK_CUR);

  char **filenames = malloc(arc_num_files * sizeof(char *));

  if(!filenames)
  {
    fclose(fp);
    return;
  }

  for(uint32_t i = 0; i < arc_num_files; i++)
  {
    char buf[1024];
    int c, j = 0;

    while((c = fgetc(fp)) != EOF && c != '\0' && j < 1023)
      buf[j++] = (char)c;

    buf[j] = '\0';
    filenames[i] = strdup(buf);
  }

  fseek(fp, -44LL * arc_num_files, SEEK_END);

  for(uint32_t i = 0; i < arc_num_files; i++)
  {
    uint32_t rec[11];

    if(fread(rec, 4, 11, fp) != 11)
      break;

    if(b->num_entries >= b->max_entries)
    {
      b->max_entries *= 2;
      b->entries = realloc(b->entries, b->max_entries * sizeof(TQAssetEntry));

      if(!b->entries)
        break;
    }

    char full_internal_path[1024];

    snprintf(full_internal_path, sizeof(full_internal_path), "%s%s", prefix, filenames[i]);

    b->entries[b->num_entries].hash = calculate_hash(full_internal_path);
    b->entries[b->num_entries].file_id = (uint16_t)file_id;
    b->entries[b->num_entries].offset = rec[1];
    b->entries[b->num_entries].size = rec[2];
    b->entries[b->num_entries].real_size = rec[3];
    b->entries[b->num_entries].flags = 1;
    b->num_entries++;
  }

  for(uint32_t i = 0; i < arc_num_files; i++)
    free(filenames[i]);

  free(filenames);
  fclose(fp);
}

// builder_scan_dir - recursively scan a directory for ARZ/ARC files
// b: index builder to populate
// base_path: root game directory
// sub_path: subdirectory to scan relative to base_path
static void
builder_scan_dir(IndexBuilder *b, const char *base_path, const char *sub_path)
{
  char *full_dir = g_build_filename(base_path, sub_path, NULL);
  GError *open_err = NULL;
  GDir *d = g_dir_open(full_dir, 0, &open_err);

  if(!d)
  {
    fprintf(stderr, "builder_scan_dir: g_dir_open(%s) failed: %s\n",
            full_dir, open_err ? open_err->message : "(no error info)");
    if(open_err)
      g_error_free(open_err);
    g_free(full_dir);
    return;
  }

  const gchar *name;

  while((name = g_dir_read_name(d)) != NULL)
  {
    if(name[0] == '.')
      continue;

    char *full_path = g_build_filename(full_dir, name, NULL);

    if(g_file_test(full_path, G_FILE_TEST_IS_DIR))
    {
      char *next_sub = g_build_filename(sub_path, name, NULL);

      builder_scan_dir(b, base_path, next_sub);
      g_free(next_sub);
    }
    else
    {
      const char *ext = strrchr(name, '.');

      if(ext && (strcasecmp(ext, ".arz") == 0 || strcasecmp(ext, ".arc") == 0))
      {
        if(b->num_files >= b->max_files)
        {
          b->max_files *= 2;
          b->files = realloc(b->files, b->max_files * sizeof(BuildGameFile));

          if(!b->files)
          {
            g_free(full_path);
            break;
          }
        }

        char *rel_path = g_build_filename(sub_path, name, NULL);

        b->files[b->num_files].path = strdup(rel_path);

        if(strcasecmp(ext, ".arz") == 0)
          builder_process_arz(b, full_path, b->num_files);
        else
          builder_process_arc(b, full_path, rel_path, b->num_files);

        g_free(rel_path);
        b->num_files++;
      }
    }

    g_free(full_path);
  }

  g_dir_close(d);
  g_free(full_dir);
}

// asset_index_build - build and write the asset index to disk
// game_path: root path to the game installation
// index_path: filesystem path to write the index file
static void
asset_index_build(const char *game_path, const char *index_path)
{
  IndexBuilder b;

  b.max_entries = 200000;
  b.entries = malloc(b.max_entries * sizeof(TQAssetEntry));
  b.num_entries = 0;
  b.max_files = 512;
  b.files = malloc(b.max_files * sizeof(BuildGameFile));
  b.num_files = 0;

  if(!b.entries || !b.files)
  {
    free(b.entries);
    free(b.files);
    return;
  }

  builder_scan_dir(&b, game_path, "Database");
  builder_scan_dir(&b, game_path, "Resources");

  fprintf(stderr, "asset_index_build: scan of %s found %d files, %d entries\n",
          game_path, b.num_files, b.num_entries);

  if(b.num_entries == 0)
  {
    fprintf(stderr, "asset_index_build: no entries — game_path is wrong or "
            "Database/Resources subdirs not found. Aborting (no index written).\n");
    free(b.entries);
    free(b.files);
    return;
  }

  qsort(b.entries, b.num_entries, sizeof(TQAssetEntry), compare_entries);

  int write_idx = 1;

  for(int read_idx = 1; read_idx < b.num_entries; read_idx++)
  {
    if(b.entries[read_idx].hash != b.entries[write_idx - 1].hash)
      b.entries[write_idx++] = b.entries[read_idx];
  }

  b.num_entries = write_idx;

  FILE *fp = fopen(index_path, "wb");

  if(!fp)
  {
    fprintf(stderr, "asset_index_build: fopen(%s, wb) failed: %s\n",
            index_path, strerror(errno));
    return;
  }

  TQIndexHeader header;

  memcpy(header.magic, "TQVI", 4);
  header.version = 1;
  header.num_files = b.num_files;
  header.num_entries = b.num_entries;
  header.entries_offset = sizeof(TQIndexHeader);
  header.string_table_offset = header.entries_offset + (b.num_entries * sizeof(TQAssetEntry));
  header.reserved[0] = 0;
  header.reserved[1] = 0;

  fwrite(&header, sizeof(header), 1, fp);
  fwrite(b.entries, sizeof(TQAssetEntry), b.num_entries, fp);

  for(int i = 0; i < b.num_files; i++)
    fwrite(b.files[i].path, 1, strlen(b.files[i].path) + 1, fp);

  fclose(fp);

  for(int i = 0; i < b.num_files; i++)
    free(b.files[i].path);

  free(b.files);
  free(b.entries);
}

// asset_index_load - memory-map and validate the pre-built asset index
// index_path: filesystem path to the index file
// returns: true if the index was loaded successfully, false otherwise
static bool
asset_index_load(const char *index_path)
{
  g_index_mmap = platform_mmap_readonly(index_path, &g_index_size);

  if(!g_index_mmap)
    return(false);

  g_index_header = (TQIndexHeader *)g_index_mmap;

  if(memcmp(g_index_header->magic, "TQVI", 4) != 0 || g_index_header->version != 1)
  {
    platform_munmap(g_index_mmap, g_index_size);
    g_index_mmap = NULL;
    return(false);
  }

  g_index_entries = (TQAssetEntry *)((char *)g_index_mmap + g_index_header->entries_offset);

  g_num_files = g_index_header->num_files;

  if(g_game_files)
    free(g_game_files);

  g_game_files = malloc((size_t)g_num_files * sizeof(char *));

  if(!g_game_files)
    return(false);

  const char *str = (const char *)g_index_mmap + g_index_header->string_table_offset;

  for(int i = 0; i < g_num_files; i++)
  {
    g_game_files[i] = str;
    str += strlen(str) + 1;
  }

  return(true);
}

// asset_manager_init - initialize the asset manager with the game path
// game_path: root path to the game installation
void
asset_manager_init(const char *game_path)
{
  g_game_path = strdup(game_path);

  char *cache_subdir = tqvc_cache_dir_new();
  char *index_path = g_build_filename(cache_subdir, "tqvc-resource-index.bin", NULL);

  g_free(cache_subdir);

  if(!asset_index_load(index_path))
  {
    fprintf(stderr, "asset_manager_init: building index at %s\n", index_path);
    asset_index_build(game_path, index_path);
    asset_index_load(index_path);
  }

  fprintf(stderr, "asset_manager_init: game_path=%s, index has %d files, %u entries\n",
          game_path, g_num_files,
          g_index_header ? g_index_header->num_entries : 0);

  // Sanity-check the index with a known asset that ships with every TQ install.
  const char *probe = "Items\\AnimalRelics\\AnimalPart07B_L.tex";
  const TQAssetEntry *e = asset_lookup(probe);
  fprintf(stderr, "asset_manager_init: probe '%s' -> %s\n",
          probe, e ? "FOUND" : "NOT FOUND (game folder may be wrong)");

  g_free(index_path);

  g_arz_cache = calloc((size_t)g_num_files, sizeof(TQArzFile *));
  g_arc_cache = calloc((size_t)g_num_files, sizeof(TQArcFile *));
  g_dbr_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)arz_record_data_free);

  // Pre-load all ARZ file handles (mmap only, fast) so background
  // prefetch threads don't race on lazy arz_load().
  for(int i = 0; i < g_num_files; i++)
  {
    const char *path = g_game_files[i];
    size_t len = strlen(path);

    if(len > 4 && strcasecmp(path + len - 4, ".arz") == 0)
      asset_get_arz((uint16_t)i);
  }
}

// asset_get_arz - get a cached TQArzFile for a given file_id
// file_id: index into the file table
// returns: cached ARZ file handle, or NULL on failure
TQArzFile *
asset_get_arz(uint16_t file_id)
{
  if(file_id >= g_num_files)
    return(NULL);

  if(g_arz_cache[file_id])
    return(g_arz_cache[file_id]);

  char *full_path = g_build_filename(g_game_path, g_game_files[file_id], NULL);

  g_arz_cache[file_id] = arz_load(full_path);
  g_free(full_path);
  return(g_arz_cache[file_id]);
}

// asset_get_arc - get a cached TQArcFile for a given file_id
// file_id: index into the file table
// returns: cached ARC file handle, or NULL on failure
TQArcFile *
asset_get_arc(uint16_t file_id)
{
  if(file_id >= g_num_files)
    return(NULL);

  if(g_arc_cache[file_id])
    return(g_arc_cache[file_id]);

  char *full_path = g_build_filename(g_game_path, g_game_files[file_id], NULL);

  g_arc_cache[file_id] = arc_load(full_path);
  g_free(full_path);
  return(g_arc_cache[file_id]);
}

// asset_get_dbr - get a cached TQArzRecordData for a given record path
// record_path: game-relative path to the DBR record
// returns: cached record data, or NULL if not found
TQArzRecordData *
asset_get_dbr(const char *record_path)
{
  if(!record_path)
    return(NULL);

  char *key = strdup(record_path);

  if(!key)
    return(NULL);

  for(int i = 0; key[i]; i++)
  {
    if(key[i] == '/')
      key[i] = '\\';

    if(key[i] >= 'A' && key[i] <= 'Z')
      key[i] += 32;
  }

  g_mutex_lock(&g_dbr_mutex);
  TQArzRecordData *data = g_hash_table_lookup(g_dbr_cache, key);
  g_mutex_unlock(&g_dbr_mutex);

  if(data)
  {
    free(key);
    return(data);
  }

  // Decompress from ARZ -- reads mmap'd data, allocates fresh memory.
  // ARZ files are pre-loaded in asset_manager_init(), so this is safe
  // to call from the background prefetch thread.
  const TQAssetEntry *entry = asset_lookup(key);

  if(entry)
  {
    TQArzFile *arz = g_arz_cache[entry->file_id];

    if(arz)
    {
      data = arz_read_record_at(arz, entry->offset, entry->size);

      if(data)
      {
        g_mutex_lock(&g_dbr_mutex);
        // Double-check: another thread may have inserted while we decompressed
        TQArzRecordData *existing = g_hash_table_lookup(g_dbr_cache, key);

        if(existing)
        {
          g_mutex_unlock(&g_dbr_mutex);
          arz_record_data_free(data);
          free(key);
          return(existing);
        }

        g_hash_table_insert(g_dbr_cache, key, data);
        g_mutex_unlock(&g_dbr_mutex);
        return(data);
      }
    }
  }

  free(key);
  return(NULL);
}

// asset_cache_insert - insert a pre-built record into the DBR cache
// key: malloc'd normalized path (ownership transferred to cache)
// data: record data (ownership transferred to cache)
void
asset_cache_insert(char *key, TQArzRecordData *data)
{
  if(!g_dbr_cache || !key || !data)
    return;

  g_mutex_lock(&g_dbr_mutex);
  g_hash_table_insert(g_dbr_cache, key, data);
  g_mutex_unlock(&g_dbr_mutex);
}

// asset_manager_free - free all cached resources and the asset manager state
void
asset_manager_free(void)
{
  for(int i = 0; i < g_num_files; i++)
  {
    if(g_arz_cache[i])
      arz_free(g_arz_cache[i]);

    if(g_arc_cache[i])
      arc_free(g_arc_cache[i]);
  }

  if(g_dbr_cache)
    g_hash_table_destroy(g_dbr_cache);

  free(g_arz_cache);
  free(g_arc_cache);
  free(g_game_path);

  if(g_game_files)
    free(g_game_files);

  if(g_index_mmap)
    platform_munmap(g_index_mmap, g_index_size);
}

// asset_lookup - find an asset entry by its path using binary search
// path: game-relative asset path to look up
// returns: pointer to the matching asset entry, or NULL if not found
const TQAssetEntry *
asset_lookup(const char *path)
{
  if(!path || !g_index_entries || g_index_header->num_entries == 0)
    return(NULL);

  uint32_t target_hash = calculate_hash(path);

  int low = 0;
  int high = (int)g_index_header->num_entries - 1;

  while(low <= high)
  {
    int mid = low + (high - low) / 2;

    if(mid < 0 || mid >= (int)g_index_header->num_entries)
      break;

    uint32_t mid_hash = g_index_entries[mid].hash;

    if(mid_hash == target_hash)
      return(&g_index_entries[mid]);

    if(mid_hash < target_hash)
      low = mid + 1;
    else
      high = mid - 1;
  }

  return(NULL);
}

// asset_get_num_files - get the total number of indexed game files
// returns: number of files in the index
int
asset_get_num_files(void)
{
  return(g_num_files);
}

// asset_get_file_path - get the relative file path for a file_id
// file_id: index into the file table
// returns: file path string (internal pointer, do not free), or NULL if invalid
const char *
asset_get_file_path(uint16_t file_id)
{
  if(file_id >= g_num_files)
    return(NULL);

  return(g_game_files[file_id]);
}
