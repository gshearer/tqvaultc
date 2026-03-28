#include "item_stats.h"
#include "arz.h"
#include "asset_lookup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>
#include <stdarg.h>

// Initialize a BufWriter with the given buffer and size.
// w: the BufWriter to initialize.
// buffer: output buffer.
// size: buffer capacity.
void
buf_init(BufWriter *w, char *buffer, size_t size)
{
  w->buf = buffer;
  w->size = size;
  w->pos = 0;

  if(size > 0)
    buffer[0] = '\0';
}

// Append formatted text to a BufWriter.
// w: the BufWriter to append to.
// fmt: printf-style format string.
void
buf_write(BufWriter *w, const char *fmt, ...)
{
  if(w->pos >= w->size - 1)
    return;

  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(w->buf + w->pos, w->size - w->pos, fmt, ap);
  va_end(ap);

  if(n > 0)
    w->pos += (size_t)n;

  if(w->pos >= w->size)
    w->pos = w->size - 1;
}

// AttributeMap with interned pointers

typedef struct {
  const char *variable;
  const char *format;
  bool is_percent;
  const char *interned; // resolved at init time
} AttributeMap;

static AttributeMap attr_maps[] = {
  // Character stats
  {"characterStrength", "%d Strength", false, NULL},
  {"characterStrengthModifier", "+%d%% Strength", true, NULL},
  {"characterDexterity", "%d Dexterity", false, NULL},
  {"characterDexterityModifier", "+%d%% Dexterity", true, NULL},
  {"characterIntelligence", "%d Intelligence", false, NULL},
  {"characterIntelligenceModifier", "+%d%% Intelligence", true, NULL},
  {"characterLife", "%d Health", false, NULL},
  {"characterLifeModifier", "+%d%% Health", true, NULL},
  {"characterMana", "%d Energy", false, NULL},
  {"characterManaModifier", "+%d%% Energy", true, NULL},
  {"characterLifeRegen", "%+.1f Health Regeneration per second", false, NULL},
  {"characterManaRegen", "%+.1f Energy Regeneration per second", false, NULL},
  {"characterAttackSpeedModifier", "+%d%% Attack Speed", true, NULL},
  {"characterSpellCastSpeedModifier", "+%d%% Casting Speed", true, NULL},
  {"characterRunSpeedModifier", "+%d%% Movement Speed", true, NULL},
  {"characterDeflectProjectile", "%.0f%% Chance to Dodge Projectiles", false, NULL},
  {"characterDodgePercent", "%.0f%% Chance to Avoid Melee Attacks", false, NULL},
  {"characterEnergyAbsorptionPercent", "%.0f%% Energy Absorbed from Enemy Spells", false, NULL},

  // Offensive/Defensive ability
  {"characterOffensiveAbility", "+%d Offensive Ability", false, NULL},
  {"characterDefensiveAbility", "+%d Defensive Ability", false, NULL},
  {"characterOffensiveAbilityModifier", "+%d%% Offensive Ability", true, NULL},
  {"characterDefensiveAbilityModifier", "+%d%% Defensive Ability", true, NULL},

  // offensive*Modifier and offensiveTotalDamageModifier handled in dedicated block (may have Chance)

  // Offensive DoT modifiers
  {"offensiveSlowFireModifier", "+%d%% Burn Damage", true, NULL},
  {"offensiveSlowColdModifier", "+%d%% Frostburn Damage", true, NULL},
  {"offensiveSlowLightningModifier", "+%d%% Electrical Burn Damage", true, NULL},
  {"offensiveSlowPoisonModifier", "+%d%% Poison Damage", true, NULL},
  {"offensiveSlowLifeLeachModifier", "+%d%% Life Leech", true, NULL},
  {"offensiveSlowLifeModifier", "+%d%% Vitality Decay", true, NULL},

  // Armor
  {"defensiveProtection", "%d Armor", false, NULL},
  {"defensiveProtectionModifier", "+%d%% Armor", true, NULL},
  {"defensiveAbsorptionModifier", "+%d%% Armor Absorption", true, NULL},

  // Resistances -- all handled in dedicated block (may have Chance)
  // defensiveFire, defensiveCold, defensiveLightning, defensivePoison,
  //   defensivePierce, defensiveLife, defensiveBleeding, defensivePhysical
  {"defensiveStun", "%+d%% Stun Resistance", false, NULL},
  {"defensiveStunModifier", "+%d%% Reduced Stun Duration", true, NULL},
  {"defensiveConfusion", "%+d%% Confusion Resistance", false, NULL},
  {"defensiveConvert", "%+d%% Convert Resistance", false, NULL},
  {"defensiveReflect", "%d%% Damage Reflected", false, NULL},

  // Duration reductions
  {"defensiveFreeze", "%+d%% Reduced Freeze Duration", false, NULL},
  {"defensiveFreezeModifier", "+%d%% Reduced Freeze Duration", true, NULL},
  {"defensiveDisruption", "%.1f%% Reduced Skill Disruption", false, NULL},
  {"defensiveSlowLifeLeach", "%+d%% Vitality Decay Resistance", false, NULL},
  {"defensiveSlowManaLeach", "%+d%% Energy Drain Resistance", false, NULL},

  // Retaliation
  {"retaliationFireMin", "%d Fire Retaliation", false, NULL},
  {"retaliationColdMin", "%d Cold Retaliation", false, NULL},
  {"retaliationLightningMin", "%d Lightning Retaliation", false, NULL},
  {"retaliationPierceMin", "%d Pierce Retaliation", false, NULL},
  {"retaliationPhysicalMin", "%d Physical Retaliation", false, NULL},

  // Misc offensive
  {"offensivePierceRatioMin", "%.0f%% Pierce Ratio", false, NULL},
  {"piercingProjectile", "%d%% Chance to pass through Enemies", true, NULL},
  // offensiveManaBurnDrain* handled in dedicated block (has DamageRatio qualifier)

  // offensivePercentCurrentLifeMin handled in dedicated block (has Chance)

  // Flat damage (non-range, for bonus summaries)
  {"offensivePierceMin", "%d Pierce Damage", false, NULL},
  {"offensiveStunMin", "%.1f Second Stun", false, NULL},
  {"offensiveElementalMin", "%d Elemental Damage", false, NULL},
  // offensiveLifeMin handled in dedicated block (has Chance)
  // Offensive chance-based
  {"offensiveStunChance", "%.0f%% Chance to Stun", false, NULL},

  // Energy leech / drain over time
  {"offensiveSlowManaLeachMin", "%d Energy Leech over time", false, NULL},

  // Slow (total speed reduction)
  {"offensiveSlowTotalSpeedMin", "%.0f%% Reduced Total Speed", false, NULL},

  // Taunt
  {"offensiveTauntMin", "%.0f%% Taunt", false, NULL},

  // Retaliation DoT
  // retaliationSlow* DoTs handled in dedicated blocks (have Duration + Chance)

  // Shield
  {"defensiveBlockModifier", "+%d%% Shield Block Chance", true, NULL},
  {"defensiveBlockModifierChance", "+%d%% Shield Block Chance", true, NULL},

  // Poison/Disruption duration
  {"defensivePoisonDuration", "%+d%% Reduced Poison Duration", false, NULL},

  // Regen modifiers
  {"characterLifeRegenModifier", "+%d%% Health Regeneration", true, NULL},
  {"characterManaRegenModifier", "+%d%% Energy Regeneration", true, NULL},

  // Projectile speed
  {"skillProjectileSpeedModifier", "+%d%% Projectile Speed", true, NULL},

  // Misc
  {"characterTotalSpeedModifier", "%+d%% Total Speed", true, NULL},
  {"skillCooldownReduction", "-%.0f%% Recharge", false, NULL},
  {"skillManaCostReduction", "+%.0f%% Skill Energy Cost Reduction", false, NULL},
  {"augmentAllLevel", "+%d to all Skills", false, NULL},
  {"characterIncreasedExperience", "%+d%% Increased Experience", false, NULL},

  {NULL, NULL, false, NULL}
};

// hash-based lookup tables (built at init)

static GHashTable *g_skip_set = NULL;    // interned ptr -> (gpointer)1
static GHashTable *g_attr_map_ht = NULL; // interned ptr -> &attr_maps[i]

