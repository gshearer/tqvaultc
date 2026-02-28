#ifndef ASSET_LOOKUP_H
#define ASSET_LOOKUP_H

#include "asset_index.h"
#include "arz.h"
#include "arc.h"
#include <stddef.h>

/**
 * asset_lookup - Find an asset by its path.
 */
const TQAssetEntry* asset_lookup(const char *path);

/**
 * asset_get_file_path - Get the relative file path for a file_id.
 */
const char* asset_get_file_path(uint16_t file_id);

/**
 * asset_manager_init - Initialize the asset manager with the game path.
 */
void asset_manager_init(const char *game_path);

/**
 * asset_get_arz - Get a cached TQArzFile for a given file_id.
 */
TQArzFile* asset_get_arz(uint16_t file_id);

/**
 * asset_get_arc - Get a cached TQArcFile for a given file_id.
 */
TQArcFile* asset_get_arc(uint16_t file_id);

/**
 * asset_get_dbr - Get a cached TQArzRecordData for a given record path.
 */
TQArzRecordData* asset_get_dbr(const char *record_path);

/**
 * asset_get_num_files - Get the total number of indexed game files.
 */
int asset_get_num_files(void);

/**
 * asset_cache_insert - Insert a pre-built record into the DBR cache.
 * Key is a malloc'd normalized path (ownership transferred to cache).
 * Record ownership is also transferred to cache.
 */
void asset_cache_insert(char *key, TQArzRecordData *data);

/**
 * asset_manager_free - Free all cached resources.
 */
void asset_manager_free(void);

#endif
