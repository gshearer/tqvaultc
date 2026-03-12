#include "arc.h"
#include "platform_mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#pragma pack(push, 1)

typedef struct {
    uint8_t magic[4];
    uint32_t version;
    uint32_t num_files;
    uint32_t num_parts;
    uint32_t unknown1;
    uint32_t unknown2;
    uint32_t toc_offset;
} ArcHeader;

typedef struct {
    uint32_t storage_type;
    uint32_t file_offset;
    uint32_t compressed_size;
    uint32_t real_size;
    uint32_t unknown[3];
    uint32_t num_parts;
    uint32_t first_part_index;
    uint32_t unknown2[2];
} ArcFileRecord;

#pragma pack(pop)

TQArcFile* arc_load(const char *filepath) {
    size_t file_size = 0;
    uint8_t *data = platform_mmap_readonly(filepath, &file_size);
    if (!data) return NULL;

    ArcHeader *header = (ArcHeader*)data;
    if (memcmp(header->magic, "ARC\0", 4) != 0) {
        platform_munmap(data, file_size);
        return NULL;
    }

    TQArcFile *arc = calloc(1, sizeof(TQArcFile));
    arc->filepath = strdup(filepath);
    arc->raw_data = data;
    arc->data_size = file_size;
    arc->num_files = header->num_files;
    arc->entries = calloc(arc->num_files, sizeof(TQArcEntry));
    arc->num_parts = header->num_parts;
    arc->parts = calloc(arc->num_parts, sizeof(TQArcPart));

    memcpy(arc->parts, data + header->toc_offset, arc->num_parts * sizeof(TQArcPart));

    size_t filenames_offset = header->toc_offset + arc->num_parts * sizeof(TQArcPart);

    // Read records from end
    ArcFileRecord *records = (ArcFileRecord*)(data + file_size - 44LL * header->num_files);

    // Read filenames sequentially
    uint8_t *name_ptr = data + filenames_offset;
    for (uint32_t i = 0; i < header->num_files; i++) {
        arc->entries[i].path = strdup((char*)name_ptr);
        name_ptr += strlen((char*)name_ptr) + 1;

        arc->entries[i].real_size = records[i].real_size;
        arc->entries[i].num_parts = records[i].num_parts;
        arc->entries[i].first_part_index = records[i].first_part_index;
    }

    return arc;
}

// Internal helper for extraction
static uint8_t* decompress_part(const uint8_t *src, uint32_t comp_size, uint32_t real_size) {
    uint8_t *dest = malloc(real_size);
    uLongf dest_len = real_size;
    if (uncompress(dest, &dest_len, src, comp_size) != Z_OK) {
        if (comp_size == real_size) {
            memcpy(dest, src, real_size);
        } else {
            free(dest);
            return NULL;
        }
    }
    return dest;
}

uint8_t* arc_extract_file_at(TQArcFile *arc, uint32_t offset, uint32_t compressed_size, uint32_t real_size, size_t *out_size) {
    if (!arc || !arc->raw_data) return NULL;
    if ((size_t)offset + compressed_size > arc->data_size) return NULL;

    uint8_t *data = decompress_part(arc->raw_data + offset, compressed_size, real_size);
    if (data) *out_size = real_size;
    return data;
}

uint8_t* arc_extract_file(TQArcFile *arc, uint32_t entry_index, size_t *out_size) {
    if (!arc || !arc->raw_data || entry_index >= arc->num_files) return NULL;

    TQArcEntry *entry = &arc->entries[entry_index];

    uint8_t *real_data = malloc(entry->real_size);
    size_t current_offset = 0;

    for (uint32_t p = 0; p < entry->num_parts; p++) {
        uint32_t part_idx = entry->first_part_index + p;
        if (part_idx >= arc->num_parts) break;

        TQArcPart *part = &arc->parts[part_idx];
        if ((size_t)part->file_offset + part->compressed_size > arc->data_size) break;

        uLongf dest_len = part->real_size;
        if (uncompress(real_data + current_offset, &dest_len,
                       arc->raw_data + part->file_offset, part->compressed_size) != Z_OK) {
            if (part->compressed_size == part->real_size) {
                memcpy(real_data + current_offset,
                       arc->raw_data + part->file_offset, part->real_size);
            }
        }
        current_offset += part->real_size;
    }

    *out_size = entry->real_size;
    return real_data;
}

void arc_free(TQArcFile *arc) {
    if (!arc) return;
    for (uint32_t i = 0; i < arc->num_files; i++) {
        free(arc->entries[i].path);
    }
    free(arc->entries);
    free(arc->parts);
    free(arc->filepath);
    if (arc->raw_data) platform_munmap(arc->raw_data, arc->data_size);
    free(arc);
}
