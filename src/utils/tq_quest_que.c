// tq-quest-tool: the .que state-machine files and Quest.myw -- structural
// dumps, the flag clear, and the directory compare/categorize reports.

#include "tq_quest_tool.h"

// ── .que file helpers ────────────────────────────────────────────────────

// que_read_key -- read a length-prefixed key from a .que/.myw binary blob.
// Returns the key string (malloc'd) and advances *off past the key.
// Does NOT skip the value -- caller does that.
// data: binary data buffer.
// len: total buffer length.
// off: pointer to current offset (updated on return).
// returns malloc'd key string, or NULL on EOF/error.
char *
que_read_key(const uint8_t *data, size_t len, size_t *off)
{
  if(*off + 4 > len)
    return(NULL);

  uint32_t slen;

  memcpy(&slen, data + *off, 4);
  *off += 4;

  if(slen == 0 || *off + slen > len)
    return(NULL);

  char *s = malloc(slen + 1);

  if(!s)
    return(NULL);

  memcpy(s, data + *off, slen);
  s[slen] = '\0';
  *off += slen;
  return(s);
}

// que_read_u32 -- read a little-endian uint32 from binary data and advance offset.
// data: binary data buffer.
// off: pointer to current offset (updated on return).
// returns the uint32 value.
uint32_t
que_read_u32(const uint8_t *data, size_t *off)
{
  uint32_t val;

  memcpy(&val, data + *off, 4);
  *off += 4;
  return(val);
}

// read_file -- read an entire file into a malloc'd buffer.
// path: path to the file.
// out_size: receives the file size in bytes.
// returns malloc'd buffer, or NULL on error.
uint8_t *
read_file(const char *path, long *out_size)
{
  struct stat st;
  FILE *f = fopen(path, "rb");

  if(!f)
    return(NULL);

  // A directory opens fine here, and then ftell reports LONG_MAX.
  if(fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode))
  {
    fclose(f);
    return(NULL);
  }

  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);

  if(fsize <= 0)
  {
    fclose(f);
    return(NULL);
  }

  rewind(f);

  uint8_t *data = malloc(fsize);

  if(!data)
  {
    fclose(f);
    return(NULL);
  }

  if(fread(data, 1, fsize, f) != (size_t)fsize)
  {
    free(data);
    fclose(f);
    return(NULL);
  }

  fclose(f);
  *out_size = fsize;
  return(data);
}

// cmd_dump_que -- full structural dump of a .que file (all fields).
// path: path to .que file.
// returns 0 on success, 1 on error.
int
cmd_dump_que(const char *path)
{
  long fsize;
  uint8_t *data = read_file(path, &fsize);

  if(!data)
  {
    fprintf(stderr, "Error: cannot read %s\n", path);
    return(1);
  }

  int depth = 0;
  int has_fired_count = 0, pending_fire_count = 0;
  int trigger_count = 0, condition_count = 0, action_count = 0;
  size_t off = 0;

  while(off + 4 <= (size_t)fsize)
  {
    char *key = que_read_key(data, fsize, &off);

    if(!key)
      break;

    if(strcmp(key, "begin_block") == 0)
    {
      if(off + 4 <= (size_t)fsize)
      {
        uint32_t marker = que_read_u32(data, &off);

        printf("%*s{ (0x%08X)\n", depth * 2, "", marker);
      }
      depth++;
      free(key);
      continue;
    }

    if(strcmp(key, "end_block") == 0)
    {
      depth--;
      if(off + 4 <= (size_t)fsize)
        off += 4; // skip marker
      printf("%*s}\n", depth * 2, "");
      free(key);
      continue;
    }

    // "comments" has a string value (LP-string)
    if(strcmp(key, "comments") == 0)
    {
      if(off + 4 > (size_t)fsize)
      {
        free(key);
        break;
      }

      uint32_t slen = que_read_u32(data, &off);

      if(slen > 0 && off + slen <= (size_t)fsize)
      {
        char *s = malloc(slen + 1);

        if(!s)
        {
          free(key);
          break;
        }

        memcpy(s, data + off, slen);
        s[slen] = '\0';
        printf("%*scomments = \"%s\"\n", depth * 2, "", s);
        free(s);
      }
      else
      {
        printf("%*scomments = \"\"\n", depth * 2, "");
      }

      off += slen;
      free(key);
      continue;
    }

    if(off + 4 > (size_t)fsize)
    {
      free(key);
      break;
    }

    uint32_t val = que_read_u32(data, &off);

    printf("%*s%s = %u", depth * 2, "", key, val);

    if(strcmp(key, "hasFired") == 0)
    {
      has_fired_count++;
      trigger_count++;
    }
    else if(strcmp(key, "isPendingFire") == 0)
      pending_fire_count++;
    else if(strcmp(key, "conditionCount") == 0)
      condition_count += val;
    else if(strcmp(key, "actionCount") == 0)
      action_count += val;
    else if(strcmp(key, "crcFile") == 0)
      printf(" (0x%08X)", val);

    printf("\n");
    free(key);
  }

  printf("\n--- Summary: %d triggers, %d hasFired, %d isPendingFire, "
         "%d conditions, %d actions\n",
         trigger_count, has_fired_count, pending_fire_count,
         condition_count, action_count);
  printf("--- File: %s (%ld bytes)\n", path, fsize);
  free(data);
  return(0);
}

