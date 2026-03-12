/*
 * tq_quest_tool.c — Universal quest data inspection and manipulation tool
 *
 * Parses and analyzes all three quest state file types:
 *   - QuestToken.myw — flat token bag
 *   - Quest.myw — trigger log + rewards section
 *   - *.que files — per-quest state machines
 *
 * Usage:
 *   tq-quest-tool <command> [options]
 *
 * Token Commands:
 *   dump     <myw>                   List all tokens in a QuestToken.myw file
 *   count    <myw>                   Count tokens
 *   search   <myw> <pattern>         List tokens matching a substring (case-insensitive)
 *   has      <myw> <token>           Check if a specific token exists
 *   acts     <myw>                   Group tokens by act (using prefix heuristics)
 *   quests   <myw>                   Show quest completion status against quest_defs[]
 *   add      <myw> <token>           Add a token to the file
 *   remove   <myw> <token>           Remove a token from the file
 *   complete <myw> <quest_name>      Add all tokens for a named quest
 *   clear    <myw> <quest_name>      Remove all tokens for a named quest
 *   roundtrip <myw>                  Load, save to temp, compare (verify parser/writer)
 *   defs                             List all quest definitions
 *   diff     <myw_a> <myw_b>        Show tokens present in one file but not the other
 *   coverage <myw>                   Report covered/orphaned/uncovered tokens vs quest_defs[]
 *
 * Quest State Commands:
 *   dump-que      <file>             Full structural dump of .que file (all fields)
 *   dump-quest-myw <file>            Full Quest.myw parser (triggers + rewards + MD5 mapping)
 *   clear-que     <dir>              Zero all hasFired/isPendingFire in .que files
 *   compare-que   <dir_a> <dir_b>    Compare .que flag differences between directories
 *   que-info      <dir>              Identify/categorize all .que files (embedded paths, flags)
 *   scan          <save_dir>         Full overview of a character's quest state across all files
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <glib.h>
#include "../quest_tokens.h"

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> [options]\n"
        "\n"
        "Token Commands (QuestToken.myw):\n"
        "  dump     <myw>                 List all tokens in a QuestToken.myw file\n"
        "  count    <myw>                 Count tokens\n"
        "  search   <myw> <pattern>       List tokens matching substring (case-insensitive)\n"
        "  has      <myw> <token>         Check if a specific token exists (exact match)\n"
        "  acts     <myw>                 Group tokens by act using prefix heuristics\n"
        "  quests   <myw>                 Show quest completion status vs quest_defs[]\n"
        "  add      <myw> <token>         Add a token to the file (saves in-place, .bak created)\n"
        "  remove   <myw> <token>         Remove a token from the file (saves in-place, .bak created)\n"
        "  complete <myw> <quest_name>    Add all tokens for a named quest\n"
        "  clear    <myw> <quest_name>    Remove all tokens for a named quest\n"
        "  roundtrip <myw>                Load, save to /tmp, byte-compare with original\n"
        "  defs                           List all quest definitions\n"
        "  diff     <myw_a> <myw_b>      Show tokens present in one file but not the other\n"
        "  coverage <myw>                 Report covered/orphaned/uncovered tokens vs quest_defs[]\n"
        "\n"
        "Quest State Commands (.que files + Quest.myw):\n"
        "  dump-que       <file>          Full structural dump of .que file (all fields)\n"
        "  dump-quest-myw <file>          Full Quest.myw dump (triggers + rewards + MD5 mapping)\n"
        "  clear-que      <dir>           Zero all hasFired/isPendingFire in .que files\n"
        "  compare-que    <dir_a> <dir_b> Compare .que flag differences between directories\n"
        "  que-info       <dir>           Identify/categorize all .que files in directory\n"
        "\n"
        "Analysis Commands:\n"
        "  scan           <save_dir>      Full character quest state overview across all files\n"
        "\n"
        "Examples:\n"
        "  %s dump testdata/saves/_soothie/Levels_World_World01.map/Legendary/QuestToken.myw\n"
        "  %s dump-que testdata/saves/_soothie/Levels_World_World01.map/Legendary/0273f539*.que\n"
        "  %s dump-quest-myw testdata/saves/_soothie/Levels_World_World01.map/Legendary/Quest.myw\n"
        "  %s que-info testdata/saves/_soothie/Levels_World_World01.map/Legendary/\n"
        "  %s scan testdata/saves/_soothie/\n"
        "  %s defs\n",
        prog, prog, prog, prog, prog, prog, prog);
}

/* Case-insensitive substring search */
static bool ci_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* Sort comparison for qsort of string pointers */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Heuristic: determine which act a token belongs to based on prefix */
static QuestAct guess_act(const char *token) {
    if (strncasecmp(token, "x4", 2) == 0) return ACT_ETERNAL_EMBERS;
    if (strncasecmp(token, "MQ", 2) == 0 ||
        strncasecmp(token, "X3_", 3) == 0 ||
        strncasecmp(token, "x2SF", 4) == 0 ||
        strncasecmp(token, "x3", 2) == 0 ||
        strcasecmp(token, "MapUnlockAtlantis") == 0) return ACT_ATLANTIS;
    if (strncasecmp(token, "xQ", 2) == 0 ||
        strncasecmp(token, "xSQ", 3) == 0 ||
        strncasecmp(token, "xBossChest_", 11) == 0 ||
        strcasecmp(token, "Olympus - Typhon Defeated") == 0) return ACT_IMMORTAL_THRONE;
    if (strncasecmp(token, "Q0", 2) == 0 && isdigit((unsigned char)token[2]) && token[2] >= '1' && token[2] <= '7' &&
        !strchr(token, '_')) return ACT_RAGNAROK;
    if (strncasecmp(token, "SQ", 2) == 0 && isdigit((unsigned char)token[2])) return ACT_RAGNAROK;
    if (strncasecmp(token, "EPILOGUE", 8) == 0) return ACT_RAGNAROK;
    if (strncasecmp(token, "Q08_", 4) == 0 ||
        strncasecmp(token, "Q09_", 4) == 0 ||
        strncasecmp(token, "Q10_", 4) == 0 ||
        strncasecmp(token, "Q11_", 4) == 0 ||
        strncasecmp(token, "JE", 2) == 0 ||
        strcasecmp(token, "Egypt - Telkine Defeated") == 0 ||
        strcasecmp(token, "ImhotepKnown") == 0) return ACT_EGYPT;
    if (strncasecmp(token, "Q12_", 4) == 0 ||
        strncasecmp(token, "JO", 2) == 0 ||
        strcasecmp(token, "Orient - Telkine Defeated") == 0 ||
        strcasecmp(token, "MapUnlockOrient") == 0 ||
        strcasecmp(token, "YellowEmperorKnown") == 0) return ACT_ORIENT;
    if (strncasecmp(token, "Q1_", 3) == 0 ||
        strncasecmp(token, "Q2_", 3) == 0 ||
        strncasecmp(token, "JG", 2) == 0 ||
        strncasecmp(token, "SS_", 3) == 0 ||
        strncasecmp(token, "BossChest_", 10) == 0 ||
        strcasecmp(token, "Greece - Telkine Defeated") == 0) return ACT_GREECE;
    /* Misc tokens that don't clearly belong to any act */
    return ACT_GREECE; /* default bucket */
}

