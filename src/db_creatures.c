// db_creatures.c -- boss/hero/quest creature loot index (see db_creatures.h).
//
// Mirrors reference/tqdb/tqdb/parsers/creatures.py (MonsterParser) and
// main.py:parse_creatures.  For every Quest/Hero/Boss creature we resolve, per
// difficulty, the loot it can drop:
//
//   * Worn equipment: for each of the 11 equip slots a `chanceToEquip<Slot>`
//     gate plus up to 6 weighted `loot<Slot>Item{i}` loot tables.
//   * A boss chest, looked up by the creature's name tag in the CHESTS map.
//
// Each referenced loot table is flattened via db_loot, the item chances scaled
// by the slot weight x equip chance, and accumulated.  A reverse index
// (item -> creatures) is then built for the browser's "dropped by" view.

#include "db_creatures.h"
#include "db_loot.h"
#include <stdlib.h>
#include <string.h>

// Equip slots a monster can wear, in tqdb's order.
static const char *EQUIP_SLOTS[] = {
  "Head", "Torso", "LowerBody", "Forearm", "Finger1", "Finger2",
  "RightHand", "LeftHand", "Misc1", "Misc2", "Misc3",
};

// Boss-chest loot tables keyed by creature name tag, [normal, epic, legendary];
// "" = no chest that difficulty.  Ported verbatim from tqdb resources.py CHESTS.
static const struct { const char *tag, *n, *e, *l; } CHESTS[] = {
  {"tagMonsterName004", "records/item/containers/boss/bosschest13_chimera_normal.dbr", "records/item/containers/boss/bosschest13_chimera_epic.dbr.dbr", "records/item/containers/boss/bosschest13_chimera_legendary.dbr"},
  {"tagMonsterName122", "records/item/containers/boss/bosschest20_ormenos_normal.dbr", "records/item/containers/boss/bosschest20_ormenos_epic.dbr", "records/item/containers/boss/bosschest20_ormenos_legendary.dbr"},
  {"tagMonsterName155", "records/item/containers/boss/bosschest03_cyclops_normal.dbr", "records/item/containers/boss/bosschest03_cyclops_epic.dbr", "records/item/containers/boss/bosschest03_cyclops_legendary.dbr"},
  {"tagMonsterName1184", "records/item/containers/boss/bosschest19_yaoguai_normal.dbr", "records/item/containers/boss/bosschest19_yaoguai_epic.dbr", "records/item/containers/boss/bosschest19_yaoguai_legendary.dbr"},
  {"tagMonsterName1186", "", "records/item/containers/boss/bosschest24_dragonliche_epic.dbr", "records/item/containers/boss/bosschest24_dragonliche_legendary.dbr"},
  {"tagMonsterName121", "records/item/containers/boss/bosschest11_aktaios_normal.dbr", "records/item/containers/boss/bosschest11_aktaios_epic.dbr", "records/item/containers/boss/bosschest11_aktaios_legendary.dbr"},
  {"tagMonsterName1182", "records/item/containers/boss/bosschest15_gargantuanyeti_normal.dbr", "records/item/containers/boss/bosschest15_gargantuanyeti_epic.dbr", "records/item/containers/boss/bosschest15_gargantuanyeti_legendary.dbr"},
  {"tagMonsterName143", "records/item/containers/boss/bosschest05_gorgons_normal.dbr", "records/item/containers/boss/bosschest05_gorgons_epic.dbr", "records/item/containers/boss/bosschest05_gorgons_legendary.dbr"},
  {"tagMonsterName145", "records/item/containers/boss/bosschest05_gorgons_normal.dbr", "records/item/containers/boss/bosschest05_gorgons_epic.dbr", "records/item/containers/boss/bosschest05_gorgons_legendary.dbr"},
  {"tagMonsterName144", "records/item/containers/boss/bosschest05_gorgons_normal.dbr", "records/item/containers/boss/bosschest05_gorgons_epic.dbr", "records/item/containers/boss/bosschest05_gorgons_legendary.dbr"},
  {"tagMonsterName120", "records/item/containers/boss/bosschest07_megalesios_normal.dbr", "records/item/containers/boss/bosschest07_megalesios_epic.dbr", "records/item/containers/boss/bosschest07_megalesios_legendary.dbr"},
  {"tagMonsterName126", "", "", "records/item/containers/boss/bosschest25_hydra_legendary.dbr"},
  {"tagMonsterName1185", "", "records/item/containers/boss/bosschest23_manticore_epic.dbr", "records/item/containers/boss/bosschest23_manticore_legendary.dbr"},
  {"tagMonsterName286", "records/item/containers/boss/bosschest07a_minotaurlord_normal.dbr", "records/item/containers/boss/bosschest07a_minotaurlord_epic.dbr", "records/item/containers/boss/bosschest07a_minotaurlord_legendary.dbr"},
  {"tagMonsterName1183", "records/item/containers/boss/bosschest14_barmanu_normal.dbr", "records/item/containers/boss/bosschest14_barmanu_epic.dbr", "records/item/containers/boss/bosschest14_barmanu_legendary.dbr"},
  {"tagMonsterName110", "records/item/containers/boss/bosschest06_alastor_normal.dbr", "records/item/containers/boss/bosschest06_alastor_epic.dbr", "records/item/containers/boss/bosschest06_alastor_legendary.dbr"},
  {"tagMonsterName1180", "records/item/containers/boss/bosschest09_pharaohshonorguard_normal.dbr", "records/item/containers/boss/bosschest09_pharaohshonorguard_epic.dbr", "records/item/containers/boss/bosschest09_pharaohshonorguard_legendary.dbr"},
  {"tagMonsterName1129", "records/item/containers/boss/bosschest08x_masika_normal.dbr", "records/item/containers/boss/bosschest08x_masika_epic.dbr", "records/item/containers/boss/bosschest08x_masika_legendary.dbr"},
  {"tagMonsterName060", "records/item/containers/boss/bosschest12_sandwraithlord_normal.dbr", "records/item/containers/boss/bosschest12_sandwraithlord_epic.dbr", "records/item/containers/boss/bosschest12_sandwraithlord_legendary.dbr"},
  {"tagMonsterName293", "records/item/containers/boss/bosschest01_satyrshaman_normal.dbr", "records/item/containers/boss/bosschest01_satyrshaman_epic.dbr", "records/item/containers/boss/bosschest01_satyrshaman_legendary.dbr"},
  {"tagMonsterName043", "records/item/containers/boss/bosschest08_scarabaeus_normal.dbr", "records/item/containers/boss/bosschest08_scarabaeus_epic.dbr", "records/item/containers/boss/bosschest08_scarabaeus_legendary.dbr"},
  {"tagMonsterName115", "records/item/containers/boss/bosschest10_scorposking_normal.dbr", "records/item/containers/boss/bosschest10_scorposking_epic.dbr", "records/item/containers/boss/bosschest10_scorposking_legendary.dbr"},
  {"tagMonsterName097", "records/item/containers/boss/bosschest02_nessus_normal.dbr", "records/item/containers/boss/bosschest02_nessus_epic.dbr", "records/item/containers/boss/bosschest02_nessus_legendary.dbr"},
  {"tagMonsterName114", "records/item/containers/boss/bosschest04_arachne_normal.dbr", "records/item/containers/boss/bosschest04_arachne_epic.dbr", "records/item/containers/boss/bosschest04_arachne_legendary.dbr"},
  {"tagMonsterName066", "", "records/item/containers/boss/bosschest22_talos_epic.dbr", "records/item/containers/boss/bosschest22_talos_legendary.dbr"},
  {"tagMonsterName123", "records/item/containers/boss/bosschest18_bandari_normal.dbr", "records/item/containers/boss/bosschest18_bandari_epic.dbr", "records/item/containers/boss/bosschest18_bandari_legendary.dbr"},
  {"tagMonsterName382", "records/item/containers/boss/bosschest21_typhon_normal.dbr", "records/item/containers/boss/bosschest21_typhon_epic.dbr", "records/item/containers/boss/bosschest21_typhon_legendary.dbr"},
  {"tagMonsterName361", "records/item/containers/boss/bosschest17_xiao_normal.dbr", "records/item/containers/boss/bosschest17_xiao_epic.dbr", "records/item/containers/boss/bosschest17_xiao_legendary.dbr"},
  {"xtagMonsterGraeae1", "records/xpack/item/containers/bosschest01_graeae_01.dbr", "records/xpack/item/containers/bosschest01_graeae_02.dbr", "records/xpack/item/containers/bosschest01_graeae_03.dbr"},
  {"xtagMonsterGraeae2", "records/xpack/item/containers/bosschest01_graeae_01.dbr", "records/xpack/item/containers/bosschest01_graeae_02.dbr", "records/xpack/item/containers/bosschest01_graeae_03.dbr"},
  {"xtagMonsterGraeae3", "records/xpack/item/containers/bosschest01_graeae_01.dbr", "records/xpack/item/containers/bosschest01_graeae_02.dbr", "records/xpack/item/containers/bosschest01_graeae_03.dbr"},
  {"xtagMonsterCharon", "records/xpack/item/containers/bosschest02_charon_01.dbr", "records/xpack/item/containers/bosschest02_charon_02.dbr", "records/xpack/item/containers/bosschest02_charon_03.dbr"},
  {"xtagMonsterCerberus", "records/xpack/item/containers/bosschest03_cerberus_01.dbr", "records/xpack/item/containers/bosschest03_cerberus_02.dbr", "records/xpack/item/containers/bosschest03_cerberus_03.dbr"},
  {"xtagMonsterSkeletalTyphon", "records/xpack/item/containers/bosschest04_skeletaltyphon_01.dbr", "records/xpack/item/containers/bosschest04_skeletaltyphon_02.dbr", "records/xpack/item/containers/bosschest04_skeletaltyphon_03.dbr"},
  {"xtagMonsterHades", "records/xpack/item/containers/bosschest05_hades_01.dbr", "records/xpack/item/containers/bosschest05_hades_02.dbr", "records/xpack/item/containers/bosschest05_hades_03.dbr"},
  {"x2tagMonsterName096", "records/xpack2/item/containers/boss containers/bosschest01_porcus_01.dbr", "records/xpack2/item/containers/boss containers/bosschest01_porcus_02.dbr", "records/xpack2/item/containers/boss containers/bosschest01_porcus_03.dbr"},
  {"x2tagMonsterName184", "records/xpack2/item/containers/boss containers/bosschest03_mineghost_01.dbr", "records/xpack2/item/containers/boss containers/bosschest03_mineghost_02.dbr", "records/xpack2/item/containers/boss containers/bosschest03_mineghost_03.dbr"},
  {"x2tagMonsterName147", "records/xpack2/item/containers/boss containers/bosschest05_ancients_01.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_02.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_03.dbr"},
  {"x2tagMonsterName148", "records/xpack2/item/containers/boss containers/bosschest05_ancients_01.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_02.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_03.dbr"},
  {"x2tagMonsterName149", "records/xpack2/item/containers/boss containers/bosschest05_ancients_01.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_02.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_03.dbr"},
  {"x2tagMonsterName150", "records/xpack2/item/containers/boss containers/bosschest05_ancients_01.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_02.dbr", "records/xpack2/item/containers/boss containers/bosschest05_ancients_03.dbr"},
  {"x2tagMonsterName011", "records/xpack2/item/containers/boss containers/bosschest07_hildisvini_01.dbr", "records/xpack2/item/containers/boss containers/bosschest07_hildisvini_02.dbr", "records/xpack2/item/containers/boss containers/bosschest07_hildisvini_03.dbr"},
  {"x2tagMonsterName121", "records/xpack2/item/containers/boss containers/bosschest09_shroomking_01.dbr", "records/xpack2/item/containers/boss containers/bosschest09_shroomking_02.dbr", "records/xpack2/item/containers/boss containers/bosschest09_shroomking_03.dbr"},
  {"x2tagasgard_odin", "records/xpack2/item/containers/boss containers/bosschest10_aesir_01.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_02.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_03.dbr"},
  {"x2tagasgard_freyja", "records/xpack2/item/containers/boss containers/bosschest10_aesir_01.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_02.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_03.dbr"},
  {"x2tagasgard_baldr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_01.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_02.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_03.dbr"},
  {"x2tagasgard_tyr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_01.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_02.dbr", "records/xpack2/item/containers/boss containers/bosschest10_aesir_03.dbr"},
  {"x2tagutgard_mimer", "records/xpack2/item/containers/boss containers/bosschest12_mimir_01.dbr", "records/xpack2/item/containers/boss containers/bosschest12_mimir_02.dbr", "records/xpack2/item/containers/boss containers/bosschest12_mimir_03.dbr"},
  {"x2tagMonsterName093", "records/xpack2/item/containers/boss containers/bosschest14_loki_01.dbr", "records/xpack2/item/containers/boss containers/bosschest14_loki_02.dbr", "records/xpack2/item/containers/boss containers/bosschest14_loki_03.dbr"},
  {"x2tagmuspelheim_loki", "records/xpack2/item/containers/boss containers/bosschest14_loki_01.dbr", "records/xpack2/item/containers/boss containers/bosschest14_loki_02.dbr", "records/xpack2/item/containers/boss containers/bosschest14_loki_03.dbr"},
  {"x2tagMonsterName058", "records/xpack2/item/containers/boss containers/bosschest15_surtr_01.dbr", "records/xpack2/item/containers/boss containers/bosschest15_surtr_02.dbr", "records/xpack2/item/containers/boss containers/bosschest15_surtr_03.dbr"},
  {"x2tagMonsterName059", "records/xpack2/item/containers/boss containers/bosschest15_surtr_01.dbr", "records/xpack2/item/containers/boss containers/bosschest15_surtr_02.dbr", "records/xpack2/item/containers/boss containers/bosschest15_surtr_03.dbr"},
  {"x2tagMonsterName094", "", "", "records/xpack2/item/containers/boss containers/bosschest17_fafnir_03.dbr"},
};

