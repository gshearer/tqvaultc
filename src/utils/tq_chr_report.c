// tq-chr-tool: the read-only reports -- dump, inv, equip, validate, hex.

#include "tq_chr_tool.h"

// ── Hex dump helper ──────────────────────────────────────────────────────

// print_hex_line -- print one 16-byte hex dump line with ASCII sidebar.
// data: binary data buffer.
// offset: starting offset in data.
// len: number of bytes to print (up to 16).
static void
print_hex_line(const uint8_t *data, size_t offset, size_t len)
{
  printf("  %08zx: ", offset);

  for(size_t i = 0; i < 16; i++)
  {
    if(i < len)
      printf("%02x ", data[offset + i]);
    else
      printf("   ");
    if(i == 7)
      printf(" ");
  }

  printf(" |");

  for(size_t i = 0; i < 16 && i < len; i++)
  {
    uint8_t c = data[offset + i];

    printf("%c", (c >= 32 && c < 127) ? c : '.');
  }

  printf("|\n");
}

// hex_dump_range -- print a hex dump of a byte range.
// data: binary data buffer.
// start: starting offset in data.
// len: number of bytes to dump.
void
hex_dump_range(const uint8_t *data, size_t start, size_t len)
{
  for(size_t off = 0; off < len; off += 16)
  {
    size_t chunk = (len - off) < 16 ? (len - off) : 16;

    print_hex_line(data, start + off, chunk);
  }
}

// ── Equipment slot names ─────────────────────────────────────────────────

// equip_slot_name -- return a human-readable name for an equipment slot index.
// slot: slot index (0-11).
// returns the slot name string.
const char *
equip_slot_name(int slot)
{
  static const char *names[] = {
    "Head", "Neck", "Chest", "Legs", "Arms",
    "Ring1", "Ring2", "Weapon1", "Shield1",
    "Weapon2", "Shield2", "Artifact"
  };

  if(slot >= 0 && slot < 12)
    return(names[slot]);

  return("Unknown");
}

// ── Basename tail: last path component for readable output ───────────────

// basename_tail -- return the last path component of a file path.
// path: full file path string.
// returns pointer to the last component, or "(empty)" if path is empty.
const char *
basename_tail(const char *path)
{
  if(!path || !*path)
    return("(empty)");

  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  const char *last = slash > bslash ? slash : bslash;

  return(last ? last + 1 : path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  COMMANDS
// ═══════════════════════════════════════════════════════════════════════════

// ── cmd_dump ─────────────────────────────────────────────────────────────

// cmd_dump -- raw key-value dump with file offsets, block depth, and value types.
// path: path to .chr file.
// returns 0 on success, 1 on error.
int
cmd_dump(const char *path)
{
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);

  if(!data)
    return(1);

  ChrEntryList entries;
  int count = parse_entries(data, file_size, &entries);

  printf("=== %s (%zu bytes, %d entries) ===\n\n", path, file_size, count);

  for(int i = 0; i < entries.count; i++)
  {
    const ChrEntry *e = &entries.entries[i];

    // Indentation by depth
    for(int d = 0; d < e->depth; d++)
      printf("  ");

    // Offset and key
    printf("@%06zx ", e->offset);

    if(strcmp(e->key, "begin_block") == 0)
    {
      printf("BEGIN_BLOCK (0x%08X)\n", e->u32_val);
      continue;
    }

    if(strcmp(e->key, "end_block") == 0)
    {
      printf("END_BLOCK (0x%08X)\n", e->u32_val);
      continue;
    }

    // Key name
    printf("%-40s = ", e->key);

    // Value
    switch(e->type)
    {
    case VAL_U32:
    {
      float fv;

      memcpy(&fv, &e->u32_val, 4);
      if(e->u32_val == 0)
        printf("0");
      else if(e->u32_val == 0xFFFFFFFF)
        printf("-1 (0xFFFFFFFF)");
      else if(e->u32_val < 100000)
        printf("%u (0x%08X)", e->u32_val, e->u32_val);
      else
        printf("0x%08X (%u)", e->u32_val, e->u32_val);
      break;
    }
    case VAL_FLOAT:
      printf("%.6f (0x%08X)", e->float_val, e->u32_val);
      break;
    case VAL_STRING:
      printf("\"%s\"", e->str_val);
      break;
    case VAL_UTF16:
      printf("u\"%s\"", e->str_val);
      break;
    }

    if(e->ambiguous)
      printf("  [AMBIGUOUS]");

    printf("\n");
  }

  entry_list_free(&entries);
  free(data);
  return(0);
}

// ── cmd_inv ──────────────────────────────────────────────────────────────

// cmd_inv -- inventory listing: per-sack items with all fields.
// path: path to .chr file.
// returns 0 on success, 1 on error.
int
cmd_inv(const char *path)
{
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);

  if(!data)
    return(1);

  ChrEntryList entries;

  parse_entries(data, file_size, &entries);

  RawChrParse parse;

  parse.data = data;
  parse.file_size = file_size;
  parse_chr_structured(&entries, &parse);

  printf("=== Inventory: %s ===\n", path);
  printf("numberOfSacks: %u\n", parse.num_sacks);
  printf("currentlyFocusedSackNumber: %u\n", parse.focused_sack);
  printf("currentlySelectedSackNumber: %u\n", parse.selected_sack);
  printf("inv_block: [%zu..%zu) = %zu bytes\n\n",
         parse.inv_start, parse.inv_end, parse.inv_end - parse.inv_start);

  for(uint32_t s = 0; s < parse.num_sacks && s < 8; s++)
  {
    RawSack *sack = &parse.sacks[s];

    printf("── Sack %u (declared_size=%u, actual=%d) ──\n",
           s, sack->declared_size, sack->actual_count);

    // Count unique items (collapse stacks with point_x=-1)
    int unique = 0;

    for(int i = 0; i < sack->actual_count; i++)
    {
      if(sack->items[i].point_x != -1 || sack->items[i].point_y != -1)
        unique++;
      else if(i == 0)
        unique++;  // edge case: first item at -1,-1
    }

    printf("  unique positions: %d, expanded entries: %d\n\n", unique, sack->actual_count);

    for(int i = 0; i < sack->actual_count; i++)
    {
      RawItem *it = &sack->items[i];

      printf("  [%d] @%06zx  pos=(%d,%d)  seed=0x%08X\n",
             i, it->offset, it->point_x, it->point_y, it->seed);
      if(it->base_name[0])
        printf("      base:   %s\n", it->base_name);
      if(it->prefix_name[0])
        printf("      prefix: %s\n", it->prefix_name);
      if(it->suffix_name[0])
        printf("      suffix: %s\n", it->suffix_name);
      if(it->relic_name[0])
        printf("      relic:  %s\n", it->relic_name);
      if(it->relic_bonus[0])
        printf("      bonus:  %s\n", it->relic_bonus);
      if(it->relic_name2[0])
        printf("      relic2: %s\n", it->relic_name2);
      if(it->relic_bonus2[0])
        printf("      bonus2: %s\n", it->relic_bonus2);
      if(it->var1 || it->var2)
        printf("      var1=%u  var2=0x%08X\n", it->var1, it->var2);
    }

    printf("\n");
  }

  free_chr_parse(&parse);
  entry_list_free(&entries);
  free(data);
  return(0);
}

