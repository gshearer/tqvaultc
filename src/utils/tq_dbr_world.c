// tq-dbr-tool: loot tables, creature drops and quest rewards.

#include "tq_dbr_tool.h"
#include "../arc.h"
#include "../db_loot.h"
#include "../db_creatures.h"
#include "../db_quests.h"

// -- Creatures + Quests (Phase 6) -------------------------------------------
//
// These exercise the shared db_loot / db_creatures / db_quests modules headless
// (no GTK/translation), so names use FileDescription or the path basename.

// Resolve a readable name for a record path: FileDescription, else basename.
// Returns a g_strdup'd string the caller frees.
static char *
tool_record_name(TQArzFile *arz, const char *path)
{
  TQArzRecordData *d = arz_read_record(arz, path);

  if(d)
  {
    char *fd = arz_record_get_string(d, "FileDescription", NULL);
    arz_record_data_free(d);
    if(fd && fd[0])
      return(fd);
    free(fd);
  }

  // Basename without extension.
  const char *slash = strrchr(path, '\\');
  const char *base = slash ? slash + 1 : path;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", base);
  char *dot = strrchr(buf, '.');
  if(dot)
    *dot = '\0';

  return(g_strdup(buf));
}

// Sort helper: descending by a double* GHashTable value, keyed by string.
typedef struct { const char *key; double val; } KV;

static gint
kv_cmp_desc(gconstpointer a, gconstpointer b)
{
  double da = ((const KV *)a)->val, db = ((const KV *)b)->val;
  return(da < db) ? 1 : (da > db) ? -1 : 0;
}

// loot <arz> <table> [level]  -- flatten one loot table and print its items.
int
cmd_loot(const char *arz_path, const char *table, int level)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  GHashTable *out = db_loot_resolve(arz, table, level);

  if(!out)
  {
    printf("Not a loot table (or not found): %s\n", table);
    arz_free(arz);
    return(1);
  }

  GArray *rows = g_array_new(FALSE, FALSE, sizeof(KV));
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, out);
  while(g_hash_table_iter_next(&it, &k, &v))
  {
    KV row = { (const char *)k, *(double *)v };
    g_array_append_val(rows, row);
  }
  g_array_sort(rows, kv_cmp_desc);

  printf("Loot table: %s  (level %d)\n%u item(s):\n\n",
         table, level, rows->len);

  for(guint i = 0; i < rows->len; i++)
  {
    KV *row = &g_array_index(rows, KV, i);
    char *name = tool_record_name(arz, row->key);
    printf("  %8.4f  %-34s  %s\n", row->val, name, row->key);
    g_free(name);
  }

  g_array_free(rows, TRUE);
  g_hash_table_destroy(out);
  arz_free(arz);
  return(0);
}

// creatures <arz>  -- build the creature loot index and print a summary.
int
cmd_creatures(const char *arz_path)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  DbCreatureIndex *idx = db_creature_index_build(arz);

  int total = (int)idx->creatures->len, with_drops = 0;
  int boss = 0, hero = 0, quest = 0;

  for(guint i = 0; i < idx->creatures->len; i++)
  {
    DbCreature *c = g_ptr_array_index(idx->creatures, i);
    if(c->has_drops)
      with_drops++;
    if(g_ascii_strcasecmp(c->classification, "Boss") == 0)
      boss++;
    else if(g_ascii_strcasecmp(c->classification, "Hero") == 0)
      hero++;
    else
      quest++;
  }

  printf("Database Browser creatures — %s\n\n", arz_path);
  printf("  %d creatures (Boss %d / Hero %d / Quest %d); %d drop indexable loot\n",
         total, boss, hero, quest, with_drops);
  printf("  %u distinct items have a 'dropped by' source\n\n",
         g_hash_table_size(idx->by_item));

  // Show a sample of bosses with their largest drop, to eyeball correctness.
  printf("Sample bosses (name [levels] -> drop count):\n");
  int shown = 0;
  for(guint i = 0; i < idx->creatures->len && shown < 20; i++)
  {
    DbCreature *c = g_ptr_array_index(idx->creatures, i);
    if(g_ascii_strcasecmp(c->classification, "Boss") != 0 || !c->has_drops)
      continue;

    char *name = tool_record_name(arz, c->path);
    printf("  %-44s [%d/%d/%d]  %u items\n", name,
           c->level[0], c->level[1], c->level[2], c->drops->len);
    g_free(name);
    shown++;
  }

  db_creature_index_free(idx);
  arz_free(arz);
  return(0);
}