// -- record value helpers (local; db_loot's are static) ---------------------

static TQVariable *
rec_var(TQArzRecordData *d, const char *name)
{
  return(arz_record_get_var(d, arz_intern(name)));
}

static const char *
rec_str(TQArzRecordData *d, const char *name)
{
  TQVariable *v = rec_var(d, name);

  if(v && v->type == TQ_VAR_STRING && v->count > 0 && v->value.str &&
     v->value.str[0] && v->value.str[0][0])
    return(v->value.str[0]);

  return(NULL);
}

static const char *
rec_str_at(TQArzRecordData *d, const char *name, uint32_t idx)
{
  TQVariable *v = rec_var(d, name);

  if(v && v->type == TQ_VAR_STRING && v->value.str && idx < v->count &&
     v->value.str[idx] && v->value.str[idx][0])
    return(v->value.str[idx]);

  return(NULL);
}

static double
rec_num(TQArzRecordData *d, const char *name)
{
  TQVariable *v = rec_var(d, name);

  if(!v || v->count == 0)
    return(0.0);

  if(v->type == TQ_VAR_INT && v->value.i32)
    return((double)v->value.i32[0]);
  if(v->type == TQ_VAR_FLOAT && v->value.f32)
    return((double)v->value.f32[0]);

  return(0.0);
}

