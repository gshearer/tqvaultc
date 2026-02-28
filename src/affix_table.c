#include "affix_table.h"
#include "arz.h"
#include "asset_lookup.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <glib.h>
#include <time.h>

/* ── Internal types ──────────────────────────────────────────────── */

/* A single affix-table triplet extracted from a LootItemTable record */
typedef struct {
    char *prefix_table;     /* path to LootRandomizerTable for prefixes */
    char *suffix_table;     /* path to LootRandomizerTable for suffixes */
} AffixTablePair;

/* Per-item list of affix table pairs */
typedef struct {
    AffixTablePair *pairs;
    int count;
} AffixTableList;

/* Global map: normalized item path -> AffixTableList */
static GHashTable *g_affix_map = NULL;

/* Cache of resolved affix results: normalized item path -> TQItemAffixes* */
static GHashTable *g_affix_cache = NULL;

/* Expansion sibling groups: category_key -> GPtrArray of normalized record paths.
 * Groups LootRandomizerTable records by gear-type category so that expansion
 * variant tables (e.g. xpack3's armmelee_l04.dbr) can be found as siblings of
 * the base table (armmelee_l01.dbr). */
static GHashTable *g_randomizer_groups = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

static char* normalize_path(const char *path) {
    if (!path) return NULL;
    char *norm = g_ascii_strdown(path, -1);
    for (char *p = norm; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    return norm;
}

static bool path_contains_ci(const char *path, const char *needle) {
    if (!path || !needle) return false;
    size_t plen = strlen(path), nlen = strlen(needle);
    for (size_t i = 0; i + nlen <= plen; i++) {
        if (strncasecmp(path + i, needle, nlen) == 0) return true;
    }
    return false;
}

/* Extract a pretty name from a file path for fallback display */
static char* pretty_filename(const char *path) {
    if (!path) return strdup("???");
    const char *last_sep = strrchr(path, '\\');
    if (!last_sep) last_sep = strrchr(path, '/');
    const char *name = last_sep ? last_sep + 1 : path;
    char *copy = strdup(name);
    /* Strip .dbr extension */
    char *dot = strrchr(copy, '.');
    if (dot) *dot = '\0';
    /* Replace underscores with spaces and capitalize first letter */
    for (char *p = copy; *p; p++) {
        if (*p == '_') *p = ' ';
    }
    if (copy[0] >= 'a' && copy[0] <= 'z') copy[0] -= 32;
    return copy;
}

/* Extract a category key from a LootRandomizerTable path.
 * E.g. "Records/Item/LootMagicalAffixes/Suffix/TablesArmor/ArmMelee_L01.dbr"
 *    → "lootmagicalaffixes\\suffix\\tablesarmor\\armmelee_l"
 * The key covers everything from "lootmagicalaffixes" onward, lowercased, with
 * trailing digits and extension stripped — so base and expansion variants share
 * the same key. Returns a g_malloc'd string or NULL. */
static char* extract_randomizer_key(const char *path) {
    if (!path) return NULL;
    const char *anchor = strcasestr(path, "lootmagicalaffixes");
    if (!anchor) return NULL;

    char *lower = g_ascii_strdown(anchor, -1);
    for (char *p = lower; *p; p++)
        if (*p == '/') *p = '\\';

    /* Strip .dbr extension */
    char *dot = strrchr(lower, '.');
    if (dot) *dot = '\0';

    /* Strip trailing digits from filename (e.g. "armmelee_l01" → "armmelee_l") */
    size_t len = strlen(lower);
    while (len > 0 && lower[len - 1] >= '0' && lower[len - 1] <= '9')
        len--;
    lower[len] = '\0';

    return lower;
}

static void ptr_array_free_wrapper(gpointer data) {
    g_ptr_array_free(data, TRUE);
}

/* ── Free helpers ────────────────────────────────────────────────── */

static void affix_table_list_free(gpointer data) {
    AffixTableList *list = data;
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->pairs[i].prefix_table);
        free(list->pairs[i].suffix_table);
    }
    free(list->pairs);
    free(list);
}

