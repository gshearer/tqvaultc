// tq-chr-tool: the raw .chr stream walker -- known-key table, low-level
// readers, the entry list, and the structured inventory/equipment parse.

#include "tq_chr_tool.h"

// Every key that appears in a .chr file, mapped to its value type.
// This table is the antidote to Bug 1's heuristic.
static const KnownKey known_keys[] = {
  // Block sentinels
  {"begin_block",                       VAL_U32},
  {"end_block",                         VAL_U32},

  // Inventory header
  {"itemPositionsSavedAsGridCoords",    VAL_U32},
  {"numberOfSacks",                     VAL_U32},
  {"currentlyFocusedSackNumber",        VAL_U32},
  {"currentlySelectedSackNumber",       VAL_U32},
  {"tempBool",                          VAL_U32},
  {"size",                              VAL_U32},

  // Item string fields
  {"baseName",                          VAL_STRING},
  {"prefixName",                        VAL_STRING},
  {"suffixName",                        VAL_STRING},
  {"relicName",                         VAL_STRING},
  {"relicBonus",                        VAL_STRING},
  {"relicName2",                        VAL_STRING},
  {"relicBonus2",                       VAL_STRING},

  // Item integer fields
  {"seed",                              VAL_U32},
  {"var1",                              VAL_U32},
  {"var2",                              VAL_U32},
  {"pointX",                            VAL_U32},
  {"pointY",                            VAL_U32},

  // Equipment
  {"equipmentCtrlIOStreamVersion",      VAL_U32},
  {"alternate",                         VAL_U32},
  {"itemAttached",                      VAL_U32},
  {"useAlternate",                      VAL_U32},

  // Character identity
  {"myPlayerName",                      VAL_UTF16},
  {"playerCharacterClass",              VAL_STRING},
  {"isInMainQuest",                     VAL_U32},
  {"hasBeenInGame",                     VAL_U32},
  {"disableAutoPopV2",                  VAL_U32},
  {"numTutorialPagesV2",                VAL_U32},
  {"currentPageV2",                     VAL_U32},
  {"hideCompletedV2",                   VAL_U32},

  // Character stats
  {"playerLevel",                       VAL_U32},
  {"playerTexture",                     VAL_STRING},
  {"money",                             VAL_U32},
  {"temp",                              VAL_FLOAT},
  {"currentStats.charLevel",            VAL_U32},
  {"currentStats.experiencePoints",     VAL_U32},
  {"modifierPoints",                    VAL_U32},
  {"skillPoints",                       VAL_U32},
  {"masteriesAllowed",                  VAL_U32},

  // Skills
  {"skillName",                         VAL_STRING},
  {"skillLevel",                        VAL_U32},
  {"skillEnabled",                      VAL_U32},
  {"skillSubLevel",                     VAL_U32},
  {"skillActive",                       VAL_U32},
  {"skillTransition",                   VAL_U32},
  {"max",                               VAL_U32},

  // Play stats
  {"numberOfKills",                     VAL_U32},
  {"numberOfDeaths",                    VAL_U32},
  {"experienceFromKills",               VAL_U32},
  {"healthPotionsUsed",                 VAL_U32},
  {"manaPotionsUsed",                   VAL_U32},
  {"maxLevel",                          VAL_U32},
  {"numHitsReceived",                   VAL_U32},
  {"numHitsInflicted",                  VAL_U32},
  {"greatestDamageInflicted",           VAL_FLOAT},
  {"greatestDamageReceived",            VAL_FLOAT},
  {"criticalHitsInflicted",             VAL_U32},
  {"criticalHitsReceived",              VAL_U32},
  {"playTimeInSeconds",                 VAL_U32},
  {"greatestMonsterKilledName",         VAL_STRING},
  {"greatestMonsterKilledLevel",        VAL_U32},
  {"greatestMonsterKilledLifeAndMana",  VAL_U32},
  {"lastMonsterHit",                    VAL_STRING},
  {"lastMonsterHitBy",                  VAL_STRING},

  // Teleport/waypoint
  {"teleportUIDsName",                  VAL_STRING},
  {"teleportUIDsX",                     VAL_FLOAT},
  {"teleportUIDsY",                     VAL_FLOAT},
  {"teleportUID",                       VAL_U32},
  {"teleportUIDsSize",                  VAL_U32},

  // Respawn/markers
  {"respawnUID",                        VAL_U32},
  {"respawnUIDsSize",                   VAL_U32},
  {"markerUID",                         VAL_U32},
  {"markerUIDsSize",                    VAL_U32},

  // Lore
  {"intArray",                          VAL_U32},
  {"storedType",                        VAL_U32},
  {"itemName",                          VAL_STRING},
  {"isItemSkill",                       VAL_U32},

  // Misc state
  {"strategicMovement",                 VAL_U32},
  {"versionRespawn",                    VAL_U32},
  {"versionCheckEquipment",             VAL_U32},
  {"versionCheckSkills",                VAL_U32},
  {"compassState",                      VAL_U32},
  {"skillWindowShowHelp",               VAL_U32},
  {"skillWindowSelection",              VAL_U32},
  {"alternateConfig",                   VAL_U32},
  {"alternateConfigEnabled",            VAL_U32},
  {"headerVersion",                     VAL_U32},
  {"playerVersion",                     VAL_U32},

  // Mobile (iOS) port additions (headerVersion 4)
  {"currentDifficulty",                 VAL_U32},
  {"mySaveId",                          VAL_STRING},

  {"playerClassTag",                    VAL_STRING},
  {"uniqueId",                          VAL_STRING},
  {"streamData",                        VAL_STRING},

  // Version checks
  {"versionCheckTeleportInfo",          VAL_U32},
  {"versionCheckRespawnInfo",           VAL_U32},
  {"versionCheckMovementInfo",          VAL_U32},
  {"versionRespawnPoint",               VAL_U32},

  // Skill bar / secondary
  {"primarySkill1",                     VAL_U32},
  {"primarySkill2",                     VAL_U32},
  {"primarySkill3",                     VAL_U32},
  {"primarySkill4",                     VAL_U32},
  {"primarySkill5",                     VAL_U32},
  {"secondarySkill1",                   VAL_U32},
  {"secondarySkill2",                   VAL_U32},
  {"secondarySkill3",                   VAL_U32},
  {"secondarySkill4",                   VAL_U32},
  {"secondarySkill5",                   VAL_U32},
  {"skillActive1",                      VAL_U32},
  {"skillActive2",                      VAL_U32},
  {"skillActive3",                      VAL_U32},
  {"skillActive4",                      VAL_U32},
  {"skillActive5",                      VAL_U32},
  {"skillSettingValid",                 VAL_U32},
  {"skillReclamationPointsUsed",        VAL_U32},

  // Per-difficulty play stats (array-indexed variants)
  {"(*greatestMonsterKilledName)[i]",   VAL_STRING},
  {"(*greatestMonsterKilledLevel)[i]",  VAL_U32},
  {"(*greatestMonsterKilledLifeAndMana)[i]", VAL_U32},

  // Difficulty-indexed arrays
  {"tartarusDefeatedCount[i]",          VAL_U32},
  {"strategicMovementRespawnPoint[i]",  VAL_FLOAT},
  {"itemsFoundOverLifetimeUniqueTotal", VAL_U32},

  // UI / misc state
  {"altMoney",                          VAL_U32},
  {"bitmapDownName",                    VAL_STRING},
  {"bitmapUpName",                      VAL_STRING},
  {"boostedCharacterForX4",             VAL_U32},
  {"controllerStreamed",                VAL_U32},
  {"defaultText",                       VAL_STRING},
  {"equipmentSelection",               VAL_U32},
  {"hasSkillServices",                  VAL_U32},
  {"itemsFoundOverLifetimeRandomizedTotal", VAL_U32},
  {"scrollName",                        VAL_STRING},

  {"version",                            VAL_U32},
  {"description",                       VAL_STRING},

  // Difficulty unlock / tokens
  {"oTokens",                           VAL_STRING},
  {"oTokensCount",                      VAL_U32},

  {NULL, 0}
};

