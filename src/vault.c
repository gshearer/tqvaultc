#include "vault.h"
#include "config.h"
#include "asset_lookup.h"
#include "arz.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TQVault* vault_load_json(const char *filepath) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buffer = malloc((size_t)size + 1);
    if (fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp); free(buffer); return NULL;
    }
    fclose(fp);
    buffer[size] = '\0';

    char *json_ptr = buffer;
    if (size >= 3 && (unsigned char)buffer[0] == 0xEF && (unsigned char)buffer[1] == 0xBB && (unsigned char)buffer[2] == 0xBF) {
        json_ptr += 3;
    }

    struct json_object *parsed_json = json_tokener_parse(json_ptr);
    free(buffer);

    if (!parsed_json) {
        return NULL;
    }

    TQVault *vault = calloc(1, sizeof(TQVault));
    vault->vault_name = strdup(filepath);

    struct json_object *sacks_arr;
    if (json_object_object_get_ex(parsed_json, "sacks", &sacks_arr) || 
        json_object_object_get_ex(parsed_json, "Sacks", &sacks_arr)) {
        
        int num_sacks = json_object_array_length(sacks_arr);
        vault->num_sacks = num_sacks;
        vault->sacks = calloc((size_t)num_sacks, sizeof(TQVaultSack));

        for (int s = 0; s < num_sacks; s++) {
            struct json_object *sack_obj = json_object_array_get_idx(sacks_arr, s);
            if (!sack_obj) continue;

            struct json_object *items_arr;
            if (!json_object_object_get_ex(sack_obj, "items", &items_arr) &&
                !json_object_object_get_ex(sack_obj, "Items", &items_arr))
                continue;

            int num_items = json_object_array_length(items_arr);
            vault->sacks[s].num_items = num_items;
            vault->sacks[s].items = calloc((size_t)num_items, sizeof(TQVaultItem));

            for (int i = 0; i < num_items; i++) {
                struct json_object *item_obj = json_object_array_get_idx(items_arr, i);
                if (!item_obj) continue;

                struct json_object *val;
                if (json_object_object_get_ex(item_obj, "seed", &val))
                    vault->sacks[s].items[i].seed = (uint32_t)json_object_get_int(val);
                if (json_object_object_get_ex(item_obj, "baseName", &val))
                    vault->sacks[s].items[i].base_name = strdup(json_object_get_string(val));
                if (json_object_object_get_ex(item_obj, "prefixName", &val))
                    vault->sacks[s].items[i].prefix_name = strdup(json_object_get_string(val));
                if (json_object_object_get_ex(item_obj, "suffixName", &val))
                    vault->sacks[s].items[i].suffix_name = strdup(json_object_get_string(val));
                if (json_object_object_get_ex(item_obj, "relicName", &val))
                    vault->sacks[s].items[i].relic_name = strdup(json_object_get_string(val));
                if (json_object_object_get_ex(item_obj, "relicBonus", &val))
                    vault->sacks[s].items[i].relic_bonus = strdup(json_object_get_string(val));
                if (json_object_object_get_ex(item_obj, "relicName2", &val))
                    vault->sacks[s].items[i].relic_name2 = strdup(json_object_get_string(val));
                if (json_object_object_get_ex(item_obj, "relicBonus2", &val))
                    vault->sacks[s].items[i].relic_bonus2 = strdup(json_object_get_string(val));
                if (json_object_object_get_ex(item_obj, "var1", &val))
                    vault->sacks[s].items[i].var1 = (uint32_t)json_object_get_int(val);
                if (json_object_object_get_ex(item_obj, "var2", &val))
                    vault->sacks[s].items[i].var2 = (uint32_t)json_object_get_int(val);

                if (json_object_object_get_ex(item_obj, "stackSize", &val))
                    vault->sacks[s].items[i].stack_size = json_object_get_int(val);
                if (vault->sacks[s].items[i].stack_size < 1)
                    vault->sacks[s].items[i].stack_size = 1;

                if (json_object_object_get_ex(item_obj, "pointX", &val))
                    vault->sacks[s].items[i].point_x = json_object_get_int(val);
                if (json_object_object_get_ex(item_obj, "pointY", &val))
                    vault->sacks[s].items[i].point_y = json_object_get_int(val);
                
                // Determine dimensions from DBR Class
                vault->sacks[s].items[i].width = 1;
                vault->sacks[s].items[i].height = 1;
                if (vault->sacks[s].items[i].base_name) {
                    TQArzRecordData *dbr = asset_get_dbr(vault->sacks[s].items[i].base_name);
                    if (!dbr) {
                        if (tqvc_debug)
                            fprintf(stderr, "vault: asset_get_dbr failed for '%s'\n",
                                    vault->sacks[s].items[i].base_name);
                    } else {
                        bool class_found;
                        char *class_name = arz_record_get_string(dbr, "Class", &class_found);
                        if (!class_found) {
                            if (tqvc_debug)
                                fprintf(stderr, "vault: 'Class' not found in DBR '%s'\n",
                                        vault->sacks[s].items[i].base_name);
                        } else if (class_name) {
                            /* Armor */
                            if      (strstr(class_name, "UpperBody"))  { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 4; }
                            else if (strstr(class_name, "LowerBody"))  { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 2; }
                            else if (strstr(class_name, "Head"))       { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 2; }
                            else if (strstr(class_name, "Forearm"))    { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 2; }
                            /* Weapons */
                            else if (strstr(class_name, "WeaponMelee"))    { vault->sacks[s].items[i].width = 1; vault->sacks[s].items[i].height = 3; }
                            else if (strstr(class_name, "WeaponHunting"))  { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 4; }
                            else if (strstr(class_name, "WeaponMagical"))  { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 4; }
                            else if (strstr(class_name, "Shield"))         { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 3; }
                            /* Jewelry */
                            else if (strstr(class_name, "Amulet"))     { vault->sacks[s].items[i].width = 1; vault->sacks[s].items[i].height = 2; }
                            else if (strstr(class_name, "Ring"))       { /* 1x1 default */ }
                            /* Artifacts and formulas */
                            else if (strstr(class_name, "ItemArtifactFormula")) { vault->sacks[s].items[i].width = 1; vault->sacks[s].items[i].height = 2; }
                            else if (strstr(class_name, "ItemArtifact"))        { vault->sacks[s].items[i].width = 2; vault->sacks[s].items[i].height = 2; }
                            /* Relics, charms, consumables, quest items — all 1x1 */
                            else if (strstr(class_name, "ItemRelic"))  { /* 1x1 default */ }
                            else if (strstr(class_name, "ItemCharm"))  { /* 1x1 default */ }
                            else if (strstr(class_name, "OneShot"))    { /* 1x1 default */ }
                            else if (strstr(class_name, "QuestItem"))  { /* 1x1 default */ }
                            else if (strstr(class_name, "ItemEquipment")) { /* 1x1 default */ }
                            else {
                                if (tqvc_debug)
                                    fprintf(stderr, "vault: unrecognised Class '%s' in DBR '%s'\n",
                                            class_name, vault->sacks[s].items[i].base_name);
                            }
                            free(class_name);
                        }
                        bool iw_found, ih_found;
                        int iw = arz_record_get_int(dbr, "ItemWidth",  0, &iw_found);
                        int ih = arz_record_get_int(dbr, "ItemHeight", 0, &ih_found);
                        if (iw_found && iw > 0) vault->sacks[s].items[i].width  = iw;
                        if (ih_found && ih > 0) vault->sacks[s].items[i].height = ih;
                    }
                }
            }
        }
    }

    json_object_put(parsed_json);
    return vault;
}