// ── cmd_equip ────────────────────────────────────────────────────────────

// cmd_equip -- equipment listing: 12 slots with alternate flags.
// path: path to .chr file.
// returns 0 on success, 1 on error.
int
cmd_equip(const char *path)
{
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);

  if(!data)
    return(1);

  ChrEntryList entries;

  parse_entries(data, file_size, &entries);

  RawChrParse parse;

  parse.data = data;
  parse.file_size = file_size;
  parse_chr_structured(&entries, &parse);

  printf("=== Equipment: %s ===\n", path);
  printf("equipmentCtrlIOStreamVersion: %u\n", parse.equip_version);
  printf("equip_block: [%zu..%zu) = %zu bytes\n",
         parse.equip_start, parse.equip_end,
         parse.equip_end - parse.equip_start);
  printf("slots_parsed: %d\n\n", parse.slots_parsed);

  for(int i = 0; i < 12; i++)
  {
    RawEquipSlot *s = &parse.slots[i];

    printf("  [%2d] %-10s  attached=%d  alternate=%d",
           i, equip_slot_name(i), s->attached, s->alternate);

    if(s->base_name[0])
    {
      printf("  %s\n", basename_tail(s->base_name));
      printf("       base:   %s\n", s->base_name);
      if(s->prefix_name[0])
        printf("       prefix: %s\n", s->prefix_name);
      if(s->suffix_name[0])
        printf("       suffix: %s\n", s->suffix_name);
      if(s->relic_name[0])
        printf("       relic:  %s\n", s->relic_name);
      if(s->relic_bonus[0])
        printf("       bonus:  %s\n", s->relic_bonus);
      if(s->relic_name2[0])
        printf("       relic2: %s\n", s->relic_name2);
      if(s->relic_bonus2[0])
        printf("       bonus2: %s\n", s->relic_bonus2);
      printf("       seed=0x%08X  var1=%u  var2=0x%08X\n",
             s->seed, s->var1, s->var2);
    }
    else
    {
      printf("  (empty)");
      if(s->var2 != 0)
        printf("  var2=0x%08X", s->var2);
      printf("\n");
    }
  }

  free_chr_parse(&parse);
  entry_list_free(&entries);
  free(data);
  return(0);
}