// md5_chunks_to_hex -- format 4 MD5 chunks as a hex filename string.
// Each chunk is stored as a LE u32, but .que filenames use BE byte order per chunk.
// chunks: array of 4 uint32 values.
// hex: output buffer (must be at least 33 bytes).
static void
md5_chunks_to_hex(const uint32_t chunks[4], char hex[33])
{
  for(int i = 0; i < 4; i++)
  {
    uint8_t b[4];

    memcpy(b, &chunks[i], 4);
    // Reverse byte order within each chunk (LE->BE)
    snprintf(hex + i * 8, 9, "%02x%02x%02x%02x", b[3], b[2], b[1], b[0]);
  }

  hex[32] = '\0';
}

// qmyw_expect_key -- read a Quest.myw LP-key and check if it matches expected.
// data: binary data buffer.
// len: total buffer length.
// off: pointer to current offset (updated on return).
// expected: expected key string.
// returns true if the key matches expected.
bool
qmyw_expect_key(const uint8_t *data, size_t len, size_t *off, const char *expected)
{
  if(*off + 4 > len)
    return(false);

  uint32_t slen;

  memcpy(&slen, data + *off, 4);
  *off += 4;

  if(*off + slen > len)
    return(false);

  bool match = (slen == strlen(expected) && memcmp(data + *off, expected, slen) == 0);

  *off += slen;
  return(match);
}

