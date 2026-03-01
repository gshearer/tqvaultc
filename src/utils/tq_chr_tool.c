// tq_chr_tool.c — Player.chr debugging/troubleshooting CLI
//
// Independent binary parser for Titan Quest .chr files. Does NOT reuse
// character_load() for analysis commands — has its own raw stream walker
// with a known-key table to avoid the heuristic bug (Bug 1 in TODO.md).
//
// Usage:
//   tq-chr-tool <command> [args...]
//
// Commands:
//   dump      <chr>                Raw key-value dump with offsets and types
//   inv       <chr>                Inventory listing: per-sack items
//   equip     <chr>                Equipment listing: 12 slots
//   compare   <chr_a> <chr_b>     Structural diff (flagship feature)
//   validate  <chr>                Structural integrity checks
//   hex       <chr> <section|offset> [len]   Hex dump of sections or offsets
//   roundtrip <chr>                Load via character_load(), save, compare

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>


// Stubs — character.c references these but we don't need them for analysis
#include "../character.h"
void vault_item_free_strings(TQVaultItem *item) {
  if (!item) return;
  free(item->base_name);
  free(item->prefix_name);
  free(item->suffix_name);
  free(item->relic_name);
  free(item->relic_bonus);
  free(item->relic_name2);
  free(item->relic_bonus2);
  free(item->stack_seeds);
  free(item->stack_var2);
}
int tqvc_debug = 0;

// ── Sentinel values ──────────────────────────────────────────────────────
#define TQ_BEGIN_BLOCK  0xB01DFACE
#define TQ_END_BLOCK    0xDEADC0DE

// ── Value types for known-key table ──────────────────────────────────────
typedef enum {
  VAL_U32,       // 4-byte unsigned integer
  VAL_FLOAT,     // 4-byte IEEE float
  VAL_STRING,    // length-prefixed ASCII string
  VAL_UTF16,     // length-prefixed UTF-16LE string (myPlayerName)
} ValType;

typedef struct {
  const char *name;
  ValType type;
} KnownKey;

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

// ── Parsed entry from raw binary stream ──────────────────────────────────
typedef struct {
  size_t offset;          // file offset of key length prefix
  int depth;              // block nesting depth
  char key[256];          // key name
  ValType type;           // resolved type
  bool ambiguous;         // true if type was guessed via heuristic
  uint32_t u32_val;       // for VAL_U32
  float float_val;        // for VAL_FLOAT
  char str_val[1024];     // for VAL_STRING / VAL_UTF16
  size_t val_offset;      // file offset of the value
  size_t next_offset;     // file offset after this entry
} ChrEntry;

// Dynamic array of entries
typedef struct {
  ChrEntry *entries;
  int count;
  int cap;
} ChrEntryList;

// ── Parsed item from structured parse ────────────────────────────────────
typedef struct {
  char base_name[512];
  char prefix_name[512];
  char suffix_name[512];
  char relic_name[512];
  char relic_bonus[512];
  char relic_name2[512];
  char relic_bonus2[512];
  uint32_t seed;
  uint32_t var1;
  uint32_t var2;
  int32_t point_x;
  int32_t point_y;
  size_t offset;          // start of outer begin_block
  bool has_atlantis;      // had relicName2/relicBonus2/var2
} RawItem;

typedef struct {
  uint32_t declared_size; // from "size" key
  int actual_count;       // items actually parsed
  RawItem *items;
  int items_cap;
  size_t offset;          // start of sack begin_block
} RawSack;

typedef struct {
  char base_name[512];
  char prefix_name[512];
  char suffix_name[512];
  char relic_name[512];
  char relic_bonus[512];
  char relic_name2[512];
  char relic_bonus2[512];
  uint32_t seed;
  uint32_t var1;
  uint32_t var2;
  bool attached;
  int alternate;          // -1 if not in weapon wrapper, 0 or 1
  size_t offset;
  bool has_atlantis;
} RawEquipSlot;

typedef struct {
  // Section boundaries
  size_t inv_start;       // offset of "numberOfSacks" key
  size_t inv_end;         // after last inventory end_block value
  size_t equip_start;     // after useAlternate's value
  size_t equip_end;       // after final equipment end_block value

  // Inventory
  uint32_t num_sacks;
  uint32_t focused_sack;
  uint32_t selected_sack;
  RawSack sacks[8];      // up to 8 sacks (generous)

  // Equipment
  uint32_t equip_version;
  RawEquipSlot slots[12];
  int slots_parsed;

  // File info
  size_t file_size;
  uint8_t *data;

  // Validation
  int errors;
  int warnings;
} RawChrParse;

// ── Low-level helpers ────────────────────────────────────────────────────

static uint8_t *load_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "error: cannot open '%s': ", path);
    perror(NULL);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fprintf(stderr, "error: '%s' is empty or unreadable\n", path);
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc((size_t)sz);
  if (!data) {
    fprintf(stderr, "error: malloc failed for %ld bytes\n", sz);
    fclose(f);
    return NULL;
  }
  if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf(stderr, "error: short read on '%s'\n", path);
    free(data);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_size = (size_t)sz;
  return data;
}

static inline uint32_t rd_u32(const uint8_t *data, size_t off) {
  uint32_t v;
  memcpy(&v, data + off, 4);
  return v;
}

static inline float rd_float(const uint8_t *data, size_t off) {
  float v;
  memcpy(&v, data + off, 4);
  return v;
}

// Read length-prefixed ASCII string into buf. Returns bytes consumed.
static size_t rd_string(const uint8_t *data, size_t off, size_t file_size,
                        char *buf, size_t buf_size) {
  uint32_t len = rd_u32(data, off);
  if (len == 0 || off + 4 + len > file_size) {
    buf[0] = '\0';
    return 4;
  }
  size_t copy = len < buf_size - 1 ? len : buf_size - 1;
  memcpy(buf, data + off + 4, copy);
  buf[copy] = '\0';
  return 4 + len;
}

