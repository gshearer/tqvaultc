#include "config.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#define CONFIG_FILENAME "tqvc-config.json"

TQConfig global_config = {NULL, NULL, NULL, NULL, 0, NULL};
bool tqvc_debug = false;
static bool g_first_run = false;

static void load_from_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buffer = malloc(size + 1);
    if (fread(buffer, 1, size, fp) != (size_t)size) {
        free(buffer);
        fclose(fp);
        return;
    }
    fclose(fp);
    buffer[size] = '\0';

    struct json_object *parsed_json = json_tokener_parse(buffer);
    free(buffer);

    if (!parsed_json) return;

    struct json_object *save_folder_obj, *game_folder_obj, *last_char_obj, *last_vault_obj;

    if (json_object_object_get_ex(parsed_json, "save_folder", &save_folder_obj)) {
        global_config.save_folder = strdup(json_object_get_string(save_folder_obj));
    }
    if (json_object_object_get_ex(parsed_json, "game_folder", &game_folder_obj)) {
        global_config.game_folder = strdup(json_object_get_string(game_folder_obj));
    }
    if (json_object_object_get_ex(parsed_json, "last_character_path", &last_char_obj)) {
        global_config.last_character_path = strdup(json_object_get_string(last_char_obj));
    }
    if (json_object_object_get_ex(parsed_json, "last_vault_name", &last_vault_obj)) {
        global_config.last_vault_name = strdup(json_object_get_string(last_vault_obj));
    }
    struct json_object *last_vault_bag_obj;
    if (json_object_object_get_ex(parsed_json, "last_vault_bag", &last_vault_bag_obj)) {
        global_config.last_vault_bag = json_object_get_int(last_vault_bag_obj);
    }

    global_config.config_path = strdup(path);
    json_object_put(parsed_json);
}

void config_init(const char *override_path) {
    if (override_path) {
        load_from_file(override_path);
        if (global_config.config_path) return;
    }

    // Try current folder
    if (g_file_test(CONFIG_FILENAME, G_FILE_TEST_EXISTS)) {
        load_from_file(CONFIG_FILENAME);
        if (global_config.config_path) return;
    }

    // Try user config dir (XDG_CONFIG_HOME on Linux, %APPDATA% on Windows)
    char *path = g_build_filename(g_get_user_config_dir(), "tqvaultc", CONFIG_FILENAME, NULL);

    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        load_from_file(path);
        g_free(path);
    } else {
        // If not found, set default save path for later
        global_config.config_path = g_strdup(path);
        g_free(path);
        g_first_run = true;
    }

#ifdef _WIN32
    // Set default game folder if not loaded (Windows Steam path)
    if (!global_config.game_folder) {
        char *default_game_path = g_build_filename(
            "C:\\Program Files (x86)", "Steam", "steamapps", "common",
            "Titan Quest Anniversary Edition", NULL);
        if (g_file_test(default_game_path, G_FILE_TEST_IS_DIR))
            global_config.game_folder = default_game_path;
        else
            g_free(default_game_path);
    }

    // Set default save folder if not loaded (Windows save path)
    if (!global_config.save_folder) {
        const char *userprofile = g_get_home_dir();
        if (userprofile) {
            char *save_path = g_build_filename(userprofile, "Documents",
                "My Games", "Titan Quest - Immortal Throne", NULL);
            if (g_file_test(save_path, G_FILE_TEST_IS_DIR))
                global_config.save_folder = save_path;
            else
                g_free(save_path);
        }
    }
#else
    // Set default game folder if not loaded (Linux Steam path)
    if (!global_config.game_folder) {
        const char *home = g_get_home_dir();
        if (home) {
            char *default_game_path = g_build_filename(home,
                ".local", "share", "Steam", "steamapps", "common",
                "Titan Quest Anniversary Edition", NULL);
            global_config.game_folder = default_game_path;
        }
    }

    // Set default save folder if not loaded — scan compatdata for the TQ save dir
    if (!global_config.save_folder) {
        const char *home = g_get_home_dir();
        if (home) {
            char *compat_base = g_build_filename(home,
                ".local", "share", "Steam", "steamapps", "compatdata", NULL);
            GDir *dp = g_dir_open(compat_base, 0, NULL);
            if (dp) {
                const gchar *ent_name;
                while ((ent_name = g_dir_read_name(dp)) != NULL) {
                    if (ent_name[0] == '.') continue;
                    char *candidate = g_build_filename(compat_base, ent_name,
                        "pfx", "drive_c", "users", "steamuser", "Documents",
                        "My Games", "Titan Quest - Immortal Throne", NULL);
                    if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
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

void config_set_save_folder(const char *path) {
    if (global_config.save_folder) free(global_config.save_folder);
    global_config.save_folder = path ? strdup(path) : NULL;
}

void config_set_game_folder(const char *path) {
    if (global_config.game_folder) free(global_config.game_folder);
    global_config.game_folder = path ? strdup(path) : NULL;
}

void config_set_last_character(const char *name) {
    if (global_config.last_character_path) free(global_config.last_character_path);
    global_config.last_character_path = name ? strdup(name) : NULL;
}

void config_set_last_vault(const char *name) {
    if (global_config.last_vault_name) free(global_config.last_vault_name);
    global_config.last_vault_name = name ? strdup(name) : NULL;
}

void config_set_last_vault_bag(int bag_idx) {
    global_config.last_vault_bag = bag_idx;
}

bool config_is_first_run(void) {
    return g_first_run;
}

bool config_save(void) {
    if (!global_config.config_path) return false;

    // Ensure directory exists
    char *dir_path = g_path_get_dirname(global_config.config_path);
    g_mkdir_with_parents(dir_path, 0755);
    g_free(dir_path);

    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "save_folder", json_object_new_string(global_config.save_folder ? global_config.save_folder : ""));
    json_object_object_add(root, "game_folder", json_object_new_string(global_config.game_folder ? global_config.game_folder : ""));
    json_object_object_add(root, "last_character_path", json_object_new_string(global_config.last_character_path ? global_config.last_character_path : ""));
    json_object_object_add(root, "last_vault_name", json_object_new_string(global_config.last_vault_name ? global_config.last_vault_name : ""));
    json_object_object_add(root, "last_vault_bag", json_object_new_int(global_config.last_vault_bag));

    const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    FILE *fp = fopen(global_config.config_path, "w");
    if (!fp) {
        json_object_put(root);
        return false;
    }

    fputs(json_str, fp);
    fclose(fp);
    json_object_put(root);
    return true;
}

void config_free(void) {
    free(global_config.save_folder);
    free(global_config.game_folder);
    free(global_config.last_character_path);
    free(global_config.last_vault_name);
    free(global_config.config_path);
}
