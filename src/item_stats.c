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
  {"offensiveSlowLifeLeachModifier", "%+d%% Life Leech", true, NULL},
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
  // defensiveReflect handled in dedicated block (has chance)

  // Duration reductions
  {"defensiveFreeze", "%+d%% Reduced Freeze Duration", false, NULL},
  {"defensiveFreezeModifier", "+%d%% Reduced Freeze Duration", true, NULL},
  {"defensiveDisruption", "%.1f%% Reduced Skill Disruption", false, NULL},
  {"defensiveSleep", "%+d%% Sleep Resistance", false, NULL},
  {"defensiveTrap", "%+d%% Trap Resistance", false, NULL},
  {"defensiveTotalSpeedResistance", "%+d%% Slow Resistance", false, NULL},
  // defensiveSlowLifeLeach, defensiveSlowManaLeach, defensivePoisonDuration
  //   handled in dedicated block (may have Chance)

  // Retaliation (flat types handled in dedicated block with chance support)

  // Misc offensive
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

  // Slow (total speed reduction) -- handled in dedicated block (has Chance + Duration)

  // Taunt
  {"offensiveTauntMin", "%.0f%% Taunt", false, NULL},

  // Retaliation DoT
  // retaliationSlow* DoTs handled in dedicated blocks (have Duration + Chance)

  // Shield
  {"defensiveBlockModifier", "+%d%% Shield Block Chance", true, NULL},
  {"defensiveBlockModifierChance", "+%d%% Shield Block Chance", true, NULL},
  {"shieldBlockChanceModifier", "+%d%% Shield Block Chance", true, NULL},

  // Poison/Disruption duration
  {"defensivePoisonDuration", "%+d%% Reduced Poison Duration", false, NULL},

  // Defensive resist-duration partners
  {"defensiveBleedingDuration",          "%+d%% Reduced Bleeding Duration",        false, NULL},
  {"defensiveBleedingDurationModifier",  "+%d%% Reduced Bleeding Duration",        true,  NULL},
  {"defensiveColdDuration",              "%+d%% Reduced Frostburn Duration",       false, NULL},
  {"defensiveColdDurationModifier",      "+%d%% Reduced Frostburn Duration",       true,  NULL},
  {"defensiveFireDuration",              "%+d%% Reduced Burn Duration",            false, NULL},
  {"defensiveFireDurationModifier",      "+%d%% Reduced Burn Duration",            true,  NULL},
  {"defensiveLifeDuration",              "%+d%% Reduced Vitality Decay Duration",  false, NULL},
  {"defensiveLifeDurationModifier",      "+%d%% Reduced Vitality Decay Duration",  true,  NULL},
  {"defensiveLightningDurationModifier", "+%d%% Reduced Electrical Burn Duration", true,  NULL},
  {"defensivePoisonDurationModifier",    "+%d%% Reduced Poison Duration",          true,  NULL},
  {"defensivePhysicalDuration",          "%+d%% Reduced Physical Trauma Duration", false, NULL},
  {"defensivePhysicalDurationModifier",  "+%d%% Reduced Physical Trauma Duration", true,  NULL},

  // Regen modifiers
  {"characterLifeRegenModifier", "+%d%% Health Regeneration", true, NULL},
  {"characterManaRegenModifier", "+%d%% Energy Regeneration", true, NULL},

  // Projectile speed
  {"skillProjectileSpeedModifier", "+%d%% Projectile Speed", true, NULL},

  // Requirement reductions
  {"characterGlobalReqReduction", "-%d%% Reduction to all Requirements", true, NULL},
  {"characterLevelReqReduction", "-%d%% Player Level Requirement for Items", true, NULL},
  {"characterArmorStrengthReqReduction", "-%d%% Strength Requirement for Armor", true, NULL},
  {"characterArmorDexterityReqReduction", "-%d%% Dexterity Requirement for Armor", true, NULL},
  {"characterArmorIntelligenceReqReduction", "-%d%% Intelligence Requirement for Armor", true, NULL},
  {"characterMeleeStrengthReqReduction", "-%d%% Strength Requirement for Melee Weapons", true, NULL},
  {"characterMeleeDexterityReqReduction", "-%d%% Dexterity Requirement for Melee Weapons", true, NULL},
  {"characterMeleeIntelligenceReqReduction", "-%d%% Intelligence Requirement for Melee Weapons", true, NULL},
  {"characterHuntingStrengthReqReduction", "-%d%% Strength Requirement for Hunting Weapons", true, NULL},
  {"characterHuntingDexterityReqReduction", "-%d%% Dexterity Requirement for Hunting Weapons", true, NULL},
  {"characterHuntingIntelligenceReqReduction", "-%d%% Intelligence Requirement for Hunting Weapons", true, NULL},
  {"characterStaffStrengthReqReduction", "-%d%% Strength Requirement for Staff Weapons", true, NULL},
  {"characterStaffDexterityReqReduction", "-%d%% Dexterity Requirement for Staff Weapons", true, NULL},
  {"characterStaffIntelligenceReqReduction", "-%d%% Intelligence Requirement for Staff Weapons", true, NULL},
  {"characterShieldStrengthReqReduction", "-%d%% Strength Requirement for Shields", true, NULL},
  {"characterShieldDexterityReqReduction", "-%d%% Dexterity Requirement for Shields", true, NULL},
  {"characterShieldIntelligenceReqReduction", "-%d%% Intelligence Requirement for Shields", true, NULL},
  {"characterJewelryStrengthReqReduction", "-%d%% Strength Requirement for Jewelry", true, NULL},
  {"characterJewelryDexterityReqReduction", "-%d%% Dexterity Requirement for Jewelry", true, NULL},
  {"characterJewelryIntelligenceReqReduction", "-%d%% Intelligence Requirement for Jewelry", true, NULL},

  // Misc
  {"characterTotalSpeedModifier", "%+d%% Total Speed", true, NULL},
  {"skillCooldownReduction", "-%.0f%% Recharge", false, NULL},
  {"skillCooldownReductionModifier", "+%d%% Recharge", true, NULL},
  {"skillCooldownReductionChance", "%.0f%% Chance of Recharge Reduction", false, NULL},
  {"skillManaCostReduction", "-%.0f%% Energy Cost", false, NULL},
  {"skillManaCostReductionModifier", "+%d%% Energy Cost", true, NULL},
  {"skillManaCostReductionChance", "%.0f%% Chance of Energy Cost Reduction", false, NULL},
  {"augmentAllLevel", "+%d to all Skills", false, NULL},
  {"characterIncreasedExperience", "%+d%% Increased Experience", false, NULL},

  // Character utility / damage conversion / requirement reductions
  {"characterPhysToElementalRatio",              "%d%% of Physical Damage Converted to Elemental", false, NULL},
  {"characterDefensiveBlockRecoveryReduction",   "+%d%% Reduced Block Recovery",                   true,  NULL},
  {"characterManaLimitReserveReduction",         "-%d%% Energy Reserved",                          true,  NULL},
  {"characterManaLimitReserve",                  "+%d%% Energy Reserved",                          true,  NULL},
  {"characterManaLimitReserveModifier",          "+%d%% Energy Reserved",                          true,  NULL},
  {"characterManaLimitReserveReductionModifier", "-%d%% Energy Reserved",                          true,  NULL},
  {"characterWeaponStrengthReqReduction",        "-%d%% Strength Requirement for Weapons",         true,  NULL},
  {"characterWeaponDexterityReqReduction",       "-%d%% Dexterity Requirement for Weapons",        true,  NULL},
  {"characterWeaponIntelligenceReqReduction",    "-%d%% Intelligence Requirement for Weapons",     true,  NULL},

  {NULL, NULL, false, NULL}
};

// hash-based lookup tables (built at init)

static GHashTable *g_skip_set = NULL;    // interned ptr -> (gpointer)1
static GHashTable *g_attr_map_ht = NULL; // interned ptr -> &attr_maps[i]

