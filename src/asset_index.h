#ifndef ASSET_INDEX_H
#define ASSET_INDEX_H

#include <stdint.h>

#pragma pack(push, 1)

// TQAssetEntry - represents a single asset (record or file) in the game archives
// size: 16 bytes
typedef struct {
  uint32_t hash;      // CRC32 of the normalized path
  uint16_t file_id;   // index into the game_files array
  uint16_t flags;     // reserved
  uint32_t offset;    // offset in the source file
  uint32_t size;      // compressed size
  uint32_t real_size; // uncompressed size
} TQAssetEntry;

// Bump on any index format/content change.  v2 added game_folder so a stale
// index built for a different install is rebuilt instead of silently reused.
#define TQ_INDEX_VERSION 2

// Max length (incl. NUL) of the game-folder path stamped into the index header.
#define TQ_INDEX_GAMEDIR_MAX 1024

// TQIndexHeader - header for the binary resource index file
typedef struct {
  char magic[4];               // "TQVI"
  uint32_t version;            // index version (TQ_INDEX_VERSION)
  uint32_t num_files;          // number of ARC/ARZ containers
  uint32_t num_entries;        // total number of assets
  uint32_t string_table_offset; // offset to null-terminated string table
  uint32_t entries_offset;      // offset to TQAssetEntry array
  uint32_t reserved[2];
  // The game folder this index was built for.  asset_index_load rejects an
  // index whose game_folder differs from the current one, so correcting the
  // path in Settings rebuilds the index instead of reusing a stale/wrong one.
  char game_folder[TQ_INDEX_GAMEDIR_MAX];
} TQIndexHeader;

#pragma pack(pop)

#endif