// ── Low-level helpers ────────────────────────────────────────────────────

// load_file -- read an entire file into a malloc'd buffer.
// path: path to the file.
// out_size: receives the file size in bytes.
// returns malloc'd buffer, or NULL on error.
uint8_t *
load_file(const char *path, size_t *out_size)
{
  struct stat st;
  FILE *f = fopen(path, "rb");

  if(!f)
  {
    fprintf(stderr, "error: cannot open '%s': ", path);
    perror(NULL);
    return(NULL);
  }

  // A directory opens fine here, and then ftell reports LONG_MAX.
  if(fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode))
  {
    fprintf(stderr, "error: '%s' is not a regular file\n", path);
    fclose(f);
    return(NULL);
  }

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);

  if(sz <= 0)
  {
    fprintf(stderr, "error: '%s' is empty or unreadable\n", path);
    fclose(f);
    return(NULL);
  }

  fseek(f, 0, SEEK_SET);

  uint8_t *data = malloc((size_t)sz);

  if(!data)
  {
    fprintf(stderr, "error: malloc failed for %ld bytes\n", sz);
    fclose(f);
    return(NULL);
  }

  if(fread(data, 1, (size_t)sz, f) != (size_t)sz)
  {
    fprintf(stderr, "error: short read on '%s'\n", path);
    free(data);
    fclose(f);
    return(NULL);
  }

  fclose(f);
  *out_size = (size_t)sz;
  return(data);
}

