#ifndef ASSET_LOOKUP_H
#define ASSET_LOOKUP_H

#include "asset_index.h"
#include "arz.h"
#include "arc.h"
#include <stddef.h>

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

// asset_manager_free - free all cached resources
void asset_manager_free(void);

#endif
