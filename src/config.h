#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

typedef struct {
    char *save_folder;
    char *game_folder;
    char *last_character_path;
    char *last_vault_name;
    int last_vault_bag;
    char *config_path; // The path where the config was loaded from
} TQConfig;

extern TQConfig global_config;

/* Global debug/verbose flag — set via --debug on the command line */
extern bool tqvc_debug;

/**
 * config_init - Load configuration from the search paths or override path
 */
void config_init(const char *override_path);

/**
 * config_save - Save current configuration to the loaded path
 */
bool config_save();

void config_set_save_folder(const char *path);
void config_set_game_folder(const char *path);
void config_set_last_character(const char *name);
void config_set_last_vault(const char *name);
void config_set_last_vault_bag(int bag_idx);

/**
 * config_is_first_run - Returns true if no config file existed on disk at startup
 */
bool config_is_first_run(void);

/**
 * config_free - Free resources used by the configuration
 */
void config_free();

#endif