// rd_u32 -- read a little-endian uint32 from a data buffer.
// data: binary data buffer.
// off: byte offset to read from.
// returns the uint32 value.
uint32_t
rd_u32(const uint8_t *data, size_t off)
{
  uint32_t v;

  memcpy(&v, data + off, 4);
  return(v);
}

// rd_float -- read a little-endian IEEE float from a data buffer.
// data: binary data buffer.
// off: byte offset to read from.
// returns the float value.
float
rd_float(const uint8_t *data, size_t off)
{
  float v;

  memcpy(&v, data + off, 4);
  return(v);
}

// rd_string -- read a length-prefixed ASCII string into buf.
// data: binary data buffer.
// off: byte offset of the length prefix.
// file_size: total buffer size for bounds checking.
// buf: output buffer for the string.
// buf_size: size of buf.
// returns number of bytes consumed.
size_t
rd_string(const uint8_t *data, size_t off, size_t file_size,
          char *buf, size_t buf_size)
{
  uint32_t len = rd_u32(data, off);

  if(len == 0 || off + 4 + len > file_size)
  {
    buf[0] = '\0';
    return(4);
  }

  size_t copy = len < buf_size - 1 ? len : buf_size - 1;

  memcpy(buf, data + off + 4, copy);
  buf[copy] = '\0';
  return(4 + len);
}

// rd_utf16 -- read a length-prefixed wide string, convert to ASCII.
// Desktop saves use UTF-16LE (char_width 2); the iOS mobile port (headerVersion
// >= 4) uses UTF-32LE (char_width 4). The prefix counts characters either way.
// data: binary data buffer.
// off: byte offset of the length prefix.
// file_size: total buffer size for bounds checking.
// char_width: bytes per character (2 = UTF-16LE, 4 = UTF-32LE).
// buf: output buffer for the ASCII string.
// buf_size: size of buf.
// returns number of bytes consumed.
size_t
rd_utf16(const uint8_t *data, size_t off, size_t file_size, int char_width,
         char *buf, size_t buf_size)
{
  uint32_t len = rd_u32(data, off);

  if(len == 0 || off + 4 + (size_t)len * char_width > file_size)
  {
    buf[0] = '\0';
    return(4);
  }

  size_t copy = len < buf_size - 1 ? len : buf_size - 1;

  for(size_t i = 0; i < copy; i++)
    buf[i] = (char)data[off + 4 + i * char_width];

  buf[copy] = '\0';
  return(4 + (size_t)len * char_width);
}

