#include "arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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

TQArcFile* arc_load(const char *filepath) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    uint8_t *data = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    close(fd);

    ArcHeader *header = (ArcHeader*)data;
    if (memcmp(header->magic, "ARC\0", 4) != 0) {
        munmap(data, st.st_size);
        return NULL;
    }

    TQArcFile *arc = calloc(1, sizeof(TQArcFile));
    arc->filepath = strdup(filepath);
    arc->num_files = header->num_files;
    arc->entries = calloc(arc->num_files, sizeof(TQArcEntry));
    arc->num_parts = header->num_parts;
    arc->parts = calloc(arc->num_parts, sizeof(TQArcPart));
    
    // In mmap mode, we can point directly to parts if we want, 
    // but for compatibility with existing code we copy them or just point.
    // Let's copy for now to keep the struct unchanged.
    memcpy(arc->parts, data + header->toc_offset, arc->num_parts * sizeof(TQArcPart));

    size_t filenames_offset = header->toc_offset + arc->num_parts * sizeof(TQArcPart);

    // Read records from end
    ArcFileRecord *records = (ArcFileRecord*)(data + st.st_size - 44LL * header->num_files);

    // Read filenames sequentially
    uint8_t *name_ptr = data + filenames_offset;
    for (uint32_t i = 0; i < header->num_files; i++) {
        arc->entries[i].path = strdup((char*)name_ptr);
        name_ptr += strlen((char*)name_ptr) + 1;
        
        arc->entries[i].real_size = records[i].real_size;
        arc->entries[i].num_parts = records[i].num_parts;
        arc->entries[i].first_part_index = records[i].first_part_index;
    }

    // Store the mmap pointer in a private field if we want to use it for extraction
    // For now we'll just open the file again in extract, or better, keep the mmap.
    // Let's add raw_data and data_size to TQArcFile.
    // Wait, I need to update the struct in arc.h.
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
    // This is a simplified version for when we have the exact offset (e.g. from index)
    // ARC files can be multi-part, but many resources are single-part.
    // If the index only gives one offset/size, we assume it's the whole file or first part.
    
    int fd = open(arc->filepath, O_RDONLY);
    if (fd < 0) return NULL;
    
    uint8_t *comp_buf = malloc(compressed_size);
    lseek(fd, offset, SEEK_SET);
    read(fd, comp_buf, compressed_size);
    close(fd);
    
    uint8_t *data = decompress_part(comp_buf, compressed_size, real_size);
    free(comp_buf);
    
    if (data) *out_size = real_size;
    return data;
}

uint8_t* arc_extract_file(TQArcFile *arc, uint32_t entry_index, size_t *out_size) {
    if (!arc || entry_index >= arc->num_files) return NULL;

    TQArcEntry *entry = &arc->entries[entry_index];
    int fd = open(arc->filepath, O_RDONLY);
    if (fd < 0) return NULL;

    uint8_t *real_data = malloc(entry->real_size);
    size_t current_offset = 0;
    
    for (uint32_t p = 0; p < entry->num_parts; p++) {
        uint32_t part_idx = entry->first_part_index + p;
        if (part_idx >= arc->num_parts) break;

        TQArcPart *part = &arc->parts[part_idx];
        uint8_t *comp_buf = malloc(part->compressed_size);
        lseek(fd, part->file_offset, SEEK_SET);
        read(fd, comp_buf, part->compressed_size);
        
        uLongf dest_len = part->real_size;
        if (uncompress(real_data + current_offset, &dest_len, comp_buf, part->compressed_size) != Z_OK) {
            if (part->compressed_size == part->real_size) {
                memcpy(real_data + current_offset, comp_buf, part->real_size);
            }
        }
        current_offset += part->real_size;
        free(comp_buf);
    }

    close(fd);
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
    free(arc);
}