static char *
lc_path(const char *p)
{
  char *out = g_ascii_strdown(p, -1);

  for(char *q = out; *q; q++)
    if(*q == '/')
      *q = '\\';

  return(out);
}

// Match the tqdb CREATURES globs: records\creature\monster\... (base) and
// records\xpack*\creatures\monster\... (expansions).
static bool
is_creature_path(const char *path)
{
  if(g_ascii_strncasecmp(path, "records\\creature\\monster\\", 25) == 0)
    return(true);

  if(g_ascii_strncasecmp(path, "records\\xpack", 13) != 0)
    return(false);

  const char *p = strstr(path, "\\creatures\\monster\\");
  if(!p)
  {
    char *lc = g_ascii_strdown(path, -1);
    bool m = (strstr(lc, "\\creatures\\monster\\") != NULL);
    g_free(lc);
    return(m);
  }

  return(true);
}

// Free callback for a GPtrArray<DbDrop*> stored in by_item.
static void
drop_array_free(gpointer data)
{
  g_ptr_array_free((GPtrArray *)data, TRUE);
}

// Merge a resolved loot map (item_lc -> double*) into a per-creature aggregate
// (item_lc -> double[3]), scaled by `factor`, recording into difficulty d.
static void
merge_into_agg(GHashTable *agg, GHashTable *resolved, double factor, int d)
{
  GHashTableIter it;
  gpointer k, v;

  g_hash_table_iter_init(&it, resolved);
  while(g_hash_table_iter_next(&it, &k, &v))
  {
    double add = (*(double *)v) * factor;
    double *tier = g_hash_table_lookup(agg, k);

    if(!tier)
    {
      tier = g_malloc0(sizeof(double) * 3);
      g_hash_table_insert(agg, g_strdup((const char *)k), tier);
    }

    tier[d] += add;
  }
}