int vault_save_json(TQVault *vault, const char *filepath) {
    if (!vault || !filepath) return -1;

    struct json_object *root = json_object_new_object();

    /* Top-level fields matching TQVaultAE format */
    json_object_object_add(root, "disabledtooltip", json_object_new_array());
    json_object_object_add(root, "currentlyFocusedSackNumber", json_object_new_int(0));
    json_object_object_add(root, "currentlySelectedSackNumber", json_object_new_int(0));

    struct json_object *sacks_arr = json_object_new_array();
    for (int s = 0; s < vault->num_sacks; s++) {
        struct json_object *sack_obj = json_object_new_object();
        json_object_object_add(sack_obj, "iconinfo", NULL);

        struct json_object *items_arr = json_object_new_array();
        TQVaultSack *sack = &vault->sacks[s];
        for (int i = 0; i < sack->num_items; i++) {
            TQVaultItem *it = &sack->items[i];
            struct json_object *item_obj = json_object_new_object();
            json_object_object_add(item_obj, "stackSize",
                json_object_new_int(it->stack_size > 0 ? it->stack_size : 1));
            json_object_object_add(item_obj, "seed", json_object_new_int((int32_t)it->seed));
            json_object_object_add(item_obj, "baseName",
                json_object_new_string(it->base_name ? it->base_name : ""));
            json_object_object_add(item_obj, "prefixName",
                json_object_new_string(it->prefix_name ? it->prefix_name : ""));
            json_object_object_add(item_obj, "suffixName",
                json_object_new_string(it->suffix_name ? it->suffix_name : ""));
            json_object_object_add(item_obj, "relicName",
                json_object_new_string(it->relic_name ? it->relic_name : ""));
            json_object_object_add(item_obj, "relicBonus",
                json_object_new_string(it->relic_bonus ? it->relic_bonus : ""));
            json_object_object_add(item_obj, "var1", json_object_new_int((int32_t)it->var1));
            json_object_object_add(item_obj, "relicName2",
                json_object_new_string(it->relic_name2 ? it->relic_name2 : ""));
            json_object_object_add(item_obj, "relicBonus2",
                json_object_new_string(it->relic_bonus2 ? it->relic_bonus2 : ""));
            json_object_object_add(item_obj, "var2", json_object_new_int((int32_t)it->var2));
            json_object_object_add(item_obj, "pointX", json_object_new_int(it->point_x));
            json_object_object_add(item_obj, "pointY", json_object_new_int(it->point_y));
            json_object_array_add(items_arr, item_obj);
        }
        json_object_object_add(sack_obj, "items", items_arr);
        json_object_array_add(sacks_arr, sack_obj);
    }
    json_object_object_add(root, "sacks", sacks_arr);

    const char *json_str = json_object_to_json_string_ext(root,
        JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);

    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        json_object_put(root);
        return -1;
    }
    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    json_object_put(root);
    return 0;
}