// Read length-prefixed UTF-16LE string, convert to ASCII. Returns bytes consumed.
static size_t rd_utf16(const uint8_t *data, size_t off, size_t file_size,
                       char *buf, size_t buf_size) {
  uint32_t len = rd_u32(data, off);
  if (len == 0 || off + 4 + len * 2 > file_size) {
    buf[0] = '\0';
    return 4;
  }
  size_t copy = len < buf_size - 1 ? len : buf_size - 1;
  for (size_t i = 0; i < copy; i++)
    buf[i] = (char)data[off + 4 + i * 2];
  buf[copy] = '\0';
  return 4 + len * 2;
}

static const KnownKey *lookup_key(const char *name) {
  for (int i = 0; known_keys[i].name; i++) {
    if (strcmp(known_keys[i].name, name) == 0)
      return &known_keys[i];
  }
  return NULL;
}

// ── Entry-level parser ───────────────────────────────────────────────────
// Walks the raw binary stream, using the known-key table to determine types.
// Unknown keys fall back to the heuristic but are flagged as AMBIGUOUS.

static void entry_list_init(ChrEntryList *list) {
  list->count = 0;
  list->cap = 4096;
  list->entries = malloc(list->cap * sizeof(ChrEntry));
}

static void entry_list_push(ChrEntryList *list, const ChrEntry *e) {
  if (list->count >= list->cap) {
    list->cap *= 2;
    list->entries = realloc(list->entries, list->cap * sizeof(ChrEntry));
  }
  list->entries[list->count++] = *e;
}

static void entry_list_free(ChrEntryList *list) {
  free(list->entries);
  list->entries = NULL;
  list->count = list->cap = 0;
}

static int parse_entries(const uint8_t *data, size_t file_size,
                         ChrEntryList *out) {
  entry_list_init(out);
  size_t offset = 0;
  int depth = 0;

  while (offset + 4 <= file_size) {
    size_t key_start = offset;
    uint32_t klen = rd_u32(data, offset);

    // Validate: plausible key length with printable ASCII
    if (klen == 0 || klen >= 256 || offset + 4 + klen > file_size) {
      offset++;
      continue;
    }

    bool printable = true;
    for (uint32_t i = 0; i < klen; i++) {
      if (!isprint(data[offset + 4 + i])) {
        printable = false;
        break;
      }
    }
    if (!printable) {
      offset++;
      continue;
    }

    ChrEntry e = {0};
    e.offset = key_start;
    memcpy(e.key, data + offset + 4, klen);
    e.key[klen] = '\0';
    offset += 4 + klen;
    e.val_offset = offset;

    if (offset + 4 > file_size) break;

    // Track block depth
    if (strcmp(e.key, "begin_block") == 0) {
      e.type = VAL_U32;
      e.u32_val = rd_u32(data, offset);
      e.depth = depth;
      depth++;
      offset += 4;
    } else if (strcmp(e.key, "end_block") == 0) {
      depth--;
      if (depth < 0) depth = 0;
      e.type = VAL_U32;
      e.u32_val = rd_u32(data, offset);
      e.depth = depth;
      offset += 4;
    } else {
      e.depth = depth;

      const KnownKey *kk = lookup_key(e.key);
      if (kk) {
        e.type = kk->type;
        e.ambiguous = false;
        switch (kk->type) {
          case VAL_U32:
            e.u32_val = rd_u32(data, offset);
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
                               e.str_val, sizeof(e.str_val));
            break;
        }
      } else {
        // Heuristic fallback (same as character.c but flagged)
        e.ambiguous = true;
        uint32_t val = rd_u32(data, offset);
        if (val > 0 && val < 512 && offset + 4 + val <= file_size) {
          // Looks like a string
          e.type = VAL_STRING;
          size_t copy = val < sizeof(e.str_val) - 1 ? val : sizeof(e.str_val) - 1;
          memcpy(e.str_val, data + offset + 4, copy);
          e.str_val[copy] = '\0';
          offset += 4 + val;
        } else {
          e.type = VAL_U32;
          e.u32_val = val;
          offset += 4;
        }
      }
    }

    e.next_offset = offset;
    entry_list_push(out, &e);
  }

  return out->count;
}

// ── Structured chr parser ────────────────────────────────────────────────
// Builds inventory/equipment structures from the entry list.

static void sack_add_item(RawSack *sack, const RawItem *item) {
  if (sack->actual_count >= sack->items_cap) {
    sack->items_cap = sack->items_cap ? sack->items_cap * 2 : 64;
    sack->items = realloc(sack->items, sack->items_cap * sizeof(RawItem));
  }
  sack->items[sack->actual_count++] = *item;
}