static void affix_result_free_internal(gpointer data) {
    affix_result_free((TQItemAffixes*)data);
}

/* ── Phase A: Build item->table map ──────────────────────────────── */

/* Process a single LootItemTable record (FixedWeight or DynWeight) */
static void process_loot_item_table(TQArzRecordData *dbr, const char *class_name) {
    /* Collect loot names (item paths this table applies to) */
    char *loot_names[64];
    int num_loot_names = 0;

    bool is_fixed = (strcasecmp(class_name, "LootItemTable_FixedWeight") == 0);

    if (is_fixed) {
        /* FixedWeight: individual lootName, lootName01, lootName02, ... fields */
        for (uint32_t i = 0; i < dbr->num_vars; i++) {
            if (!dbr->vars[i].name) continue;
            if (strncasecmp(dbr->vars[i].name, "lootName", 8) == 0 &&
                dbr->vars[i].type == TQ_VAR_STRING && dbr->vars[i].count > 0) {
                const char *val = dbr->vars[i].value.str[0];
                if (val && val[0] && num_loot_names < 64) {
                    loot_names[num_loot_names++] = strdup(val);
                }
            }
        }
    } else {
        /* DynWeight: itemNames array field */
        for (uint32_t i = 0; i < dbr->num_vars; i++) {
            if (!dbr->vars[i].name) continue;
            if (strcasecmp(dbr->vars[i].name, "itemNames") == 0 &&
                dbr->vars[i].type == TQ_VAR_STRING) {
                for (uint32_t j = 0; j < dbr->vars[i].count && num_loot_names < 64; j++) {
                    const char *val = dbr->vars[i].value.str[j];
                    if (val && val[0]) {
                        loot_names[num_loot_names++] = strdup(val);
                    }
                }
                break;
            }
        }
    }

    if (num_loot_names == 0) return;

    /* Collect prefix/suffix randomizer table paths, grouped by numeric suffix.
     * Variables look like: prefixRandomizerName, prefixRandomizerName01, ... */
    GHashTable *table_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    for (uint32_t i = 0; i < dbr->num_vars; i++) {
        if (!dbr->vars[i].name || dbr->vars[i].type != TQ_VAR_STRING ||
            dbr->vars[i].count == 0 || !dbr->vars[i].value.str[0] ||
            !dbr->vars[i].value.str[0][0])
            continue;

        const char *vname = dbr->vars[i].name;
        const char *val = dbr->vars[i].value.str[0];
        const char *num_suffix = NULL;
        bool is_prefix = false, is_suffix = false;

        if (strncasecmp(vname, "prefixRandomizerName", 20) == 0) {
            num_suffix = vname + 20;
            is_prefix = true;
        } else if (strncasecmp(vname, "suffixRandomizerName", 20) == 0) {
            num_suffix = vname + 20;
            is_suffix = true;
        }

        if (!is_prefix && !is_suffix) continue;

        /* Use the numeric suffix as group key (empty string for base) */
        char *key = g_strdup(num_suffix);

        /* Two strings stored as: prefix_table\0suffix_table\0 in a struct */
        typedef struct { char *prefix; char *suffix; } PairBuf;
        PairBuf *pair = g_hash_table_lookup(table_groups, key);
        if (!pair) {
            pair = g_new0(PairBuf, 1);
            g_hash_table_insert(table_groups, key, pair);
        } else {
            g_free(key);
        }

        if (is_prefix) { free(pair->prefix); pair->prefix = strdup(val); }
        if (is_suffix) { free(pair->suffix); pair->suffix = strdup(val); }
    }

    /* Convert group hash to AffixTablePair array */
    int num_pairs = g_hash_table_size(table_groups);
    if (num_pairs == 0) {
        g_hash_table_destroy(table_groups);
        for (int i = 0; i < num_loot_names; i++) free(loot_names[i]);
        return;
    }

    AffixTablePair *pairs = calloc(num_pairs, sizeof(AffixTablePair));
    GHashTableIter iter;
    gpointer gkey, gval;
    int pair_idx = 0;
    g_hash_table_iter_init(&iter, table_groups);
    while (g_hash_table_iter_next(&iter, &gkey, &gval)) {
        typedef struct { char *prefix; char *suffix; } PairBuf;
        PairBuf *pb = gval;
        pairs[pair_idx].prefix_table = pb->prefix;  /* transfer ownership */
        pairs[pair_idx].suffix_table = pb->suffix;
        pb->prefix = NULL;
        pb->suffix = NULL;
        pair_idx++;
    }
    /* Don't free pair strings — ownership transferred */
    g_hash_table_destroy(table_groups);

    /* Insert into global map for each loot name */
    for (int i = 0; i < num_loot_names; i++) {
        char *norm_key = normalize_path(loot_names[i]);
        free(loot_names[i]);

        AffixTableList *existing = g_hash_table_lookup(g_affix_map, norm_key);
        if (existing) {
            /* Append pairs to existing list */
            int new_count = existing->count + num_pairs;
            existing->pairs = realloc(existing->pairs, new_count * sizeof(AffixTablePair));
            for (int j = 0; j < num_pairs; j++) {
                existing->pairs[existing->count + j].prefix_table =
                    pairs[j].prefix_table ? strdup(pairs[j].prefix_table) : NULL;
                existing->pairs[existing->count + j].suffix_table =
                    pairs[j].suffix_table ? strdup(pairs[j].suffix_table) : NULL;
            }
            existing->count = new_count;
            g_free(norm_key);
        } else {
            AffixTableList *list = g_new0(AffixTableList, 1);
            if (i == num_loot_names - 1) {
                /* Last item: transfer ownership of pairs array */
                list->pairs = pairs;
                list->count = num_pairs;
                pairs = NULL;  /* prevent double-free below */
            } else {
                /* Duplicate pairs for earlier items */
                list->pairs = calloc(num_pairs, sizeof(AffixTablePair));
                for (int j = 0; j < num_pairs; j++) {
                    list->pairs[j].prefix_table =
                        pairs[j].prefix_table ? strdup(pairs[j].prefix_table) : NULL;
                    list->pairs[j].suffix_table =
                        pairs[j].suffix_table ? strdup(pairs[j].suffix_table) : NULL;
                }
                list->count = num_pairs;
            }
            g_hash_table_insert(g_affix_map, norm_key, list);
        }
    }

    /* Free pairs array if not transferred */
    if (pairs) {
        for (int j = 0; j < num_pairs; j++) {
            free(pairs[j].prefix_table);
            free(pairs[j].suffix_table);
        }
        free(pairs);
    }
}

