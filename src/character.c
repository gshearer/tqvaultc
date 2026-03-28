#include "character.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

// String-valued keys that reach the default handler (not explicitly parsed).
// Used to fix Bug 1: the old heuristic misinterpreted small integer values
// (1-511) as string length prefixes, corrupting the parse stream.
static const char *const default_string_keys[] = {
  "playerTexture", "greatestMonsterKilledName",
  "lastMonsterHit", "lastMonsterHitBy", "teleportUIDsName",
  "itemName", "bitmapDownName", "bitmapUpName", "defaultText",
  "scrollName", "description", "oTokens", "playerClassTag",
  "uniqueId", "streamData", NULL
};

// Check whether key is in the default_string_keys table.
// key: the key name to look up.
// Returns: 1 if found, 0 otherwise.
static int
is_default_string_key(const char *key)
{
  for(int i = 0; default_string_keys[i]; i++)
    if(strcmp(key, default_string_keys[i]) == 0)
      return(1);

  return(0);
}

// Read a little-endian uint32 from data at the given byte offset.
// data: raw byte buffer.
// offset: byte position to read from.
// Returns: the decoded uint32 value.
static uint32_t
read_u32(const uint8_t *data, size_t offset)
{
  uint32_t val;

  memcpy(&val, data + offset, 4);
  return(val);
}

// Read a length-prefixed ASCII string from data at offset.
// data: raw byte buffer.
// offset: byte position of the 4-byte length prefix.
// next_offset: if non-NULL, set to the byte position after the string.
// Returns: allocated string, or NULL if length is 0 or > 1024.
static char *
read_string(const uint8_t *data, size_t offset, size_t *next_offset)
{
  uint32_t len = read_u32(data, offset);

  if(len == 0 || len > 1024)
  {
    if(next_offset)
      *next_offset = offset + 4;
    return(NULL);
  }

  char *str = malloc(len + 1);

  if(!str)
  {
    if(next_offset)
      *next_offset = offset + 4;
    return(NULL);
  }

  memcpy(str, data + offset + 4, len);
  str[len] = '\0';

  if(next_offset)
    *next_offset = offset + 4 + len;

  return(str);
}

// Read a length-prefixed UTF-16LE string, converting to ASCII (low byte only).
// data: raw byte buffer.
// offset: byte position of the 4-byte character count prefix.
// next_offset: if non-NULL, set to the byte position after the string.
// Returns: allocated ASCII string, or NULL if length is 0 or > 1024.
static char *
read_string_utf16_as_ascii(const uint8_t *data, size_t offset, size_t *next_offset)
{
  uint32_t len = read_u32(data, offset);

  if(len == 0 || len > 1024)
  {
    if(next_offset)
      *next_offset = offset + 4;
    return(NULL);
  }

  char *str = malloc(len + 1);

  if(!str)
  {
    if(next_offset)
      *next_offset = offset + 4;
    return(NULL);
  }

  for(uint32_t i = 0; i < len; i++)
    str[i] = (char)data[offset + 4 + i * 2];

  str[len] = '\0';

  if(next_offset)
    *next_offset = offset + 4 + len * 2;

  return(str);
}

// ── ByteBuf: growable byte buffer for encoding ──────────────────────────

typedef struct
{
  uint8_t *data;
  size_t size;
  size_t cap;
} ByteBuf;

// Initialize a ByteBuf with the given initial capacity.
// b: the buffer to initialize.
// cap: initial allocation size in bytes.
static void
bb_init(ByteBuf *b, size_t cap)
{
  b->data = malloc(cap);
  b->size = 0;
  b->cap  = cap;
}

// Ensure the buffer has room for at least need more bytes.
// b: the buffer.
// need: number of additional bytes required.
static void
bb_ensure(ByteBuf *b, size_t need)
{
  if(b->size + need <= b->cap)
    return;

  while(b->cap < b->size + need)
    b->cap *= 2;

  b->data = realloc(b->data, b->cap);
}

// Append len bytes from src to the buffer.
// b: the buffer.
// src: source data pointer.
// len: number of bytes to append.
static void
bb_write(ByteBuf *b, const void *src, size_t len)
{
  bb_ensure(b, len);
  memcpy(b->data + b->size, src, len);
  b->size += len;
}

// Append a uint32 value (4 bytes) to the buffer.
// b: the buffer.
// val: the value to write.
static void
bb_write_u32(ByteBuf *b, uint32_t val)
{
  bb_write(b, &val, 4);
}

// Write a length-prefixed string: [4-byte len][bytes].
// b: the buffer.
// s: the string to write (NULL treated as zero-length).
static void
bb_write_str(ByteBuf *b, const char *s)
{
  uint32_t len = s ? (uint32_t)strlen(s) : 0;

  bb_write_u32(b, len);

  if(len > 0)
    bb_write(b, s, len);
}

// Write a key-value pair where value is a string: [key_str][val_str].
// b: the buffer.
// key: the key string.
// val: the value string (NULL written as empty string).
static void
bb_write_key_str(ByteBuf *b, const char *key, const char *val)
{
  bb_write_str(b, key);
  bb_write_str(b, val ? val : "");
}

// Write a key-value pair where value is uint32: [key_str][4-byte val].
// b: the buffer.
// key: the key string.
// val: the uint32 value.
static void
bb_write_key_u32(ByteBuf *b, const char *key, uint32_t val)
{
  bb_write_str(b, key);
  bb_write_u32(b, val);
}

// ── Block sentinel values used by the TQ engine ─────────────────────────

#define TQ_BEGIN_BLOCK  0xB01DFACE
#define TQ_END_BLOCK    0xDEADC0DE

// ── Encode inventory blob ───────────────────────────────────────────────

