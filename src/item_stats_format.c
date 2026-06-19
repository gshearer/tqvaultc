#include "item_stats.h"
#include "asset_lookup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>

// relic/charm header helpers

// Compare two path characters, treating '/' and '\\' as the same separator
// and ignoring case.  DBR paths reach us in mixed forms -- save files use
// lowercase backslashes, TQVaultAE vaults use uppercase backslashes, and some
// in-game drops use forward slashes -- so segment checks like "\\charms\\" must
// match regardless of which separator/case the path happens to use.
static inline bool
path_char_eq(char a, char b)
{
  if((a == '/' || a == '\\') && (b == '/' || b == '\\'))
    return(true);

  return(g_ascii_tolower((guchar)a) == g_ascii_tolower((guchar)b));
}

// Case-insensitive, separator-agnostic substring search within a path.
// path: string to search in.
// needle: substring to find ('/' and '\\' are interchangeable).
// Returns: true if needle found.
bool
path_contains_ci(const char *path, const char *needle)
{
  if(!path || !needle)
    return(false);

  size_t plen = strlen(path), nlen = strlen(needle);

  if(nlen == 0)
    return(true);

  for(size_t i = 0; i + nlen <= plen; i++)
  {
    size_t j = 0;

    while(j < nlen && path_char_eq(path[i + j], needle[j]))
      j++;

    if(j == nlen)
      return(true);
  }

  return(false);
}

// Get the relic type label ("Charm" or "Relic") from a relic path.
// relic_path: DBR path to the relic/charm.
// Returns: "Charm" or "Relic" string literal.
static const char*
relic_type_label(const char *relic_path)
{
  if(path_contains_ci(relic_path, "charm"))
    return("Charm");

  if(path_contains_ci(relic_path, "animalrelic"))
    return("Charm");

  return("Relic");
}

// Returns the number of shards needed to complete a relic/charm.
// relic_path: DBR path to the relic/charm record.
// Returns: max shard count, or default (5 for charms, 3 for relics).
int
relic_max_shards(const char *relic_path)
{
  TQArzRecordData *data = asset_get_dbr(relic_path);

  if(data)
  {
    TQVariable *v = arz_record_get_var(data, INT_completedRelicLevel);

    if(v && v->type == TQ_VAR_INT && v->count > 0 && v->value.i32[0] > 0)
      return(v->value.i32[0]);
  }

  if(path_contains_ci(relic_path, "charm") || path_contains_ci(relic_path, "animalrelic"))
    return(5);

  return(3);
}

// True for "single-piece" relics/charms whose completedRelicLevel is 1 -- quest
// rewards (e.g. quest_artifice charm, nerthusmistletoe relic) that are always
// awarded complete and never exist as shards.  See item_stats.h.
bool
relic_is_single_piece(const char *relic_path)
{
  TQArzRecordData *data = asset_get_dbr(relic_path);

  if(!data)
    return(false);

  TQVariable *v = arz_record_get_var(data, INT_completedRelicLevel);

  return(v && v->type == TQ_VAR_INT && v->count > 0 &&
         v->value.i32[0] > 0 && v->value.i32[0] <= 1);
}

// Add a relic/charm section to the tooltip output.
// relic_name: DBR path to the relic/charm.
// relic_bonus: DBR path to the completion bonus record (may be NULL).
// shard_count: number of shards collected.
// tr: translation table.
// w: BufWriter to append to.
static void
add_relic_section(const char *relic_name, const char *relic_bonus,
                  uint32_t shard_count, TQTranslation *tr,
                  BufWriter *w)
{
  if(!relic_name || !relic_name[0])
    return;

  const char *relic_tag = get_record_variable_string(relic_name, INT_description);
  const char *relic_str = relic_tag ? translation_get(tr, relic_tag) : NULL;
  char *pretty_fallback = NULL;

  if(!relic_str || !relic_str[0])
  {
    pretty_fallback = pretty_name_from_path(relic_name);
    relic_str = pretty_fallback;
  }

  char *e_relic = escape_markup(relic_str);
  const char *type_label = relic_type_label(relic_name);
  int max_shards = relic_max_shards(relic_name);
  bool completed = relic_is_single_piece(relic_name) || relic_bonus ||
                   (shard_count >= (uint32_t)max_shards);

  buf_write(w, "\n<b><span color='#C1A472'>%s</span></b>\n", e_relic);

  if(completed)
    buf_write(w, "<span color='#C1A472'>Completed %s</span>\n", type_label);

  else if(shard_count > 0)
    buf_write(w, "<span color='#C1A472'>%s (+%u)</span>\n", type_label, shard_count);

  else
    buf_write(w, "<span color='#C1A472'>%s</span>\n", type_label);

  free(e_relic);
  free(pretty_fallback);

  int stat_idx = completed ? max_shards - 1 : (shard_count > 0 ? (int)shard_count - 1 : 0);

  add_stats_from_record(relic_name, tr, w, "#C1A472", stat_idx);

  if(relic_bonus)
  {
    buf_write(w, "\n<span color='#C1A472'>Completed %s Bonus</span>\n", type_label);
    add_stats_from_record(relic_bonus, tr, w, "#C1A472", 0);
  }
}

// requirements

// Simple recursive-descent expression evaluator for itemCost equations.
// Supports: +, -, *, /, ^ (power), parentheses, decimal numbers, and
// variable substitution for "itemLevel" and "totalAttCount".

typedef struct {
  const char *p;
  double item_level;
  double total_att_count;
} ExprCtx;

static double expr_parse_expr(ExprCtx *c);

// Skip whitespace in the expression context.
// c: expression context.
static void
expr_skip_ws(ExprCtx *c)
{
  while(*c->p == ' ' || *c->p == '\t')
    c->p++;
}