static void parse_chr_structured(const ChrEntryList *entries,
                                 RawChrParse *out) {
  memset(out, 0, sizeof(*out));
  for (int i = 0; i < 12; i++)
    out->slots[i].alternate = -1;

  // State machine — mirrors character.c but with correct type handling
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

  for (int i = 0; i < entries->count; i++) {
    const ChrEntry *e = &entries->entries[i];

    // ── Section triggers ──
    if (strcmp(e->key, "itemPositionsSavedAsGridCoords") == 0) {
      inv_state = 1;
      continue;
    }

    if (strcmp(e->key, "useAlternate") == 0) {
      out->equip_start = e->next_offset;
      in_equipment = 1;
      equip_slot = 0;
      continue;
    }

    // ── Inventory header ──
    if (inv_state == 1 && strcmp(e->key, "numberOfSacks") == 0) {
      out->inv_start = e->offset;
      out->num_sacks = e->u32_val;
      inv_state = 2;
      continue;
    }
    if (inv_state == 2 && strcmp(e->key, "currentlyFocusedSackNumber") == 0) {
      out->focused_sack = e->u32_val;
      inv_state = 3;
      continue;
    }
    if (inv_state == 3 && strcmp(e->key, "currentlySelectedSackNumber") == 0) {
      out->selected_sack = e->u32_val;
      sack_idx = -1;
      inv_state = 4;
      continue;
    }

    // ── begin_block ──
    if (strcmp(e->key, "begin_block") == 0) {
      if (inv_state == 4) {
        sack_idx++;
        if (sack_idx < 8) {
          out->sacks[sack_idx].offset = e->offset;
          out->sacks[sack_idx].actual_count = 0;
          out->sacks[sack_idx].items = NULL;
          out->sacks[sack_idx].items_cap = 0;
        }
        inv_state = 5;
      } else if (inv_state == 7 && !in_item_outer) {
        memset(&cur_item, 0, sizeof(cur_item));
        cur_item.offset = e->offset;
        in_item_outer = true;
      } else if (inv_state == 7 && in_item_outer && !in_item_inner) {
        in_item_inner = true;
      }
      continue;
    }

    // ── end_block ──
    if (strcmp(e->key, "end_block") == 0) {
      if (inv_state == 7 && in_item_inner) {
        // Inner block closes
        in_item_inner = false;
      } else if (inv_state == 7 && in_item_outer && !in_item_inner) {
        // Outer block closes — finalize item
        if (sack_idx >= 0 && sack_idx < 8)
          sack_add_item(&out->sacks[sack_idx], &cur_item);
        in_item_outer = false;
        memset(&cur_item, 0, sizeof(cur_item));
      } else if (inv_state == 7 && !in_item_outer) {
        // Sack ends
        if (sack_idx + 1 >= (int)out->num_sacks) {
          inv_state = 0;
          out->inv_end = e->next_offset;
        } else {
          inv_state = 4;
        }
      } else if (equip_end_pending) {
        out->equip_end = e->next_offset;
        equip_end_pending = 0;
      } else if (in_equipment && cur_alternate >= 0 && weapon_sub >= 2) {
        // Weapon set wrapper end_block
        cur_alternate = -1;
        if (equip_count >= 11) equip_slot = 11;
      }
      continue;
    }

    // ── Sack header ──
    if (strcmp(e->key, "tempBool") == 0 && inv_state == 5) {
      inv_state = 6;
      continue;
    }
    if (strcmp(e->key, "size") == 0 && inv_state == 6) {
      if (sack_idx >= 0 && sack_idx < 8)
        out->sacks[sack_idx].declared_size = e->u32_val;
      inv_state = 7;
      continue;
    }

    // ── Item fields (inventory) ──
    if (inv_state == 7 && in_item_inner) {
      if (strcmp(e->key, "baseName") == 0)
        strncpy(cur_item.base_name, e->str_val, sizeof(cur_item.base_name) - 1);
      else if (strcmp(e->key, "prefixName") == 0)
        strncpy(cur_item.prefix_name, e->str_val, sizeof(cur_item.prefix_name) - 1);
      else if (strcmp(e->key, "suffixName") == 0)
        strncpy(cur_item.suffix_name, e->str_val, sizeof(cur_item.suffix_name) - 1);
      else if (strcmp(e->key, "relicName") == 0)
        strncpy(cur_item.relic_name, e->str_val, sizeof(cur_item.relic_name) - 1);
      else if (strcmp(e->key, "relicBonus") == 0)
        strncpy(cur_item.relic_bonus, e->str_val, sizeof(cur_item.relic_bonus) - 1);
      else if (strcmp(e->key, "relicName2") == 0) {
        strncpy(cur_item.relic_name2, e->str_val, sizeof(cur_item.relic_name2) - 1);
        cur_item.has_atlantis = true;
      } else if (strcmp(e->key, "relicBonus2") == 0)
        strncpy(cur_item.relic_bonus2, e->str_val, sizeof(cur_item.relic_bonus2) - 1);
      else if (strcmp(e->key, "seed") == 0)
        cur_item.seed = e->u32_val;
      else if (strcmp(e->key, "var1") == 0)
        cur_item.var1 = e->u32_val;
      else if (strcmp(e->key, "var2") == 0)
        cur_item.var2 = e->u32_val;
      continue;
    }

    // ── Item position (between inner end_block and outer end_block) ──
    if (inv_state == 7 && in_item_outer && !in_item_inner) {
      if (strcmp(e->key, "pointX") == 0)
        cur_item.point_x = (int32_t)e->u32_val;
      else if (strcmp(e->key, "pointY") == 0)
        cur_item.point_y = (int32_t)e->u32_val;
      continue;
    }

    // ── Equipment section ──
    if (in_equipment) {
      if (strcmp(e->key, "equipmentCtrlIOStreamVersion") == 0) {
        out->equip_version = e->u32_val;
        continue;
      }
      if (strcmp(e->key, "alternate") == 0) {
        cur_alternate = (int)e->u32_val;
        weapon_sub = 0;
        equip_slot = 7 + cur_alternate * 2;
        continue;
      }
      if (strcmp(e->key, "itemAttached") == 0) {
        if (equip_slot < 12)
          out->slots[equip_slot].attached = (e->u32_val != 0);
        equip_count++;
        if (cur_alternate >= 0) {
          weapon_sub++;
          if (weapon_sub < 2)
            equip_slot = 7 + cur_alternate * 2 + weapon_sub;
        } else if (equip_count < 7) {
          equip_slot = equip_count;
        } else {
          equip_slot = 11;
        }
        out->slots_parsed = equip_count;
        if (equip_count >= 12) {
          in_equipment = 0;
          equip_end_pending = 1;
        }
        continue;
      }

      // Item fields for current equipment slot
      if (equip_slot < 12) {
        RawEquipSlot *s = &out->slots[equip_slot];
        s->alternate = cur_alternate;
        s->offset = e->offset;
        if (strcmp(e->key, "baseName") == 0)
          strncpy(s->base_name, e->str_val, sizeof(s->base_name) - 1);
        else if (strcmp(e->key, "prefixName") == 0)
          strncpy(s->prefix_name, e->str_val, sizeof(s->prefix_name) - 1);
        else if (strcmp(e->key, "suffixName") == 0)
          strncpy(s->suffix_name, e->str_val, sizeof(s->suffix_name) - 1);
        else if (strcmp(e->key, "relicName") == 0)
          strncpy(s->relic_name, e->str_val, sizeof(s->relic_name) - 1);
        else if (strcmp(e->key, "relicBonus") == 0)
          strncpy(s->relic_bonus, e->str_val, sizeof(s->relic_bonus) - 1);
        else if (strcmp(e->key, "relicName2") == 0) {
          strncpy(s->relic_name2, e->str_val, sizeof(s->relic_name2) - 1);
          s->has_atlantis = true;
        } else if (strcmp(e->key, "relicBonus2") == 0)
          strncpy(s->relic_bonus2, e->str_val, sizeof(s->relic_bonus2) - 1);
        else if (strcmp(e->key, "seed") == 0)
          s->seed = e->u32_val;
        else if (strcmp(e->key, "var1") == 0)
          s->var1 = e->u32_val;
        else if (strcmp(e->key, "var2") == 0)
          s->var2 = e->u32_val;
      }
    }
  }
}

