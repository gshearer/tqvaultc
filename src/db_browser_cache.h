#ifndef DB_BROWSER_CACHE_H
#define DB_BROWSER_CACHE_H

// db_browser_cache -- on-disk cache of the Database Browser's built index.
//
// Building the browser index cold takes ~4s (decompressing all 74k database
// records to categorize them + resolving creature loot tables).  This module
// serializes the finished index (every category's DbBrowseItem list + the
// creature and quest indexes) to a small binary file in the per-user cache dir
// so subsequent sessions open the browser instantly.
//
// It mirrors the resource-index cache (asset_lookup.c): a magic + version
// header, validated against the live database.arz size+mtime so a game update
// (or a format-version bump) transparently forces a rebuild.  The reverse
// "dropped by" / "reward from" maps are NOT stored — they are rebuilt from the
// forward lists on load via db_{creature,quest}_index_finalize_reverse().
//
// Lives in the GUI executable only (it touches DbBrowseItem/GListStore).

#include <stdbool.h>
#include "ui_db_browser_internal.h"

// Bump on ANY change to the on-disk layout below OR to the game content it
// encodes (categorization rules, loot resolution, name resolution): a stale
// cache from an older binary must not be trusted.  (v6 added the game_folder
// to the header so a path correction rebuilds instead of reusing a stale cache.)
// (v7: granted pet-summon skills now render the pet's attributes + abilities in
// the card, so the search blob gained that text and must be rebuilt.)
#define DB_CACHE_VERSION 7

// True iff a cache file exists whose header (magic + version + database.arz
// size/mtime + game_folder) matches the current install — i.e. a load would
// succeed without a rebuild.  Used by the startup path to decide whether to
// show the "Setting up…" popup.
bool db_browser_cache_valid(const char *game_folder);

// Serialize a fully-built browser state (all cat_stores + st->creatures +
// st->quests) to the cache file (written atomically via a temp file + rename).
// Returns true on success.
bool db_browser_cache_save(const DbBrowserState *st, const char *game_folder);

// Populate a browser state from the cache file: appends DbBrowseItems into the
// (already-allocated, empty) cat_stores, sets cat_counts, and rebuilds
// st->creatures / st->quests with their reverse maps.  Returns true on a
// validated, complete load.  On any header mismatch or corruption it restores
// st to the empty state it received (so the caller can fall back to a live
// build) and returns false.
bool db_browser_cache_load(DbBrowserState *st, const char *game_folder);

// Headless round-trip self-test (tqvaultc --db-cache-selftest): build the index
// live, save it, reload it into a fresh state, print per-category + creature +
// quest counts for both, and report ROUNDTRIP OK/FAIL.  Returns a process exit
// code (0 == OK).  Assumes the asset manager + interns + item_stats + affix
// tables are already initialized by the caller.
int db_browser_cache_selftest(void);

// Headless keyword/content-search self-test (tqvaultc --db-search-selftest
// "<keywords>"): build the real full index live, generate every entry's search
// blob, apply the multi-token AND match across all categories, and print each
// hit ("CAT_LABEL  name") plus a per-category tally and grand total.  Exercises
// the actual blob + match path without the GUI.  Returns a process exit code.
// Assumes the asset manager + interns + item_stats + affix tables are already
// initialized by the caller.
int db_browser_search_selftest(const char *keywords);

// Headless display-sort self-test (tqvaultc --db-sort-selftest): build the real
// full index live, apply db_browser_sort_stores(), then assert that every
// non-skill category is alphabetical by name and that same-named difficulty
// variants run legendary->epic->normal.  Prints any ordering violations plus a
// few sample multi-tier runs, and returns a process exit code (0 == all pass).
int db_browser_sort_selftest(void);

// Headless affix drill-down self-test (tqvaultc --affix-items "<affix>"): build
// the real full index live, find the prefix/suffix whose name contains <affix>
// (case-insensitive), and for each gear type it lists print the concrete items
// of that type able to roll it -- exercising db_affix_items_for_type() without
// the GUI.  Returns a process exit code (0 == found).
int db_browser_affix_items_selftest(const char *affix_query);

#endif
