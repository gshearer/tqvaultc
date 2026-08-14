// Shared types and declarations for the tq-chr-tool modules: the raw .chr
// stream walker's entry/parse structures, and one header per command group.

#ifndef TQ_CHR_TOOL_H
#define TQ_CHR_TOOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <glib.h>
#include <sys/stat.h>
#include "../character.h"
#include "../config.h"  // extern bool tqvc_debug (defined in tq_chr_tool.c)

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

// -- tq_chr_parse.c: the raw stream walker -----------------------------------

// Reads a whole file into a malloc'd buffer; *out_size receives its size.
// Returns NULL (after printing why) on error.
uint8_t *load_file(const char *path, size_t *out_size);

uint32_t rd_u32(const uint8_t *data, size_t off);
float rd_float(const uint8_t *data, size_t off);

// Reads a length-prefixed string at off into out; returns the offset after it.
size_t rd_string(const uint8_t *data, size_t off, size_t file_size,
                 char *out, size_t out_size);

// As rd_string for a length-prefixed UTF-16LE (char_width 2) or UTF-32LE
// (char_width 4) string, transliterated to ASCII.
size_t rd_utf16(const uint8_t *data, size_t off, size_t file_size, int char_width,
                char *out, size_t out_size);

// Returns the known-key table entry for name, or NULL when the key is unknown.
const KnownKey *lookup_key(const char *name);

void entry_list_init(ChrEntryList *list);
void entry_list_push(ChrEntryList *list, const ChrEntry *e);
void entry_list_free(ChrEntryList *list);

// Walks the whole file into out.  Returns 0 on success, 1 on error.
int parse_entries(const uint8_t *data, size_t file_size, ChrEntryList *out);

// Builds the structured inventory/equipment view from a parsed entry list.
// out->data is borrowed from the caller's buffer, not owned.
void parse_chr_structured(const ChrEntryList *entries, RawChrParse *out);
void free_chr_parse(RawChrParse *p);

// -- tq_chr_report.c: read-only reports --------------------------------------

// Hex-dumps len bytes from start; used by cmd_hex and the compare report.
void hex_dump_range(const uint8_t *data, size_t start, size_t len);

// Returns a static name for equipment slot 0-11, or "?" when out of range.
const char *equip_slot_name(int slot);

// Returns a pointer into path at the last '\' or '/' component.
const char *basename_tail(const char *path);

int cmd_dump(const char *path);
int cmd_inv(const char *path);
int cmd_equip(const char *path);
int cmd_validate(const char *path);
int cmd_hex(const char *path, const char *section, const char *len_str);

// -- tq_chr_compare.c --------------------------------------------------------

int cmd_compare(const char *path_a, const char *path_b);

// -- tq_chr_edit.c: the commands that write ----------------------------------

int cmd_roundtrip(const char *path);
int cmd_add_skill(const char *path, const char *skill_path, uint32_t level);

#endif
