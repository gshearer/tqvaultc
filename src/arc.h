#ifndef ARC_H
#define ARC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t file_offset;
    uint32_t compressed_size;
    uint32_t real_size;
} TQArcPart;

typedef struct {
    char *path;
    uint32_t real_size;
    uint32_t num_parts;
    uint32_t first_part_index;
} TQArcEntry;

typedef struct {
    char *filepath;
    uint32_t num_files;
    TQArcEntry *entries;
    uint32_t num_parts;
    TQArcPart *parts;
} TQArcFile;

TQArcFile* arc_load(const char *filepath);
void arc_free(TQArcFile *arc);

/**
 * arc_extract_file - Extract a file from the archive
 */
uint8_t* arc_extract_file(TQArcFile *arc, uint32_t entry_index, size_t *out_size);

/**
 * arc_extract_file_at - Extract a file from specific offset and size
 */
uint8_t* arc_extract_file_at(TQArcFile *arc, uint32_t offset, uint32_t compressed_size, uint32_t real_size, size_t *out_size);

#endif
