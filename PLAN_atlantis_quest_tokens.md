# Plan: Fix Atlantis Quest Completion Detection

## Problem

Our quest manager dialog shows some Atlantis side quests as "unchecked" (incomplete) for characters that have completed all Atlantis quests in-game. The game's quest dialog shows all green checkmarks, but ours doesn't match.

## Root Cause (suspected)

Most Atlantis side quests (SQ02, SQ03, SQ05, SQ07-SQ11) have **no tokens at all** in QuestToken.myw — verified across two fully-completed characters (_soothie and _deathtouch). Only SQ01, SQ04, and SQ06 write `X3_SQ##GotQuest` tokens. The quests with 0 tokens use `completion_token` values like `X3_SQ02GotQuest` that don't exist in any save file, so they always show as unchecked.

Additionally, the quests that DO have tokens (SQ01, SQ04, SQ06) may also show wrong because the `completion_token` might not be the right token to check. For example, `X3_SQ01GotQuest` means "got quest", not "completed quest" — there might be a separate completion token, or the game might not use tokens for completion.

## Investigation Steps

1. **Check ui_quest_dialog.c** — understand how the dialog determines checked/unchecked state. It likely checks `quest_token_set_contains(set, def->completion_token)`. For quests with empty token arrays (`{ NULL }`), the completion_token still points to a non-existent token.

2. **Examine what the game actually tracks** — The game shows quests as complete but QuestToken.myw doesn't have matching tokens. Possibilities:
   - Atlantis quest completion is tracked in a different file (Player.chr? a .map file?)
   - The `X3_SQ##GotQuest` token name pattern is wrong — the actual tokens use different names
   - The game's quest system tracks completion internally and QuestToken.myw only stores trigger flags

3. **Search for hidden Atlantis tokens** — Run broader searches against the save files:
   ```bash
   # Dump ALL tokens and manually inspect for anything Atlantis-related
   ./build/tq-quest-tool dump testdata/_soothie/Levels_World_World01.map/Legendary/QuestToken.myw | sort > /tmp/soothie_tokens.txt
   ./build/tq-quest-tool dump testdata/_deathtouch/Levels_World_World01.map/Legendary/QuestToken.myw | sort > /tmp/deathtouch_tokens.txt
   diff /tmp/soothie_tokens.txt /tmp/deathtouch_tokens.txt
   ```
   Look for tokens that BOTH characters have that aren't currently mapped to any quest.

4. **Check the Atlantis quest DBR files** — Use tq-dbr-tool to inspect the actual quest definition records:
   ```bash
   ./build/tq-dbr-tool search testdata/database.arz "xpack3/quests" | grep -v proxy | grep -v pool | grep -v location
   ./build/tq-dbr-tool dump testdata/database.arz "records\xpack3\quests\sidequests\x3sq02.dbr"  # or similar paths
   ```
   Quest DBR files often contain `triggerToken` or `completionToken` fields that tell us what tokens the game writes.

5. **Consider the dialog UX for tokenless quests** — If investigation confirms that most Atlantis quests genuinely have no QuestToken entries, we need a UI decision:
   - Option A: Show tokenless quests as always-checked (greyed out, non-toggleable)
   - Option B: Show tokenless quests as always-unchecked with a note
   - Option C: Hide quests with no tokens entirely
   - Option D: Mark them differently in the QuestDef struct (add a `has_tokens` flag?)

## Files to Read

- `src/ui_quest_dialog.c` — how checked/unchecked state is determined
- `src/quest_tokens.h` — QuestDef struct, may need a new field
- `src/quest_tokens.c` lines ~693-740 — current Atlantis token arrays and defs

## Current State of Atlantis in quest_tokens.c

```
q_atl_new_adventures[] = { "X3_GotQuest_FindMarinos", "X3_FindMarinos_Complete", NULL }
q_atl_main[] = { "MQFirstTalkMarinosDone", ..., "MQTelkineDead", NULL }  // 13 tokens
q_atl_sq01[] = { "X3_SQ01GotQuest", NULL }
q_atl_sq02[] = { NULL }   // empty — no known tokens
q_atl_sq03[] = { NULL }
q_atl_sq04[] = { "X3_SQ04GotQuest", NULL }
q_atl_sq05[] = { NULL }
q_atl_sq06[] = { "X3_SQ06GotQuest", NULL }
q_atl_sq07-11[] = { NULL } // all empty
```

## Area Headers in Quest Dialog

The quest manager dialog should display area/region headers as the user scrolls through quests, matching the in-game quest log layout. The reference file `testdata/quests.txt` lists the area headers for each act. For example, Greece side quests are grouped under: Helos, Sparta, Megara, Delphi, Athens, Knossos. Atlantis under: Portal to Gadir, Atlantis, Gadir Outskirts, Gaulos Wilderness, Atlas Mountains, Mud Shoals.

### Implementation

