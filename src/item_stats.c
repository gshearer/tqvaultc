#include "item_stats.h"
#include "arz.h"
#include "asset_lookup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>
#include <stdarg.h>

/* ── BufWriter: O(1) append instead of O(N) strlen ──────────────── */

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} BufWriter;

static void buf_init(BufWriter *w, char *buffer, size_t size) {
    w->buf = buffer;
    w->size = size;
    w->pos = 0;
    if (size > 0) buffer[0] = '\0';
}

static void buf_write(BufWriter *w, const char *fmt, ...) {
    if (w->pos >= w->size - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->pos, w->size - w->pos, fmt, ap);
    va_end(ap);
    if (n > 0) w->pos += (size_t)n;
    if (w->pos >= w->size) w->pos = w->size - 1;
}

/* ── AttributeMap with interned pointers ─────────────────────────── */

typedef struct {
    const char *variable;
    const char *format;
    bool is_percent;
    const char *interned; /* resolved at init time */
} AttributeMap;

static AttributeMap attr_maps[] = {
    /* Character stats */
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
    {"characterLifeRegen", "+%.1f Health Regeneration per second", false, NULL},
    {"characterManaRegen", "+%.1f Energy Regeneration per second", false, NULL},
    {"characterAttackSpeedModifier", "+%d%% Attack Speed", true, NULL},
    {"characterSpellCastSpeedModifier", "+%d%% Casting Speed", true, NULL},
    {"characterRunSpeedModifier", "+%d%% Movement Speed", true, NULL},
    {"characterDeflectProjectile", "%.0f%% Chance to Dodge Projectiles", false, NULL},
    {"characterDodgePercent", "%.0f%% Chance to Avoid Melee Attacks", false, NULL},
    {"characterEnergyAbsorptionPercent", "%.0f%% Energy Absorbed from Enemy Spells", false, NULL},

    /* Offensive/Defensive ability */
    {"characterOffensiveAbility", "+%d Offensive Ability", false, NULL},
    {"characterDefensiveAbility", "+%d Defensive Ability", false, NULL},
    {"characterOffensiveAbilityModifier", "+%d%% Offensive Ability", true, NULL},
    {"characterDefensiveAbilityModifier", "+%d%% Defensive Ability", true, NULL},

    /* Offensive percent modifiers */
    {"offensivePhysicalModifier", "+%d%% Physical Damage", true, NULL},
    {"offensiveFireModifier", "+%d%% Fire Damage", true, NULL},
    {"offensiveColdModifier", "+%d%% Cold Damage", true, NULL},
    {"offensiveLightningModifier", "+%d%% Lightning Damage", true, NULL},
    {"offensivePoisonModifier", "+%d%% Poison Damage", true, NULL},
    {"offensivePierceModifier", "+%d%% Pierce Damage", true, NULL},
    {"offensiveElementalModifier", "+%d%% Elemental Damage", true, NULL},
    /* offensiveTotalDamageModifier handled in dedicated block (has Chance) */

    /* Offensive DoT modifiers */
    {"offensiveSlowFireModifier", "+%d%% Burn Damage", true, NULL},
    {"offensiveSlowColdModifier", "+%d%% Frostburn Damage", true, NULL},
    {"offensiveSlowLightningModifier", "+%d%% Electrical Burn Damage", true, NULL},
    {"offensiveSlowPoisonModifier", "+%d%% Poison Damage", true, NULL},
    {"offensiveSlowLifeLeachModifier", "+%d%% Life Leech", true, NULL},
    {"offensiveSlowLifeModifier", "+%d%% Vitality Decay", true, NULL},

    /* Armor */
    {"defensiveProtection", "%d Armor", false, NULL},
    {"defensiveProtectionModifier", "+%d%% Armor", true, NULL},
    {"defensiveAbsorptionModifier", "+%d%% Armor Absorption", true, NULL},

    /* Resistances */
    {"defensiveFire", "%+d%% Fire Resistance", false, NULL},
    {"defensiveCold", "%+d%% Cold Resistance", false, NULL},
    {"defensiveLightning", "%+d%% Lightning Resistance", false, NULL},
    {"defensivePoison", "%+d%% Poison Resistance", false, NULL},
    {"defensivePierce", "%+d%% Pierce Resistance", false, NULL},
    {"defensiveLife", "%+d%% Vitality Resistance", false, NULL},
    {"defensiveBleeding", "%+d%% Bleeding Resistance", false, NULL},
    {"defensivePhysical", "%+d%% Physical Resistance", false, NULL},
    {"defensiveElementalResistance", "%+d%% Elemental Resistance", false, NULL},
    {"defensiveStun", "%+d%% Stun Resistance", false, NULL},
    {"defensiveStunModifier", "+%d%% Reduced Stun Duration", true, NULL},

    /* Duration reductions */
    {"defensiveFreeze", "%+d%% Reduced Freeze Duration", false, NULL},
    {"defensiveFreezeModifier", "+%d%% Reduced Freeze Duration", true, NULL},
    {"defensiveDisruption", "%.1f%% Reduced Skill Disruption", false, NULL},
    {"defensiveSlowLifeLeach", "%+d%% Vitality Decay Resistance", false, NULL},
    {"defensiveSlowManaLeach", "%+d%% Energy Drain Resistance", false, NULL},

    /* Retaliation */
    {"retaliationFireMin", "%d Fire Retaliation", false, NULL},
    {"retaliationColdMin", "%d Cold Retaliation", false, NULL},
    {"retaliationLightningMin", "%d Lightning Retaliation", false, NULL},
    {"retaliationPierceMin", "%d Pierce Retaliation", false, NULL},
    {"retaliationPhysicalMin", "%d Physical Retaliation", false, NULL},

    /* Misc offensive */
    {"offensivePierceRatioMin", "%.0f%% Pierce Ratio", false, NULL},
    {"piercingProjectile", "%d%% Chance to pass through Enemies", true, NULL},
    {"offensiveManaBurnDrainMin", "%d Energy Burned", false, NULL},
    {"offensiveManaBurnDrainRatioMin", "%.0f%% Energy Burned", false, NULL},

    /* offensivePercentCurrentLifeMin handled in dedicated block (has Chance) */

    /* Flat damage (non-range, for bonus summaries) */
    {"offensivePierceMin", "%d Pierce Damage", false, NULL},
    {"offensiveStunMin", "%.1f Second Stun", false, NULL},
    {"offensiveElementalMin", "%d Elemental Damage", false, NULL},
    {"offensiveLifeMin", "%d Vitality Damage", false, NULL},
    /* Offensive chance-based */
    {"offensiveStunChance", "%.0f%% Chance to Stun", false, NULL},

    /* Energy leech / drain over time */
    {"offensiveSlowManaLeachMin", "%d Energy Leech over time", false, NULL},

    /* Slow (total speed reduction) */
    {"offensiveSlowTotalSpeedMin", "%.0f%% Reduced Total Speed", false, NULL},

    /* Retaliation DoT */
    {"retaliationSlowLifeMin", "%d Vitality Decay Retaliation", false, NULL},

    /* Shield */
    {"defensiveBlockModifier", "+%d%% Shield Block Chance", true, NULL},
    {"defensiveBlockModifierChance", "+%d%% Shield Block Chance", true, NULL},

    /* Poison/Disruption duration */
    {"defensivePoisonDuration", "%+d%% Reduced Poison Duration", false, NULL},

    /* Regen modifiers */
    {"characterLifeRegenModifier", "+%d%% Health Regeneration", true, NULL},
    {"characterManaRegenModifier", "+%d%% Energy Regeneration", true, NULL},

    /* Projectile speed */
    {"skillProjectileSpeedModifier", "+%d%% Projectile Speed", true, NULL},

    /* Misc */
    {"characterTotalSpeedModifier", "+%d%% Total Speed", true, NULL},
    {"skillCooldownReduction", "-%.0f%% Recharge", false, NULL},
    {"skillManaCostReduction", "+%.0f%% Skill Energy Cost Reduction", false, NULL},
    {"augmentAllLevel", "+%d to all Skills", false, NULL},
    {"characterIncreasedExperience", "%+d%% Increased Experience", false, NULL},

    {NULL, NULL, false, NULL}
};

/* ── hash-based lookup tables (built at init) ────────────────────── */

static GHashTable *g_skip_set = NULL;   /* interned ptr -> (gpointer)1 */
static GHashTable *g_attr_map_ht = NULL; /* interned ptr -> &attr_maps[i] */