static void free_chr_parse(RawChrParse *p) {
  for (int i = 0; i < 8; i++)
    free(p->sacks[i].items);
}

// ── Hex dump helper ──────────────────────────────────────────────────────

static void print_hex_line(const uint8_t *data, size_t offset, size_t len) {
  printf("  %08zx: ", offset);
  for (size_t i = 0; i < 16; i++) {
    if (i < len)
      printf("%02x ", data[offset + i]);
    else
      printf("   ");
    if (i == 7) printf(" ");
  }
  printf(" |");
  for (size_t i = 0; i < 16 && i < len; i++) {
    uint8_t c = data[offset + i];
    printf("%c", (c >= 32 && c < 127) ? c : '.');
  }
  printf("|\n");
}

static void hex_dump_range(const uint8_t *data, size_t start, size_t len) {
  for (size_t off = 0; off < len; off += 16) {
    size_t chunk = (len - off) < 16 ? (len - off) : 16;
    print_hex_line(data, start + off, chunk);
  }
}

// ── Equipment slot names ─────────────────────────────────────────────────

static const char *equip_slot_name(int slot) {
  static const char *names[] = {
    "Head", "Neck", "Chest", "Legs", "Arms",
    "Ring1", "Ring2", "Weapon1", "Shield1",
    "Weapon2", "Shield2", "Artifact"
  };
  if (slot >= 0 && slot < 12) return names[slot];
  return "Unknown";
}

// ── Basename tail: last path component for readable output ───────────────

static const char *basename_tail(const char *path) {
  if (!path || !*path) return "(empty)";
  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  const char *last = slash > bslash ? slash : bslash;
  return last ? last + 1 : path;
}

// ═══════════════════════════════════════════════════════════════════════════
//  COMMANDS
// ═══════════════════════════════════════════════════════════════════════════

// ── cmd_dump ─────────────────────────────────────────────────────────────
// Raw key-value dump with file offsets, block depth, and value types.

static int cmd_dump(const char *path) {
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);
  if (!data) return 1;

  ChrEntryList entries;
  int count = parse_entries(data, file_size, &entries);

  printf("=== %s (%zu bytes, %d entries) ===\n\n", path, file_size, count);

  for (int i = 0; i < entries.count; i++) {
    const ChrEntry *e = &entries.entries[i];

    // Indentation by depth
    for (int d = 0; d < e->depth; d++) printf("  ");

    // Offset and key
    printf("@%06zx ", e->offset);

    if (strcmp(e->key, "begin_block") == 0) {
      printf("BEGIN_BLOCK (0x%08X)\n", e->u32_val);
      continue;
    }
    if (strcmp(e->key, "end_block") == 0) {
      printf("END_BLOCK (0x%08X)\n", e->u32_val);
      continue;
    }

    // Key name
    printf("%-40s = ", e->key);

    // Value
    switch (e->type) {
      case VAL_U32: {
        float fv;
        memcpy(&fv, &e->u32_val, 4);
        if (e->u32_val == 0)
          printf("0");
        else if (e->u32_val == 0xFFFFFFFF)
          printf("-1 (0xFFFFFFFF)");
        else if (e->u32_val < 100000)
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

    if (e->ambiguous) printf("  [AMBIGUOUS]");
    printf("\n");
  }

  entry_list_free(&entries);
  free(data);
  return 0;
}

// ── cmd_inv ──────────────────────────────────────────────────────────────
// Inventory listing: per-sack items with all fields.

static int cmd_inv(const char *path) {
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);
  if (!data) return 1;

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

  for (uint32_t s = 0; s < parse.num_sacks && s < 8; s++) {
    RawSack *sack = &parse.sacks[s];
    printf("── Sack %u (declared_size=%u, actual=%d) ──\n",
           s, sack->declared_size, sack->actual_count);

    // Count unique items (collapse stacks with point_x=-1)
    int unique = 0;
    for (int i = 0; i < sack->actual_count; i++) {
      if (sack->items[i].point_x != -1 || sack->items[i].point_y != -1)
        unique++;
      else if (i == 0)
        unique++;  // edge case: first item at -1,-1
    }
    printf("  unique positions: %d, expanded entries: %d\n\n", unique, sack->actual_count);

    for (int i = 0; i < sack->actual_count; i++) {
      RawItem *it = &sack->items[i];
      printf("  [%d] @%06zx  pos=(%d,%d)  seed=0x%08X\n",
             i, it->offset, it->point_x, it->point_y, it->seed);
      if (it->base_name[0])
        printf("      base:   %s\n", it->base_name);
      if (it->prefix_name[0])
        printf("      prefix: %s\n", it->prefix_name);
      if (it->suffix_name[0])
        printf("      suffix: %s\n", it->suffix_name);
      if (it->relic_name[0])
        printf("      relic:  %s\n", it->relic_name);
      if (it->relic_bonus[0])
        printf("      bonus:  %s\n", it->relic_bonus);
      if (it->relic_name2[0])
        printf("      relic2: %s\n", it->relic_name2);
      if (it->relic_bonus2[0])
        printf("      bonus2: %s\n", it->relic_bonus2);
      if (it->var1 || it->var2)
        printf("      var1=%u  var2=0x%08X\n", it->var1, it->var2);
    }
    printf("\n");
  }

  free_chr_parse(&parse);
  entry_list_free(&entries);
  free(data);
  return 0;
}

