#ifndef ASSET_INDEX_H
#define ASSET_INDEX_H

#include <stdint.h>

/**
 * entry_t - Represents a single asset (record or file) in the game archives.
 * Size: 16 bytes
 */
typedef struct {
    uint32_t hash;        // CRC32 of the normalized path
    uint16_t file_id;     // Index into the game_files array
    uint16_t flags;       // Reserved
    uint32_t offset;      // Offset in the source file
    uint32_t size;        // Compressed size
    uint32_t real_size;   // Uncompressed size
} TQAssetEntry;

/**
 * TQIndexHeader - Header for the binary resource index file.
 * Size: 32 bytes
 */
typedef struct {
    char magic[4];               // "TQVI"
    uint32_t version;            // Index version (1)
    uint32_t num_files;          // Number of ARC/ARZ containers
    uint32_t num_entries;        // Total number of assets
    uint32_t string_table_offset; // Offset to null-terminated string table
    uint32_t entries_offset;      // Offset to TQAssetEntry array
    uint32_t reserved[2];
} TQIndexHeader;

#endif
