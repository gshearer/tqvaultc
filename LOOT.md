# Loot Table & Affix System — Research Notes

## Overview

This document captures research into how Titan Quest assigns affixes (prefixes/suffixes)
to items, how our code determines valid affixes, and how TQVaultAE does the same.
The goal is to ensure we never allow creation of items that can't drop naturally in-game.

## Game Architecture

### Expansion Structure
- **Base game (TQ)**: `records\item\...`
- **Immortal Throne (xpack)**: `records\xpack\item\...`
- **Ragnarok (xpack2)**: `records\xpack2\item\...`
- **Atlantis (xpack3)**: `records\xpack3\items\...` (note: "items" not "item")
- **Eternal Embers (xpack4)**: `records\xpack4\item\...`

### Key Record Types
- **LootItemTable_FixedWeight / LootItemTable_DynWeight**: Maps items to affix randomizer tables.
  Contains `lootName[N]` (item paths), `prefixRandomizerName[N]`, `suffixRandomizerName[N]`,
  and corresponding weights.
- **LootRandomizerTable**: Contains `randomizerName[N]` / `randomizerWeight[N]` pairs pointing
  to individual affix records.
- **LootRandomizer**: Individual affix record with `lootRandomizerName` (translation tag) and stat effects.
- **LootMasterTable**: Chains to other loot tables (used for non-MI common drops and Tartarus).

### How Items Get Affixes (Normal Gameplay)
1. A monster has a loot table attached (e.g. `li_spear_eldjotun.dbr`)
2. That table specifies which item drops (`lootName1`) and which randomizer tables to use
3. The randomizer tables contain the pool of valid affixes with weights
4. The game picks affixes from these tables based on weights and roll chances

### MI (Monster Infrequent) Items
- MI items have dedicated loot tables per monster per difficulty (normal/epic/legendary)
- These tables explicitly reference specific affix randomizer tables
- Example: `records/xpack2/item/loottables/weapons/monster/li_spear_eldjotun.dbr` references
  `records\xpack\item\lootmagicalaffixes\suffix\tablesweapons\speara_l04.dbr`

## Numbered Tier System

The numbered tiers (01, 02, 03, 04, 05) in affix table filenames represent game progression
eras, NOT item level ranges. Each tier has a genuinely different affix pool — a sliding window
of affix sub-tiers that shifts upward with higher numbers.

### Tier-to-expansion mapping
| Tier | Source | Usage |
|------|--------|-------|
| 01a/01b | Base game | Act 1-2 MIs (e.g. Gorgon Archer) |
| 02 | Base game | Act 3 MIs (e.g. Tigerman) |
| 03 | Base game | Act 4+ MIs (e.g. Neanderthal Mage) |
| 04 | Immortal Throne | ALL xpack/xpack2/xpack3 MIs (Machae, Dvergr, Eldjotun, etc.) |
| 05 | Eternal Embers | xpack4 MIs only (with china/egypt suffixes) |

### Affix pool differences between tiers
Each tier contains a sliding window of individual affix power levels. Example for
`AbilityOffensive` in sword suffix tables:
- N01a: `_01` only
- N02: `_01`, `_02`, `_03`
- E01: `_03`, `_04`, `_05`
- L01: `_04`, `_05`, `_06`
- L04: `_05`, `_06`, `_07`

Higher tiers drop lower-power affixes and add higher-power ones. Weights also shift to
favor stronger variants at higher tiers.

### Difficulty letter (n/e/l) differences
The difficulty letter controls the overall power band AND rare affix availability:
- **Normal (n)**: Basic affixes only (attributes, offensive ability, attack speed)
- **Epic (e)**: Adds rare affixes (e.g. Betrayer, Siren, life damage reduction)
- **Legendary (l)**: Same rare categories as Epic but upgraded tiers

## Expansion-Specific Affix Tables

Each expansion provides its own set of `LootRandomizerTable` records. These are NOT overrides
of earlier expansion tables — they exist at different paths.

### Pattern: xpack3 (Atlantis) Tables
xpack3 provides parallel affix tables for every gear category. These are **supersets** of the
corresponding xpack tables — they contain all the same affixes PLUS expansion-specific ones.

Example for spear suffixes:
- `records\xpack\item\lootmagicalaffixes\suffix\tablesweapons\speara_l04.dbr` — 15 affixes
- `records\xpack3\items\lootmagicalaffixes\suffix\tablesweapons\speara_l04.dbr` — 16 affixes (same 15 + `rare_extrarelic_01.dbr` = "Of the Tinkerer")

The xpack3 tables are used by:
- xpack3 monster loot tables (Atlantis monsters)
- xpack3 commondynamic tables (random drops in Atlantis areas)
- **Tartarus MI loot tables** (see below)

### Pattern: xpack4 (Eternal Embers) Tables
xpack4 tables use a different naming convention with geographic suffixes:
- `speara_l05china.dbr`, `speara_l05egypt.dbr`
These contain unique xpack4 affixes (e.g. "Of Tranquility", "Of Harmony", "Of the Emperor").

### "Of the Tinkerer" Distribution
Tinkerer (`records\xpack3\items\lootmagicalaffixes\suffix\default\rare_extrarelic_01.dbr`)
appears in ALL 75 xpack3 suffix tables — every gear category at every difficulty level.
It does NOT appear in any base game, xpack, xpack2, or xpack4 suffix table.

## Tartarus MI Loot Tables (Critical Discovery)

