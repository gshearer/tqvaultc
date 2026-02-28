#include "arz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ── string intern table ─────────────────────────────────────────── */

static GHashTable *g_intern_table = NULL; /* lowercase string -> canonical pointer */

void arz_intern_init(void) {
    if (g_intern_table) return;
    g_intern_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

const char* arz_intern(const char *name) {
    if (!name) return NULL;
    if (!g_intern_table) arz_intern_init();

    /* Lowercase the name for canonical form */
    size_t len = strlen(name);
    char *lower = malloc(len + 1);
    for (size_t i = 0; i <= len; i++)
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];

    const char *existing = g_hash_table_lookup(g_intern_table, lower);
    if (existing) {
        free(lower);
        return existing;
    }

    /* lower becomes the canonical copy owned by the hash table */
    g_hash_table_insert(g_intern_table, lower, lower);
    return lower;
}

void arz_intern_free(void) {
    if (g_intern_table) {
        g_hash_table_destroy(g_intern_table);
        g_intern_table = NULL;
    }
}

/* ── var_index building ──────────────────────────────────────────── */

void arz_record_build_var_index(TQArzRecordData *data) {
    if (!data || data->var_index) return;
    data->var_index = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (uint32_t i = 0; i < data->num_vars; i++) {
        if (!data->vars[i].name) continue;
        const char *interned = arz_intern(data->vars[i].name);
        /* Only insert the first occurrence (matches original linear scan behavior) */
        if (!g_hash_table_contains(data->var_index, interned))
            g_hash_table_insert(data->var_index, (gpointer)interned, &data->vars[i]);
    }
}

TQVariable* arz_record_get_var(TQArzRecordData *data, const char *interned_name) {
    if (!data || !interned_name) return NULL;
    if (!data->var_index) return NULL;
    return g_hash_table_lookup(data->var_index, interned_name);
}

/* ── low-level readers ───────────────────────────────────────────── */

static uint32_t read_u32(const uint8_t *data, size_t offset) {
    uint32_t val;
    memcpy(&val, data + offset, 4);
    return val;
}

static uint16_t read_u16(const uint8_t *data, size_t offset) {
    uint16_t val;
    memcpy(&val, data + offset, 2);
    return val;
}

static char* read_string_prefixed(const uint8_t *data, size_t offset, size_t *next_offset) {
    uint32_t len = read_u32(data, offset);
    char *str = malloc(len + 1);
    memcpy(str, data + offset + 4, len);
    str[len] = '\0';
    if (next_offset) *next_offset = offset + 4 + len;
    return str;
}

TQArzFile* arz_load(const char *filepath) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }

    uint8_t *data = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return NULL; }
    close(fd);

    uint32_t magic = read_u32(data, 0);
    if (magic != 0x0052415a && magic != 0x00030004) {
        munmap(data, st.st_size);
        return NULL;
    }

    TQArzFile *arz = calloc(1, sizeof(TQArzFile));
    arz->filepath = strdup(filepath);
    arz->raw_data = data;
    arz->data_size = st.st_size;

    uint32_t record_start = read_u32(data, 4);
    uint32_t record_count = read_u32(data, 12);
    uint32_t string_start = read_u32(data, 16);

    arz->num_strings = read_u32(data, string_start);
    arz->string_table = calloc(arz->num_strings, sizeof(char*));
    size_t s_off = string_start + 4;
    for (uint32_t i = 0; i < arz->num_strings; i++) {
        arz->string_table[i] = read_string_prefixed(data, s_off, &s_off);
    }

    arz->num_records = record_count;
    arz->records = calloc(record_count, sizeof(TQArzRecord));
    size_t r_off = record_start;
    for (uint32_t i = 0; i < record_count; i++) {
        uint32_t name_idx = read_u32(data, r_off);
        uint32_t type_len = read_u32(data, r_off + 4);
        r_off += 8 + type_len;

        arz->records[i].path = (name_idx < arz->num_strings) ? arz->string_table[name_idx] : NULL;
        arz->records[i].offset = read_u32(data, r_off) + 24;
        arz->records[i].compressed_size = read_u32(data, r_off + 4);
        r_off += 16;
    }

    /* Pre-intern all string table entries so var_index lookups work */
    for (uint32_t i = 0; i < arz->num_strings; i++) {
        if (arz->string_table[i])
            arz_intern(arz->string_table[i]);
    }

    return arz;
}