/* Find a quest def by name (case-insensitive substring match) */
static const QuestDef *find_quest_by_name(const char *name) {
    int count;
    const QuestDef *defs = quest_get_defs(&count);
    /* Try exact match first */
    for (int i = 0; i < count; i++) {
        if (strcasecmp(defs[i].name, name) == 0)
            return &defs[i];
    }
    /* Try substring */
    for (int i = 0; i < count; i++) {
        if (ci_contains(defs[i].name, name))
            return &defs[i];
    }
    return NULL;
}

/* ── Commands ─────────────────────────────────────────────────────────── */

static int cmd_dump(const char *path) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    /* Sort for consistent output */
    qsort(set.tokens, set.count, sizeof(char *), cmp_str);
    for (int i = 0; i < set.count; i++)
        printf("%s\n", set.tokens[i]);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_count(const char *path) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    printf("%d tokens\n", set.count);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_search(const char *path, const char *pattern) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    qsort(set.tokens, set.count, sizeof(char *), cmp_str);
    int hits = 0;
    for (int i = 0; i < set.count; i++) {
        if (ci_contains(set.tokens[i], pattern)) {
            printf("%s\n", set.tokens[i]);
            hits++;
        }
    }
    printf("--- %d matches\n", hits);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_has(const char *path, const char *token) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    bool found = quest_token_set_contains(&set, token);
    printf("%s: %s\n", token, found ? "YES" : "NO");
    quest_token_set_free(&set);
    return found ? 0 : 1;
}

static int cmd_acts(const char *path) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    qsort(set.tokens, set.count, sizeof(char *), cmp_str);

    for (int a = 0; a < NUM_ACTS; a++) {
        printf("\n=== %s ===\n", quest_act_name((QuestAct)a));
        int act_count = 0;
        for (int i = 0; i < set.count; i++) {
            if (guess_act(set.tokens[i]) == (QuestAct)a) {
                printf("  %s\n", set.tokens[i]);
                act_count++;
            }
        }
        if (act_count == 0)
            printf("  (none)\n");
        else
            printf("  --- %d tokens\n", act_count);
    }
    quest_token_set_free(&set);
    return 0;
}

static int cmd_quests(const char *path) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }

    int count;
    const QuestDef *defs = quest_get_defs(&count);
    QuestAct last_act = -1;
    bool last_main = true;
    int complete = 0, total = 0;

    for (int i = 0; i < count; i++) {
        if (defs[i].act != last_act) {
            printf("\n=== %s ===\n", quest_act_name(defs[i].act));
            last_act = defs[i].act;
            last_main = true;
        }
        if (defs[i].is_main && !last_main) {
            /* shouldn't happen — mains come first */
        }
        if (!defs[i].is_main && last_main) {
            printf("  --- Side Quests ---\n");
            last_main = false;
        }

        bool done = quest_token_set_contains(&set, defs[i].completion_token);
        /* Count how many of this quest's tokens are present */
        int present = 0, quest_total = 0;
        for (const char **t = defs[i].tokens; *t; t++) {
            quest_total++;
            if (quest_token_set_contains(&set, *t))
                present++;
        }

        printf("  [%s] %-35s (%d/%d tokens)\n",
               done ? "x" : " ", defs[i].name, present, quest_total);
        total++;
        if (done) complete++;
    }

    printf("\n--- %d/%d quests completed\n", complete, total);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_add(const char *path, const char *token) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    if (quest_token_set_contains(&set, token)) {
        printf("Token already present: %s\n", token);
        quest_token_set_free(&set);
        return 0;
    }
    quest_token_set_add(&set, token);
    if (quest_tokens_save(path, &set) != 0) {
        fprintf(stderr, "Error: failed to save %s\n", path);
        quest_token_set_free(&set);
        return 1;
    }
    printf("Added token: %s (now %d tokens)\n", token, set.count);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_remove(const char *path, const char *token) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    if (!quest_token_set_contains(&set, token)) {
        printf("Token not found: %s\n", token);
        quest_token_set_free(&set);
        return 0;
    }
    quest_token_set_remove(&set, token);
    if (quest_tokens_save(path, &set) != 0) {
        fprintf(stderr, "Error: failed to save %s\n", path);
        quest_token_set_free(&set);
        return 1;
    }
    printf("Removed token: %s (now %d tokens)\n", token, set.count);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_complete(const char *path, const char *quest_name) {
    const QuestDef *qd = find_quest_by_name(quest_name);
    if (!qd) {
        fprintf(stderr, "Error: no quest matching '%s'\n", quest_name);
        fprintf(stderr, "Use 'defs' command to list available quest names.\n");
        return 1;
    }

    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }

    int added = 0;
    for (const char **t = qd->tokens; *t; t++) {
        if (!quest_token_set_contains(&set, *t)) {
            quest_token_set_add(&set, *t);
            added++;
        }
    }

    if (added == 0) {
        printf("Quest '%s' already complete (all tokens present)\n", qd->name);
        quest_token_set_free(&set);
        return 0;
    }

    if (quest_tokens_save(path, &set) != 0) {
        fprintf(stderr, "Error: failed to save %s\n", path);
        quest_token_set_free(&set);
        return 1;
    }
    printf("Completed quest '%s': added %d tokens (now %d total)\n",
           qd->name, added, set.count);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_clear(const char *path, const char *quest_name) {
    const QuestDef *qd = find_quest_by_name(quest_name);
    if (!qd) {
        fprintf(stderr, "Error: no quest matching '%s'\n", quest_name);
        fprintf(stderr, "Use 'defs' command to list available quest names.\n");
        return 1;
    }

    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }

    int removed = 0;
    for (const char **t = qd->tokens; *t; t++) {
        if (quest_token_set_contains(&set, *t)) {
            quest_token_set_remove(&set, *t);
            removed++;
        }
    }

    if (removed == 0) {
        printf("Quest '%s' already clear (no tokens present)\n", qd->name);
        quest_token_set_free(&set);
        return 0;
    }

    if (quest_tokens_save(path, &set) != 0) {
        fprintf(stderr, "Error: failed to save %s\n", path);
        quest_token_set_free(&set);
        return 1;
    }
    printf("Cleared quest '%s': removed %d tokens (now %d total)\n",
           qd->name, removed, set.count);
    quest_token_set_free(&set);
    return 0;
}