// Add a single direct-item drop into the aggregate.
static void
add_item_to_agg(GHashTable *agg, const char *item_lc, double chance, int d)
{
  double *tier = g_hash_table_lookup(agg, item_lc);

  if(!tier)
  {
    tier = g_malloc0(sizeof(double) * 3);
    g_hash_table_insert(agg, g_strdup(item_lc), tier);
  }

  tier[d] += chance;
}

// Resolve a loot-table reference and merge its (scaled) items into agg; if the
// reference is a direct item (not a table), add it singly.  Mirrors tqdb's
// "if 'tag' in loot / elif 'loot_table' in loot" branch.
static void
merge_ref(DbLootCtx *ctx, GHashTable *agg, const char *ref, int level,
          double factor, int d)
{
  if(!ref || !ref[0])
    return;

  GHashTable *resolved = db_loot_resolve_ctx(ctx, ref, level);

  if(resolved)
  {
    merge_into_agg(agg, resolved, factor, d);
    g_hash_table_destroy(resolved);
  }
  else
  {
    char *lc = lc_path(ref);
    add_item_to_agg(agg, lc, factor, d);
    g_free(lc);
  }
}

// Find a boss chest difficulty path for a creature name tag, or NULL.
static const char *
chest_path_for(const char *tag, int d)
{
  if(!tag)
    return(NULL);

  for(size_t i = 0; i < G_N_ELEMENTS(CHESTS); i++)
  {
    if(g_ascii_strcasecmp(CHESTS[i].tag, tag) == 0)
    {
      const char *p = (d == 0) ? CHESTS[i].n : (d == 1) ? CHESTS[i].e : CHESTS[i].l;
      return((p && p[0]) ? p : NULL);
    }
  }

  return(NULL);
}

