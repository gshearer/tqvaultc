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

// TQArzFile - represents a Titan Quest Database file (.arz)
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
  const char *name; // pointer to string in arz->string_table
  TQVarType type;
  uint32_t count;
  union {
    int32_t *i32;
    float *f32;
    const char **str; // pointers to strings in arz->string_table
  } value;
} TQVariable;

typedef struct {
  TQVariable *vars;
  uint32_t num_vars;
  uint8_t *buffer_to_free; // the uncompressed buffer
  uint8_t *pool_to_free;   // the data pool
  GHashTable *var_index;   // interned name ptr -> TQVariable*, O(1) lookup
} TQArzRecordData;

// arz_load - load and parse an ARZ database file
// filepath: path to the .arz file
// returns: parsed database, or NULL on failure
TQArzFile *arz_load(const char *filepath);

// arz_free - free all resources associated with an ARZ database
// arz: database to free
void arz_free(TQArzFile *arz);

// arz_read_record - read a record by its path from the database
// arz: database file
// record_path: path of the record to read
// returns: parsed record data, or NULL if not found
TQArzRecordData *arz_read_record(TQArzFile *arz, const char *record_path);

// arz_read_record_at - read a record from a specific offset and size
// arz: database file
// offset: byte offset into the database
// compressed_size: compressed record size
// returns: parsed record data, or NULL on failure
TQArzRecordData *arz_read_record_at(TQArzFile *arz, uint32_t offset,
                                    uint32_t compressed_size);

// arz_record_get_string - get a string variable value from a record
// data: record data
// var_name: variable name to look up
// found: optional, set to true if variable exists
// returns: strdup'd string (caller must free), or NULL
char *arz_record_get_string(TQArzRecordData *data, const char *var_name,
                            bool *found);

// arz_record_get_int - get an integer variable value from a record
// data: record data
// var_name: variable name to look up
// default_val: value to return if variable not found
// found: optional, set to true if variable exists
// returns: integer value, or default_val if not found
int arz_record_get_int(TQArzRecordData *data, const char *var_name,
                       int default_val, bool *found);

// arz_record_get_var - O(1) variable lookup by interned name pointer
// data: record data
// interned_name: MUST be an interned pointer from arz_intern()
// returns: pointer to variable, or NULL if not found
TQVariable *arz_record_get_var(TQArzRecordData *data,
                               const char *interned_name);

// arz_record_data_free - free a parsed record and all its resources
// data: record data to free
void arz_record_data_free(TQArzRecordData *data);

// arz_intern_init - initialize the string interning system
void arz_intern_init(void);

// arz_intern - intern a variable name string
// name: variable name to intern
// returns: canonical pointer for this name
const char *arz_intern(const char *name);

// arz_intern_free - free the string interning system
void arz_intern_free(void);

// arz_record_build_var_index - build the var_index hash table for a record
// data: record data to index
// called automatically by arz_read_record_at(), but can be called manually
// for records constructed by the cache loader
void arz_record_build_var_index(TQArzRecordData *data);

#endif
