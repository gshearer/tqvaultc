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
  uint8_t *raw_data;  // mmap'd file data (or NULL)
  size_t data_size;   // size of mmap'd region
} TQArcFile;

// arc_load - load and parse an ARC archive file
// filepath: path to the .arc file
// returns: parsed archive, or NULL on failure
TQArcFile *arc_load(const char *filepath);

// arc_free - free all resources associated with an ARC archive
// arc: archive to free
void arc_free(TQArcFile *arc);

// arc_extract_file - extract a file from the archive by entry index
// arc: the archive file
// entry_index: index into arc->entries
// out_size: on success, set to the extracted data size
// returns: malloc'd buffer with extracted data, or NULL on failure
uint8_t *arc_extract_file(TQArcFile *arc, uint32_t entry_index, size_t *out_size);

// arc_extract_file_at - extract a file from a specific offset and size
// arc: the archive file
// offset: byte offset into the archive
// compressed_size: compressed data size
// real_size: uncompressed data size
// out_size: on success, set to the extracted data size
// returns: malloc'd buffer with extracted data, or NULL on failure
uint8_t *arc_extract_file_at(TQArcFile *arc, uint32_t offset,
                             uint32_t compressed_size, uint32_t real_size,
                             size_t *out_size);

#endif
