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
  uint32_t var1;  // relic/charm slot 1 shard count
  uint32_t var2;  // relic/charm slot 2 shard count
  int point_x;
  int point_y;
  int width;
  int height;
  int stack_size;
  uint32_t *stack_seeds; // seeds for stack entries 1..stack_size-1 (entry 0 uses .seed)
  uint32_t *stack_var2;  // var2 for stack entries 1..stack_size-1 (entry 0 uses .var2)
  int stack_seed_count;
} TQVaultItem;

typedef struct {
  TQVaultItem *items;
  int num_items;
} TQVaultSack;

// TQVault - represents a storage vault (saved as JSON)
typedef struct {
  char *vault_name;
  TQVaultSack *sacks;
  int num_sacks;
} TQVault;

// vault_load_json - load a vault from a JSON file
// filepath: path to the vault JSON file
// returns: parsed vault, or NULL on failure
TQVault *vault_load_json(const char *filepath);

// vault_save_json - save a vault to a JSON file
// vault: vault to save
// filepath: path to write the JSON file
// returns: 0 on success, -1 on failure
int vault_save_json(TQVault *vault, const char *filepath);

// vault_free - free all resources associated with a vault
// vault: vault to free
void vault_free(TQVault *vault);

// vault_item_free_strings - free all string fields in a vault item
// it: vault item whose strings to free
void vault_item_free_strings(TQVaultItem *it);

// vault_get_item_at - find an item at a specific grid position
// vault: vault to search
// sack: sack index
// x: grid column
// y: grid row
// returns: pointer to the item, or NULL if no item at that position
TQVaultItem *vault_get_item_at(TQVault *vault, int sack, int x, int y);

#endif