// ── cmd_equip ────────────────────────────────────────────────────────────
// Equipment listing: 12 slots with alternate flags.

static int cmd_equip(const char *path) {
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);
  if (!data) return 1;

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

  for (int i = 0; i < 12; i++) {
    RawEquipSlot *s = &parse.slots[i];
    printf("  [%2d] %-10s  attached=%d  alternate=%d",
           i, equip_slot_name(i), s->attached, s->alternate);

    if (s->base_name[0]) {
      printf("  %s\n", basename_tail(s->base_name));
      printf("       base:   %s\n", s->base_name);
      if (s->prefix_name[0])  printf("       prefix: %s\n", s->prefix_name);
      if (s->suffix_name[0])  printf("       suffix: %s\n", s->suffix_name);
      if (s->relic_name[0])   printf("       relic:  %s\n", s->relic_name);
      if (s->relic_bonus[0])  printf("       bonus:  %s\n", s->relic_bonus);
      if (s->relic_name2[0])  printf("       relic2: %s\n", s->relic_name2);
      if (s->relic_bonus2[0]) printf("       bonus2: %s\n", s->relic_bonus2);
      printf("       seed=0x%08X  var1=%u  var2=0x%08X\n",
             s->seed, s->var1, s->var2);
    } else {
      printf("  (empty)");
      if (s->var2 != 0)
        printf("  var2=0x%08X", s->var2);
      printf("\n");
    }
  }

  free_chr_parse(&parse);
  entry_list_free(&entries);
  free(data);
  return 0;
}

// ── cmd_validate ─────────────────────────────────────────────────────────
// Structural integrity checks.

static int cmd_validate(const char *path) {
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);
  if (!data) return 1;

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

  for (int i = 0; i < entries.count; i++) {
    const ChrEntry *e = &entries.entries[i];
    if (strcmp(e->key, "begin_block") == 0) {
      begin_count++;
      depth++;
      if (depth > max_depth) max_depth = depth;
      if (e->u32_val != TQ_BEGIN_BLOCK) {
        printf("  ERROR: begin_block @%06zx has unexpected sentinel 0x%08X\n",
               e->offset, e->u32_val);
        errors++;
      }
    } else if (strcmp(e->key, "end_block") == 0) {
      end_count++;
      depth--;
      if (depth < 0) {
        printf("  ERROR: end_block @%06zx causes negative depth\n", e->offset);
        errors++;
        depth = 0;
      }
      if (e->u32_val != TQ_END_BLOCK) {
        printf("  ERROR: end_block @%06zx has unexpected sentinel 0x%08X\n",
               e->offset, e->u32_val);
        errors++;
      }
    }
  }

  if (depth != 0) {
    printf("  ERROR: unclosed blocks — final depth = %d\n", depth);
    errors++;
  }

  printf("  blocks: %d begin, %d end, max depth = %d\n",
         begin_count, end_count, max_depth);

  if (begin_count == end_count && depth == 0)
    printf("  block nesting: OK\n");

  // 2. Check for ambiguous keys
  int ambiguous_count = 0;
  for (int i = 0; i < entries.count; i++) {
    if (entries.entries[i].ambiguous)
      ambiguous_count++;
  }
  if (ambiguous_count > 0) {
    printf("\n  WARNING: %d keys used heuristic type detection (AMBIGUOUS)\n",
           ambiguous_count);
    warnings++;
    for (int i = 0; i < entries.count; i++) {
      const ChrEntry *e = &entries.entries[i];
      if (e->ambiguous) {
        printf("    @%06zx %-40s ", e->offset, e->key);
        if (e->type == VAL_STRING)
          printf("-> string \"%s\"\n", e->str_val);
        else
          printf("-> u32 %u (0x%08X)\n", e->u32_val, e->u32_val);
      }
    }
  } else {
    printf("  ambiguous keys: none (all keys recognized)\n");
  }

  // 3. Structured parse validation
  RawChrParse parse;
  parse.data = data;
  parse.file_size = file_size;
  parse_chr_structured(&entries, &parse);

  printf("\n  ── Inventory ──\n");
  printf("  numberOfSacks: %u\n", parse.num_sacks);
  printf("  inv_block: [%zu..%zu)\n", parse.inv_start, parse.inv_end);

  if (parse.inv_start == 0) {
    printf("  ERROR: inventory section not found\n");
    errors++;
  }

  for (uint32_t s = 0; s < parse.num_sacks && s < 8; s++) {
    RawSack *sk = &parse.sacks[s];
    printf("  sack[%u]: declared=%u  actual=%d",
           s, sk->declared_size, sk->actual_count);
    if (sk->declared_size != (uint32_t)sk->actual_count) {
      printf("  ** MISMATCH **");
      warnings++;
    }
    printf("\n");
  }

  printf("\n  ── Equipment ──\n");
  printf("  equip_block: [%zu..%zu)\n", parse.equip_start, parse.equip_end);
  printf("  version: %u, slots_parsed: %d\n",
         parse.equip_version, parse.slots_parsed);

  if (parse.equip_start == 0) {
    printf("  ERROR: equipment section not found\n");
    errors++;
  }
  if (parse.slots_parsed != 12) {
    printf("  ERROR: expected 12 equipment slots, got %d\n", parse.slots_parsed);
    errors++;
  }

  // Check weapon set ordering
  printf("\n  ── Weapon Sets ──\n");
  for (int i = 7; i <= 10; i++) {
    printf("  slot[%d] %-10s  alternate=%d  attached=%d",
           i, equip_slot_name(i), parse.slots[i].alternate,
           parse.slots[i].attached);
    if (parse.slots[i].base_name[0])
      printf("  %s", basename_tail(parse.slots[i].base_name));
    printf("\n");
  }

  // Check boundary ordering
  printf("\n  ── Section Boundaries ──\n");
  printf("  prefix:    [0..%zu)\n", parse.inv_start);
  printf("  inventory: [%zu..%zu)\n", parse.inv_start, parse.inv_end);
  if (parse.inv_end > 0 && parse.equip_start > 0) {
    printf("  middle:    [%zu..%zu) = %zu bytes\n",
           parse.inv_end, parse.equip_start,
           parse.equip_start - parse.inv_end);
  }
  printf("  equipment: [%zu..%zu)\n", parse.equip_start, parse.equip_end);
  printf("  suffix:    [%zu..%zu)\n", parse.equip_end, file_size);

  if (parse.inv_end > parse.equip_start && parse.inv_end > 0 && parse.equip_start > 0) {
    printf("  ERROR: inventory end (%zu) > equipment start (%zu)\n",
           parse.inv_end, parse.equip_start);
    errors++;
  }

  // Check empty slot var2 values
  printf("\n  ── Empty Slot var2 Values ──\n");
  bool any_nonzero_var2 = false;
  for (int i = 0; i < 12; i++) {
    if (!parse.slots[i].base_name[0] && parse.slots[i].var2 != 0) {
      printf("  slot[%d] %-10s  var2=0x%08X (non-zero on empty slot)\n",
             i, equip_slot_name(i), parse.slots[i].var2);
      any_nonzero_var2 = true;
      warnings++;
    }
  }
  if (!any_nonzero_var2)
    printf("  (all empty slots have var2=0)\n");

  printf("\n  ══ Summary: %d errors, %d warnings ══\n", errors, warnings);

  free_chr_parse(&parse);
  entry_list_free(&entries);
  free(data);
  return errors > 0 ? 1 : 0;
}

