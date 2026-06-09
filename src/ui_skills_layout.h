// ui_skills_layout.h -- in-game skill-window icon positions, read from the DB
//
// The visual skill tree places each icon at the exact pixel position the game
// itself uses, rather than computing a layout.  Those positions are authored in
// the game UI database: records/<xpack>/ui/skills/skillswindow.dbr lists a
// per-mastery control pane; each pane's `tabSkillButtons` lists the skill-button
// UI records; each button record carries `skillName`, `bitmapPositionX`,
// `bitmapPositionY` and `isCircular`.  We walk that chain once and cache a map
// from skill record path -> position.  (This mirrors how the TitanQuestCalculator
// reference -- reference/TitanQuestCalculator -- derives its layout.)

#ifndef UI_SKILLS_LAYOUT_H
#define UI_SKILLS_LAYOUT_H

#include <stdbool.h>

// Native in-game skill-window position for one skill icon.  Coordinates are in
// the game's skill-panel pixel space (X grows right, Y grows DOWN -- the mastery
// bar sits at the largest Y, tier 1 just above it, tier 7 near the top).
typedef struct {
  int  pos_x;     // bitmapPositionX
  int  pos_y;     // bitmapPositionY
  bool circular;  // isCircular: round icon border (mastery/passive) vs square
} SkillIconPos;

// Look up a skill's authored icon position by its record path (any case, '/'
// or '\\' separators -- normalized internally).  Returns true and fills *out
// when the skill has a button in the in-game skill window; false otherwise
// (hidden helper skills with no button, or the UI records are unavailable).
// The underlying map is built lazily on first call and cached for the process
// lifetime.
bool skill_layout_lookup(const char *skill_path, SkillIconPos *out);

#endif // UI_SKILLS_LAYOUT_H