// cmd_dump_quest_myw -- full Quest.myw parser (triggers + rewards + MD5 mapping).
// path: path to Quest.myw file.
// returns 0 on success, 1 on error.
int
cmd_dump_quest_myw(const char *path)
{
  long fsize;
  uint8_t *data = read_file(path, &fsize);

  if(!data)
  {
    fprintf(stderr, "Error: cannot read %s\n", path);
    return(1);
  }

  size_t off = 0;

  // --- Triggers section ---
  if(!qmyw_expect_key(data, fsize, &off, "begin_block"))
  {
    fprintf(stderr, "Error: bad format (expected begin_block)\n");
    free(data);
    return(1);
  }

  off += 4; // skip marker

  if(!qmyw_expect_key(data, fsize, &off, "numberOfTriggers"))
  {
    fprintf(stderr, "Error: bad format (expected numberOfTriggers)\n");
    free(data);
    return(1);
  }

  uint32_t num_triggers = que_read_u32(data, &off);

  printf("=== Triggers (%u entries) ===\n", num_triggers);

  // Track unique MD5 hashes
  char (*md5s)[33] = malloc(num_triggers * sizeof(*md5s));

  if(!md5s)
  {
    fprintf(stderr, "Error: malloc failed\n");
    free(data);
    return(1);
  }

  int unique_md5_count = 0;

  for(uint32_t i = 0; i < num_triggers && off + 4 <= (size_t)fsize; i++)
  {
    // questName -- just a key with no value (next key follows immediately)
    qmyw_expect_key(data, fsize, &off, "questName");

    // md5ChunkCount
    qmyw_expect_key(data, fsize, &off, "md5ChunkCount");
    uint32_t chunk_count = que_read_u32(data, &off);

    uint32_t chunks[4] = {0};

    for(uint32_t c = 0; c < chunk_count && c < 4; c++)
    {
      qmyw_expect_key(data, fsize, &off, "md5Chunk");
      chunks[c] = que_read_u32(data, &off);
    }

    char hex[33];

    md5_chunks_to_hex(chunks, hex);

    // stepIdx, triggerIdx, target
    qmyw_expect_key(data, fsize, &off, "stepIdx");
    uint32_t step_idx = que_read_u32(data, &off);

    qmyw_expect_key(data, fsize, &off, "triggerIdx");
    uint32_t trigger_idx = que_read_u32(data, &off);

    qmyw_expect_key(data, fsize, &off, "target");
    uint32_t target = que_read_u32(data, &off);

    printf("  [%4u] %s.que  step=%u trig=%u target=%u\n",
           i, hex, step_idx, trigger_idx, target);

    // Track unique MD5s
    bool found = false;

    for(int j = 0; j < unique_md5_count; j++)
    {
      if(strcmp(md5s[j], hex) == 0)
      {
        found = true;
        break;
      }
    }

    if(!found)
    {
      memcpy(md5s[unique_md5_count], hex, 33);
      unique_md5_count++;
    }
  }

  // end_block for triggers
  qmyw_expect_key(data, fsize, &off, "end_block");
  off += 4; // skip marker

  printf("\n--- %u triggers, %d unique .que files\n\n", num_triggers, unique_md5_count);

  // --- Rewards section ---
  if(!qmyw_expect_key(data, fsize, &off, "begin_block"))
  {
    printf("(No rewards section found)\n");
    free(md5s); free(data);
    return(0);
  }

  off += 4; // skip marker

  if(!qmyw_expect_key(data, fsize, &off, "numRewards"))
  {
    printf("(Expected numRewards)\n");
    free(md5s); free(data);
    return(0);
  }

  uint32_t num_rewards = que_read_u32(data, &off);

  printf("=== Rewards (%u entries) ===\n", num_rewards);

  for(uint32_t i = 0; i < num_rewards && off + 4 <= (size_t)fsize; i++)
  {
    qmyw_expect_key(data, fsize, &off, "questName");

    qmyw_expect_key(data, fsize, &off, "md5ChunkCount");
    uint32_t chunk_count = que_read_u32(data, &off);
    uint32_t chunks[4] = {0};

    for(uint32_t c = 0; c < chunk_count && c < 4; c++)
    {
      qmyw_expect_key(data, fsize, &off, "md5Chunk");
      chunks[c] = que_read_u32(data, &off);
    }

    char hex[33];

    md5_chunks_to_hex(chunks, hex);

    // region
    qmyw_expect_key(data, fsize, &off, "region");
    uint32_t region = que_read_u32(data, &off);

    // locationTag (LP-string)
    qmyw_expect_key(data, fsize, &off, "locationTag");

    if(off + 4 > (size_t)fsize)
      break;

    uint32_t loc_len = que_read_u32(data, &off);
    char *loc_tag = NULL;

    if(loc_len > 0 && off + loc_len <= (size_t)fsize)
    {
      loc_tag = malloc(loc_len + 1);

      if(loc_tag)
      {
        memcpy(loc_tag, data + off, loc_len);
        loc_tag[loc_len] = '\0';
      }
    }

    off += loc_len;

    // titleTag (LP-string)
    qmyw_expect_key(data, fsize, &off, "titleTag");

    if(off + 4 > (size_t)fsize)
    {
      free(loc_tag);
      break;
    }

    uint32_t title_len = que_read_u32(data, &off);
    char *title_tag = NULL;

    if(title_len > 0 && off + title_len <= (size_t)fsize)
    {
      title_tag = malloc(title_len + 1);

      if(title_tag)
      {
        memcpy(title_tag, data + off, title_len);
        title_tag[title_len] = '\0';
      }
    }

    off += title_len;

    // text (UTF-16LE string -- LP value is CHARACTER count, bytes = count * 2)
    qmyw_expect_key(data, fsize, &off, "text");

    if(off + 4 > (size_t)fsize)
    {
      free(loc_tag); free(title_tag);
      break;
    }

    uint32_t text_chars = que_read_u32(data, &off);
    uint32_t text_bytes = text_chars * 2;

    // Decode UTF-16LE to ASCII for display
    char *text_str = NULL;

    if(text_bytes > 0 && off + text_bytes <= (size_t)fsize)
    {
      text_str = malloc(text_chars + 1);

      if(text_str)
      {
        for(uint32_t c = 0; c < text_chars; c++)
        {
          uint16_t ch;

          memcpy(&ch, data + off + c * 2, 2);
          text_str[c] = (ch < 128) ? (char)ch : '?';
        }
        text_str[text_chars] = '\0';
      }
    }

    off += text_bytes;

    const char *region_name = "?";

    switch(region)
    {
    case 1: region_name = "Greece"; break;
    case 2: region_name = "Egypt"; break;
    case 3: region_name = "Orient"; break;
    case 4: region_name = "Hades"; break;
    case 5: region_name = "Ragnarok"; break;
    case 6: region_name = "Atlantis"; break;
    case 7: region_name = "EternalEmbers"; break;
    }

    printf("  [%3u] %s.que  region=%u(%s)  loc=\"%s\"  title=\"%s\"  text=\"%s\"\n",
           i, hex, region, region_name,
           loc_tag ? loc_tag : "", title_tag ? title_tag : "",
           text_str ? text_str : "");

    free(loc_tag);
    free(title_tag);
    free(text_str);
  }

  printf("\n--- %u triggers, %d unique .que files, %u rewards\n",
         num_triggers, unique_md5_count, num_rewards);
  printf("--- File: %s (%ld bytes)\n", path, fsize);
  free(md5s);
  free(data);
  return(0);
}

