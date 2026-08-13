#include "config.h"
#include "io_atomic.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>          // GetProcessMemoryInfo for tq_proc_peak_mem_mb
#else
#include <sys/resource.h>   // getrusage for tq_proc_peak_mem_mb
#endif

#define CONFIG_FILENAME "tqvc-config.json"

TQConfig global_config = {NULL, NULL, NULL, NULL, NULL, 0, NULL};
bool tqvc_debug = false;
static bool g_first_run = false;

// json_strdup - strdup a JSON string value, tolerating a JSON null.
// json_object_get_string() returns NULL for a JSON null, and strdup(NULL) is
// undefined; a corrupt/hand-edited config must not crash on load.
// val: json object (may represent null); returns malloc'd copy or NULL.
static char *
json_strdup(struct json_object *val)
{
  const char *s = json_object_get_string(val);

  return(s ? strdup(s) : NULL);
}

// load_from_file - load configuration from a JSON file on disk
// path: filesystem path to the config JSON file
static void
load_from_file(const char *path)
{
  // Open in binary mode: Windows text mode translates CRLF on read, so the
  // byte count returned by fread() is smaller than the on-disk size that
  // ftell() reports — which our size-mismatch check below would reject.
  FILE *fp = fopen(path, "rb");

  if(!fp)
  {
    fprintf(stderr, "load_from_file: fopen(%s) failed: %s\n", path, strerror(errno));
    return;
  }

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if(size < 0)
  {
    fprintf(stderr, "load_from_file: ftell(%s) failed: %s\n", path, strerror(errno));
    fclose(fp);
    return;
  }

  char *buffer = malloc((size_t)size + 1);

  if(!buffer)
  {
    fclose(fp);
    return;
  }

  if(fread(buffer, 1, size, fp) != (size_t)size)
  {
    fprintf(stderr, "load_from_file: short read from %s\n", path);
    free(buffer);
    fclose(fp);
    return;
  }
  fclose(fp);
  buffer[size] = '\0';

  struct json_object *parsed_json = json_tokener_parse(buffer);

  free(buffer);

  if(!parsed_json)
    return;

  struct json_object *save_folder_obj, *game_folder_obj, *last_char_obj, *last_vault_obj;

  if(json_object_object_get_ex(parsed_json, "save_folder", &save_folder_obj))
    global_config.save_folder = json_strdup(save_folder_obj);

  if(json_object_object_get_ex(parsed_json, "game_folder", &game_folder_obj))
    global_config.game_folder = json_strdup(game_folder_obj);

  // vault_folder: only honor a non-empty value (config_save writes "" when
  // unset, which must read back as NULL so we fall back to the default dir).
  struct json_object *vault_folder_obj;

  if(json_object_object_get_ex(parsed_json, "vault_folder", &vault_folder_obj))
  {
    const char *vf = json_object_get_string(vault_folder_obj);

    if(vf && vf[0])
      global_config.vault_folder = strdup(vf);
  }

  if(json_object_object_get_ex(parsed_json, "last_character_path", &last_char_obj))
    global_config.last_character_path = json_strdup(last_char_obj);

  if(json_object_object_get_ex(parsed_json, "last_vault_name", &last_vault_obj))
    global_config.last_vault_name = json_strdup(last_vault_obj);

  struct json_object *last_vault_bag_obj;

  if(json_object_object_get_ex(parsed_json, "last_vault_bag", &last_vault_bag_obj))
    global_config.last_vault_bag = json_object_get_int(last_vault_bag_obj);

  global_config.config_path = strdup(path);
  json_object_put(parsed_json);
}

// config_default_game_folder - see config.h. Static per-platform default; no
// existence check (callers gate on g_file_test when they need a real dir).
char *
config_default_game_folder(void)
{
#ifdef _WIN32
  return(g_build_filename("C:\\Program Files (x86)", "Steam", "steamapps",
                          "common", "Titan Quest Anniversary Edition", NULL));
#else
  const char *home = g_get_home_dir();

  if(!home)
    return(NULL);
  return(g_build_filename(home, ".local", "share", "Steam", "steamapps",
                          "common", "Titan Quest Anniversary Edition", NULL));
#endif
}

// config_default_save_folder - see config.h. Windows has a static default;
// Linux saves live under a Proton compatdata prefix that must be scanned, so
// there is no static default there (returns NULL).
char *
config_default_save_folder(void)
{
#ifdef _WIN32
  const char *home = g_get_home_dir();

  if(!home)
    return(NULL);
  return(g_build_filename(home, "Documents", "My Games",
                          "Titan Quest - Immortal Throne", NULL));
#else
  return(NULL);
#endif
}