// Encode the character's inventory sacks into a binary blob.
// chr: the character whose inventory to encode.
// out: receives the allocated blob data.
// out_size: receives the blob size in bytes.
static void
encode_inventory_blob(TQCharacter *chr, uint8_t **out, size_t *out_size)
{
  ByteBuf b;

  bb_init(&b, 4096);

  bb_write_key_u32(&b, "numberOfSacks", (uint32_t)chr->num_inv_sacks);
  bb_write_key_u32(&b, "currentlyFocusedSackNumber", chr->focused_sack);
  bb_write_key_u32(&b, "currentlySelectedSackNumber", chr->selected_sack);

  for(int s = 0; s < chr->num_inv_sacks; s++)
  {
    TQVaultSack *sack = &chr->inv_sacks[s];

    bb_write_key_u32(&b, "begin_block", TQ_BEGIN_BLOCK);
    bb_write_key_u32(&b, "tempBool", 0);

    // Compute expanded item count (stacked items expand to multiple entries)
    uint32_t expanded_count = 0;

    for(int i = 0; i < sack->num_items; i++)
    {
      int ss = sack->items[i].stack_size;

      expanded_count += (uint32_t)(ss > 1 ? ss : 1);
    }

    bb_write_key_u32(&b, "size", expanded_count);

    for(int i = 0; i < sack->num_items; i++)
    {
      TQVaultItem *item = &sack->items[i];
      int stack = item->stack_size > 1 ? item->stack_size : 1;

      for(int si = 0; si < stack; si++)
      {
        // Outer begin_block (Sack type)
        bb_write_key_u32(&b, "begin_block", TQ_BEGIN_BLOCK);
        // Inner begin_block
        bb_write_key_u32(&b, "begin_block", TQ_BEGIN_BLOCK);

        bb_write_key_str(&b, "baseName",    item->base_name);
        bb_write_key_str(&b, "prefixName",  item->prefix_name);
        bb_write_key_str(&b, "suffixName",  item->suffix_name);
        bb_write_key_str(&b, "relicName",   item->relic_name);
        bb_write_key_str(&b, "relicBonus",  item->relic_bonus);
        bb_write_key_u32(&b, "seed",
          si == 0 ? item->seed
          : (si - 1 < item->stack_seed_count ? item->stack_seeds[si - 1]
                                             : item->seed));
        bb_write_key_u32(&b, "var1",        item->var1);

        if(chr->has_atlantis)
        {
          bb_write_key_str(&b, "relicName2",  item->relic_name2);
          bb_write_key_str(&b, "relicBonus2", item->relic_bonus2);
          bb_write_key_u32(&b, "var2",
            si == 0 ? item->var2
            : (si - 1 < item->stack_seed_count && item->stack_var2
               ? item->stack_var2[si - 1] : item->var2));
        }

        // Inner end_block
        bb_write_key_u32(&b, "end_block", TQ_END_BLOCK);

        // First entry uses real position; extras use (-1, -1)
        if(si == 0)
        {
          bb_write_key_u32(&b, "pointX", (uint32_t)item->point_x);
          bb_write_key_u32(&b, "pointY", (uint32_t)item->point_y);
        }

        else
        {
          bb_write_key_u32(&b, "pointX", (uint32_t)-1);
          bb_write_key_u32(&b, "pointY", (uint32_t)-1);
        }

        // Outer end_block
        bb_write_key_u32(&b, "end_block", TQ_END_BLOCK);
      }
    }

    // Sack end_block
    bb_write_key_u32(&b, "end_block", TQ_END_BLOCK);
  }

  *out = b.data;
  *out_size = b.size;
}

// ── Encode equipment blob ───────────────────────────────────────────────

// Encode the character's equipment slots into a binary blob.
// chr: the character whose equipment to encode.
// out: receives the allocated blob data.
// out_size: receives the blob size in bytes.
static void
encode_equipment_blob(TQCharacter *chr, uint8_t **out, size_t *out_size)
{
  ByteBuf b;

  bb_init(&b, 2048);

  bb_write_key_u32(&b, "equipmentCtrlIOStreamVersion", 1);

  // Helper: write one equipment slot (item block + itemAttached)
  #define WRITE_EQUIP_SLOT(slot) do { \
    TQItem *eq = chr->equipment[slot]; \
    bb_write_key_u32(&b, "begin_block", TQ_BEGIN_BLOCK); \
    bb_write_key_str(&b, "baseName",    eq ? eq->base_name    : NULL); \
    bb_write_key_str(&b, "prefixName",  eq ? eq->prefix_name  : NULL); \
    bb_write_key_str(&b, "suffixName",  eq ? eq->suffix_name  : NULL); \
    bb_write_key_str(&b, "relicName",   eq ? eq->relic_name   : NULL); \
    bb_write_key_str(&b, "relicBonus",  eq ? eq->relic_bonus  : NULL); \
    bb_write_key_u32(&b, "seed",        eq ? eq->seed : 0); \
    bb_write_key_u32(&b, "var1",        eq ? eq->var1 : 0); \
    if(chr->has_atlantis) { \
      bb_write_key_str(&b, "relicName2",  eq ? eq->relic_name2  : NULL); \
      bb_write_key_str(&b, "relicBonus2", eq ? eq->relic_bonus2 : NULL); \
      bb_write_key_u32(&b, "var2",        eq ? eq->var2 : chr->equip_slot_var2[slot]); \
    } \
    bb_write_key_u32(&b, "end_block", TQ_END_BLOCK); \
    bb_write_key_u32(&b, "itemAttached", (uint32_t)chr->equip_attached[slot]); \
  } while(0)

  // Slots 0-6: Head, Neck, Chest, Legs, Arms, Ring1, Ring2
  for(int slot = 0; slot < 7; slot++)
    WRITE_EQUIP_SLOT(slot);

  // Weapon sets: write in original order (first_alternate)
  int first = chr->first_alternate;  // 0 or 1
  int second = 1 - first;

  for(int wi = 0; wi < 2; wi++)
  {
    int alt = (wi == 0) ? first : second;
    int base_slot = 7 + alt * 2;

    bb_write_key_u32(&b, "begin_block", TQ_BEGIN_BLOCK);
    bb_write_key_u32(&b, "alternate", (uint32_t)alt);
    WRITE_EQUIP_SLOT(base_slot);
    WRITE_EQUIP_SLOT(base_slot + 1);
    bb_write_key_u32(&b, "end_block", TQ_END_BLOCK);
  }

  // Slot 11: Artifact
  WRITE_EQUIP_SLOT(11);

  #undef WRITE_EQUIP_SLOT

  // Final equipment end_block
  bb_write_key_u32(&b, "end_block", TQ_END_BLOCK);

  *out = b.data;
  *out_size = b.size;
}

