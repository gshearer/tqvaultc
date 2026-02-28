#include "prefetch.h"
#include "asset_lookup.h"
#include "arz.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern int tqvc_debug;

static GThread *g_prefetch_thread;
static volatile int g_prefetch_cancel;

/* Interned variable name pointers — resolved once on first use */
static const char *INT_itemSkillName;
static const char *INT_buffSkillName;
static const char *INT_petBonusName;
static const char *INT_itemSetName;
static const char *INT_setMembers;
static int g_interns_ready;

static void ensure_interns(void) {
    if (g_interns_ready) return;
    INT_itemSkillName = arz_intern("itemSkillName");
    INT_buffSkillName = arz_intern("buffSkillName");
    INT_petBonusName  = arz_intern("petBonusName");
    INT_itemSetName   = arz_intern("itemSetName");
    INT_setMembers    = arz_intern("setMembers");
    g_interns_ready = 1;
}

/* Read a string variable from a record using interned name lookup */
static const char* record_str(TQArzRecordData *data, const char *interned) {
    if (!data || !interned) return NULL;
    TQVariable *v = arz_record_get_var(data, interned);
    if (!v || v->type != TQ_VAR_STRING || v->count == 0 || !v->value.str) return NULL;
    return v->value.str[0];
}

/* Follow one level of DBR chain references from a base record */
static void follow_chains(TQArzRecordData *base) {
    if (!base) return;

    /* itemSkillName -> buffSkillName */
    const char *skill_path = record_str(base, INT_itemSkillName);
    if (skill_path && skill_path[0]) {
        TQArzRecordData *skill = asset_get_dbr(skill_path);
        if (skill) {
            const char *buff = record_str(skill, INT_buffSkillName);
            if (buff && buff[0])
                asset_get_dbr(buff);
        }
    }

    /* petBonusName -> buffSkillName */
    const char *pet_path = record_str(base, INT_petBonusName);
    if (pet_path && pet_path[0]) {
        TQArzRecordData *pet = asset_get_dbr(pet_path);
        if (pet) {
            const char *buff = record_str(pet, INT_buffSkillName);
            if (buff && buff[0])
                asset_get_dbr(buff);
        }
    }

    /* itemSetName -> setMembers array */
    const char *set_path = record_str(base, INT_itemSetName);
    if (set_path && set_path[0]) {
        TQArzRecordData *set_data = asset_get_dbr(set_path);
        if (set_data) {
            TQVariable *members = arz_record_get_var(set_data, INT_setMembers);
            if (members && members->type == TQ_VAR_STRING && members->value.str) {
                for (uint32_t i = 0; i < members->count; i++) {
                    if (g_atomic_int_get(&g_prefetch_cancel)) return;
                    if (members->value.str[i] && members->value.str[i][0])
                        asset_get_dbr(members->value.str[i]);
                }
            }
        }
    }
}

static gpointer prefetch_thread_func(gpointer data) {
    char **paths = (char **)data;

    ensure_interns();

    for (int i = 0; paths[i] && !g_atomic_int_get(&g_prefetch_cancel); i++) {
        TQArzRecordData *rec = asset_get_dbr(paths[i]);

        /* Follow chain references for base item records */
        if (rec && !g_atomic_int_get(&g_prefetch_cancel))
            follow_chains(rec);

        free(paths[i]);
    }
    free(paths);

    if (tqvc_debug)
        printf("Prefetch: thread finished%s\n",
               g_atomic_int_get(&g_prefetch_cancel) ? " (cancelled)" : "");
    return NULL;
}

/* Collect all unique DBR paths from vault sack items into a NULL-terminated array */
static char** collect_item_paths(TQVaultSack *sacks, int num_sacks) {
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    int count = 0;
    int capacity = 256;
    char **paths = malloc((size_t)capacity * sizeof(char *));

    for (int s = 0; s < num_sacks; s++) {
        TQVaultSack *sack = &sacks[s];
        for (int i = 0; i < sack->num_items; i++) {
            TQVaultItem *it = &sack->items[i];
            const char *item_paths[] = {
                it->base_name, it->prefix_name, it->suffix_name,
                it->relic_name, it->relic_bonus,
                it->relic_name2, it->relic_bonus2
            };
            for (int p = 0; p < 7; p++) {
                if (!item_paths[p] || !item_paths[p][0]) continue;
                if (g_hash_table_contains(seen, item_paths[p])) continue;
                g_hash_table_add(seen, (gpointer)item_paths[p]);
                if (count + 1 >= capacity) {
                    capacity *= 2;
                    paths = realloc(paths, (size_t)capacity * sizeof(char *));
                }
                paths[count++] = strdup(item_paths[p]);
            }
        }
    }
    g_hash_table_destroy(seen);
    paths[count] = NULL;

    if (tqvc_debug)
        printf("Prefetch: collected %d unique DBR paths\n", count);

    return paths;
}

static void start_prefetch(char **paths) {
    g_atomic_int_set(&g_prefetch_cancel, 0);
    g_prefetch_thread = g_thread_new("dbr-prefetch", prefetch_thread_func, paths);
}

void prefetch_for_vault(TQVault *vault) {
    if (!vault || vault->num_sacks <= 0) return;
    prefetch_cancel();
    char **paths = collect_item_paths(vault->sacks, vault->num_sacks);
    if (!paths[0]) { free(paths); return; }
    start_prefetch(paths);
}

void prefetch_for_character(TQCharacter *character) {
    if (!character || character->num_inv_sacks <= 0) return;
    prefetch_cancel();
    char **paths = collect_item_paths(character->inv_sacks, character->num_inv_sacks);
    if (!paths[0]) { free(paths); return; }
    start_prefetch(paths);
}

void prefetch_cancel(void) {
    if (!g_prefetch_thread) return;
    g_atomic_int_set(&g_prefetch_cancel, 1);
    g_thread_join(g_prefetch_thread);
    g_prefetch_thread = NULL;
}

void prefetch_free(void) {
    prefetch_cancel();
}
