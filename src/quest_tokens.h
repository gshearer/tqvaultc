#ifndef QUEST_TOKENS_H
#define QUEST_TOKENS_H

#include <stdbool.h>
#include <stdint.h>

// ── Quest enums ──────────────────────────────────────────────────────────

typedef enum {
    ACT_GREECE = 0, ACT_EGYPT, ACT_ORIENT, ACT_IMMORTAL_THRONE,
    ACT_RAGNAROK, ACT_ATLANTIS, ACT_ETERNAL_EMBERS, NUM_ACTS
} QuestAct;

typedef enum {
    DIFF_NORMAL = 0, DIFF_EPIC, DIFF_LEGENDARY, NUM_DIFFICULTIES
} QuestDifficulty;

// ── Quest definition table ───────────────────────────────────────────────

typedef struct {
    const char *name;              // Human-readable quest name
    const char *area;              // Region/area header (e.g. "Helos", "Sparta"); NULL for main quests
    QuestAct act;
    bool is_main;                  // Main quest vs side quest
    const char *const *tokens;     // NULL-terminated array of all tokens for this quest
    const char *completion_token;  // Token that means "quest done" (for checkbox state)
} QuestDef;

// ── Token set (dynamic array of token names) ─────────────────────────────

typedef struct {
    char **tokens;   // Dynamic array of token names (strdup'd)
    int count;
    int capacity;
    bool dirty;
} QuestTokenSet;

// ── Parser/writer ────────────────────────────────────────────────────────

// Load quest tokens from a QuestToken.myw file.
// filepath: path to the .myw file.
// out: token set to populate (must be initialized).
// Returns: 0 on success, -1 on error.
int
quest_tokens_load(const char *filepath, QuestTokenSet *out);

// Save quest tokens to a QuestToken.myw file.
// filepath: path to the .myw file.
// set: token set to write.
// Returns: 0 on success, -1 on error.
int
quest_tokens_save(const char *filepath, const QuestTokenSet *set);

// ── Token set operations ─────────────────────────────────────────────────

// Initialize a token set to empty state.
// set: the token set to initialize.
void
quest_token_set_init(QuestTokenSet *set);

// Free all memory in a token set.
// set: the token set to free.
void
quest_token_set_free(QuestTokenSet *set);

// Check if a token set contains a specific token.
// set: the token set to search.
// token: the token name to look for.
// Returns: true if the token is present.
bool
quest_token_set_contains(const QuestTokenSet *set, const char *token);

// Add a token to the set (no-op if already present).
// set: the token set to modify.
// token: the token name to add (will be strdup'd).
void
quest_token_set_add(QuestTokenSet *set, const char *token);

// Remove a token from the set (no-op if not present).
// set: the token set to modify.
// token: the token name to remove.
void
quest_token_set_remove(QuestTokenSet *set, const char *token);

// ── Path helpers ─────────────────────────────────────────────────────────

// Returns malloc'd path to QuestToken.myw for a given character + difficulty.
// char_filepath: path to Player.chr (e.g. .../SaveData/Main/_soothie/Player.chr).
// diff: which difficulty level.
// Returns: allocated path string, or NULL on error. Caller must free.
char *
quest_token_path(const char *char_filepath, QuestDifficulty diff);

// Returns malloc'd path to the quest state directory for a given character + difficulty.
// e.g. .../SaveData/Main/_soothie/Levels_World_World01.map/Legendary/
// char_filepath: path to Player.chr.
// diff: which difficulty level.
// Returns: allocated path string, or NULL on error. Caller must free.
char *
quest_state_dir(const char *char_filepath, QuestDifficulty diff);

// ── Quest state file operations ──────────────────────────────────────────

// Backup a file to <filepath>.bak.
// filepath: path to the file to back up.
// Returns: 0 on success, -1 on error. If file doesn't exist, returns 0.
int
quest_backup_file(const char *filepath);

// Zero all hasFired/isPendingFire flags in all .que files in quest_dir.
// quest_dir: path to the quest state directory.
// Returns: number of modified files, or -1 on error.
int
quest_que_clear_all(const char *quest_dir);

// Write a minimal empty Quest.myw to quest_dir (backs up existing).
// quest_dir: path to the quest state directory.
// Returns: 0 on success, -1 on error.
int
quest_myw_clear(const char *quest_dir);

// Copy all .que files + Quest.myw from src_dir to dst_dir (backs up dst files).
// src_dir: source quest state directory.
// dst_dir: destination quest state directory.
// Returns: 0 on success, -1 on error.
int
quest_copy_state_from(const char *src_dir, const char *dst_dir);

// ── Quest definition table access ────────────────────────────────────────

// Get the global quest definition table.
// count_out: receives the number of entries.
// Returns: pointer to the quest definitions array.
const QuestDef *
quest_get_defs(int *count_out);

// Get the display name for a quest act.
// act: the act enum value.
// Returns: static string with the act name.
const char *
quest_act_name(QuestAct act);

// Get the display name for a difficulty level.
// diff: the difficulty enum value.
// Returns: static string with the difficulty name.
const char *
quest_difficulty_name(QuestDifficulty diff);

// ── Checklist extras (non-quest achievements) ────────────────────────────

typedef enum {
    CHECK_CAT_BOSS_CHEST,     // Boss loot chests
    CHECK_CAT_EXPLORATION,    // Map unlocks, bags, portals, difficulty
    CHECK_CAT_NPC,            // NPC knowledge/conversation flags
    CHECK_CAT_MISC,           // Scripted sequences, dungeon keys, misc
    NUM_CHECK_CATEGORIES
} ChecklistCategory;

typedef struct {
    const char *name;              // Display name (in-game verbiage)
    const char *token;             // Token for Normal difficulty
    const char *token_epic;        // Token for Epic/Legendary, or NULL if same
    ChecklistCategory category;
    QuestAct act;                  // Which act this belongs to (-1 for global)
} ChecklistExtraDef;

// Get the global checklist extras table.
// count_out: receives the number of entries.
// Returns: pointer to the checklist extras array.
const ChecklistExtraDef *
checklist_get_extras(int *count_out);

// Get the display name for a checklist category.
// cat: the category enum value.
// Returns: static string with the category name.
const char *
checklist_category_name(ChecklistCategory cat);

#endif
