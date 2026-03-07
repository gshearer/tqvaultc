# Stats Manager — Planning Notes

Research from TQVaultAE (C#) and tqrespec (Java), cross-referenced with our character.c/h parser.

## Save File Keys for Stats

All stored as length-prefixed key + 4-byte value in binary `.chr` file:

| Key | Type | Purpose |
|-----|------|---------|
| `temp` | float | Base attributes (occurrence 2=str, 3=dex, 4=int, 5=health, 6=mana) |
| `modifierPoints` | u32 | Available (unspent) attribute points |
| `skillPoints` | u32 | Available (unspent) skill points |
| `masteriesAllowed` | u32 | Max masteries allowed (0, 1, or 2 based on level) |
| `skillName` | string | Skill DBR path (per-skill block) |
| `skillLevel` | u32 | Points allocated to this skill |
| `skillEnabled` | u32 | Skill enabled (0/1) |
| `skillActive` | u32 | Skill active (0/1) |
| `skillSubLevel` | u32 | Sub-level value |
| `skillTransition` | u32 | Transition state |

## Game Constants

| Constant | Value |
|----------|-------|
| Min Strength | 50 |
| Min Dexterity | 50 |
| Min Intelligence | 50 |
| Min Health | 300 |
| Min Mana | 300 |
| Attribute increment per point | 4 (str/dex/int) |
| Health/Mana increment per point | 40 |
| Attribute points per level | 2 |
| Skill points per level | 3 |
| Max level | 85 |

## Attribute Points Math

- Points come from two sources: leveling (2 per level) AND quest rewards (e.g., "The Good Centaur" grants 2 extra). The save file does not distinguish the source.
- Available = modifierPoints (from save)
- Spent = (str - 50)/4 + (dex - 50)/4 + (int - 50)/4 + (health - 300)/40 + (mana - 300)/40
- **Total earned = Available + Spent** (computed at load, treated as immutable)
- User can only **redistribute** — never add or remove total points. The total is preserved exactly as the game granted it.
- Do NOT attempt to calculate total from level alone — quest rewards make that unreliable.

## Skill System

### Masteries (11 total)
| Mastery | DBR Path |
|---------|----------|
| Defense | `Records\Skills\Defensive\DefensiveMastery.dbr` |
| Earth | `Records\Skills\Earth\EarthMastery.dbr` |
| Hunting | `Records\Skills\Hunting\HuntingMastery.dbr` |
| Nature | `Records\Skills\Nature\NatureMastery.dbr` |
| Spirit | `Records\Skills\Spirit\SpiritMastery.dbr` |
| Storm | `Records\Skills\Storm\StormMastery.dbr` |
| Stealth | `Records\Skills\Stealth\StealthMastery.dbr` |
| Warfare | `Records\Skills\Warfare\WarfareMastery.dbr` |
| Dream | `Records\XPack\Skills\Dream\DreamMastery.dbr` |
| Rune | `Records\XPack2\Skills\Runemaster\Runemaster_Mastery.dbr` |
| Neidan | `Records\XPack4\Skills\Neidan\NeidanMastery.dbr` |

### Skill Point Allocation Rules
- Each mastery has a level (its own skill points) + child skills with their own levels
- Mastery level determines which child skills are available (tier unlocking)
- A mastery requires minimum 1 point to exist
- Removing ALL points from a mastery removes it, allowing selection of a different mastery
- Character can have 0, 1, or 2 masteries (controlled by `masteriesAllowed`)
- Skills belong to a mastery by directory path (e.g., `Records\Skills\Nature\*.dbr`)

### Skill Points Math
- Points come from leveling (3 per level) AND quest rewards. The save file does not distinguish the source.
- Available = skillPoints (from save)
- Spent = sum of all skillLevel values across all skill blocks
- **Total earned = Available + Spent** (computed at load, treated as immutable)
- Same principle as attributes: user can only redistribute, never add or remove total points.

## Current Parser State (character.c)

### Already Parsed
- `temp` occurrences (str/dex/int/health/mana as floats)
- `skillName` (only stored if contains "Mastery.dbr" — mastery1/mastery2)
- `level`, `experience`, `kills`, `deaths`

### NOT Yet Parsed (need to add)
- `modifierPoints` — available attribute points
- `skillPoints` — available skill points
- `masteriesAllowed`
- Full skill list (all skillName + skillLevel pairs, not just masteries)

### Save Mechanism
Currently uses "splice" approach: marks block boundaries (inv_block_start/end, equip_block_start/end), re-encodes modified blocks, preserves everything else. For stats, we need a different approach since attribute values are scattered throughout the binary.

**Recommended approach for stat modification:** Record byte offsets of each field during parsing, then do in-place writes at those offsets when saving. This is what TQVaultAE does (`WriteIntAfter`/`WriteFloatAfter`).

## UI Plan — Attributes Tab (Phase 1)

### Layout
```
+------------------------------------------+
| Stats Manager                     [X]    |
+------------------------------------------+
| [Attributes] [Skills] [Misc]             |
+------------------------------------------+
|                                          |
|  Available Attribute Points: [12]        |
|                                          |
|  Strength:     [====|------] 78          |
|  Dexterity:    [==|--------] 58          |
|  Intelligence: [===|-------] 62          |
|  Health:       [=====|-----] 500         |
|  Energy:       [====|------] 420         |
|                                          |
|        [Apply]        [Cancel]           |
+------------------------------------------+
```

### Behavior
- Sliders show current allocation, constrained by min values and total earned points
- Moving a slider down frees points → "Available" increases
- Moving a slider up spends points → "Available" decreases
- Slider step: 4 for str/dex/int, 40 for health/mana
- Slider min: 50 for str/dex/int, 300 for health/mana
- Slider max: min_value + (total_earned_points * increment_per_point), but limited by available
- "Apply" writes modified values back to save file
- "Cancel" discards changes

### Implementation Steps
1. Extend `TQCharacter` struct: add `modifier_points`, `skill_points`, `masteries_allowed`, and byte offsets for each modifiable field
2. Extend parser: capture `modifierPoints`, `skillPoints`, `masteriesAllowed`, and record offsets of `temp` values
3. Add `character_save_stats()` — writes stat values at recorded offsets
4. Create `ui_stats_dialog.c` — GTK4 dialog with notebook, attributes tab with sliders
5. Add "Stats Manager" button to header bar (sensitive when character loaded)

## UI Plan — Skills Tab (Phase 2)

### Layout
```
+------------------------------------------+
| [Attributes] [Skills] [Misc]             |
+------------------------------------------+
|  Available Skill Points: [15]            |
|                                          |
| +-Mastery 1: Nature (40)-------+         |
| | [Remove All]                 |         |
| | Mastery Level: [====] 40     |         |
| | Regrowth:      [===]  8     |         |
| | Briar Ward:    [===]  8     |         |
| | Heart of Oak:  [==]   4     |         |
| | ...                          |         |
| +------------------------------+         |
|                                          |
| +-Mastery 2: Storm (32)--------+         |
| | [Remove All]                 |         |
| | Mastery Level: [====] 32     |         |
| | Ice Shard:     [===]  6     |         |
| | ...                          |         |
| +------------------------------+         |
+------------------------------------------+
```

### Behavior
- Two panes, one per mastery. Second grayed out if no second mastery
- Each pane: mastery level slider + child skill sliders
- "Remove All" zeros all skills in that mastery, refunding points
- If mastery level reaches 0: mastery is removed, dropdown appears to select new mastery
- Dropdown lists all 11 masteries minus the other active one
- Skill names resolved from DBR paths via translation tags

### Implementation Steps
1. Extend parser: capture full skill list (name + level + offset) for all skills, not just masteries
2. Build mastery→skill mapping from DBR paths
3. Resolve skill display names from `skillDisplayName` tag in skill DBR records
4. Create Skills tab UI with two mastery panes, sliders, and remove/select controls
