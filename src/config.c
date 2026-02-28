#include "config.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#define CONFIG_FILENAME "tqvc-config.json"

TQConfig global_config = {NULL, NULL, NULL, NULL, NULL};
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

    global_config.config_path = strdup(path);
    json_object_put(parsed_json);
}

void config_init(const char *override_path) {
    if (override_path) {
        load_from_file(override_path);
        if (global_config.config_path) return;
    }

    // Try current folder
    if (access(CONFIG_FILENAME, F_OK) == 0) {
        load_from_file(CONFIG_FILENAME);
        if (global_config.config_path) return;
    }

    // Try XDG_CONFIG_HOME
    char path[1024];
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config) {
        snprintf(path, sizeof(path), "%s/tqvaultc/%s", xdg_config, CONFIG_FILENAME);
    } else {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(path, sizeof(path), "%s/.config/tqvaultc/%s", home, CONFIG_FILENAME);
        } else {
            return;
        }
    }

    if (access(path, F_OK) == 0) {
        load_from_file(path);
    } else {
        // If not found, set default save path for later
        global_config.config_path = strdup(path);
        g_first_run = true;
    }

    // Set default game folder if not loaded
    if (!global_config.game_folder) {
        const char *home = getenv("HOME");
        if (home) {
            char default_game_path[1024];
            snprintf(default_game_path, sizeof(default_game_path),
                     "%s/.local/share/Steam/steamapps/common/Titan Quest Anniversary Edition", home);
            global_config.game_folder = strdup(default_game_path);
        }
    }

    // Set default save folder if not loaded — scan compatdata for the TQ save dir
    if (!global_config.save_folder) {
        const char *home = getenv("HOME");
        if (home) {
            char compat_base[1024];
            snprintf(compat_base, sizeof(compat_base),
                     "%s/.local/share/Steam/steamapps/compatdata", home);
            static const char *tq_suffix =
                "/pfx/drive_c/users/steamuser/Documents/My Games"
                "/Titan Quest - Immortal Throne";
            DIR *dp = opendir(compat_base);
            if (dp) {
                struct dirent *ent;
                while ((ent = readdir(dp)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    char candidate[2048];
                    snprintf(candidate, sizeof(candidate), "%s/%s%s",
                             compat_base, ent->d_name, tq_suffix);
                    struct stat st;
                    if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                        global_config.save_folder = strdup(candidate);
                        break;
                    }
                }
                closedir(dp);
            }
        }
    }
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

bool config_is_first_run(void) {
    return g_first_run;
}

bool config_save() {
    if (!global_config.config_path) return false;

    // Ensure directory exists
    char *dir_path = strdup(global_config.config_path);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        // Simple mkdir -p equivalent for one level
        mkdir(dir_path, 0755);
    }
    free(dir_path);

    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "save_folder", json_object_new_string(global_config.save_folder ? global_config.save_folder : ""));
    json_object_object_add(root, "game_folder", json_object_new_string(global_config.game_folder ? global_config.game_folder : ""));
    json_object_object_add(root, "last_character_path", json_object_new_string(global_config.last_character_path ? global_config.last_character_path : ""));
    json_object_object_add(root, "last_vault_name", json_object_new_string(global_config.last_vault_name ? global_config.last_vault_name : ""));

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

void config_free() {
    free(global_config.save_folder);
    free(global_config.game_folder);
    free(global_config.last_character_path);
    free(global_config.last_vault_name);
    free(global_config.config_path);
}