// Pre-interned variable name pointers for frequently used names
static const char *INT_offensivePhysicalMin, *INT_offensivePhysicalMax, *INT_offensivePhysicalChance;
static const char *INT_offensiveFireMin, *INT_offensiveFireMax, *INT_offensiveFireChance;
static const char *INT_offensiveColdMin, *INT_offensiveColdMax, *INT_offensiveColdChance;
static const char *INT_offensiveLightningMin, *INT_offensiveLightningMax, *INT_offensiveLightningChance;
static const char *INT_offensivePoisonMin, *INT_offensivePoisonMax, *INT_offensivePoisonChance;
static const char *INT_offensivePierceMin, *INT_offensivePierceMax, *INT_offensivePierceChance;
static const char *INT_offensiveElementalMin, *INT_offensiveElementalMax, *INT_offensiveElementalChance;
static const char *INT_offensiveConvertChance;
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
static const char *INT_retaliationPhysicalMin, *INT_retaliationPhysicalChance;
static const char *INT_retaliationFireMin, *INT_retaliationFireChance;
static const char *INT_retaliationColdMin, *INT_retaliationColdChance;
static const char *INT_retaliationLightningMin, *INT_retaliationLightningChance;
static const char *INT_retaliationPierceMin, *INT_retaliationPierceChance;
static const char *INT_retaliationSlowFireMin, *INT_retaliationSlowFireMax, *INT_retaliationSlowFireDurationMin, *INT_retaliationSlowFireChance;
static const char *INT_retaliationSlowColdMin, *INT_retaliationSlowColdMax, *INT_retaliationSlowColdDurationMin, *INT_retaliationSlowColdChance;
static const char *INT_retaliationSlowLightningMin, *INT_retaliationSlowLightningMax, *INT_retaliationSlowLightningDurationMin, *INT_retaliationSlowLightningChance;
static const char *INT_retaliationSlowPoisonMin, *INT_retaliationSlowPoisonMax, *INT_retaliationSlowPoisonDurationMin, *INT_retaliationSlowPoisonChance;
static const char *INT_retaliationSlowLifeMin, *INT_retaliationSlowLifeMax, *INT_retaliationSlowLifeDurationMin, *INT_retaliationSlowLifeChance;
static const char *INT_retaliationSlowBleedingMin, *INT_retaliationSlowBleedingMax, *INT_retaliationSlowBleedingDurationMin, *INT_retaliationSlowBleedingChance;
static const char *INT_retaliationSlowLifeLeachMin, *INT_retaliationSlowLifeLeachMax, *INT_retaliationSlowLifeLeachDurationMin, *INT_retaliationSlowLifeLeachChance;
static const char *INT_retaliationSlowManaLeachMax;
static const char *INT_retaliationSlowRunSpeedMin, *INT_retaliationSlowRunSpeedMax, *INT_retaliationSlowRunSpeedDurationMin, *INT_retaliationSlowRunSpeedChance;
static const char *INT_retaliationSlowDefensiveAbilityMin, *INT_retaliationSlowDefensiveAbilityMax, *INT_retaliationSlowDefensiveAbilityDurationMin, *INT_retaliationSlowDefensiveAbilityChance;
static const char *INT_retaliationSlowOffensiveAbilityMin, *INT_retaliationSlowOffensiveAbilityMax, *INT_retaliationSlowOffensiveAbilityDurationMin, *INT_retaliationSlowOffensiveAbilityChance;
static const char *INT_retaliationSlowOffensiveReductionMin, *INT_retaliationSlowOffensiveReductionMax, *INT_retaliationSlowOffensiveReductionDurationMin, *INT_retaliationSlowOffensiveReductionChance;
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
static const char *INT_offensiveManaBurnDrainMin, *INT_offensiveManaBurnDrainMax, *INT_offensiveManaBurnDrainRatioMin, *INT_offensiveManaBurnDamageRatio;
static const char *INT_offensiveManaBurnChance, *INT_offensiveManaBurnRatioAdder, *INT_offensiveManaBurnRatioAdderChance;
static const char *INT_retaliationPhysicalModifier, *INT_retaliationPhysicalModifierChance;
static const char *INT_retaliationColdModifier, *INT_retaliationColdModifierChance;
static const char *INT_retaliationFireModifier, *INT_retaliationFireModifierChance;
static const char *INT_retaliationLightningModifier, *INT_retaliationLightningModifierChance;
static const char *INT_retaliationPoisonModifier, *INT_retaliationPoisonModifierChance;
static const char *INT_retaliationPierceModifier, *INT_retaliationPierceModifierChance;
static const char *INT_retaliationLifeModifier, *INT_retaliationLifeModifierChance;
static const char *INT_retaliationStunModifier, *INT_retaliationStunModifierChance;
static const char *INT_retaliationElementalModifier, *INT_retaliationElementalModifierChance;
static const char *INT_racialBonusPercentDamage, *INT_racialBonusPercentDefense, *INT_racialBonusRace;
static const char *INT_petBonusName;
static const char *INT_skillCooldownTime, *INT_refreshTime;
static const char *INT_skillTargetNumber, *INT_skillActiveDuration, *INT_skillTargetRadius;
static const char *INT_offensiveGlobalChance;
static const char *INT_offensiveDisruptionMin;
static const char *INT_offensiveSlowLightningDurationMax, *INT_offensiveSlowFireDurationMax;
static const char *INT_offensiveSlowColdDurationMax, *INT_offensiveSlowPoisonDurationMax;
static const char *INT_defensiveDisruption, *INT_defensiveDisruptionDuration;
static const char *INT_offensiveTotalResistanceReductionAbsoluteMin, *INT_offensiveTotalResistanceReductionAbsoluteDurationMin;
static const char *INT_offensiveTotalResistanceReductionAbsoluteChance, *INT_offensiveTotalResistanceReductionAbsoluteMax;
static const char *INT_offensiveTotalResistanceReductionPercentMin, *INT_offensiveTotalResistanceReductionPercentDurationMin;
static const char *INT_offensiveTotalResistanceReductionPercentChance;
static const char *INT_offensiveSlowOffensiveAbilityModifier, *INT_offensiveSlowOffensiveAbilityDurationMin;
static const char *INT_offensiveSlowPhysicalMin, *INT_offensiveSlowPhysicalMax, *INT_offensiveSlowPhysicalDurationMin, *INT_offensiveSlowPhysicalChance;
static const char *INT_offensiveSlowDefensiveAbilityMin, *INT_offensiveSlowDefensiveAbilityMax, *INT_offensiveSlowDefensiveAbilityDurationMin, *INT_offensiveSlowDefensiveAbilityChance;
static const char *INT_offensiveSlowOffensiveAbilityMin, *INT_offensiveSlowOffensiveAbilityMax, *INT_offensiveSlowOffensiveAbilityChance;
static const char *INT_offensiveSlowOffensiveReductionModifier, *INT_offensiveSlowOffensiveReductionDurationMin;
static const char *INT_offensiveSlowTotalSpeedMin, *INT_offensiveSlowTotalSpeedChance, *INT_offensiveSlowTotalSpeedDurationMin;
static const char *INT_offensivePercentCurrentLifeMax, *INT_offensiveConfusionMax;
static const char *INT_racialBonusAbsoluteDamage, *INT_racialBonusAbsoluteDefense;
static const char *INT_retaliationSlowAttackSpeedMin, *INT_retaliationSlowAttackSpeedDurationMin;
static const char *INT_retaliationSlowManaLeachMin, *INT_retaliationSlowManaLeachDurationMin, *INT_retaliationSlowManaLeachChance;
static const char *INT_retaliationPierceMax;
static const char *INT_retaliationFireMax, *INT_retaliationColdMax, *INT_retaliationLightningMax, *INT_retaliationPhysicalMax;
static const char *INT_retaliationLifeMin, *INT_retaliationLifeMax, *INT_retaliationLifeChance;
static const char *INT_retaliationPoisonMin, *INT_retaliationPoisonMax, *INT_retaliationPoisonChance;
static const char *INT_retaliationStunMin, *INT_retaliationStunMax, *INT_retaliationStunChance;
static const char *INT_retaliationElementalMin, *INT_retaliationElementalMax, *INT_retaliationElementalChance;
static const char *INT_retaliationPercentCurrentLifeMin, *INT_retaliationPercentCurrentLifeMax, *INT_retaliationPercentCurrentLifeChance;
static const char *INT_retaliationGlobalChance;
static const char *INT_defensiveBlock, *INT_defensiveBlockChance, *INT_defensiveAbsorption, *INT_defensivePetrify;
static const char *INT_offensivePierceRatioMin, *INT_offensivePierceRatioMax, *INT_offensivePierceRatioChance;
static const char *INT_offensivePierceRatioModifier, *INT_offensivePierceRatioModifierChance;
static const char *INT_offensiveSlowRunSpeedMax;
static const char *INT_offensiveStunMax, *INT_offensiveStunModifier;
static const char *INT_offensiveFreezeMax;
static const char *INT_offensiveSleepMin, *INT_offensiveSleepMax, *INT_offensiveSleepChance;
static const char *INT_offensiveSleepDurationMin, *INT_offensiveSleepModifier;
static const char *INT_offensiveDisruptionMax, *INT_offensiveDisruptionChance;
static const char *INT_offensiveBaseFireChance, *INT_offensiveBaseColdChance, *INT_offensiveBaseLightningChance;
static const char *INT_offensiveSlowLifeLeachDurationMax, *INT_offensiveSlowManaLeachDurationMax;
static const char *INT_offensiveSlowBleedingDurationMax;
static const char *INT_defensiveSlowLifeLeach, *INT_defensiveSlowLifeLeachChance;
static const char *INT_defensiveSlowManaLeach, *INT_defensiveSlowManaLeachChance;
static const char *INT_defensivePoisonDuration, *INT_defensivePoisonDurationChance;
static const char *INT_defensiveReflect, *INT_defensiveReflectChance;
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
    "offensivePhysicalMin", "offensivePhysicalMax", "offensivePhysicalChance",
    "offensiveFireMin", "offensiveFireMax",
    "offensiveColdMin", "offensiveColdMax", "offensiveColdChance",
    "offensiveLightningMin", "offensiveLightningMax", "offensiveLightningChance",
    "offensivePoisonMin", "offensivePoisonMax", "offensivePoisonChance",
    "offensivePierceMin", "offensivePierceMax", "offensivePierceChance",
    "offensiveElementalMin", "offensiveElementalMax", "offensiveElementalChance",
    "offensiveConvertChance",
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
    "offensiveManaBurnDrainMin", "offensiveManaBurnDrainMax", "offensiveManaBurnDrainRatioMin", "offensiveManaBurnDamageRatio",
    "offensiveManaBurnChance", "offensiveManaBurnRatioAdder", "offensiveManaBurnRatioAdderChance",
    "retaliationPhysicalModifier", "retaliationPhysicalModifierChance",
    "retaliationColdModifier", "retaliationColdModifierChance",
    "retaliationFireModifier", "retaliationFireModifierChance",
    "retaliationLightningModifier", "retaliationLightningModifierChance",
    "retaliationPoisonModifier", "retaliationPoisonModifierChance",
    "retaliationPierceModifier", "retaliationPierceModifierChance",
    "retaliationLifeModifier", "retaliationLifeModifierChance",
    "retaliationStunModifier", "retaliationStunModifierChance",
    "retaliationElementalModifier", "retaliationElementalModifierChance",
    "offensiveGlobalChance",
    "offensiveDisruptionMin",
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
    "offensiveTotalResistanceReductionAbsoluteMin", "offensiveTotalResistanceReductionAbsoluteDurationMin",
    "offensiveTotalResistanceReductionAbsoluteChance", "offensiveTotalResistanceReductionAbsoluteMax",
    "offensiveTotalResistanceReductionPercentMin", "offensiveTotalResistanceReductionPercentDurationMin",
    "offensiveTotalResistanceReductionPercentChance",
    "offensiveSlowOffensiveAbilityModifier", "offensiveSlowOffensiveAbilityDurationMin",
    "offensiveSlowPhysicalMin", "offensiveSlowPhysicalMax", "offensiveSlowPhysicalDurationMin", "offensiveSlowPhysicalChance",
    "offensiveSlowDefensiveAbilityMin", "offensiveSlowDefensiveAbilityMax", "offensiveSlowDefensiveAbilityDurationMin", "offensiveSlowDefensiveAbilityChance",
    "offensiveSlowOffensiveAbilityMin", "offensiveSlowOffensiveAbilityMax", "offensiveSlowOffensiveAbilityChance",
    "offensiveSlowOffensiveReductionModifier", "offensiveSlowOffensiveReductionDurationMin",
    "offensiveSlowTotalSpeedMin", "offensiveSlowTotalSpeedChance", "offensiveSlowTotalSpeedDurationMin",
    "offensivePercentCurrentLifeMax", "offensiveConfusionMax",
    "racialBonusAbsoluteDamage", "racialBonusAbsoluteDefense",
    "defensiveSlowLifeLeach", "defensiveSlowLifeLeachChance",
    "defensiveSlowManaLeach", "defensiveSlowManaLeachChance",
    "defensivePoisonDuration", "defensivePoisonDurationChance",
    "defensiveReflect", "defensiveReflectChance",
    "retaliationSlowAttackSpeedMin", "retaliationSlowAttackSpeedDurationMin",
    "retaliationSlowManaLeachMin", "retaliationSlowManaLeachDurationMin", "retaliationSlowManaLeachChance",
    "retaliationPierceMax",
    "retaliationPhysicalMin", "retaliationPhysicalMax", "retaliationPhysicalChance",
    "retaliationFireMin", "retaliationFireMax", "retaliationFireChance",
    "retaliationColdMin", "retaliationColdMax", "retaliationColdChance",
    "retaliationLightningMin", "retaliationLightningMax", "retaliationLightningChance",
    "retaliationPierceMin", "retaliationPierceChance",
    "retaliationLifeMin", "retaliationLifeMax", "retaliationLifeChance",
    "retaliationPoisonMin", "retaliationPoisonMax", "retaliationPoisonChance",
    "retaliationStunMin", "retaliationStunMax", "retaliationStunChance",
    "retaliationElementalMin", "retaliationElementalMax", "retaliationElementalChance",
    "retaliationPercentCurrentLifeMin", "retaliationPercentCurrentLifeMax", "retaliationPercentCurrentLifeChance",
    "retaliationGlobalChance",
    "defensiveBlock", "defensiveBlockChance", "defensiveAbsorption", "defensivePetrify",
    "offensivePierceRatioMin", "offensivePierceRatioMax", "offensivePierceRatioChance",
    "offensivePierceRatioModifier", "offensivePierceRatioModifierChance",
    "offensiveSlowRunSpeedMax",
    "offensiveStunMax", "offensiveStunModifier",
    "offensiveFreezeMax",
    "offensiveSleepMin", "offensiveSleepMax", "offensiveSleepChance",
    "offensiveSleepDurationMin", "offensiveSleepModifier",
    "offensiveDisruptionMax", "offensiveDisruptionChance",
    "offensiveBaseFireChance", "offensiveBaseColdChance", "offensiveBaseLightningChance",
    "offensiveSlowLifeLeachDurationMax", "offensiveSlowManaLeachDurationMax",
    "offensiveSlowBleedingDurationMax",
    "retaliationSlowFireMin", "retaliationSlowFireMax", "retaliationSlowFireDurationMin", "retaliationSlowFireChance",
    "retaliationSlowColdMin", "retaliationSlowColdMax", "retaliationSlowColdDurationMin", "retaliationSlowColdChance",
    "retaliationSlowLightningMin", "retaliationSlowLightningMax", "retaliationSlowLightningDurationMin", "retaliationSlowLightningChance",
    "retaliationSlowPoisonMin", "retaliationSlowPoisonMax", "retaliationSlowPoisonDurationMin", "retaliationSlowPoisonChance",
    "retaliationSlowLifeMin", "retaliationSlowLifeMax", "retaliationSlowLifeDurationMin", "retaliationSlowLifeChance",
    "retaliationSlowBleedingMin", "retaliationSlowBleedingMax", "retaliationSlowBleedingDurationMin", "retaliationSlowBleedingChance",
    "retaliationSlowManaLeachMax",
    "retaliationSlowRunSpeedMin", "retaliationSlowRunSpeedMax", "retaliationSlowRunSpeedDurationMin", "retaliationSlowRunSpeedChance",
    "retaliationSlowDefensiveAbilityMin", "retaliationSlowDefensiveAbilityMax", "retaliationSlowDefensiveAbilityDurationMin", "retaliationSlowDefensiveAbilityChance",
    "retaliationSlowOffensiveAbilityMin", "retaliationSlowOffensiveAbilityMax", "retaliationSlowOffensiveAbilityDurationMin", "retaliationSlowOffensiveAbilityChance",
    "retaliationSlowOffensiveReductionMin", "retaliationSlowOffensiveReductionMax", "retaliationSlowOffensiveReductionDurationMin", "retaliationSlowOffensiveReductionChance",
    "retaliationSlowLifeLeachMin", "retaliationSlowLifeLeachMax", "retaliationSlowLifeLeachDurationMin", "retaliationSlowLifeLeachChance",
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
  INTERN(offensivePhysicalMin); INTERN(offensivePhysicalMax); INTERN(offensivePhysicalChance);
  INTERN(offensiveFireMin); INTERN(offensiveFireMax); INTERN(offensiveFireChance);
  INTERN(offensiveColdMin); INTERN(offensiveColdMax); INTERN(offensiveColdChance);
  INTERN(offensiveLightningMin); INTERN(offensiveLightningMax); INTERN(offensiveLightningChance);
  INTERN(offensivePoisonMin); INTERN(offensivePoisonMax); INTERN(offensivePoisonChance);
  INTERN(offensivePierceMin); INTERN(offensivePierceMax); INTERN(offensivePierceChance);
  INTERN(offensiveElementalMin); INTERN(offensiveElementalMax); INTERN(offensiveElementalChance);
  INTERN(offensiveConvertChance);
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
  INTERN(retaliationPhysicalMin); INTERN(retaliationPhysicalChance);
  INTERN(retaliationFireMin); INTERN(retaliationFireChance);
  INTERN(retaliationColdMin); INTERN(retaliationColdChance);
  INTERN(retaliationLightningMin); INTERN(retaliationLightningChance);
  INTERN(retaliationPierceMin); INTERN(retaliationPierceChance);
  INTERN(retaliationSlowFireMin); INTERN(retaliationSlowFireMax); INTERN(retaliationSlowFireDurationMin); INTERN(retaliationSlowFireChance);
  INTERN(retaliationSlowColdMin); INTERN(retaliationSlowColdMax); INTERN(retaliationSlowColdDurationMin); INTERN(retaliationSlowColdChance);
  INTERN(retaliationSlowLightningMin); INTERN(retaliationSlowLightningMax); INTERN(retaliationSlowLightningDurationMin); INTERN(retaliationSlowLightningChance);
  INTERN(retaliationSlowPoisonMin); INTERN(retaliationSlowPoisonMax); INTERN(retaliationSlowPoisonDurationMin); INTERN(retaliationSlowPoisonChance);
  INTERN(retaliationSlowLifeMin); INTERN(retaliationSlowLifeMax); INTERN(retaliationSlowLifeDurationMin); INTERN(retaliationSlowLifeChance);
  INTERN(retaliationSlowBleedingMin); INTERN(retaliationSlowBleedingMax); INTERN(retaliationSlowBleedingDurationMin); INTERN(retaliationSlowBleedingChance);
  INTERN(retaliationSlowLifeLeachMin); INTERN(retaliationSlowLifeLeachMax); INTERN(retaliationSlowLifeLeachDurationMin); INTERN(retaliationSlowLifeLeachChance);
  INTERN(retaliationSlowManaLeachMax);
  INTERN(retaliationSlowRunSpeedMin); INTERN(retaliationSlowRunSpeedMax); INTERN(retaliationSlowRunSpeedDurationMin); INTERN(retaliationSlowRunSpeedChance);
  INTERN(retaliationSlowDefensiveAbilityMin); INTERN(retaliationSlowDefensiveAbilityMax); INTERN(retaliationSlowDefensiveAbilityDurationMin); INTERN(retaliationSlowDefensiveAbilityChance);
  INTERN(retaliationSlowOffensiveAbilityMin); INTERN(retaliationSlowOffensiveAbilityMax); INTERN(retaliationSlowOffensiveAbilityDurationMin); INTERN(retaliationSlowOffensiveAbilityChance);
  INTERN(retaliationSlowOffensiveReductionMin); INTERN(retaliationSlowOffensiveReductionMax); INTERN(retaliationSlowOffensiveReductionDurationMin); INTERN(retaliationSlowOffensiveReductionChance);
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
  INTERN(offensiveManaBurnDrainMin); INTERN(offensiveManaBurnDrainMax); INTERN(offensiveManaBurnDrainRatioMin); INTERN(offensiveManaBurnDamageRatio);
  INTERN(offensiveManaBurnChance); INTERN(offensiveManaBurnRatioAdder); INTERN(offensiveManaBurnRatioAdderChance);
  INTERN(retaliationPhysicalModifier); INTERN(retaliationPhysicalModifierChance);
  INTERN(retaliationColdModifier); INTERN(retaliationColdModifierChance);
  INTERN(retaliationFireModifier); INTERN(retaliationFireModifierChance);
  INTERN(retaliationLightningModifier); INTERN(retaliationLightningModifierChance);
  INTERN(retaliationPoisonModifier); INTERN(retaliationPoisonModifierChance);
  INTERN(retaliationPierceModifier); INTERN(retaliationPierceModifierChance);
  INTERN(retaliationLifeModifier); INTERN(retaliationLifeModifierChance);
  INTERN(retaliationStunModifier); INTERN(retaliationStunModifierChance);
  INTERN(retaliationElementalModifier); INTERN(retaliationElementalModifierChance);
  INTERN(offensiveTotalDamageReductionPercentMin); INTERN(offensiveTotalDamageReductionPercentChance);
  INTERN(offensiveTotalDamageReductionPercentDurationMin);
  INTERN(racialBonusPercentDamage); INTERN(racialBonusPercentDefense); INTERN(racialBonusRace);
  INTERN(petBonusName);
  INTERN(skillCooldownTime); INTERN(refreshTime);
  INTERN(skillTargetNumber); INTERN(skillActiveDuration); INTERN(skillTargetRadius);
  INTERN(offensiveGlobalChance);
  INTERN(offensiveDisruptionMin);
  INTERN(offensiveSlowLightningDurationMax); INTERN(offensiveSlowFireDurationMax);
  INTERN(offensiveSlowColdDurationMax); INTERN(offensiveSlowPoisonDurationMax);
  INTERN(defensiveDisruption); INTERN(defensiveDisruptionDuration);
  INTERN(offensiveTotalResistanceReductionAbsoluteMin); INTERN(offensiveTotalResistanceReductionAbsoluteDurationMin);
  INTERN(offensiveTotalResistanceReductionAbsoluteChance); INTERN(offensiveTotalResistanceReductionAbsoluteMax);
  INTERN(offensiveTotalResistanceReductionPercentMin); INTERN(offensiveTotalResistanceReductionPercentDurationMin);
  INTERN(offensiveTotalResistanceReductionPercentChance);
  INTERN(offensiveSlowOffensiveAbilityModifier); INTERN(offensiveSlowOffensiveAbilityDurationMin);
  INTERN(offensiveSlowPhysicalMin); INTERN(offensiveSlowPhysicalMax); INTERN(offensiveSlowPhysicalDurationMin); INTERN(offensiveSlowPhysicalChance);
  INTERN(offensiveSlowDefensiveAbilityMin); INTERN(offensiveSlowDefensiveAbilityMax); INTERN(offensiveSlowDefensiveAbilityDurationMin); INTERN(offensiveSlowDefensiveAbilityChance);
  INTERN(offensiveSlowOffensiveAbilityMin); INTERN(offensiveSlowOffensiveAbilityMax); INTERN(offensiveSlowOffensiveAbilityChance);
  INTERN(offensiveSlowOffensiveReductionModifier); INTERN(offensiveSlowOffensiveReductionDurationMin);
  INTERN(offensiveSlowTotalSpeedMin); INTERN(offensiveSlowTotalSpeedChance); INTERN(offensiveSlowTotalSpeedDurationMin);
  INTERN(offensivePercentCurrentLifeMax); INTERN(offensiveConfusionMax);
  INTERN(racialBonusAbsoluteDamage); INTERN(racialBonusAbsoluteDefense);
  INTERN(retaliationSlowAttackSpeedMin); INTERN(retaliationSlowAttackSpeedDurationMin);
  INTERN(retaliationSlowManaLeachMin); INTERN(retaliationSlowManaLeachDurationMin); INTERN(retaliationSlowManaLeachChance);
  INTERN(retaliationPierceMax);
  INTERN(retaliationFireMax); INTERN(retaliationColdMax); INTERN(retaliationLightningMax); INTERN(retaliationPhysicalMax);
  INTERN(retaliationLifeMin); INTERN(retaliationLifeMax); INTERN(retaliationLifeChance);
  INTERN(retaliationPoisonMin); INTERN(retaliationPoisonMax); INTERN(retaliationPoisonChance);
  INTERN(retaliationStunMin); INTERN(retaliationStunMax); INTERN(retaliationStunChance);
  INTERN(retaliationElementalMin); INTERN(retaliationElementalMax); INTERN(retaliationElementalChance);
  INTERN(retaliationPercentCurrentLifeMin); INTERN(retaliationPercentCurrentLifeMax); INTERN(retaliationPercentCurrentLifeChance);
  INTERN(retaliationGlobalChance);
  INTERN(defensiveBlock); INTERN(defensiveBlockChance); INTERN(defensiveAbsorption); INTERN(defensivePetrify);
  INTERN(offensivePierceRatioMin); INTERN(offensivePierceRatioMax); INTERN(offensivePierceRatioChance);
  INTERN(offensivePierceRatioModifier); INTERN(offensivePierceRatioModifierChance);
  INTERN(offensiveSlowRunSpeedMax);
  INTERN(offensiveStunMax); INTERN(offensiveStunModifier);
  INTERN(offensiveFreezeMax);
  INTERN(offensiveSleepMin); INTERN(offensiveSleepMax); INTERN(offensiveSleepChance);
  INTERN(offensiveSleepDurationMin); INTERN(offensiveSleepModifier);
  INTERN(offensiveDisruptionMax); INTERN(offensiveDisruptionChance);
  INTERN(offensiveBaseFireChance); INTERN(offensiveBaseColdChance); INTERN(offensiveBaseLightningChance);
  INTERN(offensiveSlowLifeLeachDurationMax); INTERN(offensiveSlowManaLeachDurationMax);
  INTERN(offensiveSlowBleedingDurationMax);
  INTERN(defensiveSlowLifeLeach); INTERN(defensiveSlowLifeLeachChance);
  INTERN(defensiveSlowManaLeach); INTERN(defensiveSlowManaLeachChance);
  INTERN(defensivePoisonDuration); INTERN(defensivePoisonDurationChance);
  INTERN(defensiveReflect); INTERN(defensiveReflectChance);
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

