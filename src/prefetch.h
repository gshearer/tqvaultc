#ifndef PREFETCH_H
#define PREFETCH_H

#include "vault.h"
#include "character.h"

// prefetch_for_vault - start prefetching DBR records for the given vault
// vault: vault whose item paths to prefetch
// cancels any in-progress prefetch first; warms the DBR cache for all
// item paths and follows one level of DBR chain references
void prefetch_for_vault(TQVault *vault);

// prefetch_for_character - start prefetching DBR records for the given character
// character: character whose inventory item paths to prefetch
void prefetch_for_character(TQCharacter *character);

// prefetch_cancel - cancel any in-progress prefetch (blocks until thread exits)
void prefetch_cancel(void);

// prefetch_free - cleanup at shutdown
void prefetch_free(void);

#endif