// ── cmd_validate ─────────────────────────────────────────────────────────

// cmd_validate -- structural integrity checks on a .chr file.
// path: path to .chr file.
// returns 0 if no errors, 1 if errors found.
int
cmd_validate(const char *path)
{
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);

  if(!data)
    return(1);

  ChrEntryList entries;

  parse_entries(data, file_size, &entries);

  int errors = 0;
  int warnings = 0;

  printf("=== Validate: %s (%zu bytes, %d entries) ===\n\n",
         path, file_size, entries.count);

  // 1. Check block nesting
  int depth = 0;
  int max_depth = 0;
  int begin_count = 0;
  int end_count = 0;

  for(int i = 0; i < entries.count; i++)
  {
    const ChrEntry *e = &entries.entries[i];

    if(strcmp(e->key, "begin_block") == 0)
    {
      begin_count++;
      depth++;
      if(depth > max_depth)
        max_depth = depth;
      if(e->u32_val != TQ_BEGIN_BLOCK)
      {
        printf("  ERROR: begin_block @%06zx has unexpected sentinel 0x%08X\n",
               e->offset, e->u32_val);
        errors++;
      }
    }
    else if(strcmp(e->key, "end_block") == 0)
    {
      end_count++;
      depth--;
      if(depth < 0)
      {
        printf("  ERROR: end_block @%06zx causes negative depth\n", e->offset);
        errors++;
        depth = 0;
      }
      if(e->u32_val != TQ_END_BLOCK)
      {
        printf("  ERROR: end_block @%06zx has unexpected sentinel 0x%08X\n",
               e->offset, e->u32_val);
        errors++;
      }
    }
  }

  if(depth != 0)
  {
    printf("  ERROR: unclosed blocks -- final depth = %d\n", depth);
    errors++;
  }

  printf("  blocks: %d begin, %d end, max depth = %d\n",
         begin_count, end_count, max_depth);

  if(begin_count == end_count && depth == 0)
    printf("  block nesting: OK\n");

  // 2. Check for ambiguous keys
  int ambiguous_count = 0;

  for(int i = 0; i < entries.count; i++)
  {
    if(entries.entries[i].ambiguous)
      ambiguous_count++;
  }

  if(ambiguous_count > 0)
  {
    printf("\n  WARNING: %d keys used heuristic type detection (AMBIGUOUS)\n",
           ambiguous_count);
    warnings++;

    for(int i = 0; i < entries.count; i++)
    {
      const ChrEntry *e = &entries.entries[i];

      if(e->ambiguous)
      {
        printf("    @%06zx %-40s ", e->offset, e->key);
        if(e->type == VAL_STRING)
          printf("-> string \"%s\"\n", e->str_val);
        else
          printf("-> u32 %u (0x%08X)\n", e->u32_val, e->u32_val);
      }
    }
  }
  else
  {
    printf("  ambiguous keys: none (all keys recognized)\n");
  }

  // 3. Structured parse validation
  RawChrParse parse;

  parse.data = data;
  parse.file_size = file_size;
  parse_chr_structured(&entries, &parse);

  printf("\n  -- Inventory --\n");
  printf("  numberOfSacks: %u\n", parse.num_sacks);
  printf("  inv_block: [%zu..%zu)\n", parse.inv_start, parse.inv_end);

  if(parse.inv_start == 0)
  {
    printf("  ERROR: inventory section not found\n");
    errors++;
  }

  for(uint32_t s = 0; s < parse.num_sacks && s < 8; s++)
  {
    RawSack *sk = &parse.sacks[s];

    printf("  sack[%u]: declared=%u  actual=%d",
           s, sk->declared_size, sk->actual_count);

    if(sk->declared_size != (uint32_t)sk->actual_count)
    {
      printf("  ** MISMATCH **");
      warnings++;
    }

    printf("\n");
  }

  printf("\n  -- Equipment --\n");
  printf("  equip_block: [%zu..%zu)\n", parse.equip_start, parse.equip_end);
  printf("  version: %u, slots_parsed: %d\n",
         parse.equip_version, parse.slots_parsed);

  if(parse.equip_start == 0)
  {
    printf("  ERROR: equipment section not found\n");
    errors++;
  }

  if(parse.slots_parsed != 12)
  {
    printf("  ERROR: expected 12 equipment slots, got %d\n", parse.slots_parsed);
    errors++;
  }

  // Check weapon set ordering
  printf("\n  -- Weapon Sets --\n");

  for(int i = 7; i <= 10; i++)
  {
    printf("  slot[%d] %-10s  alternate=%d  attached=%d",
           i, equip_slot_name(i), parse.slots[i].alternate,
           parse.slots[i].attached);
    if(parse.slots[i].base_name[0])
      printf("  %s", basename_tail(parse.slots[i].base_name));
    printf("\n");
  }

  // Check boundary ordering
  printf("\n  -- Section Boundaries --\n");
  printf("  prefix:    [0..%zu)\n", parse.inv_start);
  printf("  inventory: [%zu..%zu)\n", parse.inv_start, parse.inv_end);

  if(parse.inv_end > 0 && parse.equip_start > 0)
  {
    printf("  middle:    [%zu..%zu) = %zu bytes\n",
           parse.inv_end, parse.equip_start,
           parse.equip_start - parse.inv_end);
  }

  printf("  equipment: [%zu..%zu)\n", parse.equip_start, parse.equip_end);
  printf("  suffix:    [%zu..%zu)\n", parse.equip_end, file_size);

  if(parse.inv_end > parse.equip_start && parse.inv_end > 0 && parse.equip_start > 0)
  {
    printf("  ERROR: inventory end (%zu) > equipment start (%zu)\n",
           parse.inv_end, parse.equip_start);
    errors++;
  }

  // Check empty slot var2 values
  printf("\n  -- Empty Slot var2 Values --\n");
  bool any_nonzero_var2 = false;

  for(int i = 0; i < 12; i++)
  {
    if(!parse.slots[i].base_name[0] && parse.slots[i].var2 != 0)
    {
      printf("  slot[%d] %-10s  var2=0x%08X (non-zero on empty slot)\n",
             i, equip_slot_name(i), parse.slots[i].var2);
      any_nonzero_var2 = true;
      warnings++;
    }
  }

  if(!any_nonzero_var2)
    printf("  (all empty slots have var2=0)\n");

  printf("\n  == Summary: %d errors, %d warnings ==\n", errors, warnings);

  free_chr_parse(&parse);
  entry_list_free(&entries);
  free(data);
  return(errors > 0 ? 1 : 0);
}

