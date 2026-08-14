// Shared helpers for the tq-quest-tool command modules, and one section per
// command group.

#ifndef TQ_QUEST_TOOL_H
#define TQ_QUEST_TOOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <glib.h>
#include <sys/stat.h>
#include "../quest_tokens.h"

// Case-insensitive substring search.
bool ci_contains(const char *haystack, const char *needle);

// qsort comparator over char * elements.
int cmp_str(const void *a, const void *b);

// Best-effort act for a token, from its name prefix.
QuestAct guess_act(const char *token);

// Reads size bytes from an already-open stream into a malloc'd buffer.
// Returns NULL on a short read or allocation failure.
uint8_t *slurp_stream(FILE *f, long size);

// Returns the quest_defs[] entry with this name, or NULL.
const QuestDef *find_quest_by_name(const char *name);

// -- tq_quest_token.c: QuestToken.myw commands -------------------------------

int cmd_dump(const char *path);
int cmd_count(const char *path);
int cmd_search(const char *path, const char *pattern);
int cmd_has(const char *path, const char *token);
int cmd_acts(const char *path);
int cmd_quests(const char *path);
int cmd_add(const char *path, const char *token);
int cmd_remove(const char *path, const char *token);
int cmd_complete(const char *path, const char *quest_name);
int cmd_clear(const char *path, const char *quest_name);
int cmd_roundtrip(const char *path);
int cmd_defs(void);
int cmd_diff(const char *path_a, const char *path_b);
int cmd_coverage(const char *path);

// -- tq_quest_que.c: .que and Quest.myw commands -----------------------------

// Reads a whole file into a malloc'd buffer; *out_size receives its size.
// Returns NULL (after printing why) on error.
uint8_t *read_file(const char *path, long *out_size);

// Reads the length-prefixed key at *off, advancing it past the key.
// Returns a newly allocated string (free with free), or NULL when malformed.
char *que_read_key(const uint8_t *data, size_t len, size_t *off);

// Reads a little-endian u32 at *off and advances it 4 bytes.
uint32_t que_read_u32(const uint8_t *data, size_t *off);

// Reads the next key and checks it equals expected, advancing *off past it.
bool qmyw_expect_key(const uint8_t *data, size_t len, size_t *off,
                     const char *expected);

int cmd_dump_que(const char *path);
int cmd_dump_quest_myw(const char *path);
int cmd_clear_que(const char *dir);
int cmd_compare_que(const char *dir_a, const char *dir_b);
int cmd_que_info(const char *dir);

// -- tq_quest_scan.c ---------------------------------------------------------

int cmd_scan(const char *save_dir);

#endif