// droppedby <arz> <item>  -- list creatures that drop an item, by difficulty.
int
cmd_droppedby(const char *arz_path, const char *item)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  DbCreatureIndex *idx = db_creature_index_build(arz);
  GPtrArray *drops = db_creature_drops_for_item(idx, item);

  char *iname = tool_record_name(arz, item);
  printf("Dropped by — %s (%s)\n\n", iname, item);
  g_free(iname);

  if(!drops || drops->len == 0)
  {
    printf("  (no creature drops this item)\n");
  }
  else
  {
    printf("  %-44s   Normal     Epic Legendary\n", "Creature");
    for(guint i = 0; i < drops->len; i++)
    {
      DbDrop *d = g_ptr_array_index(drops, i);
      DbCreature *c = g_ptr_array_index(idx->creatures, d->creature_idx);
      char *name = tool_record_name(arz, c->path);
      printf("  %-44s %8.4f %8.4f %8.4f\n", name,
             d->chance[0], d->chance[1], d->chance[2]);
      g_free(name);
    }
  }

  db_creature_index_free(idx);
  arz_free(arz);
  return(0);
}

// quests <arz> <resources_dir>  -- build the quest reward index and summarize.
int
cmd_quests(const char *arz_path, const char *res_dir)
{
  TQArzFile *arz = arz_load(arz_path);

  if(!arz)
  {
    fprintf(stderr, "Failed to load ARZ: %s\n", arz_path);
    return(1);
  }

  // Candidate Quests.arc locations under the Resources directory.
  static const char *REL[] = {
    "Quests.arc", "xpack/Quests.arc", "XPack2/Quests.arc",
    "XPack3/Quests.arc", "XPack4/Quests.arc",
  };
  GPtrArray *arcs = g_ptr_array_new_with_free_func(g_free);
  for(size_t i = 0; i < sizeof(REL) / sizeof(REL[0]); i++)
  {
    char *p = g_build_filename(res_dir, REL[i], NULL);
    if(g_file_test(p, G_FILE_TEST_IS_REGULAR))
    {
      printf("  arc: %s\n", p);
      g_ptr_array_add(arcs, p);
    }
    else
      g_free(p);
  }

  if(arcs->len == 0)
    fprintf(stderr, "  (no Quests.arc found under %s)\n", res_dir);

  DbQuestIndex *idx = db_quest_index_build(arz,
      (const char *const *)arcs->pdata, (int)arcs->len);

  printf("\nDatabase Browser quests — %s\n\n", arz_path);
  printf("  %u quests grant item rewards; %u distinct reward items\n\n",
         idx->quests->len, g_hash_table_size(idx->by_item));

  int shown = 0;
  for(guint i = 0; i < idx->quests->len && shown < 20; i++)
  {
    DbQuest *q = g_ptr_array_index(idx->quests, i);
    int n0 = q->rewards[0] ? (int)g_hash_table_size(q->rewards[0]) : 0;
    int n1 = q->rewards[1] ? (int)g_hash_table_size(q->rewards[1]) : 0;
    int n2 = q->rewards[2] ? (int)g_hash_table_size(q->rewards[2]) : 0;
    printf("  %-40s  rewards N/E/L = %d/%d/%d\n", q->title_tag, n0, n1, n2);
    shown++;
  }

  db_quest_index_free(idx);
  g_ptr_array_free(arcs, TRUE);
  arz_free(arz);
  return(0);
}