// ── cmd_hex ──────────────────────────────────────────────────────────────

// cmd_hex -- hex dump of named sections or arbitrary offsets.
// path: path to .chr file.
// section: section name ("prefix", "inventory", "middle", "equipment", "suffix")
//          or a numeric offset.
// len_str: optional length string (decimal or hex), or NULL for default 256.
// returns 0 on success, 1 on error.
int
cmd_hex(const char *path, const char *section, const char *len_str)
{
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);

  if(!data)
    return(1);

  size_t start = 0;
  size_t len = 0;

  // Try named section first
  if(strcmp(section, "prefix") == 0 || strcmp(section, "inventory") == 0 ||
     strcmp(section, "middle") == 0 || strcmp(section, "equipment") == 0 ||
     strcmp(section, "suffix") == 0)
  {
    // Need structural parse for boundaries
    ChrEntryList entries;

    parse_entries(data, file_size, &entries);

    RawChrParse parse;

    parse.data = data;
    parse.file_size = file_size;
    parse_chr_structured(&entries, &parse);

    if(parse.inv_start == 0 || parse.equip_start == 0)
    {
      fprintf(stderr, "error: could not determine section boundaries\n");
      entry_list_free(&entries);
      free(data);
      return(1);
    }

    if(strcmp(section, "prefix") == 0)
    {
      start = 0;
      len = parse.inv_start;
    }
    else if(strcmp(section, "inventory") == 0)
    {
      start = parse.inv_start;
      len = parse.inv_end - parse.inv_start;
    }
    else if(strcmp(section, "middle") == 0)
    {
      start = parse.inv_end;
      len = parse.equip_start - parse.inv_end;
    }
    else if(strcmp(section, "equipment") == 0)
    {
      start = parse.equip_start;
      len = parse.equip_end - parse.equip_start;
    }
    else if(strcmp(section, "suffix") == 0)
    {
      start = parse.equip_end;
      len = file_size - parse.equip_end;
    }

    printf("=== %s section: [%zu..%zu) = %zu bytes ===\n\n",
           section, start, start + len, len);

    free_chr_parse(&parse);
    entry_list_free(&entries);
  }
  else
  {
    // Numeric offset
    char *endptr;
    unsigned long off = strtoul(section, &endptr, 0);

    if(*endptr != '\0')
    {
      fprintf(stderr, "error: unknown section '%s'\n"
              "  valid sections: prefix, inventory, middle, equipment, suffix\n"
              "  or a numeric offset (decimal or 0x hex)\n", section);
      free(data);
      return(1);
    }

    start = (size_t)off;

    if(start >= file_size)
    {
      fprintf(stderr, "error: offset %zu beyond file size %zu\n",
              start, file_size);
      free(data);
      return(1);
    }

    len = len_str ? (size_t)strtoul(len_str, NULL, 0) : 256;

    if(start + len > file_size)
      len = file_size - start;

    printf("=== hex dump @%zu (0x%zx), %zu bytes ===\n\n", start, start, len);
  }

  hex_dump_range(data, start, len);

  free(data);
  return(0);
}