static int cmd_roundtrip(const char *path) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }
    printf("Loaded %d tokens from %s\n", set.count, path);

    char *tmp = g_build_filename(g_get_tmp_dir(), "tq_quest_roundtrip.myw", NULL);
    if (quest_tokens_save(tmp, &set) != 0) {
        fprintf(stderr, "Error: failed to save to %s\n", tmp);
        quest_token_set_free(&set);
        g_free(tmp);
        return 1;
    }

    /* Byte-compare original and round-tripped file */
    FILE *fa = fopen(path, "rb");
    FILE *fb = fopen(tmp, "rb");
    if (!fa || !fb) {
        fprintf(stderr, "Error: cannot open files for comparison\n");
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        quest_token_set_free(&set);
        g_free(tmp);
        return 1;
    }

    fseek(fa, 0, SEEK_END);
    fseek(fb, 0, SEEK_END);
    long sa = ftell(fa);
    long sb = ftell(fb);
    rewind(fa);
    rewind(fb);

    if (sa != sb) {
        printf("DIFFER: size mismatch (%ld vs %ld bytes)\n", sa, sb);
        fclose(fa); fclose(fb);
        quest_token_set_free(&set);
        g_free(tmp);
        return 1;
    }

    uint8_t *ba = malloc(sa);
    uint8_t *bb = malloc(sb);
    fread(ba, 1, sa, fa);
    fread(bb, 1, sb, fb);
    fclose(fa);
    fclose(fb);

    bool identical = memcmp(ba, bb, sa) == 0;
    free(ba);
    free(bb);

    if (identical) {
        printf("PASS: round-trip produces identical %ld-byte file\n", sa);
    } else {
        printf("FAIL: files differ!\n");
        quest_token_set_free(&set);
        g_free(tmp);
        return 1;
    }

    /* Also verify re-load */
    QuestTokenSet set2;
    if (quest_tokens_load(tmp, &set2) != 0) {
        fprintf(stderr, "Error: failed to reload %s\n", tmp);
        quest_token_set_free(&set);
        g_free(tmp);
        return 1;
    }
    g_free(tmp);
    if (set.count != set2.count) {
        printf("FAIL: token count mismatch (%d vs %d)\n", set.count, set2.count);
        quest_token_set_free(&set);
        quest_token_set_free(&set2);
        return 1;
    }
    for (int i = 0; i < set.count; i++) {
        if (strcmp(set.tokens[i], set2.tokens[i]) != 0) {
            printf("FAIL: token[%d] mismatch: '%s' vs '%s'\n",
                   i, set.tokens[i], set2.tokens[i]);
            quest_token_set_free(&set);
            quest_token_set_free(&set2);
            return 1;
        }
    }
    printf("PASS: re-loaded %d tokens match original order\n", set2.count);

    quest_token_set_free(&set);
    quest_token_set_free(&set2);
    return 0;
}

static int cmd_defs(void) {
    int count;
    const QuestDef *defs = quest_get_defs(&count);
    QuestAct last_act = -1;
    bool last_main = true;

    for (int i = 0; i < count; i++) {
        if (defs[i].act != last_act) {
            printf("\n=== %s ===\n", quest_act_name(defs[i].act));
            last_act = defs[i].act;
            last_main = true;
        }
        if (!defs[i].is_main && last_main) {
            printf("  --- Side Quests ---\n");
            last_main = false;
        }

        int ntokens = 0;
        for (const char **t = defs[i].tokens; *t; t++)
            ntokens++;

        printf("  %-35s [%s] %d tokens  (check: %s)\n",
               defs[i].name,
               defs[i].is_main ? "MAIN" : "SIDE",
               ntokens,
               defs[i].completion_token);
    }
    printf("\n--- %d quest definitions total\n", count);
    return 0;
}

static int cmd_diff(const char *path_a, const char *path_b) {
    QuestTokenSet sa, sb;
    if (quest_tokens_load(path_a, &sa) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path_a);
        return 1;
    }
    if (quest_tokens_load(path_b, &sb) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path_b);
        quest_token_set_free(&sa);
        return 1;
    }

    /* Tokens only in A */
    printf("--- Only in %s (%d tokens) ---\n", path_a, sa.count);
    qsort(sa.tokens, sa.count, sizeof(char *), cmp_str);
    int only_a = 0;
    for (int i = 0; i < sa.count; i++) {
        if (!quest_token_set_contains(&sb, sa.tokens[i])) {
            printf("  - %s\n", sa.tokens[i]);
            only_a++;
        }
    }
    if (only_a == 0) printf("  (none)\n");

    /* Tokens only in B */
    printf("\n--- Only in %s (%d tokens) ---\n", path_b, sb.count);
    qsort(sb.tokens, sb.count, sizeof(char *), cmp_str);
    int only_b = 0;
    for (int i = 0; i < sb.count; i++) {
        if (!quest_token_set_contains(&sa, sb.tokens[i])) {
            printf("  + %s\n", sb.tokens[i]);
            only_b++;
        }
    }
    if (only_b == 0) printf("  (none)\n");

    /* Count shared */
    int shared = 0;
    for (int i = 0; i < sa.count; i++) {
        if (quest_token_set_contains(&sb, sa.tokens[i]))
            shared++;
    }
    printf("\n--- Summary: %d shared, %d only-A, %d only-B\n", shared, only_a, only_b);

    quest_token_set_free(&sa);
    quest_token_set_free(&sb);
    return 0;
}

