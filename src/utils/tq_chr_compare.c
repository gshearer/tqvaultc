// tq-chr-tool: the structural diff between two .chr files.

#include "tq_chr_tool.h"

// ── cmd_compare ──────────────────────────────────────────────────────────

// compare_bytes -- compare two byte ranges and report differences.
// label: section label for output.
// a: first file data.
// a_off: offset into first file.
// a_len: length of first range.
// b: second file data.
// b_off: offset into second file.
// b_len: length of second range.
// diffs: pointer to difference counter (incremented on mismatch).
static void
compare_bytes(const char *label,
              const uint8_t *a, size_t a_off, size_t a_len,
              const uint8_t *b, size_t b_off, size_t b_len,
              int *diffs)
{
  if(a_len != b_len)
  {
    printf("  %-20s SIZE DIFFERS: %zu vs %zu bytes (delta %+zd)\n",
           label, a_len, b_len, (ssize_t)(b_len - a_len));
    (*diffs)++;
  }
  else if(memcmp(a + a_off, b + b_off, a_len) != 0)
  {
    // Find first difference
    size_t first_diff = 0;
    int diff_count = 0;

    for(size_t i = 0; i < a_len; i++)
    {
      if(a[a_off + i] != b[b_off + i])
      {
        if(diff_count == 0)
          first_diff = i;
        diff_count++;
      }
    }

    printf("  %-20s %d byte(s) differ (first at +%zu)\n",
           label, diff_count, first_diff);
    (*diffs)++;
  }
  else
  {
    printf("  %-20s identical (%zu bytes)\n", label, a_len);
  }
}

// compare_string_field -- compare two string fields and report if different.
// label: field label for output.
// a: first string value.
// b: second string value.
// diffs: pointer to difference counter (incremented on mismatch).
static void
compare_string_field(const char *label, const char *a,
                     const char *b, int *diffs)
{
  bool a_empty = (!a || !*a);
  bool b_empty = (!b || !*b);

  if(a_empty && b_empty)
    return;

  if(a_empty != b_empty || strcmp(a ? a : "", b ? b : "") != 0)
  {
    printf("      %s: \"%s\" -> \"%s\"\n", label,
           a_empty ? "" : a, b_empty ? "" : b);
    (*diffs)++;
  }
}

// compare_u32_field -- compare two uint32 fields and report if different.
// label: field label for output.
// a: first value.
// b: second value.
// diffs: pointer to difference counter (incremented on mismatch).
static void
compare_u32_field(const char *label, uint32_t a, uint32_t b,
                  int *diffs)
{
  if(a != b)
  {
    printf("      %s: %u (0x%08X) -> %u (0x%08X)\n", label, a, a, b, b);
    (*diffs)++;
  }
}

