/*
 * tq_quest_tool.c — QuestToken.myw inspection and manipulation tool
 *
 * Works directly against QuestToken.myw files for development, debugging,
 * and testing the quest management feature.
 *
 * Usage:
 *   tq-quest-tool <command> [options]
 *
 * Commands:
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include "../quest_tokens.h"

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
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
        "Examples:\n"
        "  %s dump testdata/_soothie/Levels_World_World01.map/Legendary/QuestToken.myw\n"
        "  %s search testdata/_soothie/.../QuestToken.myw x4MQ\n"
        "  %s quests testdata/_soothie/.../QuestToken.myw\n"
        "  %s defs\n"
        "  %s roundtrip testdata/_soothie/.../QuestToken.myw\n",
        prog, prog, prog, prog, prog, prog);
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

    const char *tmp = "/tmp/tq_quest_roundtrip.myw";
    if (quest_tokens_save(tmp, &set) != 0) {
        fprintf(stderr, "Error: failed to save to %s\n", tmp);
        quest_token_set_free(&set);
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
        return 1;
    }

    /* Also verify re-load */
    QuestTokenSet set2;
    if (quest_tokens_load(tmp, &set2) != 0) {
        fprintf(stderr, "Error: failed to reload %s\n", tmp);
        quest_token_set_free(&set);
        return 1;
    }
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

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

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

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage(argv[0]);
    return 1;
}
