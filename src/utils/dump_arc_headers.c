#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arc.h"

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    TQArcFile *arc = arc_load(argv[1]);
    if (!arc) return 1;

    for (uint32_t i = 0; i < arc->num_files; i++) {
        size_t size;
        uint8_t *data = arc_extract_file(arc, i, &size);
        if (data) {
            printf("File %u: %s (%zu bytes)\n", i, arc->entries[i].path, size);
            printf("  Header: ");
            for (int j = 0; j < 64 && j < (int)size; j++) {
                printf("%02X ", data[j]);
            }
            printf("\n");
            
            // Check for DDS magic
            for (int j = 0; j < 64 && j < (int)size - 4; j++) {
                if (memcmp(data + j, "DDS ", 4) == 0 || memcmp(data + j, "DDSR", 4) == 0) {
                    printf("  DDS magic at offset %d\n", j);
                }
            }
            free(data);
        }
        if (i > 100) break; // Just check first 100
    }
    arc_free(arc);
    return 0;
}