// Pre-interned variable name pointers for frequently used names
static const char *INT_offensivePhysicalMin, *INT_offensivePhysicalMax;
static const char *INT_offensiveFireMin, *INT_offensiveFireMax;
static const char *INT_offensiveColdMin, *INT_offensiveColdMax;
static const char *INT_offensiveLightningMin, *INT_offensiveLightningMax;
static const char *INT_offensivePoisonMin, *INT_offensivePoisonMax;
static const char *INT_offensivePierceMin, *INT_offensivePierceMax, *INT_offensivePierceChance;
static const char *INT_offensiveElementalMin, *INT_offensiveElementalMax;
static const char *INT_offensiveManaLeechMin, *INT_offensiveManaLeechMax;
static const char *INT_offensiveBasePhysicalMin, *INT_offensiveBasePhysicalMax;
static const char *INT_offensiveBaseColdMin, *INT_offensiveBaseColdMax;
static const char *INT_offensiveBaseFireMin, *INT_offensiveBaseFireMax;
static const char *INT_offensiveBaseLightningMin, *INT_offensiveBaseLightningMax;
static const char *INT_offensiveBasePoisonMin, *INT_offensiveBasePoisonMax;
static const char *INT_offensiveBaseLifeMin, *INT_offensiveBaseLifeMax;
static const char *INT_offensiveLifeMin, *INT_offensiveLifeMax, *INT_offensiveLifeChance;
static const char *INT_offensiveBonusPhysicalMin, *INT_offensiveBonusPhysicalMax, *INT_offensiveBonusPhysicalChance;
static const char *INT_offensiveLifeLeechMin, *INT_offensiveLifeLeechMax;
static const char *INT_offensiveSlowFireMin, *INT_offensiveSlowFireMax, *INT_offensiveSlowFireDurationMin, *INT_offensiveSlowFireChance;
static const char *INT_offensiveSlowLightningMin, *INT_offensiveSlowLightningMax, *INT_offensiveSlowLightningDurationMin, *INT_offensiveSlowLightningChance;
static const char *INT_offensiveSlowColdMin, *INT_offensiveSlowColdMax, *INT_offensiveSlowColdDurationMin, *INT_offensiveSlowColdChance;
static const char *INT_offensiveSlowPoisonMin, *INT_offensiveSlowPoisonMax, *INT_offensiveSlowPoisonDurationMin, *INT_offensiveSlowPoisonChance;
static const char *INT_offensiveSlowLifeLeachMin, *INT_offensiveSlowLifeLeachMax, *INT_offensiveSlowLifeLeachDurationMin, *INT_offensiveSlowLifeLeachChance;
static const char *INT_offensiveSlowLifeMin, *INT_offensiveSlowLifeMax, *INT_offensiveSlowLifeDurationMin, *INT_offensiveSlowLifeChance;
static const char *INT_offensiveSlowManaLeachMin, *INT_offensiveSlowManaLeachMax, *INT_offensiveSlowManaLeachDurationMin, *INT_offensiveSlowManaLeachChance;
static const char *INT_offensiveSlowBleedingMin, *INT_offensiveSlowBleedingMax, *INT_offensiveSlowBleedingDurationMin, *INT_offensiveSlowBleedingChance;
static const char *INT_offensiveSlowBleedingModifier, *INT_offensiveSlowBleedingModifierChance;
static const char *INT_offensiveSlowDefensiveReductionMin, *INT_offensiveSlowDefensiveReductionDurationMin;
static const char *INT_offensiveSlowAttackSpeedMin, *INT_offensiveSlowAttackSpeedDurationMin;
static const char *INT_offensiveSlowRunSpeedMin, *INT_offensiveSlowRunSpeedDurationMin;
static const char *INT_offensiveStunMin, *INT_offensiveStunDurationMin, *INT_offensiveStunChance;
static const char *INT_offensiveFumbleMin, *INT_offensiveFumbleDurationMin, *INT_offensiveFumbleChance;
static const char *INT_offensiveProjectileFumbleMin, *INT_offensiveProjectileFumbleDurationMin, *INT_offensiveProjectileFumbleChance;
static const char *INT_offensiveFreezeMin, *INT_offensiveFreezeDurationMin, *INT_offensiveFreezeChance;
static const char *INT_offensivePetrifyMin, *INT_offensivePetrifyDurationMin, *INT_offensivePetrifyChance;
static const char *INT_offensiveConfusionMin, *INT_offensiveConfusionDurationMin, *INT_offensiveConfusionChance;
static const char *INT_offensiveFearMin, *INT_offensiveFearMax, *INT_offensiveFearChance;
static const char *INT_offensiveConvertMin;
static const char *INT_retaliationSlowFireMin, *INT_retaliationSlowFireDurationMin, *INT_retaliationSlowFireChance;
static const char *INT_retaliationSlowColdMin, *INT_retaliationSlowColdDurationMin, *INT_retaliationSlowColdChance;
static const char *INT_retaliationSlowLightningMin, *INT_retaliationSlowLightningDurationMin, *INT_retaliationSlowLightningChance;
static const char *INT_retaliationSlowPoisonMin, *INT_retaliationSlowPoisonDurationMin, *INT_retaliationSlowPoisonChance;
static const char *INT_retaliationSlowLifeMin, *INT_retaliationSlowLifeDurationMin, *INT_retaliationSlowLifeChance;
static const char *INT_retaliationSlowBleedingMin, *INT_retaliationSlowBleedingDurationMin, *INT_retaliationSlowBleedingChance;
static const char *INT_offensivePhysicalModifier, *INT_offensivePhysicalModifierChance;
static const char *INT_offensiveFireModifier, *INT_offensiveFireModifierChance;
static const char *INT_offensiveColdModifier, *INT_offensiveColdModifierChance;
static const char *INT_offensiveLightningModifier, *INT_offensiveLightningModifierChance;
static const char *INT_offensivePoisonModifier, *INT_offensivePoisonModifierChance;
static const char *INT_offensiveLifeModifier, *INT_offensiveLifeModifierChance;
static const char *INT_offensivePierceModifier, *INT_offensivePierceModifierChance;
static const char *INT_offensiveElementalModifier, *INT_offensiveElementalModifierChance;
static const char *INT_offensiveTotalDamageModifier, *INT_offensiveTotalDamageModifierChance;
static const char *INT_defensivePhysical, *INT_defensivePhysicalChance;
static const char *INT_defensiveFire, *INT_defensiveFireChance;
static const char *INT_defensiveCold, *INT_defensiveColdChance;
static const char *INT_defensiveLightning, *INT_defensiveLightningChance;
static const char *INT_defensivePoison, *INT_defensivePoisonChance;
static const char *INT_defensivePierce, *INT_defensivePierceChance;
static const char *INT_defensiveLife, *INT_defensiveLifeChance;
static const char *INT_defensiveBleeding, *INT_defensiveBleedingChance;
static const char *INT_defensiveElementalResistance, *INT_defensiveElementalResistanceChance;
static const char *INT_offensivePercentCurrentLifeMin, *INT_offensivePercentCurrentLifeChance;
static const char *INT_offensiveTotalDamageReductionPercentMin, *INT_offensiveTotalDamageReductionPercentChance;
static const char *INT_offensiveTotalDamageReductionPercentDurationMin;
static const char *INT_offensiveManaBurnDrainMin, *INT_offensiveManaBurnDrainRatioMin, *INT_offensiveManaBurnDamageRatio;
static const char *INT_racialBonusPercentDamage, *INT_racialBonusPercentDefense, *INT_racialBonusRace;
static const char *INT_petBonusName;
static const char *INT_skillCooldownTime, *INT_refreshTime;
static const char *INT_skillTargetNumber, *INT_skillActiveDuration, *INT_skillTargetRadius;
static const char *INT_offensiveGlobalChance;
static const char *INT_offensiveSlowLightningDurationMax, *INT_offensiveSlowFireDurationMax;
static const char *INT_offensiveSlowColdDurationMax, *INT_offensiveSlowPoisonDurationMax;
static const char *INT_defensiveDisruption, *INT_defensiveDisruptionDuration;
const char *INT_itemNameTag, *INT_description, *INT_lootRandomizerName, *INT_FileDescription;
const char *INT_itemClassification, *INT_itemText;
const char *INT_characterBaseAttackSpeedTag, *INT_artifactClassification;
const char *INT_itemSkillName, *INT_buffSkillName, *INT_skillDisplayName;
const char *INT_itemSkillAutoController, *INT_triggerType, *INT_itemSkillLevel;
const char *INT_skillBaseDescription, *INT_petSkillName, *INT_skillChanceWeight;
const char *INT_itemSetName, *INT_setName, *INT_setMembers;
const char *INT_completedRelicLevel;
const char *INT_dexterityRequirement, *INT_intelligenceRequirement;
const char *INT_strengthRequirement, *INT_levelRequirement;
const char *INT_itemLevel, *INT_itemCostName, *INT_Class;

#define INTERN(name) INT_##name = arz_intern(#name)

