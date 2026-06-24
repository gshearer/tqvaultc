#ifndef CHARACTER_H
#define CHARACTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "vault.h"

// Grid dimensions for character inventory panels
#define CHAR_INV_COLS 12
#define CHAR_INV_ROWS  5
#define CHAR_BAG_COLS  8
#define CHAR_BAG_ROWS  5

typedef struct {
    char *base_name;
    uint32_t seed;
    char *prefix_name;
    char *suffix_name;
    char *relic_name;
    char *relic_bonus;
    char *relic_name2;
    char *relic_bonus2;
    uint32_t var1;   // relic/charm slot 1 shard count
    uint32_t var2;   // relic/charm slot 2 shard count
} TQItem;

// Skill block from the character save
typedef struct {
    char *skill_name;       // DBR path
    uint32_t skill_level;
    uint32_t skill_enabled;
    uint32_t skill_active;
    uint32_t skill_sublevel;
    uint32_t skill_transition;
    size_t off_skill_level; // byte offset for in-place write
} TQCharSkill;

// TQCharacter - Represents a Titan Quest player character (.chr)
typedef struct {
    char *filepath;
    uint8_t *raw_data;
    size_t data_size;

    // Save-format provenance. headerVersion is the very first key: desktop
    // (TQ:AE) writes 3, the iOS mobile port writes 4. has_my_save_id is set
    // when the mobile-only `mySaveId` key is present, so a hypothetical future
    // desktop v4 isn't mis-classified as mobile. See character_is_mobile().
    int header_version;
    bool has_my_save_id;

    // Extracted statistics
    char *character_name;
    char *class_name;   // playerCharacterClass, e.g. "Sorceress"
    char *class_tag;    // playerClassTag, e.g. "tagCClass36" / "x4tagNeidanEarth"
    uint32_t level;
    uint32_t experience;
    uint32_t kills;
    uint32_t deaths;

    float strength;
    float dexterity;
    float intelligence;
    float health;
    float mana;

    char *mastery1;
    char *mastery2;

    // Stat editing support
    uint32_t modifier_points;   // unspent attribute points
    uint32_t skill_points;      // unspent skill points
    uint32_t masteries_allowed; // 0, 1, or 2

    // Byte offsets for in-place stat writes (0 = not found)
    size_t off_strength;
    size_t off_dexterity;
    size_t off_intelligence;
    size_t off_health;
    size_t off_mana;
    size_t off_modifier_points;
    size_t off_skill_points;

    // Parsed skill list
    TQCharSkill *skills;
    int num_skills;

    // Skill-list boundaries for inserting brand-new skill records (0 = unknown).
    // Each skill is its own begin_block..end_block. off_skill_max is the byte
    // offset of the `max` (skill-count) value preceding the list;
    // skill_list_end_off is the offset just past the last skill's end_block
    // (i.e. where a new begin_block..end_block skill record is spliced in).
    size_t off_skill_max;
    size_t skill_list_end_off;

    TQItem *equipment[12]; // Head, Neck, Chest, Legs, Arms, Ring1, Ring2, Wep1, Shld1, Wep2, Shld2, Artifact
    uint32_t equip_slot_var2[12]; // per-slot var2, even for empty slots
    int equip_attached[12];       // per-slot itemAttached flag
    int first_alternate;          // which alternate wrapper came first (0 or 1)

    // Inventory: sacks[0] = main 12x5, sacks[1..3] = extra bags 8x5
    TQVaultSack inv_sacks[4];
    int num_inv_sacks;
    uint32_t focused_sack;    // currentlyFocusedSackNumber
    uint32_t selected_sack;   // currentlySelectedSackNumber

    // Splice boundary offsets for binary save
    size_t inv_block_start;    // offset of "numberOfSacks" key length prefix
    size_t inv_block_end;      // offset after final inventory end_block value
    size_t equip_block_start;  // offset after useAlternate's value
    size_t equip_block_end;    // offset after final equipment end_block value
    bool has_atlantis;         // true if relicName2 fields were present
} TQCharacter;

// Load a character from a .chr file.
// filepath: path to the Player.chr file.
// Returns: allocated TQCharacter, or NULL on failure.
TQCharacter *
character_load(const char *filepath);

// Free all memory associated with a character.
// character: the character to free.
void
character_free(TQCharacter *character);

// True if this character came from the iOS mobile port (headerVersion >= 4 and
// the mobile-only `mySaveId` key was present). Mobile saves differ from desktop
// in two ways the loader handles: a UTF-32LE myPlayerName and a three-file
// Storage stash (0/1/2winsys.dxb at stashVersion 6).
// character: the character to test (may be NULL -> false).
bool
character_is_mobile(const TQCharacter *character);

// Convert a mastery DBR path to a short display name.
// e.g. "Records\Skills\Earth\EarthMastery.dbr" -> "Earth",
//      "...\neidanmastery.dbr" -> "Neidan". Applies the "Stealth"->"Rogue"
// cosmetic. Writes a NUL-terminated string into out.
// dbr_path: full mastery DBR path (may be NULL -> empty result).
// out:      destination buffer.
// outsz:    size of out.
void
character_mastery_display_name(const char *dbr_path, char *out, size_t outsz);

// Save a character to disk (full binary rewrite).
// character: the character to save.
// filepath: output path for the .chr file.
// Returns: 0 on success, -1 on error.
int
character_save(TQCharacter *character, const char *filepath);

// Write modified stats back into the raw data buffer in-place.
// character: the character whose stats to persist.
// Returns: 0 on success, -1 on error.
int
character_save_stats(TQCharacter *character);

// Write modified skill levels back into the raw data buffer in-place.
// character: the character whose skills to persist.
// Returns: 0 on success, -1 on error.
int
character_save_skills(TQCharacter *character);

// Like character_save_skills, but also splices `n_new` brand-new skill records
// (parallel arrays of DBR paths and levels) into the skill list before saving,
// bumping the list's `max` count. Existing skill levels and skillPoints are
// written in-place first (their offsets are unaffected by the end-of-list
// splice). After this returns the in-memory offsets past the skill list are
// stale, so the caller MUST reload the character. new_paths/new_levels may be
// NULL when n_new == 0 (then this behaves exactly like character_save_skills).
// Returns: 0 on success, -1 on error.
int
character_save_skills_ex(TQCharacter *character,
                         const char *const *new_paths,
                         const uint32_t *new_levels, int n_new);

#endif