// config_init - load configuration from the search paths or override path
// override_path: if non-NULL, load from this path instead of default locations
void
config_init(const char *override_path)
{
  fprintf(stderr, "config_init: g_get_user_config_dir() = %s\n",
          g_get_user_config_dir());

  if(override_path)
  {
    load_from_file(override_path);
    if(global_config.config_path)
    {
      fprintf(stderr, "config_init: loaded override %s\n", override_path);
      return;
    }
  }

  // try current folder
  if(g_file_test(CONFIG_FILENAME, G_FILE_TEST_EXISTS))
  {
    load_from_file(CONFIG_FILENAME);
    if(global_config.config_path)
      return;
  }

  // try user config dir (XDG_CONFIG_HOME on Linux; on Windows GLib maps
  // g_get_user_config_dir() to %LOCALAPPDATA% -- the SAME base tqvc_cache_dir_new
  // uses, so the config file and the cache share %LOCALAPPDATA%\tqvaultc)
  char *path = g_build_filename(g_get_user_config_dir(), "tqvaultc", CONFIG_FILENAME, NULL);

  if(g_file_test(path, G_FILE_TEST_EXISTS))
  {
    fprintf(stderr, "config_init: loading existing %s\n", path);
    load_from_file(path);
  }
  else
  {
    fprintf(stderr, "config_init: no existing config; first-run path = %s\n", path);
    g_first_run = true;
  }

  // Always set config_path so subsequent saves work, even if load_from_file
  // bailed out (e.g. parse error, partial write from a previous crash).
  if(!global_config.config_path)
    global_config.config_path = g_strdup(path);
  g_free(path);

#ifdef _WIN32
  // set default game folder if not loaded (Windows Steam path)
  if(!global_config.game_folder)
  {
    char *default_game_path = config_default_game_folder();

    if(default_game_path && g_file_test(default_game_path, G_FILE_TEST_IS_DIR))
      global_config.game_folder = default_game_path;
    else
      g_free(default_game_path);
  }

  // set default save folder if not loaded (Windows save path)
  if(!global_config.save_folder)
  {
    char *save_path = config_default_save_folder();

    if(save_path && g_file_test(save_path, G_FILE_TEST_IS_DIR))
      global_config.save_folder = save_path;
    else
      g_free(save_path);
  }
#else
  // set default game folder if not loaded (Linux Steam path)
  if(!global_config.game_folder)
    global_config.game_folder = config_default_game_folder();

  // set default save folder if not loaded -- scan compatdata for the TQ save dir
  if(!global_config.save_folder)
  {
    const char *home = g_get_home_dir();

    if(home)
    {
      char *compat_base = g_build_filename(home,
          ".local", "share", "Steam", "steamapps", "compatdata", NULL);
      GDir *dp = g_dir_open(compat_base, 0, NULL);

      if(dp)
      {
        const gchar *ent_name;

        while((ent_name = g_dir_read_name(dp)) != NULL)
        {
          if(ent_name[0] == '.')
            continue;

          char *candidate = g_build_filename(compat_base, ent_name,
              "pfx", "drive_c", "users", "steamuser", "Documents",
              "My Games", "Titan Quest - Immortal Throne", NULL);

          if(g_file_test(candidate, G_FILE_TEST_IS_DIR))
          {
            global_config.save_folder = candidate;
            break;
          }
          g_free(candidate);
        }
        g_dir_close(dp);
      }
      g_free(compat_base);
    }
  }
#endif
}

// config_set_save_folder - update the save folder path in config
// path: new save folder path, or NULL to clear
void
config_set_save_folder(const char *path)
{
  if(global_config.save_folder)
    free(global_config.save_folder);

  global_config.save_folder = path ? strdup(path) : NULL;
}

// config_set_game_folder - update the game folder path in config
// path: new game folder path, or NULL to clear
void
config_set_game_folder(const char *path)
{
  if(global_config.game_folder)
    free(global_config.game_folder);

  global_config.game_folder = path ? strdup(path) : NULL;
}

// config_set_vault_folder - update the vault data folder path in config
// path: new vault folder path, or NULL/"" to clear (use the default dir)
void
config_set_vault_folder(const char *path)
{
  if(global_config.vault_folder)
    free(global_config.vault_folder);

  global_config.vault_folder = (path && path[0]) ? strdup(path) : NULL;
}

// config_vault_dir_new - directory that holds the .vault.json files
char *
config_vault_dir_new(void)
{
  if(global_config.vault_folder && global_config.vault_folder[0])
    return(g_strdup(global_config.vault_folder));

  if(global_config.save_folder && global_config.save_folder[0])
    return(g_build_filename(global_config.save_folder, "TQVaultData", NULL));

  return(NULL);
}

// config_vault_file_new - full path to <vault dir>/<name>.vault.json
char *
config_vault_file_new(const char *name)
{
  char *dir = config_vault_dir_new();

  if(!dir)
    return(NULL);

  char *base = g_strconcat(name, ".vault.json", NULL);
  char *full = g_build_filename(dir, base, NULL);

  g_free(dir);
  g_free(base);
  return(full);
}

