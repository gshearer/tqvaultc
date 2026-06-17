#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

typedef struct {
  char *save_folder;
  char *game_folder;
  char *vault_folder; // user-chosen vault data dir; NULL => {save_folder}/TQVaultData
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

// config_set_vault_folder - update the vault data folder path in config.
// Pass NULL or "" to clear it (fall back to {save_folder}/TQVaultData).
void config_set_vault_folder(const char *path);

// config_vault_dir_new - return a newly-allocated path to the directory that
// holds the .vault.json files: the configured vault_folder when set, else
// {save_folder}/TQVaultData. Returns NULL if neither is available.
// Caller frees with g_free().
char *config_vault_dir_new(void);

// config_vault_file_new - return a newly-allocated full path to the vault file
// for the bare vault name `name` (i.e. <vault dir>/<name>.vault.json), honoring
// the configured vault folder. Returns NULL if no vault dir is available.
// Caller frees with g_free().
char *config_vault_file_new(const char *name);

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

// tqvc_cache_dir_new - return a newly-allocated path to the per-user cache
// directory used by tqvaultc (for the resource index, log file, etc).
//
// On Linux this is g_get_user_cache_dir()/tqvaultc. On Windows we deliberately
// avoid g_get_user_cache_dir() because GLib maps it to FOLDERID_InternetCache
// (the IE temp cache, which Windows can auto-purge); we use
// g_get_user_config_dir() instead so the cache lives in
// %LOCALAPPDATA%\tqvaultc next to the config file.
//
// Caller frees with g_free(). The directory is created if missing.
char *tqvc_cache_dir_new(void);

// tq_proc_peak_mem_mb - report this process's PEAK memory use, in MiB.
//   working_set_mb: peak resident/working set (physical RAM high-water).
//   commit_mb:      peak commit charge (private committed bytes) — the figure
//                   that locks up low-RAM Windows; -1 where unavailable (Linux).
// Either pointer may be NULL.  Used by `tqvaultc --first-run-build` to measure
// the first-run footprint on both platforms.
void tq_proc_peak_mem_mb(double *working_set_mb, double *commit_mb);

#endif
