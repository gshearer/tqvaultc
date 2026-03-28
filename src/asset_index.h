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

// TQIndexHeader - header for the binary resource index file
// size: 32 bytes
typedef struct {
  char magic[4];               // "TQVI"
  uint32_t version;            // index version (1)
  uint32_t num_files;          // number of ARC/ARZ containers
  uint32_t num_entries;        // total number of assets
  uint32_t string_table_offset; // offset to null-terminated string table
  uint32_t entries_offset;      // offset to TQAssetEntry array
  uint32_t reserved[2];
} TQIndexHeader;

#pragma pack(pop)

#endif