// ── Parser state for character_load sub-parsers ─────────────────────────

typedef struct
{
  TQCharacter *chr;
  const uint8_t *data;
  size_t size;
  size_t *offset;
  size_t pre_key_offset;

  // temp key counter: 1=difficulty, 2-6=base stats
  int temp_count;

  // Equipment parser state
  int in_equipment;
  int equip_count;         // linear counter: how many itemAttached seen
  int cur_equip_slot;      // actual slot index for current item (0-11)
  int cur_alternate;       // -1 = not in weapon wrapper, 0 or 1
  int weapon_sub;          // index within weapon wrapper (0 or 1)
  int equip_end_pending;   // set when all 12 equipment slots parsed

  // Inventory parser state machine
  //
  // Trigger: key "itemPositionsSavedAsGridCoords" -> inv_state = 1
  //
  //  0  scanning (not yet in inventory section)
  //  1  expect "numberOfSacks"
  //  2  expect "currentlyFocusedSackNumber"
  //  3  expect "currentlySelectedSackNumber"
  //  4  expect begin_block  (start of a sack)
  //  5  expect "tempBool"
  //  6  expect "size"       (item count for this sack)
  //  7  reading items: begin_block (next item) or end_block (sack done)
  //  8  item outer block open -- expect inner begin_block
  //  9  item inner block open -- reading baseName/prefix/etc., end_block -> 10
  // 10  after inner end_block -- reading pointX/pointY then outer end_block -> 7
  int inv_state;
  int inv_num_sacks;       // from numberOfSacks
  int inv_sack_idx;        // current sack index (0-based)
  int inv_items_expected;  // from "size" key
  int inv_items_read;      // items fully parsed in current sack
  TQVaultItem *cur_inv_item;
  char *pending_skill_name;  // tracks skillName for next skillLevel
} ParseState;

// Parse a "begin_block" key during character loading.
// ps: parser state.
static void
parse_begin_block(ParseState *ps)
{
  *ps->offset += 4;

  if(ps->inv_state == 4)
  {
    // Start of a sack
    ps->inv_sack_idx++;
    if(ps->inv_sack_idx < 4)
    {
      ps->chr->inv_sacks[ps->inv_sack_idx].items     = NULL;
      ps->chr->inv_sacks[ps->inv_sack_idx].num_items = 0;
    }
    ps->inv_state = 5;
  }

  else if(ps->inv_state == 7 && ps->inv_items_read < ps->inv_items_expected)
  {
    // Outer item block
    ps->cur_inv_item = calloc(1, sizeof(TQVaultItem));
    ps->inv_state = 8;
  }

  else if(ps->inv_state == 8)
  {
    // Inner item block
    ps->inv_state = 9;
  }

  // All other states: value already consumed, nothing else to do
}

// Parse an "end_block" key during character loading.
// ps: parser state.
static void
parse_end_block(ParseState *ps)
{
  *ps->offset += 4;

  if(ps->inv_state == 9)
  {
    // Inner block closes
    ps->inv_state = 10;
  }

  else if(ps->inv_state == 10)
  {
    // Outer block closes -> finalise item
    if(ps->cur_inv_item)
    {
      if(ps->cur_inv_item->base_name
          && ps->inv_sack_idx >= 0 && ps->inv_sack_idx < 4)
      {
        TQVaultSack *sk = &ps->chr->inv_sacks[ps->inv_sack_idx];

        if(ps->cur_inv_item->point_x == -1 && ps->cur_inv_item->point_y == -1
            && sk->num_items > 0)
        {
          // Stackable extra: merge into previous item, preserve seed+var2
          TQVaultItem *prev = &sk->items[sk->num_items - 1];
          int idx = prev->stack_seed_count;

          prev->stack_seeds = realloc(prev->stack_seeds,
            (idx + 1) * sizeof(uint32_t));
          prev->stack_var2 = realloc(prev->stack_var2,
            (idx + 1) * sizeof(uint32_t));
          prev->stack_seeds[idx] = ps->cur_inv_item->seed;
          prev->stack_var2[idx] = ps->cur_inv_item->var2;
          prev->stack_seed_count++;
          prev->stack_size++;
          vault_item_free_strings(ps->cur_inv_item);
        }

        else
        {
          ps->cur_inv_item->stack_size = 1;
          sk->items = realloc(sk->items,
                              (sk->num_items + 1) * sizeof(TQVaultItem));
          sk->items[sk->num_items++] = *ps->cur_inv_item;
        }
      }

      else
      {
        vault_item_free_strings(ps->cur_inv_item);
      }

      free(ps->cur_inv_item);
      ps->cur_inv_item = NULL;
    }
    ps->inv_items_read++;
    ps->inv_state = 7;
  }

  else if(ps->inv_state == 7)
  {
    // Sack ends
    ps->chr->num_inv_sacks = ps->inv_sack_idx + 1;
    ps->inv_items_expected = 0;
    ps->inv_items_read     = 0;

    if(ps->inv_sack_idx + 1 >= ps->inv_num_sacks)
    {
      ps->inv_state = 0;   // done with all sacks
      ps->chr->inv_block_end = *ps->offset;
      if(tqvc_debug)
        printf("  inventory done: %d sacks\n", ps->chr->num_inv_sacks);
    }

    else
    {
      ps->inv_state = 4;   // read next sack
    }
  }

  else if(ps->equip_end_pending)
  {
    // Final equipment end_block
    ps->chr->equip_block_end = *ps->offset;
    ps->equip_end_pending = 0;
  }

  else if(ps->in_equipment && ps->cur_alternate >= 0 && ps->weapon_sub >= 2)
  {
    // Weapon wrapper end_block
    ps->cur_alternate = -1;
    if(ps->equip_count >= 11)
      ps->cur_equip_slot = 11;
  }
}