static int cmd_coverage(const char *path) {
    QuestTokenSet set;
    if (quest_tokens_load(path, &set) != 0) {
        fprintf(stderr, "Error: failed to load %s\n", path);
        return 1;
    }

    int count;
    const QuestDef *defs = quest_get_defs(&count);

    /* Build a set of all tokens covered by quest definitions */
    /* Use a simple array + linear scan since counts are small */
    int total_def_tokens = 0;
    for (int i = 0; i < count; i++)
        for (const char **t = defs[i].tokens; *t; t++)
            total_def_tokens++;

    const char **def_tokens = malloc(total_def_tokens * sizeof(char *));
    int idx = 0;
    for (int i = 0; i < count; i++)
        for (const char **t = defs[i].tokens; *t; t++)
            def_tokens[idx++] = *t;

    /* 1. Orphaned tokens: in file but not in any quest definition */
    printf("=== Orphaned Tokens (in file, not in any quest def) ===\n");
    qsort(set.tokens, set.count, sizeof(char *), cmp_str);
    int orphaned = 0;
    for (int i = 0; i < set.count; i++) {
        bool found = false;
        for (int j = 0; j < total_def_tokens; j++) {
            if (strcmp(set.tokens[i], def_tokens[j]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            printf("  %s\n", set.tokens[i]);
            orphaned++;
        }
    }
    if (orphaned == 0) printf("  (none)\n");
    printf("--- %d orphaned tokens\n\n", orphaned);

    /* 2. Covered tokens: in file and in a quest definition */
    int covered = 0;
    for (int i = 0; i < set.count; i++) {
        for (int j = 0; j < total_def_tokens; j++) {
            if (strcmp(set.tokens[i], def_tokens[j]) == 0) {
                covered++;
                break;
            }
        }
    }
    printf("=== Coverage Summary ===\n");
    printf("  File tokens:    %d\n", set.count);
    printf("  Covered:        %d\n", covered);
    printf("  Orphaned:       %d\n", orphaned);

    /* 3. Uncovered quests: quest defs with zero matching tokens in the file */
    printf("\n=== Uncovered Quests (zero tokens in file) ===\n");
    int uncovered = 0;
    for (int i = 0; i < count; i++) {
        int present = 0;
        for (const char **t = defs[i].tokens; *t; t++) {
            if (quest_token_set_contains(&set, *t))
                present++;
        }
        if (present == 0) {
            printf("  [%s] %s\n", quest_act_name(defs[i].act), defs[i].name);
            uncovered++;
        }
    }
    if (uncovered == 0) printf("  (none)\n");
    printf("--- %d uncovered quests\n", uncovered);

    free(def_tokens);
    quest_token_set_free(&set);
    return 0;
}

/* ── .que file helpers ─────────────────────────────────────────────────── */

/* Read a length-prefixed key from a .que/.myw binary blob.
 * Returns the key string (malloc'd) and advances *off past key + value.
 * Returns NULL on EOF/error. Does NOT skip the value — caller does that. */
static char *que_read_key(const uint8_t *data, size_t len, size_t *off) {
    if (*off + 4 > len) return NULL;
    uint32_t slen;
    memcpy(&slen, data + *off, 4);
    *off += 4;
    if (slen == 0 || *off + slen > len) return NULL;
    char *s = malloc(slen + 1);
    memcpy(s, data + *off, slen);
    s[slen] = '\0';
    *off += slen;
    return s;
}

static uint32_t que_read_u32(const uint8_t *data, size_t *off) {
    uint32_t val;
    memcpy(&val, data + *off, 4);
    *off += 4;
    return val;
}

/* Read file into malloc'd buffer, set *out_size. Returns NULL on error. */
static uint8_t *read_file(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *data = malloc(fsize);
    if (fread(data, 1, fsize, f) != (size_t)fsize) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = fsize;
    return data;
}

static int cmd_dump_que(const char *path) {
    long fsize;
    uint8_t *data = read_file(path, &fsize);
    if (!data) { fprintf(stderr, "Error: cannot read %s\n", path); return 1; }

    int depth = 0;
    int has_fired_count = 0, pending_fire_count = 0;
    int trigger_count = 0, condition_count = 0, action_count = 0;
    size_t off = 0;

    while (off + 4 <= (size_t)fsize) {
        char *key = que_read_key(data, fsize, &off);
        if (!key) break;

        if (strcmp(key, "begin_block") == 0) {
            if (off + 4 <= (size_t)fsize) {
                uint32_t marker = que_read_u32(data, &off);
                printf("%*s{ (0x%08X)\n", depth * 2, "", marker);
            }
            depth++;
            free(key);
            continue;
        }
        if (strcmp(key, "end_block") == 0) {
            depth--;
            if (off + 4 <= (size_t)fsize) off += 4; /* skip marker */
            printf("%*s}\n", depth * 2, "");
            free(key);
            continue;
        }

        /* "comments" has a string value (LP-string) */
        if (strcmp(key, "comments") == 0) {
            if (off + 4 > (size_t)fsize) { free(key); break; }
            uint32_t slen = que_read_u32(data, &off);
            if (slen > 0 && off + slen <= (size_t)fsize) {
                char *s = malloc(slen + 1);
                memcpy(s, data + off, slen);
                s[slen] = '\0';
                printf("%*scomments = \"%s\"\n", depth * 2, "", s);
                free(s);
            } else {
                printf("%*scomments = \"\"\n", depth * 2, "");
            }
            off += slen;
            free(key);
            continue;
        }

        if (off + 4 > (size_t)fsize) { free(key); break; }
        uint32_t val = que_read_u32(data, &off);

        printf("%*s%s = %u", depth * 2, "", key, val);

        if (strcmp(key, "hasFired") == 0) {
            has_fired_count++;
            trigger_count++;
        } else if (strcmp(key, "isPendingFire") == 0) {
            pending_fire_count++;
        } else if (strcmp(key, "conditionCount") == 0) {
            condition_count += val;
        } else if (strcmp(key, "actionCount") == 0) {
            action_count += val;
        } else if (strcmp(key, "crcFile") == 0) {
            printf(" (0x%08X)", val);
        }
        printf("\n");
        free(key);
    }

    printf("\n--- Summary: %d triggers, %d hasFired, %d isPendingFire, "
           "%d conditions, %d actions\n",
           trigger_count, has_fired_count, pending_fire_count,
           condition_count, action_count);
    printf("--- File: %s (%ld bytes)\n", path, fsize);
    free(data);
    return 0;
}

/* Format 4 MD5 chunks as a hex filename string (e.g. "0273f539854b76b9...").
 * Each chunk is stored as a LE u32, but .que filenames use BE byte order per chunk. */
static void md5_chunks_to_hex(const uint32_t chunks[4], char hex[33]) {
    for (int i = 0; i < 4; i++) {
        uint8_t b[4];
        memcpy(b, &chunks[i], 4);
        /* Reverse byte order within each chunk (LE→BE) */
        sprintf(hex + i * 8, "%02x%02x%02x%02x", b[3], b[2], b[1], b[0]);
    }
    hex[32] = '\0';
}

/* Read a Quest.myw LP-key and skip it. Returns true if it matches expected. */
static bool qmyw_expect_key(const uint8_t *data, size_t len, size_t *off, const char *expected) {
    if (*off + 4 > len) return false;
    uint32_t slen;
    memcpy(&slen, data + *off, 4);
    *off += 4;
    if (*off + slen > len) return false;
    bool match = (slen == strlen(expected) && memcmp(data + *off, expected, slen) == 0);
    *off += slen;
    return match;
}

static int cmd_dump_quest_myw(const char *path) {
    long fsize;
    uint8_t *data = read_file(path, &fsize);
    if (!data) { fprintf(stderr, "Error: cannot read %s\n", path); return 1; }

    size_t off = 0;

    /* --- Triggers section --- */
    if (!qmyw_expect_key(data, fsize, &off, "begin_block")) {
        fprintf(stderr, "Error: bad format (expected begin_block)\n");
        free(data); return 1;
    }
    off += 4; /* skip marker */

    if (!qmyw_expect_key(data, fsize, &off, "numberOfTriggers")) {
        fprintf(stderr, "Error: bad format (expected numberOfTriggers)\n");
        free(data); return 1;
    }
    uint32_t num_triggers = que_read_u32(data, &off);
    printf("=== Triggers (%u entries) ===\n", num_triggers);

    /* Track unique MD5 hashes */
    char (*md5s)[33] = malloc(num_triggers * sizeof(*md5s));
    int unique_md5_count = 0;

    for (uint32_t i = 0; i < num_triggers && off + 4 <= (size_t)fsize; i++) {
        /* questName — just a key with no value (next key follows immediately) */
        qmyw_expect_key(data, fsize, &off, "questName");

        /* md5ChunkCount */
        qmyw_expect_key(data, fsize, &off, "md5ChunkCount");
        uint32_t chunk_count = que_read_u32(data, &off);

        uint32_t chunks[4] = {0};
        for (uint32_t c = 0; c < chunk_count && c < 4; c++) {
            qmyw_expect_key(data, fsize, &off, "md5Chunk");
            chunks[c] = que_read_u32(data, &off);
        }

        char hex[33];
        md5_chunks_to_hex(chunks, hex);

        /* stepIdx, triggerIdx, target */
        qmyw_expect_key(data, fsize, &off, "stepIdx");
        uint32_t step_idx = que_read_u32(data, &off);
        qmyw_expect_key(data, fsize, &off, "triggerIdx");
        uint32_t trigger_idx = que_read_u32(data, &off);
        qmyw_expect_key(data, fsize, &off, "target");
        uint32_t target = que_read_u32(data, &off);

        printf("  [%4u] %s.que  step=%u trig=%u target=%u\n",
               i, hex, step_idx, trigger_idx, target);

        /* Track unique MD5s */
        bool found = false;
        for (int j = 0; j < unique_md5_count; j++) {
            if (strcmp(md5s[j], hex) == 0) { found = true; break; }
        }
        if (!found) {
            memcpy(md5s[unique_md5_count], hex, 33);
            unique_md5_count++;
        }
    }

    /* end_block for triggers */
    qmyw_expect_key(data, fsize, &off, "end_block");
    off += 4; /* skip marker */

    printf("\n--- %u triggers, %d unique .que files\n\n", num_triggers, unique_md5_count);

    /* --- Rewards section --- */
    if (!qmyw_expect_key(data, fsize, &off, "begin_block")) {
        printf("(No rewards section found)\n");
        free(md5s); free(data);
        return 0;
    }
    off += 4; /* skip marker */

    if (!qmyw_expect_key(data, fsize, &off, "numRewards")) {
        printf("(Expected numRewards)\n");
        free(md5s); free(data);
        return 0;
    }
    uint32_t num_rewards = que_read_u32(data, &off);
    printf("=== Rewards (%u entries) ===\n", num_rewards);

    for (uint32_t i = 0; i < num_rewards && off + 4 <= (size_t)fsize; i++) {
        qmyw_expect_key(data, fsize, &off, "questName");

        qmyw_expect_key(data, fsize, &off, "md5ChunkCount");
        uint32_t chunk_count = que_read_u32(data, &off);
        uint32_t chunks[4] = {0};
        for (uint32_t c = 0; c < chunk_count && c < 4; c++) {
            qmyw_expect_key(data, fsize, &off, "md5Chunk");
            chunks[c] = que_read_u32(data, &off);
        }
        char hex[33];
        md5_chunks_to_hex(chunks, hex);

        /* region */
        qmyw_expect_key(data, fsize, &off, "region");
        uint32_t region = que_read_u32(data, &off);

        /* locationTag (LP-string) */
        qmyw_expect_key(data, fsize, &off, "locationTag");
        if (off + 4 > (size_t)fsize) break;
        uint32_t loc_len = que_read_u32(data, &off);
        char *loc_tag = NULL;
        if (loc_len > 0 && off + loc_len <= (size_t)fsize) {
            loc_tag = malloc(loc_len + 1);
            memcpy(loc_tag, data + off, loc_len);
            loc_tag[loc_len] = '\0';
        }
        off += loc_len;

        /* titleTag (LP-string) */
        qmyw_expect_key(data, fsize, &off, "titleTag");
        if (off + 4 > (size_t)fsize) { free(loc_tag); break; }
        uint32_t title_len = que_read_u32(data, &off);
        char *title_tag = NULL;
        if (title_len > 0 && off + title_len <= (size_t)fsize) {
            title_tag = malloc(title_len + 1);
            memcpy(title_tag, data + off, title_len);
            title_tag[title_len] = '\0';
        }
        off += title_len;

        /* text (UTF-16LE string — LP value is CHARACTER count, bytes = count * 2) */
        qmyw_expect_key(data, fsize, &off, "text");
        if (off + 4 > (size_t)fsize) { free(loc_tag); free(title_tag); break; }
        uint32_t text_chars = que_read_u32(data, &off);
        uint32_t text_bytes = text_chars * 2;
        /* Decode UTF-16LE to ASCII for display */
        char *text_str = NULL;
        if (text_bytes > 0 && off + text_bytes <= (size_t)fsize) {
            text_str = malloc(text_chars + 1);
            for (uint32_t c = 0; c < text_chars; c++) {
                uint16_t ch;
                memcpy(&ch, data + off + c * 2, 2);
                text_str[c] = (ch < 128) ? (char)ch : '?';
            }
            text_str[text_chars] = '\0';
        }
        off += text_bytes;

        const char *region_name = "?";
        switch (region) {
        case 1: region_name = "Greece"; break;
        case 2: region_name = "Egypt"; break;
        case 3: region_name = "Orient"; break;
        case 4: region_name = "Hades"; break;
        case 5: region_name = "Ragnarok"; break;
        case 6: region_name = "Atlantis"; break;
        case 7: region_name = "EternalEmbers"; break;
        }

        printf("  [%3u] %s.que  region=%u(%s)  loc=\"%s\"  title=\"%s\"  text=\"%s\"\n",
               i, hex, region, region_name,
               loc_tag ? loc_tag : "", title_tag ? title_tag : "",
               text_str ? text_str : "");

        free(loc_tag);
        free(title_tag);
        free(text_str);
    }

    printf("\n--- %u triggers, %d unique .que files, %u rewards\n",
           num_triggers, unique_md5_count, num_rewards);
    printf("--- File: %s (%ld bytes)\n", path, fsize);
    free(md5s);
    free(data);
    return 0;
}

static int cmd_clear_que(const char *dir) {
    int result = quest_que_clear_all(dir);
    if (result < 0) {
        fprintf(stderr, "Error: failed to clear .que files in %s\n", dir);
        return 1;
    }
    printf("Modified %d .que files in %s\n", result, dir);

    /* Also show Quest.myw clearing */
    if (quest_myw_clear(dir) == 0)
        printf("Wrote empty Quest.myw\n");
    else
        fprintf(stderr, "Warning: failed to clear Quest.myw\n");

    return 0;
}

static int cmd_compare_que(const char *dir_a, const char *dir_b) {
    GDir *d = g_dir_open(dir_a, 0, NULL);
    if (!d) { fprintf(stderr, "Error: cannot open %s\n", dir_a); return 1; }

    int files_compared = 0, files_differ = 0;
    const gchar *ent_name;
    while ((ent_name = g_dir_read_name(d)) != NULL) {
        size_t nlen = strlen(ent_name);
        if (nlen < 5 || strcmp(ent_name + nlen - 4, ".que") != 0)
            continue;

        char *path_a = g_build_filename(dir_a, ent_name, NULL);
        char *path_b = g_build_filename(dir_b, ent_name, NULL);

        FILE *fa = fopen(path_a, "rb");
        FILE *fb = fopen(path_b, "rb");
        if (!fa || !fb) {
            if (fa) { printf("  %s: only in dir_a\n", ent_name); fclose(fa); }
            else if (fb) { printf("  %s: only in dir_b\n", ent_name); fclose(fb); }
            g_free(path_a); g_free(path_b);
            continue;
        }
        g_free(path_a); g_free(path_b);

        fseek(fa, 0, SEEK_END); long sa = ftell(fa); rewind(fa);
        fseek(fb, 0, SEEK_END); long sb = ftell(fb); rewind(fb);

        uint8_t *da = malloc(sa);
        uint8_t *db = malloc(sb);
        fread(da, 1, sa, fa);
        fread(db, 1, sb, fb);
        fclose(fa); fclose(fb);

        if (sa != sb) {
            printf("  %s: size differs (%ld vs %ld)\n", ent_name, sa, sb);
            files_differ++;
        } else {
            bool differ = false;
            static const struct { const char *key; size_t klen; } targets[] = {
                { "hasFired", 8 }, { "isPendingFire", 13 },
            };

            for (int t = 0; t < 2; t++) {
                const char *key = targets[t].key;
                size_t klen = targets[t].klen;
                size_t oa = 0, ob = 0;
                int idx = 0;

                while (1) {
                    size_t pa = (size_t)-1, pb = (size_t)-1;
                    for (size_t i = oa; i + 4 + klen + 4 <= (size_t)sa; i++) {
                        uint32_t slen;
                        memcpy(&slen, da + i, 4);
                        if (slen == (uint32_t)klen && memcmp(da + i + 4, key, klen) == 0) {
                            pa = i; break;
                        }
                    }
                    for (size_t i = ob; i + 4 + klen + 4 <= (size_t)sb; i++) {
                        uint32_t slen;
                        memcpy(&slen, db + i, 4);
                        if (slen == (uint32_t)klen && memcmp(db + i + 4, key, klen) == 0) {
                            pb = i; break;
                        }
                    }
                    if (pa == (size_t)-1 || pb == (size_t)-1) break;

                    uint32_t va, vb;
                    memcpy(&va, da + pa + 4 + klen, 4);
                    memcpy(&vb, db + pb + 4 + klen, 4);
                    if (va != vb) {
                        if (!differ)
                            printf("  %s:\n", ent_name);
                        printf("    %s[%d]: %u vs %u\n", key, idx, va, vb);
                        differ = true;
                    }
                    oa = pa + 4 + klen + 4;
                    ob = pb + 4 + klen + 4;
                    idx++;
                }
            }
            if (differ) files_differ++;
        }
        files_compared++;
        free(da); free(db);
    }
    g_dir_close(d);

    printf("\n--- %d files compared, %d differ\n", files_compared, files_differ);
    return 0;
}

/* ── que-info: categorize all .que files in a directory ────────────────── */

/* Extract embedded info from a .que file: comments strings, flag counts.
 * Searches for LP-string patterns that look like .qst file paths. */
static int cmd_que_info(const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) { fprintf(stderr, "Error: cannot open %s\n", dir); return 1; }

    struct que_entry {
        char filename[40];
        int has_fired_total;
        int has_fired_set;
        int pending_total;
        int pending_set;
        int trigger_count;
        char embedded_path[256];
        long filesize;
    };

    struct que_entry *entries = NULL;
    int nentries = 0, cap = 0;

    const gchar *ent_name;
    while ((ent_name = g_dir_read_name(d)) != NULL) {
        size_t nlen = strlen(ent_name);
        if (nlen < 5 || strcmp(ent_name + nlen - 4, ".que") != 0) continue;

        char *filepath = g_build_filename(dir, ent_name, NULL);

        long fsize;
        uint8_t *data = read_file(filepath, &fsize);
        g_free(filepath);
        if (!data) continue;

        if (nentries >= cap) {
            cap = cap ? cap * 2 : 256;
            entries = realloc(entries, cap * sizeof(*entries));
        }
        struct que_entry *e = &entries[nentries++];
        memset(e, 0, sizeof(*e));
        snprintf(e->filename, sizeof(e->filename), "%.*s", (int)(nlen - 4), ent_name);
        e->filesize = fsize;

        /* Scan for keys */
        for (size_t off = 0; off + 8 <= (size_t)fsize; ) {
            uint32_t slen;
            memcpy(&slen, data + off, 4);
            if (slen == 0 || slen > 256 || off + 4 + slen > (size_t)fsize) {
                off++;
                continue;
            }

            if (slen == 8 && memcmp(data + off + 4, "hasFired", 8) == 0) {
                e->has_fired_total++;
                if (off + 4 + 8 + 4 <= (size_t)fsize) {
                    uint32_t val;
                    memcpy(&val, data + off + 4 + 8, 4);
                    if (val) e->has_fired_set++;
                }
                off += 4 + 8 + 4;
            } else if (slen == 13 && memcmp(data + off + 4, "isPendingFire", 13) == 0) {
                e->pending_total++;
                if (off + 4 + 13 + 4 <= (size_t)fsize) {
                    uint32_t val;
                    memcpy(&val, data + off + 4 + 13, 4);
                    if (val) e->pending_set++;
                }
                off += 4 + 13 + 4;
            } else if (slen == 8 && memcmp(data + off + 4, "comments", 8) == 0) {
                size_t coff = off + 4 + 8;
                if (coff + 4 <= (size_t)fsize) {
                    uint32_t clen;
                    memcpy(&clen, data + coff, 4);
                    coff += 4;
                    if (clen > 0 && coff + clen <= (size_t)fsize && !e->embedded_path[0]) {
                        /* Check if comment contains .qst path */
                        char *tmp = malloc(clen + 1);
                        memcpy(tmp, data + coff, clen);
                        tmp[clen] = '\0';
                        if (strstr(tmp, ".qst") || strstr(tmp, ".QST")) {
                            snprintf(e->embedded_path, sizeof(e->embedded_path), "%s", tmp);
                        }
                        free(tmp);
                    }
                    off = coff + clen;
                } else {
                    off++;
                }
            } else {
                off++;
            }
        }

        /* Count triggers (each hasFired = one trigger) */
        e->trigger_count = e->has_fired_total;
        free(data);
    }
    g_dir_close(d);

    /* Sort by filename */
    for (int i = 0; i < nentries - 1; i++)
        for (int j = i + 1; j < nentries; j++)
            if (strcmp(entries[i].filename, entries[j].filename) > 0) {
                struct que_entry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }

    /* Print results */
    int with_path = 0, with_fired = 0, total_fired = 0, total_pending = 0;
    printf("=== .que File Analysis (%d files) ===\n\n", nentries);

    printf("%-34s %5s %8s %10s %s\n",
           "MD5 Hash", "Trigs", "Fired", "Pending", "Embedded Path");
    printf("%-34s %5s %8s %10s %s\n",
           "──────────────────────────────────", "─────", "────────", "──────────",
           "──────────────");

    for (int i = 0; i < nentries; i++) {
        struct que_entry *e = &entries[i];
        printf("%-34s %5d %4d/%-3d %5d/%-4d %s\n",
               e->filename,
               e->trigger_count,
               e->has_fired_set, e->has_fired_total,
               e->pending_set, e->pending_total,
               e->embedded_path[0] ? e->embedded_path : "-");
        if (e->embedded_path[0]) with_path++;
        if (e->has_fired_set > 0) with_fired++;
        total_fired += e->has_fired_set;
        total_pending += e->pending_set;
    }

    printf("\n--- Summary ---\n");
    printf("  Total .que files:     %d\n", nentries);
    printf("  With embedded paths:  %d\n", with_path);
    printf("  With fired triggers:  %d\n", with_fired);
    printf("  Total fired flags:    %d\n", total_fired);
    printf("  Total pending flags:  %d\n", total_pending);

    free(entries);
    return 0;
}

/* ── scan: full character quest state overview ────────────────────────── */

static int cmd_scan(const char *save_dir) {
    static const char *diffs[] = { "Normal", "Epic", "Legendary" };
    static const char *map_subdir = "Levels_World_World01.map";

    char *test_path = g_build_filename(save_dir, map_subdir, NULL);
    bool has_map_dir = g_file_test(test_path, G_FILE_TEST_EXISTS);
    g_free(test_path);

    if (!has_map_dir) {
        fprintf(stderr, "Error: %s does not contain %s/\n", save_dir, map_subdir);
        fprintf(stderr, "Expected a character save directory (e.g. testdata/saves/_soothie/)\n");
        return 1;
    }

    printf("=== Character Quest State Scan ===\n");
    printf("Directory: %s\n\n", save_dir);

    for (int di = 0; di < 3; di++) {
        char *diff_dir = g_build_filename(save_dir, map_subdir, diffs[di], NULL);

        if (!g_file_test(diff_dir, G_FILE_TEST_EXISTS)) {
            printf("--- %s: (not present)\n\n", diffs[di]);
            g_free(diff_dir);
            continue;
        }

        printf("=== %s ===\n", diffs[di]);

        char *qt_path = g_build_filename(diff_dir, "QuestToken.myw", NULL);
        if (g_file_test(qt_path, G_FILE_TEST_EXISTS)) {
            QuestTokenSet set;
            if (quest_tokens_load(qt_path, &set) == 0) {
                int qcount;
                const QuestDef *qdefs = quest_get_defs(&qcount);
                int complete = 0;
                for (int i = 0; i < qcount; i++) {
                    if (quest_token_set_contains(&set, qdefs[i].completion_token))
                        complete++;
                }

                int act_complete[NUM_ACTS] = {0};
                int act_total[NUM_ACTS] = {0};
                for (int i = 0; i < qcount; i++) {
                    act_total[qdefs[i].act]++;
                    if (quest_token_set_contains(&set, qdefs[i].completion_token))
                        act_complete[qdefs[i].act]++;
                }

                printf("  QuestToken.myw: %d tokens, %d/%d quests complete\n",
                       set.count, complete, qcount);
                printf("    Per act:");
                for (int a = 0; a < NUM_ACTS; a++) {
                    printf(" %s=%d/%d", quest_act_name((QuestAct)a),
                           act_complete[a], act_total[a]);
                }
                printf("\n");

                quest_token_set_free(&set);
            } else {
                printf("  QuestToken.myw: (parse error)\n");
            }
        } else {
            printf("  QuestToken.myw: (not present)\n");
        }
        g_free(qt_path);

        char *qm_path = g_build_filename(diff_dir, "Quest.myw", NULL);
        if (g_file_test(qm_path, G_FILE_TEST_EXISTS)) {
            long fsize;
            uint8_t *data = read_file(qm_path, &fsize);
            if (data) {
                size_t off = 0;
                qmyw_expect_key(data, fsize, &off, "begin_block");
                off += 4;
                qmyw_expect_key(data, fsize, &off, "numberOfTriggers");
                uint32_t num_trig = que_read_u32(data, &off);

                uint32_t num_rew = 0;
                const char *nr_key = "numRewards";
                size_t nr_len = strlen(nr_key);
                for (size_t i = 0; i + 4 + nr_len + 4 <= (size_t)fsize; i++) {
                    uint32_t slen;
                    memcpy(&slen, data + i, 4);
                    if (slen == (uint32_t)nr_len && memcmp(data + i + 4, nr_key, nr_len) == 0) {
                        memcpy(&num_rew, data + i + 4 + nr_len, 4);
                        break;
                    }
                }

                printf("  Quest.myw: %u triggers, %u rewards (%ld bytes)\n",
                       num_trig, num_rew, fsize);
                free(data);
            }
        } else {
            printf("  Quest.myw: (not present)\n");
        }
        g_free(qm_path);

        GDir *dd = g_dir_open(diff_dir, 0, NULL);
        if (dd) {
            int que_count = 0, que_fired = 0;
            long total_bytes = 0;
            const gchar *ent_name;
            while ((ent_name = g_dir_read_name(dd)) != NULL) {
                size_t nlen = strlen(ent_name);
                if (nlen < 5 || strcmp(ent_name + nlen - 4, ".que") != 0) continue;
                que_count++;

                char *fpath = g_build_filename(diff_dir, ent_name, NULL);
                long fsz;
                uint8_t *fdata = read_file(fpath, &fsz);
                g_free(fpath);
                if (!fdata) continue;
                total_bytes += fsz;

                bool any_fired = false;
                for (size_t off = 0; off + 16 <= (size_t)fsz; off++) {
                    uint32_t slen;
                    memcpy(&slen, fdata + off, 4);
                    if (slen == 8 && memcmp(fdata + off + 4, "hasFired", 8) == 0) {
                        uint32_t val;
                        memcpy(&val, fdata + off + 12, 4);
                        if (val) { any_fired = true; break; }
                    }
                }
                if (any_fired) que_fired++;
                free(fdata);
            }
            g_dir_close(dd);
            printf("  .que files: %d total, %d with fired triggers (%ld KB)\n",
                   que_count, que_fired, total_bytes / 1024);
        }
        printf("\n");
        g_free(diff_dir);
    }

    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (strcmp(cmd, "dump") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s dump <myw>\n", argv[0]); return 1; }
        return cmd_dump(argv[2]);
    }
    if (strcmp(cmd, "count") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s count <myw>\n", argv[0]); return 1; }
        return cmd_count(argv[2]);
    }
    if (strcmp(cmd, "search") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s search <myw> <pattern>\n", argv[0]); return 1; }
        return cmd_search(argv[2], argv[3]);
    }
    if (strcmp(cmd, "has") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s has <myw> <token>\n", argv[0]); return 1; }
        return cmd_has(argv[2], argv[3]);
    }
    if (strcmp(cmd, "acts") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s acts <myw>\n", argv[0]); return 1; }
        return cmd_acts(argv[2]);
    }
    if (strcmp(cmd, "quests") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s quests <myw>\n", argv[0]); return 1; }
        return cmd_quests(argv[2]);
    }
    if (strcmp(cmd, "add") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s add <myw> <token>\n", argv[0]); return 1; }
        return cmd_add(argv[2], argv[3]);
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s remove <myw> <token>\n", argv[0]); return 1; }
        return cmd_remove(argv[2], argv[3]);
    }
    if (strcmp(cmd, "complete") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s complete <myw> <quest_name>\n", argv[0]); return 1; }
        return cmd_complete(argv[2], argv[3]);
    }
    if (strcmp(cmd, "clear") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s clear <myw> <quest_name>\n", argv[0]); return 1; }
        return cmd_clear(argv[2], argv[3]);
    }
    if (strcmp(cmd, "roundtrip") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s roundtrip <myw>\n", argv[0]); return 1; }
        return cmd_roundtrip(argv[2]);
    }
    if (strcmp(cmd, "defs") == 0) {
        return cmd_defs();
    }
    if (strcmp(cmd, "diff") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s diff <myw_a> <myw_b>\n", argv[0]); return 1; }
        return cmd_diff(argv[2], argv[3]);
    }
    if (strcmp(cmd, "coverage") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s coverage <myw>\n", argv[0]); return 1; }
        return cmd_coverage(argv[2]);
    }

    if (strcmp(cmd, "dump-que") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s dump-que <file>\n", argv[0]); return 1; }
        return cmd_dump_que(argv[2]);
    }
    if (strcmp(cmd, "dump-quest-myw") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s dump-quest-myw <file>\n", argv[0]); return 1; }
        return cmd_dump_quest_myw(argv[2]);
    }
    if (strcmp(cmd, "clear-que") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s clear-que <dir>\n", argv[0]); return 1; }
        return cmd_clear_que(argv[2]);
    }
    if (strcmp(cmd, "compare-que") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s compare-que <dir_a> <dir_b>\n", argv[0]); return 1; }
        return cmd_compare_que(argv[2], argv[3]);
    }
    if (strcmp(cmd, "que-info") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s que-info <dir>\n", argv[0]); return 1; }
        return cmd_que_info(argv[2]);
    }
    if (strcmp(cmd, "scan") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s scan <save_dir>\n", argv[0]); return 1; }
        return cmd_scan(argv[2]);
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage(argv[0]);
    return 1;
}