/* Pre-interned variable name pointers for frequently used names */
static const char *INT_offensivePhysicalMin, *INT_offensivePhysicalMax;
static const char *INT_offensiveFireMin, *INT_offensiveFireMax;
static const char *INT_offensiveColdMin, *INT_offensiveColdMax;
static const char *INT_offensiveLightningMin, *INT_offensiveLightningMax;
static const char *INT_offensivePoisonMin, *INT_offensivePoisonMax;
static const char *INT_offensivePierceMin, *INT_offensivePierceMax;
static const char *INT_offensiveElementalMin, *INT_offensiveElementalMax;
static const char *INT_offensiveManaLeechMin, *INT_offensiveManaLeechMax;
static const char *INT_offensiveBasePhysicalMin, *INT_offensiveBasePhysicalMax;
static const char *INT_offensiveBaseColdMin, *INT_offensiveBaseColdMax;
static const char *INT_offensiveBaseFireMin, *INT_offensiveBaseFireMax;
static const char *INT_offensiveBaseLightningMin, *INT_offensiveBaseLightningMax;
static const char *INT_offensiveBasePoisonMin, *INT_offensiveBasePoisonMax;
static const char *INT_offensiveBaseLifeMin, *INT_offensiveBaseLifeMax;
static const char *INT_offensiveLifeLeechMin, *INT_offensiveLifeLeechMax;
static const char *INT_offensiveSlowFireMin, *INT_offensiveSlowFireMax, *INT_offensiveSlowFireDurationMin;
static const char *INT_offensiveSlowLightningMin, *INT_offensiveSlowLightningMax, *INT_offensiveSlowLightningDurationMin;
static const char *INT_offensiveSlowColdMin, *INT_offensiveSlowColdMax, *INT_offensiveSlowColdDurationMin;
static const char *INT_offensiveSlowPoisonMin, *INT_offensiveSlowPoisonMax, *INT_offensiveSlowPoisonDurationMin;
static const char *INT_offensiveSlowLifeLeachMin, *INT_offensiveSlowLifeLeachMax, *INT_offensiveSlowLifeLeachDurationMin;
static const char *INT_offensiveSlowLifeMin, *INT_offensiveSlowLifeMax, *INT_offensiveSlowLifeDurationMin;
static const char *INT_offensiveSlowManaLeachMin, *INT_offensiveSlowManaLeachMax, *INT_offensiveSlowManaLeachDurationMin;
static const char *INT_offensiveSlowBleedingMin, *INT_offensiveSlowBleedingMax, *INT_offensiveSlowBleedingDurationMin;
static const char *INT_offensiveSlowBleedingModifier, *INT_offensiveSlowBleedingModifierChance;
static const char *INT_offensiveSlowDefensiveReductionMin, *INT_offensiveSlowDefensiveReductionDurationMin;
static const char *INT_offensiveSlowAttackSpeedMin, *INT_offensiveSlowAttackSpeedDurationMin;
static const char *INT_offensiveSlowRunSpeedMin, *INT_offensiveSlowRunSpeedDurationMin;
static const char *INT_offensiveStunMin, *INT_offensiveStunDurationMin, *INT_offensiveStunChance;
static const char *INT_offensiveFumbleMin, *INT_offensiveFumbleDurationMin, *INT_offensiveFumbleChance;
static const char *INT_offensiveFreezeMin, *INT_offensiveFreezeDurationMin, *INT_offensiveFreezeChance;
static const char *INT_offensivePetrifyMin, *INT_offensivePetrifyDurationMin, *INT_offensivePetrifyChance;
static const char *INT_offensiveConfusionMin, *INT_offensiveConfusionDurationMin, *INT_offensiveConfusionChance;
static const char *INT_offensiveFearMin, *INT_offensiveFearMax, *INT_offensiveFearChance;
static const char *INT_offensiveConvertMin;
static const char *INT_offensiveTotalDamageModifier, *INT_offensiveTotalDamageModifierChance;
static const char *INT_offensivePercentCurrentLifeMin, *INT_offensivePercentCurrentLifeChance;
static const char *INT_offensiveTotalDamageReductionPercentMin, *INT_offensiveTotalDamageReductionPercentChance;
static const char *INT_offensiveTotalDamageReductionPercentDurationMin;
static const char *INT_racialBonusPercentDamage, *INT_racialBonusPercentDefense, *INT_racialBonusRace;
static const char *INT_petBonusName;
static const char *INT_skillCooldownTime, *INT_refreshTime;
static const char *INT_skillTargetNumber, *INT_skillActiveDuration, *INT_skillTargetRadius;
static const char *INT_offensiveGlobalChance;
static const char *INT_offensiveSlowLightningDurationMax, *INT_offensiveSlowFireDurationMax;
static const char *INT_offensiveSlowColdDurationMax, *INT_offensiveSlowPoisonDurationMax;
static const char *INT_defensiveDisruption, *INT_defensiveDisruptionDuration;
static const char *INT_itemNameTag, *INT_description, *INT_lootRandomizerName, *INT_FileDescription;
static const char *INT_itemClassification, *INT_itemText;
static const char *INT_characterBaseAttackSpeedTag, *INT_artifactClassification;
static const char *INT_itemSkillName, *INT_buffSkillName, *INT_skillDisplayName;
static const char *INT_itemSkillAutoController, *INT_triggerType, *INT_itemSkillLevel;
static const char *INT_skillBaseDescription, *INT_petSkillName, *INT_skillChanceWeight;
static const char *INT_itemSetName, *INT_setName, *INT_setMembers;
static const char *INT_completedRelicLevel;
static const char *INT_dexterityRequirement, *INT_intelligenceRequirement;
static const char *INT_strengthRequirement, *INT_levelRequirement;
static const char *INT_itemLevel, *INT_itemCostName, *INT_Class;

#define INTERN(name) INT_##name = arz_intern(#name)

