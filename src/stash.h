#ifndef STASH_H
#define STASH_H

#include <stdint.h>
#include <stdbool.h>
#include "vault.h"

typedef enum {
    STASH_TRANSFER,      // SaveData/Sys/winsys.dxb -- global, shared across characters
    STASH_PLAYER,        // SaveData/Main/_CharName/winsys.dxb -- per-character
    STASH_RELIC_VAULT    // SaveData/Sys/miscsys.dxb -- global, relics/charms
} StashType;

typedef struct {
    char *filepath;
    StashType type;
    int stash_version;
    char *stash_name;        // fName from file (raw bytes)
    int stash_name_len;      // length of fName (may contain non-ASCII)
    int sack_width, sack_height;
    uint32_t begin_block_val;  // "crap" value preserved for round-trip
    TQVaultSack sack;
    bool dirty;
} TQStash;

// Load a stash from a .dxb file.
// filepath: path to the .dxb stash file.
// Returns: allocated TQStash, or NULL if file doesn't exist or fails.
TQStash *
stash_load(const char *filepath);

// Save a stash to its filepath.
// stash: the stash to save.
// Returns: 0 on success, -1 on error.
int
stash_save(TQStash *stash);

// Free all memory associated with a stash.
// stash: the stash to free (NULL-safe).
void
stash_free(TQStash *stash);

// Build the filesystem path for a stash type.
// type: which stash type to build the path for.
// char_folder_name: only used for STASH_PLAYER (e.g. "_Artemis").
// Returns: allocated path string, or NULL on error. Caller must free.
char *
stash_build_path(StashType type, const char *char_folder_name);

#endif
