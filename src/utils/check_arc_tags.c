/*
 * check_arc_tags.c — Load a .arc file, list all entries, and search
 * .txt files for "x4tag" (case-insensitive) to find XPack4 translation tags.
 *
 * Usage: check_arc_tags <path-to-arc-file> [additional-arc-files...]
 *
 * Compile from project root:
 *   gcc -o /tmp/check_arc_tags src/utils/check_arc_tags.c src/arc.c -I. -lz -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasestr, strncasecmp */
#include <ctype.h>
#include <stdint.h>
#include "src/arc.h"

/* Case-insensitive substring search within a buffer of known length */
static const char *ci_strstr(const char *haystack, size_t haystack_len,
                             const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    if (haystack_len < nlen) return NULL;

    for (size_t i = 0; i <= haystack_len - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0)
            return haystack + i;
    }
    return NULL;
}

/* Check if a string ends with a suffix (case-insensitive) */
static int ends_with_ci(const char *str, const char *suffix)
{
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (slen < xlen) return 0;
    return strcasecmp(str + slen - xlen, suffix) == 0;
}

static void process_arc(const char *path)
{
    printf("========================================\n");
    printf("Loading: %s\n", path);
    printf("========================================\n");

    TQArcFile *arc = arc_load(path);
    if (!arc) {
        fprintf(stderr, "ERROR: Failed to load arc file: %s\n", path);
        return;
    }

    printf("  Total entries: %u\n", arc->num_files);
    printf("  Total parts:   %u\n\n", arc->num_parts);

    /* List all files */
    printf("--- All entries ---\n");
    for (uint32_t i = 0; i < arc->num_files; i++) {
        printf("  [%3u] %-60s  (size: %u bytes)\n",
               i, arc->entries[i].path, arc->entries[i].real_size);
    }
    printf("\n");

    /* Search .txt files for x4tag */
    int txt_count = 0;
    int x4_hit_count = 0;
    int x4_miss_count = 0;

    printf("--- Searching .txt files for 'x4tag' (case-insensitive) ---\n\n");

    for (uint32_t i = 0; i < arc->num_files; i++) {
        const char *entry_path = arc->entries[i].path;

        if (!ends_with_ci(entry_path, ".txt"))
            continue;

        txt_count++;
        size_t out_size = 0;
        uint8_t *data = arc_extract_file(arc, i, &out_size);
        if (!data) {
            printf("  [%3u] %-50s  EXTRACT FAILED\n", i, entry_path);
            continue;
        }

        /* Search for x4tag in the extracted content */
        const char *match = ci_strstr((const char *)data, out_size, "x4tag");
        if (match) {
            x4_hit_count++;
            printf("  HIT   [%3u] %s  (%zu bytes)\n", i, entry_path, out_size);

            /* Print up to 10 matching lines for context */
            int shown = 0;
            const char *p = (const char *)data;
            const char *end = p + out_size;
            while (p < end && shown < 10) {
                size_t remaining = (size_t)(end - p);
                const char *hit = ci_strstr(p, remaining, "x4tag");
                if (!hit) break;

                /* Back up to start of line */
                const char *line_start = hit;
                while (line_start > (const char *)data && *(line_start - 1) != '\n')
                    line_start--;

                /* Find end of line */
                const char *line_end = hit;
                while (line_end < end && *line_end != '\n' && *line_end != '\r')
                    line_end++;

                /* Print the line (truncate if too long) */
                int line_len = (int)(line_end - line_start);
                if (line_len > 200) line_len = 200;
                printf("         > %.*s\n", line_len, line_start);

                shown++;
                p = line_end + 1;
            }
            if (shown == 0) {
                /* Content might be UTF-16; show offset */
                printf("         (match at offset %zu, content may be UTF-16)\n",
                       (size_t)(match - (const char *)data));
            }
        } else {
            x4_miss_count++;
            printf("  MISS  [%3u] %s  (%zu bytes)\n", i, entry_path, out_size);
        }

        free(data);
    }

    printf("\n--- Summary for %s ---\n", path);
    printf("  Total .txt files: %d\n", txt_count);
    printf("  With x4tag:       %d\n", x4_hit_count);
    printf("  Without x4tag:    %d\n", x4_miss_count);
    printf("\n");

    /* Also search non-.txt files for x4tag */
    int other_hits = 0;
    for (uint32_t i = 0; i < arc->num_files; i++) {
        if (ends_with_ci(arc->entries[i].path, ".txt"))
            continue;

        size_t out_size = 0;
        uint8_t *data = arc_extract_file(arc, i, &out_size);
        if (!data) continue;

        if (ci_strstr((const char *)data, out_size, "x4tag")) {
            if (other_hits == 0)
                printf("--- x4tag found in non-.txt files ---\n");
            printf("  HIT   [%3u] %s  (%zu bytes)\n",
                   i, arc->entries[i].path, out_size);
            other_hits++;
        }
        free(data);
    }
    if (other_hits == 0)
        printf("  (No x4tag matches in non-.txt files)\n");
    printf("\n");

    arc_free(arc);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <arc-file> [arc-file ...]\n", argv[0]);
        fprintf(stderr, "\nSearches .arc files for XPack4 translation tags (x4tag).\n");
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s testdata/Text_EN.arc\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        process_arc(argv[i]);
    }

    return 0;
}