void affix_table_init(TQTranslation *tr) {
    (void)tr;  /* tr not needed at init time, only at resolve time */

    if (g_affix_map) return;  /* already initialized */

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    g_affix_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, affix_table_list_free);
    g_affix_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, affix_result_free_internal);
    g_randomizer_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ptr_array_free_wrapper);

    int num_files = asset_get_num_files();
    int records_scanned = 0;
    int tables_found = 0;
    int randomizer_groups = 0;

    for (int fid = 0; fid < num_files; fid++) {
        const char *fpath = asset_get_file_path((uint16_t)fid);
        if (!fpath) continue;
        /* Only process .arz files */
        const char *ext = strrchr(fpath, '.');
        if (!ext || strcasecmp(ext, ".arz") != 0) continue;

        TQArzFile *arz = asset_get_arz((uint16_t)fid);
        if (!arz) continue;

        for (uint32_t ri = 0; ri < arz->num_records; ri++) {
            const char *rpath = arz->records[ri].path;
            if (!rpath) continue;

            /* Phase 1: Index LootRandomizerTable records by gear-type category.
             * This lets us find expansion sibling tables later. */
            if (path_contains_ci(rpath, "lootmagicalaffixes")) {
                char *cat_key = extract_randomizer_key(rpath);
                if (cat_key) {
                    GPtrArray *arr = g_hash_table_lookup(g_randomizer_groups, cat_key);
                    if (!arr) {
                        arr = g_ptr_array_new_with_free_func(g_free);
                        g_hash_table_insert(g_randomizer_groups, cat_key, arr);
                        randomizer_groups++;
                    } else {
                        g_free(cat_key);
                    }
                    g_ptr_array_add(arr, normalize_path(rpath));
                }
            }

            /* Phase 2: Process LootItemTable records (existing logic) */
            if (!path_contains_ci(rpath, "loottables") &&
                !path_contains_ci(rpath, "merchantloottables"))
                continue;

            records_scanned++;

            /* Decompress and check Class field */
            TQArzRecordData *dbr = arz_read_record_at(arz,
                arz->records[ri].offset, arz->records[ri].compressed_size);
            if (!dbr) continue;

            char *class_name = arz_record_get_string(dbr, "Class", NULL);
            if (class_name &&
                (strcasecmp(class_name, "LootItemTable_FixedWeight") == 0 ||
                 strcasecmp(class_name, "LootItemTable_DynWeight") == 0)) {
                process_loot_item_table(dbr, class_name);
                tables_found++;
            }
            free(class_name);
            arz_record_data_free(dbr);
        }
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    if (tqvc_debug) {
        fprintf(stderr, "Affix table init: scanned %d loot records, found %d tables, "
                "%d items mapped, %d randomizer groups in %.1f ms\n",
                records_scanned, tables_found,
                g_hash_table_size(g_affix_map), randomizer_groups, ms);
    }
}

