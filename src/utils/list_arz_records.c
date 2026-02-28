#include "../arz.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <database.arz>\n", argv[0]);
        return 1;
    }

    TQArzFile *arz = arz_load(argv[1]);
    if (!arz) {
        printf("Failed to load ARZ: %s\n", argv[1]);
        return 1;
    }

    for (uint32_t i = 0; i < arz->num_records; i++) {
        if (arz->records[i].path) {
            printf("%s\n", arz->records[i].path);
        }
    }

    arz_free(arz);
    return 0;
}