static void
item_chance_free(gpointer data)
{
  DbItemChance *ic = data;
  g_free(ic->item_lc);
  g_free(ic);
}

// Sort a creature's forward drop list by best per-difficulty chance, desc.
static gint
item_chance_cmp(gconstpointer a, gconstpointer b)
{
  const DbItemChance *x = *(const DbItemChance *const *)a;
  const DbItemChance *y = *(const DbItemChance *const *)b;
  double mx = MAX(x->chance[0], MAX(x->chance[1], x->chance[2]));
  double my = MAX(y->chance[0], MAX(y->chance[1], y->chance[2]));
  return(mx < my) ? 1 : (mx > my) ? -1 : 0;
}

// Sort a reverse (item -> creatures) list by best per-difficulty chance, desc,
// so the most likely sources show first.
static gint
drop_cmp(gconstpointer a, gconstpointer b)
{
  const DbDrop *x = *(const DbDrop *const *)a;
  const DbDrop *y = *(const DbDrop *const *)b;
  double mx = MAX(x->chance[0], MAX(x->chance[1], x->chance[2]));
  double my = MAX(y->chance[0], MAX(y->chance[1], y->chance[2]));
  return(mx < my) ? 1 : (mx > my) ? -1 : 0;
}

static void
db_creature_free(gpointer data)
{
  DbCreature *c = data;

  g_free(c->path);
  g_free(c->name_tag);
  g_free(c->classification);
  g_free(c->race);
  if(c->drops)
    g_ptr_array_free(c->drops, TRUE);
  g_free(c);
}