// Check whether a proc-style offensive stat group belongs inside the
// "X% Chance of:" block.  TQ flags this with offensive<Prefix>Global == 1
// (boolean).  prefix is the stat name segment (e.g. "Elemental", "Fire",
// "LifeLeech", "PercentCurrentLife").  Returns true if Global is set.
static bool
offensive_proc_in_chance(TQArzRecordData *data, const char *prefix, int shard_index)
{
  if(!prefix || !*prefix)
    return false;

  char name[96];
  snprintf(name, sizeof(name), "offensive%sGlobal", prefix);
  const char *iv = arz_intern(name);

  return dbr_get_float_fast(data, iv, shard_index) > 0.5f;
}

__attribute__((unused)) static bool
retaliation_proc_in_chance(TQArzRecordData *data, const char *prefix, int shard_index)
{
  if(!prefix || !*prefix)
    return false;

  char name[96];
  snprintf(name, sizeof(name), "retaliation%sGlobal", prefix);
  const char *iv = arz_intern(name);

  return dbr_get_float_fast(data, iv, shard_index) > 0.5f;
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
item_bonus_stat_summary(const char *record_path, TQTranslation *tr)
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
    {&INT_defensiveElementalResistance, "%+d%% Elemental Resistance"},
    {&INT_defensiveDisruption, "%+d%% Skill Disruption Protection"},
    {&INT_offensiveTotalResistanceReductionAbsoluteMin, "%+d Reduced Resistances"},
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

  // Flat retaliation (may have chance)
  static const struct { const char **val; const char **chance; const char *label; } extra_retal[] = {
    {&INT_retaliationPhysicalMin,  &INT_retaliationPhysicalChance,  "Physical Retaliation"},
    {&INT_retaliationFireMin,      &INT_retaliationFireChance,      "Fire Retaliation"},
    {&INT_retaliationColdMin,      &INT_retaliationColdChance,      "Cold Retaliation"},
    {&INT_retaliationLightningMin, &INT_retaliationLightningChance, "Lightning Retaliation"},
    {&INT_retaliationPierceMin,    &INT_retaliationPierceChance,    "Pierce Retaliation"},
  };

  for(int e = 0; e < (int)(sizeof extra_retal / sizeof extra_retal[0]) && found < 3; e++)
  {
    float val = dbr_get_float_fast(data, *extra_retal[e].val, 0);

    if(val < 0.001f)
      continue;

    float chance = dbr_get_float_fast(data, *extra_retal[e].chance, 0);

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
      snprintf(part, sizeof(part), "%.1f%% Chance of %d %s", chance, (int)roundf(val), extra_retal[e].label);

    else
      snprintf(part, sizeof(part), "%d %s", (int)roundf(val), extra_retal[e].label);

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
    float drain_max = dbr_get_float_fast(data, INT_offensiveManaBurnDrainMax, 0);
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
      char val_str[32];

      if(drain_max > val)
        snprintf(val_str, sizeof(val_str), "%.0f-%.0f", val, drain_max);
      else
        snprintf(val_str, sizeof(val_str), "%.0f", val);

      if(dmg_ratio > 0.001f)
        snprintf(part, sizeof(part), "%s%% Energy Drain (%.0f%% as damage)", val_str, dmg_ratio);

      else
        snprintf(part, sizeof(part), "%s%% Energy Drain", val_str);

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

      snprintf(part, sizeof(part), "%+d%% Damage to %ss", (int)roundf(rdmg), race);

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

  // Offensive damage modifiers (may have chance qualifier)
  {
    static const struct { const char **val; const char **chance; const char *label; } off_mod[] = {
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

    for(int e = 0; e < (int)(sizeof off_mod / sizeof off_mod[0]) && found < 3; e++)
    {
      float val = dbr_get_float_fast(data, *off_mod[e].val, 0);

      if(fabs(val) < 0.001f)
        continue;

      float chance = dbr_get_float_fast(data, *off_mod[e].chance, 0);

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

      if(chance > 0 && chance < 100)
        snprintf(part, sizeof(part), "%.0f%% Chance of %+d%% %s", chance, (int)roundf(val), off_mod[e].label);
      else
        snprintf(part, sizeof(part), "%+d%% %s", (int)roundf(val), off_mod[e].label);

      size_t cur = strlen(buf), plen = strlen(part);

      if(cur + plen < sizeof(buf) - 1)
        memcpy(buf + cur, part, plen + 1);

      found++;
    }
  }

  // Mastery augmentation: "+N to all skills in X Mastery"
  for(uint32_t i = 0; i < data->num_vars && found < 3; i++)
  {
    if(!data->vars[i].name)
      continue;

    if(strncasecmp(data->vars[i].name, "augmentMasteryLevel", 19) == 0)
    {
      float val = (data->vars[i].type == TQ_VAR_INT && data->vars[i].count > 0)
                  ? (float)data->vars[i].value.i32[0]
                  : (data->vars[i].type == TQ_VAR_FLOAT && data->vars[i].count > 0)
                    ? data->vars[i].value.f32[0] : 0;

      if(fabs(val) < 0.001f)
        continue;

      char mastery_var[64];

      snprintf(mastery_var, sizeof(mastery_var), "augmentMasteryName%s", data->vars[i].name + 19);

      const char *mastery_var_int = arz_intern(mastery_var);
      TQVariable *mv = arz_record_get_var(data, mastery_var_int);
      const char *mastery_path = (mv && mv->type == TQ_VAR_STRING && mv->count > 0) ? mv->value.str[0] : NULL;

      if(!mastery_path || !mastery_path[0])
        continue;

      const char *mastery_name = NULL;
      const char *name_tag = get_record_variable_string(mastery_path, INT_skillDisplayName);

      if(name_tag && tr)
      {
        const char *translated = translation_get(tr, name_tag);

        if(translated)
          mastery_name = translated;
      }

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

      char part[128];

      if(mastery_name)
        snprintf(part, sizeof(part), "+%d to all skills in %s", (int)roundf(val), mastery_name);
      else
        snprintf(part, sizeof(part), "+%d to a Mastery", (int)roundf(val));

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
      char *pet_summary = item_bonus_stat_summary(pet, tr);

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

  // offensiveGlobalChance > 0 wraps proc-style offensive effects under an
  // "X% Chance of:" header.  On weapons, the offensiveBase* damage entries
  // are the weapon's actual base attack damage and stay outside the chance
  // block (handled below via is_base), while non-Base bonuses go inside.
  const char *cls = record_get_string_fast(data, INT_Class);
  bool is_proc_wrapped = cls && (strcasecmp(cls, "ItemRelic") == 0 ||
                                 strcasecmp(cls, "ItemCharm") == 0 ||
                                 strncasecmp(cls, "Armor", 5) == 0 ||
                                 strncasecmp(cls, "Weapon", 6) == 0);
  float global_chance = is_proc_wrapped
                        ? dbr_get_float_fast(data, INT_offensiveGlobalChance, shard_index)
                        : 0;
  float retal_global_chance = is_proc_wrapped
                              ? dbr_get_float_fast(data, INT_retaliationGlobalChance, shard_index)
                              : 0;

  BufWriter *ow = w;
  const char *indent = "";
  char ow_buffer[8192];
  BufWriter ow_writer;

  BufWriter *rw = w;
  char rw_buffer[8192];
  BufWriter rw_writer;
  const char *retal_indent = "";

  if(global_chance > 0)
  {
    buf_init(&ow_writer, ow_buffer, sizeof(ow_buffer));
    ow = &ow_writer;
    indent = "    ";
  }

  if(retal_global_chance > 0)
  {
    buf_init(&rw_writer, rw_buffer, sizeof(rw_buffer));
    rw = &rw_writer;
    retal_indent = "    ";
  }

  // Flat damage ranges (min-max), with optional chance qualifier
  {
    // prefix: stat name segment used to look up offensive<Prefix>Global.
    // is_base: weapon base damage (always top-level).  NULL prefix on
    // non-base entries means "no Global lookup" (treat like top-level).
    static struct { const char **min_int; const char **max_int; const char **chance_int; const char *label; bool is_base; const char *prefix; } damage_types[] = {
      {&INT_offensivePhysicalMin, &INT_offensivePhysicalMax, &INT_offensivePhysicalChance, "Physical Damage", false, "Physical"},
      {&INT_offensiveFireMin, &INT_offensiveFireMax, &INT_offensiveFireChance, "Fire Damage", false, "Fire"},
      {&INT_offensiveColdMin, &INT_offensiveColdMax, &INT_offensiveColdChance, "Cold Damage", false, "Cold"},
      {&INT_offensiveLightningMin, &INT_offensiveLightningMax, &INT_offensiveLightningChance, "Lightning Damage", false, "Lightning"},
      {&INT_offensivePoisonMin, &INT_offensivePoisonMax, &INT_offensivePoisonChance, "Poison Damage", false, "Poison"},
      {&INT_offensivePierceMin, &INT_offensivePierceMax, &INT_offensivePierceChance, "Piercing Damage", false, "Pierce"},
      {&INT_offensiveElementalMin, &INT_offensiveElementalMax, &INT_offensiveElementalChance, "Elemental Damage", false, "Elemental"},
      {&INT_offensiveManaLeechMin, &INT_offensiveManaLeechMax, NULL, "Mana Leech", false, NULL},
      {&INT_offensiveBasePhysicalMin, &INT_offensiveBasePhysicalMax, NULL, "Physical Damage", true, NULL},
      {&INT_offensiveBaseColdMin, &INT_offensiveBaseColdMax, &INT_offensiveBaseColdChance, "Cold Damage", true, NULL},
      {&INT_offensiveBaseFireMin, &INT_offensiveBaseFireMax, &INT_offensiveBaseFireChance, "Fire Damage", true, NULL},
      {&INT_offensiveBaseLightningMin, &INT_offensiveBaseLightningMax, &INT_offensiveBaseLightningChance, "Lightning Damage", true, NULL},
      {&INT_offensiveBasePoisonMin, &INT_offensiveBasePoisonMax, NULL, "Poison Damage", true, NULL},
      {&INT_offensiveBaseLifeMin, &INT_offensiveBaseLifeMax, NULL, "Vitality Damage", true, NULL},
      {&INT_offensiveLifeMin, &INT_offensiveLifeMax, &INT_offensiveLifeChance, "Vitality Damage", false, "Life"},
      {&INT_offensiveBonusPhysicalMin, &INT_offensiveBonusPhysicalMax, &INT_offensiveBonusPhysicalChance, "Physical Damage", false, "BonusPhysical"},
      {NULL, NULL, NULL, NULL, false, NULL}
    };

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
            buf_write(w, "<span color='%s'>%.1f%% Chance of %d - %d %s</span>\n", color, chance, (int)round(mn), (int)round(mx), dmg_label);

          else
            buf_write(w, "<span color='%s'>%.1f%% Chance of %d %s</span>\n", color, chance, (int)round(mn), dmg_label);
        }

        else
        {
          // Base weapon damage and stats whose Global flag is false stay
          // outside the chance block; only stats with Global=true go inside.
          bool in_chance = !damage_types[d].is_base
                           && global_chance > 0
                           && offensive_proc_in_chance(data, damage_types[d].prefix, shard_index);
          BufWriter *target = in_chance ? ow : w;
          const char *target_indent = in_chance ? indent : "";

          if(mx > mn)
            buf_write(target, "<span color='%s'>%s%d - %d %s</span>\n", color, target_indent, (int)round(mn), (int)round(mx), dmg_label);

          else
            buf_write(target, "<span color='%s'>%s%d %s</span>\n", color, target_indent, (int)round(mn), dmg_label);
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
      bool in_chance = global_chance > 0 && offensive_proc_in_chance(data, "LifeLeech", shard_index);
      BufWriter *target = in_chance ? ow : w;
      const char *target_indent = in_chance ? indent : "";

      if(mx > mn)
        buf_write(target, "<span color='%s'>%s%d%% - %d%% Attack Damage Converted to Health</span>\n", color, target_indent, (int)round(mn), (int)round(mx));

      else
        buf_write(target, "<span color='%s'>%s%d%% Attack Damage Converted to Health</span>\n", color, target_indent, (int)round(mn));
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
      {&INT_offensiveSlowPhysicalMin,  &INT_offensiveSlowPhysicalMax,  &INT_offensiveSlowPhysicalDurationMin,  &INT_offensiveSlowPhysicalChance,  "Physical Damage"},
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
      buf_write(ow, "<span color='%s'>%s%.1f%% Chance of %+d%% Bleeding Damage</span>\n", color, indent, chance, (int)round(mod));

    else if(fabs(mod) > 0.001f)
      buf_write(ow, "<span color='%s'>%s%+d%% Bleeding Damage</span>\n", color, indent, (int)round(mod));
  }

  // Retaliation DoTs (damage over time triggered on retaliation, may have chance)
  {
    static const struct {
      const char **val; const char **max; const char **dur; const char **chance; const char *label;
    } retal_dots[] = {
      {&INT_retaliationSlowFireMin,      &INT_retaliationSlowFireMax,      &INT_retaliationSlowFireDurationMin,      &INT_retaliationSlowFireChance,      "Burn Retaliation"},
      {&INT_retaliationSlowColdMin,      &INT_retaliationSlowColdMax,      &INT_retaliationSlowColdDurationMin,      &INT_retaliationSlowColdChance,      "Frostburn Retaliation"},
      {&INT_retaliationSlowLightningMin, &INT_retaliationSlowLightningMax, &INT_retaliationSlowLightningDurationMin, &INT_retaliationSlowLightningChance, "Electrical Burn Retaliation"},
      {&INT_retaliationSlowPoisonMin,    &INT_retaliationSlowPoisonMax,    &INT_retaliationSlowPoisonDurationMin,    &INT_retaliationSlowPoisonChance,    "Poison Retaliation"},
      {&INT_retaliationSlowLifeMin,      &INT_retaliationSlowLifeMax,      &INT_retaliationSlowLifeDurationMin,      &INT_retaliationSlowLifeChance,      "Vitality Decay Retaliation"},
      {&INT_retaliationSlowBleedingMin,  &INT_retaliationSlowBleedingMax,  &INT_retaliationSlowBleedingDurationMin,  &INT_retaliationSlowBleedingChance,  "Bleeding Retaliation"},
      {&INT_retaliationSlowLifeLeachMin, &INT_retaliationSlowLifeLeachMax, &INT_retaliationSlowLifeLeachDurationMin, &INT_retaliationSlowLifeLeachChance, "Life Leech Retaliation"},
      {&INT_retaliationSlowManaLeachMin, &INT_retaliationSlowManaLeachMax, &INT_retaliationSlowManaLeachDurationMin, &INT_retaliationSlowManaLeachChance, "Energy Leech Retaliation"},
      {&INT_retaliationSlowAttackSpeedMin, NULL,                           &INT_retaliationSlowAttackSpeedDurationMin, NULL,                               "Reduced Attack Speed Retaliation"},
    };

    for(int ri = 0; ri < (int)(sizeof retal_dots / sizeof retal_dots[0]); ri++)
    {
      float mn = dbr_get_float_fast(data, *retal_dots[ri].val, shard_index);
      float dur = dbr_get_float_fast(data, *retal_dots[ri].dur, shard_index);

      if(mn <= 0 || dur <= 0)
        continue;

      float mx = retal_dots[ri].max ? dbr_get_float_fast(data, *retal_dots[ri].max, shard_index) : 0;
      float ch = retal_dots[ri].chance ? dbr_get_float_fast(data, *retal_dots[ri].chance, shard_index) : 0;

      if(ch > 0 && ch < 100)
      {
        if(mx > mn)
          buf_write(w, "<span color='%s'>%s%.0f%% Chance of %.0f - %.0f %s over %.1f Seconds</span>\n",
                    color, indent, ch, mn * dur, mx * dur, retal_dots[ri].label, dur);
        else
          buf_write(w, "<span color='%s'>%s%.0f%% Chance of %.0f %s over %.1f Seconds</span>\n",
                    color, indent, ch, mn * dur, retal_dots[ri].label, dur);
      }

      else
      {
        if(mx > mn)
          buf_write(rw, "<span color='%s'>%s%.0f - %.0f %s over %.1f Seconds</span>\n",
                    color, retal_indent, mn * dur, mx * dur, retal_dots[ri].label, dur);
        else
          buf_write(rw, "<span color='%s'>%s%.0f %s over %.1f Seconds</span>\n",
                    color, retal_indent, mn * dur, retal_dots[ri].label, dur);
      }
    }
  }

  // Flat retaliation (may have chance and/or max range)
  {
    static const struct {
      const char **val; const char **max; const char **chance; const char *label;
    } retal_flat[] = {
      {&INT_retaliationPhysicalMin,  &INT_retaliationPhysicalMax,  &INT_retaliationPhysicalChance,  "Physical Retaliation"},
      {&INT_retaliationFireMin,      &INT_retaliationFireMax,      &INT_retaliationFireChance,      "Fire Retaliation"},
      {&INT_retaliationColdMin,      &INT_retaliationColdMax,      &INT_retaliationColdChance,      "Cold Retaliation"},
      {&INT_retaliationLightningMin, &INT_retaliationLightningMax, &INT_retaliationLightningChance, "Lightning Retaliation"},
      {&INT_retaliationPierceMin,    &INT_retaliationPierceMax,    &INT_retaliationPierceChance,    "Pierce Retaliation"},
      {&INT_retaliationLifeMin,      &INT_retaliationLifeMax,      &INT_retaliationLifeChance,      "Vitality Retaliation"},
      {&INT_retaliationPoisonMin,    &INT_retaliationPoisonMax,    &INT_retaliationPoisonChance,    "Poison Retaliation"},
      {&INT_retaliationElementalMin, &INT_retaliationElementalMax, &INT_retaliationElementalChance, "Elemental Retaliation"},
    };

    for(int ri = 0; ri < (int)(sizeof retal_flat / sizeof retal_flat[0]); ri++)
    {
      float mn = dbr_get_float_fast(data, *retal_flat[ri].val, shard_index);

      if(mn <= 0)
        continue;

      float mx = retal_flat[ri].max ? dbr_get_float_fast(data, *retal_flat[ri].max, shard_index) : 0;
      float ch = dbr_get_float_fast(data, *retal_flat[ri].chance, shard_index);

      if(ch > 0 && ch < 100)
      {
        if(mx > mn)
          buf_write(w, "<span color='%s'>%.1f%% Chance of %d - %d %s</span>\n",
                    color, ch, (int)round(mn), (int)round(mx), retal_flat[ri].label);
        else
          buf_write(w, "<span color='%s'>%.1f%% Chance of %d %s</span>\n",
                    color, ch, (int)round(mn), retal_flat[ri].label);
      }

      else
      {
        if(mx > mn)
          buf_write(rw, "<span color='%s'>%s%d - %d %s</span>\n",
                    color, retal_indent, (int)round(mn), (int)round(mx), retal_flat[ri].label);
        else
          buf_write(rw, "<span color='%s'>%s%d %s</span>\n",
                    color, retal_indent, (int)round(mn), retal_flat[ri].label);
      }
    }
  }

  // Stun Retaliation (seconds, may have chance + max range)
  {
    float mn = dbr_get_float_fast(data, INT_retaliationStunMin, shard_index);

    if(mn > 0)
    {
      float mx = dbr_get_float_fast(data, INT_retaliationStunMax, shard_index);
      float ch = dbr_get_float_fast(data, INT_retaliationStunChance, shard_index);

      if(ch > 0 && ch < 100)
      {
        if(mx > mn)
          buf_write(w, "<span color='%s'>%.0f%% Chance of %.1f - %.1f Second Stun Retaliation</span>\n", color, ch, mn, mx);
        else
          buf_write(w, "<span color='%s'>%.0f%% Chance of %.1f Second Stun Retaliation</span>\n", color, ch, mn);
      }
      else
      {
        if(mx > mn)
          buf_write(rw, "<span color='%s'>%s%.1f - %.1f Second Stun Retaliation</span>\n", color, retal_indent, mn, mx);
        else
          buf_write(rw, "<span color='%s'>%s%.1f Second Stun Retaliation</span>\n", color, retal_indent, mn);
      }
    }
  }

  // % of Current Life Retaliation (may have chance + max range)
  {
    float mn = dbr_get_float_fast(data, INT_retaliationPercentCurrentLifeMin, shard_index);

    if(mn > 0)
    {
      float mx = dbr_get_float_fast(data, INT_retaliationPercentCurrentLifeMax, shard_index);
      float ch = dbr_get_float_fast(data, INT_retaliationPercentCurrentLifeChance, shard_index);

      if(ch > 0 && ch < 100)
      {
        if(mx > mn)
          buf_write(w, "<span color='%s'>%.0f%% Chance of %.0f%% - %.0f%% of Current Life Retaliation</span>\n", color, ch, mn, mx);
        else
          buf_write(w, "<span color='%s'>%.0f%% Chance of %.0f%% of Current Life Retaliation</span>\n", color, ch, mn);
      }
      else
      {
        if(mx > mn)
          buf_write(rw, "<span color='%s'>%s%.0f%% - %.0f%% of Current Life Retaliation</span>\n", color, retal_indent, mn, mx);
        else
          buf_write(rw, "<span color='%s'>%s%.0f%% of Current Life Retaliation</span>\n", color, retal_indent, mn);
      }
    }
  }

  // Slow / ability / damage-reduction retaliation debuffs (rate, not damage * duration)
  {
    static const struct {
      const char **min; const char **max; const char **dur; const char **chance;
      const char *label; bool is_percent;
    } retal_debuffs[] = {
      {&INT_retaliationSlowRunSpeedMin,         &INT_retaliationSlowRunSpeedMax,         &INT_retaliationSlowRunSpeedDurationMin,         &INT_retaliationSlowRunSpeedChance,         "Slow Retaliation",                          true},
      {&INT_retaliationSlowDefensiveAbilityMin, &INT_retaliationSlowDefensiveAbilityMax, &INT_retaliationSlowDefensiveAbilityDurationMin, &INT_retaliationSlowDefensiveAbilityChance, "Reduced Defensive Ability Retaliation",     false},
      {&INT_retaliationSlowOffensiveAbilityMin, &INT_retaliationSlowOffensiveAbilityMax, &INT_retaliationSlowOffensiveAbilityDurationMin, &INT_retaliationSlowOffensiveAbilityChance, "Reduced Offensive Ability Retaliation",     false},
      {&INT_retaliationSlowOffensiveReductionMin, &INT_retaliationSlowOffensiveReductionMax, &INT_retaliationSlowOffensiveReductionDurationMin, &INT_retaliationSlowOffensiveReductionChance, "Reduced Damage Retaliation",            true},
    };

    for(int ri = 0; ri < (int)(sizeof retal_debuffs / sizeof retal_debuffs[0]); ri++)
    {
      float mn = dbr_get_float_fast(data, *retal_debuffs[ri].min, shard_index);
      float dur = dbr_get_float_fast(data, *retal_debuffs[ri].dur, shard_index);

      if(mn <= 0 || dur <= 0)
        continue;

      float mx = dbr_get_float_fast(data, *retal_debuffs[ri].max, shard_index);
      float ch = dbr_get_float_fast(data, *retal_debuffs[ri].chance, shard_index);

      char val_str[64];
      const char *pct = retal_debuffs[ri].is_percent ? "%" : "";

      if(mx > mn)
        snprintf(val_str, sizeof(val_str), "%.0f%s - %.0f%s", mn, pct, mx, pct);
      else
        snprintf(val_str, sizeof(val_str), "%.0f%s", mn, pct);

      if(ch > 0 && ch < 100)
        buf_write(w, "<span color='%s'>%.0f%% Chance of %s %s for %.1f Second(s)</span>\n",
                  color, ch, val_str, retal_debuffs[ri].label, dur);
      else
        buf_write(rw, "<span color='%s'>%s%s %s for %.1f Second(s)</span>\n",
                  color, retal_indent, val_str, retal_debuffs[ri].label, dur);
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

  // Reduced Resistances (value + duration, may have chance and max range)
  {
    float val = dbr_get_float_fast(data, INT_offensiveTotalResistanceReductionAbsoluteMin, shard_index);
    float val_max = dbr_get_float_fast(data, INT_offensiveTotalResistanceReductionAbsoluteMax, shard_index);
    float dur = dbr_get_float_fast(data, INT_offensiveTotalResistanceReductionAbsoluteDurationMin, shard_index);
    float ch = dbr_get_float_fast(data, INT_offensiveTotalResistanceReductionAbsoluteChance, shard_index);

    if(val > 0)
    {
      char val_str[64];

      if(val_max > val)
        snprintf(val_str, sizeof(val_str), "%.0f - %.0f", val, val_max);
      else
        snprintf(val_str, sizeof(val_str), "%.0f", val);

      if(ch > 0 && ch < 100 && dur > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %s Reduced Resistances for %.1f Second(s)</span>\n", color, indent, ch, val_str, dur);
      else if(dur > 0)
        buf_write(ow, "<span color='%s'>%s%s Reduced Resistances for %.1f Second(s)</span>\n", color, indent, val_str, dur);
      else
        buf_write(ow, "<span color='%s'>%s%s Reduced Resistances</span>\n", color, indent, val_str);
    }
  }

  // Reduced Resistances (percent variant: e.g. "30% Reduced Resistances for 3 Sec")
  {
    float val = dbr_get_float_fast(data, INT_offensiveTotalResistanceReductionPercentMin, shard_index);
    float dur = dbr_get_float_fast(data, INT_offensiveTotalResistanceReductionPercentDurationMin, shard_index);
    float ch = dbr_get_float_fast(data, INT_offensiveTotalResistanceReductionPercentChance, shard_index);

    if(val > 0)
    {
      if(ch > 0 && ch < 100 && dur > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.0f%% Reduced Resistances for %.1f Second(s)</span>\n", color, indent, ch, val, dur);
      else if(dur > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Resistances for %.1f Second(s)</span>\n", color, indent, val, dur);
      else
        buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Resistances</span>\n", color, indent, val);
    }
  }

  // Skill disruption protection (defensive, no duration) / offensive skill disruption (with duration)
  {
    float val = dbr_get_float_fast(data, INT_defensiveDisruption, shard_index);
    float dur = dbr_get_float_fast(data, INT_defensiveDisruptionDuration, shard_index);

    if(val > 0 && dur > 0)
      buf_write(ow, "<span color='%s'>%s%.1f%% Chance of %.1f Second(s) of Skill Disruption</span>\n", color, indent, val, dur);

    else if(val > 0)
      buf_write(w, "<span color='%s'>%.0f%% Skill Disruption Protection</span>\n", color, val);
  }

  // Offensive skill disruption (offensiveDisruptionMin -- seconds of disruption)
  {
    float val = dbr_get_float_fast(data, INT_offensiveDisruptionMin, shard_index);
    float val_max = dbr_get_float_fast(data, INT_offensiveDisruptionMax, shard_index);
    float ch = dbr_get_float_fast(data, INT_offensiveDisruptionChance, shard_index);

    if(val > 0)
    {
      char val_str[64];

      if(val_max > val)
        snprintf(val_str, sizeof(val_str), "%.1f - %.1f", val, val_max);
      else
        snprintf(val_str, sizeof(val_str), "%.1f", val);

      if(ch > 0 && ch < 100)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %s Second(s) of Skill Disruption</span>\n", color, indent, ch, val_str);
      else
        buf_write(ow, "<span color='%s'>%s%s Second(s) of Skill Disruption</span>\n", color, indent, val_str);
    }
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

  // Reduced Run Speed (value + duration, may have max range)
  {
    float val = dbr_get_float_fast(data, INT_offensiveSlowRunSpeedMin, shard_index);
    float val_max = dbr_get_float_fast(data, INT_offensiveSlowRunSpeedMax, shard_index);
    float dur = dbr_get_float_fast(data, INT_offensiveSlowRunSpeedDurationMin, shard_index);

    if(val > 0)
    {
      char val_str[64];

      if(val_max > val)
        snprintf(val_str, sizeof(val_str), "%.0f%% - %.0f%%", val, val_max);
      else
        snprintf(val_str, sizeof(val_str), "%.0f%%", val);

      if(dur > 0)
        buf_write(ow, "<span color='%s'>%s%s Reduced Run Speed for %.1f Second(s)</span>\n", color, indent, val_str, dur);
      else
        buf_write(ow, "<span color='%s'>%s%s Reduced Run Speed</span>\n", color, indent, val_str);
    }
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
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %+d%% %s</span>\n", color, indent, mc, (int)round(mv), off_mod_defs[mi].label);

      else
        buf_write(ow, "<span color='%s'>%s%+d%% %s</span>\n", color, indent, (int)round(mv), off_mod_defs[mi].label);
    }
  }

  // Retaliation %-modifiers (may have chance)
  {
    static const struct { const char **val; const char **chance; const char *label; } retal_mod_defs[] = {
      {&INT_retaliationPhysicalModifier,  &INT_retaliationPhysicalModifierChance,  "Physical Retaliation"},
      {&INT_retaliationColdModifier,      &INT_retaliationColdModifierChance,      "Cold Retaliation"},
      {&INT_retaliationFireModifier,      &INT_retaliationFireModifierChance,      "Fire Retaliation"},
      {&INT_retaliationLightningModifier, &INT_retaliationLightningModifierChance, "Lightning Retaliation"},
      {&INT_retaliationPoisonModifier,    &INT_retaliationPoisonModifierChance,    "Poison Retaliation"},
      {&INT_retaliationPierceModifier,    &INT_retaliationPierceModifierChance,    "Pierce Retaliation"},
      {&INT_retaliationLifeModifier,      &INT_retaliationLifeModifierChance,      "Vitality Retaliation"},
      {&INT_retaliationStunModifier,      &INT_retaliationStunModifierChance,      "Stun Retaliation"},
      {&INT_retaliationElementalModifier, &INT_retaliationElementalModifierChance, "Elemental Retaliation"},
    };

    for(int mi = 0; mi < (int)(sizeof retal_mod_defs / sizeof retal_mod_defs[0]); mi++)
    {
      float mv = dbr_get_float_fast(data, *retal_mod_defs[mi].val, shard_index);

      if(fabs(mv) < 0.001f)
        continue;

      float mc = dbr_get_float_fast(data, *retal_mod_defs[mi].chance, shard_index);

      if(mc > 0 && mc < 100)
        buf_write(rw, "<span color='%s'>%s%.0f%% Chance of %+d%% %s</span>\n", color, retal_indent, mc, (int)round(mv), retal_mod_defs[mi].label);

      else
        buf_write(rw, "<span color='%s'>%s%+d%% %s</span>\n", color, retal_indent, (int)round(mv), retal_mod_defs[mi].label);
    }
  }

  // Percent current life reduction (may have chance, may have max range)
  {
    float pcl = dbr_get_float_fast(data, INT_offensivePercentCurrentLifeMin, shard_index);

    if(fabs(pcl) > 0.001f)
    {
      float pcl_max = dbr_get_float_fast(data, INT_offensivePercentCurrentLifeMax, shard_index);
      float pcl_chance = dbr_get_float_fast(data, INT_offensivePercentCurrentLifeChance, shard_index);
      bool in_chance = global_chance > 0 && offensive_proc_in_chance(data, "PercentCurrentLife", shard_index);
      BufWriter *target = in_chance ? ow : w;
      const char *target_indent = in_chance ? indent : "";

      if(pcl_chance > 0 && pcl_chance < 100)
      {
        if(pcl_max > pcl)
          buf_write(target, "<span color='%s'>%s%.1f%% Chance of %.0f%% - %.0f%% Reduction to Enemy's Health</span>\n", color, target_indent, pcl_chance, pcl, pcl_max);
        else
          buf_write(target, "<span color='%s'>%s%.1f%% Chance of %.0f%% Reduction to Enemy's Health</span>\n", color, target_indent, pcl_chance, pcl);
      }

      else
      {
        if(pcl_max > pcl)
          buf_write(target, "<span color='%s'>%s%.0f%% - %.0f%% Reduction to Enemy's Health</span>\n", color, target_indent, pcl, pcl_max);
        else
          buf_write(target, "<span color='%s'>%s%.0f%% Reduction to Enemy's Health</span>\n", color, target_indent, pcl);
      }
    }
  }

  // Energy drain / energy burn (both DrainMin and DrainRatioMin are percentages)
  {
    float drain = dbr_get_float_fast(data, INT_offensiveManaBurnDrainMin, shard_index);
    float drain_max = dbr_get_float_fast(data, INT_offensiveManaBurnDrainMax, shard_index);
    float drain_ratio = dbr_get_float_fast(data, INT_offensiveManaBurnDrainRatioMin, shard_index);
    float dmg_ratio = dbr_get_float_fast(data, INT_offensiveManaBurnDamageRatio, shard_index);
    float chance = dbr_get_float_fast(data, INT_offensiveManaBurnChance, shard_index);
    float val = (drain > 0.001f) ? drain : drain_ratio;

    if(val > 0.001f)
    {
      char val_str[64];

      if(drain_max > val)
        snprintf(val_str, sizeof(val_str), "%.0f - %.0f", val, drain_max);
      else
        snprintf(val_str, sizeof(val_str), "%.0f", val);

      const char *prefix = (chance > 0 && chance < 100) ? "Chance of " : "";
      char chance_buf[32] = "";

      if(chance > 0 && chance < 100)
        snprintf(chance_buf, sizeof(chance_buf), "%.0f%% ", chance);

      if(dmg_ratio > 0.001f)
        buf_write(ow, "<span color='%s'>%s%s%s%s%% Energy Drain (%.0f%% of lost energy as damage)</span>\n", color, indent, chance_buf, prefix, val_str, dmg_ratio);

      else
        buf_write(ow, "<span color='%s'>%s%s%s%s%% Energy Drain</span>\n", color, indent, chance_buf, prefix, val_str);
    }

    // Damage to Burning Energy (RatioAdder is +X% damage modifier)
    float ratio_adder = dbr_get_float_fast(data, INT_offensiveManaBurnRatioAdder, shard_index);

    if(fabs(ratio_adder) > 0.001f)
    {
      float ra_chance = dbr_get_float_fast(data, INT_offensiveManaBurnRatioAdderChance, shard_index);

      if(ra_chance > 0 && ra_chance < 100)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %+d%% Damage to Burning Energy</span>\n", color, indent, ra_chance, (int)round(ratio_adder));
      else
        buf_write(ow, "<span color='%s'>%s%+d%% Damage to Burning Energy</span>\n", color, indent, (int)round(ratio_adder));
    }
  }

  // Offensive stun (may have range, chance, modifier)
  {
    float stun_min = dbr_get_float_fast(data, INT_offensiveStunMin, shard_index);
    float stun_max = dbr_get_float_fast(data, INT_offensiveStunMax, shard_index);
    float stun_dur_min = dbr_get_float_fast(data, INT_offensiveStunDurationMin, shard_index);
    float stun_lo = (stun_dur_min > 0) ? stun_dur_min : stun_min;
    float stun_hi = (stun_max > stun_lo) ? stun_max : 0;

    if(stun_lo > 0)
    {
      char val_str[64];

      if(stun_hi > stun_lo)
        snprintf(val_str, sizeof(val_str), "%.1f - %.1f", stun_lo, stun_hi);
      else
        snprintf(val_str, sizeof(val_str), "%.1f", stun_lo);

      float stun_chance = dbr_get_float_fast(data, INT_offensiveStunChance, shard_index);

      if(stun_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %s Second(s) of Stun</span>\n", color, indent, stun_chance, val_str);
      else
        buf_write(ow, "<span color='%s'>%s%s Second(s) of Stun</span>\n", color, indent, val_str);
    }

    float stun_mod = dbr_get_float_fast(data, INT_offensiveStunModifier, shard_index);

    if(fabs(stun_mod) > 0.001f)
      buf_write(ow, "<span color='%s'>%s%+d%% Stun Duration</span>\n", color, indent, (int)round(stun_mod));
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

  // Offensive freeze (may have range)
  {
    float freeze_min = dbr_get_float_fast(data, INT_offensiveFreezeMin, shard_index);
    float freeze_max = dbr_get_float_fast(data, INT_offensiveFreezeMax, shard_index);
    float freeze_dur_min = dbr_get_float_fast(data, INT_offensiveFreezeDurationMin, shard_index);
    float lo = (freeze_dur_min > 0) ? freeze_dur_min : freeze_min;
    float hi = (freeze_max > lo) ? freeze_max : 0;

    if(lo > 0)
    {
      char val_str[64];

      if(hi > lo)
        snprintf(val_str, sizeof(val_str), "%.1f - %.1f", lo, hi);
      else
        snprintf(val_str, sizeof(val_str), "%.1f", lo);

      float freeze_chance = dbr_get_float_fast(data, INT_offensiveFreezeChance, shard_index);

      if(freeze_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %s Second(s) of Freeze</span>\n", color, indent, freeze_chance, val_str);
      else
        buf_write(ow, "<span color='%s'>%s%s Second(s) of Freeze</span>\n", color, indent, val_str);
    }
  }

  // Offensive sleep (range, chance, modifier — Atlantis content)
  {
    float sleep_min = dbr_get_float_fast(data, INT_offensiveSleepMin, shard_index);
    float sleep_max = dbr_get_float_fast(data, INT_offensiveSleepMax, shard_index);
    float sleep_dur_min = dbr_get_float_fast(data, INT_offensiveSleepDurationMin, shard_index);
    float lo = (sleep_dur_min > 0) ? sleep_dur_min : sleep_min;
    float hi = (sleep_max > lo) ? sleep_max : 0;

    if(lo > 0)
    {
      char val_str[64];

      if(hi > lo)
        snprintf(val_str, sizeof(val_str), "%.1f - %.1f", lo, hi);
      else
        snprintf(val_str, sizeof(val_str), "%.1f", lo);

      float sleep_chance = dbr_get_float_fast(data, INT_offensiveSleepChance, shard_index);

      if(sleep_chance > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %s Second(s) of Sleep</span>\n", color, indent, sleep_chance, val_str);
      else
        buf_write(ow, "<span color='%s'>%s%s Second(s) of Sleep</span>\n", color, indent, val_str);
    }

    float sleep_mod = dbr_get_float_fast(data, INT_offensiveSleepModifier, shard_index);

    if(fabs(sleep_mod) > 0.001f)
      buf_write(ow, "<span color='%s'>%s%+d%% Sleep Duration</span>\n", color, indent, (int)round(sleep_mod));
  }

  // Pierce Ratio: damage converted to piercing (raw + modifier)
  {
    float pr_min = dbr_get_float_fast(data, INT_offensivePierceRatioMin, shard_index);
    float pr_max = dbr_get_float_fast(data, INT_offensivePierceRatioMax, shard_index);
    float pr_chance = dbr_get_float_fast(data, INT_offensivePierceRatioChance, shard_index);

    if(pr_min > 0)
    {
      char val_str[64];

      if(pr_max > pr_min)
        snprintf(val_str, sizeof(val_str), "%.0f%% - %.0f%%", pr_min, pr_max);
      else
        snprintf(val_str, sizeof(val_str), "%.0f%%", pr_min);

      if(pr_chance > 0 && pr_chance < 100)
        buf_write(w, "<span color='%s'>%.1f%% Chance of %s Pierce Ratio</span>\n", color, pr_chance, val_str);
      else
        buf_write(w, "<span color='%s'>%s Pierce Ratio</span>\n", color, val_str);
    }

    float pr_mod = dbr_get_float_fast(data, INT_offensivePierceRatioModifier, shard_index);
    float pr_mod_chance = dbr_get_float_fast(data, INT_offensivePierceRatioModifierChance, shard_index);

    if(fabs(pr_mod) > 0.001f)
    {
      if(pr_mod_chance > 0 && pr_mod_chance < 100)
        buf_write(w, "<span color='%s'>%.1f%% Chance of %+d%% Pierce Ratio</span>\n", color, pr_mod_chance, (int)round(pr_mod));
      else
        buf_write(w, "<span color='%s'>%+d%% Pierce Ratio</span>\n", color, (int)round(pr_mod));
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
    {
      float convert_chance = dbr_get_float_fast(data, INT_offensiveConvertChance, shard_index);

      if(convert_chance > 0 && convert_chance < 100)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.1f Seconds of Mind Control</span>\n", color, indent, convert_chance, convert_min);
      else
        buf_write(ow, "<span color='%s'>%s%.1f Seconds of Mind Control</span>\n", color, indent, convert_min);
    }
  }

  // Offensive confusion
  {
    float confuse_min = dbr_get_float_fast(data, INT_offensiveConfusionMin, shard_index);
    float confuse_max = dbr_get_float_fast(data, INT_offensiveConfusionMax, shard_index);
    float confuse_dur = dbr_get_float_fast(data, INT_offensiveConfusionDurationMin, shard_index);

    if(confuse_min > 0)
    {
      float confuse_chance = dbr_get_float_fast(data, INT_offensiveConfusionChance, shard_index);
      float val = confuse_dur > 0 ? confuse_dur : confuse_min;
      float val_max = confuse_max > val ? confuse_max : 0;

      if(confuse_chance > 0)
      {
        if(val_max > 0)
          buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.1f - %.1f Second(s) of Confusion</span>\n", color, indent, confuse_chance, val, val_max);
        else
          buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Confusion</span>\n", color, indent, confuse_chance, val);
      }

      else
      {
        if(val_max > 0)
          buf_write(ow, "<span color='%s'>%s%.1f - %.1f Second(s) of Confusion</span>\n", color, indent, val, val_max);
        else
          buf_write(ow, "<span color='%s'>%s%.1f Second(s) of Confusion</span>\n", color, indent, val);
      }
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

  // Reduced Offensive Ability debuff (applied to enemies)
  {
    float oa_mod = dbr_get_float_fast(data, INT_offensiveSlowOffensiveAbilityModifier, shard_index);
    float oa_dur = dbr_get_float_fast(data, INT_offensiveSlowOffensiveAbilityDurationMin, shard_index);

    if(fabs(oa_mod) > 0.001f && oa_dur > 0)
      buf_write(ow, "<span color='%s'>%s%+d%% Offensive Ability for %.1f Second(s)</span>\n", color, indent, (int)round(-oa_mod), oa_dur);
  }

  // Reduced Defensive Ability debuff (applied to enemies)
  {
    float da_mod = dbr_get_float_fast(data, INT_offensiveSlowOffensiveReductionModifier, shard_index);
    float da_dur = dbr_get_float_fast(data, INT_offensiveSlowOffensiveReductionDurationMin, shard_index);

    if(fabs(da_mod) > 0.001f && da_dur > 0)
      buf_write(ow, "<span color='%s'>%s%+d%% Defensive Ability for %.1f Second(s)</span>\n", color, indent, (int)round(-da_mod), da_dur);
  }

  // Reduced Defensive/Offensive Ability proc-style (Min/Max/Chance, applied to enemies)
  {
    static const struct {
      const char **min; const char **max; const char **dur; const char **chance; const char *label;
    } ability_debuffs[] = {
      {&INT_offensiveSlowDefensiveAbilityMin, &INT_offensiveSlowDefensiveAbilityMax, &INT_offensiveSlowDefensiveAbilityDurationMin, &INT_offensiveSlowDefensiveAbilityChance, "Reduced Defensive Ability"},
      {&INT_offensiveSlowOffensiveAbilityMin, &INT_offensiveSlowOffensiveAbilityMax, &INT_offensiveSlowOffensiveAbilityDurationMin, &INT_offensiveSlowOffensiveAbilityChance, "Reduced Offensive Ability"},
    };

    for(int ad = 0; ad < (int)(sizeof ability_debuffs / sizeof ability_debuffs[0]); ad++)
    {
      float mn = dbr_get_float_fast(data, *ability_debuffs[ad].min, shard_index);
      float dur = dbr_get_float_fast(data, *ability_debuffs[ad].dur, shard_index);

      if(mn <= 0 || dur <= 0)
        continue;

      float mx = dbr_get_float_fast(data, *ability_debuffs[ad].max, shard_index);
      float ch = dbr_get_float_fast(data, *ability_debuffs[ad].chance, shard_index);

      char val_str[64];

      if(mx > mn)
        snprintf(val_str, sizeof(val_str), "%.0f - %.0f", mn, mx);
      else
        snprintf(val_str, sizeof(val_str), "%.0f", mn);

      if(ch > 0 && ch < 100)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %s %s for %.1f Second(s)</span>\n",
                  color, indent, ch, val_str, ability_debuffs[ad].label, dur);
      else
        buf_write(ow, "<span color='%s'>%s%s %s for %.1f Second(s)</span>\n",
                  color, indent, val_str, ability_debuffs[ad].label, dur);
    }
  }

  // Slow total speed (with optional chance and duration)
  {
    float slow_min = dbr_get_float_fast(data, INT_offensiveSlowTotalSpeedMin, shard_index);

    if(slow_min > 0)
    {
      float slow_ch = dbr_get_float_fast(data, INT_offensiveSlowTotalSpeedChance, shard_index);
      float slow_dur = dbr_get_float_fast(data, INT_offensiveSlowTotalSpeedDurationMin, shard_index);

      if(slow_ch > 0 && slow_dur > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Chance of %.0f%% Reduced Total Speed for %.1f Second(s)</span>\n", color, indent, slow_ch, slow_min, slow_dur);
      else if(slow_dur > 0)
        buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Total Speed for %.1f Second(s)</span>\n", color, indent, slow_min, slow_dur);
      else
        buf_write(ow, "<span color='%s'>%s%.0f%% Reduced Total Speed</span>\n", color, indent, slow_min);
    }
  }

  // -- End of offensive sections --
  // (non-offensive sections follow below, then chance group is flushed at the end)

  // Shield block: raw value, raw chance, and absorption
  {
    float blk_val = dbr_get_float_fast(data, INT_defensiveBlock, shard_index);
    float blk_ch = dbr_get_float_fast(data, INT_defensiveBlockChance, shard_index);
    float blk_abs = dbr_get_float_fast(data, INT_defensiveAbsorption, shard_index);

    if(blk_ch > 0)
      buf_write(w, "<span color='%s'>%.0f%% Shield Block Chance</span>\n", color, blk_ch);

    if(blk_val > 0)
      buf_write(w, "<span color='%s'>%.0f Damage Blocked</span>\n", color, blk_val);

    if(blk_abs > 0)
      buf_write(w, "<span color='%s'>%.0f%% Damage Absorption</span>\n", color, blk_abs);
  }

  // Petrify resistance (% reduced petrify duration)
  {
    float val = dbr_get_float_fast(data, INT_defensivePetrify, shard_index);

    if(val > 0)
      buf_write(w, "<span color='%s'>%.0f%% Reduced Petrify Duration</span>\n", color, val);
  }

  // Racial bonus
  {
    const char *race = "Enemies";
    TQVariable *rv = arz_record_get_var(data, INT_racialBonusRace);

    if(rv && rv->type == TQ_VAR_STRING && rv->count > 0 && rv->value.str[0])
      race = rv->value.str[0];

    float dmg = dbr_get_float_fast(data, INT_racialBonusPercentDamage, shard_index);

    if(fabs(dmg) > 0.001f)
      buf_write(w, "<span color='%s'>%+d%% Damage to %ss</span>\n", color, (int)round(dmg), race);

    float abs_dmg = dbr_get_float_fast(data, INT_racialBonusAbsoluteDamage, shard_index);

    if(fabs(abs_dmg) > 0.001f)
      buf_write(w, "<span color='%s'>%+d Damage to %ss</span>\n", color, (int)round(abs_dmg), race);

    float def = dbr_get_float_fast(data, INT_racialBonusPercentDefense, shard_index);

    if(fabs(def) > 0.001f)
      buf_write(w, "<span color='%s'>%d%% less damage from %ss</span>\n", color, (int)round(def), race);

    float abs_def = dbr_get_float_fast(data, INT_racialBonusAbsoluteDefense, shard_index);

    if(fabs(abs_def) > 0.001f)
      buf_write(w, "<span color='%s'>%d Less Damage from %ss</span>\n", color, (int)round(abs_def), race);
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

  // DoT resistances / duration reductions with optional chance
  {
    static const struct { const char **val; const char **chance; const char *label; } dot_resist[] = {
      {&INT_defensiveSlowLifeLeach,  &INT_defensiveSlowLifeLeachChance,  "Vitality Decay Resistance"},
      {&INT_defensiveSlowManaLeach,  &INT_defensiveSlowManaLeachChance,  "Energy Drain Resistance"},
      {&INT_defensivePoisonDuration, &INT_defensivePoisonDurationChance, "Reduced Poison Duration"},
    };

    for(int ri = 0; ri < (int)(sizeof dot_resist / sizeof dot_resist[0]); ri++)
    {
      float rv = dbr_get_float_fast(data, *dot_resist[ri].val, shard_index);

      if(fabs(rv) < 0.001f)
        continue;

      float rc = dbr_get_float_fast(data, *dot_resist[ri].chance, shard_index);

      if(rc > 0 && rc < 100)
        buf_write(w, "<span color='%s'>%.0f%% Chance of %+d%% %s</span>\n", color, rc, (int)round(rv), dot_resist[ri].label);
      else
        buf_write(w, "<span color='%s'>%+d%% %s</span>\n", color, (int)round(rv), dot_resist[ri].label);
    }
  }

  // Damage reflected (may have chance)
  {
    float rv = dbr_get_float_fast(data, INT_defensiveReflect, shard_index);

    if(rv > 0)
    {
      float rc = dbr_get_float_fast(data, INT_defensiveReflectChance, shard_index);

      if(rc > 0 && rc < 100)
        buf_write(w, "<span color='%s'>%.0f%% Chance of %d Damage Reflected</span>\n", color, rc, (int)round(rv));
      else
        buf_write(w, "<span color='%s'>%d Damage Reflected</span>\n", color, (int)round(rv));
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


  // Follow petBonusName reference (LootRandomizer pet bonus sub-records)
  {
    const char *pet_bonus = record_get_string_fast(data, INT_petBonusName);

    if(pet_bonus && pet_bonus[0])
    {
      buf_write(w, "\n<span color='%s'>Bonus to All Pets:</span>\n", color);
      add_stats_from_record(pet_bonus, tr, w, color, shard_index);
    }
  }

  // Flush the global-chance-wrapped section (relics/charms/armor/jewelry).
  if(global_chance > 0 && ow_writer.pos > 0)
  {
    buf_write(w, "<span color='%s'>%.0f%% Chance of:</span>\n", color, global_chance);
    buf_write(w, "%s", ow_buffer);
  }

  // Flush the retaliation global-chance section (separate header, separate buffer).
  if(retal_global_chance > 0 && rw_writer.pos > 0)
  {
    buf_write(w, "<span color='%s'>%.0f%% Chance of:</span>\n", color, retal_global_chance);
    buf_write(w, "%s", rw_buffer);
  }
}