// Parse inventory section header keys (numberOfSacks, focused, selected).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_inv_header(ParseState *ps, const char *key)
{
  if(ps->inv_state == 1 && strcmp(key, "numberOfSacks") == 0)
  {
    ps->chr->inv_block_start = ps->pre_key_offset;
    ps->inv_num_sacks = (int)read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    ps->inv_state = 2;
    return(1);
  }

  if(ps->inv_state == 2 && strcmp(key, "currentlyFocusedSackNumber") == 0)
  {
    ps->chr->focused_sack = read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    ps->inv_state = 3;
    return(1);
  }

  if(ps->inv_state == 3 && strcmp(key, "currentlySelectedSackNumber") == 0)
  {
    ps->chr->selected_sack = read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    ps->inv_sack_idx = -1;
    ps->inv_state = 4;
    return(1);
  }

  return(0);
}

// Parse sack header fields (tempBool, size).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_sack_header(ParseState *ps, const char *key)
{
  if(strcmp(key, "tempBool") == 0)
  {
    *ps->offset += 4;
    if(ps->inv_state == 5)
      ps->inv_state = 6;
    return(1);
  }

  if(strcmp(key, "size") == 0)
  {
    if(ps->inv_state == 6)
    {
      ps->inv_items_expected = (int)read_u32(ps->data, *ps->offset);
      ps->inv_items_read     = 0;
      ps->inv_state = 7;
    }
    *ps->offset += 4;
    return(1);
  }

  return(0);
}

// Parse item string fields (baseName, prefixName, suffixName, etc.).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_item_strings(ParseState *ps, const char *key)
{
  if(strcmp(key, "baseName") == 0)
  {
    char *val = read_string(ps->data, *ps->offset, ps->offset);

    if(ps->in_equipment)
    {
      if(val && *val && ps->cur_equip_slot < 12)
      {
        free(ps->chr->equipment[ps->cur_equip_slot]);
        ps->chr->equipment[ps->cur_equip_slot] = calloc(1, sizeof(TQItem));
        ps->chr->equipment[ps->cur_equip_slot]->base_name = val;
      }

      else
      {
        free(val);
      }
    }

    else if(ps->inv_state == 9 && ps->cur_inv_item)
    {
      if(val && *val)
      {
        free(ps->cur_inv_item->base_name);
        ps->cur_inv_item->base_name = val;
      }

      else
      {
        free(val);
      }
    }

    else
    {
      free(val);
    }

    return(1);
  }

  if(strcmp(key, "prefixName") == 0 ||
     strcmp(key, "suffixName")  == 0 ||
     strcmp(key, "relicName")   == 0 ||
     strcmp(key, "relicBonus")  == 0 ||
     strcmp(key, "relicName2")  == 0 ||
     strcmp(key, "relicBonus2") == 0)
  {
    char *val = read_string(ps->data, *ps->offset, ps->offset);

    if(ps->in_equipment && ps->cur_equip_slot < 12 && ps->chr->equipment[ps->cur_equip_slot])
    {
      TQItem *eq = ps->chr->equipment[ps->cur_equip_slot];

      if(strcmp(key, "prefixName")  == 0)
        eq->prefix_name  = val;
      else if(strcmp(key, "suffixName")  == 0)
        eq->suffix_name  = val;
      else if(strcmp(key, "relicName")   == 0)
        eq->relic_name   = val;
      else if(strcmp(key, "relicBonus")  == 0)
        eq->relic_bonus  = val;
      else if(strcmp(key, "relicName2")  == 0)
        eq->relic_name2  = val;
      else if(strcmp(key, "relicBonus2") == 0)
        eq->relic_bonus2 = val;
      else
        free(val);
    }

    else if(ps->inv_state == 9 && ps->cur_inv_item)
    {
      TQVaultItem *vi = ps->cur_inv_item;

      if(strcmp(key, "prefixName")  == 0) { free(vi->prefix_name);  vi->prefix_name  = val; }
      else if(strcmp(key, "suffixName")  == 0) { free(vi->suffix_name);  vi->suffix_name  = val; }
      else if(strcmp(key, "relicName")   == 0) { free(vi->relic_name);   vi->relic_name   = val; }
      else if(strcmp(key, "relicBonus")  == 0) { free(vi->relic_bonus);  vi->relic_bonus  = val; }
      else if(strcmp(key, "relicName2")  == 0) { free(vi->relic_name2);  vi->relic_name2  = val; }
      else if(strcmp(key, "relicBonus2") == 0) { free(vi->relic_bonus2); vi->relic_bonus2 = val; }
      else
        free(val);
    }

    else
    {
      free(val);
    }

    return(1);
  }

  return(0);
}

