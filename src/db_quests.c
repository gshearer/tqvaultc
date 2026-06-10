// db_quests.c -- quest item-reward index (see db_quests.h).
//
// Mirrors reference/tqdb/tqdb/main.py:parse_quests, but reads the .qst binary
// token stream directly instead of tqdb's printable-strip + regex.  A .qst is a
// flat sequence of fields, each `<u32 nameLen><name><u32 valLen><value>` for
// string fields (int/float fields store raw bytes after the name).  We only
// need two string fields: `titleTag` (the quest's title tag) and `item[N]`
// (a per-difficulty reward DBR reference).  Each unique reward table is
// flattened through db_loot and its item chances recorded as percentages.

#include "db_quests.h"
#include "db_loot.h"
#include "arc.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// -- helpers ----------------------------------------------------------------

static char *
lc_path(const char *p)
{
  char *out = g_ascii_strdown(p, -1);

  for(char *q = out; *q; q++)
    if(*q == '/')
      *q = '\\';

  return(out);
}

static uint32_t
rd_u32(const uint8_t *b, size_t sz, size_t off)
{
  if(off + 4 > sz)
    return(0xFFFFFFFFu);

  return((uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
         ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24));
}

// Does a (lowercased) value look like a quest reward DBR reference?
// Matches tqdb's regex: records[\\/](xpack\d*[\\/])?quests[\\/]rewards[\\/]...dbr
static bool
is_reward_path(const char *lc)
{
  size_t len = strlen(lc);

  if(len < 5 || g_ascii_strcasecmp(lc + len - 4, ".dbr") != 0)
    return(false);

  return(strstr(lc, "records") != NULL &&
         strstr(lc, "\\quests\\") != NULL &&
         strstr(lc, "\\rewards\\") != NULL);
}

static void
quest_reward_array_free(gpointer data)
{
  g_ptr_array_free((GPtrArray *)data, TRUE);
}

static void
db_quest_free(gpointer data)
{
  DbQuest *q = data;

  g_free(q->title_tag);
  g_free(q->label);
  for(int i = 0; i < 3; i++)
    if(q->rewards[i])
      g_hash_table_destroy(q->rewards[i]);
  g_free(q);
}

// Accumulate a reward percentage for an item into a quest difficulty bucket.
static void
quest_add_reward(DbQuest *q, int diff, const char *item_lc, double percent)
{
  if(!q->rewards[diff])
    q->rewards[diff] = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);

  double *cur = g_hash_table_lookup(q->rewards[diff], item_lc);
  if(cur)
  {
    *cur += percent;
  }
  else
  {
    double *v = g_malloc(sizeof(double));
    *v = percent;
    g_hash_table_insert(q->rewards[diff], g_strdup(item_lc), v);
  }
}

// Parse one .qst blob: find its title tag and resolve every item[N] reward.
//   ctx        -- shared loot resolve context (record cache)
//   idx        -- index being built
//   by_tag     -- title_tag -> DbQuest*, so a quest spanning files merges
//   blob, sz   -- the extracted .qst bytes
//   label      -- the .qst entry name (fallback label)
static void
parse_qst(DbLootCtx *ctx, DbQuestIndex *idx, GHashTable *by_tag,
          const uint8_t *blob, size_t sz, const char *label)
{
  char *title = NULL;

  // First pass: locate the title tag.
  for(size_t i = 4; i + 4 <= sz && !title; i++)
  {
    uint32_t nlen = rd_u32(blob, sz, i - 4);
    if(nlen != 8 || i + nlen + 4 > sz)
      continue;
    if(g_ascii_strncasecmp((const char *)(blob + i), "titleTag", 8) != 0)
      continue;

    size_t voff = i + nlen;
    uint32_t vlen = rd_u32(blob, sz, voff);
    if(vlen == 0 || vlen > 256 || voff + 4 + vlen > sz)
      continue;

    title = g_strndup((const char *)(blob + voff + 4), vlen);
  }

  if(!title || !title[0])
  {
    g_free(title);
    return;
  }

  // Find-or-create the quest by its title tag (quests can span .qst files).
  DbQuest *q = g_hash_table_lookup(by_tag, title);
  if(!q)
  {
    q = g_new0(DbQuest, 1);
    q->title_tag = g_strdup(title);
    q->label = g_strdup(label);
    g_ptr_array_add(idx->quests, q);
    g_hash_table_insert(by_tag, g_strdup(title), q);
  }
  g_free(title);

  // Second pass: collect item[N] reward references, deduped by file (matching
  // tqdb: a reward file is processed once, at its first-seen difficulty).
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for(size_t i = 4; i + 4 <= sz; i++)
  {
    uint32_t nlen = rd_u32(blob, sz, i - 4);
    if(nlen != 7 || i + nlen + 4 > sz)
      continue;

    const char *nm = (const char *)(blob + i);
    if(g_ascii_strncasecmp(nm, "item[", 5) != 0 ||
       !isdigit((unsigned char)nm[5]) || nm[6] != ']')
      continue;

    int diff = nm[5] - '0';
    if(diff < 0 || diff > 2)
      continue;

    size_t voff = i + nlen;
    uint32_t vlen = rd_u32(blob, sz, voff);
    if(vlen == 0 || vlen > 512 || voff + 4 + vlen > sz)
      continue;

    char *val = g_strndup((const char *)(blob + voff + 4), vlen);
    char *lc  = lc_path(val);
    g_free(val);

    if(!is_reward_path(lc) || g_hash_table_contains(seen, lc))
    {
      g_free(lc);
      continue;
    }
    g_hash_table_add(seen, g_strdup(lc));

    // Resolve the reward table (no level reference, like tqdb).
    GHashTable *resolved = db_loot_resolve_ctx(ctx, lc, DB_LOOT_NO_LEVEL);
    g_free(lc);
    if(!resolved)
      continue;

    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, resolved);
    while(g_hash_table_iter_next(&it, &k, &v))
      quest_add_reward(q, diff, (const char *)k, (*(double *)v) * 100.0);

    g_hash_table_destroy(resolved);
  }

  g_hash_table_destroy(seen);
}