void item_stats_init(void) {
    /* Pre-intern all attr_maps variable names */
    for (int i = 0; attr_maps[i].variable; i++)
        attr_maps[i].interned = arz_intern(attr_maps[i].variable);

    /* Build skip_set */
    static const char *skip_var_names[] = {
        "offensivePhysicalMin", "offensivePhysicalMax",
        "offensiveFireMin", "offensiveFireMax",
        "offensiveColdMin", "offensiveColdMax",
        "offensiveLightningMin", "offensiveLightningMax",
        "offensivePoisonMin", "offensivePoisonMax",
        "offensivePierceMin", "offensivePierceMax",
        "offensiveElementalMin", "offensiveElementalMax",
        "offensiveLifeLeechMin", "offensiveLifeLeechMax",
        "offensiveManaLeechMin", "offensiveManaLeechMax",
        "offensiveSlowFireMin", "offensiveSlowFireMax", "offensiveSlowFireDurationMin",
        "offensiveSlowLightningMin", "offensiveSlowLightningMax", "offensiveSlowLightningDurationMin",
        "offensiveSlowColdMin", "offensiveSlowColdMax", "offensiveSlowColdDurationMin",
        "offensiveSlowPoisonMin", "offensiveSlowPoisonMax", "offensiveSlowPoisonDurationMin",
        "offensiveSlowLifeLeachMin", "offensiveSlowLifeLeachMax", "offensiveSlowLifeLeachDurationMin",
        "offensiveSlowLifeMin", "offensiveSlowLifeMax", "offensiveSlowLifeDurationMin",
        "offensiveSlowBleedingMin", "offensiveSlowBleedingMax", "offensiveSlowBleedingDurationMin",
        "offensiveSlowManaLeachMin", "offensiveSlowManaLeachMax", "offensiveSlowManaLeachDurationMin",
        "offensiveSlowBleedingModifier", "offensiveSlowBleedingModifierChance",
        "offensiveSlowDefensiveReductionMin", "offensiveSlowDefensiveReductionDurationMin",
        "offensiveSlowAttackSpeedMin", "offensiveSlowAttackSpeedDurationMin",
        "offensiveSlowRunSpeedMin", "offensiveSlowRunSpeedDurationMin",
        "offensiveStunMin", "offensiveStunDurationMin", "offensiveStunChance",
        "offensiveFearMin", "offensiveFearMax", "offensiveFearChance",
        "offensiveConvertMin",
        "offensiveTotalDamageReductionPercentMin", "offensiveTotalDamageReductionPercentChance",
        "offensiveTotalDamageReductionPercentDurationMin",
        "offensiveTotalDamageModifier", "offensiveTotalDamageModifierChance",
        "offensivePercentCurrentLifeMin", "offensivePercentCurrentLifeChance",
        "offensiveGlobalChance",
        "offensiveBasePhysicalMin", "offensiveBasePhysicalMax",
        "offensiveBaseColdMin", "offensiveBaseColdMax",
        "offensiveBaseFireMin", "offensiveBaseFireMax",
        "offensiveBaseLightningMin", "offensiveBaseLightningMax",
        "offensiveBasePoisonMin", "offensiveBasePoisonMax",
        "offensiveBaseLifeMin", "offensiveBaseLifeMax",
        "defensiveDisruption", "defensiveDisruptionDuration",
        "racialBonusPercentDamage", "racialBonusPercentDefense", "racialBonusRace",
        NULL
    };

    g_skip_set = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (const char **sp = skip_var_names; *sp; sp++)
        g_hash_table_insert(g_skip_set, (gpointer)arz_intern(*sp), (gpointer)1);

    /* Build attr_map_ht */
    g_attr_map_ht = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (int i = 0; attr_maps[i].variable; i++)
        g_hash_table_insert(g_attr_map_ht, (gpointer)attr_maps[i].interned, &attr_maps[i]);

    /* Pre-intern all frequently used variable names */
    INTERN(offensivePhysicalMin); INTERN(offensivePhysicalMax);
    INTERN(offensiveFireMin); INTERN(offensiveFireMax);
    INTERN(offensiveColdMin); INTERN(offensiveColdMax);
    INTERN(offensiveLightningMin); INTERN(offensiveLightningMax);
    INTERN(offensivePoisonMin); INTERN(offensivePoisonMax);
    INTERN(offensivePierceMin); INTERN(offensivePierceMax);
    INTERN(offensiveElementalMin); INTERN(offensiveElementalMax);
    INTERN(offensiveManaLeechMin); INTERN(offensiveManaLeechMax);
    INTERN(offensiveBasePhysicalMin); INTERN(offensiveBasePhysicalMax);
    INTERN(offensiveBaseColdMin); INTERN(offensiveBaseColdMax);
    INTERN(offensiveBaseFireMin); INTERN(offensiveBaseFireMax);
    INTERN(offensiveBaseLightningMin); INTERN(offensiveBaseLightningMax);
    INTERN(offensiveBasePoisonMin); INTERN(offensiveBasePoisonMax);
    INTERN(offensiveBaseLifeMin); INTERN(offensiveBaseLifeMax);
    INTERN(offensiveLifeLeechMin); INTERN(offensiveLifeLeechMax);
    INTERN(offensiveSlowFireMin); INTERN(offensiveSlowFireMax); INTERN(offensiveSlowFireDurationMin);
    INTERN(offensiveSlowLightningMin); INTERN(offensiveSlowLightningMax); INTERN(offensiveSlowLightningDurationMin);
    INTERN(offensiveSlowColdMin); INTERN(offensiveSlowColdMax); INTERN(offensiveSlowColdDurationMin);
    INTERN(offensiveSlowPoisonMin); INTERN(offensiveSlowPoisonMax); INTERN(offensiveSlowPoisonDurationMin);
    INTERN(offensiveSlowLifeLeachMin); INTERN(offensiveSlowLifeLeachMax); INTERN(offensiveSlowLifeLeachDurationMin);
    INTERN(offensiveSlowLifeMin); INTERN(offensiveSlowLifeMax); INTERN(offensiveSlowLifeDurationMin);
    INTERN(offensiveSlowManaLeachMin); INTERN(offensiveSlowManaLeachMax); INTERN(offensiveSlowManaLeachDurationMin);
    INTERN(offensiveSlowBleedingMin); INTERN(offensiveSlowBleedingMax); INTERN(offensiveSlowBleedingDurationMin);
    INTERN(offensiveSlowBleedingModifier); INTERN(offensiveSlowBleedingModifierChance);
    INTERN(offensiveSlowDefensiveReductionMin); INTERN(offensiveSlowDefensiveReductionDurationMin);
    INTERN(offensiveSlowAttackSpeedMin); INTERN(offensiveSlowAttackSpeedDurationMin);
    INTERN(offensiveSlowRunSpeedMin); INTERN(offensiveSlowRunSpeedDurationMin);
    INTERN(offensiveStunMin); INTERN(offensiveStunDurationMin); INTERN(offensiveStunChance);
    INTERN(offensiveFumbleMin); INTERN(offensiveFumbleDurationMin); INTERN(offensiveFumbleChance);
    INTERN(offensiveFreezeMin); INTERN(offensiveFreezeDurationMin); INTERN(offensiveFreezeChance);
    INTERN(offensivePetrifyMin); INTERN(offensivePetrifyDurationMin); INTERN(offensivePetrifyChance);
    INTERN(offensiveConfusionMin); INTERN(offensiveConfusionDurationMin); INTERN(offensiveConfusionChance);
    INTERN(offensiveFearMin); INTERN(offensiveFearMax); INTERN(offensiveFearChance);
    INTERN(offensiveConvertMin);
    INTERN(offensiveTotalDamageModifier); INTERN(offensiveTotalDamageModifierChance);
    INTERN(offensivePercentCurrentLifeMin); INTERN(offensivePercentCurrentLifeChance);
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

void item_stats_free(void) {
    if (g_skip_set) { g_hash_table_destroy(g_skip_set); g_skip_set = NULL; }
    if (g_attr_map_ht) { g_hash_table_destroy(g_attr_map_ht); g_attr_map_ht = NULL; }
}

/* ── helpers ───────────────────────────────────────────────────────── */

/* Fast variable lookup using interned name + var_index */
static inline float dbr_get_float_fast(TQArzRecordData *data, const char *interned_name, int si) {
    TQVariable *v = arz_record_get_var(data, interned_name);
    if (!v || !v->value.i32) return 0.0f;
    int idx = (si < (int)v->count) ? si : (int)v->count - 1;
    if (idx < 0) idx = 0;
    return (v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx];
}

/* Get string variable from a pre-fetched record using interned name.
 * Returns internal pointer (do NOT free). */
static inline const char* record_get_string_fast(TQArzRecordData *data, const char *interned_name) {
    if (!data) return NULL;
    TQVariable *v = arz_record_get_var(data, interned_name);
    if (!v || v->type != TQ_VAR_STRING || v->count == 0) return NULL;
    return v->value.str[0];
}

/* Get string variable by loading a record path first.
 * Returns internal pointer (do NOT free). */
static const char* get_record_variable_string(const char *record_path, const char *interned_name) {
    if (!record_path || !record_path[0]) return NULL;
    TQArzRecordData *data = asset_get_dbr(record_path);
    if (!data) return NULL;
    return record_get_string_fast(data, interned_name);
}

static bool path_contains_ci(const char *path, const char *needle);

static const char* get_item_color(const char *base_name, const char *prefix_name, const char *suffix_name) {
    if (!base_name) return "white";

    /* 1. BROKEN prefix check */
    if (prefix_name && prefix_name[0]) {
        const char *pfx_class = get_record_variable_string(prefix_name, INT_itemClassification);
        if (pfx_class && strcasecmp(pfx_class, "Broken") == 0)
            return "#999999";
    }

    /* 2. Special item types by path (case-insensitive — vault paths use mixed case) */
    if (path_contains_ci(base_name, "\\artifacts\\") && !path_contains_ci(base_name, "\\arcaneformulae\\"))
        return "#00FFD1";
    if (path_contains_ci(base_name, "\\arcaneformulae\\"))
        return "#00FFD1";
    if (path_contains_ci(base_name, "\\scrolls\\"))
        return "#91CB00";
    if (path_contains_ci(base_name, "parchment"))
        return "#00A3FF";
    if (path_contains_ci(base_name, "\\relics\\") || path_contains_ci(base_name, "\\charms\\"))
        return "#FFAD00";
    if (path_contains_ci(base_name, "\\oneshot\\potion"))
        return "#FF0000";
    if (path_contains_ci(base_name, "quest"))
        return "#D905FF";

    /* 3. Base item classification from DBR */
    const char *base_class = get_record_variable_string(base_name, INT_itemClassification);
    if (base_class) {
        if (strcasecmp(base_class, "Epic") == 0) return "#00A3FF";
        if (strcasecmp(base_class, "Legendary") == 0) return "#D905FF";
        if (strcasecmp(base_class, "Rare") == 0) return "#40FF40";
    }

    /* 4. Prefix/suffix classification == RARE */
    if (prefix_name && prefix_name[0]) {
        const char *pfx_class = get_record_variable_string(prefix_name, INT_itemClassification);
        if (pfx_class && strcasecmp(pfx_class, "Rare") == 0)
            return "#40FF40";
    }
    if (suffix_name && suffix_name[0]) {
        const char *sfx_class = get_record_variable_string(suffix_name, INT_itemClassification);
        if (sfx_class && strcasecmp(sfx_class, "Rare") == 0)
            return "#40FF40";
    }

    /* 5. Has any prefix or suffix → common (yellow) */
    if ((prefix_name && prefix_name[0]) || (suffix_name && suffix_name[0]))
        return "#FFF52B";

    /* 6. Default → mundane (white) */
    return "white";
}

/* Derive a human-readable name from a DBR path when translation tags are missing. */
static char* pretty_name_from_path(const char *path) {
    if (!path) return strdup("Unknown");
    const char *sep = strrchr(path, '\\');
    if (!sep) sep = strrchr(path, '/');
    const char *fname = sep ? sep + 1 : path;
    int len = (int)strlen(fname);
    if (len > 4 && strcasecmp(fname + len - 4, ".dbr") == 0)
        len -= 4;

    const char *start = fname;
    const char *end = fname + len;
    while (start < end && (*start >= '0' && *start <= '9')) start++;
    if (start < end && *start == '_') start++;
    const char *us = start;
    while (us < end && *us != '_') us++;
    if (us < end && *us == '_' && (us - start) <= 4) start = us + 1;

    char buf[256];
    int pos = 0;
    bool prev_lower = false;
    for (const char *p = start; p < end && pos < (int)sizeof(buf) - 2; p++) {
        char c = *p;
        if (c == '_') {
            if (pos > 0) buf[pos++] = ' ';
            prev_lower = false;
            continue;
        }
        if (prev_lower && c >= 'A' && c <= 'Z' && pos > 0)
            buf[pos++] = ' ';
        if ((pos == 0 || buf[pos-1] == ' ') && c >= 'a' && c <= 'z')
            c = c - 32;
        buf[pos++] = c;
        prev_lower = (c >= 'a' && c <= 'z');
    }
    buf[pos] = '\0';
    if (pos == 0) return strdup("Unknown");
    return strdup(buf);
}

/* Fast-path escape_markup: scan first, strdup if clean */
static char* escape_markup(const char *str) {
    if (!str) return strdup("");

    /* Fast path: check if any special chars exist */
    const char *p = str;
    for (; *p; p++) {
        if (*p == '&' || *p == '<' || *p == '>' || *p == '\'' || *p == '\"')
            goto slow_path;
    }
    return strdup(str);

slow_path:;
    /* Worst case: every char becomes &quot; (6 chars) */
    size_t slen = strlen(str);
    char *out = malloc(slen * 6 + 1);
    char *d = out;
    for (p = str; *p; p++) {
        switch (*p) {
            case '&': memcpy(d, "&amp;", 5); d += 5; break;
            case '<': memcpy(d, "&lt;", 4); d += 4; break;
            case '>': memcpy(d, "&gt;", 4); d += 4; break;
            case '\'': memcpy(d, "&apos;", 6); d += 6; break;
            case '\"': memcpy(d, "&quot;", 6); d += 6; break;
            default: *d++ = *p; break;
        }
    }
    *d = '\0';
    return out;
}

char* item_bonus_stat_summary(const char *record_path) {
    if (!record_path || !record_path[0]) return NULL;
    TQArzRecordData *data = asset_get_dbr(record_path);
    if (!data) return NULL;

    char buf[256];
    buf[0] = '\0';
    int found = 0;

    for (int a = 0; attr_maps[a].variable && found < 3; a++) {
        float val = dbr_get_float_fast(data, attr_maps[a].interned, 0);
        if (val == 0.0f) continue;

        if (found > 0) {
            size_t len = strlen(buf);
            if (len < sizeof(buf) - 3) {
                buf[len] = ',';
                buf[len+1] = ' ';
                buf[len+2] = '\0';
            }
        }

        char part[80];
        const char *pct = strchr(attr_maps[a].format, '%');
        bool fmt_is_float = false;
        if (pct) {
            const char *pp = pct + 1;
            while (*pp == '+' || *pp == '-' || *pp == '0' || *pp == ' ' || *pp == '#') pp++;
            while ((*pp >= '0' && *pp <= '9') || *pp == '.') pp++;
            if (*pp == 'f') fmt_is_float = true;
        }
        if (attr_maps[a].is_percent || !fmt_is_float)
            snprintf(part, sizeof(part), attr_maps[a].format, (int)roundf(val));
        else
            snprintf(part, sizeof(part), attr_maps[a].format, val);

        size_t cur = strlen(buf);
        size_t plen = strlen(part);
        if (cur + plen < sizeof(buf) - 1)
            memcpy(buf + cur, part, plen + 1);
        found++;
    }

    if (found == 0) return NULL;
    return strdup(buf);
}

static void add_stats_from_record(const char *record_path, TQTranslation *tr, BufWriter *w, const char *color, int shard_index) {
    if (!record_path || !record_path[0]) return;
    TQArzRecordData *data = asset_get_dbr(record_path);
    if (!data) return;

    /* Detect "X% Chance of:" conditional wrapper — all effects get indented */
    float global_chance = dbr_get_float_fast(data, INT_offensiveGlobalChance, shard_index);
    const char *indent = "";
    if (global_chance > 0) {
        buf_write(w, "<span color='%s'>%.0f%% Chance of:</span>\n", color, global_chance);
        indent = "    ";
    }

    /* Flat damage ranges (min-max) */
    {
        static struct { const char **min_int; const char **max_int; const char *label; } damage_types[] = {
            {&INT_offensivePhysicalMin, &INT_offensivePhysicalMax, "Physical Damage"},
            {&INT_offensiveFireMin, &INT_offensiveFireMax, "Fire Damage"},
            {&INT_offensiveColdMin, &INT_offensiveColdMax, "Cold Damage"},
            {&INT_offensiveLightningMin, &INT_offensiveLightningMax, "Lightning Damage"},
            {&INT_offensivePoisonMin, &INT_offensivePoisonMax, "Poison Damage"},
            {&INT_offensivePierceMin, &INT_offensivePierceMax, "Pierce Damage"},
            {&INT_offensiveElementalMin, &INT_offensiveElementalMax, "Elemental Damage"},
            {&INT_offensiveManaLeechMin, &INT_offensiveManaLeechMax, "Mana Leech"},
            {&INT_offensiveBasePhysicalMin, &INT_offensiveBasePhysicalMax, "Physical Damage"},
            {&INT_offensiveBaseColdMin, &INT_offensiveBaseColdMax, "Cold Damage"},
            {&INT_offensiveBaseFireMin, &INT_offensiveBaseFireMax, "Fire Damage"},
            {&INT_offensiveBaseLightningMin, &INT_offensiveBaseLightningMax, "Lightning Damage"},
            {&INT_offensiveBasePoisonMin, &INT_offensiveBasePoisonMax, "Poison Damage"},
            {&INT_offensiveBaseLifeMin, &INT_offensiveBaseLifeMax, "Vitality Damage"},
            {NULL, NULL, NULL}
        };
        for (int d = 0; damage_types[d].min_int; d++) {
            float mn = dbr_get_float_fast(data, *damage_types[d].min_int, shard_index);
            float mx = dbr_get_float_fast(data, *damage_types[d].max_int, shard_index);
            if (mn > 0) {
                if (mx > mn)
                    buf_write(w, "<span color='%s'>%d - %d %s</span>\n", color, (int)round(mn), (int)round(mx), damage_types[d].label);
                else
                    buf_write(w, "<span color='%s'>%d %s</span>\n", color, (int)round(mn), damage_types[d].label);
            }
        }
    }

    /* ADCTH: attack damage converted to health */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveLifeLeechMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveLifeLeechMax, shard_index);
        if (mn > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%d%% - %d%% Attack Damage Converted to Health</span>\n", color, (int)round(mn), (int)round(mx));
            else
                buf_write(w, "<span color='%s'>%d%% Attack Damage Converted to Health</span>\n", color, (int)round(mn));
        }
    }

    /* DoT: burn (fire) */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowFireMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowFireMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowFireDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Burn Damage over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Burn Damage over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* DoT: electrical burn (lightning) */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowLightningMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowLightningMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowLightningDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Electrical Burn Damage over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Electrical Burn Damage over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* DoT: frostburn (cold) */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowColdMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowColdMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowColdDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Frostburn Damage over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Frostburn Damage over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* DoT: poison */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowPoisonMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowPoisonMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowPoisonDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Poison Damage over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Poison Damage over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* DoT: life leech */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowLifeLeachMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowLifeLeachMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowLifeLeachDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Life Leech over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Life Leech over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* DoT: vitality decay */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowLifeMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowLifeMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowLifeDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Vitality Decay over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Vitality Decay over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* DoT: energy leech */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowManaLeachMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowManaLeachMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowManaLeachDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Energy Leech over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Energy Leech over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* DoT: bleeding */
    {
        float mn = dbr_get_float_fast(data, INT_offensiveSlowBleedingMin, shard_index);
        float mx = dbr_get_float_fast(data, INT_offensiveSlowBleedingMax, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowBleedingDurationMin, shard_index);
        if (mn > 0 && dur > 0) {
            if (mx > mn)
                buf_write(w, "<span color='%s'>%.0f - %.0f Bleeding Damage over %.1f Seconds</span>\n", color, mn * dur, mx * dur, dur);
            else
                buf_write(w, "<span color='%s'>%.0f Bleeding Damage over %.1f Seconds</span>\n", color, mn * dur, dur);
        }
    }

    /* Bleeding damage modifier with chance */
    {
        float mod = dbr_get_float_fast(data, INT_offensiveSlowBleedingModifier, shard_index);
        float chance = dbr_get_float_fast(data, INT_offensiveSlowBleedingModifierChance, shard_index);
        if (fabs(mod) > 0.001f && chance > 0)
            buf_write(w, "<span color='%s'>%.1f%% Chance of +%d%% Bleeding Damage</span>\n", color, chance, (int)round(mod));
        else if (fabs(mod) > 0.001f)
            buf_write(w, "<span color='%s'>+%d%% Bleeding Damage</span>\n", color, (int)round(mod));
    }

    /* Reduced Armor (value + duration) */
    {
        float val = dbr_get_float_fast(data, INT_offensiveSlowDefensiveReductionMin, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowDefensiveReductionDurationMin, shard_index);
        if (val > 0 && dur > 0)
            buf_write(w, "<span color='%s'>%.0f Reduced Armor for %.1f Second(s)</span>\n", color, val, dur);
        else if (val > 0)
            buf_write(w, "<span color='%s'>%.0f Reduced Armor</span>\n", color, val);
    }

    /* Skill disruption (chance + duration) */
    {
        float chance = dbr_get_float_fast(data, INT_defensiveDisruption, shard_index);
        float dur = dbr_get_float_fast(data, INT_defensiveDisruptionDuration, shard_index);
        if (chance > 0 && dur > 0)
            buf_write(w, "<span color='%s'>%.1f%% Chance of %.1f Second(s) of Skill Disruption</span>\n", color, chance, dur);
        else if (chance > 0)
            buf_write(w, "<span color='%s'>%.1f%% Skill Disruption</span>\n", color, chance);
    }

    /* Reduced Attack Speed (value + duration) */
    {
        float val = dbr_get_float_fast(data, INT_offensiveSlowAttackSpeedMin, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowAttackSpeedDurationMin, shard_index);
        if (val > 0 && dur > 0)
            buf_write(w, "<span color='%s'>%.0f%% Reduced Attack Speed for %.1f Second(s)</span>\n", color, val, dur);
        else if (val > 0)
            buf_write(w, "<span color='%s'>%.0f%% Reduced Attack Speed</span>\n", color, val);
    }

    /* Reduced Run Speed (value + duration) */
    {
        float val = dbr_get_float_fast(data, INT_offensiveSlowRunSpeedMin, shard_index);
        float dur = dbr_get_float_fast(data, INT_offensiveSlowRunSpeedDurationMin, shard_index);
        if (val > 0 && dur > 0)
            buf_write(w, "<span color='%s'>%.0f%% Reduced Run Speed for %.1f Second(s)</span>\n", color, val, dur);
        else if (val > 0)
            buf_write(w, "<span color='%s'>%.0f%% Reduced Run Speed</span>\n", color, val);
    }

    /* Racial bonus */
    {
        const char *race = "Enemies";
        TQVariable *rv = arz_record_get_var(data, INT_racialBonusRace);
        if (rv && rv->type == TQ_VAR_STRING && rv->count > 0 && rv->value.str[0])
            race = rv->value.str[0];

        float dmg = dbr_get_float_fast(data, INT_racialBonusPercentDamage, shard_index);
        if (fabs(dmg) > 0.001f)
            buf_write(w, "<span color='%s'>+%d%% Damage to %ss</span>\n", color, (int)round(dmg), race);

        float def = dbr_get_float_fast(data, INT_racialBonusPercentDefense, shard_index);
        if (fabs(def) > 0.001f)
            buf_write(w, "<span color='%s'>%d%% less damage from %ss</span>\n", color, (int)round(def), race);
    }

    /* Mastery augmentation: "+N to all skills in X Mastery" */
    for (uint32_t i = 0; i < data->num_vars; i++) {
        if (!data->vars[i].name) continue;
        if (strncasecmp(data->vars[i].name, "augmentMasteryLevel", 19) == 0) {
            TQVariable *v = &data->vars[i];
            int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;
            if (idx < 0) idx = 0;
            float val = (v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx];
            if (fabs(val) < 0.001f) continue;

            char mastery_var[64];
            snprintf(mastery_var, sizeof(mastery_var), "augmentMasteryName%s", data->vars[i].name + 19);
            const char *mastery_var_int = arz_intern(mastery_var);
            TQVariable *mv = arz_record_get_var(data, mastery_var_int);
            const char *mastery_path = (mv && mv->type == TQ_VAR_STRING && mv->count > 0) ? mv->value.str[0] : NULL;

            const char *mastery_name = "Unknown Mastery";
            if (mastery_path) {
                const char *name_tag = get_record_variable_string(mastery_path, INT_skillDisplayName);
                if (name_tag) {
                    const char *translated = translation_get(tr, name_tag);
                    if (translated) mastery_name = translated;
                }
            }
            buf_write(w, "<span color='%s'>+%d to all skills in %s</span>\n", color, (int)round(val), mastery_name);
        }
    }

    /* Skill augmentation: "+N to [Skill Name]" */
    for (uint32_t i = 0; i < data->num_vars; i++) {
        if (!data->vars[i].name) continue;
        if (strncasecmp(data->vars[i].name, "augmentSkillLevel", 17) == 0) {
            TQVariable *v = &data->vars[i];
            int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;
            if (idx < 0) idx = 0;
            float val = (v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx];
            if (fabs(val) < 0.001f) continue;

            char skill_var[64];
            snprintf(skill_var, sizeof(skill_var), "augmentSkillName%s", data->vars[i].name + 17);
            const char *skill_var_int = arz_intern(skill_var);
            TQVariable *sv = arz_record_get_var(data, skill_var_int);
            const char *skill_path = (sv && sv->type == TQ_VAR_STRING && sv->count > 0) ? sv->value.str[0] : NULL;

            const char *skill_name = "Unknown Skill";
            if (skill_path) {
                /* Follow petSkillName if present (e.g. PetModifier skills) */
                const char *pet_path = get_record_variable_string(skill_path, INT_petSkillName);
                const char *base_path = (pet_path && pet_path[0]) ? pet_path : skill_path;
                const char *buff_path = get_record_variable_string(base_path, INT_buffSkillName);
                const char *lookup_path = (buff_path && buff_path[0]) ? buff_path : base_path;
                const char *name_tag = get_record_variable_string(lookup_path, INT_skillDisplayName);
                if (name_tag) {
                    const char *translated = translation_get(tr, name_tag);
                    if (translated) skill_name = translated;
                }
            }
            buf_write(w, "<span color='%s'>+%d to %s</span>\n", color, (int)round(val), skill_name);
        }
    }

    /* Standard attr_maps iteration — O(num_vars) with O(1) per variable */
    for (uint32_t i = 0; i < data->num_vars; i++) {
        TQVariable *v = &data->vars[i];
        if (!v->name) continue;

        const char *interned = arz_intern(v->name);

        /* skip already-handled vars */
        if (strncasecmp(v->name, "augmentMastery", 14) == 0) continue;
        if (strncasecmp(v->name, "augmentSkill", 12) == 0) continue;
        if (g_hash_table_contains(g_skip_set, interned)) continue;

        /* O(1) lookup instead of linear scan */
        AttributeMap *am = g_hash_table_lookup(g_attr_map_ht, interned);
        if (!am) continue;

        int val_idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;
        if (val_idx < 0) val_idx = 0;
        float val = (v->type == TQ_VAR_INT) ? (float)v->value.i32[val_idx] : v->value.f32[val_idx];
        if (fabs(val) < 0.001f) continue;

        char line[256];
        if (am->is_percent || strstr(am->format, "%d") || strstr(am->format, "%+d")) {
            snprintf(line, sizeof(line), am->format, (int)round(val));
        } else {
            snprintf(line, sizeof(line), am->format, val);
        }
        buf_write(w, "<span color='%s'>%s</span>\n", color, line);
    }

    /* Offensive total damage modifier (may have chance) */
    {
        float tdm = dbr_get_float_fast(data, INT_offensiveTotalDamageModifier, shard_index);
        if (fabs(tdm) > 0.001f) {
            float tdm_chance = dbr_get_float_fast(data, INT_offensiveTotalDamageModifierChance, shard_index);
            if (tdm_chance > 0 && tdm_chance < 100)
                buf_write(w, "<span color='%s'>%.0f%% Chance of +%d%% Total Damage</span>\n", color, tdm_chance, (int)round(tdm));
            else
                buf_write(w, "<span color='%s'>+%d%% Total Damage</span>\n", color, (int)round(tdm));
        }
    }

    /* Percent current life reduction (may have chance) */
    {
        float pcl = dbr_get_float_fast(data, INT_offensivePercentCurrentLifeMin, shard_index);
        if (fabs(pcl) > 0.001f) {
            float pcl_chance = dbr_get_float_fast(data, INT_offensivePercentCurrentLifeChance, shard_index);
            if (pcl_chance > 0 && pcl_chance < 100)
                buf_write(w, "<span color='%s'>%.1f%% Chance of %.0f%% Reduction to Enemy's Health</span>\n", color, pcl_chance, pcl);
            else
                buf_write(w, "<span color='%s'>%.0f%% Reduction to Enemy's Health</span>\n", color, pcl);
        }
    }

    /* Skill parameters: cooldown/recharge time */
    {
        float cooldown = dbr_get_float_fast(data, INT_skillCooldownTime, shard_index);
        if (cooldown <= 0) cooldown = dbr_get_float_fast(data, INT_refreshTime, shard_index);
        if (cooldown > 0)
            buf_write(w, "<span color='%s'>%.1f Second(s) Recharge</span>\n", color, cooldown);
    }

    /* Skill parameters: target number */
    {
        float targets = dbr_get_float_fast(data, INT_skillTargetNumber, shard_index);
        if (targets > 0)
            buf_write(w, "<span color='%s'>Affects up to %d targets</span>\n", color, (int)targets);
    }

    /* Skill parameters: active duration */
    {
        float duration = dbr_get_float_fast(data, INT_skillActiveDuration, shard_index);
        if (duration > 0)
            buf_write(w, "<span color='%s'>%.1f Second Duration</span>\n", color, duration);
    }

    /* Skill parameters: target radius */
    {
        float radius = dbr_get_float_fast(data, INT_skillTargetRadius, shard_index);
        if (radius > 0)
            buf_write(w, "<span color='%s'>%.1f Meter Radius</span>\n", color, radius);
    }

    /* Offensive stun */
    {
        float stun_min = dbr_get_float_fast(data, INT_offensiveStunMin, shard_index);
        float stun_dur = dbr_get_float_fast(data, INT_offensiveStunDurationMin, shard_index);
        if (stun_dur <= 0) stun_dur = stun_min;
        float stun_chance = dbr_get_float_fast(data, INT_offensiveStunChance, shard_index);
        if (stun_dur > 0) {
            if (stun_chance > 0)
                buf_write(w, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Stun</span>\n", color, indent, stun_chance, stun_dur);
            else
                buf_write(w, "<span color='%s'>%s%.1f Second(s) of Stun</span>\n", color, indent, stun_dur);
        }
    }

    /* Offensive fumble (impaired aim) */
    {
        float fumble_min = dbr_get_float_fast(data, INT_offensiveFumbleMin, shard_index);
        float fumble_dur = dbr_get_float_fast(data, INT_offensiveFumbleDurationMin, shard_index);
        if (fumble_min > 0 && fumble_dur > 0) {
            float fumble_chance = dbr_get_float_fast(data, INT_offensiveFumbleChance, shard_index);
            if (fumble_chance > 0)
                buf_write(w, "<span color='%s'>%s%.0f%% Chance of Impaired Aim over %.1f Seconds</span>\n", color, indent, fumble_chance, fumble_dur);
            else
                buf_write(w, "<span color='%s'>%sImpaired Aim over %.1f Seconds</span>\n", color, indent, fumble_dur);
        }
    }

    /* Offensive freeze */
    {
        float freeze_min = dbr_get_float_fast(data, INT_offensiveFreezeMin, shard_index);
        float freeze_dur = dbr_get_float_fast(data, INT_offensiveFreezeDurationMin, shard_index);
        if (freeze_min > 0 && freeze_dur > 0) {
            float freeze_chance = dbr_get_float_fast(data, INT_offensiveFreezeChance, shard_index);
            if (freeze_chance > 0)
                buf_write(w, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Freeze</span>\n", color, indent, freeze_chance, freeze_dur);
            else
                buf_write(w, "<span color='%s'>%s%.1f Second(s) of Freeze</span>\n", color, indent, freeze_dur);
        }
    }

    /* Offensive petrify */
    {
        float petrify_min = dbr_get_float_fast(data, INT_offensivePetrifyMin, shard_index);
        float petrify_dur = dbr_get_float_fast(data, INT_offensivePetrifyDurationMin, shard_index);
        if (petrify_min > 0 && petrify_dur > 0) {
            float petrify_chance = dbr_get_float_fast(data, INT_offensivePetrifyChance, shard_index);
            if (petrify_chance > 0)
                buf_write(w, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Petrify</span>\n", color, indent, petrify_chance, petrify_dur);
            else
                buf_write(w, "<span color='%s'>%s%.1f Second(s) of Petrify</span>\n", color, indent, petrify_dur);
        }
    }

    /* Mind control (offensiveConvertMin — duration-based conversion of enemies) */
    {
        float convert_min = dbr_get_float_fast(data, INT_offensiveConvertMin, shard_index);
        if (convert_min > 0)
            buf_write(w, "<span color='%s'>%s%.1f Seconds of Mind Control</span>\n", color, indent, convert_min);
    }

    /* Offensive confusion */
    {
        float confuse_min = dbr_get_float_fast(data, INT_offensiveConfusionMin, shard_index);
        float confuse_dur = dbr_get_float_fast(data, INT_offensiveConfusionDurationMin, shard_index);
        if (confuse_min > 0) {
            float confuse_chance = dbr_get_float_fast(data, INT_offensiveConfusionChance, shard_index);
            if (confuse_dur > 0) {
                if (confuse_chance > 0)
                    buf_write(w, "<span color='%s'>%s%.0f%% Chance of %.1f Second(s) of Confusion</span>\n", color, indent, confuse_chance, confuse_dur);
                else
                    buf_write(w, "<span color='%s'>%s%.1f Second(s) of Confusion</span>\n", color, indent, confuse_dur);
            } else {
                buf_write(w, "<span color='%s'>%s%.1f Second(s) of Confusion</span>\n", color, indent, confuse_min);
            }
        }
    }

    /* Fear */
    {
        float fear_min = dbr_get_float_fast(data, INT_offensiveFearMin, shard_index);
        float fear_max = dbr_get_float_fast(data, INT_offensiveFearMax, shard_index);
        if (fear_min > 0) {
            if (fear_max > fear_min)
                buf_write(w, "<span color='%s'>%s%.1f - %.1f Second(s) of Fear</span>\n", color, indent, fear_min, fear_max);
            else
                buf_write(w, "<span color='%s'>%s%.1f Second(s) of Fear</span>\n", color, indent, fear_min);
        }
    }

    /* Total damage reduction (debuff applied to enemies) */
    {
        float tdmg_min = dbr_get_float_fast(data, INT_offensiveTotalDamageReductionPercentMin, shard_index);
        float tdmg_dur = dbr_get_float_fast(data, INT_offensiveTotalDamageReductionPercentDurationMin, shard_index);
        if (tdmg_min > 0) {
            if (tdmg_dur > 0)
                buf_write(w, "<span color='%s'>%s%.0f%% Reduced Damage for %.1f Second(s)</span>\n", color, indent, tdmg_min, tdmg_dur);
            else
                buf_write(w, "<span color='%s'>%s%.0f%% Reduced Damage</span>\n", color, indent, tdmg_min);
        }
    }

    /* Follow petBonusName reference (LootRandomizer pet bonus sub-records) */
    {
        const char *pet_bonus = record_get_string_fast(data, INT_petBonusName);
        if (pet_bonus && pet_bonus[0]) {
            buf_write(w, "<span color='%s'>Bonus to All Pets:</span>\n", color);
            add_stats_from_record(pet_bonus, tr, w, color, shard_index);
        }
    }
}

/* ── relic/charm header helpers ──────────────────────────────────── */

static bool path_contains_ci(const char *path, const char *needle) {
    if (!path || !needle) return false;
    size_t plen = strlen(path), nlen = strlen(needle);
    for (size_t i = 0; i + nlen <= plen; i++) {
        if (strncasecmp(path + i, needle, nlen) == 0) return true;
    }
    return false;
}

static const char* relic_type_label(const char *relic_path) {
    if (path_contains_ci(relic_path, "charm"))       return "Charm";
    if (path_contains_ci(relic_path, "animalrelic"))  return "Charm";
    return "Relic";
}

int relic_max_shards(const char *relic_path) {
    TQArzRecordData *data = asset_get_dbr(relic_path);
    if (data) {
        TQVariable *v = arz_record_get_var(data, INT_completedRelicLevel);
        if (v && v->type == TQ_VAR_INT && v->count > 0 && v->value.i32[0] > 0)
            return v->value.i32[0];
    }
    if (path_contains_ci(relic_path, "charm") || path_contains_ci(relic_path, "animalrelic")) return 5;
    return 3;
}

static void add_relic_section(const char *relic_name, const char *relic_bonus,
                              uint32_t shard_count, TQTranslation *tr,
                              BufWriter *w) {
    if (!relic_name || !relic_name[0]) return;

    const char *relic_tag = get_record_variable_string(relic_name, INT_description);
    const char *relic_str = relic_tag ? translation_get(tr, relic_tag) : NULL;
    char *pretty_fallback = NULL;
    if (!relic_str || !relic_str[0]) {
        pretty_fallback = pretty_name_from_path(relic_name);
        relic_str = pretty_fallback;
    }
    char *e_relic = escape_markup(relic_str);
    const char *type_label = relic_type_label(relic_name);
    int max_shards = relic_max_shards(relic_name);
    bool completed = relic_bonus || (shard_count >= (uint32_t)max_shards);

    buf_write(w, "\n<b><span color='#C1A472'>%s</span></b>\n", e_relic);
    if (completed)
        buf_write(w, "<span color='#C1A472'>Completed %s</span>\n", type_label);
    else if (shard_count > 0)
        buf_write(w, "<span color='#C1A472'>%s (+%u)</span>\n", type_label, shard_count);
    else
        buf_write(w, "<span color='#C1A472'>%s</span>\n", type_label);
    free(e_relic);
    free(pretty_fallback);

    int stat_idx = completed ? max_shards - 1 : (shard_count > 0 ? (int)shard_count - 1 : 0);
    add_stats_from_record(relic_name, tr, w, "#C1A472", stat_idx);

    if (relic_bonus) {
        buf_write(w, "\n<span color='#C1A472'>Completed %s Bonus</span>\n", type_label);
        add_stats_from_record(relic_bonus, tr, w, "#C1A472", 0);
    }
}

/* ── requirements ────────────────────────────────────────────────── */

/* Simple recursive-descent expression evaluator for itemCost equations.
 * Supports: +, -, *, /, ^ (power), parentheses, decimal numbers, and
 * variable substitution for "itemLevel" and "totalAttCount". */

typedef struct {
    const char *p;
    double item_level;
    double total_att_count;
} ExprCtx;

static double expr_parse_expr(ExprCtx *c);

static void expr_skip_ws(ExprCtx *c) {
    while (*c->p == ' ' || *c->p == '\t') c->p++;
}

static double expr_parse_atom(ExprCtx *c) {
    expr_skip_ws(c);
    if (*c->p == '(') {
        c->p++;
        double v = expr_parse_expr(c);
        expr_skip_ws(c);
        if (*c->p == ')') c->p++;
        return v;
    }
    /* variable or number */
    if ((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z')) {
        const char *start = c->p;
        while ((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z') ||
               (*c->p >= '0' && *c->p <= '9') || *c->p == '_')
            c->p++;
        size_t len = (size_t)(c->p - start);
        if (len == 9 && strncmp(start, "itemLevel", 9) == 0) return c->item_level;
        if (len == 13 && strncmp(start, "totalAttCount", 13) == 0) return c->total_att_count;
        return 0.0;
    }
    /* number (possibly negative handled by caller via unary minus) */
    char *end;
    double v = strtod(c->p, &end);
    if (end == c->p) return 0.0;
    c->p = end;
    return v;
}

static double expr_parse_unary(ExprCtx *c) {
    expr_skip_ws(c);
    if (*c->p == '-') { c->p++; return -expr_parse_unary(c); }
    if (*c->p == '+') { c->p++; return expr_parse_unary(c); }
    return expr_parse_atom(c);
}

static double expr_parse_power(ExprCtx *c) {
    double v = expr_parse_unary(c);
    expr_skip_ws(c);
    if (*c->p == '^') { c->p++; v = pow(v, expr_parse_power(c)); }
    return v;
}

static double expr_parse_muldiv(ExprCtx *c) {
    double v = expr_parse_power(c);
    for (;;) {
        expr_skip_ws(c);
        if (*c->p == '*') { c->p++; v *= expr_parse_power(c); }
        else if (*c->p == '/') { c->p++; double d = expr_parse_power(c); if (d != 0) v /= d; }
        else break;
    }
    return v;
}

static double expr_parse_expr(ExprCtx *c) {
    double v = expr_parse_muldiv(c);
    for (;;) {
        expr_skip_ws(c);
        if (*c->p == '+') { c->p++; v += expr_parse_muldiv(c); }
        else if (*c->p == '-') { c->p++; v -= expr_parse_muldiv(c); }
        else break;
    }
    return v;
}

static double eval_equation(const char *eq, double item_level, double total_att_count) {
    ExprCtx c = { .p = eq, .item_level = item_level, .total_att_count = total_att_count };
    return expr_parse_expr(&c);
}

/* Map item Class to equation prefix used in itemCost records */
static const char *class_to_equation_prefix(const char *item_class) {
    if (!item_class) return NULL;
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
    for (int i = 0; map[i].cls; i++)
        if (strcasecmp(item_class, map[i].cls) == 0) return map[i].prefix;
    return NULL;
}

static void add_requirements(const char *record_path, BufWriter *w) {
    if (!record_path || !record_path[0]) return;
    TQArzRecordData *data = asset_get_dbr(record_path);
    if (!data) return;

    buf_write(w, "\n");

    static struct { const char **interned; const char *label; const char *eq_suffix; } req_types[] = {
        {&INT_levelRequirement,        "Required Player Level", "LevelEquation"},
        {&INT_dexterityRequirement,    "Required Dexterity",    "DexterityEquation"},
        {&INT_intelligenceRequirement, "Required Intelligence", "IntelligenceEquation"},
        {&INT_strengthRequirement,     "Required Strength",     "StrengthEquation"},
        {NULL, NULL, NULL}
    };

    /* Read static requirement values */
    int vals[4] = {0};
    for (int r = 0; req_types[r].interned; r++) {
        TQVariable *v = arz_record_get_var(data, *req_types[r].interned);
        if (!v || v->count == 0) continue;
        vals[r] = (v->type == TQ_VAR_FLOAT) ? (int)v->value.f32[0] : v->value.i32[0];
    }

    /* For any requirement type still zero, try dynamic computation via equations */
    int needs_dynamic = 0;
    for (int r = 0; req_types[r].interned; r++)
        if (vals[r] <= 0) { needs_dynamic = 1; break; }

    if (needs_dynamic) {
        const char *item_class = record_get_string_fast(data, INT_Class);
        const char *eq_prefix = class_to_equation_prefix(item_class);
        if (eq_prefix) {
            TQVariable *lvl_var = arz_record_get_var(data, INT_itemLevel);
            double item_level = 0;
            if (lvl_var && lvl_var->count > 0)
                item_level = (lvl_var->type == TQ_VAR_FLOAT) ? lvl_var->value.f32[0] : (double)lvl_var->value.i32[0];

            if (item_level > 0) {
                const char *cost_path = record_get_string_fast(data, INT_itemCostName);
                TQArzRecordData *cost_data = NULL;
                if (cost_path && cost_path[0])
                    cost_data = asset_get_dbr(cost_path);
                if (!cost_data)
                    cost_data = asset_get_dbr("records\\game\\itemcost.dbr");

                if (cost_data) {
                    for (int r = 0; req_types[r].interned; r++) {
                        if (vals[r] > 0) continue;
                        char eq_name[128];
                        snprintf(eq_name, sizeof(eq_name), "%s%s", eq_prefix, req_types[r].eq_suffix);
                        const char *equation = record_get_string_fast(cost_data, arz_intern(eq_name));
                        if (!equation || !equation[0]) continue;
                        int val = (int)ceil(eval_equation(equation, item_level, 0.0));
                        if (val > 0) vals[r] = val;
                    }
                }
            }
        }
    }

    for (int r = 0; req_types[r].interned; r++)
        if (vals[r] > 0)
            buf_write(w, "%s: %d\n", req_types[r].label, vals[r]);
}

/* ── main tooltip formatter ──────────────────────────────────────── */

static void format_stats_common(uint32_t seed, const char *base_name, const char *prefix_name,
    const char *suffix_name, const char *relic_name, const char *relic_bonus,
    uint32_t var1, const char *relic_name2, const char *relic_bonus2,
    uint32_t var2, TQTranslation *tr, char *buffer, size_t size)
{
    BufWriter w;
    buf_init(&w, buffer, size);

    /* Pre-fetch record data pointers */
    TQArzRecordData *base_data = base_name ? asset_get_dbr(base_name) : NULL;
    TQArzRecordData *prefix_data = (prefix_name && prefix_name[0]) ? asset_get_dbr(prefix_name) : NULL;
    TQArzRecordData *suffix_data = (suffix_name && suffix_name[0]) ? asset_get_dbr(suffix_name) : NULL;

    /* Item title */
    const char *base_tag = base_data ? record_get_string_fast(base_data, INT_itemNameTag) : NULL;
    const char *prefix_tag = prefix_data ? record_get_string_fast(prefix_data, INT_description) : NULL;
    if (!prefix_tag && prefix_data) prefix_tag = record_get_string_fast(prefix_data, INT_lootRandomizerName);
    if (!prefix_tag && prefix_data) prefix_tag = record_get_string_fast(prefix_data, INT_FileDescription);
    const char *suffix_tag = suffix_data ? record_get_string_fast(suffix_data, INT_description) : NULL;
    if (!suffix_tag && suffix_data) suffix_tag = record_get_string_fast(suffix_data, INT_lootRandomizerName);
    if (!suffix_tag && suffix_data) suffix_tag = record_get_string_fast(suffix_data, INT_FileDescription);

    /* Fallback: try "description" tag if "itemNameTag" is missing (XPack4 relics/charms) */
    if (!base_tag && base_data) base_tag = record_get_string_fast(base_data, INT_description);

    /* Strip "records\" prefix from path for display */
    const char *display_path = base_name;
    if (display_path && strncasecmp(display_path, "records\\", 8) == 0)
        display_path += 8;

    const char *base_str = base_tag ? translation_get(tr, base_tag) : NULL;
    char *base_pretty = NULL;
    if (!base_str || !base_str[0]) {
        base_pretty = pretty_name_from_path(base_name);
        base_str = base_pretty;
    }
    const char *prefix_str = prefix_tag ? translation_get(tr, prefix_tag) : "";
    if (!prefix_str || !prefix_str[0]) prefix_str = prefix_tag ? prefix_tag : "";
    const char *suffix_str = suffix_tag ? translation_get(tr, suffix_tag) : "";
    if (!suffix_str || !suffix_str[0]) suffix_str = suffix_tag ? suffix_tag : "";
    if (!base_str) base_str = display_path;

    const char *item_color = get_item_color(base_name, prefix_name, suffix_name);
    char *e_base = escape_markup(base_str);
    char *e_prefix = escape_markup(prefix_str);
    char *e_suffix = escape_markup(suffix_str);

    buf_write(&w, "<b><span color='%s'>", item_color);
    if (e_prefix[0]) buf_write(&w, "%s  ", e_prefix);
    buf_write(&w, "%s", e_base);
    if (e_suffix[0]) buf_write(&w, " %s", e_suffix);
    buf_write(&w, "</span></b>\n");

    /* Weapon/equipment type */
    {
        const char *item_text_tag = base_data ? record_get_string_fast(base_data, INT_itemText) : NULL;
        if (item_text_tag) {
            const char *text_str = translation_get(tr, item_text_tag);
            if (text_str) {
                char *e_text = escape_markup(text_str);
                buf_write(&w, "<span color='white'>%s</span>\n", e_text);
                free(e_text);
            }
        }
    }

    free(e_base); free(e_prefix); free(e_suffix); free(base_pretty);

    /* Artifact classification subtitle */
    {
        const char *art_class = base_data ? record_get_string_fast(base_data, INT_artifactClassification) : NULL;
        if (art_class && art_class[0]) {
            const char *class_tag = NULL;
            if (strcasecmp(art_class, "LESSER") == 0) class_tag = "xtagArtifactClass01";
            else if (strcasecmp(art_class, "GREATER") == 0) class_tag = "xtagArtifactClass02";
            else if (strcasecmp(art_class, "DIVINE") == 0) class_tag = "xtagArtifactClass03";
            if (class_tag) {
                const char *class_str = translation_get(tr, class_tag);
                if (class_str) {
                    char *e_class = escape_markup(class_str);
                    buf_write(&w, "<span color='white'>%s</span>\n", e_class);
                    free(e_class);
                }
            }
        }
    }

    /* Prefix properties */
    if (prefix_name && prefix_name[0]) {
        size_t pos_before = w.pos;
        const char *pname = prefix_tag ? translation_get(tr, prefix_tag) : NULL;
        char *ep = escape_markup(pname ? pname : "");
        if (ep[0])
            buf_write(&w, "\n<span color='white'><b>Prefix Properties : %s</b></span>\n", ep);
        else
            buf_write(&w, "\n<span color='white'><b>Prefix Properties</b></span>\n");
        free(ep);
        size_t pos_after_header = w.pos;
        add_stats_from_record(prefix_name, tr, &w, "#00A3FF", 0);
        if (w.pos == pos_after_header) {
            w.pos = pos_before;
            w.buf[w.pos] = '\0';
        }
    }

    /* Detect if this is a standalone relic/charm */
    bool standalone_relic_charm = false;
    if (base_name) {
        if (path_contains_ci(base_name, "animalrelic")
            || path_contains_ci(base_name, "\\relics\\")
            || path_contains_ci(base_name, "\\charms\\"))
            standalone_relic_charm = true;
        else if (base_data) {
            /* Fallback: check Class for items in non-standard dirs (e.g. HCDUNGEON) */
            const char *cls = record_get_string_fast(base_data, INT_Class);
            if (cls && (strcasecmp(cls, "ItemRelic") == 0 ||
                        strcasecmp(cls, "ItemCharm") == 0))
                standalone_relic_charm = true;
        }
    }
    bool is_artifact = (base_name && path_contains_ci(base_name, "\\artifacts\\") && !path_contains_ci(base_name, "\\arcaneformulae\\"));
    int base_shard_index = 0;
    bool standalone_complete = false;
    int standalone_max_shards = 0;
    if (standalone_relic_charm) {
        standalone_max_shards = relic_max_shards(base_name);
        standalone_complete = (relic_bonus && relic_bonus[0]) || (var1 >= (uint32_t)standalone_max_shards);
        base_shard_index = standalone_complete ? standalone_max_shards - 1
            : (var1 > 0 ? (int)var1 - 1 : 0);
    }

    /* Base item properties */
    {
        const char *bname = base_tag ? translation_get(tr, base_tag) : NULL;
        char *eb = escape_markup(bname ? bname : "");
        if (standalone_relic_charm) {
            const char *type_label = relic_type_label(base_name);
            if (standalone_complete)
                buf_write(&w, "\n<span color='#C1A472'>Completed %s</span>\n", type_label);
            else if (var1 > 0)
                buf_write(&w, "\n<span color='#C1A472'>%s (+%u)</span>\n", type_label, var1);
            else
                buf_write(&w, "\n<span color='#C1A472'>%s</span>\n", type_label);
        } else {
            if (eb[0])
                buf_write(&w, "\n<span color='#FFA500'><b>Base Item Properties : %s</b></span>\n", eb);
            else
                buf_write(&w, "\n<span color='#FFA500'><b>Base Item Properties</b></span>\n");
        }
        free(eb);
        /* Attack speed tag — only meaningful for weapons and shields */
        if (!standalone_relic_charm && !is_artifact && base_data) {
            const char *item_class = record_get_string_fast(base_data, INT_Class);
            if (item_class && (strncasecmp(item_class, "WeaponMelee_", 12) == 0 ||
                               strncasecmp(item_class, "WeaponHunting_", 14) == 0 ||
                               strncasecmp(item_class, "WeaponMagical_", 14) == 0 ||
                               strcasecmp(item_class, "WeaponArmor_Shield") == 0)) {
                const char *speed_tag = record_get_string_fast(base_data, INT_characterBaseAttackSpeedTag);
                if (speed_tag) {
                    const char *speed_str = translation_get(tr, speed_tag);
                    if (speed_str)
                        buf_write(&w, "<span color='#00FFFF'>%s</span>\n", speed_str);
                }
            }
        }
        add_stats_from_record(base_name, tr, &w,
            standalone_relic_charm ? "#C1A472" : "#00FFFF", base_shard_index);
    }

    /* Standalone relic/charm completion bonus */
    if (standalone_relic_charm && standalone_complete && relic_bonus && relic_bonus[0]) {
        const char *type_label = relic_type_label(base_name);
        buf_write(&w, "\n<span color='#C1A472'>Completed %s Bonus</span>\n", type_label);
        add_stats_from_record(relic_bonus, tr, &w, "#C1A472", 0);
    }

    /* Suffix properties */
    if (suffix_name && suffix_name[0]) {
        size_t pos_before = w.pos;
        const char *sname = suffix_tag ? translation_get(tr, suffix_tag) : NULL;
        char *es = escape_markup(sname ? sname : "");
        if (es[0])
            buf_write(&w, "\n<span color='white'><b>Suffix Properties : %s</b></span>\n", es);
        else
            buf_write(&w, "\n<span color='white'><b>Suffix Properties</b></span>\n");
        free(es);
        size_t pos_after_header = w.pos;
        add_stats_from_record(suffix_name, tr, &w, "#00A3FF", 0);
        if (w.pos == pos_after_header) {
            w.pos = pos_before;
            w.buf[w.pos] = '\0';
        }
    }

    /* Granted skill */
    {
        const char *skill_dbr = base_data ? record_get_string_fast(base_data, INT_itemSkillName) : NULL;
        if (skill_dbr && skill_dbr[0]) {
            TQArzRecordData *skill_data = asset_get_dbr(skill_dbr);
            const char *buff_path = skill_data ? record_get_string_fast(skill_data, INT_buffSkillName) : NULL;
            const char *effect_dbr = (buff_path && buff_path[0]) ? buff_path : skill_dbr;
            TQArzRecordData *effect_data = asset_get_dbr(effect_dbr);

            const char *skill_tag = effect_data ? record_get_string_fast(effect_data, INT_skillDisplayName) : NULL;
            if (!skill_tag && skill_data) skill_tag = record_get_string_fast(skill_data, INT_skillDisplayName);
            const char *skill_name = skill_tag ? translation_get(tr, skill_tag) : NULL;

            const char *trigger_text = "";
            const char *controller_dbr = base_data ? record_get_string_fast(base_data, INT_itemSkillAutoController) : NULL;
            if (controller_dbr && controller_dbr[0]) {
                TQArzRecordData *ctrl_data = asset_get_dbr(controller_dbr);
                const char *trigger_type = ctrl_data ? record_get_string_fast(ctrl_data, INT_triggerType) : NULL;
                if (trigger_type) {
                    if (strcasecmp(trigger_type, "onAttack") == 0)
                        trigger_text = " (Activated on attack)";
                    else if (strcasecmp(trigger_type, "onHit") == 0)
                        trigger_text = " (Activated on hit)";
                    else if (strcasecmp(trigger_type, "onBeingHit") == 0)
                        trigger_text = " (Activated when hit)";
                    else if (strcasecmp(trigger_type, "onEquip") == 0)
                        trigger_text = " (Activated on equip)";
                    else if (strcasecmp(trigger_type, "onLowHealth") == 0)
                        trigger_text = " (Activated on low health)";
                    else if (strcasecmp(trigger_type, "onKill") == 0)
                        trigger_text = " (Activated on kill)";
                }
            }

            TQVariable *skill_level_var = base_data ? arz_record_get_var(base_data, INT_itemSkillLevel) : NULL;
            int skill_level = 1;
            if (skill_level_var) {
                if (skill_level_var->type == TQ_VAR_INT)
                    skill_level = skill_level_var->value.i32[0];
                else if (skill_level_var->type == TQ_VAR_FLOAT)
                    skill_level = (int)skill_level_var->value.f32[0];
            }
            int skill_index = (skill_level > 1) ? skill_level - 1 : 0;

            buf_write(&w, "\n<span color='white'><b>Grants Skill :</b></span>\n");
            if (skill_name) {
                char *e_name = escape_markup(skill_name);
                buf_write(&w, "<span color='white'>%s%s</span>\n", e_name, trigger_text);
                free(e_name);
            }

            const char *desc_tag = effect_data ? record_get_string_fast(effect_data, INT_skillBaseDescription) : NULL;
            if (!desc_tag && skill_data) desc_tag = record_get_string_fast(skill_data, INT_skillBaseDescription);
            if (desc_tag) {
                const char *desc_text = translation_get(tr, desc_tag);
                if (desc_text) {
                    char *e_desc = escape_markup(desc_text);
                    buf_write(&w, "<span color='white'>%s</span>\n", e_desc);
                    free(e_desc);
                }
            }

            add_stats_from_record(effect_dbr, tr, &w, "#DAA520", skill_index);

            if (buff_path && buff_path[0])
                add_stats_from_record(skill_dbr, tr, &w, "#DAA520", skill_index);

            /* Pet/secondary skill */
            const char *pet_skill_path = effect_data ? record_get_string_fast(effect_data, INT_petSkillName) : NULL;
            if ((!pet_skill_path || !pet_skill_path[0]) && skill_data)
                pet_skill_path = record_get_string_fast(skill_data, INT_petSkillName);
            if (pet_skill_path && pet_skill_path[0]) {
                TQArzRecordData *pet_data = asset_get_dbr(pet_skill_path);
                if (pet_data) {
                    float chance = 0;
                    TQVariable *cv = arz_record_get_var(pet_data, INT_skillChanceWeight);
                    if (cv) {
                        int ci = (skill_index < (int)cv->count) ? skill_index : (int)cv->count - 1;
                        if (ci < 0) ci = 0;
                        chance = (cv->type == TQ_VAR_INT) ? (float)cv->value.i32[ci] : cv->value.f32[ci];
                    }
                    if (chance > 0)
                        buf_write(&w, "<span color='#DAA520'>%.0f%% Chance of:</span>\n", chance);

                    const char *pet_buff = record_get_string_fast(pet_data, INT_buffSkillName);
                    const char *pet_effect = (pet_buff && pet_buff[0]) ? pet_buff : pet_skill_path;
                    add_stats_from_record(pet_effect, tr, &w, "#DAA520", skill_index);
                }
            }

        }
    }

    /* Artifact completion bonus */
    if (is_artifact && relic_bonus && relic_bonus[0]) {
        buf_write(&w, "\n\n<span color='#40FF40'><b>Completion Bonus :</b></span>\n");
        add_stats_from_record(relic_bonus, tr, &w, "#40FF40", 0);
    }

    /* Relic/Charm slot 1 */
    if (!is_artifact)
        add_relic_section(relic_name, relic_bonus, var1, tr, &w);

    /* Relic/Charm slot 2 */
    add_relic_section(relic_name2, relic_bonus2, var2, tr, &w);

    /* Item seed with hex and percentage */
    float seed_pct = ((float)seed / 65536.0f) * 100.0f;
    buf_write(&w, "\nitemSeed: %u (0x%08X) (%.3f %%)\n", seed, seed, seed_pct);

    /* Expansion indicator based on item path */
    {
        const char *expansion_label = NULL;
        if (base_name) {
            if (strncasecmp(base_name, "records\\xpack4\\", 15) == 0)
                expansion_label = "Eternal Embers Item";
            else if (strncasecmp(base_name, "records\\xpack3\\", 15) == 0)
                expansion_label = "Atlantis Item";
            else if (strncasecmp(base_name, "records\\xpack2\\", 15) == 0)
                expansion_label = "Ragnarok Item";
            else if (strncasecmp(base_name, "records\\xpack\\", 14) == 0)
                expansion_label = "Immortal Throne Item";
        }
        if (expansion_label)
            buf_write(&w, "<span color='#40FF40'>%s</span>\n", expansion_label);
    }

    /* Set info */
    {
        const char *set_dbr = base_data ? record_get_string_fast(base_data, INT_itemSetName) : NULL;
        if (set_dbr && set_dbr[0]) {
            TQArzRecordData *set_data = asset_get_dbr(set_dbr);
            const char *set_tag = set_data ? record_get_string_fast(set_data, INT_setName) : NULL;
            const char *set_name = set_tag ? translation_get(tr, set_tag) : NULL;
            if (set_name) {
                char *e_set = escape_markup(set_name);
                buf_write(&w, "\n<span color='#40FF40'>%s</span>\n", e_set);
                free(e_set);
            }
            /* List set members */
            TQVariable *members_var = set_data ? arz_record_get_var(set_data, INT_setMembers) : NULL;
            if (members_var && members_var->type == TQ_VAR_STRING) {
                for (uint32_t m = 0; m < members_var->count; m++) {
                    const char *member_path = members_var->value.str[m];
                    if (!member_path || !member_path[0]) continue;
                    TQArzRecordData *member_data = asset_get_dbr(member_path);
                    const char *member_tag = member_data ? record_get_string_fast(member_data, INT_description) : NULL;
                    if (!member_tag && member_data) member_tag = record_get_string_fast(member_data, INT_itemNameTag);
                    const char *member_name = member_tag ? translation_get(tr, member_tag) : NULL;
                    if (member_name) {
                        char *e_member = escape_markup(member_name);
                        buf_write(&w, "<span color='#FFF52B'>    %s</span>\n", e_member);
                        free(e_member);
                    }
                }
            }
        }
    }

    /* Requirements */
    add_requirements(base_name, &w);
}

/* ── resistance lookup (used by UI resistance table) ─────────────── */

static float get_dbr_resistance(const char *record_path, const char *attr_name, int shard_index) {
    if (!record_path || !record_path[0]) return 0.0f;
    TQArzRecordData *data = asset_get_dbr(record_path);
    if (!data) return 0.0f;
    const char *interned = arz_intern(attr_name);
    TQVariable *v = arz_record_get_var(data, interned);
    if (!v) return 0.0f;
    int idx = (shard_index < (int)v->count) ? shard_index : (int)v->count - 1;
    if (idx < 0) idx = 0;
    return (v->type == TQ_VAR_INT) ? (float)v->value.i32[idx] : v->value.f32[idx];
}

float item_get_resistance(TQItem *item, const char *attr_name) {
    if (!item || !attr_name) return 0.0f;
    int si1 = item->var1 > 0 ? (int)item->var1 - 1 : 0;
    int si2 = item->var2 > 0 ? (int)item->var2 - 1 : 0;
    return get_dbr_resistance(item->base_name, attr_name, 0)
         + get_dbr_resistance(item->prefix_name, attr_name, 0)
         + get_dbr_resistance(item->suffix_name, attr_name, 0)
         + get_dbr_resistance(item->relic_name, attr_name, si1)
         + get_dbr_resistance(item->relic_bonus, attr_name, 0)
         + get_dbr_resistance(item->relic_name2, attr_name, si2)
         + get_dbr_resistance(item->relic_bonus2, attr_name, 0);
}

/* ── public API ──────────────────────────────────────────────────── */

void item_format_stats(TQItem *item, TQTranslation *tr, char *buffer, size_t size) {
    if (!item) return;
    format_stats_common(item->seed, item->base_name, item->prefix_name, item->suffix_name,
        item->relic_name, item->relic_bonus, item->var1,
        item->relic_name2, item->relic_bonus2, item->var2, tr, buffer, size);
}

void vault_item_format_stats(TQVaultItem *item, TQTranslation *tr, char *buffer, size_t size) {
    if (!item) return;
    format_stats_common(item->seed, item->base_name, item->prefix_name, item->suffix_name,
        item->relic_name, item->relic_bonus, item->var1,
        item->relic_name2, item->relic_bonus2, item->var2, tr, buffer, size);
}