// Parse item integer fields (seed, var1, var2).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_item_integers(ParseState *ps, const char *key)
{
  if(strcmp(key, "seed") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    *ps->offset += 4;

    if(ps->in_equipment && ps->cur_equip_slot < 12 && ps->chr->equipment[ps->cur_equip_slot])
      ps->chr->equipment[ps->cur_equip_slot]->seed = v;
    else if(ps->inv_state == 9 && ps->cur_inv_item)
      ps->cur_inv_item->seed = v;

    return(1);
  }

  if(strcmp(key, "var1") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    *ps->offset += 4;

    if(ps->in_equipment && ps->cur_equip_slot < 12 && ps->chr->equipment[ps->cur_equip_slot])
      ps->chr->equipment[ps->cur_equip_slot]->var1 = v;
    else if(ps->inv_state == 9 && ps->cur_inv_item)
      ps->cur_inv_item->var1 = v;

    return(1);
  }

  if(strcmp(key, "var2") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    *ps->offset += 4;

    if(ps->in_equipment && ps->cur_equip_slot < 12)
    {
      ps->chr->equip_slot_var2[ps->cur_equip_slot] = v;
      if(ps->chr->equipment[ps->cur_equip_slot])
        ps->chr->equipment[ps->cur_equip_slot]->var2 = v;
    }
    else if(ps->inv_state == 9 && ps->cur_inv_item)
      ps->cur_inv_item->var2 = v;

    return(1);
  }

  return(0);
}

// Parse item position fields (pointX, pointY).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_item_position(ParseState *ps, const char *key)
{
  if(strcmp(key, "pointX") == 0)
  {
    if(ps->inv_state == 10 && ps->cur_inv_item)
      ps->cur_inv_item->point_x = (int)read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "pointY") == 0)
  {
    if(ps->inv_state == 10 && ps->cur_inv_item)
      ps->cur_inv_item->point_y = (int)read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    return(1);
  }

  return(0);
}

// Parse equipment machinery keys (equipmentCtrlIOStreamVersion, alternate,
// itemAttached).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_equipment(ParseState *ps, const char *key)
{
  if(strcmp(key, "equipmentCtrlIOStreamVersion") == 0)
  {
    *ps->offset += 4;  // uint32, skip
    return(1);
  }

  if(strcmp(key, "alternate") == 0)
  {
    if(ps->in_equipment)
    {
      ps->cur_alternate = (int)read_u32(ps->data, *ps->offset);
      if(ps->equip_count == 7)
        ps->chr->first_alternate = ps->cur_alternate;
      ps->weapon_sub = 0;
      ps->cur_equip_slot = 7 + ps->cur_alternate * 2;
    }
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "itemAttached") == 0)
  {
    if(ps->in_equipment && ps->cur_equip_slot < 12)
      ps->chr->equip_attached[ps->cur_equip_slot] =
        (int)read_u32(ps->data, *ps->offset);
    *ps->offset += 4;

    if(ps->in_equipment)
    {
      ps->equip_count++;
      if(ps->cur_alternate >= 0)
      {
        ps->weapon_sub++;
        if(ps->weapon_sub < 2)
          ps->cur_equip_slot = 7 + ps->cur_alternate * 2 + ps->weapon_sub;
      }

      else if(ps->equip_count < 7)
      {
        ps->cur_equip_slot = ps->equip_count;
      }

      else
      {
        ps->cur_equip_slot = 11;
      }

      if(ps->equip_count >= 12)
      {
        ps->in_equipment = 0;
        ps->equip_end_pending = 1;
      }
    }
    return(1);
  }

  return(0);
}

// Parse character stat keys (myPlayerName, playerCharacterClass, level,
// experience, kills, deaths, temp, modifierPoints, etc.).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_char_stats(ParseState *ps, const char *key)
{
  if(strcmp(key, "myPlayerName") == 0)
  {
    free(ps->chr->character_name);
    ps->chr->character_name = read_string_utf16_as_ascii(ps->data, *ps->offset, ps->offset);
    return(1);
  }

  if(strcmp(key, "playerCharacterClass") == 0)
  {
    free(ps->chr->class_name);
    ps->chr->class_name = read_string(ps->data, *ps->offset, ps->offset);
    return(1);
  }

  if(strcmp(key, "modifierPoints") == 0)
  {
    ps->chr->modifier_points = read_u32(ps->data, *ps->offset);
    ps->chr->off_modifier_points = *ps->offset;
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "masteriesAllowed") == 0)
  {
    ps->chr->masteries_allowed = read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "temp") == 0)
  {
    ps->temp_count++;
    float fval;

    memcpy(&fval, ps->data + *ps->offset, 4);

    switch(ps->temp_count)
    {
      case 2:
        ps->chr->strength     = fval;
        ps->chr->off_strength = *ps->offset;
        break;
      case 3:
        ps->chr->dexterity    = fval;
        ps->chr->off_dexterity = *ps->offset;
        break;
      case 4:
        ps->chr->intelligence = fval;
        ps->chr->off_intelligence = *ps->offset;
        break;
      case 5:
        ps->chr->health       = fval;
        ps->chr->off_health = *ps->offset;
        break;
      case 6:
        ps->chr->mana         = fval;
        ps->chr->off_mana = *ps->offset;
        break;
      default:
        break;
    }

    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "playerLevel") == 0 ||
     strcmp(key, "currentStats.charLevel") == 0)
  {
    ps->chr->level = read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "currentStats.experiencePoints") == 0)
  {
    ps->chr->experience = read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "numberOfKills") == 0)
  {
    ps->chr->kills = read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "numberOfDeaths") == 0)
  {
    ps->chr->deaths = read_u32(ps->data, *ps->offset);
    *ps->offset += 4;
    return(1);
  }

  return(0);
}

