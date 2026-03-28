#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

typedef struct {
  char *save_folder;
  char *game_folder;
  char *last_character_path;
  char *last_vault_name;
  int last_vault_bag;
  char *config_path; // the path where the config was loaded from
} TQConfig;

extern TQConfig global_config;

// global debug/verbose flag — set via --debug on the command line
extern bool tqvc_debug;

// config_init - load configuration from the search paths or override path
// override_path: if non-NULL, load from this path instead of default locations
void config_init(const char *override_path);

// config_save - save current configuration to the loaded path
// returns: true on success, false on failure
bool config_save(void);

// config_set_save_folder - update the save folder path in config
// path: new save folder path
void config_set_save_folder(const char *path);

// config_set_game_folder - update the game folder path in config
// path: new game folder path
void config_set_game_folder(const char *path);

// config_set_last_character - update the last loaded character name
// name: character name
void config_set_last_character(const char *name);

// config_set_last_vault - update the last loaded vault name
// name: vault name
void config_set_last_vault(const char *name);

// config_set_last_vault_bag - update the last selected vault bag index
// bag_idx: bag index
void config_set_last_vault_bag(int bag_idx);

// config_is_first_run - check if no config file existed on disk at startup
// returns: true if this is the first run
bool config_is_first_run(void);

// config_free - free resources used by the configuration
void config_free(void);

#endif