1. **Add `area` field to `QuestDef`** in `src/quest_tokens.h`:
   ```c
   typedef struct {
       const char *name;
       const char *area;              /* Region/area header (e.g. "Helos", "Sparta") */
       QuestAct act;
       bool is_main;
       const char **tokens;
       const char *completion_token;
   } QuestDef;
   ```

2. **Populate area strings** in `quest_defs[]` in `src/quest_tokens.c` using `testdata/quests.txt` as reference. Main quests don't need areas (set to NULL). Side quests get their area string.

3. **Render area headers** in `src/ui_quest_dialog.c` — when iterating quest defs to build the scrollable list, check if the current quest's `area` differs from the previous quest's `area`. If so, insert a non-interactive header label (bold or styled differently) before the quest checkbox row. The header should only appear for side quests (main quests don't have area groupings).

4. **Full area mapping from testdata/quests.txt**:

   **Greece side quests:**
   - Helos: Monstrous Brigands, The Cornered Man, Medicines Waylaid
   - Sparta: The Lost Dowry, The Ancient of War, The Poisoned Spring
   - Megara: Skeleton Raiders, News of a Shipwreck
   - Delphi: A Proper Offering, The Good Centaur, A Master Blacksmith, Goods Abandoned, The Grieving Widow
   - Athens: Trapped in the Ruins, Spartans Lost
   - Knossos: Xanthippus the Healer, The Undead Tyrant

   **Egypt side quests:**
   - Rhakotis: The Family Heirloom
   - Lower Nile: The Beast of Legend, Plight of the Nile Farmers, A Promethean Surrounded
   - Memphis: Lowest of the Low, The High Priest's Request, The Missing Brother
   - Giza: Khufu's Curse
   - Fayum Oasis: A Hidden Treasure, Caravan Woes
   - Thebes: The Corrupted Priest

   **Orient side quests:**
   - Babylon: The Seeds of Destruction
   - Silk Road: A Gargantuan Yeti, Mystery in the Mountains, Caravan in Trouble
   - Great Wall: The Child and the Raptor, Peng Problems, Stalker in the Woods, The Wealthy Collector, A Lesson in Despair
   - Chang'an: The Emperor's Clay Soldiers, Terra Cottas at Large, Behind the Waterfall
   - Oiyum: A General in Repose, The Hermit Mage, Three Sisters

   **IT side quests:**
   - Rhodes: A Crab Story, An Impossible Task, The Torch-Lighter's Gauntlet, Outpost in the Woods
   - Ixian Woods: The Stolen Sigil, The Wealth of Ancient Kings, Lampido's Potion, The Treasure Hunters
   - Epirus: Among the Ruins, A Dangerous Mission, The Enemy's Captain
   - Styx: The Stygian Lurker, One Who Would Lead
   - Plains of Judgement: Hades' Treasury, The Dust of a Titan, Eurydice and Orpheus, An Invitation, The Necromanteion, Admetus Among the Dead, An Inside Source
   - Elysium: The Siege Striders, Flight of the Messenger, The Achaean Pass, A Noisy Diversion
   - Palace of Hades: The Shards of Erebus, Hades' Generals

   **Ragnarok side quests:**
   - Corinth: Festivities, Sciron, A Northern Contact
   - Heuneburg: Heart to Stomach, White Gold, The Golden Sickle
   - Glauberg: Wine from the Rhine, The Troubled Son, Celtic Plaid
   - Wildlands: Little Friends, The Trapped Nixie, The Kornwyt's Scythe
   - Scandia: Giesel, Fir Cone Liquor, The Restless King, The Magic Cauldron, The Survivor
   - Dark Lands: Dvergar History, Squabbling Merchants, The Craftsman's Passion, Legendary Craftsmanship

   **Atlantis side quests:**
   - Portal to Gadir: New Adventures Await
   - Atlantis: The City of Atlas, The Mysterious Artifacts, The Exterminator, Ancient Craft, A Score to Settle
   - Gadir Outskirts: The Lost Wanderer
   - Gaulos Wilderness: Nightly Guests
   - Atlas Mountains: The Letter, The Secret Depot, Of Goats and Bards
   - Mud Shoals: The Foraging Mission, Friends Like These

   **EE side quests:**
   - Mainland China: The Root of the Problem, Monkey Business, A Book and its Cover, The Council of Three
   - Pingyang: Emperor Yao's Bell, Love and Loss, Lust for Jade, Fire and Gold
   - The Heavens: Heavenly Lovers, Maze of Mirages
   - Lower Egypt: Shadowstone of Anubis, No Peace for the Fallen, A Venom Most Foul
   - The Marshland: The Phoenix and the Frog
   - The Ravaged Square: Just Deserts

## Test Data

- `testdata/_soothie/Levels_World_World01.map/Legendary/QuestToken.myw` — 621 tokens, all quests complete
- `testdata/_deathtouch/Levels_World_World01.map/Legendary/QuestToken.myw` — all Atlantis complete
- `testdata/soothie-atlantis-quests.png` — screenshot of game's quest dialog showing all complete
- Use `./build/tq-quest-tool coverage <myw>` to verify changes
