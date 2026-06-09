// ui_skills_layout.c -- in-game skill-window icon positions, read from the DB
//
// Builds a map from skill record path -> in-game icon position by walking the
// game UI database, exactly as the game (and the TitanQuestCalculator reference)
// do:
//
//   records/xpack4/ui/skills/skillswindow.dbr   (newest expansion wins)
//     skillCtrlPane1..N  ->  .../Mastery N/PaneCtrl.dbr
//       tabSkillButtons  ->  InGameUI/Player Skills/Mastery N/SkillNN.dbr
//         skillName / bitmapPositionX / bitmapPositionY / isCircular
//
// The map is built once on first lookup and cached for the process lifetime.

#include "ui_skills_layout.h"
#include "arz.h"
#include "asset_lookup.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// skillswindow.dbr candidates, newest expansion first.  A later expansion's
// window supersedes the earlier ones (it re-lists every mastery's panes).
static const char *SKILLSWINDOW_CANDIDATES[] = {
  "records\\xpack4\\ui\\skills\\skillswindow.dbr",
  "records\\xpack3\\ui\\skills\\skillswindow.dbr",
  "records\\xpack\\ui\\skills\\skillswindow.dbr",
  "records\\ui\\skills\\skillswindow.dbr",
  NULL,
};

static GHashTable *g_pos_map;  // owned char* path -> owned SkillIconPos*
static bool        g_loaded;

// Lowercase + '/' -> '\\', so keys agree regardless of how a caller spells the
// path.  Matches the normalization used in ui_skills_tree.c and asset_get_dbr.
static void
normalize_path(const char *src, char *dst, size_t dst_size)
{
  size_t i = 0;

  if(dst_size == 0)
    return;

  for(; src && src[i] && i + 1 < dst_size; i++)
  {
    char c = src[i];

    if(c == '/')
      c = '\\';

    dst[i] = (char)tolower((unsigned char)c);
  }

  dst[i] = '\0';
}

// Read one skill-button UI record and add its skill -> position mapping.
static void
add_button(const char *button_path)
{
  TQArzRecordData *btn = asset_get_dbr(button_path);  // borrowed (cached)

  if(!btn)
    return;

  char *skill = arz_record_get_string(btn, "skillName", NULL);

  if(!skill || !skill[0])
  {
    free(skill);
    return;
  }

  bool has_x = false, has_y = false;
  int px = arz_record_get_int(btn, "bitmapPositionX", 0, &has_x);
  int py = arz_record_get_int(btn, "bitmapPositionY", 0, &has_y);

  if(!has_x || !has_y)
  {
    free(skill);
    return;
  }

  char key[256];
  normalize_path(skill, key, sizeof(key));
  free(skill);

  // First definition wins, so a newer expansion's window takes precedence.
  if(g_hash_table_contains(g_pos_map, key))
    return;

  SkillIconPos *pos = g_new(SkillIconPos, 1);

  pos->pos_x = px;
  pos->pos_y = py;
  pos->circular = arz_record_get_int(btn, "isCircular", 0, NULL) != 0;

  g_hash_table_insert(g_pos_map, g_strdup(key), pos);
}

// Read one mastery control pane and add every skill button it lists.
static void
add_pane(const char *pane_path)
{
  TQArzRecordData *pane = asset_get_dbr(pane_path);  // borrowed (cached)

  if(!pane)
    return;

  // tabSkillButtons is a multi-value string array (one entry per button).
  TQVariable *buttons = arz_record_get_var(pane, arz_intern("tabSkillButtons"));

  if(!buttons || buttons->type != TQ_VAR_STRING)
    return;

  for(uint32_t j = 0; j < buttons->count; j++)
    if(buttons->value.str[j] && buttons->value.str[j][0])
      add_button(buttons->value.str[j]);
}

static void
load_map(void)
{
  g_loaded = true;
  g_pos_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  TQArzRecordData *win = NULL;

  for(int i = 0; SKILLSWINDOW_CANDIDATES[i]; i++)
    if((win = asset_get_dbr(SKILLSWINDOW_CANDIDATES[i])) != NULL)
      break;

  if(!win)
    return;

  // skillCtrlPane1..N -- enumerate until a gap; 16 comfortably covers the 11
  // base masteries plus headroom for mods.
  for(int i = 1; i <= 16; i++)
  {
    char field[32];

    snprintf(field, sizeof(field), "skillCtrlPane%d", i);

    char *pane = arz_record_get_string(win, field, NULL);

    if(pane && pane[0])
    {
      char norm[256];

      normalize_path(pane, norm, sizeof(norm));
      add_pane(norm);
    }

    free(pane);
  }
}

bool
skill_layout_lookup(const char *skill_path, SkillIconPos *out)
{
  if(!skill_path)
    return(false);

  if(!g_loaded)
    load_map();

  if(!g_pos_map)
    return(false);

  char key[256];

  normalize_path(skill_path, key, sizeof(key));

  SkillIconPos *pos = g_hash_table_lookup(g_pos_map, key);

  if(!pos)
    return(false);

  if(out)
    *out = *pos;

  return(true);
}