// config_set_last_character - update the last loaded character path in config
// name: character path, or NULL to clear
void
config_set_last_character(const char *name)
{
  if(global_config.last_character_path)
    free(global_config.last_character_path);

  global_config.last_character_path = name ? strdup(name) : NULL;
}

// config_set_last_vault - update the last loaded vault name in config
// name: vault name, or NULL to clear
void
config_set_last_vault(const char *name)
{
  if(global_config.last_vault_name)
    free(global_config.last_vault_name);

  global_config.last_vault_name = name ? strdup(name) : NULL;
}

// config_set_last_vault_bag - update the last selected vault bag index
// bag_idx: bag index to store
void
config_set_last_vault_bag(int bag_idx)
{
  global_config.last_vault_bag = bag_idx;
}

// config_is_first_run - check if no config file existed on disk at startup
// returns: true if this is the first run
bool
config_is_first_run(void)
{
  return(g_first_run);
}

// config_save - serialize current configuration to JSON and write to disk
// returns: true on success, false on failure
bool
config_save(void)
{
  if(!global_config.config_path)
  {
    fprintf(stderr, "config_save: no config_path set, aborting\n");
    return(false);
  }

  fprintf(stderr, "config_save: writing %s\n", global_config.config_path);

  // ensure directory exists
  char *dir_path = g_path_get_dirname(global_config.config_path);

  if(g_mkdir_with_parents(dir_path, 0755) != 0)
    fprintf(stderr, "config_save: g_mkdir_with_parents(%s) failed\n", dir_path);
  g_free(dir_path);

  struct json_object *root = json_object_new_object();

  json_object_object_add(root, "save_folder",
      json_object_new_string(global_config.save_folder ? global_config.save_folder : ""));
  json_object_object_add(root, "game_folder",
      json_object_new_string(global_config.game_folder ? global_config.game_folder : ""));
  json_object_object_add(root, "vault_folder",
      json_object_new_string(global_config.vault_folder ? global_config.vault_folder : ""));
  json_object_object_add(root, "last_character_path",
      json_object_new_string(global_config.last_character_path ? global_config.last_character_path : ""));
  json_object_object_add(root, "last_vault_name",
      json_object_new_string(global_config.last_vault_name ? global_config.last_vault_name : ""));
  json_object_object_add(root, "last_vault_bag",
      json_object_new_int(global_config.last_vault_bag));

  const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
  // Written as raw bytes (never stdio text mode): that keeps the on-disk size
  // honest so the read-back path's size check doesn't reject our own file,
  // which Windows text mode would break by expanding "\n" to "\r\n".
  bool ok = tq_write_file_atomic(global_config.config_path, json_str,
                                 strlen(json_str));

  json_object_put(root);

  if(!ok)
  {
    fprintf(stderr, "config_save: cannot write %s\n",
            global_config.config_path);
    return(false);
  }

  fprintf(stderr, "config_save: success\n");
  return(true);
}

// tqvc_cache_dir_new - per-user cache dir (avoids GLib's INetCache mapping
// of g_get_user_cache_dir() on Windows). See config.h for details.
char *
tqvc_cache_dir_new(void)
{
#ifdef _WIN32
  const char *base = g_get_user_config_dir();  // -> %LOCALAPPDATA%
#else
  const char *base = g_get_user_cache_dir();
#endif
  char *dir = g_build_filename(base, "tqvaultc", NULL);

  g_mkdir_with_parents(dir, 0755);
  return(dir);
}

// config_free - free all resources used by the global configuration
void
config_free(void)
{
  free(global_config.save_folder);
  free(global_config.game_folder);
  free(global_config.vault_folder);
  free(global_config.last_character_path);
  free(global_config.last_vault_name);
  free(global_config.config_path);
}

// tq_proc_peak_mem_mb - peak working set + peak commit charge, in MiB.
void
tq_proc_peak_mem_mb(double *working_set_mb, double *commit_mb)
{
#ifdef _WIN32
  // PeakWorkingSetSize = peak physical RAM; PeakPagefileUsage = peak commit
  // charge (private committed bytes) — the figure that exhausts Windows.
  PROCESS_MEMORY_COUNTERS pmc;

  memset(&pmc, 0, sizeof(pmc));
  pmc.cb = sizeof(pmc);
  if(GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
  {
    if(working_set_mb)
      *working_set_mb = (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    if(commit_mb)
      *commit_mb = (double)pmc.PeakPagefileUsage / (1024.0 * 1024.0);
  }
  else
  {
    if(working_set_mb)
      *working_set_mb = -1.0;
    if(commit_mb)
      *commit_mb = -1.0;
  }
#else
  struct rusage ru;

  getrusage(RUSAGE_SELF, &ru);
  if(working_set_mb)
    *working_set_mb = (double)ru.ru_maxrss / 1024.0;   // ru_maxrss is KiB on Linux
  if(commit_mb)
    *commit_mb = -1.0;                                  // not tracked here on POSIX
#endif
}
