#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

/**
 * tq_stats.c - Titan Quest Character Parser
 * 
 * This program reads a .chr file and attempts to extract
 * all length-prefixed strings and their associated values.
 */

void parse_character(const char *filepath) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("Error opening file");
        return;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size == 0) {
        printf("File is empty: %s\n", filepath);
        fclose(file);
        return;
    }

    uint8_t *data = malloc(size);
    if (!data) {
        fprintf(stderr, "Out of memory\n");
        fclose(file);
        return;
    }

    if (fread(data, 1, size, file) != (size_t)size) {
        perror("Error reading file");
        free(data);
        fclose(file);
        return;
    }
    fclose(file);

    printf("--- Titan Quest Character Report ---\n");
    printf("File: %s\n", filepath);
    printf("Size: %ld bytes\n\n", size);

    long offset = 0;
    while (offset + 4 <= size) {
        // Read 4-byte length
        uint32_t len;
        memcpy(&len, data + offset, 4);
        
        // TQ strings are usually short tags (e.g., "begin_block", "playerStats").
        // We look for lengths between 1 and 255 and check for printable characters.
        if (len > 0 && len < 256 && offset + 4 + len <= size) {
            int printable = 1;
            for (uint32_t i = 0; i < len; i++) {
                if (!isprint(data[offset + 4 + i])) {
                    printable = 0;
                    break;
                }
            }

            if (printable) {
                char *key = malloc(len + 1);
                memcpy(key, data + offset + 4, len);
                key[len] = '\0';
                
                offset += 4 + len;

                // Print key
                printf("%-30s: ", key);

                // Try to read the following value.
                // In TQ files, values can be 4-byte integers/floats or another string.
                if (offset + 4 <= size) {
                    uint32_t val;
                    memcpy(&val, data + offset, 4);
                    
                    // Check if the value is actually another length-prefixed string
                    if (val > 0 && val < 512 && offset + 4 + val <= size) {
                        int val_printable = 1;
                        for (uint32_t i = 0; i < val; i++) {
                            if (!isprint(data[offset + 4 + i])) {
                                val_printable = 0;
                                break;
                            }
                        }
                        
                        if (val_printable) {
                            char *val_str = malloc(val + 1);
                            memcpy(val_str, data + offset + 4, val);
                            val_str[val] = '\0';
                            printf("\"%s\"\n", val_str);
                            free(val_str);
                            offset += 4 + val;
                        } else {
                            // Assume it's a number (int or float)
                            printf("%u (0x%08X)\n", val, val);
                            offset += 4;
                        }
                    } else {
                        // Assume it's a number
                        printf("%u (0x%08X)\n", val, val);
                        offset += 4;
                    }
                } else {
                    printf("[End of File]\n");
                }
                free(key);
                continue;
            }
        }
        offset++; // Move by 1 byte to find the next possible string
    }

    free(data);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path_to_player.chr>\n", argv[0]);
        return 1;
    }

    parse_character(argv[1]);
    return 0;
}