// Initialize the item stats subsystem: pre-intern attribute names,
// build skip set and attr_map hash tables.
void
item_stats_init(void)
{
  // Pre-intern all attr_maps variable names
  for(int i = 0; attr_maps[i].variable; i++)
    attr_maps[i].interned = arz_intern(attr_maps[i].variable);

  // Build skip_set
  static const char *skip_var_names[] = {
    "offensivePhysicalMin", "offensivePhysicalMax",
    "offensiveFireMin", "offensiveFireMax",
    "offensiveColdMin", "offensiveColdMax",
    "offensiveLightningMin", "offensiveLightningMax",
    "offensivePoisonMin", "offensivePoisonMax",
    "offensivePierceMin", "offensivePierceMax", "offensivePierceChance",
    "offensiveElementalMin", "offensiveElementalMax",
    "offensiveLifeLeechMin", "offensiveLifeLeechMax",
    "offensiveManaLeechMin", "offensiveManaLeechMax",
    "offensiveSlowFireMin", "offensiveSlowFireMax", "offensiveSlowFireDurationMin", "offensiveSlowFireChance",
    "offensiveSlowLightningMin", "offensiveSlowLightningMax", "offensiveSlowLightningDurationMin", "offensiveSlowLightningChance",
    "offensiveSlowColdMin", "offensiveSlowColdMax", "offensiveSlowColdDurationMin", "offensiveSlowColdChance",
    "offensiveSlowPoisonMin", "offensiveSlowPoisonMax", "offensiveSlowPoisonDurationMin", "offensiveSlowPoisonChance",
    "offensiveSlowLifeLeachMin", "offensiveSlowLifeLeachMax", "offensiveSlowLifeLeachDurationMin", "offensiveSlowLifeLeachChance",
    "offensiveSlowLifeMin", "offensiveSlowLifeMax", "offensiveSlowLifeDurationMin", "offensiveSlowLifeChance",
    "offensiveSlowBleedingMin", "offensiveSlowBleedingMax", "offensiveSlowBleedingDurationMin", "offensiveSlowBleedingChance",
    "offensiveSlowManaLeachMin", "offensiveSlowManaLeachMax", "offensiveSlowManaLeachDurationMin", "offensiveSlowManaLeachChance",
    "offensiveSlowBleedingModifier", "offensiveSlowBleedingModifierChance",
    "offensiveSlowDefensiveReductionMin", "offensiveSlowDefensiveReductionDurationMin",
    "offensiveSlowAttackSpeedMin", "offensiveSlowAttackSpeedDurationMin",
    "offensiveSlowRunSpeedMin", "offensiveSlowRunSpeedDurationMin",
    "offensiveStunMin", "offensiveStunDurationMin", "offensiveStunChance",
    "offensiveFearMin", "offensiveFearMax", "offensiveFearChance",
    "offensiveConvertMin",
    "offensiveTotalDamageReductionPercentMin", "offensiveTotalDamageReductionPercentChance",
    "offensiveTotalDamageReductionPercentDurationMin",
    "offensivePhysicalModifier", "offensivePhysicalModifierChance",
    "offensiveFireModifier", "offensiveFireModifierChance",
    "offensiveColdModifier", "offensiveColdModifierChance",
    "offensiveLightningModifier", "offensiveLightningModifierChance",
    "offensivePoisonModifier", "offensivePoisonModifierChance",
    "offensiveLifeModifier", "offensiveLifeModifierChance",
    "offensivePierceModifier", "offensivePierceModifierChance",
    "offensiveElementalModifier", "offensiveElementalModifierChance",
    "offensiveTotalDamageModifier", "offensiveTotalDamageModifierChance",
    "offensivePercentCurrentLifeMin", "offensivePercentCurrentLifeChance",
    "offensiveManaBurnDrainMin", "offensiveManaBurnDrainRatioMin", "offensiveManaBurnDamageRatio",
    "offensiveGlobalChance",
    "offensiveBasePhysicalMin", "offensiveBasePhysicalMax",
    "offensiveBaseColdMin", "offensiveBaseColdMax",
    "offensiveBaseFireMin", "offensiveBaseFireMax",
    "offensiveBaseLightningMin", "offensiveBaseLightningMax",
    "offensiveBasePoisonMin", "offensiveBasePoisonMax",
    "offensiveBaseLifeMin", "offensiveBaseLifeMax",
    "offensiveLifeMin", "offensiveLifeMax", "offensiveLifeChance",
    "offensiveBonusPhysicalMin", "offensiveBonusPhysicalMax", "offensiveBonusPhysicalChance",
    "defensivePhysical", "defensivePhysicalChance",
    "defensiveFire", "defensiveFireChance",
    "defensiveCold", "defensiveColdChance",
    "defensiveLightning", "defensiveLightningChance",
    "defensivePoison", "defensivePoisonChance",
    "defensivePierce", "defensivePierceChance",
    "defensiveLife", "defensiveLifeChance",
    "defensiveBleeding", "defensiveBleedingChance",
    "defensiveElementalResistance", "defensiveElementalResistanceChance",
    "defensiveDisruption", "defensiveDisruptionDuration",
    "retaliationSlowFireMin", "retaliationSlowFireDurationMin", "retaliationSlowFireChance",
    "retaliationSlowColdMin", "retaliationSlowColdDurationMin", "retaliationSlowColdChance",
    "retaliationSlowLightningMin", "retaliationSlowLightningDurationMin", "retaliationSlowLightningChance",
    "retaliationSlowPoisonMin", "retaliationSlowPoisonDurationMin", "retaliationSlowPoisonChance",
    "retaliationSlowLifeMin", "retaliationSlowLifeDurationMin", "retaliationSlowLifeChance",
    "retaliationSlowBleedingMin", "retaliationSlowBleedingDurationMin", "retaliationSlowBleedingChance",
    "racialBonusPercentDamage", "racialBonusPercentDefense", "racialBonusRace",
    NULL
  };

  g_skip_set = g_hash_table_new(g_direct_hash, g_direct_equal);
  for(const char **sp = skip_var_names; *sp; sp++)
    g_hash_table_insert(g_skip_set, (gpointer)arz_intern(*sp), (gpointer)1);

  // Build attr_map_ht
  g_attr_map_ht = g_hash_table_new(g_direct_hash, g_direct_equal);
  for(int i = 0; attr_maps[i].variable; i++)
    g_hash_table_insert(g_attr_map_ht, (gpointer)attr_maps[i].interned, &attr_maps[i]);

  // Pre-intern all frequently used variable names
  INTERN(offensivePhysicalMin); INTERN(offensivePhysicalMax);
  INTERN(offensiveFireMin); INTERN(offensiveFireMax);
  INTERN(offensiveColdMin); INTERN(offensiveColdMax);
  INTERN(offensiveLightningMin); INTERN(offensiveLightningMax);
  INTERN(offensivePoisonMin); INTERN(offensivePoisonMax);
  INTERN(offensivePierceMin); INTERN(offensivePierceMax); INTERN(offensivePierceChance);
  INTERN(offensiveElementalMin); INTERN(offensiveElementalMax);
  INTERN(offensiveManaLeechMin); INTERN(offensiveManaLeechMax);
  INTERN(offensiveBasePhysicalMin); INTERN(offensiveBasePhysicalMax);
  INTERN(offensiveBaseColdMin); INTERN(offensiveBaseColdMax);
  INTERN(offensiveBaseFireMin); INTERN(offensiveBaseFireMax);
  INTERN(offensiveBaseLightningMin); INTERN(offensiveBaseLightningMax);
  INTERN(offensiveBasePoisonMin); INTERN(offensiveBasePoisonMax);
  INTERN(offensiveBaseLifeMin); INTERN(offensiveBaseLifeMax);
  INTERN(offensiveLifeMin); INTERN(offensiveLifeMax); INTERN(offensiveLifeChance);
  INTERN(offensiveBonusPhysicalMin); INTERN(offensiveBonusPhysicalMax); INTERN(offensiveBonusPhysicalChance);
  INTERN(offensiveLifeLeechMin); INTERN(offensiveLifeLeechMax);
  INTERN(offensiveSlowFireMin); INTERN(offensiveSlowFireMax); INTERN(offensiveSlowFireDurationMin); INTERN(offensiveSlowFireChance);
  INTERN(offensiveSlowLightningMin); INTERN(offensiveSlowLightningMax); INTERN(offensiveSlowLightningDurationMin); INTERN(offensiveSlowLightningChance);
  INTERN(offensiveSlowColdMin); INTERN(offensiveSlowColdMax); INTERN(offensiveSlowColdDurationMin); INTERN(offensiveSlowColdChance);
  INTERN(offensiveSlowPoisonMin); INTERN(offensiveSlowPoisonMax); INTERN(offensiveSlowPoisonDurationMin); INTERN(offensiveSlowPoisonChance);
  INTERN(offensiveSlowLifeLeachMin); INTERN(offensiveSlowLifeLeachMax); INTERN(offensiveSlowLifeLeachDurationMin); INTERN(offensiveSlowLifeLeachChance);
  INTERN(offensiveSlowLifeMin); INTERN(offensiveSlowLifeMax); INTERN(offensiveSlowLifeDurationMin); INTERN(offensiveSlowLifeChance);
  INTERN(offensiveSlowManaLeachMin); INTERN(offensiveSlowManaLeachMax); INTERN(offensiveSlowManaLeachDurationMin); INTERN(offensiveSlowManaLeachChance);
  INTERN(offensiveSlowBleedingMin); INTERN(offensiveSlowBleedingMax); INTERN(offensiveSlowBleedingDurationMin); INTERN(offensiveSlowBleedingChance);
  INTERN(offensiveSlowBleedingModifier); INTERN(offensiveSlowBleedingModifierChance);
  INTERN(offensiveSlowDefensiveReductionMin); INTERN(offensiveSlowDefensiveReductionDurationMin);
  INTERN(offensiveSlowAttackSpeedMin); INTERN(offensiveSlowAttackSpeedDurationMin);
  INTERN(offensiveSlowRunSpeedMin); INTERN(offensiveSlowRunSpeedDurationMin);
  INTERN(offensiveStunMin); INTERN(offensiveStunDurationMin); INTERN(offensiveStunChance);
  INTERN(offensiveFumbleMin); INTERN(offensiveFumbleDurationMin); INTERN(offensiveFumbleChance);
  INTERN(offensiveProjectileFumbleMin); INTERN(offensiveProjectileFumbleDurationMin); INTERN(offensiveProjectileFumbleChance);
  INTERN(offensiveFreezeMin); INTERN(offensiveFreezeDurationMin); INTERN(offensiveFreezeChance);
  INTERN(offensivePetrifyMin); INTERN(offensivePetrifyDurationMin); INTERN(offensivePetrifyChance);
  INTERN(offensiveConfusionMin); INTERN(offensiveConfusionDurationMin); INTERN(offensiveConfusionChance);
  INTERN(offensiveFearMin); INTERN(offensiveFearMax); INTERN(offensiveFearChance);
  INTERN(offensiveConvertMin);
  INTERN(retaliationSlowFireMin); INTERN(retaliationSlowFireDurationMin); INTERN(retaliationSlowFireChance);
  INTERN(retaliationSlowColdMin); INTERN(retaliationSlowColdDurationMin); INTERN(retaliationSlowColdChance);
  INTERN(retaliationSlowLightningMin); INTERN(retaliationSlowLightningDurationMin); INTERN(retaliationSlowLightningChance);
  INTERN(retaliationSlowPoisonMin); INTERN(retaliationSlowPoisonDurationMin); INTERN(retaliationSlowPoisonChance);
  INTERN(retaliationSlowLifeMin); INTERN(retaliationSlowLifeDurationMin); INTERN(retaliationSlowLifeChance);
  INTERN(retaliationSlowBleedingMin); INTERN(retaliationSlowBleedingDurationMin); INTERN(retaliationSlowBleedingChance);
  INTERN(offensivePhysicalModifier); INTERN(offensivePhysicalModifierChance);
  INTERN(offensiveFireModifier); INTERN(offensiveFireModifierChance);
  INTERN(offensiveColdModifier); INTERN(offensiveColdModifierChance);
  INTERN(offensiveLightningModifier); INTERN(offensiveLightningModifierChance);
  INTERN(offensivePoisonModifier); INTERN(offensivePoisonModifierChance);
  INTERN(offensiveLifeModifier); INTERN(offensiveLifeModifierChance);
  INTERN(offensivePierceModifier); INTERN(offensivePierceModifierChance);
  INTERN(offensiveElementalModifier); INTERN(offensiveElementalModifierChance);
  INTERN(offensiveTotalDamageModifier); INTERN(offensiveTotalDamageModifierChance);
  INTERN(defensivePhysical); INTERN(defensivePhysicalChance);
  INTERN(defensiveFire); INTERN(defensiveFireChance);
  INTERN(defensiveCold); INTERN(defensiveColdChance);
  INTERN(defensiveLightning); INTERN(defensiveLightningChance);
  INTERN(defensivePoison); INTERN(defensivePoisonChance);
  INTERN(defensivePierce); INTERN(defensivePierceChance);
  INTERN(defensiveLife); INTERN(defensiveLifeChance);
  INTERN(defensiveBleeding); INTERN(defensiveBleedingChance);
  INTERN(defensiveElementalResistance); INTERN(defensiveElementalResistanceChance);
  INTERN(offensivePercentCurrentLifeMin); INTERN(offensivePercentCurrentLifeChance);
  INTERN(offensiveManaBurnDrainMin); INTERN(offensiveManaBurnDrainRatioMin); INTERN(offensiveManaBurnDamageRatio);
  INTERN(offensiveTotalDamageReductionPercentMin); INTERN(offensiveTotalDamageReductionPercentChance);
  INTERN(offensiveTotalDamageReductionPercentDurationMin);
  INTERN(racialBonusPercentDamage); INTERN(racialBonusPercentDefense); INTERN(racialBonusRace);
  INTERN(petBonusName);
  INTERN(skillCooldownTime); INTERN(refreshTime);
  INTERN(skillTargetNumber); INTERN(skillActiveDuration); INTERN(skillTargetRadius);
  INTERN(offensiveGlobalChance);
  INTERN(offensiveSlowLightningDurationMax); INTERN(offensiveSlowFireDurationMax);
  INTERN(offensiveSlowColdDurationMax); INTERN(offensiveSlowPoisonDurationMax);
  INTERN(defensiveDisruption); INTERN(defensiveDisruptionDuration);
  INTERN(itemNameTag); INTERN(description); INTERN(lootRandomizerName); INTERN(FileDescription);
  INTERN(itemClassification); INTERN(itemText);
  INTERN(characterBaseAttackSpeedTag); INTERN(artifactClassification);
  INTERN(itemSkillName); INTERN(buffSkillName); INTERN(skillDisplayName);
  INTERN(itemSkillAutoController); INTERN(triggerType); INTERN(itemSkillLevel);
  INTERN(skillBaseDescription); INTERN(petSkillName); INTERN(skillChanceWeight);
  INTERN(itemSetName); INTERN(setName); INTERN(setMembers);
  INTERN(completedRelicLevel);
  INTERN(dexterityRequirement); INTERN(intelligenceRequirement);
  INTERN(strengthRequirement); INTERN(levelRequirement);
  INTERN(itemLevel); INTERN(itemCostName); INTERN(Class);
}