// Parse an atom (number, variable, or parenthesized expression).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_atom(ExprCtx *c)
{
  expr_skip_ws(c);

  if(*c->p == '(')
  {
    c->p++;

    double v = expr_parse_expr(c);

    expr_skip_ws(c);

    if(*c->p == ')')
      c->p++;

    return(v);
  }

  // variable or number
  if((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z'))
  {
    const char *start = c->p;

    while((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z') ||
           (*c->p >= '0' && *c->p <= '9') || *c->p == '_')
      c->p++;

    size_t len = (size_t)(c->p - start);

    if(len == 9 && strncmp(start, "itemLevel", 9) == 0)
      return(c->item_level);

    if(len == 13 && strncmp(start, "totalAttCount", 13) == 0)
      return(c->total_att_count);

    return(0.0);
  }

  // number (possibly negative handled by caller via unary minus)
  char *end;
  double v = strtod(c->p, &end);

  if(end == c->p)
    return(0.0);

  c->p = end;

  return(v);
}

// Parse a unary expression (handles leading +/-).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_unary(ExprCtx *c)
{
  expr_skip_ws(c);

  if(*c->p == '-')
  {
    c->p++;
    return(-expr_parse_unary(c));
  }

  if(*c->p == '+')
  {
    c->p++;
    return(expr_parse_unary(c));
  }

  return(expr_parse_atom(c));
}

// Parse a power expression (right-associative ^).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_power(ExprCtx *c)
{
  double v = expr_parse_unary(c);

  expr_skip_ws(c);

  if(*c->p == '^')
  {
    c->p++;
    v = pow(v, expr_parse_power(c));
  }

  return(v);
}

// Parse a multiplication/division expression.
// c: expression context.
// Returns: parsed value.
static double
expr_parse_muldiv(ExprCtx *c)
{
  double v = expr_parse_power(c);

  for(;;)
  {
    expr_skip_ws(c);

    if(*c->p == '*')
    {
      c->p++;
      v *= expr_parse_power(c);
    }

    else if(*c->p == '/')
    {
      c->p++;

      double d = expr_parse_power(c);

      if(d != 0)
        v /= d;
    }

    else
      break;
  }

  return(v);
}

// Parse an addition/subtraction expression (top-level).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_expr(ExprCtx *c)
{
  double v = expr_parse_muldiv(c);

  for(;;)
  {
    expr_skip_ws(c);

    if(*c->p == '+')
    {
      c->p++;
      v += expr_parse_muldiv(c);
    }

    else if(*c->p == '-')
    {
      c->p++;
      v -= expr_parse_muldiv(c);
    }

    else
      break;
  }

  return(v);
}

// Evaluate a string equation with variable substitution.
// eq: equation string.
// item_level: value for "itemLevel" variable.
// total_att_count: value for "totalAttCount" variable.
// Returns: computed result.
static double
eval_equation(const char *eq, double item_level, double total_att_count)
{
  ExprCtx c = { .p = eq, .item_level = item_level, .total_att_count = total_att_count };

  return(expr_parse_expr(&c));
}

// Map item Class to equation prefix used in itemCost records.
// item_class: Class string from DBR.
// Returns: equation prefix, or NULL if unknown.
static const char *
class_to_equation_prefix(const char *item_class)
{
  if(!item_class)
    return(NULL);

  static const struct { const char *cls; const char *prefix; } map[] = {
    {"ArmorProtective_Head",          "head"},
    {"ArmorProtective_UpperBody",     "upperBody"},
    {"ArmorProtective_Forearm",       "forearm"},
    {"ArmorProtective_LowerBody",     "lowerBody"},
    {"ArmorJewelry_Ring",             "ring"},
    {"ArmorJewelry_Amulet",           "amulet"},
    {"WeaponHunting_Spear",           "spear"},
    {"WeaponMagical_Staff",           "staff"},
    {"WeaponHunting_RangedOneHand",   "bow"},
    {"WeaponHunting_Bow",             "bow"},
    {"WeaponMelee_Sword",             "sword"},
    {"WeaponMelee_Mace",              "mace"},
    {"WeaponMelee_Axe",               "axe"},
    {"WeaponArmor_Shield",            "shield"},
    {"ArmorJewelry_Bracelet",         "bracelet"},
    {NULL, NULL}
  };

  for(int i = 0; map[i].cls; i++)
  {
    if(strcasecmp(item_class, map[i].cls) == 0)
      return(map[i].prefix);
  }

  return(NULL);
}

// Requirement slot order shared by the requirement helpers below.
// Index 0 = level, 1 = dexterity, 2 = intelligence, 3 = strength.
static const struct { const char **interned; const char *eq_suffix; } req_slots[4] = {
  {&INT_levelRequirement,        "LevelEquation"},
  {&INT_dexterityRequirement,    "DexterityEquation"},
  {&INT_intelligenceRequirement, "IntelligenceEquation"},
  {&INT_strengthRequirement,     "StrengthEquation"},
};

// Compute the level/dex/int/str requirements for a single DBR record.
// record_path: DBR path to a base item or affix record.
// out: filled with [level, dexterity, intelligence, strength]; zero where unset.
// Reads the static requirement fields; for any that remain zero on a gear
// record, falls back to the itemCost level-scaling equations.
void
item_record_requirements(const char *record_path, int out[4])
{
  out[0] = out[1] = out[2] = out[3] = 0;

  if(!record_path || !record_path[0])
    return;

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return;

  // Read static requirement values
  for(int r = 0; r < 4; r++)
  {
    TQVariable *v = arz_record_get_var(data, *req_slots[r].interned);

    if(!v || v->count == 0)
      continue;

    out[r] = (v->type == TQ_VAR_FLOAT) ? (int)v->value.f32[0] : v->value.i32[0];
  }

  // For any requirement type still zero, try dynamic computation via equations
  int needs_dynamic = 0;

  for(int r = 0; r < 4; r++)
  {
    if(out[r] <= 0)
    {
      needs_dynamic = 1;
      break;
    }
  }

  if(!needs_dynamic)
    return;

  const char *item_class = record_get_string_fast(data, INT_Class);
  const char *eq_prefix = class_to_equation_prefix(item_class);

  if(!eq_prefix)
    return;

  TQVariable *lvl_var = arz_record_get_var(data, INT_itemLevel);
  double item_level = 0;

  if(lvl_var && lvl_var->count > 0)
    item_level = (lvl_var->type == TQ_VAR_FLOAT) ? lvl_var->value.f32[0] : (double)lvl_var->value.i32[0];

  if(item_level <= 0)
    return;

  const char *cost_path = record_get_string_fast(data, INT_itemCostName);
  TQArzRecordData *cost_data = NULL;

  if(cost_path && cost_path[0])
    cost_data = asset_get_dbr(cost_path);

  if(!cost_data)
    cost_data = asset_get_dbr("records\\game\\itemcost.dbr");

  if(!cost_data)
    return;

  for(int r = 0; r < 4; r++)
  {
    if(out[r] > 0)
      continue;

    char eq_name[128];

    snprintf(eq_name, sizeof(eq_name), "%s%s", eq_prefix, req_slots[r].eq_suffix);

    const char *equation = record_get_string_fast(cost_data, arz_intern(eq_name));

    if(!equation || !equation[0])
      continue;

    int val = (int)ceil(eval_equation(equation, item_level, 0.0));

    if(val > 0)
      out[r] = val;
  }
}

// Compute the aggregate requirements of a full item across its base, prefix,
// suffix and socketed relic/charm records, keeping the maximum per type (the
// game enforces the highest requirement among an item's components).  Any
// NULL/empty path is skipped.  out: [level, dexterity, intelligence, strength].
void
item_requirements(const char *base_name, const char *prefix_name,
                  const char *suffix_name, const char *relic_name,
                  const char *relic_name2, int out[4])
{
  out[0] = out[1] = out[2] = out[3] = 0;

  const char *paths[5] = {base_name, prefix_name, suffix_name,
                          relic_name, relic_name2};

  for(int p = 0; p < 5; p++)
  {
    int r[4];

    item_record_requirements(paths[p], r);

    for(int i = 0; i < 4; i++)
      if(r[i] > out[i])
        out[i] = r[i];
  }
}

// Add item requirements (level, str, dex, int) to the tooltip.
// When `reduction` is non-NULL, requirement lines whose value is lowered by a
// gear/skill requirement reduction also show the reduced value in parentheses,
// e.g. "Required Intelligence: 632 (518 reduction)".
// record_path: DBR path to the base item.
// reduction: per-attribute reduction percentages, or NULL for none.
// w: BufWriter to append to.
static void
add_requirements(const char *record_path, const ItemReqReduction *reduction,
                 BufWriter *w)
{
  if(!record_path || !record_path[0])
    return;

  if(!asset_get_dbr(record_path))
    return;

  int vals[4];

  item_record_requirements(record_path, vals);

  static const char *labels[4] = {
    "Required Player Level",
    "Required Dexterity",
    "Required Intelligence",
    "Required Strength",
  };

  // Reduction percentages in the same slot order as vals/labels
  // ([level, dexterity, intelligence, strength]).
  int red_pct[4] = {0, 0, 0, 0};

  if(reduction)
  {
    red_pct[0] = reduction->level;
    red_pct[1] = reduction->dexterity;
    red_pct[2] = reduction->intelligence;
    red_pct[3] = reduction->strength;
  }

  buf_write(w, "\n");

  for(int r = 0; r < 4; r++)
  {
    if(vals[r] <= 0)
      continue;

    int pct = red_pct[r];

    if(pct > 100)
      pct = 100;

    // Effective requirement after reduction.  The (int) truncation matches
    // apply_reduction() used by the equippability highlight.
    int reduced = (pct > 0) ? (int)((float)vals[r] * (1.0f - (float)pct / 100.0f)) : vals[r];

    if(reduced < vals[r])
      buf_write(w, "%s: %d <span color='#66CC66'>(%d reduction)</span>\n",
                labels[r], vals[r], reduced);
    else
      buf_write(w, "%s: %d\n", labels[r], vals[r]);
  }
}

// A granted skill of class Skill_SpawnPet[Monster] (e.g. the Summoner's Trinket)
// conjures a combat pet.  Surface what the pet does so the tooltip conveys the
// item's value: the pet's attributes (life time, health, energy, physical
// damage) followed by its active abilities (named non-passive skills, resolving
// buffSkillName for the display name + its effects).  Mirrors -- and extends, to
// the Skill_SpawnPetMonster class -- TQVaultAE's ConvertPetStats.
//   skill_data  - the resolved granted-skill record (carries spawnObjects)
//   skill_index - per-level array index (itemSkillLevel - 1)
// A no-op unless spawnObjects resolves to a Class=="Pet" record.  Aura wards
// (also Pet-class) naturally show fewer lines -- they have no hand-hit damage
// and their granted auras are listed as abilities.
static void
append_pet_summon_stats(TQArzRecordData *skill_data, TQTranslation *tr,
                        BufWriter *w, const char *color, int skill_index)
{
  if(!skill_data)
    return;

  const char *pet_path = record_get_string_fast(skill_data, INT_spawnObjects);

  if(!pet_path || !pet_path[0])
    return;

  // spawnObjects may list several variants (comma-separated); the first is
  // enough to describe the summon.
  char first_pet[512];

  snprintf(first_pet, sizeof(first_pet), "%s", pet_path);

  char *comma = strchr(first_pet, ',');

  if(comma)
    *comma = '\0';

  TQArzRecordData *pet = asset_get_dbr(first_pet);

  if(!pet)
    return;

  // A summoned object is a creature worth describing if it carries the
  // characterLife trait.  This accepts every class the game uses for item
  // summons -- Pet, Monster, Guard, DynamicBarrier -- whereas gating on
  // Class=="Pet" alone missed combat pets defined as monsters (e.g. Medusa's
  // Anguish -> Gorgon Guard, Class "Monster"); non-creature spawn objects
  // (projectiles/effects) have no characterLife and are skipped.
  if(!arz_record_get_var(pet, INT_characterLife))
    return;

  const char *pet_tag = record_get_string_fast(pet, INT_description);
  const char *pet_name = pet_tag ? translation_get(tr, pet_tag) : NULL;

  if(!pet_name || !pet_name[0])
    pet_name = "Pet";

  char *e_name = escape_markup(pet_name);

  // Summon limit (only worth noting when more than one can be active).
  int limit = arz_record_get_int(skill_data, "petLimit", 0, NULL);

  if(limit > 1)
    buf_write(w, "<span color='%s'>%d Summon Limit</span>\n", color, limit);

  buf_write(w, "\n<span color='%s'>%s Attributes:</span>\n", color, e_name);

  // Time to live (a property of the spawning skill, not the pet).  Absent for
  // permanent summons.
  float ttl = dbr_get_float_fast(skill_data, INT_spawnObjectsTimeToLive, skill_index);

  if(ttl > 0)
    buf_write(w, "<span color='%s'>Life Time %.1f Seconds</span>\n", color, ttl);

  float life = dbr_get_float_fast(pet, INT_characterLife, 0);

  if(life != 0)
    buf_write(w, "<span color='%s'>%.0f Health</span>\n", color, life);

  float mana = dbr_get_float_fast(pet, INT_characterMana, 0);

  if(mana != 0)
    buf_write(w, "<span color='%s'>%.0f Energy</span>\n", color, mana);

  // Build the abilities into a scratch buffer so the "Abilities:" header (and
  // its leading blank line) can be skipped entirely when the pet has none --
  // e.g. self-drop/wall "pets" with no combat damage or skills.
  char ab_buf[16384];
  BufWriter ab;

  buf_init(&ab, ab_buf, sizeof(ab_buf));

  // Physical (hand-to-hand) damage.
  float dmin = dbr_get_float_fast(pet, INT_handHitDamageMin, 0);
  float dmax = dbr_get_float_fast(pet, INT_handHitDamageMax, 0);

  if(dmin > 1.0f || dmax > 2.0f)
  {
    if(dmax == 0 || dmin == dmax)
      buf_write(&ab, "<span color='%s'>%.0f Damage</span>\n", color, dmin);
    else
      buf_write(&ab, "<span color='%s'>%.0f - %.0f Damage</span>\n", color, dmin, dmax);
  }

  // Whether the damage line was written, so the first named ability below can be
  // separated from it by a blank line (only when both are present).
  bool had_damage = ab.pos > 0;
  bool first_named = true;

  // Named active abilities (skillName0..N), deduped by display name.  Passives
  // are inherent, not "abilities", so they are skipped (matching the game).
  char seen[24][128];
  int nseen = 0;

  for(int i = 0; i <= 31; i++)
  {
    char key[24];

    snprintf(key, sizeof(key), "skillName%d", i);

    char *sp = arz_record_get_string(pet, key, NULL);

    if(!sp || !sp[0] || strncasecmp(sp, "records", 7) != 0)
    {
      free(sp);
      continue;
    }

    TQArzRecordData *sd = asset_get_dbr(sp);

    if(!sd)
    {
      free(sp);
      continue;
    }

    const char *scls = record_get_string_fast(sd, INT_Class);

    if(scls && strcasecmp(scls, "Skill_Passive") == 0)
    {
      free(sp);
      continue;
    }

    // The name (and effect stats) live on the granted buff when present.
    const char *buff = record_get_string_fast(sd, INT_buffSkillName);
    const char *eff_path = (buff && buff[0]) ? buff : sp;
    TQArzRecordData *ed = (buff && buff[0]) ? asset_get_dbr(buff) : sd;

    const char *ntag = ed ? record_get_string_fast(ed, INT_skillDisplayName) : NULL;

    if(!ntag)
      ntag = record_get_string_fast(sd, INT_skillDisplayName);

    const char *nm = ntag ? translation_get(tr, ntag) : NULL;

    // Skip unnamed utility skills (no display name) -- they are internal and
    // add only noise.
    if(!nm || !nm[0])
    {
      free(sp);
      continue;
    }

    bool dup = false;

    for(int j = 0; j < nseen; j++)
      if(strcasecmp(seen[j], nm) == 0)
      {
        dup = true;
        break;
      }

    if(dup)
    {
      free(sp);
      continue;
    }

    snprintf(seen[nseen], sizeof(seen[nseen]), "%s", nm);

    if(nseen < (int)G_N_ELEMENTS(seen) - 1)
      nseen++;

    // The pet's level for this ability (per-difficulty arrays collapse to the
    // first value); the effect stats are read at that level - 1.
    char lkey[24];

    snprintf(lkey, sizeof(lkey), "skillLevel%d", i);

    int lvl = arz_record_get_int(pet, lkey, 1, NULL);

    if(lvl < 1)
      lvl = 1;

    // Separate the named abilities from the damage line above with a blank line.
    if(first_named && had_damage)
      buf_write(&ab, "\n");

    first_named = false;

    char *enm = escape_markup(nm);

    buf_write(&ab, "<span color='%s'><b>%s</b></span>\n", color, enm ? enm : nm);
    free(enm);

    // Render the ability's effects into a scratch buffer, then re-emit each
    // line indented a little under its (bold) name.
    char eff_buf[4096];
    BufWriter ew;

    buf_init(&ew, eff_buf, sizeof(eff_buf));
    add_stats_from_record(eff_path, tr, &ew, color, lvl - 1);

    for(char *line = eff_buf; *line; )
    {
      char *nl = strchr(line, '\n');
      int len = nl ? (int)(nl - line) : (int)strlen(line);

      if(len > 0)
        buf_write(&ab, "    %.*s\n", len, line);
      else
        buf_write(&ab, "\n");   // preserve blank separator lines

      if(!nl)
        break;

      line = nl + 1;
    }

    free(sp);
  }

  // Only show the "Abilities:" header (and its blank-line separator) when the
  // pet actually has some -- otherwise it is dead screen space.
  if(ab.pos > 0)
  {
    buf_write(w, "\n<span color='%s'>%s Abilities:</span>\n", color, e_name);
    buf_write(w, "%s", ab_buf);
  }

  free(e_name);
}

// Emit one source record's "Bonus to All Pets" block (header + the petBonus
// sub-record's stats) if it carries a `petBonusName`.  Called at the end of the
// card for base/prefix/suffix so every pet bonus is grouped after the wearer's
// own benefits (the inline emission in add_stats_from_record is deferred while
// those sections render -- see g_item_stats_defer_pet_bonus).  Returns true if
// anything was written.
static bool
append_pet_bonus_block(TQArzRecordData *src, TQTranslation *tr, BufWriter *w,
                       const char *color, int shard_index)
{
  if(!src)
    return(false);

  char *pet_bonus = arz_record_get_string(src, "petBonusName", NULL);

  if(!pet_bonus || !pet_bonus[0])
  {
    free(pet_bonus);
    return(false);
  }

  buf_write(w, "\n<span color='%s'>Bonus to All Pets:</span>\n", color);
  add_stats_from_record(pet_bonus, tr, w, color, shard_index);
  free(pet_bonus);
  return(true);
}

// main tooltip formatter

// Format item stats into a markup string, shared by both character and vault items.
// seed: item seed value.
// base_name: DBR path to the base item.
// prefix_name: DBR path to the prefix record (may be NULL).
// suffix_name: DBR path to the suffix record (may be NULL).
// relic_name: DBR path to the first relic/charm (may be NULL).
// relic_bonus: DBR path to the first relic completion bonus (may be NULL).
// var1: shard count for the first relic.
// relic_name2: DBR path to the second relic/charm (may be NULL).
// relic_bonus2: DBR path to the second relic completion bonus (may be NULL).
// var2: shard count for the second relic.
// tr: translation table.
// buffer: output buffer.
// size: buffer capacity.
static void
format_stats_common(uint32_t seed, const char *base_name, const char *prefix_name,
    const char *suffix_name, const char *relic_name, const char *relic_bonus,
    uint32_t var1, const char *relic_name2, const char *relic_bonus2,
    uint32_t var2, TQTranslation *tr, const ItemReqReduction *reduction,
    unsigned flags, char *buffer, size_t size)
{
  BufWriter w;

  buf_init(&w, buffer, size);

  // Pre-fetch record data pointers
  TQArzRecordData *base_data = base_name ? asset_get_dbr(base_name) : NULL;
  TQArzRecordData *prefix_data = (prefix_name && prefix_name[0]) ? asset_get_dbr(prefix_name) : NULL;
  TQArzRecordData *suffix_data = (suffix_name && suffix_name[0]) ? asset_get_dbr(suffix_name) : NULL;

  // Item title
  const char *base_tag = base_data ? record_get_string_fast(base_data, INT_itemNameTag) : NULL;
  const char *prefix_tag = prefix_data ? record_get_string_fast(prefix_data, INT_description) : NULL;

  if(!prefix_tag && prefix_data)
    prefix_tag = record_get_string_fast(prefix_data, INT_lootRandomizerName);

  if(!prefix_tag && prefix_data)
    prefix_tag = record_get_string_fast(prefix_data, INT_FileDescription);

  const char *suffix_tag = suffix_data ? record_get_string_fast(suffix_data, INT_description) : NULL;

  if(!suffix_tag && suffix_data)
    suffix_tag = record_get_string_fast(suffix_data, INT_lootRandomizerName);

  if(!suffix_tag && suffix_data)
    suffix_tag = record_get_string_fast(suffix_data, INT_FileDescription);

  // Fallback: try "description" tag if "itemNameTag" is missing (XPack4 relics/charms)
  if(!base_tag && base_data)
    base_tag = record_get_string_fast(base_data, INT_description);

  // Strip "records\" prefix from path for display
  const char *display_path = base_name;

  if(display_path && strncasecmp(display_path, "records\\", 8) == 0)
    display_path += 8;

  const char *base_str = base_tag ? translation_get(tr, base_tag) : NULL;
  char *base_pretty = NULL;

  if(!base_str || !base_str[0])
  {
    base_pretty = pretty_name_from_path(base_name);
    base_str = base_pretty;
  }

  const char *prefix_str = prefix_tag ? translation_get(tr, prefix_tag) : "";

  if(!prefix_str || !prefix_str[0])
    prefix_str = prefix_tag ? prefix_tag : "";

  const char *suffix_str = suffix_tag ? translation_get(tr, suffix_tag) : "";

  if(!suffix_str || !suffix_str[0])
    suffix_str = suffix_tag ? suffix_tag : "";

  if(!base_str)
    base_str = display_path;

  const char *item_color = get_item_color(base_name, prefix_name, suffix_name);
  char *e_base = escape_markup(base_str);
  char *e_prefix = escape_markup(prefix_str);
  char *e_suffix = escape_markup(suffix_str);

  buf_write(&w, "<b><span color='%s'>", item_color);

  if(e_prefix[0])
    buf_write(&w, "%s  ", e_prefix);

  buf_write(&w, "%s", e_base);

  if(e_suffix[0])
    buf_write(&w, " %s", e_suffix);

  buf_write(&w, "</span></b>\n");

  // Weapon/equipment type
  {
    const char *item_text_tag = base_data ? record_get_string_fast(base_data, INT_itemText) : NULL;

    if(item_text_tag)
    {
      const char *text_str = translation_get(tr, item_text_tag);

      if(text_str)
      {
        char *e_text = escape_markup(text_str);

        buf_write(&w, "<span color='white'>%s</span>\n", e_text);
        free(e_text);
      }
    }
  }

  free(e_base); free(e_prefix); free(e_suffix); free(base_pretty);

  // Artifact classification subtitle
  {
    const char *art_class = base_data ? record_get_string_fast(base_data, INT_artifactClassification) : NULL;

    if(art_class && art_class[0])
    {
      const char *class_tag = NULL;

      if(strcasecmp(art_class, "LESSER") == 0)
        class_tag = "xtagArtifactClass01";

      else if(strcasecmp(art_class, "GREATER") == 0)
        class_tag = "xtagArtifactClass02";

      else if(strcasecmp(art_class, "DIVINE") == 0)
        class_tag = "xtagArtifactClass03";

      if(class_tag)
      {
        const char *class_str = translation_get(tr, class_tag);

        if(class_str)
        {
          char *e_class = escape_markup(class_str);

          buf_write(&w, "<span color='white'>%s</span>\n", e_class);
          free(e_class);
        }
      }
    }
  }

  // Prefix properties
  if(prefix_name && prefix_name[0])
  {
    size_t pos_before = w.pos;
    const char *pname = prefix_tag ? translation_get(tr, prefix_tag) : NULL;
    char *ep = escape_markup(pname ? pname : "");

    if(ep[0])
      buf_write(&w, "\n<span color='white'><b>Prefix Properties : %s</b></span>\n", ep);

    else
      buf_write(&w, "\n<span color='white'><b>Prefix Properties</b></span>\n");

    free(ep);

    size_t pos_after_header = w.pos;

    // Affix pet bonuses render inline, as the last items under the affix (the
    // in-record reorder already puts the "Bonus to All Pets" block after the
    // affix's own stats/skill augments).
    add_stats_from_record(prefix_name, tr, &w, "#00A3FF", 0);

    if(w.pos == pos_after_header)
    {
      w.pos = pos_before;
      w.buf[w.pos] = '\0';
    }
  }

  // Detect if this is a standalone relic/charm
  bool standalone_relic_charm = false;

  if(base_name)
  {
    if(path_contains_ci(base_name, "animalrelic")
        || path_contains_ci(base_name, "\\relics\\")
        || path_contains_ci(base_name, "\\charms\\"))
      standalone_relic_charm = true;

    else if(base_data)
    {
      // Fallback: check Class for items in non-standard dirs (e.g. HCDUNGEON)
      const char *cls = record_get_string_fast(base_data, INT_Class);

      if(cls && (strcasecmp(cls, "ItemRelic") == 0 ||
                  strcasecmp(cls, "ItemCharm") == 0))
        standalone_relic_charm = true;
    }
  }

  bool is_artifact = (base_name && path_contains_ci(base_name, "\\artifacts\\") && !path_contains_ci(base_name, "\\arcaneformulae\\"));
  int base_shard_index = 0;
  bool standalone_complete = false;
  int standalone_max_shards = 0;

  if(standalone_relic_charm)
  {
    standalone_max_shards = relic_max_shards(base_name);
    // Single-piece quest relics/charms are always awarded complete.
    standalone_complete = relic_is_single_piece(base_name)
        || (relic_bonus && relic_bonus[0]) || (var1 >= (uint32_t)standalone_max_shards);
    base_shard_index = standalone_complete ? standalone_max_shards - 1
        : (var1 > 0 ? (int)var1 - 1 : 0);
  }

  // Base item properties
  {
    const char *bname = base_tag ? translation_get(tr, base_tag) : NULL;
    char *eb = escape_markup(bname ? bname : "");

    if(standalone_relic_charm)
    {
      const char *type_label = relic_type_label(base_name);

      if(standalone_complete)
        buf_write(&w, "\n<span color='#C1A472'>Completed %s</span>\n", type_label);

      else if(var1 > 0)
        buf_write(&w, "\n<span color='#C1A472'>%s (+%u)</span>\n", type_label, var1);

      else
        buf_write(&w, "\n<span color='#C1A472'>%s</span>\n", type_label);
    }

    else
    {
      if(eb[0])
        buf_write(&w, "\n<span color='#FFA500'><b>Base Item Properties : %s</b></span>\n", eb);

      else
        buf_write(&w, "\n<span color='#FFA500'><b>Base Item Properties</b></span>\n");
    }

    free(eb);

    // Attack speed tag -- only meaningful for weapons and shields
    if(!standalone_relic_charm && !is_artifact && base_data)
    {
      const char *item_class = record_get_string_fast(base_data, INT_Class);

      if(item_class && (strncasecmp(item_class, "WeaponMelee_", 12) == 0 ||
                         strncasecmp(item_class, "WeaponHunting_", 14) == 0 ||
                         strncasecmp(item_class, "WeaponMagical_", 14) == 0 ||
                         strcasecmp(item_class, "WeaponArmor_Shield") == 0))
      {
        const char *speed_tag = record_get_string_fast(base_data, INT_characterBaseAttackSpeedTag);

        if(speed_tag)
        {
          const char *speed_str = translation_get(tr, speed_tag);

          if(speed_str)
            buf_write(&w, "<span color='#00FFFF'>%s</span>\n\n", speed_str);
        }
      }
    }

    g_item_stats_defer_pet_bonus = true;   // grouped at the end of the card
    add_stats_from_record(base_name, tr, &w,
        standalone_relic_charm ? "#C1A472" : "#00FFFF", base_shard_index);
    g_item_stats_defer_pet_bonus = false;
  }

  // Standalone relic/charm completion bonus
  if(standalone_relic_charm && standalone_complete && relic_bonus && relic_bonus[0])
  {
    const char *type_label = relic_type_label(base_name);

    buf_write(&w, "\n<span color='#C1A472'>Completed %s Bonus</span>\n", type_label);
    add_stats_from_record(relic_bonus, tr, &w, "#C1A472", 0);
  }

  // Suffix properties
  if(suffix_name && suffix_name[0])
  {
    size_t pos_before = w.pos;
    const char *sname = suffix_tag ? translation_get(tr, suffix_tag) : NULL;
    char *es = escape_markup(sname ? sname : "");

    if(es[0])
      buf_write(&w, "\n<span color='white'><b>Suffix Properties : %s</b></span>\n", es);

    else
      buf_write(&w, "\n<span color='white'><b>Suffix Properties</b></span>\n");

    free(es);

    size_t pos_after_header = w.pos;

    // Affix pet bonuses render inline, as the last items under the affix.
    add_stats_from_record(suffix_name, tr, &w, "#00A3FF", 0);

    if(w.pos == pos_after_header)
    {
      w.pos = pos_before;
      w.buf[w.pos] = '\0';
    }
  }

  // Granted skill
  {
    const char *skill_dbr = base_data ? record_get_string_fast(base_data, INT_itemSkillName) : NULL;

    if(skill_dbr && skill_dbr[0])
    {
      TQArzRecordData *skill_data = asset_get_dbr(skill_dbr);
      const char *buff_path = skill_data ? record_get_string_fast(skill_data, INT_buffSkillName) : NULL;
      const char *effect_dbr = (buff_path && buff_path[0]) ? buff_path : skill_dbr;
      TQArzRecordData *effect_data = asset_get_dbr(effect_dbr);

      const char *skill_tag = effect_data ? record_get_string_fast(effect_data, INT_skillDisplayName) : NULL;

      if(!skill_tag && skill_data)
        skill_tag = record_get_string_fast(skill_data, INT_skillDisplayName);

      const char *skill_name = skill_tag ? translation_get(tr, skill_tag) : NULL;

      const char *trigger_text = "";
      const char *controller_dbr = base_data ? record_get_string_fast(base_data, INT_itemSkillAutoController) : NULL;

      if(controller_dbr && controller_dbr[0])
      {
        TQArzRecordData *ctrl_data = asset_get_dbr(controller_dbr);
        const char *trigger_type = ctrl_data ? record_get_string_fast(ctrl_data, INT_triggerType) : NULL;

        if(trigger_type)
        {
          if(strcasecmp(trigger_type, "onAttack") == 0)
            trigger_text = " (Activated on attack)";

          else if(strcasecmp(trigger_type, "onHit") == 0)
            trigger_text = " (Activated on hit)";

          else if(strcasecmp(trigger_type, "onBeingHit") == 0)
            trigger_text = " (Activated when hit)";

          else if(strcasecmp(trigger_type, "onEquip") == 0)
            trigger_text = " (Activated on equip)";

          else if(strcasecmp(trigger_type, "onLowHealth") == 0)
            trigger_text = " (Activated on low health)";

          else if(strcasecmp(trigger_type, "onKill") == 0)
            trigger_text = " (Activated on kill)";
        }
      }

      TQVariable *skill_level_var = base_data ? arz_record_get_var(base_data, INT_itemSkillLevel) : NULL;
      int skill_level = 1;

      if(skill_level_var)
      {
        if(skill_level_var->type == TQ_VAR_INT)
          skill_level = skill_level_var->value.i32[0];

        else if(skill_level_var->type == TQ_VAR_FLOAT)
          skill_level = (int)skill_level_var->value.f32[0];
      }

      int skill_index = (skill_level > 1) ? skill_level - 1 : 0;

      buf_write(&w, "\n<span color='white'><b>Grants Skill :</b></span>\n");

      if(skill_name)
      {
        char *e_name = escape_markup(skill_name);

        buf_write(&w, "<span color='white'>%s%s</span>\n", e_name, trigger_text);
        free(e_name);
      }

      const char *desc_tag = effect_data ? record_get_string_fast(effect_data, INT_skillBaseDescription) : NULL;

      if(!desc_tag && skill_data)
        desc_tag = record_get_string_fast(skill_data, INT_skillBaseDescription);

      if(desc_tag)
      {
        const char *desc_text = translation_get(tr, desc_tag);

        if(desc_text)
        {
          char *e_desc = escape_markup(desc_text);

          buf_write(&w, "<span color='white'>%s</span>\n", e_desc);
          free(e_desc);
        }
      }

      add_stats_from_record(effect_dbr, tr, &w, "#DAA520", skill_index);

      if(buff_path && buff_path[0])
        add_stats_from_record(skill_dbr, tr, &w, "#DAA520", skill_index);

      // Pet summon (Skill_SpawnPet[Monster]): list the conjured pet's
      // attributes and abilities.  The spawn fields live on the skill record,
      // but fall through a buff indirection just in case.  No-op for non-summon
      // skills.
      {
        TQArzRecordData *spawn_src = NULL;

        if(skill_data && record_get_string_fast(skill_data, INT_spawnObjects))
          spawn_src = skill_data;
        else if(effect_data && record_get_string_fast(effect_data, INT_spawnObjects))
          spawn_src = effect_data;

        append_pet_summon_stats(spawn_src, tr, &w, "#DAA520", skill_index);
      }

      // Pet/secondary skill
      const char *pet_skill_path = effect_data ? record_get_string_fast(effect_data, INT_petSkillName) : NULL;

      if((!pet_skill_path || !pet_skill_path[0]) && skill_data)
        pet_skill_path = record_get_string_fast(skill_data, INT_petSkillName);

      if(pet_skill_path && pet_skill_path[0])
      {
        TQArzRecordData *pet_data = asset_get_dbr(pet_skill_path);

        if(pet_data)
        {
          float chance = 0;
          TQVariable *cv = arz_record_get_var(pet_data, INT_skillChanceWeight);

          if(cv)
          {
            int ci = (skill_index < (int)cv->count) ? skill_index : (int)cv->count - 1;

            if(ci < 0)
              ci = 0;

            chance = (cv->type == TQ_VAR_INT) ? (float)cv->value.i32[ci] : cv->value.f32[ci];
          }

          if(chance > 0)
            buf_write(&w, "<span color='#DAA520'>%.0f%% Chance of:</span>\n", chance);

          const char *pet_buff = record_get_string_fast(pet_data, INT_buffSkillName);
          const char *pet_effect = (pet_buff && pet_buff[0]) ? pet_buff : pet_skill_path;

          add_stats_from_record(pet_effect, tr, &w, "#DAA520", skill_index);
        }
      }

    }
  }

  // Artifact completion bonus
  if(is_artifact && relic_bonus && relic_bonus[0])
  {
    buf_write(&w, "\n\n<span color='#40FF40'><b>Completion Bonus :</b></span>\n");
    add_stats_from_record(relic_bonus, tr, &w, "#40FF40", 0);
  }

  // Relic/Charm slot 1 (skip for standalone relics -- already shown above)
  if(!is_artifact && !standalone_relic_charm)
    add_relic_section(relic_name, relic_bonus, var1, tr, &w);

  // Relic/Charm slot 2
  add_relic_section(relic_name2, relic_bonus2, var2, tr, &w);

  // The base item's "Bonus to All Pets" -- deferred from the base render above
  // so it lands after every other wearer benefit (base stats, granted skill +
  // summoned-pet abilities).  Affix pet bonuses are NOT deferred: they render
  // inline as the last items under their own Prefix/Suffix section.
  append_pet_bonus_block(base_data, tr, &w,
      standalone_relic_charm ? "#C1A472" : "#00FFFF", base_shard_index);

  // Item seed with hex and percentage.  Suppressed for reference views (DB
  // browser) where the item is never spawned, so the seed is always zero and
  // meaningless.
  if(!(flags & ITEM_FMT_HIDE_SEED))
  {
    float seed_pct = ((float)seed / 65536.0f) * 100.0f;

    buf_write(&w, "\nitemSeed: %u (0x%08X) (%.3f %%)\n", seed, seed, seed_pct);
  }

  // Expansion indicator based on item path
  {
    const char *expansion_label = NULL;

    if(base_name)
    {
      if(strncasecmp(base_name, "records\\xpack4\\", 15) == 0)
        expansion_label = "Eternal Embers Item";

      else if(strncasecmp(base_name, "records\\xpack3\\", 15) == 0)
        expansion_label = "Atlantis Item";

      else if(strncasecmp(base_name, "records\\xpack2\\", 15) == 0)
        expansion_label = "Ragnarok Item";

      else if(strncasecmp(base_name, "records\\xpack\\", 14) == 0)
        expansion_label = "Immortal Throne Item";
    }

    if(expansion_label)
      buf_write(&w, "<span color='#40FF40'>%s</span>\n", expansion_label);
  }

  // Set info
  {
    const char *set_dbr = base_data ? record_get_string_fast(base_data, INT_itemSetName) : NULL;

    if(set_dbr && set_dbr[0])
    {
      // In the Database Browser the set name and members are clickable 'i:'
      // cross-refs (set entries are stored under their set DBR path); plain
      // text everywhere else.
      bool link = (flags & ITEM_FMT_SET_LINKS) != 0;
      TQArzRecordData *set_data = asset_get_dbr(set_dbr);
      const char *set_tag = set_data ? record_get_string_fast(set_data, INT_setName) : NULL;
      const char *set_name = set_tag ? translation_get(tr, set_tag) : NULL;

      if(set_name)
      {
        char *e_set = escape_markup(set_name);

        if(link)
        {
          char *e_href = escape_markup(set_dbr);

          buf_write(&w, "\n<a href='i:%s'><span color='#40FF40'>%s</span></a>\n",
                    e_href ? e_href : "", e_set ? e_set : "");
          free(e_href);
        }
        else
          buf_write(&w, "\n<span color='#40FF40'>%s</span>\n", e_set);

        free(e_set);
      }

      // List set members
      TQVariable *members_var = set_data ? arz_record_get_var(set_data, INT_setMembers) : NULL;

      if(members_var && members_var->type == TQ_VAR_STRING)
      {
        for(uint32_t m = 0; m < members_var->count; m++)
        {
          const char *member_path = members_var->value.str[m];

          if(!member_path || !member_path[0])
            continue;

          TQArzRecordData *member_data = asset_get_dbr(member_path);
          const char *member_tag = member_data ? record_get_string_fast(member_data, INT_description) : NULL;

          if(!member_tag && member_data)
            member_tag = record_get_string_fast(member_data, INT_itemNameTag);

          const char *member_name = member_tag ? translation_get(tr, member_tag) : NULL;

          if(member_name)
          {
            char *e_member = escape_markup(member_name);

            if(link)
            {
              // Real rarity colour (matching the set's own detail page) and a
              // jump link to the member item.
              const char *mc = get_item_color(member_path, NULL, NULL);
              char *e_href = escape_markup(member_path);

              buf_write(&w, "    <a href='i:%s'><span color='%s'>%s</span></a>\n",
                        e_href ? e_href : "", mc ? mc : "white",
                        e_member ? e_member : "");
              free(e_href);
            }
            else
              buf_write(&w, "<span color='#FFF52B'>    %s</span>\n", e_member);

            free(e_member);
          }
        }
      }
    }
  }

  // Requirements
  add_requirements(base_name, reduction, &w);
}

// resistance lookup (used by UI resistance table)

// Get a resistance value from a single DBR record.
// record_path: DBR path to load.
// attr_name: attribute name to query.
// shard_index: shard index for multi-value variables.
// Returns: resistance value, or 0.0f if not found.
static float
get_dbr_resistance(const char *record_path, const char *attr_name, int shard_index)
{
  if(!record_path || !record_path[0])
    return(0.0f);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(0.0f);

  const char *interned = arz_intern(attr_name);
  TQVariable *v = arz_record_get_var(data, interned);

  if(!v)
    return(0.0f);

  int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;

  if(idx < 0)
    idx = 0;

  return((v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx]);
}

// Like get_dbr_resistance but returns 0 if the stat has an associated Chance < 100%.
// record_path: DBR path to load.
// attr_name: attribute name to query.
// shard_index: shard index for multi-value variables.
// Returns: guaranteed stat value, or 0.0f.
static float
get_dbr_guaranteed(const char *record_path, const char *attr_name, int shard_index)
{
  if(!record_path || !record_path[0])
    return(0.0f);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(0.0f);

  const char *interned = arz_intern(attr_name);
  TQVariable *v = arz_record_get_var(data, interned);

  if(!v)
    return(0.0f);

  // Check for a corresponding Chance variable
  char chance_name[128];

  snprintf(chance_name, sizeof(chance_name), "%sChance", attr_name);

  const char *chance_interned = arz_intern(chance_name);
  TQVariable *cv = arz_record_get_var(data, chance_interned);

  if(cv && cv->count > 0)
  {
    float chance = (cv->type == TQ_VAR_INT) ? (float)cv->value.i32[0] : cv->value.f32[0];

    if(chance > 0 && chance < 100)
      return(0.0f);
  }

  int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;

  if(idx < 0)
    idx = 0;

  return((v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx]);
}

// Returns total resistance (summed across all item components) for the given
// DBR attribute name.
// item: the equipment item.
// attr_name: DBR attribute name to query.
// Returns: total resistance value.
float
item_get_resistance(TQItem *item, const char *attr_name)
{
  if(!item || !attr_name)
    return(0.0f);

  int si1 = item->var1 > 0 ? (int)item->var1 - 1 : 0;
  int si2 = item->var2 > 0 ? (int)item->var2 - 1 : 0;

  return(get_dbr_resistance(item->base_name, attr_name, 0)
       + get_dbr_resistance(item->prefix_name, attr_name, 0)
       + get_dbr_resistance(item->suffix_name, attr_name, 0)
       + get_dbr_resistance(item->relic_name, attr_name, si1)
       + get_dbr_resistance(item->relic_bonus, attr_name, 0)
       + get_dbr_resistance(item->relic_name2, attr_name, si2)
       + get_dbr_resistance(item->relic_bonus2, attr_name, 0));
}

// Like item_get_resistance but excludes stats that have an associated
// Chance < 100%.
// item: the equipment item.
// attr_name: DBR attribute name to query.
// Returns: guaranteed stat value.
float
item_get_guaranteed_stat(TQItem *item, const char *attr_name)
{
  if(!item || !attr_name)
    return(0.0f);

  int si1 = item->var1 > 0 ? (int)item->var1 - 1 : 0;
  int si2 = item->var2 > 0 ? (int)item->var2 - 1 : 0;

  return(get_dbr_guaranteed(item->base_name, attr_name, 0)
       + get_dbr_guaranteed(item->prefix_name, attr_name, 0)
       + get_dbr_guaranteed(item->suffix_name, attr_name, 0)
       + get_dbr_guaranteed(item->relic_name, attr_name, si1)
       + get_dbr_guaranteed(item->relic_bonus, attr_name, 0)
       + get_dbr_guaranteed(item->relic_name2, attr_name, si2)
       + get_dbr_guaranteed(item->relic_bonus2, attr_name, 0));
}

// Like get_dbr_guaranteed but with an explicit chance attribute name.
// record_path: DBR path to load.
// attr_name: attribute name to query.
// chance_attr: explicit chance attribute name.
// shard_index: shard index for multi-value variables.
// Returns: guaranteed stat value, or 0.0f.
static float
get_dbr_guaranteed_ex(const char *record_path, const char *attr_name,
                      const char *chance_attr, int shard_index)
{
  if(!record_path || !record_path[0])
    return(0.0f);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(0.0f);

  const char *interned = arz_intern(attr_name);
  TQVariable *v = arz_record_get_var(data, interned);

  if(!v)
    return(0.0f);

  if(chance_attr)
  {
    TQVariable *cv = arz_record_get_var(data, arz_intern(chance_attr));

    if(cv && cv->count > 0)
    {
      float chance = (cv->type == TQ_VAR_INT) ? (float)cv->value.i32[0] : cv->value.f32[0];

      if(chance > 0 && chance < 100)
        return(0.0f);
    }
  }

  int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;

  if(idx < 0)
    idx = 0;

  return((v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx]);
}

// Like item_get_guaranteed_stat but with an explicit chance attribute name.
// item: the equipment item.
// attr_name: DBR attribute name to query.
// chance_attr: explicit chance attribute name.
// Returns: guaranteed stat value.
float
item_get_guaranteed_stat_ex(TQItem *item, const char *attr_name, const char *chance_attr)
{
  if(!item || !attr_name)
    return(0.0f);

  int si1 = item->var1 > 0 ? (int)item->var1 - 1 : 0;
  int si2 = item->var2 > 0 ? (int)item->var2 - 1 : 0;

  return(get_dbr_guaranteed_ex(item->base_name, attr_name, chance_attr, 0)
       + get_dbr_guaranteed_ex(item->prefix_name, attr_name, chance_attr, 0)
       + get_dbr_guaranteed_ex(item->suffix_name, attr_name, chance_attr, 0)
       + get_dbr_guaranteed_ex(item->relic_name, attr_name, chance_attr, si1)
       + get_dbr_guaranteed_ex(item->relic_bonus, attr_name, chance_attr, 0)
       + get_dbr_guaranteed_ex(item->relic_name2, attr_name, chance_attr, si2)
       + get_dbr_guaranteed_ex(item->relic_bonus2, attr_name, chance_attr, 0));
}

// Returns mean of a guaranteed damage range from a single DBR.
// If max > min, returns (min + max) / 2; otherwise returns min.
// Returns 0 if the stat has a chance > 0 and < 100%.
// record_path: DBR path to load.
// min_attr: DBR attribute for minimum damage.
// max_attr: DBR attribute for maximum damage.
// chance_attr: DBR attribute for chance (NULL if always guaranteed).
// shard_index: shard index for multi-value variables.
// Returns: guaranteed mean damage, or 0.0f.
static float
get_dbr_guaranteed_mean(const char *record_path, const char *min_attr,
                        const char *max_attr, const char *chance_attr,
                        int shard_index)
{
  if(!record_path || !record_path[0])
    return(0.0f);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(0.0f);

  TQVariable *vmin = arz_record_get_var(data, arz_intern(min_attr));

  if(!vmin)
    return(0.0f);

  // Check chance -- if > 0 and < 100, not guaranteed
  if(chance_attr)
  {
    TQVariable *cv = arz_record_get_var(data, arz_intern(chance_attr));

    if(cv && cv->count > 0)
    {
      float chance = (cv->type == TQ_VAR_INT) ? (float)cv->value.i32[0] : cv->value.f32[0];

      if(chance > 0 && chance < 100)
        return(0.0f);
    }
  }

  int idx = (shard_index < (int)vmin->count) ? shard_index : (int)vmin->count - 1;

  if(idx < 0)
    idx = 0;

  float mn = (vmin->type == TQ_VAR_INT) ? (float)vmin->value.i32[idx] : vmin->value.f32[idx];

  if(mn <= 0)
    return(0.0f);

  TQVariable *vmax = arz_record_get_var(data, arz_intern(max_attr));

  if(vmax)
  {
    int mx_idx = (shard_index < (int)vmax->count) ? shard_index : (int)vmax->count - 1;

    if(mx_idx < 0)
      mx_idx = 0;

    float mx = (vmax->type == TQ_VAR_INT) ? (float)vmax->value.i32[mx_idx] : vmax->value.f32[mx_idx];

    if(mx > mn)
      return((mn + mx) / 2.0f);
  }

  return(mn);
}

// Returns total guaranteed mean damage summed across all item components.
// For each component, if max > min, uses (min + max) / 2.
// item: the equipment item.
// min_attr: DBR attribute for minimum damage.
// max_attr: DBR attribute for maximum damage.
// chance_attr: DBR attribute for chance (NULL if always guaranteed).
// Returns: total guaranteed mean damage.
float
item_get_guaranteed_damage_mean(TQItem *item, const char *min_attr,
                                const char *max_attr, const char *chance_attr)
{
  if(!item || !min_attr || !max_attr)
    return(0.0f);

  int si1 = item->var1 > 0 ? (int)item->var1 - 1 : 0;
  int si2 = item->var2 > 0 ? (int)item->var2 - 1 : 0;

  return(get_dbr_guaranteed_mean(item->base_name, min_attr, max_attr, chance_attr, 0)
       + get_dbr_guaranteed_mean(item->prefix_name, min_attr, max_attr, chance_attr, 0)
       + get_dbr_guaranteed_mean(item->suffix_name, min_attr, max_attr, chance_attr, 0)
       + get_dbr_guaranteed_mean(item->relic_name, min_attr, max_attr, chance_attr, si1)
       + get_dbr_guaranteed_mean(item->relic_bonus, min_attr, max_attr, chance_attr, 0)
       + get_dbr_guaranteed_mean(item->relic_name2, min_attr, max_attr, chance_attr, si2)
       + get_dbr_guaranteed_mean(item->relic_bonus2, min_attr, max_attr, chance_attr, 0));
}

// Returns min * duration from a single DBR, only if guaranteed (chance == 0 or >= 100).
// record_path: DBR path to load.
// min_attr: DBR attribute for minimum damage.
// dur_attr: DBR attribute for duration.
// chance_attr: DBR attribute for chance.
// shard_index: shard index for multi-value variables.
// Returns: total guaranteed DOT damage, or 0.0f.
static float
get_dbr_guaranteed_dot(const char *record_path, const char *min_attr,
                       const char *dur_attr, const char *chance_attr,
                       int shard_index)
{
  if(!record_path || !record_path[0])
    return(0.0f);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(0.0f);

  // Check chance -- if > 0 and < 100, not guaranteed
  TQVariable *cv = arz_record_get_var(data, arz_intern(chance_attr));

  if(cv && cv->count > 0)
  {
    float chance = (cv->type == TQ_VAR_INT) ? (float)cv->value.i32[0] : cv->value.f32[0];

    if(chance > 0 && chance < 100)
      return(0.0f);
  }

  TQVariable *mv = arz_record_get_var(data, arz_intern(min_attr));

  if(!mv || mv->count == 0)
    return(0.0f);

  int mi = (shard_index < (int)mv->count) ? shard_index : (int)mv->count - 1;

  if(mi < 0)
    mi = 0;

  float minv = (mv->type == TQ_VAR_INT) ? (float)mv->value.i32[mi] : mv->value.f32[mi];

  if(minv <= 0)
    return(0.0f);

  TQVariable *dv = arz_record_get_var(data, arz_intern(dur_attr));

  if(!dv || dv->count == 0)
    return(0.0f);

  int di = (shard_index < (int)dv->count) ? shard_index : (int)dv->count - 1;

  if(di < 0)
    di = 0;

  float durv = (dv->type == TQ_VAR_INT) ? (float)dv->value.i32[di] : dv->value.f32[di];

  return(minv * durv);
}

// Returns total DOT damage (min * duration) summed across all item components,
// only for guaranteed effects (chance == 0 or chance >= 100).
// item: the equipment item.
// min_attr: DBR attribute for minimum damage.
// dur_attr: DBR attribute for duration.
// chance_attr: DBR attribute for chance.
// Returns: total guaranteed DOT damage.
float
item_get_guaranteed_dot(TQItem *item, const char *min_attr, const char *dur_attr, const char *chance_attr)
{
  if(!item || !min_attr || !dur_attr || !chance_attr)
    return(0.0f);

  int si1 = item->var1 > 0 ? (int)item->var1 - 1 : 0;
  int si2 = item->var2 > 0 ? (int)item->var2 - 1 : 0;

  return(get_dbr_guaranteed_dot(item->base_name, min_attr, dur_attr, chance_attr, 0)
       + get_dbr_guaranteed_dot(item->prefix_name, min_attr, dur_attr, chance_attr, 0)
       + get_dbr_guaranteed_dot(item->suffix_name, min_attr, dur_attr, chance_attr, 0)
       + get_dbr_guaranteed_dot(item->relic_name, min_attr, dur_attr, chance_attr, si1)
       + get_dbr_guaranteed_dot(item->relic_bonus, min_attr, dur_attr, chance_attr, 0)
       + get_dbr_guaranteed_dot(item->relic_name2, min_attr, dur_attr, chance_attr, si2)
       + get_dbr_guaranteed_dot(item->relic_bonus2, min_attr, dur_attr, chance_attr, 0));
}

// public API

// Format stats for a character equipment item into buffer.
// item: the equipment item.
// tr: translation table for display names.
// buffer: output buffer.
// size: buffer capacity.
void
item_format_stats(TQItem *item, TQTranslation *tr, char *buffer, size_t size)
{
  item_format_stats_ex(item, tr, NULL, buffer, size);
}

// As item_format_stats, with optional requirement-reduction annotation.
void
item_format_stats_ex(TQItem *item, TQTranslation *tr,
                     const ItemReqReduction *reduction, char *buffer, size_t size)
{
  if(!item)
    return;

  format_stats_common(item->seed, item->base_name, item->prefix_name, item->suffix_name,
      item->relic_name, item->relic_bonus, item->var1,
      item->relic_name2, item->relic_bonus2, item->var2, tr, reduction,
      ITEM_FMT_DEFAULT, buffer, size);
}

// Format stats for a vault item into buffer.
// item: the vault item.
// tr: translation table for display names.
// buffer: output buffer.
// size: buffer capacity.
void
vault_item_format_stats(TQVaultItem *item, TQTranslation *tr, char *buffer, size_t size)
{
  vault_item_format_stats_ex(item, tr, NULL, buffer, size);
}

// As vault_item_format_stats, with optional requirement-reduction annotation.
void
vault_item_format_stats_ex(TQVaultItem *item, TQTranslation *tr,
                          const ItemReqReduction *reduction, char *buffer, size_t size)
{
  if(!item)
    return;

  format_stats_common(item->seed, item->base_name, item->prefix_name, item->suffix_name,
      item->relic_name, item->relic_bonus, item->var1,
      item->relic_name2, item->relic_bonus2, item->var2, tr, reduction,
      ITEM_FMT_DEFAULT, buffer, size);
}

// As vault_item_format_stats_ex, with ITEM_FMT_* rendering flags.
void
vault_item_format_stats_flags(TQVaultItem *item, TQTranslation *tr,
                              const ItemReqReduction *reduction,
                              unsigned flags, char *buffer, size_t size)
{
  if(!item)
    return;

  format_stats_common(item->seed, item->base_name, item->prefix_name, item->suffix_name,
      item->relic_name, item->relic_bonus, item->var1,
      item->relic_name2, item->relic_bonus2, item->var2, tr, reduction,
      flags, buffer, size);
}
