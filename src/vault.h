#ifndef VAULT_H
#define VAULT_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t seed;
    char *base_name;
    char *prefix_name;
    char *suffix_name;
    char *relic_name;
    char *relic_bonus;
    char *relic_name2;
    char *relic_bonus2;
    uint32_t var1;   /* relic/charm slot 1 shard count */
    uint32_t var2;   /* relic/charm slot 2 shard count */
    int point_x;
    int point_y;
    int width;
    int height;
    int stack_size;
    uint32_t *stack_seeds;  /* seeds for stack entries 1..stack_size-1 (entry 0 uses .seed) */
    uint32_t *stack_var2;   /* var2 for stack entries 1..stack_size-1 (entry 0 uses .var2) */
    int stack_seed_count;
} TQVaultItem;

typedef struct {
    TQVaultItem *items;
    int num_items;
} TQVaultSack;

/**
 * TQVault - Represents a storage vault (saved as JSON)
 */
typedef struct {
    char *vault_name;
    TQVaultSack *sacks;
    int num_sacks;
} TQVault;

TQVault* vault_load_json(const char *filepath);
int vault_save_json(TQVault *vault, const char *filepath);
void vault_free(TQVault *vault);
void vault_item_free_strings(TQVaultItem *it);
TQVaultItem* vault_get_item_at(TQVault *vault, int sack, int x, int y);

#endif
