#ifndef QUEST_TOKENS_H
#define QUEST_TOKENS_H

#include <stdbool.h>
#include <stdint.h>

/* ── Quest enums ──────────────────────────────────────────────────────── */

typedef enum {
    ACT_GREECE = 0, ACT_EGYPT, ACT_ORIENT, ACT_IMMORTAL_THRONE,
    ACT_RAGNAROK, ACT_ATLANTIS, ACT_ETERNAL_EMBERS, NUM_ACTS
} QuestAct;

typedef enum {
    DIFF_NORMAL = 0, DIFF_EPIC, DIFF_LEGENDARY, NUM_DIFFICULTIES
} QuestDifficulty;

/* ── Quest definition table ───────────────────────────────────────────── */

typedef struct {
    const char *name;              /* Human-readable quest name */
    const char *area;              /* Region/area header (e.g. "Helos", "Sparta"); NULL for main quests */
    QuestAct act;
    bool is_main;                  /* Main quest vs side quest */
    const char **tokens;           /* NULL-terminated array of all tokens for this quest */
    const char *completion_token;  /* Token that means "quest done" (for checkbox state) */
} QuestDef;

/* ── Token set (dynamic array of token names) ─────────────────────────── */

typedef struct {
    char **tokens;   /* Dynamic array of token names (strdup'd) */
    int count;
    int capacity;
    bool dirty;
} QuestTokenSet;

/* ── Parser/writer ────────────────────────────────────────────────────── */

int quest_tokens_load(const char *filepath, QuestTokenSet *out);
int quest_tokens_save(const char *filepath, const QuestTokenSet *set);

/* ── Token set operations ─────────────────────────────────────────────── */

void quest_token_set_init(QuestTokenSet *set);
void quest_token_set_free(QuestTokenSet *set);
bool quest_token_set_contains(const QuestTokenSet *set, const char *token);
void quest_token_set_add(QuestTokenSet *set, const char *token);
void quest_token_set_remove(QuestTokenSet *set, const char *token);

/* ── Path helpers ─────────────────────────────────────────────────────── */

/* Returns malloc'd path to QuestToken.myw for a given character + difficulty.
 * char_filepath is the path to Player.chr (e.g. .../SaveData/Main/_soothie/Player.chr).
 * Caller must free the returned string. Returns NULL on error. */
char *quest_token_path(const char *char_filepath, QuestDifficulty diff);

/* Returns malloc'd path to the quest state directory for a given character + difficulty.
 * e.g. .../SaveData/Main/_soothie/Levels_World_World01.map/Legendary/
 * Caller must free the returned string. Returns NULL on error. */
char *quest_state_dir(const char *char_filepath, QuestDifficulty diff);

/* ── Quest state file operations ─────────────────────────────────────── */

/* Backup a file to <filepath>.bak. Returns 0 on success, -1 on error.
 * If the source file doesn't exist, returns 0 (nothing to backup). */
int quest_backup_file(const char *filepath);

/* Zero all hasFired/isPendingFire flags in all .que files in quest_dir.
 * Returns number of modified files, or -1 on error. */
int quest_que_clear_all(const char *quest_dir);

/* Write a minimal empty Quest.myw to quest_dir (backs up existing).
 * Returns 0 on success, -1 on error. */
int quest_myw_clear(const char *quest_dir);

/* Copy all .que files + Quest.myw from src_dir to dst_dir (backs up dst files).
 * Returns 0 on success, -1 on error. */
int quest_copy_state_from(const char *src_dir, const char *dst_dir);

/* ── Quest definition table access ────────────────────────────────────── */

const QuestDef *quest_get_defs(int *count_out);
const char *quest_act_name(QuestAct act);
const char *quest_difficulty_name(QuestDifficulty diff);

/* ── Checklist extras (non-quest achievements) ────────────────────────── */

typedef enum {
    CHECK_CAT_BOSS_CHEST,     /* Boss loot chests */
    CHECK_CAT_EXPLORATION,    /* Map unlocks, bags, portals, difficulty */
    CHECK_CAT_NPC,            /* NPC knowledge/conversation flags */
    CHECK_CAT_MISC,           /* Scripted sequences, dungeon keys, misc */
    NUM_CHECK_CATEGORIES
} ChecklistCategory;

typedef struct {
    const char *name;              /* Display name (in-game verbiage) */
    const char *token;             /* Token for Normal difficulty */
    const char *token_epic;        /* Token for Epic/Legendary, or NULL if same */
    ChecklistCategory category;
    QuestAct act;                  /* Which act this belongs to (-1 for global) */
} ChecklistExtraDef;

const ChecklistExtraDef *checklist_get_extras(int *count_out);
const char *checklist_category_name(ChecklistCategory cat);

#endif
