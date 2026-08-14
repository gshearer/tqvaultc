// Shared declarations for the tq-dbr-tool command modules.  Each cmd_* is one
// subcommand and returns 0 on success, 1 on failure -- the process exit code.

#ifndef TQ_DBR_TOOL_H
#define TQ_DBR_TOOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <math.h>
#include <glib.h>
#include "../compat.h"  // portable strcasestr (mingw)
#include "../arz.h"

// Lowercases and converts '/' to '\' (arz paths use backslashes).
// Returns a newly allocated string; free with g_free.
char *normalize_path(const char *input);

// Prints one variable's name and all its values to stdout.
void print_variable(TQVariable *v);

// tq_dbr_record.c -- generic ARZ record queries.
int cmd_dump(const char *arz_path, const char *record_path);
int cmd_search(const char *arz_path, const char *pattern);
int cmd_fields(const char *arz_path, const char *pattern, const char *field_list);
int cmd_stats(const char *arz_path, const char *pattern);
int cmd_coverage(const char *arz_path, const char *path_substr);

// tq_dbr_arc.c -- .arc archive access and the mesh-render harness.
int cmd_arctxt(const char *arc_path, const char *search_term);
int cmd_arcls(const char *arc_path);
int cmd_archex(const char *arc_path, const char *file_pattern);
int cmd_arcextract(const char *arc_path, const char *file_pattern, const char *out_path);
int cmd_meshrender(int argc, char **argv);

// tq_dbr_item.c -- item-facing reports.
int cmd_bonus(const char *arz_path, const char *item_path);
int cmd_categories(const char *arz_path);
int cmd_sets(const char *arz_path);

// tq_dbr_affix.c / tq_dbr_skill.c -- Database Browser affix and skill buckets.
int cmd_affixes(const char *arz_path);
int cmd_skills(const char *arz_path);

// tq_dbr_world.c -- loot tables, creatures and quests.
int cmd_loot(const char *arz_path, const char *table, int level);
int cmd_creatures(const char *arz_path);
int cmd_droppedby(const char *arz_path, const char *item);
int cmd_quests(const char *arz_path, const char *res_dir);

#endif