// creature_from_record -- build a DbCreature from a loaded record, or NULL if
// it should be skipped (wrong classification, missing tag, or level-less).
// The returned creature has path/name_tag/classification/race + level[]/active[]
// filled; drops array and index insertion are the caller's job.
static DbCreature *
creature_from_record(TQArzRecordData *d, const char *path)
{
  const char *cls = rec_str(d, "monsterClassification");
  const char *tag = rec_str(d, "description");

  if(!cls || !tag ||
     (g_ascii_strcasecmp(cls, "Quest") != 0 &&
      g_ascii_strcasecmp(cls, "Hero")  != 0 &&
      g_ascii_strcasecmp(cls, "Boss")  != 0))
    return(NULL);

  // charLevel per difficulty (clone the last value forward if short).
  TQVariable *lv = rec_var(d, "charLevel");
  int nlv = (lv && (lv->type == TQ_VAR_INT || lv->type == TQ_VAR_FLOAT))
            ? (int)lv->count : 0;

  if(nlv == 0)
    return(NULL);  // tqdb skips level-less creatures

  DbCreature *c = g_new0(DbCreature, 1);
  c->path           = g_strdup(path);
  c->name_tag       = g_strdup(tag);
  c->classification = g_strdup(cls);
  {
    const char *race = rec_str(d, "characterRacialProfile");
    c->race = race ? g_strdup(race) : NULL;
  }

  for(int i = 0; i < 3; i++)
  {
    int src = (i < nlv) ? i : nlv - 1;
    double val = (lv->type == TQ_VAR_INT) ? (double)lv->value.i32[src]
                                          : (double)lv->value.f32[src];
    c->level[i]  = (int)val;
    c->active[i] = true;
  }

  // tqdb: a difficulty whose level repeats the first one means the creature
  // doesn't spawn there; deactivate (count(level[0]) - 1) leading tiers.
  int repeats = 0;
  for(int i = 0; i < 3; i++)
    if(c->level[i] == c->level[0])
      repeats++;
  if(repeats > 1 && repeats < 3 + 1)
    for(int i = 0; i < repeats - 1 && i < 3; i++)
      c->active[i] = false;

  return(c);
}

// creature_collect_drops -- aggregate this creature's item -> chance[3] map
// across its active difficulties (worn equipment weights + boss chest) into agg.
static void
creature_collect_drops(DbLootCtx *ctx, TQArzRecordData *d, DbCreature *c,
                       GHashTable *agg)
{
  for(int diff = 0; diff < 3; diff++)
  {
    if(!c->active[diff])
      continue;

    int level = c->level[diff];

    // Worn equipment.
    for(size_t s = 0; s < G_N_ELEMENTS(EQUIP_SLOTS); s++)
    {
      char key[48];
      g_snprintf(key, sizeof(key), "chanceToEquip%s", EQUIP_SLOTS[s]);
      double equip_chance = rec_num(d, key);
      if(equip_chance <= 0.0)
        continue;

      double summed = 0.0;
      for(int i = 1; i <= 6; i++)
      {
        g_snprintf(key, sizeof(key), "chanceToEquip%sItem%d", EQUIP_SLOTS[s], i);
        summed += rec_num(d, key);
      }
      if(summed <= 0.0)
        continue;

      for(int i = 1; i <= 6; i++)
      {
        g_snprintf(key, sizeof(key), "chanceToEquip%sItem%d", EQUIP_SLOTS[s], i);
        double weight = rec_num(d, key);
        if(weight <= 0.0)
          continue;

        g_snprintf(key, sizeof(key), "loot%sItem%d", EQUIP_SLOTS[s], i);
        const char *ref = rec_str_at(d, key, (uint32_t)diff);
        if(!ref)
          continue;

        double factor = (weight / summed) * equip_chance;
        merge_ref(ctx, agg, ref, level, factor, diff);
      }
    }

    // Boss chest.
    const char *chest = chest_path_for(c->name_tag, diff);
    if(chest)
    {
      GHashTable *resolved = db_loot_resolve_ctx(ctx, chest, level);
      if(resolved)
      {
        merge_into_agg(agg, resolved, 1.0, diff);
        g_hash_table_destroy(resolved);
      }
    }
  }
}