/* ── Phase B: Resolve affix list on demand ───────────────────────── */

static int compare_affix_entries(const void *a, const void *b) {
    const TQAffixEntry *ea = a;
    const TQAffixEntry *eb = b;
    return strcasecmp(ea->translation, eb->translation);
}

/* Resolve a single randomizer table to a list of affix entries */
static void resolve_randomizer_table(const char *table_path, TQTranslation *tr,
                                      TQAffixEntry **out_entries, int *out_count) {
    if (!table_path || !table_path[0]) return;

    TQArzRecordData *dbr = asset_get_dbr(table_path);
    if (!dbr) return;

    /* Collect randomizerName[N] / randomizerWeight[N] pairs */
    GHashTable *pairs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    typedef struct { char *path; float weight; } RandPair;

    for (uint32_t i = 0; i < dbr->num_vars; i++) {
        if (!dbr->vars[i].name) continue;
        const char *vname = dbr->vars[i].name;

        if (strncasecmp(vname, "randomizerName", 14) == 0 &&
            dbr->vars[i].type == TQ_VAR_STRING && dbr->vars[i].count > 0) {
            const char *val = dbr->vars[i].value.str[0];
            if (!val || !val[0]) continue;
            const char *num = vname + 14;
            char *key = g_strdup(num);
            RandPair *rp = g_hash_table_lookup(pairs, key);
            if (!rp) {
                rp = g_new0(RandPair, 1);
                g_hash_table_insert(pairs, key, rp);
            } else {
                g_free(key);
            }
            free(rp->path);
            rp->path = strdup(val);
        } else if (strncasecmp(vname, "randomizerWeight", 16) == 0) {
            const char *num = vname + 16;
            float w = 0;
            if (dbr->vars[i].type == TQ_VAR_INT && dbr->vars[i].count > 0)
                w = (float)dbr->vars[i].value.i32[0];
            else if (dbr->vars[i].type == TQ_VAR_FLOAT && dbr->vars[i].count > 0)
                w = dbr->vars[i].value.f32[0];
            if (w <= 0) continue;

            char *key = g_strdup(num);
            RandPair *rp = g_hash_table_lookup(pairs, key);
            if (!rp) {
                rp = g_new0(RandPair, 1);
                g_hash_table_insert(pairs, key, rp);
            } else {
                g_free(key);
            }
            rp->weight = w;
        }
    }

    /* Convert valid pairs to affix entries */
    int capacity = g_hash_table_size(pairs);
    TQAffixEntry *entries = *out_entries;
    int count = *out_count;
    entries = realloc(entries, (count + capacity) * sizeof(TQAffixEntry));

    GHashTableIter iter;
    gpointer gkey, gval;
    g_hash_table_iter_init(&iter, pairs);
    while (g_hash_table_iter_next(&iter, &gkey, &gval)) {
        RandPair *rp = gval;
        if (!rp->path || rp->weight <= 0) {
            free(rp->path);
            rp->path = NULL;
            continue;
        }

        /* Check if this affix path already exists (deduplicate, sum weights) */
        bool found_dup = false;
        for (int j = 0; j < count; j++) {
            if (strcasecmp(entries[j].affix_path, rp->path) == 0) {
                entries[j].weight += rp->weight;
                found_dup = true;
                break;
            }
        }
        if (found_dup) {
            free(rp->path);
            rp->path = NULL;
            continue;
        }

        /* Resolve translation */
        char *translation = NULL;
        TQArzRecordData *affix_dbr = asset_get_dbr(rp->path);
        if (affix_dbr) {
            char *tag = arz_record_get_string(affix_dbr, "lootRandomizerName", NULL);
            if (tag && tag[0] && tr) {
                const char *trans = translation_get(tr, tag);
                if (trans && trans[0]) translation = strdup(trans);
            }
            free(tag);
            if (!translation) {
                char *fdesc = arz_record_get_string(affix_dbr, "FileDescription", NULL);
                if (fdesc && fdesc[0]) {
                    translation = fdesc;
                } else {
                    free(fdesc);
                }
            }
        }
        if (!translation) translation = pretty_filename(rp->path);

        entries[count].affix_path = rp->path;
        entries[count].translation = translation;
        entries[count].weight = rp->weight;
        rp->path = NULL;  /* ownership transferred */
        count++;
    }

    /* Clean up any remaining rp->path that weren't transferred */
    g_hash_table_iter_init(&iter, pairs);
    while (g_hash_table_iter_next(&iter, &gkey, &gval)) {
        RandPair *rp = gval;
        free(rp->path);
        rp->path = NULL;
    }
    g_hash_table_destroy(pairs);

    *out_entries = entries;
    *out_count = count;
}

