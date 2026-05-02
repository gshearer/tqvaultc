#include "config.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>

#define CONFIG_FILENAME "tqvc-config.json"

TQConfig global_config = {NULL, NULL, NULL, NULL, 0, NULL};
bool tqvc_debug = false;
static bool g_first_run = false;

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

  char *buffer = malloc(size + 1);

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
    global_config.save_folder = strdup(json_object_get_string(save_folder_obj));

  if(json_object_object_get_ex(parsed_json, "game_folder", &game_folder_obj))
    global_config.game_folder = strdup(json_object_get_string(game_folder_obj));

  if(json_object_object_get_ex(parsed_json, "last_character_path", &last_char_obj))
    global_config.last_character_path = strdup(json_object_get_string(last_char_obj));

  if(json_object_object_get_ex(parsed_json, "last_vault_name", &last_vault_obj))
    global_config.last_vault_name = strdup(json_object_get_string(last_vault_obj));

  struct json_object *last_vault_bag_obj;

  if(json_object_object_get_ex(parsed_json, "last_vault_bag", &last_vault_bag_obj))
    global_config.last_vault_bag = json_object_get_int(last_vault_bag_obj);

  global_config.config_path = strdup(path);
  json_object_put(parsed_json);
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

  // try user config dir (XDG_CONFIG_HOME on Linux, %APPDATA% on Windows)
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
    char *default_game_path = g_build_filename(
        "C:\\Program Files (x86)", "Steam", "steamapps", "common",
        "Titan Quest Anniversary Edition", NULL);

    if(g_file_test(default_game_path, G_FILE_TEST_IS_DIR))
      global_config.game_folder = default_game_path;
    else
      g_free(default_game_path);
  }

  // set default save folder if not loaded (Windows save path)
  if(!global_config.save_folder)
  {
    const char *userprofile = g_get_home_dir();

    if(userprofile)
    {
      char *save_path = g_build_filename(userprofile, "Documents",
          "My Games", "Titan Quest - Immortal Throne", NULL);

      if(g_file_test(save_path, G_FILE_TEST_IS_DIR))
        global_config.save_folder = save_path;
      else
        g_free(save_path);
    }
  }
#else
  // set default game folder if not loaded (Linux Steam path)
  if(!global_config.game_folder)
  {
    const char *home = g_get_home_dir();

    if(home)
    {
      char *default_game_path = g_build_filename(home,
          ".local", "share", "Steam", "steamapps", "common",
          "Titan Quest Anniversary Edition", NULL);

      global_config.game_folder = default_game_path;
    }
  }

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
  json_object_object_add(root, "last_character_path",
      json_object_new_string(global_config.last_character_path ? global_config.last_character_path : ""));
  json_object_object_add(root, "last_vault_name",
      json_object_new_string(global_config.last_vault_name ? global_config.last_vault_name : ""));
  json_object_object_add(root, "last_vault_bag",
      json_object_new_int(global_config.last_vault_bag));

  const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
  // Binary mode: keeps the on-disk size honest so the read-back path's
  // size check doesn't reject our own file (Windows text mode would
  // expand "\n" to "\r\n" on write).
  FILE *fp = fopen(global_config.config_path, "wb");

  if(!fp)
  {
    fprintf(stderr, "config_save: fopen(%s, w) failed: %s\n",
            global_config.config_path, strerror(errno));
    json_object_put(root);
    return(false);
  }

  fputs(json_str, fp);
  fclose(fp);
  json_object_put(root);
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
  free(global_config.last_character_path);
  free(global_config.last_vault_name);
  free(global_config.config_path);
}