TQArzRecordData* arz_read_record_at(TQArzFile *arz, uint32_t offset, uint32_t compressed_size) {
    if (!arz || offset + compressed_size > arz->data_size) return NULL;

    uLong uncompressed_size = 1024 * 1024; // 1MB initial
    uint8_t *uncompressed = malloc(uncompressed_size);
    if (!uncompressed) return NULL;

    int res = uncompress(uncompressed, &uncompressed_size, arz->raw_data + offset, compressed_size);
    if (res == Z_BUF_ERROR) {
        uncompressed_size = 4 * 1024 * 1024; // 4MB
        uint8_t *new_buf = realloc(uncompressed, uncompressed_size);
        if (!new_buf) { free(uncompressed); return NULL; }
        uncompressed = new_buf;
        res = uncompress(uncompressed, &uncompressed_size, arz->raw_data + offset, compressed_size);
    }

    if (res != Z_OK) {
        free(uncompressed); return NULL;
    }

    uint32_t num_vars = 0;
    size_t off = 0;
    size_t data_pool_size = 0;
    while (off + 8 <= (size_t)uncompressed_size) {
        uint16_t count = read_u16(uncompressed, off + 2);
        data_pool_size += count * 8;
        off += 8 + 4 * (size_t)count;
        num_vars++;
    }

    TQArzRecordData *data = calloc(1, sizeof(TQArzRecordData));
    data->num_vars = num_vars;
    data->vars = calloc(num_vars, sizeof(TQVariable));
    data->buffer_to_free = uncompressed;
    data->pool_to_free = malloc(data_pool_size);
    data->var_index = NULL;

    size_t pool_off = 0;
    off = 0;
    for (uint32_t i = 0; i < num_vars; i++) {
        uint16_t type = read_u16(uncompressed, off);
        uint16_t count = read_u16(uncompressed, off + 2);
        uint32_t key_idx = read_u32(uncompressed, off + 4);
        off += 8;

        data->vars[i].name = (key_idx < arz->num_strings) ? arz->string_table[key_idx] : NULL;
        data->vars[i].count = count;

        if (type == 0 || type == 1) {
            data->vars[i].type = (type == 0) ? TQ_VAR_INT : TQ_VAR_FLOAT;
            data->vars[i].value.i32 = (int32_t*)(data->pool_to_free + pool_off);
            memcpy(data->vars[i].value.i32, uncompressed + off, (size_t)count * 4);
            pool_off += (size_t)count * 4;
        } else if (type == 2) {
            data->vars[i].type = TQ_VAR_STRING;
            data->vars[i].value.str = (const char**)(data->pool_to_free + pool_off);
            for (uint32_t j = 0; j < count; j++) {
                uint32_t val_idx = read_u32(uncompressed, off + 4 * j);
                data->vars[i].value.str[j] = (val_idx < arz->num_strings) ? arz->string_table[val_idx] : NULL;
            }
            pool_off += (size_t)count * sizeof(char*);
        }
        off += 4 * (size_t)count;
    }

    /* Build var_index for O(1) lookups */
    arz_record_build_var_index(data);

    return data;
}

TQArzRecordData* arz_read_record(TQArzFile *arz, const char *record_path) {
    if (!arz || !record_path) return NULL;
    char normalized_path[1024];
    strncpy(normalized_path, record_path, sizeof(normalized_path));
    normalized_path[sizeof(normalized_path)-1] = '\0';
    for (int i = 0; normalized_path[i]; i++) if (normalized_path[i] == '/') normalized_path[i] = '\\';

    for (uint32_t i = 0; i < arz->num_records; i++) {
        if (arz->records[i].path && strcasecmp(arz->records[i].path, normalized_path) == 0) {
            return arz_read_record_at(arz, arz->records[i].offset, arz->records[i].compressed_size);
        }
    }
    return NULL;
}

char* arz_record_get_string(TQArzRecordData *data, const char *var_name, bool *found) {
    if (found) *found = false;
    if (!data || !var_name) return NULL;

    /* Try fast path via var_index with interned name */
    if (data->var_index) {
        const char *interned = arz_intern(var_name);
        TQVariable *v = g_hash_table_lookup(data->var_index, interned);
        if (v && v->type == TQ_VAR_STRING && v->count > 0) {
            if (found) *found = true;
            return v->value.str[0] ? strdup(v->value.str[0]) : NULL;
        }
        return NULL;
    }

    /* Fallback: linear scan */
    for (uint32_t i = 0; i < data->num_vars; i++) {
        if (data->vars[i].name && strcasecmp(data->vars[i].name, var_name) == 0) {
            if (data->vars[i].type == TQ_VAR_STRING && data->vars[i].count > 0) {
                if (found) *found = true;
                return data->vars[i].value.str[0] ? strdup(data->vars[i].value.str[0]) : NULL;
            }
            break;
        }
    }
    return NULL;
}

int arz_record_get_int(TQArzRecordData *data, const char *var_name, int default_val, bool *found) {
    if (found) *found = false;
    if (!data || !var_name) return default_val;

    /* Try fast path via var_index */
    if (data->var_index) {
        const char *interned = arz_intern(var_name);
        TQVariable *v = g_hash_table_lookup(data->var_index, interned);
        if (v && v->type == TQ_VAR_INT && v->count > 0) {
            if (found) *found = true;
            return v->value.i32[0];
        }
        return default_val;
    }

    /* Fallback: linear scan */
    for (uint32_t i = 0; i < data->num_vars; i++) {
        if (data->vars[i].name && strcasecmp(data->vars[i].name, var_name) == 0) {
            if (data->vars[i].type == TQ_VAR_INT && data->vars[i].count > 0) {
                if (found) *found = true;
                return data->vars[i].value.i32[0];
            }
            break;
        }
    }
    return default_val;
}

void arz_record_data_free(TQArzRecordData *data) {
    if (!data) return;
    if (data->var_index) g_hash_table_destroy(data->var_index);
    free(data->vars);
    free(data->pool_to_free);
    free(data->buffer_to_free);
    free(data);
}

void arz_free(TQArzFile *arz) {
    if (!arz) return;
    free(arz->filepath);
    if (arz->raw_data) munmap(arz->raw_data, arz->data_size);
    if (arz->string_table) {
        for (uint32_t i = 0; i < arz->num_strings; i++) free(arz->string_table[i]);
        free(arz->string_table);
    }
    free(arz->records);
    free(arz);
}