// cmd_clear_que -- zero all hasFired/isPendingFire in .que files and clear Quest.myw.
// dir: directory containing .que files.
// returns 0 on success, 1 on error.
int
cmd_clear_que(const char *dir)
{
  int result = quest_que_clear_all(dir);

  if(result < 0)
  {
    fprintf(stderr, "Error: failed to clear .que files in %s\n", dir);
    return(1);
  }

  printf("Modified %d .que files in %s\n", result, dir);

  // Also show Quest.myw clearing
  if(quest_myw_clear(dir) == 0)
    printf("Wrote empty Quest.myw\n");
  else
    fprintf(stderr, "Warning: failed to clear Quest.myw\n");

  return(0);
}

// cmd_compare_que -- compare .que flag differences between two directories.
// dir_a: path to first directory.
// dir_b: path to second directory.
// returns 0 on success, 1 on error.
int
cmd_compare_que(const char *dir_a, const char *dir_b)
{
  GDir *d = g_dir_open(dir_a, 0, NULL);

  if(!d)
  {
    fprintf(stderr, "Error: cannot open %s\n", dir_a);
    return(1);
  }

  int files_compared = 0, files_differ = 0;
  const gchar *ent_name;

  while((ent_name = g_dir_read_name(d)) != NULL)
  {
    size_t nlen = strlen(ent_name);

    if(nlen < 5 || strcmp(ent_name + nlen - 4, ".que") != 0)
      continue;

    char *path_a = g_build_filename(dir_a, ent_name, NULL);
    char *path_b = g_build_filename(dir_b, ent_name, NULL);

    FILE *fa = fopen(path_a, "rb");
    FILE *fb = fopen(path_b, "rb");

    if(!fa || !fb)
    {
      if(fa)
      {
        printf("  %s: only in dir_a\n", ent_name);
        fclose(fa);
      }
      else if(fb)
      {
        printf("  %s: only in dir_b\n", ent_name);
        fclose(fb);
      }
      g_free(path_a); g_free(path_b);
      continue;
    }

    g_free(path_a); g_free(path_b);

    fseek(fa, 0, SEEK_END); long sa = ftell(fa); rewind(fa);
    fseek(fb, 0, SEEK_END); long sb = ftell(fb); rewind(fb);

    uint8_t *da = slurp_stream(fa, sa);
    uint8_t *db = slurp_stream(fb, sb);

    fclose(fa); fclose(fb);

    if(!da || !db)
    {
      free(da);
      free(db);
      continue;
    }

    if(sa != sb)
    {
      printf("  %s: size differs (%ld vs %ld)\n", ent_name, sa, sb);
      files_differ++;
    }
    else
    {
      bool differ = false;
      static const struct { const char *key; size_t klen; } targets[] = {
        { "hasFired", 8 }, { "isPendingFire", 13 },
      };

      for(int t = 0; t < 2; t++)
      {
        const char *key = targets[t].key;
        size_t klen = targets[t].klen;
        size_t oa = 0, ob = 0;
        int idx = 0;

        while(1)
        {
          size_t pa = (size_t)-1, pb = (size_t)-1;

          for(size_t i = oa; i + 4 + klen + 4 <= (size_t)sa; i++)
          {
            uint32_t slen;

            memcpy(&slen, da + i, 4);
            if(slen == (uint32_t)klen && memcmp(da + i + 4, key, klen) == 0)
            {
              pa = i;
              break;
            }
          }

          for(size_t i = ob; i + 4 + klen + 4 <= (size_t)sb; i++)
          {
            uint32_t slen;

            memcpy(&slen, db + i, 4);
            if(slen == (uint32_t)klen && memcmp(db + i + 4, key, klen) == 0)
            {
              pb = i;
              break;
            }
          }

          if(pa == (size_t)-1 || pb == (size_t)-1)
            break;

          uint32_t va, vb;

          memcpy(&va, da + pa + 4 + klen, 4);
          memcpy(&vb, db + pb + 4 + klen, 4);

          if(va != vb)
          {
            if(!differ)
              printf("  %s:\n", ent_name);
            printf("    %s[%d]: %u vs %u\n", key, idx, va, vb);
            differ = true;
          }

          oa = pa + 4 + klen + 4;
          ob = pb + 4 + klen + 4;
          idx++;
        }
      }

      if(differ)
        files_differ++;
    }

    files_compared++;
    free(da); free(db);
  }

  g_dir_close(d);

  printf("\n--- %d files compared, %d differ\n", files_compared, files_differ);
  return(0);
}