// Free all item stats resources (skip set and attr map hash tables).
void
item_stats_free(void)
{
  if(g_skip_set)
  {
    g_hash_table_destroy(g_skip_set);
    g_skip_set = NULL;
  }

  if(g_attr_map_ht)
  {
    g_hash_table_destroy(g_attr_map_ht);
    g_attr_map_ht = NULL;
  }
}

// helpers

// Fast variable lookup using interned name + shard index.
// data: pre-fetched record data.
// interned_name: interned variable name pointer.
// si: shard index.
// Returns: float value, or 0.0f if not found.
float
dbr_get_float_fast(TQArzRecordData *data, const char *interned_name, int si)
{
  TQVariable *v = arz_record_get_var(data, interned_name);

  if(!v || !v->value.i32)
    return(0.0f);

  int idx = (si < (int)v->count) ? si : (int)v->count - 1;

  if(idx < 0)
    idx = 0;

  return((v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx]);
}

// Get string variable from a pre-fetched record using interned name.
// data: pre-fetched record data.
// interned_name: interned variable name pointer.
// Returns: internal pointer (do NOT free), or NULL if not found.
const char*
record_get_string_fast(TQArzRecordData *data, const char *interned_name)
{
  if(!data)
    return(NULL);

  TQVariable *v = arz_record_get_var(data, interned_name);

  if(!v || v->type != TQ_VAR_STRING || v->count == 0)
    return(NULL);

  return(v->value.str[0]);
}

// Get string variable by loading a record path first.
// record_path: DBR path to load.
// interned_name: interned variable name pointer.
// Returns: internal pointer (do NOT free), or NULL if not found.
const char*
get_record_variable_string(const char *record_path, const char *interned_name)
{
  if(!record_path || !record_path[0])
    return(NULL);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(NULL);

  return(record_get_string_fast(data, interned_name));
}

// Determine the display color for an item based on its classification and path.
// base_name: DBR path to the base item.
// prefix_name: DBR path to the prefix record (may be NULL).
// suffix_name: DBR path to the suffix record (may be NULL).
// Returns: color string for markup.
const char*
get_item_color(const char *base_name, const char *prefix_name, const char *suffix_name)
{
  if(!base_name)
    return("white");

  // 1. BROKEN prefix check
  if(prefix_name && prefix_name[0])
  {
    const char *pfx_class = get_record_variable_string(prefix_name, INT_itemClassification);

    if(pfx_class && strcasecmp(pfx_class, "Broken") == 0)
      return("#999999");
  }

  // 2. Special item types by path (case-insensitive -- vault paths use mixed case)
  if(path_contains_ci(base_name, "\\artifacts\\") && !path_contains_ci(base_name, "\\arcaneformulae\\"))
    return("#00FFD1");

  if(path_contains_ci(base_name, "\\arcaneformulae\\"))
    return("#00FFD1");

  if(path_contains_ci(base_name, "\\scrolls\\"))
    return("#91CB00");

  if(path_contains_ci(base_name, "parchment"))
    return("#00A3FF");

  if(path_contains_ci(base_name, "\\relics\\") || path_contains_ci(base_name, "\\charms\\"))
    return("#FFAD00");

  if(path_contains_ci(base_name, "\\oneshot\\potion"))
    return("#FF0000");

  if(path_contains_ci(base_name, "quest"))
    return("#D905FF");

  // 3. Base item classification from DBR
  const char *base_class = get_record_variable_string(base_name, INT_itemClassification);

  if(base_class)
  {
    if(strcasecmp(base_class, "Epic") == 0)
      return("#00A3FF");

    if(strcasecmp(base_class, "Legendary") == 0)
      return("#D905FF");

    if(strcasecmp(base_class, "Rare") == 0)
      return("#40FF40");
  }

  // 4. Prefix/suffix classification == RARE
  if(prefix_name && prefix_name[0])
  {
    const char *pfx_class = get_record_variable_string(prefix_name, INT_itemClassification);

    if(pfx_class && strcasecmp(pfx_class, "Rare") == 0)
      return("#40FF40");
  }

  if(suffix_name && suffix_name[0])
  {
    const char *sfx_class = get_record_variable_string(suffix_name, INT_itemClassification);

    if(sfx_class && strcasecmp(sfx_class, "Rare") == 0)
      return("#40FF40");
  }

  // 5. Has any prefix or suffix -> common (yellow)
  if((prefix_name && prefix_name[0]) || (suffix_name && suffix_name[0]))
    return("#FFF52B");

  // 6. Default -> mundane (white)
  return("white");
}

// Derive a human-readable name from a DBR path when translation tags are missing.
// path: DBR path string.
// Returns: malloc'd string (caller must free), or "Unknown" on failure.
char*
pretty_name_from_path(const char *path)
{
  if(!path)
    return(strdup("Unknown"));

  const char *sep = strrchr(path, '\\');

  if(!sep)
    sep = strrchr(path, '/');

  const char *fname = sep ? sep + 1 : path;
  int len = (int)strlen(fname);

  if(len > 4 && strcasecmp(fname + len - 4, ".dbr") == 0)
    len -= 4;

  const char *start = fname;
  const char *end = fname + len;

  while(start < end && (*start >= '0' && *start <= '9'))
    start++;

  if(start < end && *start == '_')
    start++;

  const char *us = start;

  while(us < end && *us != '_')
    us++;

  if(us < end && *us == '_' && (us - start) <= 4)
    start = us + 1;

  char buf[256];
  int pos = 0;
  bool prev_lower = false;

  for(const char *p = start; p < end && pos < (int)sizeof(buf) - 2; p++)
  {
    char c = *p;

    if(c == '_')
    {
      if(pos > 0)
        buf[pos++] = ' ';
      prev_lower = false;
      continue;
    }

    if(prev_lower && c >= 'A' && c <= 'Z' && pos > 0)
      buf[pos++] = ' ';

    if((pos == 0 || buf[pos-1] == ' ') && c >= 'a' && c <= 'z')
      c = c - 32;

    buf[pos++] = c;
    prev_lower = (c >= 'a' && c <= 'z');
  }
  buf[pos] = '\0';

  if(pos == 0)
    return(strdup("Unknown"));

  return(strdup(buf));
}

// Escape Pango markup special characters in a string.
// Fast-path: scan first, strdup if clean.
// str: input string (may be NULL).
// Returns: malloc'd escaped string (caller must free).
char*
escape_markup(const char *str)
{
  if(!str)
    return(strdup(""));

  // Fast path: check if any special chars exist
  const char *p = str;

  for(; *p; p++)
  {
    if(*p == '&' || *p == '<' || *p == '>' || *p == '\'' || *p == '\"')
      goto slow_path;
  }
  return(strdup(str));

slow_path:;
  // Worst case: every char becomes &quot; (6 chars)
  size_t slen = strlen(str);
  char *out = malloc(slen * 6 + 1);

  if(!out)
    return(strdup(""));

  char *d = out;

  for(p = str; *p; p++)
  {
    switch(*p)
    {
      case '&': memcpy(d, "&amp;", 5); d += 5; break;
      case '<': memcpy(d, "&lt;", 4); d += 4; break;
      case '>': memcpy(d, "&gt;", 4); d += 4; break;
      case '\'': memcpy(d, "&apos;", 6); d += 6; break;
      case '\"': memcpy(d, "&quot;", 6); d += 6; break;
      default: *d++ = *p; break;
    }
  }
  *d = '\0';

  return(out);
}