// cmd_compare -- structural diff between two .chr files (flagship debugging command).
// path_a: path to first .chr file.
// path_b: path to second .chr file.
// returns 0 if identical, 1 if differences found or on error.
int
cmd_compare(const char *path_a, const char *path_b)
{
  size_t size_a, size_b;
  uint8_t *data_a = load_file(path_a, &size_a);

  if(!data_a)
    return(1);

  uint8_t *data_b = load_file(path_b, &size_b);

  if(!data_b)
  {
    free(data_a);
    return(1);
  }

  ChrEntryList entries_a, entries_b;

  parse_entries(data_a, size_a, &entries_a);
  parse_entries(data_b, size_b, &entries_b);

  RawChrParse pa, pb;

  pa.data = data_a; pa.file_size = size_a;
  pb.data = data_b; pb.file_size = size_b;
  parse_chr_structured(&entries_a, &pa);
  parse_chr_structured(&entries_b, &pb);

  printf("=== Compare: %s vs %s ===\n\n", path_a, path_b);
  printf("  File A: %zu bytes, %d entries\n", size_a, entries_a.count);
  printf("  File B: %zu bytes, %d entries\n", size_b, entries_b.count);

  if(size_a != size_b)
    printf("  Size delta: %+zd bytes\n", (ssize_t)(size_b - size_a));

  printf("\n");

  int diffs = 0;

  // ── Pass 1: Section byte comparison ──
  printf("-- Section Comparison --\n");
  printf("  File A boundaries: inv=[%zu..%zu) equip=[%zu..%zu)\n",
         pa.inv_start, pa.inv_end, pa.equip_start, pa.equip_end);
  printf("  File B boundaries: inv=[%zu..%zu) equip=[%zu..%zu)\n",
         pb.inv_start, pb.inv_end, pb.equip_start, pb.equip_end);
  printf("\n");

  if(pa.inv_start == 0 || pb.inv_start == 0 ||
     pa.equip_start == 0 || pb.equip_start == 0)
  {
    printf("  ERROR: could not determine boundaries for both files\n");
    diffs++;
  }
  else
  {
    compare_bytes("prefix", data_a, 0, pa.inv_start,
                  data_b, 0, pb.inv_start, &diffs);
    compare_bytes("inventory", data_a, pa.inv_start,
                  pa.inv_end - pa.inv_start,
                  data_b, pb.inv_start,
                  pb.inv_end - pb.inv_start, &diffs);
    compare_bytes("middle", data_a, pa.inv_end,
                  pa.equip_start - pa.inv_end,
                  data_b, pb.inv_end,
                  pb.equip_start - pb.inv_end, &diffs);
    compare_bytes("equipment", data_a, pa.equip_start,
                  pa.equip_end - pa.equip_start,
                  data_b, pb.equip_start,
                  pb.equip_end - pb.equip_start, &diffs);
    compare_bytes("suffix", data_a, pa.equip_end,
                  size_a - pa.equip_end,
                  data_b, pb.equip_end,
                  size_b - pb.equip_end, &diffs);
  }

  // ── Pass 2: Inventory header comparison ──
  printf("\n-- Inventory Header --\n");
  compare_u32_field("numberOfSacks", pa.num_sacks, pb.num_sacks, &diffs);
  compare_u32_field("focusedSack", pa.focused_sack, pb.focused_sack, &diffs);
  compare_u32_field("selectedSack", pa.selected_sack, pb.selected_sack, &diffs);

  // ── Pass 3: Per-sack item comparison ──
  uint32_t max_sacks = pa.num_sacks > pb.num_sacks ? pa.num_sacks : pb.num_sacks;

  if(max_sacks > 8)
    max_sacks = 8;

  for(uint32_t s = 0; s < max_sacks; s++)
  {
    printf("\n-- Sack %u --\n", s);
    RawSack *sa = (s < pa.num_sacks) ? &pa.sacks[s] : NULL;
    RawSack *sb = (s < pb.num_sacks) ? &pb.sacks[s] : NULL;

    if(!sa) { printf("  MISSING in file A\n"); diffs++; continue; }
    if(!sb) { printf("  MISSING in file B\n"); diffs++; continue; }

    compare_u32_field("declared_size", sa->declared_size,
                      sb->declared_size, &diffs);

    if(sa->actual_count != sb->actual_count)
    {
      printf("      actual_count: %d -> %d\n", sa->actual_count, sb->actual_count);
      diffs++;
    }

    // Match items by base_name + position. Heap-allocate sized to the actual
    // B item count — a fixed stack array overflowed on large sacks (a single
    // sack of stacked potions expands well past any fixed cap).
    int *matched_b = calloc((size_t)(sb->actual_count > 0 ? sb->actual_count : 1),
                            sizeof(int));

    if(!matched_b)
    {
      fprintf(stderr, "error: out of memory comparing sack %d\n", s);
      continue;
    }

    int max_items = sa->actual_count > sb->actual_count ?
                    sa->actual_count : sb->actual_count;

    for(int ia = 0; ia < sa->actual_count; ia++)
    {
      RawItem *a = &sa->items[ia];

      // Find matching item in B
      int ib_match = -1;

      for(int ib = 0; ib < sb->actual_count; ib++)
      {
        if(matched_b[ib])
          continue;

        RawItem *b = &sb->items[ib];

        if(strcmp(a->base_name, b->base_name) == 0 &&
           a->point_x == b->point_x && a->point_y == b->point_y)
        {
          ib_match = ib;
          break;
        }
      }

      if(ib_match < 0)
      {
        // Try looser match: same base_name, same position coords
        for(int ib = 0; ib < sb->actual_count; ib++)
        {
          if(matched_b[ib])
            continue;

          if(strcmp(a->base_name, sb->items[ib].base_name) == 0)
          {
            ib_match = ib;
            break;
          }
        }
      }

      if(ib_match >= 0)
      {
        matched_b[ib_match] = 1;
        RawItem *b = &sb->items[ib_match];
        int item_diffs = 0;

        // Compare all fields
        int local_diffs = 0;

        compare_string_field("baseName", a->base_name, b->base_name, &local_diffs);
        compare_string_field("prefixName", a->prefix_name, b->prefix_name, &local_diffs);
        compare_string_field("suffixName", a->suffix_name, b->suffix_name, &local_diffs);
        compare_string_field("relicName", a->relic_name, b->relic_name, &local_diffs);
        compare_string_field("relicBonus", a->relic_bonus, b->relic_bonus, &local_diffs);
        compare_string_field("relicName2", a->relic_name2, b->relic_name2, &local_diffs);
        compare_string_field("relicBonus2", a->relic_bonus2, b->relic_bonus2, &local_diffs);
        compare_u32_field("seed", a->seed, b->seed, &local_diffs);
        compare_u32_field("var1", a->var1, b->var1, &local_diffs);
        compare_u32_field("var2", a->var2, b->var2, &local_diffs);

        if(a->point_x != b->point_x || a->point_y != b->point_y)
        {
          printf("      position: (%d,%d) -> (%d,%d)\n",
                 a->point_x, a->point_y, b->point_x, b->point_y);
          local_diffs++;
        }

        item_diffs = local_diffs;

        if(item_diffs > 0)
        {
          printf("    item[%d] %s: %d difference(s)\n",
                 ia, basename_tail(a->base_name), item_diffs);
          diffs += item_diffs;
        }
      }
      else
      {
        printf("    item[%d] ONLY IN A: %s at (%d,%d)\n",
               ia, basename_tail(a->base_name), a->point_x, a->point_y);
        diffs++;
      }
    }

    // Report unmatched B items
    for(int ib = 0; ib < sb->actual_count; ib++)
    {
      if(!matched_b[ib])
      {
        RawItem *b = &sb->items[ib];

        printf("    item[%d] ONLY IN B: %s at (%d,%d)\n",
               ib, basename_tail(b->base_name), b->point_x, b->point_y);
        diffs++;
      }
    }

    (void)max_items;
    free(matched_b);
  }

  // ── Pass 4: Equipment comparison ──
  printf("\n-- Equipment --\n");
  compare_u32_field("version", pa.equip_version, pb.equip_version, &diffs);

  for(int i = 0; i < 12; i++)
  {
    RawEquipSlot *a = &pa.slots[i];
    RawEquipSlot *b = &pb.slots[i];
    int slot_diffs = 0;
    int local_diffs = 0;

    compare_string_field("baseName", a->base_name, b->base_name, &local_diffs);
    compare_string_field("prefixName", a->prefix_name, b->prefix_name, &local_diffs);
    compare_string_field("suffixName", a->suffix_name, b->suffix_name, &local_diffs);
    compare_string_field("relicName", a->relic_name, b->relic_name, &local_diffs);
    compare_string_field("relicBonus", a->relic_bonus, b->relic_bonus, &local_diffs);
    compare_string_field("relicName2", a->relic_name2, b->relic_name2, &local_diffs);
    compare_string_field("relicBonus2", a->relic_bonus2, b->relic_bonus2, &local_diffs);
    compare_u32_field("seed", a->seed, b->seed, &local_diffs);
    compare_u32_field("var1", a->var1, b->var1, &local_diffs);
    compare_u32_field("var2", a->var2, b->var2, &local_diffs);

    if(a->attached != b->attached)
    {
      printf("      attached: %d -> %d\n", a->attached, b->attached);
      local_diffs++;
    }

    if(a->alternate != b->alternate)
    {
      printf("      alternate: %d -> %d\n", a->alternate, b->alternate);
      local_diffs++;
    }

    slot_diffs = local_diffs;

    if(slot_diffs > 0)
    {
      printf("  slot[%2d] %-10s: %d difference(s)\n",
             i, equip_slot_name(i), slot_diffs);
      diffs += slot_diffs;
    }
  }

  // ── Summary ──
  printf("\n== Summary: %d total differences ==\n", diffs);

  free_chr_parse(&pa);
  free_chr_parse(&pb);
  entry_list_free(&entries_a);
  entry_list_free(&entries_b);
  free(data_a);
  free(data_b);
  return(diffs > 0 ? 1 : 0);
}