// ── cmd_hex ──────────────────────────────────────────────────────────────
// Hex dump of named sections or arbitrary offsets.

static int cmd_hex(const char *path, const char *section, const char *len_str) {
  size_t file_size;
  uint8_t *data = load_file(path, &file_size);
  if (!data) return 1;

  size_t start = 0;
  size_t len = 0;

  // Try named section first
  if (strcmp(section, "prefix") == 0 || strcmp(section, "inventory") == 0 ||
      strcmp(section, "middle") == 0 || strcmp(section, "equipment") == 0 ||
      strcmp(section, "suffix") == 0) {
    // Need structural parse for boundaries
    ChrEntryList entries;
    parse_entries(data, file_size, &entries);
    RawChrParse parse;
    parse.data = data;
    parse.file_size = file_size;
    parse_chr_structured(&entries, &parse);

    if (parse.inv_start == 0 || parse.equip_start == 0) {
      fprintf(stderr, "error: could not determine section boundaries\n");
      entry_list_free(&entries);
      free(data);
      return 1;
    }

    if (strcmp(section, "prefix") == 0) {
      start = 0;
      len = parse.inv_start;
    } else if (strcmp(section, "inventory") == 0) {
      start = parse.inv_start;
      len = parse.inv_end - parse.inv_start;
    } else if (strcmp(section, "middle") == 0) {
      start = parse.inv_end;
      len = parse.equip_start - parse.inv_end;
    } else if (strcmp(section, "equipment") == 0) {
      start = parse.equip_start;
      len = parse.equip_end - parse.equip_start;
    } else if (strcmp(section, "suffix") == 0) {
      start = parse.equip_end;
      len = file_size - parse.equip_end;
    }

    printf("=== %s section: [%zu..%zu) = %zu bytes ===\n\n",
           section, start, start + len, len);

    free_chr_parse(&parse);
    entry_list_free(&entries);
  } else {
    // Numeric offset
    char *endptr;
    unsigned long off = strtoul(section, &endptr, 0);
    if (*endptr != '\0') {
      fprintf(stderr, "error: unknown section '%s'\n"
              "  valid sections: prefix, inventory, middle, equipment, suffix\n"
              "  or a numeric offset (decimal or 0x hex)\n", section);
      free(data);
      return 1;
    }
    start = (size_t)off;
    if (start >= file_size) {
      fprintf(stderr, "error: offset %zu beyond file size %zu\n",
              start, file_size);
      free(data);
      return 1;
    }
    len = len_str ? (size_t)strtoul(len_str, NULL, 0) : 256;
    if (start + len > file_size)
      len = file_size - start;

    printf("=== hex dump @%zu (0x%zx), %zu bytes ===\n\n", start, start, len);
  }

  hex_dump_range(data, start, len);

  free(data);
  return 0;
}

// ── cmd_compare ──────────────────────────────────────────────────────────
// Structural diff between two .chr files. The flagship debugging command.