// lookup_key -- find a key in the known-key table.
// name: key name to look up.
// returns pointer to KnownKey, or NULL if not found.
const KnownKey *
lookup_key(const char *name)
{
  for(int i = 0; known_keys[i].name; i++)
  {
    if(strcmp(known_keys[i].name, name) == 0)
      return(&known_keys[i]);
  }

  return(NULL);
}

// ── Entry-level parser ───────────────────────────────────────────────────
// Walks the raw binary stream, using the known-key table to determine types.
// Unknown keys fall back to the heuristic but are flagged as AMBIGUOUS.

// entry_list_init -- initialize a dynamic entry list.
// list: list to initialize.
void
entry_list_init(ChrEntryList *list)
{
  list->count = 0;
  list->cap = 4096;
  list->entries = malloc(list->cap * sizeof(ChrEntry));

  if(!list->entries)
  {
    list->cap = 0;
    return;
  }
}

// entry_list_push -- append an entry to the dynamic list.
// list: list to append to.
// e: entry to copy into the list.
void
entry_list_push(ChrEntryList *list, const ChrEntry *e)
{
  if(list->count >= list->cap)
  {
    list->cap *= 2;
    list->entries = realloc(list->entries, list->cap * sizeof(ChrEntry));

    if(!list->entries)
    {
      list->cap = 0;
      return;
    }
  }

  list->entries[list->count++] = *e;
}

// entry_list_free -- free all memory in a dynamic entry list.
// list: list to free.
void
entry_list_free(ChrEntryList *list)
{
  free(list->entries);
  list->entries = NULL;
  list->count = list->cap = 0;
}

// parse_entries -- walk the raw binary stream and build an entry list.
// Uses the known-key table for type resolution, heuristic fallback for unknowns.
// data: binary file data.
// file_size: size of data in bytes.
// out: receives the parsed entry list.
// returns the number of entries parsed.
int
parse_entries(const uint8_t *data, size_t file_size, ChrEntryList *out)
{
  entry_list_init(out);
  size_t offset = 0;
  int depth = 0;
  int header_version = 0;  // captured from the first key; gates myPlayerName width

  while(offset + 4 <= file_size)
  {
    size_t key_start = offset;
    uint32_t klen = rd_u32(data, offset);

    // Validate: plausible key length with printable ASCII
    if(klen == 0 || klen >= 256 || offset + 4 + klen > file_size)
    {
      offset++;
      continue;
    }

    bool printable = true;

    for(uint32_t i = 0; i < klen; i++)
    {
      if(!isprint(data[offset + 4 + i]))
      {
        printable = false;
        break;
      }
    }

    if(!printable)
    {
      offset++;
      continue;
    }

    ChrEntry e = {0};

    e.offset = key_start;
    memcpy(e.key, data + offset + 4, klen);
    e.key[klen] = '\0';
    offset += 4 + klen;
    e.val_offset = offset;

    if(offset + 4 > file_size)
      break;

    // Track block depth
    if(strcmp(e.key, "begin_block") == 0)
    {
      e.type = VAL_U32;
      e.u32_val = rd_u32(data, offset);
      e.depth = depth;
      depth++;
      offset += 4;
    }
    else if(strcmp(e.key, "end_block") == 0)
    {
      depth--;
      if(depth < 0)
        depth = 0;
      e.type = VAL_U32;
      e.u32_val = rd_u32(data, offset);
      e.depth = depth;
      offset += 4;
    }
    else
    {
      e.depth = depth;

      const KnownKey *kk = lookup_key(e.key);

      if(kk)
      {
        e.type = kk->type;
        e.ambiguous = false;

        switch(kk->type)
        {
        case VAL_U32:
          e.u32_val = rd_u32(data, offset);
          if(strcmp(e.key, "headerVersion") == 0)
            header_version = (int)e.u32_val;
          offset += 4;
          break;
        case VAL_FLOAT:
          e.float_val = rd_float(data, offset);
          e.u32_val = rd_u32(data, offset);  // raw bits too
          offset += 4;
          break;
        case VAL_STRING:
          offset += rd_string(data, offset, file_size,
                              e.str_val, sizeof(e.str_val));
          break;
        case VAL_UTF16:
          offset += rd_utf16(data, offset, file_size,
                             header_version >= 4 ? 4 : 2,
                             e.str_val, sizeof(e.str_val));
          break;
        }
      }
      else
      {
        // Heuristic fallback (same as character.c but flagged)
        e.ambiguous = true;
        uint32_t val = rd_u32(data, offset);

        if(val > 0 && val < 512 && offset + 4 + val <= file_size)
        {
          // Looks like a string
          e.type = VAL_STRING;
          size_t copy = val < sizeof(e.str_val) - 1 ? val : sizeof(e.str_val) - 1;

          memcpy(e.str_val, data + offset + 4, copy);
          e.str_val[copy] = '\0';
          offset += 4 + val;
        }
        else
        {
          e.type = VAL_U32;
          e.u32_val = val;
          offset += 4;
        }
      }
    }

    e.next_offset = offset;
    entry_list_push(out, &e);
  }

  return(out->count);
}