// creature_fold_agg -- fold a creature's aggregated drop map into both the
// reverse index (idx->by_item) and its own forward list (c->drops).
static void
creature_fold_agg(DbCreatureIndex *idx, DbCreature *c, GHashTable *agg, int cidx)
{
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, agg);
  while(g_hash_table_iter_next(&it, &k, &v))
  {
    double *tier = v;
    if(tier[0] <= 0.0 && tier[1] <= 0.0 && tier[2] <= 0.0)
      continue;

    c->has_drops = true;

    // Reverse index: item -> creatures.
    GPtrArray *drops = g_hash_table_lookup(idx->by_item, k);
    if(!drops)
    {
      drops = g_ptr_array_new_with_free_func(g_free);
      g_hash_table_insert(idx->by_item, g_strdup((const char *)k), drops);
    }

    DbDrop *drop = g_new0(DbDrop, 1);
    drop->creature_idx = cidx;
    drop->chance[0] = tier[0];
    drop->chance[1] = tier[1];
    drop->chance[2] = tier[2];
    g_ptr_array_add(drops, drop);

    // Forward list: creature -> items.
    DbItemChance *ic = g_new0(DbItemChance, 1);
    ic->item_lc = g_strdup((const char *)k);
    ic->chance[0] = tier[0];
    ic->chance[1] = tier[1];
    ic->chance[2] = tier[2];
    g_ptr_array_add(c->drops, ic);
  }
}

DbCreatureIndex *
db_creature_index_build(TQArzFile *arz)
{
  if(!arz)
    return(NULL);

  DbCreatureIndex *idx = g_new0(DbCreatureIndex, 1);
  idx->creatures = g_ptr_array_new_with_free_func(db_creature_free);
  idx->by_item   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, drop_array_free);

  // One resolve context: shares a decompressed-record cache across every
  // creature so the many shared loot subtables inflate only once.
  DbLootCtx *ctx = db_loot_ctx_new(arz);

  // Count of creatures actually resolved (not raw record index), used to bound
  // the context cache periodically below.
  unsigned resolved = 0;

  for(uint32_t r = 0; r < arz->num_records; r++)
  {
    const char *path = arz->records[r].path;

    if(!path || !is_creature_path(path))
      continue;

    TQArzRecordData *d = arz_read_record(arz, path);
    if(!d)
      continue;

    DbCreature *c = creature_from_record(d, path);
    if(!c)
    {
      arz_record_data_free(d);
      continue;
    }

    int cidx = (int)idx->creatures->len;
    c->drops = g_ptr_array_new_with_free_func(item_chance_free);
    g_ptr_array_add(idx->creatures, c);

    // Aggregate item -> chance[3] across difficulties, then fold into the index.
    GHashTable *agg = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);

    creature_collect_drops(ctx, d, c, agg);
    arz_record_data_free(d);
    creature_fold_agg(idx, c, agg, cidx);

    g_ptr_array_sort(c->drops, item_chance_cmp);
    g_hash_table_destroy(agg);

    // Bound the resolve cache during the first-run build: the LootRandomizer
    // records it holds are large, so resolving all ~950 looted creatures would
    // otherwise balloon to hundreds of MB.  Drop it every so often (the shared
    // subtables re-inflate cheaply on the next creature) -- safe here, no
    // borrowed ctx_read() pointer is live between creatures.
    if((++resolved & 0x000F) == 0)
      db_loot_ctx_clear_cache(ctx);
  }

  db_loot_ctx_free(ctx);

  // Sort each reverse list so "dropped by" shows the most likely sources first.
  GHashTableIter rit;
  gpointer rk, rv;
  g_hash_table_iter_init(&rit, idx->by_item);
  while(g_hash_table_iter_next(&rit, &rk, &rv))
    g_ptr_array_sort((GPtrArray *)rv, drop_cmp);

  return(idx);
}