static void compare_bytes(const char *label,
                          const uint8_t *a, size_t a_off, size_t a_len,
                          const uint8_t *b, size_t b_off, size_t b_len,
                          int *diffs) {
  if (a_len != b_len) {
    printf("  %-20s SIZE DIFFERS: %zu vs %zu bytes (delta %+zd)\n",
           label, a_len, b_len, (ssize_t)(b_len - a_len));
    (*diffs)++;
  } else if (memcmp(a + a_off, b + b_off, a_len) != 0) {
    // Find first difference
    size_t first_diff = 0;
    int diff_count = 0;
    for (size_t i = 0; i < a_len; i++) {
      if (a[a_off + i] != b[b_off + i]) {
        if (diff_count == 0) first_diff = i;
        diff_count++;
      }
    }
    printf("  %-20s %d byte(s) differ (first at +%zu)\n",
           label, diff_count, first_diff);
    (*diffs)++;
  } else {
    printf("  %-20s identical (%zu bytes)\n", label, a_len);
  }
}

static void compare_string_field(const char *label, const char *a,
                                 const char *b, int *diffs) {
  bool a_empty = (!a || !*a);
  bool b_empty = (!b || !*b);
  if (a_empty && b_empty) return;
  if (a_empty != b_empty || strcmp(a ? a : "", b ? b : "") != 0) {
    printf("      %s: \"%s\" -> \"%s\"\n", label,
           a_empty ? "" : a, b_empty ? "" : b);
    (*diffs)++;
  }
}

static void compare_u32_field(const char *label, uint32_t a, uint32_t b,
                              int *diffs) {
  if (a != b) {
    printf("      %s: %u (0x%08X) -> %u (0x%08X)\n", label, a, a, b, b);
    (*diffs)++;
  }
}

