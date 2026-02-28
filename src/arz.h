#ifndef ARZ_H
#define ARZ_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <glib.h>

typedef struct {
    char *path;
    uint32_t offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} TQArzRecord;

/**
 * TQArzFile - Represents a Titan Quest Database file (.arz)
 */
typedef struct {
    char *filepath;
    uint8_t *raw_data;
    size_t data_size;

    char **string_table;
    uint32_t num_strings;

    TQArzRecord *records;
    uint32_t num_records;
} TQArzFile;

typedef enum {
    TQ_VAR_INT,
    TQ_VAR_FLOAT,
    TQ_VAR_STRING,
    TQ_VAR_UNKNOWN
} TQVarType;

typedef struct {
    const char *name; // Pointer to string in arz->string_table
    TQVarType type;
    uint32_t count;
    union {
        int32_t *i32;
        float *f32;
        const char **str; // Pointers to strings in arz->string_table
    } value;
} TQVariable;

typedef struct {
    TQVariable *vars;
    uint32_t num_vars;
    uint8_t *buffer_to_free; // The uncompressed buffer
    uint8_t *pool_to_free;   // The data pool
    GHashTable *var_index;   // interned name ptr -> TQVariable*, O(1) lookup
} TQArzRecordData;

TQArzFile* arz_load(const char *filepath);
void arz_free(TQArzFile *arz);
TQArzRecordData* arz_read_record(TQArzFile *arz, const char *record_path);

/**
 * arz_read_record_at - Read a record from a specific offset and size
 */
TQArzRecordData* arz_read_record_at(TQArzFile *arz, uint32_t offset, uint32_t compressed_size);

/* found (optional): set to true if the variable exists, false otherwise.
 * Pass NULL if you don't need to distinguish "not found" from a default value. */
char* arz_record_get_string(TQArzRecordData *data, const char *var_name, bool *found);
int   arz_record_get_int   (TQArzRecordData *data, const char *var_name, int default_val, bool *found);

/**
 * arz_record_get_var - O(1) variable lookup by interned name pointer.
 * The name MUST be an interned pointer from arz_intern().
 * Returns NULL if not found.
 */
TQVariable* arz_record_get_var(TQArzRecordData *data, const char *interned_name);

void arz_record_data_free(TQArzRecordData *data);

/**
 * String interning - maps variable names to canonical pointers.
 * After init, arz_intern() returns the same pointer for equivalent names,
 * enabling O(1) hash lookups with g_direct_hash/g_direct_equal.
 */
void arz_intern_init(void);
const char* arz_intern(const char *name);
void arz_intern_free(void);

/**
 * arz_record_build_var_index - Build the var_index hash table for a record.
 * Called automatically by arz_read_record_at(), but can be called manually
 * for records constructed by the cache loader.
 */
void arz_record_build_var_index(TQArzRecordData *data);

#endif