TQItemAffixes* affix_table_get(const char *item_base_name, TQTranslation *tr) {
    if (!item_base_name || !g_affix_map) return NULL;

    char *norm = normalize_path(item_base_name);

    /* Check cache */
    TQItemAffixes *cached = g_hash_table_lookup(g_affix_cache, norm);
    if (cached) {
        g_free(norm);
        return cached;
    }

    /* Look up affix table list */
    AffixTableList *tbl = g_hash_table_lookup(g_affix_map, norm);
    if (!tbl || tbl->count == 0) {
        g_free(norm);
        return NULL;
    }

    TQItemAffixes *result = calloc(1, sizeof(TQItemAffixes));

    /* Track which randomizer tables we've already resolved (avoid duplicates) */
    GHashTable *resolved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Resolve prefix and suffix tables, including expansion siblings */
    for (int i = 0; i < tbl->count; i++) {
        const char *tables[2] = { tbl->pairs[i].prefix_table, tbl->pairs[i].suffix_table };
        TQAffixEntry **lists[2] = { &result->prefixes.entries, &result->suffixes.entries };
        int *counts[2] = { &result->prefixes.count, &result->suffixes.count };

        for (int t = 0; t < 2; t++) {
            if (!tables[t] || !tables[t][0]) continue;

            /* Resolve the base table */
            char *tnorm = normalize_path(tables[t]);
            if (!g_hash_table_contains(resolved, tnorm)) {
                g_hash_table_add(resolved, tnorm);
                resolve_randomizer_table(tables[t], tr, lists[t], counts[t]);
            } else {
                g_free(tnorm);
            }

            /* Find and resolve expansion sibling tables in the same category */
            char *cat_key = extract_randomizer_key(tables[t]);
            if (cat_key && g_randomizer_groups) {
                GPtrArray *siblings = g_hash_table_lookup(g_randomizer_groups, cat_key);
                if (siblings) {
                    for (guint s = 0; s < siblings->len; s++) {
                        const char *sibling_path = g_ptr_array_index(siblings, s);
                        char *snorm = g_strdup(sibling_path);  /* already normalized */
                        if (!g_hash_table_contains(resolved, snorm)) {
                            g_hash_table_add(resolved, snorm);
                            resolve_randomizer_table(sibling_path, tr, lists[t], counts[t]);
                        } else {
                            g_free(snorm);
                        }
                    }
                }
                g_free(cat_key);
            }
        }
    }

    g_hash_table_destroy(resolved);

    /* Sort by translation */
    if (result->prefixes.count > 0)
        qsort(result->prefixes.entries, result->prefixes.count,
              sizeof(TQAffixEntry), compare_affix_entries);
    if (result->suffixes.count > 0)
        qsort(result->suffixes.entries, result->suffixes.count,
              sizeof(TQAffixEntry), compare_affix_entries);

    /* If both are empty, return NULL */
    if (result->prefixes.count == 0 && result->suffixes.count == 0) {
        free(result);
        g_free(norm);
        return NULL;
    }

    /* Cache the result (cache takes ownership of norm key) */
    g_hash_table_insert(g_affix_cache, norm, result);
    return result;
}

