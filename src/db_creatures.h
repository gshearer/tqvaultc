#ifndef DB_CREATURES_H
#define DB_CREATURES_H

// db_creatures -- shared, GTK-free index of boss/hero/quest creature loot.
//
// A C port of tqdb's parse_creatures + MonsterParser (reference/tqdb): it
// enumerates the Quest/Hero/Boss creatures, resolves their per-difficulty
// droppable loot (equipment slots + boss chests) through db_loot, and builds a
// reverse index so any item can list the creatures that drop it ("dropped by").
//
// Used by both the GUI Database Browser and the headless tq-dbr-tool.

#include <glib.h>
#include <stdbool.h>
#include "arz.h"

// One item a creature can drop, with its per-difficulty chance.
typedef struct {
  char  *item_lc;        // dropped item record path (lowercased)
  double chance[3];      // chance per difficulty
} DbItemChance;

typedef struct {
  char      *path;        // creature DBR record path
  char      *name_tag;    // 'description' tag (translate for a display name)
  char      *classification;  // "Boss" / "Hero" / "Quest"
  char      *race;        // characterRacialProfile (first), may be NULL
  int        level[3];    // charLevel per difficulty (Normal/Epic/Legendary)
  bool       active[3];   // false where the creature does not spawn
  bool       has_drops;   // true if it drops at least one indexable item
  GPtrArray *drops;       // DbItemChance*, sorted by best chance (forward list)
} DbCreature;

typedef struct {
  int    creature_idx;   // index into DbCreatureIndex.creatures
  double chance[3];      // drop chance per difficulty (0 if none that tier)
} DbDrop;

typedef struct {
  GPtrArray  *creatures;  // DbCreature*
  GHashTable *by_item;    // char* item_path_lc -> GPtrArray<DbDrop*>
} DbCreatureIndex;

// Build the full creature loot index (reads many records; a few seconds on a
// full database).  Free with db_creature_index_free().
DbCreatureIndex *db_creature_index_build(TQArzFile *arz);
void db_creature_index_free(DbCreatureIndex *idx);

// Drops for a given item path (any case/slash style), or NULL if none.
GPtrArray *db_creature_drops_for_item(DbCreatureIndex *idx, const char *item_path);

#endif