Tartarus is a procedurally generated dungeon where monsters from all expansions can spawn.
The mechanism for cross-expansion affixes is NOT the electrum system or any implicit
grouping — it is a set of **explicit Tartarus MI loot tables** under
`records\xpack3\tartarus\loot\mi_[n|e|l]\`.

### How Tartarus MI drops work
There are 53 Tartarus loot tables organized by difficulty and equipment type:
- `mi_l\spears.dbr`, `mi_l\arms_str.dbr`, `mi_l\arms_intdex.dbr`, etc.
- Each is a `LootItemTable_FixedWeight` that explicitly lists MI items from various expansions
- Each references **xpack3 affix tables** (which contain Tinkerer)
- FileDescription: "Tinkerer chance & dual drops 50%"

### Which items are included
The Tartarus MI tables are **curated** — they do NOT include every MI from every expansion.
- **Always included**: base game MIs, xpack MIs, xpack3 MIs
- **Selectively included xpack2 MIs**: Only certain items appear:
  - Ranged (ring of honor): duneraider, jackalman, machae, maenad, skeleton, tigerman
  - Centaur helm and torso
- **NOT included from xpack2**: Eldjotun (spear, sword, shield, helm, torso, greaves, armband),
  Dvergr, and many other Ragnarok MIs

### Consequence for affix validity
An item can only get Tinkerer if it appears in a loot table that references an xpack3 suffix
table. For MI items, the only such tables are:
1. xpack3 monster-specific loot tables (for Atlantis monsters)
2. The Tartarus MI tables (for curated items from all expansions)

Items NOT in any Tartarus MI table and NOT from xpack3 monsters **cannot get Tinkerer**.

### Electrum system (common drops in Tartarus)
The electrum system (`records\xpack4\item\loottables\electrum_xpack[0-4]\...`) handles
Tartarus common drops via LootMasterTable chains. These chain to each expansion's native
loot tables and affix tables. They do NOT use xpack3 affix tables for non-xpack3 items.

## Our Implementation (`affix_table.c`)

### Current Approach: Direct Table References
Our code scans all `LootItemTable_FixedWeight` and `LootItemTable_DynWeight` records and
builds a map: normalized item path → list of (prefix_table, suffix_table) pairs. When
resolving affixes for an item, we resolve only the directly-referenced randomizer tables.

This works correctly because:
- Items that can get Tinkerer appear in Tartarus MI tables → those tables directly reference
  xpack3 suffix tables → our code picks them up naturally
- Items that cannot get Tinkerer are never referenced with xpack3 suffix tables

### Previous Approach: Expansion Sibling Grouping (REMOVED)
We previously used `extract_randomizer_key()` to group affix tables by gear-type category,
stripping the expansion prefix and trailing digits. This caused tables like
`xpack\...\speara_l04.dbr` and `xpack3\...\speara_l04.dbr` to be treated as "siblings",
merging their affix pools. This was **incorrect** — it added Tinkerer to items like the
Eldjotun spear and xpack2 C06_RING05 ring, which can never actually get Tinkerer in-game
because they don't appear in any Tartarus MI table or xpack3 loot table.

### xpack4 Affixes
xpack4 affix tables (`l05china`/`l05egypt`) are exclusively used by xpack4 loot tables.
The electrum (Tartarus) system reuses each expansion's native affix tables — it does NOT
add xpack4 affixes to non-xpack4 items.

## TQVaultAE's Implementation

### Approach: Direct Table References Only
TQVaultAE uses the same direct-reference approach:
1. Scans all `LootItemTable_FixedWeight` and `LootItemTable_DynWeight` records
2. Builds a map: item path -> list of (prefix_table, suffix_table) pairs
3. For a given item, resolves only the directly-referenced tables
4. Groups results by `GameDlc` (determined from path: `\XPACK\` = IT, `\XPACK3\` = Atlantis, etc.)
5. Shows all affixes across all DLCs with a DLC suffix tag

Key files:
- `TQVaultAE/src/TQVaultAE.Data/Database.cs` — `BuildItemAffixTableMap()` (lines 457-599)
- `TQVaultAE/src/TQVaultAE.Data/ItemProvider.cs` — `GetItemAffixes()` (lines 266-315)
- `TQVaultAE/src/TQVaultAE.Data/LootTableCollectionProvider.cs` — `MakeTable()`, `LoadTable()`
- `TQVaultAE/src/TQVaultAE.GUI/Components/SackPanel.cs` — `AddPrefixSuffixMenuItems()` (line 1871+)

### TQVaultAE Bug: Socrates' on Gorgon Archer Armband
`Records/Item/EquipmentArmband/MI_L_GorgonArcher.dbr` uses `ArmsMelee_L01` prefix table.
Socrates' (`rare_intmana_04.dbr`) only exists in `armsmage` tables. TQVaultAE incorrectly
shows it because the Tartarus MI table `arms_intdex.dbr` references `armmage_l04` as its
suffix table, and this table applies to ALL items in the table (including melee armbands).
Our code also has this — it's not a TQVaultAE bug per se, it reflects the game data.

## Comparison: TQVaultAE vs Our Code

For `RECORDS/XPACK2/ITEM/EQUIPMENTWEAPONS/SPEAR/MI_L_ELDJOTUN.DBR`:
- TQVaultAE: ~23 suffixes. Correctly excludes Tinkerer (no Tartarus MI table includes it).
- Our code (after fix): Same ~23 suffixes. Tinkerer correctly excluded.

For `Records/Item/EquipmentArmband/MI_L_GorgonArcher.dbr`:
- Both show Tinkerer, correctly — it appears in Tartarus MI table `arms_intdex.dbr` which
  references xpack3 suffix tables.