/* ── Eligibility check ───────────────────────────────────────────── */

bool item_can_modify_affixes(const char *base_name) {
    if (!base_name || !base_name[0]) return false;

    TQArzRecordData *dbr = asset_get_dbr(base_name);
    if (!dbr) return false;

    /* Check classification: Epic and Legendary are non-moddable */
    char *classification = arz_record_get_string(dbr, "itemClassification", NULL);
    if (classification) {
        if (strcasecmp(classification, "Epic") == 0 ||
            strcasecmp(classification, "Legendary") == 0) {
            free(classification);
            return false;
        }
        free(classification);
    }

    /* Check Class field: must be equipment (armor/weapon/shield/jewelry) */
    char *class_name = arz_record_get_string(dbr, "Class", NULL);
    if (!class_name) return false;

    bool is_equipment =
        strstr(class_name, "UpperBody") ||
        strstr(class_name, "LowerBody") ||
        strstr(class_name, "Head") ||
        strstr(class_name, "Forearm") ||
        strstr(class_name, "WeaponMelee") ||
        strstr(class_name, "WeaponHunting") ||
        strstr(class_name, "WeaponMagical") ||
        strstr(class_name, "Shield") ||
        strstr(class_name, "Amulet") ||
        strstr(class_name, "Ring");
    free(class_name);
    return is_equipment;
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

void affix_result_free(TQItemAffixes *affixes) {
    if (!affixes) return;
    for (int i = 0; i < affixes->prefixes.count; i++) {
        free(affixes->prefixes.entries[i].affix_path);
        free(affixes->prefixes.entries[i].translation);
    }
    free(affixes->prefixes.entries);
    for (int i = 0; i < affixes->suffixes.count; i++) {
        free(affixes->suffixes.entries[i].affix_path);
        free(affixes->suffixes.entries[i].translation);
    }
    free(affixes->suffixes.entries);
    free(affixes);
}

void affix_table_free(void) {
    if (g_affix_cache) { g_hash_table_destroy(g_affix_cache); g_affix_cache = NULL; }
    if (g_affix_map) { g_hash_table_destroy(g_affix_map); g_affix_map = NULL; }
    if (g_randomizer_groups) { g_hash_table_destroy(g_randomizer_groups); g_randomizer_groups = NULL; }
}