TQVaultItem* vault_get_item_at(TQVault *vault, int sack, int x, int y) {
    if (!vault || sack < 0 || sack >= vault->num_sacks) return NULL;

    TQVaultSack *s = &vault->sacks[sack];
    for (int i = 0; i < s->num_items; i++) {
        TQVaultItem *item = &s->items[i];
        int w = item->width > 0 ? item->width : 1;
        int h = item->height > 0 ? item->height : 1;
        if (x >= item->point_x && x < item->point_x + w &&
            y >= item->point_y && y < item->point_y + h && item->base_name)
            return item;
    }
    return NULL;
}

void vault_item_free_strings(TQVaultItem *it) {
    free(it->base_name);    it->base_name    = NULL;
    free(it->prefix_name);  it->prefix_name  = NULL;
    free(it->suffix_name);  it->suffix_name  = NULL;
    free(it->relic_name);   it->relic_name   = NULL;
    free(it->relic_bonus);  it->relic_bonus  = NULL;
    free(it->relic_name2);  it->relic_name2  = NULL;
    free(it->relic_bonus2); it->relic_bonus2 = NULL;
}

void vault_free(TQVault *vault) {
    if (!vault) return;
    for (int s = 0; s < vault->num_sacks; s++) {
        for (int i = 0; i < vault->sacks[s].num_items; i++)
            vault_item_free_strings(&vault->sacks[s].items[i]);
        free(vault->sacks[s].items);
    }
    free(vault->sacks);
    free(vault->vault_name);
    free(vault);
}
