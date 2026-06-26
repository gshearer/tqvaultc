#ifndef ASSET_LOOKUP_H
#define ASSET_LOOKUP_H

#include "asset_index.h"
#include "arz.h"
#include "arc.h"
#include <stddef.h>
#include <stdbool.h>

// asset_lookup - find an asset by its path
// path: normalized game path to look up
// returns: pointer to the asset entry, or NULL if not found
const TQAssetEntry *asset_lookup(const char *path);

// asset_get_file_path - get the relative file path for a file_id
// file_id: index into the file table
// returns: file path string (internal pointer, do not free)
const char *asset_get_file_path(uint16_t file_id);

// asset_manager_init - initialize the asset manager with the game path
// game_path: root path to the game installation
void asset_manager_init(const char *game_path);

// asset_get_arz - get a cached TQArzFile for a given file_id
// file_id: index into the file table
// returns: cached ARZ file, or NULL if not an ARZ file
TQArzFile *asset_get_arz(uint16_t file_id);

// asset_get_database_arz - get the cached handle for Database/database.arz
// returns: cached database ARZ file, or NULL if not present in the index
TQArzFile *asset_get_database_arz(void);

// asset_get_arc - get a cached TQArcFile for a given file_id
// file_id: index into the file table
// returns: cached ARC file, or NULL if not an ARC file
TQArcFile *asset_get_arc(uint16_t file_id);

// asset_get_dbr - get a cached TQArzRecordData for a given record path
// record_path: normalized path to the DBR record
// returns: cached record data, or NULL if not found
TQArzRecordData *asset_get_dbr(const char *record_path);

// asset_get_num_files - get the total number of indexed game files
// returns: number of files in the index
int asset_get_num_files(void);

// asset_cache_insert - insert a pre-built record into the DBR cache
// key: malloc'd normalized path (ownership transferred to cache)
// data: record data (ownership transferred to cache)
void asset_cache_insert(char *key, TQArzRecordData *data);

// asset_dbr_cache_clear - drop all decompressed DBR records (repopulate lazily),
// bounding LIVE memory.  Does NOT return freed pages to the OS (cheap; for the
// frequent in-loop clears).  Callers must hold no TQArzRecordData pointer across
// the call.
void asset_dbr_cache_clear(void);

// asset_dbr_cache_clear_and_trim - asset_dbr_cache_clear() plus tq_heap_trim()
// to hand the freed pages back to the OS (drops RSS / Windows commit charge).
// The trim is slow (whole-heap walk), so use this only at phase boundaries.
void asset_dbr_cache_clear_and_trim(void);

// asset_set_cache_clear_hook - register a callback invoked at the start of
// asset_dbr_cache_clear() (before any record is freed).  The GUI passes
// prefetch_cancel() so a background prefetch is joined before its cache pointers
// are freed.  Pass NULL to clear.  Not thread-safe; set once at startup.
void asset_set_cache_clear_hook(void (*hook)(void));

// asset_manager_probe_ok - true iff the last asset_manager_init() resolved its
// probe asset.  False => wrong/empty game folder (data won't resolve); the
// startup path uses this to fall back to the folder picker.
bool asset_manager_probe_ok(void);

// asset_manager_free - free all cached resources
void asset_manager_free(void);

#endif