// Build a short stat summary string from a bonus DBR record.
// record_path: DBR path to the bonus record.
// Returns: malloc'd summary string, or NULL if no stats found.
char *
item_bonus_stat_summary(const char *record_path)
{
  if(!record_path || !record_path[0])
    return(NULL);

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return(NULL);

  char buf[256];
  int found = 0;

  buf[0] = '\0';

  for(int a = 0; attr_maps[a].variable && found < 3; a++)
  {
    float val = dbr_get_float_fast(data, attr_maps[a].interned, 0);

    if(val == 0.0f)
      continue;

    if(found > 0)
    {
      size_t len = strlen(buf);

      if(len < sizeof(buf) - 3)
      {
        buf[len] = ',';
        buf[len+1] = ' ';
        buf[len+2] = '\0';
      }
    }

    char part[80];
    const char *pct = strchr(attr_maps[a].format, '%');
    bool fmt_is_float = false;

    if(pct)
    {
      const char *pp = pct + 1;

      while(*pp == '+' || *pp == '-' || *pp == '0' || *pp == ' ' || *pp == '#')
        pp++;

      while((*pp >= '0' && *pp <= '9') || *pp == '.')
        pp++;

      if(*pp == 'f')
        fmt_is_float = true;
    }

    if(attr_maps[a].is_percent || !fmt_is_float)
      snprintf(part, sizeof(part), attr_maps[a].format, (int)roundf(val));

    else
      snprintf(part, sizeof(part), attr_maps[a].format, val);

    size_t cur = strlen(buf);
    size_t plen = strlen(part);

    if(cur + plen < sizeof(buf) - 1)
      memcpy(buf + cur, part, plen + 1);

    found++;
  }

  // Also check stats handled in dedicated blocks (resistances, damage ranges)
  static const struct { const char **val; const char *fmt; } extra_simple[] = {
    {&INT_defensiveFire, "%+d%% Fire Resistance"},
    {&INT_defensiveCold, "%+d%% Cold Resistance"},
    {&INT_defensiveLightning, "%+d%% Lightning Resistance"},
    {&INT_defensivePoison, "%+d%% Poison Resistance"},
    {&INT_defensivePierce, "%+d%% Pierce Resistance"},
    {&INT_defensiveLife, "%+d%% Vitality Resistance"},
    {&INT_defensiveBleeding, "%+d%% Bleeding Resistance"},
    {&INT_defensivePhysical, "%+d%% Physical Resistance"},
  };

  for(int e = 0; e < (int)(sizeof extra_simple / sizeof extra_simple[0]) && found < 3; e++)
  {
    float val = dbr_get_float_fast(data, *extra_simple[e].val, 0);

    if(fabs(val) < 0.001f)
      continue;

    if(found > 0)
    {
      size_t len = strlen(buf);

      if(len < sizeof(buf) - 3)
      {
        buf[len] = ',';
        buf[len+1] = ' ';
        buf[len+2] = '\0';
      }
    }

    char part[80];

    snprintf(part, sizeof(part), extra_simple[e].fmt, (int)roundf(val));

    size_t cur = strlen(buf), plen = strlen(part);

    if(cur + plen < sizeof(buf) - 1)
      memcpy(buf + cur, part, plen + 1);

    found++;
  }

  // Damage ranges (min-max pairs from dedicated blocks), with optional chance
  static const struct { const char **mn; const char **mx; const char **chance; const char *label; } extra_dmg[] = {
    {&INT_offensivePhysicalMin,     &INT_offensivePhysicalMax,     NULL,                      "Physical Damage"},
    {&INT_offensiveFireMin,         &INT_offensiveFireMax,         NULL,                      "Fire Damage"},
    {&INT_offensiveColdMin,         &INT_offensiveColdMax,         NULL,                      "Cold Damage"},
    {&INT_offensiveLightningMin,    &INT_offensiveLightningMax,    NULL,                      "Lightning Damage"},
    {&INT_offensivePoisonMin,       &INT_offensivePoisonMax,       NULL,                      "Poison Damage"},
    {&INT_offensivePierceMin,       &INT_offensivePierceMax,       &INT_offensivePierceChance, "Pierce Damage"},
    {&INT_offensiveElementalMin,    &INT_offensiveElementalMax,    NULL,                      "Elemental Damage"},
    {&INT_offensiveBasePhysicalMin, &INT_offensiveBasePhysicalMax, NULL,                      "Physical Damage"},
    {&INT_offensiveBaseLifeMin,     &INT_offensiveBaseLifeMax,     NULL,                      "Vitality Damage"},
    {&INT_offensiveLifeMin,         &INT_offensiveLifeMax,         &INT_offensiveLifeChance,  "Vitality Damage"},
  };

  for(int e = 0; e < (int)(sizeof extra_dmg / sizeof extra_dmg[0]) && found < 3; e++)
  {
    float mn = dbr_get_float_fast(data, *extra_dmg[e].mn, 0);

    if(mn < 0.001f)
      continue;

    float mx = dbr_get_float_fast(data, *extra_dmg[e].mx, 0);
    float chance = extra_dmg[e].chance ? dbr_get_float_fast(data, *extra_dmg[e].chance, 0) : 0;

    if(found > 0)
    {
      size_t len = strlen(buf);

      if(len < sizeof(buf) - 3)
      {
        buf[len] = ',';
        buf[len+1] = ' ';
        buf[len+2] = '\0';
      }
    }

    char part[80];

    if(chance > 0)
    {
      if(mx > mn)
        snprintf(part, sizeof(part), "%.0f%% Chance of %.0f-%.0f %s", chance, mn, mx, extra_dmg[e].label);

      else
        snprintf(part, sizeof(part), "%.0f%% Chance of %.0f %s", chance, mn, extra_dmg[e].label);
    }

    else
    {
      if(mx > mn)
        snprintf(part, sizeof(part), "%.0f-%.0f %s", mn, mx, extra_dmg[e].label);

      else
        snprintf(part, sizeof(part), "%.0f %s", mn, extra_dmg[e].label);
    }

    size_t cur = strlen(buf), plen = strlen(part);

    if(cur + plen < sizeof(buf) - 1)
      memcpy(buf + cur, part, plen + 1);

    found++;
  }

  // Energy drain (both DrainMin and DrainRatioMin are percentages)
  if(found < 3)
  {
    float drain = dbr_get_float_fast(data, INT_offensiveManaBurnDrainMin, 0);
    float drain_ratio = dbr_get_float_fast(data, INT_offensiveManaBurnDrainRatioMin, 0);
    float dmg_ratio = dbr_get_float_fast(data, INT_offensiveManaBurnDamageRatio, 0);
    float val = (drain > 0.001f) ? drain : drain_ratio;

    if(val > 0.001f)
    {
      if(found > 0)
      {
        size_t len = strlen(buf);

        if(len < sizeof(buf) - 3)
        {
          buf[len] = ',';
          buf[len+1] = ' ';
          buf[len+2] = '\0';
        }
      }

      char part[80];

      if(dmg_ratio > 0.001f)
        snprintf(part, sizeof(part), "%.0f%% Energy Drain (%.0f%% as damage)", val, dmg_ratio);

      else
        snprintf(part, sizeof(part), "%.0f%% Energy Drain", val);

      size_t cur = strlen(buf), plen = strlen(part);

      if(cur + plen < sizeof(buf) - 1)
        memcpy(buf + cur, part, plen + 1);

      found++;
    }
  }

  // Racial bonus
  if(found < 3)
  {
    const char *race = "Enemies";
    TQVariable *rv = arz_record_get_var(data, INT_racialBonusRace);

    if(rv && rv->type == TQ_VAR_STRING && rv->count > 0 && rv->value.str[0])
      race = rv->value.str[0];

    float rdmg = dbr_get_float_fast(data, INT_racialBonusPercentDamage, 0);

    if(fabs(rdmg) > 0.001f && found < 3)
    {
      if(found > 0)
      {
        size_t len = strlen(buf);

        if(len < sizeof(buf) - 3)
        {
          buf[len] = ',';
          buf[len+1] = ' ';
          buf[len+2] = '\0';
        }
      }

      char part[80];

      snprintf(part, sizeof(part), "+%d%% Damage to %ss", (int)roundf(rdmg), race);

      size_t cur = strlen(buf), plen = strlen(part);

      if(cur + plen < sizeof(buf) - 1)
        memcpy(buf + cur, part, plen + 1);

      found++;
    }

    float rdef = dbr_get_float_fast(data, INT_racialBonusPercentDefense, 0);

    if(fabs(rdef) > 0.001f && found < 3)
    {
      if(found > 0)
      {
        size_t len = strlen(buf);

        if(len < sizeof(buf) - 3)
        {
          buf[len] = ',';
          buf[len+1] = ' ';
          buf[len+2] = '\0';
        }
      }

      char part[80];

      snprintf(part, sizeof(part), "%d%% less damage from %ss", (int)roundf(rdef), race);

      size_t cur = strlen(buf), plen = strlen(part);

      if(cur + plen < sizeof(buf) - 1)
        memcpy(buf + cur, part, plen + 1);

      found++;
    }
  }

  // Pet bonus
  if(found == 0)
  {
    const char *pet = record_get_string_fast(data, INT_petBonusName);

    if(pet && pet[0])
    {
      char *pet_summary = item_bonus_stat_summary(pet);

      if(pet_summary)
      {
        snprintf(buf, sizeof(buf), "Pets: %s", pet_summary);
        free(pet_summary);
        found = 1;
      }

      else
      {
        snprintf(buf, sizeof(buf), "Bonus to All Pets");
        found = 1;
      }
    }
  }

  if(found == 0)
    return(NULL);

  return(strdup(buf));
}

