#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "arc.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <arc-file>\n"
        "\n"
        "Dump headers and DDS magic detection for the first 100 files in a\n"
        ".arc archive. Shows 64-byte hex headers and identifies DDS texture\n"
        "data offsets.\n"
        "\n"
        "Examples:\n"
        "  %s testdata/gamefiles/Resources/Items.arc\n",
        prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }
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
