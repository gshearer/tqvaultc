#ifndef DB_LOOT_H
#define DB_LOOT_H

// db_loot -- shared, GTK-free loot-table resolver.
//
// A faithful C port of tqdb's loot.py (reference/tqdb/tqdb/parsers/loot.py):
// it recursively follows a Titan Quest loot-table DBR through the five table
// template classes -- LootMasterTable, LootItemTable_FixedWeight,
// LootItemTable_DynWeight, FixedItemContainer and FixedItemLoot -- multiplying
// the per-entry weights/chances down the chain, and flattens the result into a
// map of droppable item record paths -> accumulated chance.
//
// It depends only on arz.c + glib (no GTK/translation), so it is shared by both
// the GUI Database Browser (ui_db_browser.c) and the headless tq-dbr-tool.

#include <glib.h>
#include <limits.h>
#include <stdbool.h>
#include "arz.h"

// Sentinel level meaning "no level reference available" (e.g. quest rewards,
// which tqdb parses without a parentLevel).  DynWeight tables, whose item-level
// window needs a level, then resolve to nothing -- matching tqdb's behavior
// where the missing parentLevel raises and the table is skipped.
#define DB_LOOT_NO_LEVEL INT_MIN

// A resolve context carries a cache of decompressed records so that resolving
// many tables (e.g. every creature's loot) doesn't re-inflate the same shared
// loot tables and item DBRs thousands of times.  Batch callers should create one
// context and reuse it across all db_loot_resolve_ctx() calls.
typedef struct DbLootCtx DbLootCtx;

DbLootCtx *db_loot_ctx_new(TQArzFile *arz);
void db_loot_ctx_free(DbLootCtx *ctx);

// db_loot_ctx_clear_cache -- drop the context's cached records (they re-inflate
// lazily).  Call only between top-level resolves: no borrowed ctx_read() pointer
// may be live across it.  Bounds memory during the first-run creature-loot pass.
void db_loot_ctx_clear_cache(DbLootCtx *ctx);

// db_loot_resolve_ctx -- like db_loot_resolve but reusing a context's cache.
GHashTable *db_loot_resolve_ctx(DbLootCtx *ctx, const char *table_path, int level);

// db_loot_resolve -- flatten a loot-table DBR into an item -> chance map.
//
//   arz        -- loaded database (records read on demand)
//   table_path -- the loot-table DBR record path (any case/slash style)
//   level      -- the dropping context's level for this difficulty; used as the
//                 'parentLevel'/'averagePlayerLevel' in the level/spawn
//                 equations (matters for DynWeight item-level windows)
//
// Returns a newly allocated GHashTable<char* item_path_lc, double* chance>
// (free with g_hash_table_destroy), or NULL if table_path is not one of the
// recognized loot-table classes.  Item paths are lowercased, backslash-style.
// This one-shot form builds and frees a private context internally.
GHashTable *db_loot_resolve(TQArzFile *arz, const char *table_path, int level);

// db_loot_eval -- evaluate a TQ loot equation string (e.g. "parentLevel + 2",
// "(3+(1.6*numberOfPlayers))*0.9").  Supports + - * /, parentheses, unary minus
// and the variables parentLevel / averagePlayerLevel (both = level) and
// numberOfPlayers (= 1).  Unknown identifiers evaluate to 0.
double db_loot_eval(const char *expr, double level);

#endif