// -- Cache reconstruction ---------------------------------------------------

DbCreatureIndex *
db_creature_index_new_empty(void)
{
  DbCreatureIndex *idx = g_new0(DbCreatureIndex, 1);

  idx->creatures = g_ptr_array_new_with_free_func(db_creature_free);
  idx->by_item   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, drop_array_free);
  return(idx);
}

DbCreature *
db_creature_index_append(DbCreatureIndex *idx, const char *path,
                         const char *name_tag, const char *classification,
                         const char *race, const int level[3],
                         const bool active[3], bool has_drops)
{
  DbCreature *c = g_new0(DbCreature, 1);

  c->path           = g_strdup(path);
  c->name_tag       = g_strdup(name_tag);
  c->classification = g_strdup(classification);
  c->race           = g_strdup(race);
  for(int i = 0; i < 3; i++)
  {
    c->level[i]  = level ? level[i] : 0;
    c->active[i] = active ? active[i] : false;
  }
  c->has_drops = has_drops;
  c->drops     = g_ptr_array_new_with_free_func(item_chance_free);

  g_ptr_array_add(idx->creatures, c);
  return(c);
}

void
db_creature_append_drop(DbCreature *c, const char *item_lc,
                        const double chance[3])
{
  DbItemChance *ic = g_new0(DbItemChance, 1);

  ic->item_lc   = g_strdup(item_lc);
  ic->chance[0] = chance ? chance[0] : 0.0;
  ic->chance[1] = chance ? chance[1] : 0.0;
  ic->chance[2] = chance ? chance[2] : 0.0;
  g_ptr_array_add(c->drops, ic);
}

// Rebuild the reverse (item -> creatures) map from the already-loaded forward
// drop lists, then sort each list — mirrors the tail of db_creature_index_build.
void
db_creature_index_finalize_reverse(DbCreatureIndex *idx)
{
  if(!idx)
    return;

  for(guint ci = 0; ci < idx->creatures->len; ci++)
  {
    DbCreature *c = g_ptr_array_index(idx->creatures, ci);

    for(guint di = 0; di < c->drops->len; di++)
    {
      DbItemChance *ic = g_ptr_array_index(c->drops, di);

      GPtrArray *drops = g_hash_table_lookup(idx->by_item, ic->item_lc);
      if(!drops)
      {
        drops = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(idx->by_item, g_strdup(ic->item_lc), drops);
      }

      DbDrop *drop = g_new0(DbDrop, 1);
      drop->creature_idx = (int)ci;
      drop->chance[0] = ic->chance[0];
      drop->chance[1] = ic->chance[1];
      drop->chance[2] = ic->chance[2];
      g_ptr_array_add(drops, drop);
    }
  }

  GHashTableIter rit;
  gpointer rk, rv;
  g_hash_table_iter_init(&rit, idx->by_item);
  while(g_hash_table_iter_next(&rit, &rk, &rv))
    g_ptr_array_sort((GPtrArray *)rv, drop_cmp);
}

void
db_creature_index_free(DbCreatureIndex *idx)
{
  if(!idx)
    return;

  if(idx->by_item)
    g_hash_table_destroy(idx->by_item);
  if(idx->creatures)
    g_ptr_array_free(idx->creatures, TRUE);

  g_free(idx);
}

GPtrArray *
db_creature_drops_for_item(DbCreatureIndex *idx, const char *item_path)
{
  if(!idx || !item_path)
    return(NULL);

  char *lc = lc_path(item_path);
  GPtrArray *drops = g_hash_table_lookup(idx->by_item, lc);
  g_free(lc);

  return(drops);
}
