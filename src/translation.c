#include "translation.h"
#include "arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TQTranslation* translation_init() {
    TQTranslation *t = g_malloc0(sizeof(TQTranslation));
    t->tags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return t;
}

void translation_free(TQTranslation *t) {
    if (!t) return;
    g_hash_table_destroy(t->tags);
    g_free(t);
}

/* Strip TQ format codes like {^L}, {^N}, ^L etc. in-place */
static void strip_tq_tags(char *str) {
    char *r = str, *w = str;
    while (*r) {
        if (r[0] == '{' && r[1] == '^' && r[2] && r[3] == '}') {
            r += 4; /* skip {^X} */
        } else if (r[0] == '^' && r[1] && ((r[1] >= 'A' && r[1] <= 'Z') || (r[1] >= 'a' && r[1] <= 'z'))) {
            r += 2; /* skip ^X */
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void parse_text_data(TQTranslation *t, uint8_t *data, size_t size) {
    if (!data || size == 0) return;

    char *content;
    bool utf16 = (size >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)));

    if (utf16) {
        const char *from_enc = (data[0] == 0xFF) ? "UTF-16LE" : "UTF-16BE";
        gsize bytes_written;
        content = g_convert((const gchar *)(data + 2), size - 2,
                            "UTF-8", from_enc, NULL, &bytes_written, NULL);
        if (!content) return;
    } else {
        /* Assume Windows-1252 (superset of Latin-1) if not valid UTF-8 */
        if (g_utf8_validate((const gchar *)data, size, NULL)) {
            content = malloc(size + 1);
            memcpy(content, data, size);
            content[size] = '\0';
        } else {
            gsize bytes_written;
            content = g_convert((const gchar *)data, size,
                                "UTF-8", "WINDOWS-1252", NULL, &bytes_written, NULL);
            if (!content) return;
        }
    }

    char *line = strtok(content, "\r\n");
    while (line) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *tag = g_ascii_strdown(line, -1);
            char *val = strdup(eq + 1);
            strip_tq_tags(val);
            g_hash_table_insert(t->tags, tag, val);
        }
        line = strtok(NULL, "\r\n");
    }
    g_free(content);
}

bool translation_load_from_arc(TQTranslation *t, const char *arc_path) {
    TQArcFile *arc = arc_load(arc_path);
    if (!arc) return false;

    for (uint32_t i = 0; i < arc->num_files; i++) {
        const char *ext = strrchr(arc->entries[i].path, '.');
        if (ext && strcasecmp(ext, ".txt") == 0) {
            size_t size;
            uint8_t *data = arc_extract_file(arc, i, &size);
            if (data) {
                parse_text_data(t, data, size);
                free(data);
            }
        }
    }

    arc_free(arc);
    return true;
}

const char* translation_get(TQTranslation *t, const char *tag) {
    if (!t || !tag) return NULL;
    char lower[256];
    size_t i;
    for (i = 0; tag[i] && i < 255; i++) {
        lower[i] = (tag[i] >= 'A' && tag[i] <= 'Z') ? (tag[i] + 32) : tag[i];
    }
    lower[i] = '\0';
    return g_hash_table_lookup(t->tags, lower);
}