// Parse skill-related keys (skillPoints, skillName, skillLevel, skillEnabled,
// skillActive, skillSubLevel, skillTransition).
// ps: parser state.
// key: the current key string.
// Returns: 1 if key was consumed, 0 otherwise.
static int
parse_skills(ParseState *ps, const char *key)
{
  if(strcmp(key, "skillPoints") == 0)
  {
    ps->chr->skill_points = read_u32(ps->data, *ps->offset);
    ps->chr->off_skill_points = *ps->offset;
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "skillName") == 0)
  {
    char *skill = read_string(ps->data, *ps->offset, ps->offset);

    if(skill && strstr(skill, "Mastery.dbr"))
    {
      if(!ps->chr->mastery1)
        ps->chr->mastery1 = strdup(skill);
      else if(!ps->chr->mastery2)
        ps->chr->mastery2 = strdup(skill);
    }

    // Save as pending skill for the next skillLevel
    free(ps->pending_skill_name);
    ps->pending_skill_name = skill; // take ownership
    return(1);
  }

  if(strcmp(key, "skillLevel") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    if(ps->pending_skill_name)
    {
      int idx = ps->chr->num_skills;

      ps->chr->skills = realloc(ps->chr->skills,
        (idx + 1) * sizeof(TQCharSkill));
      memset(&ps->chr->skills[idx], 0, sizeof(TQCharSkill));
      ps->chr->skills[idx].skill_name = ps->pending_skill_name;
      ps->chr->skills[idx].skill_level = v;
      ps->chr->skills[idx].off_skill_level = *ps->offset;
      ps->chr->num_skills++;
      ps->pending_skill_name = NULL; // ownership transferred
    }

    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "skillEnabled") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    if(ps->chr->num_skills > 0)
      ps->chr->skills[ps->chr->num_skills - 1].skill_enabled = v;
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "skillActive") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    if(ps->chr->num_skills > 0)
      ps->chr->skills[ps->chr->num_skills - 1].skill_active = v;
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "skillSubLevel") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    if(ps->chr->num_skills > 0)
      ps->chr->skills[ps->chr->num_skills - 1].skill_sublevel = v;
    *ps->offset += 4;
    return(1);
  }

  if(strcmp(key, "skillTransition") == 0)
  {
    uint32_t v = read_u32(ps->data, *ps->offset);

    if(ps->chr->num_skills > 0)
      ps->chr->skills[ps->chr->num_skills - 1].skill_transition = v;
    *ps->offset += 4;
    return(1);
  }

  return(0);
}

// Load a character from a .chr file, parsing stats, equipment, inventory,
// and skills from the binary save format.
// filepath: path to the Player.chr file.
// Returns: allocated TQCharacter, or NULL on failure.
TQCharacter *
character_load(const char *filepath)
{
  if(tqvc_debug)
    printf("character_load: %s\n", filepath);

  FILE *file = fopen(filepath, "rb");

  if(!file)
  {
    perror("Error opening character file");
    return(NULL);
  }

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  TQCharacter *character = calloc(1, sizeof(TQCharacter));

  if(!character)
  {
    fclose(file);
    return(NULL);
  }

  character->filepath  = strdup(filepath);
  character->data_size = (size_t)size;
  character->raw_data  = malloc((size_t)size);
  character->has_atlantis = true;  // all modern TQAE saves have Atlantis

  if(!character->raw_data)
  {
    character_free(character);
    fclose(file);
    return(NULL);
  }

  if(fread(character->raw_data, 1, (size_t)size, file) != (size_t)size)
  {
    perror("Error reading character file");
    character_free(character);
    fclose(file);
    return(NULL);
  }

  fclose(file);

  size_t offset = 0;

  ParseState ps = {
    .chr              = character,
    .data             = character->raw_data,
    .size             = (size_t)size,
    .offset           = &offset,
    .pre_key_offset   = 0,
    .temp_count       = 0,
    .in_equipment     = 0,
    .equip_count      = 0,
    .cur_equip_slot   = 0,
    .cur_alternate    = -1,
    .weapon_sub       = 0,
    .equip_end_pending = 0,
    .inv_state        = 0,
    .inv_num_sacks    = 0,
    .inv_sack_idx     = -1,
    .inv_items_expected = 0,
    .inv_items_read   = 0,
    .cur_inv_item     = NULL,
    .pending_skill_name = NULL,
  };

  while(offset + 4 <= (size_t)size)
  {
    size_t pre_key_offset = offset;  // save offset before key length
    uint32_t len = read_u32(character->raw_data, offset);

    if(len > 0 && len < 256 && offset + 4 + len <= (size_t)size)
    {
      int printable = 1;

      for(uint32_t i = 0; i < len; i++)
      {
        if(!isprint(character->raw_data[offset + 4 + i]))
        {
          printable = 0;
          break;
        }
      }

      if(printable)
      {
        char *key = malloc(len + 1);

        if(!key)
        {
          offset++;
          continue;
        }

        memcpy(key, character->raw_data + offset + 4, len);
        key[len] = '\0';
        offset += 4 + len;

        ps.pre_key_offset = pre_key_offset;

        // ── Section triggers ──────────────────────────────────
        if(strcmp(key, "itemPositionsSavedAsGridCoords") == 0)
        {
          offset += 4;   // skip value
          ps.inv_state = 1;
        }

        else if(strcmp(key, "useAlternate") == 0)
        {
          offset += 4;
          character->equip_block_start = offset;
          ps.in_equipment  = 1;
          ps.cur_equip_slot = 0;
        }

        // ── Block start/end ─────────────────────────────────
        else if(strcmp(key, "begin_block") == 0)
        {
          parse_begin_block(&ps);
        }

        else if(strcmp(key, "end_block") == 0)
        {
          parse_end_block(&ps);
        }

        // ── Delegated sub-parsers ───────────────────────────
        else if(parse_inv_header(&ps, key)) { }
        else if(parse_sack_header(&ps, key)) { }
        else if(parse_item_strings(&ps, key)) { }
        else if(parse_item_integers(&ps, key)) { }
        else if(parse_item_position(&ps, key)) { }
        else if(parse_equipment(&ps, key)) { }
        else if(parse_char_stats(&ps, key)) { }
        else if(parse_skills(&ps, key)) { }

        // ── Default: skip value ─────────────────────────────
        else
        {
          if(is_default_string_key(key))
          {
            // Known string key: read length-prefixed string
            uint32_t slen = read_u32(character->raw_data, offset);

            if(slen > 0 && slen < 4096 && offset + 4 + slen <= (size_t)size)
              offset += 4 + slen;
            else
              offset += 4;
          }

          else
          {
            // Everything else is a 4-byte value (uint32 or float)
            offset += 4;
          }
        }

        free(key);
        continue;
      }
    }

    offset++;
  }

  // Clean up any dangling in-progress item
  if(ps.cur_inv_item)
  {
    vault_item_free_strings(ps.cur_inv_item);
    free(ps.cur_inv_item);
  }

  free(ps.pending_skill_name);

  if(!character->character_name)
    character->character_name = strdup("Unknown");

  if(tqvc_debug)
  {
    printf("character_load: finished %s (level %u, inv_sacks=%d)\n",
           character->character_name, character->level, character->num_inv_sacks);
    printf("  inv_block: [%zu..%zu), equip_block: [%zu..%zu)\n",
           character->inv_block_start, character->inv_block_end,
           character->equip_block_start, character->equip_block_end);

    for(int s = 0; s < character->num_inv_sacks; s++)
      printf("  inv_sack[%d]: %d items\n", s, character->inv_sacks[s].num_items);

    printf("  skills: %d parsed, skill_points=%u, off_skill_points=%zu\n",
           character->num_skills, character->skill_points, character->off_skill_points);
  }

  return(character);
}