// ── que-info: categorize all .que files in a directory ────────────────────

// cmd_que_info -- identify/categorize all .que files in a directory.
// Extracts embedded info: comments strings, flag counts, embedded .qst paths.
// dir: path to directory containing .que files.
// returns 0 on success, 1 on error.
int
cmd_que_info(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);

  if(!d)
  {
    fprintf(stderr, "Error: cannot open %s\n", dir);
    return(1);
  }

  struct que_entry {
    char filename[40];
    int has_fired_total;
    int has_fired_set;
    int pending_total;
    int pending_set;
    int trigger_count;
    char embedded_path[256];
    long filesize;
  };

  struct que_entry *entries = NULL;
  int nentries = 0, cap = 0;
  const gchar *ent_name;

  while((ent_name = g_dir_read_name(d)) != NULL)
  {
    size_t nlen = strlen(ent_name);

    if(nlen < 5 || strcmp(ent_name + nlen - 4, ".que") != 0)
      continue;

    char *filepath = g_build_filename(dir, ent_name, NULL);

    long fsize;
    uint8_t *data = read_file(filepath, &fsize);

    g_free(filepath);

    if(!data)
      continue;

    if(nentries >= cap)
    {
      cap = cap ? cap * 2 : 256;
      entries = realloc(entries, cap * sizeof(*entries));

      if(!entries)
      {
        free(data);
        g_dir_close(d);
        return(1);
      }
    }

    struct que_entry *e = &entries[nentries++];

    memset(e, 0, sizeof(*e));
    snprintf(e->filename, sizeof(e->filename), "%.*s", (int)(nlen - 4), ent_name);
    e->filesize = fsize;

    // Scan for keys
    for(size_t off = 0; off + 8 <= (size_t)fsize; )
    {
      uint32_t slen;

      memcpy(&slen, data + off, 4);

      if(slen == 0 || slen > 256 || off + 4 + slen > (size_t)fsize)
      {
        off++;
        continue;
      }

      if(slen == 8 && memcmp(data + off + 4, "hasFired", 8) == 0)
      {
        e->has_fired_total++;

        if(off + 4 + 8 + 4 <= (size_t)fsize)
        {
          uint32_t val;

          memcpy(&val, data + off + 4 + 8, 4);
          if(val)
            e->has_fired_set++;
        }

        off += 4 + 8 + 4;
      }
      else if(slen == 13 && memcmp(data + off + 4, "isPendingFire", 13) == 0)
      {
        e->pending_total++;

        if(off + 4 + 13 + 4 <= (size_t)fsize)
        {
          uint32_t val;

          memcpy(&val, data + off + 4 + 13, 4);
          if(val)
            e->pending_set++;
        }

        off += 4 + 13 + 4;
      }
      else if(slen == 8 && memcmp(data + off + 4, "comments", 8) == 0)
      {
        size_t coff = off + 4 + 8;

        if(coff + 4 <= (size_t)fsize)
        {
          uint32_t clen;

          memcpy(&clen, data + coff, 4);
          coff += 4;

          if(clen > 0 && coff + clen <= (size_t)fsize && !e->embedded_path[0])
          {
            // Check if comment contains .qst path
            char *tmp = malloc(clen + 1);

            if(tmp)
            {
              memcpy(tmp, data + coff, clen);
              tmp[clen] = '\0';

              if(strstr(tmp, ".qst") || strstr(tmp, ".QST"))
                snprintf(e->embedded_path, sizeof(e->embedded_path), "%s", tmp);

              free(tmp);
            }
          }

          off = coff + clen;
        }
        else
        {
          off++;
        }
      }
      else
      {
        off++;
      }
    }

    // Count triggers (each hasFired = one trigger)
    e->trigger_count = e->has_fired_total;
    free(data);
  }

  g_dir_close(d);

  // Sort by filename
  for(int i = 0; i < nentries - 1; i++)
  {
    for(int j = i + 1; j < nentries; j++)
    {
      if(strcmp(entries[i].filename, entries[j].filename) > 0)
      {
        struct que_entry tmp = entries[i];

        entries[i] = entries[j];
        entries[j] = tmp;
      }
    }
  }

  // Print results
  int with_path = 0, with_fired = 0, total_fired = 0, total_pending = 0;

  printf("=== .que File Analysis (%d files) ===\n\n", nentries);

  printf("%-34s %5s %8s %10s %s\n",
         "MD5 Hash", "Trigs", "Fired", "Pending", "Embedded Path");
  printf("%-34s %5s %8s %10s %s\n",
         "──────────────────────────────────", "─────", "────────", "──────────",
         "──────────────");

  for(int i = 0; i < nentries; i++)
  {
    struct que_entry *e = &entries[i];

    printf("%-34s %5d %4d/%-3d %5d/%-4d %s\n",
           e->filename,
           e->trigger_count,
           e->has_fired_set, e->has_fired_total,
           e->pending_set, e->pending_total,
           e->embedded_path[0] ? e->embedded_path : "-");
    if(e->embedded_path[0])
      with_path++;
    if(e->has_fired_set > 0)
      with_fired++;
    total_fired += e->has_fired_set;
    total_pending += e->pending_set;
  }

  printf("\n--- Summary ---\n");
  printf("  Total .que files:     %d\n", nentries);
  printf("  With embedded paths:  %d\n", with_path);
  printf("  With fired triggers:  %d\n", with_fired);
  printf("  Total fired flags:    %d\n", total_fired);
  printf("  Total pending flags:  %d\n", total_pending);

  free(entries);
  return(0);
}