// ── Structured chr parser ────────────────────────────────────────────────
// Builds inventory/equipment structures from the entry list.

// sack_add_item -- append an item to a sack's item array.
// sack: the sack to add to.
// item: the item to copy into the sack.
static void
sack_add_item(RawSack *sack, const RawItem *item)
{
  if(sack->actual_count >= sack->items_cap)
  {
    sack->items_cap = sack->items_cap ? sack->items_cap * 2 : 64;
    sack->items = realloc(sack->items, sack->items_cap * sizeof(RawItem));

    if(!sack->items)
    {
      sack->items_cap = 0;
      return;
    }
  }

  sack->items[sack->actual_count++] = *item;
}

// parse_chr_structured -- build inventory/equipment structures from entry list.
// Mirrors character.c's state machine but with correct type handling.
// entries: parsed entry list from parse_entries().
// out: receives the structured parse result.
void
parse_chr_structured(const ChrEntryList *entries, RawChrParse *out)
{
  memset(out, 0, sizeof(*out));

  for(int i = 0; i < 12; i++)
    out->slots[i].alternate = -1;

  // State machine -- mirrors character.c but with correct type handling
  int inv_state = 0;
  int sack_idx = -1;
  RawItem cur_item = {0};
  bool in_item_inner = false;
  bool in_item_outer = false;
  int in_equipment = 0;
  int equip_count = 0;        // linear counter: how many itemAttached seen
  int equip_slot = 0;         // actual slot index (alternate-aware)
  int cur_alternate = -1;     // current weapon set wrapper alternate value
  int weapon_sub = 0;         // index within weapon wrapper (0 or 1)
  int equip_end_pending = 0;

  for(int i = 0; i < entries->count; i++)
  {
    const ChrEntry *e = &entries->entries[i];

    // ── Section triggers ──
    if(strcmp(e->key, "itemPositionsSavedAsGridCoords") == 0)
    {
      inv_state = 1;
      continue;
    }

    if(strcmp(e->key, "useAlternate") == 0)
    {
      out->equip_start = e->next_offset;
      in_equipment = 1;
      equip_slot = 0;
      continue;
    }

    // ── Inventory header ──
    if(inv_state == 1 && strcmp(e->key, "numberOfSacks") == 0)
    {
      out->inv_start = e->offset;
      out->num_sacks = e->u32_val;
      inv_state = 2;
      continue;
    }

    if(inv_state == 2 && strcmp(e->key, "currentlyFocusedSackNumber") == 0)
    {
      out->focused_sack = e->u32_val;
      inv_state = 3;
      continue;
    }

    if(inv_state == 3 && strcmp(e->key, "currentlySelectedSackNumber") == 0)
    {
      out->selected_sack = e->u32_val;
      sack_idx = -1;
      inv_state = 4;
      continue;
    }

    // ── begin_block ──
    if(strcmp(e->key, "begin_block") == 0)
    {
      if(inv_state == 4)
      {
        sack_idx++;
        if(sack_idx < 8)
        {
          out->sacks[sack_idx].offset = e->offset;
          out->sacks[sack_idx].actual_count = 0;
          out->sacks[sack_idx].items = NULL;
          out->sacks[sack_idx].items_cap = 0;
        }
        inv_state = 5;
      }
      else if(inv_state == 7 && !in_item_outer)
      {
        memset(&cur_item, 0, sizeof(cur_item));
        cur_item.offset = e->offset;
        in_item_outer = true;
      }
      else if(inv_state == 7 && in_item_outer && !in_item_inner)
      {
        in_item_inner = true;
      }
      continue;
    }

    // ── end_block ──
    if(strcmp(e->key, "end_block") == 0)
    {
      if(inv_state == 7 && in_item_inner)
      {
        // Inner block closes
        in_item_inner = false;
      }
      else if(inv_state == 7 && in_item_outer && !in_item_inner)
      {
        // Outer block closes -- finalize item
        if(sack_idx >= 0 && sack_idx < 8)
          sack_add_item(&out->sacks[sack_idx], &cur_item);
        in_item_outer = false;
        memset(&cur_item, 0, sizeof(cur_item));
      }
      else if(inv_state == 7 && !in_item_outer)
      {
        // Sack ends
        if(sack_idx + 1 >= (int)out->num_sacks)
        {
          inv_state = 0;
          out->inv_end = e->next_offset;
        }
        else
        {
          inv_state = 4;
        }
      }
      else if(equip_end_pending)
      {
        out->equip_end = e->next_offset;
        equip_end_pending = 0;
      }
      else if(in_equipment && cur_alternate >= 0 && weapon_sub >= 2)
      {
        // Weapon set wrapper end_block
        cur_alternate = -1;
        if(equip_count >= 11)
          equip_slot = 11;
      }
      continue;
    }

    // ── Sack header ──
    if(strcmp(e->key, "tempBool") == 0 && inv_state == 5)
    {
      inv_state = 6;
      continue;
    }

    if(strcmp(e->key, "size") == 0 && inv_state == 6)
    {
      if(sack_idx >= 0 && sack_idx < 8)
        out->sacks[sack_idx].declared_size = e->u32_val;
      inv_state = 7;
      continue;
    }

    // ── Item fields (inventory) ──
    if(inv_state == 7 && in_item_inner)
    {
      if(strcmp(e->key, "baseName") == 0)
        g_strlcpy(cur_item.base_name, e->str_val, sizeof(cur_item.base_name));
      else if(strcmp(e->key, "prefixName") == 0)
        g_strlcpy(cur_item.prefix_name, e->str_val, sizeof(cur_item.prefix_name));
      else if(strcmp(e->key, "suffixName") == 0)
        g_strlcpy(cur_item.suffix_name, e->str_val, sizeof(cur_item.suffix_name));
      else if(strcmp(e->key, "relicName") == 0)
        g_strlcpy(cur_item.relic_name, e->str_val, sizeof(cur_item.relic_name));
      else if(strcmp(e->key, "relicBonus") == 0)
        g_strlcpy(cur_item.relic_bonus, e->str_val, sizeof(cur_item.relic_bonus));
      else if(strcmp(e->key, "relicName2") == 0)
      {
        g_strlcpy(cur_item.relic_name2, e->str_val, sizeof(cur_item.relic_name2));
        cur_item.has_atlantis = true;
      }
      else if(strcmp(e->key, "relicBonus2") == 0)
        g_strlcpy(cur_item.relic_bonus2, e->str_val, sizeof(cur_item.relic_bonus2));
      else if(strcmp(e->key, "seed") == 0)
        cur_item.seed = e->u32_val;
      else if(strcmp(e->key, "var1") == 0)
        cur_item.var1 = e->u32_val;
      else if(strcmp(e->key, "var2") == 0)
        cur_item.var2 = e->u32_val;
      continue;
    }

    // ── Item position (between inner end_block and outer end_block) ──
    if(inv_state == 7 && in_item_outer && !in_item_inner)
    {
      if(strcmp(e->key, "pointX") == 0)
        cur_item.point_x = (int32_t)e->u32_val;
      else if(strcmp(e->key, "pointY") == 0)
        cur_item.point_y = (int32_t)e->u32_val;
      continue;
    }

    // ── Equipment section ──
    if(in_equipment)
    {
      if(strcmp(e->key, "equipmentCtrlIOStreamVersion") == 0)
      {
        out->equip_version = e->u32_val;
        continue;
      }

      if(strcmp(e->key, "alternate") == 0)
      {
        cur_alternate = (int)e->u32_val;
        weapon_sub = 0;
        equip_slot = 7 + cur_alternate * 2;
        continue;
      }

      if(strcmp(e->key, "itemAttached") == 0)
      {
        if(equip_slot < 12)
          out->slots[equip_slot].attached = (e->u32_val != 0);

        equip_count++;

        if(cur_alternate >= 0)
        {
          weapon_sub++;
          if(weapon_sub < 2)
            equip_slot = 7 + cur_alternate * 2 + weapon_sub;
        }
        else if(equip_count < 7)
        {
          equip_slot = equip_count;
        }
        else
        {
          equip_slot = 11;
        }

        out->slots_parsed = equip_count;

        if(equip_count >= 12)
        {
          in_equipment = 0;
          equip_end_pending = 1;
        }
        continue;
      }

      // Item fields for current equipment slot
      if(equip_slot < 12)
      {
        RawEquipSlot *s = &out->slots[equip_slot];

        s->alternate = cur_alternate;
        s->offset = e->offset;

        if(strcmp(e->key, "baseName") == 0)
          g_strlcpy(s->base_name, e->str_val, sizeof(s->base_name));
        else if(strcmp(e->key, "prefixName") == 0)
          g_strlcpy(s->prefix_name, e->str_val, sizeof(s->prefix_name));
        else if(strcmp(e->key, "suffixName") == 0)
          g_strlcpy(s->suffix_name, e->str_val, sizeof(s->suffix_name));
        else if(strcmp(e->key, "relicName") == 0)
          g_strlcpy(s->relic_name, e->str_val, sizeof(s->relic_name));
        else if(strcmp(e->key, "relicBonus") == 0)
          g_strlcpy(s->relic_bonus, e->str_val, sizeof(s->relic_bonus));
        else if(strcmp(e->key, "relicName2") == 0)
        {
          g_strlcpy(s->relic_name2, e->str_val, sizeof(s->relic_name2));
          s->has_atlantis = true;
        }
        else if(strcmp(e->key, "relicBonus2") == 0)
          g_strlcpy(s->relic_bonus2, e->str_val, sizeof(s->relic_bonus2));
        else if(strcmp(e->key, "seed") == 0)
          s->seed = e->u32_val;
        else if(strcmp(e->key, "var1") == 0)
          s->var1 = e->u32_val;
        else if(strcmp(e->key, "var2") == 0)
          s->var2 = e->u32_val;
      }
    }
  }
}

// free_chr_parse -- free memory allocated by parse_chr_structured.
// p: parse result to free.
void
free_chr_parse(RawChrParse *p)
{
  for(int i = 0; i < 8; i++)
    free(p->sacks[i].items);
}