// Free all memory associated with a character.
// character: the character to free (NULL is a no-op).
void
character_free(TQCharacter *character)
{
  if(!character)
    return;

  free(character->filepath);
  free(character->raw_data);
  free(character->character_name);
  free(character->class_name);
  free(character->mastery1);
  free(character->mastery2);

  for(int i = 0; i < character->num_skills; i++)
    free(character->skills[i].skill_name);

  free(character->skills);

  for(int i = 0; i < 12; i++)
  {
    if(character->equipment[i])
    {
      free(character->equipment[i]->base_name);
      free(character->equipment[i]->prefix_name);
      free(character->equipment[i]->suffix_name);
      free(character->equipment[i]->relic_name);
      free(character->equipment[i]->relic_bonus);
      free(character->equipment[i]->relic_name2);
      free(character->equipment[i]->relic_bonus2);
      free(character->equipment[i]);
    }
  }

  for(int s = 0; s < character->num_inv_sacks; s++)
  {
    for(int i = 0; i < character->inv_sacks[s].num_items; i++)
      vault_item_free_strings(&character->inv_sacks[s].items[i]);
    free(character->inv_sacks[s].items);
  }

  free(character);
}

// Write a float value at a specific byte offset in a data buffer.
// data: the raw byte buffer.
// offset: byte position to write at.
// val: the float value to write.
static void
write_float_at(uint8_t *data, size_t offset, float val)
{
  memcpy(data + offset, &val, 4);
}

// Write a uint32 value at a specific byte offset in a data buffer.
// data: the raw byte buffer.
// offset: byte position to write at.
// val: the uint32 value to write.
static void
write_u32_at(uint8_t *data, size_t offset, uint32_t val)
{
  memcpy(data + offset, &val, 4);
}

// Save modified character stats (strength, dexterity, etc.) by writing
// in-place at recorded offsets and flushing to disk.
// character: the character whose stats to persist.
// Returns: 0 on success, -1 on error.
int
character_save_stats(TQCharacter *character)
{
  if(!character || !character->raw_data)
    return(-1);

  // Verify all required offsets were found during parsing
  if(!character->off_strength || !character->off_dexterity ||
     !character->off_intelligence || !character->off_health ||
     !character->off_mana || !character->off_modifier_points)
  {
    fprintf(stderr, "character_save_stats: missing offsets\n");
    return(-1);
  }

  // All stat offsets are within the prefix region (before inv_block_start),
  // so they remain valid even after splice saves.

  // Create backup on first save
  char bak_path[1024];

  snprintf(bak_path, sizeof(bak_path), "%s.bak", character->filepath);

  if(!g_file_test(bak_path, G_FILE_TEST_EXISTS))
  {
    FILE *bak = fopen(bak_path, "wb");

    if(bak)
    {
      fwrite(character->raw_data, 1, character->data_size, bak);
      fclose(bak);
    }
  }

  // In-place writes at recorded offsets
  write_float_at(character->raw_data, character->off_strength, character->strength);
  write_float_at(character->raw_data, character->off_dexterity, character->dexterity);
  write_float_at(character->raw_data, character->off_intelligence, character->intelligence);
  write_float_at(character->raw_data, character->off_health, character->health);
  write_float_at(character->raw_data, character->off_mana, character->mana);
  write_u32_at(character->raw_data, character->off_modifier_points, character->modifier_points);

  // Write to disk
  FILE *file = fopen(character->filepath, "wb");

  if(!file)
    return(-1);

  size_t written = fwrite(character->raw_data, 1, character->data_size, file);

  fclose(file);

  if(written != character->data_size)
    return(-1);

  if(tqvc_debug)
    printf("character_save_stats: wrote %zu bytes to %s\n",
           character->data_size, character->filepath);

  return(0);
}