DbQuestIndex *
db_quest_index_build(TQArzFile *arz, const char *const *arc_files, int n_arcs)
{
  if(!arz)
    return(NULL);

  DbQuestIndex *idx = g_new0(DbQuestIndex, 1);
  idx->quests  = g_ptr_array_new_with_free_func(db_quest_free);
  idx->by_item = g_hash_table_new_full(g_str_hash, g_str_equal,
                                       g_free, quest_reward_array_free);

  GHashTable *by_tag = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);

  // Shared resolve context so repeated reward subtables inflate only once.
  DbLootCtx *ctx = db_loot_ctx_new(arz);

  for(int a = 0; a < n_arcs; a++)
  {
    if(!arc_files[a])
      continue;

    TQArcFile *arc = arc_load(arc_files[a]);
    if(!arc)
      continue;

    for(uint32_t e = 0; e < arc->num_files; e++)
    {
      const char *p = arc->entries[e].path;
      size_t plen = p ? strlen(p) : 0;

      if(plen < 4 || g_ascii_strcasecmp(p + plen - 4, ".qst") != 0)
        continue;

      size_t sz = 0;
      uint8_t *blob = arc_extract_file(arc, e, &sz);
      if(!blob)
        continue;

      parse_qst(ctx, idx, by_tag, blob, sz, p);
      free(blob);
    }

    arc_free(arc);
  }

  db_loot_ctx_free(ctx);
  g_hash_table_destroy(by_tag);

  // Drop quests with no item rewards (tqdb pops them), and build the reverse
  // item -> quests index from the survivors.
  for(guint qi = 0; qi < idx->quests->len; )
  {
    DbQuest *q = g_ptr_array_index(idx->quests, qi);

    bool any = false;
    for(int diff = 0; diff < 3; diff++)
      if(q->rewards[diff] && g_hash_table_size(q->rewards[diff]) > 0)
        any = true;

    if(!any)
    {
      g_ptr_array_remove_index(idx->quests, qi);  // frees q via free func
      continue;
    }
    qi++;
  }

  for(guint qi = 0; qi < idx->quests->len; qi++)
  {
    DbQuest *q = g_ptr_array_index(idx->quests, qi);

    // Collect the union of reward items across difficulties.
    GHashTable *items = g_hash_table_new(g_str_hash, g_str_equal);
    for(int diff = 0; diff < 3; diff++)
    {
      if(!q->rewards[diff])
        continue;
      GHashTableIter it;
      gpointer k, v;
      (void)v;
      g_hash_table_iter_init(&it, q->rewards[diff]);
      while(g_hash_table_iter_next(&it, &k, &v))
        g_hash_table_add(items, k);  // borrow key
    }

    GHashTableIter it;
    gpointer k, v;
    (void)v;
    g_hash_table_iter_init(&it, items);
    while(g_hash_table_iter_next(&it, &k, &v))
    {
      const char *item_lc = k;

      DbQuestReward *rw = g_new0(DbQuestReward, 1);
      rw->quest_idx = (int)qi;
      for(int diff = 0; diff < 3; diff++)
      {
        double *p = q->rewards[diff]
                    ? g_hash_table_lookup(q->rewards[diff], item_lc) : NULL;
        rw->percent[diff] = p ? *p : 0.0;
      }

      GPtrArray *arr = g_hash_table_lookup(idx->by_item, item_lc);
      if(!arr)
      {
        arr = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(idx->by_item, g_strdup(item_lc), arr);
      }
      g_ptr_array_add(arr, rw);
    }

    g_hash_table_destroy(items);
  }

  return(idx);
}

void
db_quest_index_free(DbQuestIndex *idx)
{
  if(!idx)
    return;

  if(idx->by_item)
    g_hash_table_destroy(idx->by_item);
  if(idx->quests)
    g_ptr_array_free(idx->quests, TRUE);

  g_free(idx);
}

GPtrArray *
db_quest_rewards_for_item(DbQuestIndex *idx, const char *item_path)
{
  if(!idx || !item_path)
    return(NULL);

  char *lc = lc_path(item_path);
  GPtrArray *arr = g_hash_table_lookup(idx->by_item, lc);
  g_free(lc);

  return(arr);
}