// Append all stat lines from a single DBR record to a BufWriter.
// record_path: DBR path to load.
// tr: translation table.
// w: BufWriter to append to.
// color: markup color string.
// shard_index: shard index for multi-value variables.
void
add_stats_from_record(const char *record_path, TQTranslation *tr, BufWriter *w, const char *color, int shard_index)
{
  if(!record_path || !record_path[0])
    return;

  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
    return;

  // Detect "X% Chance of:" conditional wrapper -- offensive effects go to
  // a secondary buffer and are flushed at the end with header + blank lines
  float global_chance = dbr_get_float_fast(data, INT_offensiveGlobalChance, shard_index);
  const char *indent = "";
  char chance_buf[4096];
  BufWriter chance_writer;
  BufWriter *ow = w;

  // For weapon records, flat damage is the base weapon damage and should not
  // be wrapped inside the "X% Chance of:" group.
  bool is_weapon = false;

  if(global_chance > 0)
  {
    const char *cls = record_get_string_fast(data, INT_Class);

    if(cls && strncasecmp(cls, "Weapon", 6) == 0)
      is_weapon = true;

    buf_init(&chance_writer, chance_buf, sizeof(chance_buf));
    ow = &chance_writer;
    indent = "  ";
  }

  // Flat damage ranges (min-max), with optional chance qualifier
  // For weapons with offensiveGlobalChance, write damage to main writer (w)
  // so base weapon damage is always shown; other effects go to chance_writer.
  {
    static struct { const char **min_int; const char **max_int; const char **chance_int; const char *label; } damage_types[] = {
      {&INT_offensivePhysicalMin, &INT_offensivePhysicalMax, NULL, "Physical Damage"},
      {&INT_offensiveFireMin, &INT_offensiveFireMax, NULL, "Fire Damage"},
      {&INT_offensiveColdMin, &INT_offensiveColdMax, NULL, "Cold Damage"},
      {&INT_offensiveLightningMin, &INT_offensiveLightningMax, NULL, "Lightning Damage"},
      {&INT_offensivePoisonMin, &INT_offensivePoisonMax, NULL, "Poison Damage"},
      {&INT_offensivePierceMin, &INT_offensivePierceMax, &INT_offensivePierceChance, "Piercing Damage"},
      {&INT_offensiveElementalMin, &INT_offensiveElementalMax, NULL, "Elemental Damage"},
      {&INT_offensiveManaLeechMin, &INT_offensiveManaLeechMax, NULL, "Mana Leech"},
      {&INT_offensiveBasePhysicalMin, &INT_offensiveBasePhysicalMax, NULL, "Physical Damage"},
      {&INT_offensiveBaseColdMin, &INT_offensiveBaseColdMax, NULL, "Cold Damage"},
      {&INT_offensiveBaseFireMin, &INT_offensiveBaseFireMax, NULL, "Fire Damage"},
      {&INT_offensiveBaseLightningMin, &INT_offensiveBaseLightningMax, NULL, "Lightning Damage"},
      {&INT_offensiveBasePoisonMin, &INT_offensiveBasePoisonMax, NULL, "Poison Damage"},
      {&INT_offensiveBaseLifeMin, &INT_offensiveBaseLifeMax, NULL, "Vitality Damage"},
      {&INT_offensiveLifeMin, &INT_offensiveLifeMax, &INT_offensiveLifeChance, "Vitality Damage"},
      {&INT_offensiveBonusPhysicalMin, &INT_offensiveBonusPhysicalMax, &INT_offensiveBonusPhysicalChance, "Physical Damage"},
      {NULL, NULL, NULL, NULL}
    };

    BufWriter *dw = is_weapon ? w : ow;

    for(int d = 0; damage_types[d].min_int; d++)
    {
      float mn = dbr_get_float_fast(data, *damage_types[d].min_int, shard_index);
      float mx = dbr_get_float_fast(data, *damage_types[d].max_int, shard_index);

      if(mn > 0)
      {
        float chance = damage_types[d].chance_int ? dbr_get_float_fast(data, *damage_types[d].chance_int, shard_index) : 0;
        const char *dmg_label = damage_types[d].label;

        if(chance > 0)
        {
          if(mx > mn)
            buf_write(dw, "<span color='%s'>%s%.1f%% Chance of %d - %d %s</span>\n", color, is_weapon ? "" : indent, chance, (int)round(mn), (int)round(mx), dmg_label);

          else
            buf_write(dw, "<span color='%s'>%s%.1f%% Chance of %d %s</span>\n", color, is_weapon ? "" : indent, chance, (int)round(mn), dmg_label);
        }

        else
        {
          if(mx > mn)
            buf_write(dw, "<span color='%s'>%s%d - %d %s</span>\n", color, is_weapon ? "" : indent, (int)round(mn), (int)round(mx), dmg_label);

          else
            buf_write(dw, "<span color='%s'>%s%d %s</span>\n", color, is_weapon ? "" : indent, (int)round(mn), dmg_label);
        }
      }
    }
  }

  // ADCTH: attack damage converted to health
  {
    float mn = dbr_get_float_fast(data, INT_offensiveLifeLeechMin, shard_index);
    float mx = dbr_get_float_fast(data, INT_offensiveLifeLeechMax, shard_index);

    if(mn > 0)
    {
      if(mx > mn)
        buf_write(ow, "<span color='%s'>%s%d%% - %d%% Attack Damage Converted to Health</span>\n", color, indent, (int)round(mn), (int)round(mx));

      else
        buf_write(ow, "<span color='%s'>%s%d%% Attack Damage Converted to Health</span>\n", color, indent, (int)round(mn));
    }
  }

  // DoT: all damage-over-time types with optional chance
  {
    static struct { const char **min_int; const char **max_int; const char **dur_int; const char **chance_int; const char *label; } dot_types[] = {
      {&INT_offensiveSlowFireMin,      &INT_offensiveSlowFireMax,      &INT_offensiveSlowFireDurationMin,      &INT_offensiveSlowFireChance,      "Burn Damage"},
      {&INT_offensiveSlowLightningMin, &INT_offensiveSlowLightningMax, &INT_offensiveSlowLightningDurationMin, &INT_offensiveSlowLightningChance, "Electrical Burn Damage"},
      {&INT_offensiveSlowColdMin,      &INT_offensiveSlowColdMax,      &INT_offensiveSlowColdDurationMin,      &INT_offensiveSlowColdChance,      "Frostburn Damage"},
      {&INT_offensiveSlowPoisonMin,    &INT_offensiveSlowPoisonMax,    &INT_offensiveSlowPoisonDurationMin,    &INT_offensiveSlowPoisonChance,    "Poison Damage"},
      {&INT_offensiveSlowLifeLeachMin, &INT_offensiveSlowLifeLeachMax, &INT_offensiveSlowLifeLeachDurationMin, &INT_offensiveSlowLifeLeachChance, "Life Leech"},
      {&INT_offensiveSlowLifeMin,      &INT_offensiveSlowLifeMax,      &INT_offensiveSlowLifeDurationMin,      &INT_offensiveSlowLifeChance,      "Vitality Decay"},
      {&INT_offensiveSlowManaLeachMin, &INT_offensiveSlowManaLeachMax, &INT_offensiveSlowManaLeachDurationMin, &INT_offensiveSlowManaLeachChance, "Energy Leech"},
      {&INT_offensiveSlowBleedingMin,  &INT_offensiveSlowBleedingMax,  &INT_offensiveSlowBleedingDurationMin,  &INT_offensiveSlowBleedingChance,  "Bleeding Damage"},
      {NULL, NULL, NULL, NULL, NULL}
    };

    for(int d = 0; dot_types[d].min_int; d++)
    {
      float mn = dbr_get_float_fast(data, *dot_types[d].min_int, shard_index);
      float mx = dbr_get_float_fast(data, *dot_types[d].max_int, shard_index);
      float dur = dbr_get_float_fast(data, *dot_types[d].dur_int, shard_index);

      if(mn > 0 && dur > 0)
      {
        float chance = dbr_get_float_fast(data, *dot_types[d].chance_int, shard_index);
        const char *lbl = dot_types[d].label;

        if(chance > 0)
        {
          if(mx > mn)
            buf_write(ow, "<span color='%s'>%s%.1f%% Chance of %.0f - %.0f %s over %.1f Seconds</span>\n", color, indent, chance, mn * dur, mx * dur, lbl, dur);

          else
            buf_write(ow, "<span color='%s'>%s%.1f%% Chance of %.0f %s over %.1f Seconds</span>\n", color, indent, chance, mn * dur, lbl, dur);
        }

        else
        {
          if(mx > mn)
            buf_write(ow, "<span color='%s'>%s%.0f - %.0f %s over %.1f Seconds</span>\n", color, indent, mn * dur, mx * dur, lbl, dur);

          else
            buf_write(ow, "<span color='%s'>%s%.0f %s over %.1f Seconds</span>\n", color, indent, mn * dur, lbl, dur);
        }
      }
    }
  }

  // Bleeding damage modifier with chance
  {
    float mod = dbr_get_float_fast(data, INT_offensiveSlowBleedingModifier, shard_index);
    float chance = dbr_get_float_fast(data, INT_offensiveSlowBleedingModifierChance, shard_index);

    if(fabs(mod) > 0.001f && chance > 0)
      buf_write(ow, "<span color='%s'>%s%.1f%% Chance of +%d%% Bleeding Damage</span>\n", color, indent, chance, (int)round(mod));

    else if(fabs(mod) > 0.001f)
      buf_write(ow, "<span color='%s'>%s+%d%% Bleeding Damage</span>\n", color, indent, (int)round(mod));
  }

  // Retaliation DoTs (damage over time triggered on retaliation, may have chance)
  {
    static const struct {
      const char **val; const char **dur; const char **chance; const char *label;
    } retal_dots[] = {
      {&INT_retaliationSlowFireMin,      &INT_retaliationSlowFireDurationMin,      &INT_retaliationSlowFireChance,      "Burn Retaliation"},
      {&INT_retaliationSlowColdMin,      &INT_retaliationSlowColdDurationMin,      &INT_retaliationSlowColdChance,      "Frostburn Retaliation"},
      {&INT_retaliationSlowLightningMin, &INT_retaliationSlowLightningDurationMin, &INT_retaliationSlowLightningChance, "Electrical Burn Retaliation"},
      {&INT_retaliationSlowPoisonMin,    &INT_retaliationSlowPoisonDurationMin,    &INT_retaliationSlowPoisonChance,    "Poison Retaliation"},
      {&INT_retaliationSlowLifeMin,      &INT_retaliationSlowLifeDurationMin,      &INT_retaliationSlowLifeChance,      "Vitality Decay Retaliation"},
      {&INT_retaliationSlowBleedingMin,  &INT_retaliationSlowBleedingDurationMin,  &INT_retaliationSlowBleedingChance,  "Bleeding Retaliation"},
    };

    for(int ri = 0; ri < (int)(sizeof retal_dots / sizeof retal_dots[0]); ri++)
    {
      float mn = dbr_get_float_fast(data, *retal_dots[ri].val, shard_index);
      float dur = dbr_get_float_fast(data, *retal_dots[ri].dur, shard_index);

      if(mn <= 0 || dur <= 0)
        continue;

      float ch = dbr_get_float_fast(data, *retal_dots[ri].chance, shard_index);

      if(ch > 0 && ch < 100)
        buf_write(w, "<span color='%s'>%s%.0f%% Chance of %.0f %s over %.1f Seconds</span>\n",
                  color, indent, ch, mn * dur, retal_dots[ri].label, dur);

      else
        buf_write(w, "<span color='%s'>%s%.0f %s over %.1f Seconds</span>\n",
                  color, indent, mn * dur, retal_dots[ri].label, dur);
    }
  }

  // Reduced Armor (value + duration)
  {
    float val = dbr_get_float_fast(data, INT_offensiveSlowDefensiveReductionMin, shard_index);
    float dur = dbr_get_float_fast(data, INT_offensiveSlowDefensiveReductionDurationMin, shard_index);

    if(val > 0 && dur > 0)
      buf_write(ow, "<span color='%s'>%s%.0f Reduced Armor for %.1f Second(s)</span>\n", color, indent, val, dur);

    else if(val > 0)
      buf_write(ow, "<span color='%s'>%s%.0f Reduced Armor</span>\n", color, indent, val);
  }

  // Skill disruption (chance + duration)
  {
    float chance = dbr_get_float_fast(data, INT_defensiveDisruption, shard_index);
    float dur = dbr_get_float_fast(data, INT_defensiveDisruptionDuration, shard_index);

    if(chance > 0 && dur > 0)
      buf_write(ow, "<span color='%s'>%s%.1f%% Chance of %.1f Second(s) of Skill Disruption</span>\n", color, indent, chance, dur);

    else if(chance > 0)
      buf_write(ow, "<span color='%s'>%s%.1f%% Skill Disruption</span>\n", color, indent, chance);
  }

  // Reduced Attack Speed (value + duration)
  {
    float val = dbr_get_float_fast(data, INT_offensiveSlowAttackSpeedMin, shard_index);
    float dur = dbr_get_float_fast(data, INT_offensiveSlowAttackSpeedDurationMin, shard_index);

    if(val > 0 && dur > 0)
      buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Attack Speed for %.1f Second(s)</span>\n", color, indent, val, dur);

    else if(val > 0)
      buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Attack Speed</span>\n", color, indent, val);
  }

  // Reduced Run Speed (value + duration)
  {
    float val = dbr_get_float_fast(data, INT_offensiveSlowRunSpeedMin, shard_index);
    float dur = dbr_get_float_fast(data, INT_offensiveSlowRunSpeedDurationMin, shard_index);

    if(val > 0 && dur > 0)
      buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Run Speed for %.1f Second(s)</span>\n", color, indent, val, dur);

    else if(val > 0)
      buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Run Speed</span>\n", color, indent, val);
  }

  // Offensive damage modifiers (may have chance)
  {
    static const struct { const char **val; const char **chance; const char *label; } off_mod_defs[] = {
      {&INT_offensivePhysicalModifier,  &INT_offensivePhysicalModifierChance,  "Physical Damage"},
      {&INT_offensiveFireModifier,      &INT_offensiveFireModifierChance,      "Fire Damage"},
      {&INT_offensiveColdModifier,      &INT_offensiveColdModifierChance,      "Cold Damage"},
      {&INT_offensiveLightningModifier, &INT_offensiveLightningModifierChance, "Lightning Damage"},
      {&INT_offensivePoisonModifier,    &INT_offensivePoisonModifierChance,    "Poison Damage"},
      {&INT_offensiveLifeModifier,      &INT_offensiveLifeModifierChance,      "Vitality Damage"},
      {&INT_offensivePierceModifier,    &INT_offensivePierceModifierChance,    "Pierce Damage"},
      {&INT_offensiveElementalModifier, &INT_offensiveElementalModifierChance, "Elemental Damage"},
      {&INT_offensiveTotalDamageModifier, &INT_offensiveTotalDamageModifierChance, "Total Damage"},
    };

    for(int mi = 0; mi < (int)(sizeof off_mod_defs / sizeof off_mod_defs[0]); mi++)
    {
      float mv = dbr_get_float_fast(data, *off_mod_defs[mi].val, shard_index);

      if(fabs(mv) < 0.001f)
        continue;

      float mc = dbr_get_float_fast(data, *off_mod_defs[mi].chance, shard_index);

      if(mc > 0 && mc < 100)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of +%d%% %s</span>\n", color, indent, mc, (int)round(mv), off_mod_defs[mi].label);

      else
        buf_write(ow, "<span color='%s'>%s+%d%% %s</span>\n", color, indent, (int)round(mv), off_mod_defs[mi].label);
    }
  }

  // Percent current life reduction (may have chance)
  {
    float pcl = dbr_get_float_fast(data, INT_offensivePercentCurrentLifeMin, shard_index);

    if(fabs(pcl) > 0.001f)
    {
      float pcl_chance = dbr_get_float_fast(data, INT_offensivePercentCurrentLifeChance, shard_index);

      if(pcl_chance > 0 && pcl_chance < 100)
        buf_write(ow, "<span color='%s'>%s%.1f%% Chance of %.0f%% Reduction to Enemy's Health</span>\n", color, indent, pcl_chance, pcl);

      else
        buf_write(ow, "<span color='%s'>%s%.0f%% Reduction to Enemy's Health</span>\n", color, indent, pcl);
    }
  }

  // Energy drain / energy burn (both DrainMin and DrainRatioMin are percentages)
  {
    float drain = dbr_get_float_fast(data, INT_offensiveManaBurnDrainMin, shard_index);
    float drain_ratio = dbr_get_float_fast(data, INT_offensiveManaBurnDrainRatioMin, shard_index);
    float dmg_ratio = dbr_get_float_fast(data, INT_offensiveManaBurnDamageRatio, shard_index);
    float val = (drain > 0.001f) ? drain : drain_ratio;

    if(val > 0.001f)
    {
      if(dmg_ratio > 0.001f)
        buf_write(ow, "<span color='%s'>%s%.0f%% Energy Drain (%.0f%% of lost energy as damage)</span>\n", color, indent, val, dmg_ratio);

      else
        buf_write(ow, "<span color='%s'>%s%.0f%% Energy Drain</span>\n", color, indent, val);
    }
  }

  // Offensive stun
  {
    float stun_min = dbr_get_float_fast(data, INT_offensiveStunMin, shard_index);
    float stun_dur = dbr_get_float_fast(data, INT_offensiveStunDurationMin, shard_index);

    if(stun_dur <= 0)
      stun_dur = stun_min;

    float stun_chance = dbr_get_float_fast(data, INT_offensiveStunChance, shard_index);

    if(stun_dur > 0)
    {
      if(stun_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Stun</span>\n", color, indent, stun_chance, stun_dur);

      else
        buf_write(ow, "<span color='%s'>%s%.1f Second(s) of Stun</span>\n", color, indent, stun_dur);
    }
  }

  // Offensive fumble (chance to fumble attacks -- melee)
  {
    float fumble_min = dbr_get_float_fast(data, INT_offensiveFumbleMin, shard_index);
    float fumble_dur = dbr_get_float_fast(data, INT_offensiveFumbleDurationMin, shard_index);

    if(fumble_dur <= 0)
      fumble_dur = fumble_min;

    if(fumble_dur > 0)
    {
      float fumble_chance = dbr_get_float_fast(data, INT_offensiveFumbleChance, shard_index);

      if(fumble_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance to Fumble Attacks for %.1f Seconds</span>\n", color, indent, fumble_chance, fumble_dur);

      else
        buf_write(ow, "<span color='%s'>%sChance to Fumble Attacks for %.1f Seconds</span>\n", color, indent, fumble_dur);
    }
  }

  // Offensive projectile fumble (impaired aim -- ranged)
  {
    float pfumble_min = dbr_get_float_fast(data, INT_offensiveProjectileFumbleMin, shard_index);
    float pfumble_dur = dbr_get_float_fast(data, INT_offensiveProjectileFumbleDurationMin, shard_index);

    if(pfumble_dur <= 0)
      pfumble_dur = pfumble_min;

    if(pfumble_dur > 0)
    {
      float pfumble_chance = dbr_get_float_fast(data, INT_offensiveProjectileFumbleChance, shard_index);

      if(pfumble_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of Impaired Aim for %.1f Seconds</span>\n", color, indent, pfumble_chance, pfumble_dur);

      else
        buf_write(ow, "<span color='%s'>%sChance of Impaired Aim for %.1f Seconds</span>\n", color, indent, pfumble_dur);
    }
  }

  // Offensive freeze
  {
    float freeze_min = dbr_get_float_fast(data, INT_offensiveFreezeMin, shard_index);
    float freeze_dur = dbr_get_float_fast(data, INT_offensiveFreezeDurationMin, shard_index);

    if(freeze_dur <= 0)
      freeze_dur = freeze_min;

    if(freeze_dur > 0)
    {
      float freeze_chance = dbr_get_float_fast(data, INT_offensiveFreezeChance, shard_index);

      if(freeze_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Freeze</span>\n", color, indent, freeze_chance, freeze_dur);

      else
        buf_write(ow, "<span color='%s'>%s%.1f Second(s) of Freeze</span>\n", color, indent, freeze_dur);
    }
  }

  // Offensive petrify
  {
    float petrify_min = dbr_get_float_fast(data, INT_offensivePetrifyMin, shard_index);
    float petrify_dur = dbr_get_float_fast(data, INT_offensivePetrifyDurationMin, shard_index);

    if(petrify_dur <= 0)
      petrify_dur = petrify_min;

    if(petrify_dur > 0)
    {
      float petrify_chance = dbr_get_float_fast(data, INT_offensivePetrifyChance, shard_index);

      if(petrify_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Petrify</span>\n", color, indent, petrify_chance, petrify_dur);

      else
        buf_write(ow, "<span color='%s'>%s%.1f Second(s) of Petrify</span>\n", color, indent, petrify_dur);
    }
  }

  // Mind control (offensiveConvertMin -- duration-based conversion of enemies)
  {
    float convert_min = dbr_get_float_fast(data, INT_offensiveConvertMin, shard_index);

    if(convert_min > 0)
      buf_write(ow, "<span color='%s'>%s%.1f Seconds of Mind Control</span>\n", color, indent, convert_min);
  }

  // Offensive confusion
  {
    float confuse_min = dbr_get_float_fast(data, INT_offensiveConfusionMin, shard_index);
    float confuse_dur = dbr_get_float_fast(data, INT_offensiveConfusionDurationMin, shard_index);

    if(confuse_min > 0)
    {
      float confuse_chance = dbr_get_float_fast(data, INT_offensiveConfusionChance, shard_index);

      if(confuse_dur > 0)
      {
        if(confuse_chance > 0)
          buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Confusion</span>\n", color, indent, confuse_chance, confuse_dur);

        else
          buf_write(ow, "<span color='%s'>%s%.1f Second(s) of Confusion</span>\n", color, indent, confuse_dur);
      }

      else
        buf_write(ow, "<span color='%s'>%s%.1f Second(s) of Confusion</span>\n", color, indent, confuse_min);
    }
  }

  // Fear
  {
    float fear_min = dbr_get_float_fast(data, INT_offensiveFearMin, shard_index);
    float fear_max = dbr_get_float_fast(data, INT_offensiveFearMax, shard_index);

    if(fear_min > 0)
    {
      if(fear_max > fear_min)
        buf_write(ow, "<span color='%s'>%s%.1f - %.1f Second(s) of Fear</span>\n", color, indent, fear_min, fear_max);

      else
        buf_write(ow, "<span color='%s'>%s%.1f Second(s) of Fear</span>\n", color, indent, fear_min);
    }
  }

  // Total damage reduction (debuff applied to enemies)
  {
    float tdmg_min = dbr_get_float_fast(data, INT_offensiveTotalDamageReductionPercentMin, shard_index);
    float tdmg_dur = dbr_get_float_fast(data, INT_offensiveTotalDamageReductionPercentDurationMin, shard_index);

    if(tdmg_min > 0)
    {
      if(tdmg_dur > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Damage for %.1f Second(s)</span>\n", color, indent, tdmg_min, tdmg_dur);

      else
        buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Damage</span>\n", color, indent, tdmg_min);
    }
  }

  // -- End of offensive sections --
  // (non-offensive sections follow below, then chance group is flushed at the end)

  // Racial bonus
  {
    const char *race = "Enemies";
    TQVariable *rv = arz_record_get_var(data, INT_racialBonusRace);

    if(rv && rv->type == TQ_VAR_STRING && rv->count > 0 && rv->value.str[0])
      race = rv->value.str[0];

    float dmg = dbr_get_float_fast(data, INT_racialBonusPercentDamage, shard_index);

    if(fabs(dmg) > 0.001f)
      buf_write(w, "<span color='%s'>+%d%% Damage to %ss</span>\n", color, (int)round(dmg), race);

    float def = dbr_get_float_fast(data, INT_racialBonusPercentDefense, shard_index);

    if(fabs(def) > 0.001f)
      buf_write(w, "<span color='%s'>%d%% less damage from %ss</span>\n", color, (int)round(def), race);
  }

  // Mastery augmentation: "+N to all skills in X Mastery"
  for(uint32_t i = 0; i < data->num_vars; i++)
  {
    if(!data->vars[i].name)
      continue;

    if(strncasecmp(data->vars[i].name, "augmentMasteryLevel", 19) == 0)
    {
      TQVariable *v = &data->vars[i];
      int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;

      if(idx < 0)
        idx = 0;

      float val = (v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx];

      if(fabs(val) < 0.001f)
        continue;

      char mastery_var[64];

      snprintf(mastery_var, sizeof(mastery_var), "augmentMasteryName%s", data->vars[i].name + 19);

      const char *mastery_var_int = arz_intern(mastery_var);
      TQVariable *mv = arz_record_get_var(data, mastery_var_int);
      const char *mastery_path = (mv && mv->type == TQ_VAR_STRING && mv->count > 0) ? mv->value.str[0] : NULL;

      if(!mastery_path || !mastery_path[0])
        continue;

      const char *mastery_name = "Unknown Mastery";
      const char *name_tag = get_record_variable_string(mastery_path, INT_skillDisplayName);

      if(name_tag)
      {
        const char *translated = translation_get(tr, name_tag);

        if(translated)
          mastery_name = translated;
      }

      buf_write(w, "<span color='%s'>+%d to all skills in %s</span>\n", color, (int)round(val), mastery_name);
    }
  }

  // Skill augmentation: "+N to [Skill Name]"
  for(uint32_t i = 0; i < data->num_vars; i++)
  {
    if(!data->vars[i].name)
      continue;

    if(strncasecmp(data->vars[i].name, "augmentSkillLevel", 17) == 0)
    {
      TQVariable *v = &data->vars[i];
      int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;

      if(idx < 0)
        idx = 0;

      float val = (v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx];

      if(fabs(val) < 0.001f)
        continue;

      char skill_var[64];

      snprintf(skill_var, sizeof(skill_var), "augmentSkillName%s", data->vars[i].name + 17);

      const char *skill_var_int = arz_intern(skill_var);
      TQVariable *sv = arz_record_get_var(data, skill_var_int);
      const char *skill_path = (sv && sv->type == TQ_VAR_STRING && sv->count > 0) ? sv->value.str[0] : NULL;

      const char *skill_name = "Unknown Skill";

      if(skill_path)
      {
        // Follow petSkillName if present (e.g. PetModifier skills)
        const char *pet_path = get_record_variable_string(skill_path, INT_petSkillName);
        const char *base_path = (pet_path && pet_path[0]) ? pet_path : skill_path;
        const char *buff_path = get_record_variable_string(base_path, INT_buffSkillName);
        const char *lookup_path = (buff_path && buff_path[0]) ? buff_path : base_path;
        const char *name_tag = get_record_variable_string(lookup_path, INT_skillDisplayName);

        if(name_tag)
        {
          const char *translated = translation_get(tr, name_tag);

          if(translated)
            skill_name = translated;
        }
      }

      buf_write(w, "<span color='%s'>+%d to %s</span>\n", color, (int)round(val), skill_name);
    }
  }

  // Standard attr_maps iteration -- O(num_vars) with O(1) per variable
  for(uint32_t i = 0; i < data->num_vars; i++)
  {
    TQVariable *v = &data->vars[i];

    if(!v->name)
      continue;

    const char *interned = arz_intern(v->name);

    // skip already-handled vars
    if(strncasecmp(v->name, "augmentMastery", 14) == 0)
      continue;

    if(strncasecmp(v->name, "augmentSkill", 12) == 0)
      continue;

    if(g_hash_table_contains(g_skip_set, interned))
      continue;

    // O(1) lookup instead of linear scan
    AttributeMap *am = g_hash_table_lookup(g_attr_map_ht, interned);

    if(!am)
      continue;

    int val_idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;

    if(val_idx < 0)
      val_idx = 0;

    float val = (v->type == TQ_VAR_INT) ? (float)v->value.i32[val_idx] : v->value.f32[val_idx];

    if(fabs(val) < 0.001f)
      continue;

    char line[256];

    if(am->is_percent || strstr(am->format, "%d") || strstr(am->format, "%+d"))
      snprintf(line, sizeof(line), am->format, (int)round(val));

    else
      snprintf(line, sizeof(line), am->format, val);

    buf_write(w, "<span color='%s'>%s</span>\n", color, line);
  }

  // Defensive resistances (may have chance)
  {
    static const struct { const char **val; const char **chance; const char *label; } resist_defs[] = {
      {&INT_defensiveFire,      &INT_defensiveFireChance,      "Fire"},
      {&INT_defensiveCold,      &INT_defensiveColdChance,      "Cold"},
      {&INT_defensiveLightning, &INT_defensiveLightningChance, "Lightning"},
      {&INT_defensivePoison,    &INT_defensivePoisonChance,    "Poison"},
      {&INT_defensivePierce,    &INT_defensivePierceChance,    "Pierce"},
      {&INT_defensiveLife,      &INT_defensiveLifeChance,      "Vitality"},
      {&INT_defensiveBleeding,  &INT_defensiveBleedingChance,  "Bleeding"},
      {&INT_defensivePhysical,  &INT_defensivePhysicalChance,  "Physical"},
      {&INT_defensiveElementalResistance, &INT_defensiveElementalResistanceChance, "Elemental"},
    };

    for(int ri = 0; ri < (int)(sizeof resist_defs / sizeof resist_defs[0]); ri++)
    {
      float rv = dbr_get_float_fast(data, *resist_defs[ri].val, shard_index);

      if(fabs(rv) < 0.001f)
        continue;

      float rc = dbr_get_float_fast(data, *resist_defs[ri].chance, shard_index);

      if(rc > 0 && rc < 100)
        buf_write(w, "<span color='%s'>%.0f%% Chance of %+d%% %s Resistance</span>\n", color, rc, (int)round(rv), resist_defs[ri].label);

      else
        buf_write(w, "<span color='%s'>%+d%% %s Resistance</span>\n", color, (int)round(rv), resist_defs[ri].label);
    }
  }

  // Skill parameters: cooldown/recharge time
  {
    float cooldown = dbr_get_float_fast(data, INT_skillCooldownTime, shard_index);

    if(cooldown <= 0)
      cooldown = dbr_get_float_fast(data, INT_refreshTime, shard_index);

    if(cooldown > 0)
      buf_write(w, "<span color='%s'>%.1f Second(s) Recharge</span>\n", color, cooldown);
  }

  // Skill parameters: target number
  {
    float targets = dbr_get_float_fast(data, INT_skillTargetNumber, shard_index);

    if(targets > 0)
      buf_write(w, "<span color='%s'>Affects up to %d targets</span>\n", color, (int)targets);
  }

  // Skill parameters: active duration
  {
    float duration = dbr_get_float_fast(data, INT_skillActiveDuration, shard_index);

    if(duration > 0)
      buf_write(w, "<span color='%s'>%.1f Second Duration</span>\n", color, duration);
  }

  // Skill parameters: target radius
  {
    float radius = dbr_get_float_fast(data, INT_skillTargetRadius, shard_index);

    if(radius > 0)
      buf_write(w, "<span color='%s'>%.1f Meter Radius</span>\n", color, radius);
  }

  // Flush deferred "Chance of" group content (if any)
  if(global_chance > 0 && chance_writer.pos > 0)
  {
    buf_write(w, "\n<span color='%s'>%.0f%% Chance of:</span>\n", color, global_chance);

    if(w->pos + chance_writer.pos < w->size)
    {
      memcpy(w->buf + w->pos, chance_buf, chance_writer.pos);
      w->pos += chance_writer.pos;
      w->buf[w->pos] = '\0';
    }
  }

  // Follow petBonusName reference (LootRandomizer pet bonus sub-records)
  {
    const char *pet_bonus = record_get_string_fast(data, INT_petBonusName);

    if(pet_bonus && pet_bonus[0])
    {
      buf_write(w, "\n<span color='%s'>Bonus to All Pets:</span>\n", color);
      add_stats_from_record(pet_bonus, tr, w, color, shard_index);
    }
  }
}