// Save modified skill levels by writing in-place at recorded offsets
// and flushing to disk.
// character: the character whose skills to persist.
// Returns: 0 on success, -1 on error.
int
character_save_skills(TQCharacter *character)
{
  if(!character || !character->raw_data)
    return(-1);

  if(!character->off_skill_points)
  {
    fprintf(stderr, "character_save_skills: missing skillPoints offset\n");
    return(-1);
  }

  // Create backup on first save
  char bak_path[1024];

  snprintf(bak_path, sizeof(bak_path), "%s.bak", character->filepath);

  if(!g_file_test(bak_path, G_FILE_TEST_EXISTS))
  {
    FILE *bak = fopen(bak_path, "wb");

    if(bak)
    {
      fwrite(character->raw_data, 1, character->data_size, bak);
      fclose(bak);
    }
  }

  // In-place writes: skill levels at recorded offsets
  for(int i = 0; i < character->num_skills; i++)
  {
    if(character->skills[i].off_skill_level)
      write_u32_at(character->raw_data,
                   character->skills[i].off_skill_level,
                   character->skills[i].skill_level);
  }

  write_u32_at(character->raw_data, character->off_skill_points,
               character->skill_points);

  FILE *file = fopen(character->filepath, "wb");

  if(!file)
    return(-1);

  size_t written = fwrite(character->raw_data, 1, character->data_size, file);

  fclose(file);

  if(written != character->data_size)
    return(-1);

  if(tqvc_debug)
    printf("character_save_skills: wrote %zu bytes to %s\n",
           character->data_size, character->filepath);

  return(0);
}

// Save a character to disk using splice encoding: prefix + new inventory
// blob + middle + new equipment blob + suffix.
// character: the character to save.
// filepath: output path for the .chr file.
// Returns: 0 on success, -1 on error.
int
character_save(TQCharacter *character, const char *filepath)
{
  if(!character || !character->raw_data)
    return(-1);

  if(character->inv_block_start == 0 || character->inv_block_end == 0 ||
     character->equip_block_start == 0 || character->equip_block_end == 0)
  {
    fprintf(stderr, "character_save: boundary offsets not set, cannot splice\n");
    return(-1);
  }

  if(character->inv_block_start >= character->inv_block_end ||
     character->inv_block_end > character->equip_block_start ||
     character->equip_block_start >= character->equip_block_end ||
     character->equip_block_end > character->data_size)
  {
    fprintf(stderr, "character_save: invalid boundary offsets\n");
    return(-1);
  }

  // Create backup on first save
  char bak_path[1024];

  snprintf(bak_path, sizeof(bak_path), "%s.bak", filepath);

  if(!g_file_test(bak_path, G_FILE_TEST_EXISTS))
  {
    FILE *bak = fopen(bak_path, "wb");

    if(bak)
    {
      fwrite(character->raw_data, 1, character->data_size, bak);
      fclose(bak);

      if(tqvc_debug)
        printf("character_save: backup created %s\n", bak_path);
    }
  }

  // Encode new blobs
  uint8_t *inv_blob = NULL, *equip_blob = NULL;
  size_t inv_size = 0, equip_size = 0;

  encode_inventory_blob(character, &inv_blob, &inv_size);
  encode_equipment_blob(character, &equip_blob, &equip_size);

  // Splice: prefix + inv_blob + middle + equip_blob + suffix
  size_t prefix_size = character->inv_block_start;
  size_t middle_size = character->equip_block_start - character->inv_block_end;
  size_t suffix_size = character->data_size - character->equip_block_end;
  size_t new_size = prefix_size + inv_size + middle_size + equip_size + suffix_size;

  uint8_t *new_data = malloc(new_size);

  if(!new_data)
  {
    free(inv_blob);
    free(equip_blob);
    return(-1);
  }

  size_t pos = 0;

  // Prefix: raw[0..inv_block_start)
  memcpy(new_data + pos, character->raw_data, prefix_size);
  pos += prefix_size;

  // New inventory blob
  memcpy(new_data + pos, inv_blob, inv_size);
  pos += inv_size;

  // Middle: raw[inv_block_end..equip_block_start)
  memcpy(new_data + pos, character->raw_data + character->inv_block_end, middle_size);
  pos += middle_size;

  // New equipment blob
  memcpy(new_data + pos, equip_blob, equip_size);
  pos += equip_size;

  // Suffix: raw[equip_block_end..EOF)
  memcpy(new_data + pos, character->raw_data + character->equip_block_end, suffix_size);
  pos += suffix_size;

  free(inv_blob);
  free(equip_blob);

  // Write to disk
  FILE *file = fopen(filepath, "wb");

  if(!file)
  {
    free(new_data);
    return(-1);
  }

  if(fwrite(new_data, 1, new_size, file) != new_size)
  {
    fclose(file);
    free(new_data);
    return(-1);
  }

  fclose(file);

  // Update raw_data and recalculate boundary offsets for subsequent saves
  free(character->raw_data);
  character->raw_data = new_data;
  character->data_size = new_size;

  // Recalculate boundaries from known structure
  // inv_block_start stays the same (prefix unchanged)
  character->inv_block_end = character->inv_block_start + inv_size;
  character->equip_block_start = character->inv_block_end + middle_size;
  character->equip_block_end = character->equip_block_start + equip_size;

  if(tqvc_debug)
    printf("character_save: wrote %zu bytes to %s\n", new_size, filepath);

  return(0);
}
