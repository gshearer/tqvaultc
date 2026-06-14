#ifndef DB_QUESTS_H
#define DB_QUESTS_H

// db_quests -- shared, GTK-free index of quest item rewards.
//
// A C port of tqdb's parse_quests (reference/tqdb/tqdb/main.py): it reads the
// .qst quest scripts out of the Quests.arc archives, pulls out the item[N]
// reward references (per-difficulty), resolves each referenced reward loot
// table via db_loot, and records the reward items with their drop percentages.
// A reverse index (item -> quests) backs the browser's "reward from" view.
//
// Unlike tqdb's lossy printable-strip + regex pass, we walk the .qst binary
// token stream (length-prefixed name/value fields) so titleTag and item[N]
// values are read exactly.
//
// Used by both the GUI Database Browser and the headless tq-dbr-tool.

#include <glib.h>
#include "arz.h"

typedef struct {
  char       *title_tag;     // quest title tag (translate for a display name)
  char       *label;         // first .qst entry name seen (fallback label)
  GHashTable *rewards[3];    // char* item_path_lc -> double* percent, per diff
} DbQuest;

typedef struct {
  int    quest_idx;          // index into DbQuestIndex.quests
  double percent[3];         // reward percentage per difficulty
} DbQuestReward;

typedef struct {
  GPtrArray  *quests;        // DbQuest*
  GHashTable *by_item;       // char* item_path_lc -> GPtrArray<DbQuestReward*>
} DbQuestIndex;

// Build the quest reward index from a set of Quests.arc archive files.
//   arz       -- loaded database (reward tables read on demand)
//   arc_files -- array of n_arcs absolute paths to Quests.arc archives
// Missing/unreadable archives are skipped.  Free with db_quest_index_free().
DbQuestIndex *db_quest_index_build(TQArzFile *arz,
                                   const char *const *arc_files, int n_arcs);
void db_quest_index_free(DbQuestIndex *idx);

// Quest rewards that can grant a given item path (any case), or NULL.
GPtrArray *db_quest_rewards_for_item(DbQuestIndex *idx, const char *item_path);

// -- Cache reconstruction ---------------------------------------------------
// Rebuild an index from pre-resolved data (the disk cache in db_browser_cache.c)
// without re-reading any .qst archives.  Allocate with db_quest_index_new_empty,
// append each quest + its per-difficulty rewards, then call …_finalize_reverse
// to build the reverse ("reward from") map.  Free with db_quest_index_free().

DbQuestIndex *db_quest_index_new_empty(void);
DbQuest *db_quest_index_append(DbQuestIndex *idx, const char *title_tag,
                               const char *label);
void db_quest_append_reward(DbQuest *q, int diff, const char *item_lc,
                            double percent);
void db_quest_index_finalize_reverse(DbQuestIndex *idx);

#endif
