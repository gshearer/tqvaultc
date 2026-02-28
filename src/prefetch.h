#ifndef PREFETCH_H
#define PREFETCH_H

#include "vault.h"
#include "character.h"

/* Start prefetching DBR records for the given vault/character.
 * Cancels any in-progress prefetch first.
 * The prefetch thread warms the DBR cache for all item paths
 * and follows one level of DBR chain references. */
void prefetch_for_vault(TQVault *vault);
void prefetch_for_character(TQCharacter *character);

/* Cancel any in-progress prefetch (blocks until thread exits). */
void prefetch_cancel(void);

/* Cleanup at shutdown. */
void prefetch_free(void);

#endif