static int cmd_compare(const char *path_a, const char *path_b) {
  size_t size_a, size_b;
  uint8_t *data_a = load_file(path_a, &size_a);
  if (!data_a) return 1;
  uint8_t *data_b = load_file(path_b, &size_b);
  if (!data_b) { free(data_a); return 1; }

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
  if (size_a != size_b)
    printf("  Size delta: %+zd bytes\n", (ssize_t)(size_b - size_a));
  printf("\n");

  int diffs = 0;

  // ── Pass 1: Section byte comparison ──
  printf("── Section Comparison ──\n");
  printf("  File A boundaries: inv=[%zu..%zu) equip=[%zu..%zu)\n",
         pa.inv_start, pa.inv_end, pa.equip_start, pa.equip_end);
  printf("  File B boundaries: inv=[%zu..%zu) equip=[%zu..%zu)\n",
         pb.inv_start, pb.inv_end, pb.equip_start, pb.equip_end);
  printf("\n");

  if (pa.inv_start == 0 || pb.inv_start == 0 ||
      pa.equip_start == 0 || pb.equip_start == 0) {
    printf("  ERROR: could not determine boundaries for both files\n");
    diffs++;
  } else {
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
  printf("\n── Inventory Header ──\n");
  compare_u32_field("numberOfSacks", pa.num_sacks, pb.num_sacks, &diffs);
  compare_u32_field("focusedSack", pa.focused_sack, pb.focused_sack, &diffs);
  compare_u32_field("selectedSack", pa.selected_sack, pb.selected_sack, &diffs);

  // ── Pass 3: Per-sack item comparison ──
  uint32_t max_sacks = pa.num_sacks > pb.num_sacks ? pa.num_sacks : pb.num_sacks;
  if (max_sacks > 8) max_sacks = 8;

  for (uint32_t s = 0; s < max_sacks; s++) {
    printf("\n── Sack %u ──\n", s);
    RawSack *sa = (s < pa.num_sacks) ? &pa.sacks[s] : NULL;
    RawSack *sb = (s < pb.num_sacks) ? &pb.sacks[s] : NULL;

    if (!sa) { printf("  MISSING in file A\n"); diffs++; continue; }
    if (!sb) { printf("  MISSING in file B\n"); diffs++; continue; }

    compare_u32_field("declared_size", sa->declared_size,
                      sb->declared_size, &diffs);
    if (sa->actual_count != sb->actual_count) {
      printf("      actual_count: %d -> %d\n", sa->actual_count, sb->actual_count);
      diffs++;
    }

    // Match items by base_name + position
    int matched_b[2048] = {0};  // track which B items were matched
    int max_items = sa->actual_count > sb->actual_count ?
                    sa->actual_count : sb->actual_count;

    for (int ia = 0; ia < sa->actual_count; ia++) {
      RawItem *a = &sa->items[ia];
      // Find matching item in B
      int ib_match = -1;
      for (int ib = 0; ib < sb->actual_count; ib++) {
        if (matched_b[ib]) continue;
        RawItem *b = &sb->items[ib];
        if (strcmp(a->base_name, b->base_name) == 0 &&
            a->point_x == b->point_x && a->point_y == b->point_y) {
          ib_match = ib;
          break;
        }
      }

      if (ib_match < 0) {
        // Try looser match: same base_name, same position coords
        for (int ib = 0; ib < sb->actual_count; ib++) {
          if (matched_b[ib]) continue;
          if (strcmp(a->base_name, sb->items[ib].base_name) == 0) {
            ib_match = ib;
            break;
          }
        }
      }

      if (ib_match >= 0) {
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

        if (a->point_x != b->point_x || a->point_y != b->point_y) {
          printf("      position: (%d,%d) -> (%d,%d)\n",
                 a->point_x, a->point_y, b->point_x, b->point_y);
          local_diffs++;
        }

        item_diffs = local_diffs;
        if (item_diffs > 0) {
          printf("    item[%d] %s: %d difference(s)\n",
                 ia, basename_tail(a->base_name), item_diffs);
          diffs += item_diffs;
        }
      } else {
        printf("    item[%d] ONLY IN A: %s at (%d,%d)\n",
               ia, basename_tail(a->base_name), a->point_x, a->point_y);
        diffs++;
      }
    }

    // Report unmatched B items
    for (int ib = 0; ib < sb->actual_count; ib++) {
      if (!matched_b[ib]) {
        RawItem *b = &sb->items[ib];
        printf("    item[%d] ONLY IN B: %s at (%d,%d)\n",
               ib, basename_tail(b->base_name), b->point_x, b->point_y);
        diffs++;
      }
    }

    (void)max_items;
  }

  // ── Pass 4: Equipment comparison ──
  printf("\n── Equipment ──\n");
  compare_u32_field("version", pa.equip_version, pb.equip_version, &diffs);

  for (int i = 0; i < 12; i++) {
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

    if (a->attached != b->attached) {
      printf("      attached: %d -> %d\n", a->attached, b->attached);
      local_diffs++;
    }
    if (a->alternate != b->alternate) {
      printf("      alternate: %d -> %d\n", a->alternate, b->alternate);
      local_diffs++;
    }

    slot_diffs = local_diffs;
    if (slot_diffs > 0) {
      printf("  slot[%2d] %-10s: %d difference(s)\n",
             i, equip_slot_name(i), slot_diffs);
      diffs += slot_diffs;
    }
  }

  // ── Summary ──
  printf("\n══ Summary: %d total differences ══\n", diffs);

  free_chr_parse(&pa);
  free_chr_parse(&pb);
  entry_list_free(&entries_a);
  entry_list_free(&entries_b);
  free(data_a);
  free(data_b);
  return diffs > 0 ? 1 : 0;
}

// ── cmd_roundtrip ────────────────────────────────────────────────────────
// Load via character_load(), save to /tmp, then run compare logic.

static int cmd_roundtrip(const char *path) {
  printf("=== Roundtrip: %s ===\n\n", path);

  // Load original file size for later comparison
  size_t orig_size;
  uint8_t *orig_data = load_file(path, &orig_size);
  if (!orig_data) return 1;

  // Use character_load() / character_save()
  tqvc_debug = 1;
  TQCharacter *chr = character_load(path);
  if (!chr) {
    fprintf(stderr, "error: character_load() failed\n");
    free(orig_data);
    return 1;
  }

  printf("\ncharacter_load() succeeded: %s level %u, %d sacks\n",
         chr->character_name, chr->level, chr->num_inv_sacks);
  printf("  inv_block: [%zu..%zu)  equip_block: [%zu..%zu)\n\n",
         chr->inv_block_start, chr->inv_block_end,
         chr->equip_block_start, chr->equip_block_end);

  // Save to temp file
  const char *tmp_path = "/tmp/tq_chr_tool_roundtrip.chr";
  int ret = character_save(chr, tmp_path);
  if (ret != 0) {
    fprintf(stderr, "error: character_save() returned %d\n", ret);
    character_free(chr);
    free(orig_data);
    return 1;
  }

  printf("character_save() wrote %s\n\n", tmp_path);
  character_free(chr);
  free(orig_data);

  // Now compare original vs roundtripped using our independent parser
  printf("────────────────────────────────────────────────────────────\n\n");
  return cmd_compare(path, tmp_path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════════════

static void usage(const char *prog) {
  fprintf(stderr,
    "Usage: %s <command> [args...]\n"
    "\n"
    "Player.chr debugging/troubleshooting tool.\n"
    "Independent binary parser — does NOT reuse character_load() bugs.\n"
    "\n"
    "Commands:\n"
    "  dump      <chr>                      Raw key-value dump with offsets\n"
    "  inv       <chr>                      Inventory listing per sack\n"
    "  equip     <chr>                      Equipment listing (12 slots)\n"
    "  compare   <chr_a> <chr_b>            Structural diff\n"
    "  validate  <chr>                      Structural integrity checks\n"
    "  hex       <chr> <section|offset> [len]\n"
    "                                       Hex dump (sections: prefix,\n"
    "                                       inventory, middle, equipment,\n"
    "                                       suffix; or numeric offset)\n"
    "  roundtrip <chr>                      Load/save via character_load(),\n"
    "                                       compare output to input\n"
    "\n"
    "Examples:\n"
    "  %s dump testdata/Player.chr | head -50\n"
    "  %s inv Player_working.chr\n"
    "  %s compare Player_working.chr Player_broken.chr\n"
    "  %s validate Player_working.chr\n"
    "  %s hex Player_working.chr equipment\n"
    "  %s hex Player_working.chr 0x1a00 128\n"
    "  %s roundtrip Player_working.chr\n",
    prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  const char *cmd = argv[1];

  if (strcmp(cmd, "dump") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: %s dump <chr>\n", argv[0]);
      return 1;
    }
    return cmd_dump(argv[2]);
  }

  if (strcmp(cmd, "inv") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: %s inv <chr>\n", argv[0]);
      return 1;
    }
    return cmd_inv(argv[2]);
  }

  if (strcmp(cmd, "equip") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: %s equip <chr>\n", argv[0]);
      return 1;
    }
    return cmd_equip(argv[2]);
  }

  if (strcmp(cmd, "compare") == 0) {
    if (argc < 4) {
      fprintf(stderr, "Usage: %s compare <chr_a> <chr_b>\n", argv[0]);
      return 1;
    }
    return cmd_compare(argv[2], argv[3]);
  }

  if (strcmp(cmd, "validate") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: %s validate <chr>\n", argv[0]);
      return 1;
    }
    return cmd_validate(argv[2]);
  }

  if (strcmp(cmd, "hex") == 0) {
    if (argc < 4) {
      fprintf(stderr, "Usage: %s hex <chr> <section|offset> [len]\n", argv[0]);
      return 1;
    }
    return cmd_hex(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
  }

  if (strcmp(cmd, "roundtrip") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: %s roundtrip <chr>\n", argv[0]);
      return 1;
    }
    return cmd_roundtrip(argv[2]);
  }

  fprintf(stderr, "error: unknown command '%s'\n\n", cmd);
  usage(argv[0]);
  return 1;
}
