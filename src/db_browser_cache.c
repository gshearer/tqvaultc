// db_browser_cache -- serialize/restore the Database Browser index to disk.
// See db_browser_cache.h for the design.

#include "db_browser_cache.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <glib/gstdio.h>

#include "config.h"
#include "asset_lookup.h"
#include "translation.h"

#define DB_CACHE_FILE "tqvc-db-browser.bin"
#define DB_CACHE_MAGIC "TQVD"

// Sentinel length meaning "NULL string" (distinct from an empty string, len 0).
#define DB_STR_NULL 0xFFFFFFFFu

// Loose sanity caps so a corrupt file can't make us allocate wildly.
#define DB_MAX_STR   (1u << 24)   // 16 MB
#define DB_MAX_ITEMS 4000000u
#define DB_MAX_LIST  1000000u

// -- Color table ------------------------------------------------------------
// DbBrowseItem.color is always one of these static strings (from
// get_item_color / db_creature_color / the set/skill/quest literals).  We
// serialize the index and map back to the canonical static pointer on load, so
// the loaded item's color stays a valid "do not free" pointer.  Adding a new
// color requires bumping DB_CACHE_VERSION.
static const char *const DB_CACHE_COLORS[] = {
  "white",    "#999999", "#00FFD1", "#91CB00", "#00A3FF", "#FFAD00",
  "#FF0000",  "#D905FF", "#40FF40", "#FFF52B",            // item rarity / class
  "#FF7050",  "#FFC850", "#70C8FF",                       // creature Boss/Hero/Quest
  "#DAA520",                                              // skill / quest gold
};
#define DB_CACHE_NCOLORS ((int)G_N_ELEMENTS(DB_CACHE_COLORS))

static uint8_t
db_color_to_index(const char *c)
{
  if(c)
    for(int i = 0; i < DB_CACHE_NCOLORS; i++)
      if(strcmp(c, DB_CACHE_COLORS[i]) == 0)
        return((uint8_t)i);

  // Unknown/NULL -> white.  Should not happen for the current builders; a new
  // color would need both a table entry and a DB_CACHE_VERSION bump.
  g_warning("db_browser_cache: unknown item color '%s' (defaulting to white)",
            c ? c : "(null)");
  return(0);
}

static const char *
db_color_from_index(uint8_t i)
{
  return(i < DB_CACHE_NCOLORS ? DB_CACHE_COLORS[i] : "white");
}

// -- Low-level IO helpers ---------------------------------------------------
// Native byte order (this is a local, per-machine cache, like the resource
// index).  Every read is checked; any short read aborts the load.

static bool
wr(FILE *f, const void *p, size_t n)
{
  return(fwrite(p, 1, n, f) == n);
}

static bool wr_u8(FILE *f, uint8_t v)   { return(wr(f, &v, 1)); }
static bool wr_u32(FILE *f, uint32_t v) { return(wr(f, &v, 4)); }
static bool wr_u64(FILE *f, uint64_t v) { return(wr(f, &v, 8)); }
static bool wr_i32(FILE *f, int32_t v)  { return(wr(f, &v, 4)); }
static bool wr_i64(FILE *f, int64_t v)  { return(wr(f, &v, 8)); }
static bool wr_f64(FILE *f, double v)   { return(wr(f, &v, 8)); }

// String: u32 length (DB_STR_NULL == NULL), then that many raw bytes (no NUL).
static bool
wr_str(FILE *f, const char *s)
{
  if(!s)
    return(wr_u32(f, DB_STR_NULL));

  size_t n = strlen(s);

  if(n >= DB_STR_NULL)
    return(false);
  if(!wr_u32(f, (uint32_t)n))
    return(false);
  return(n == 0 || wr(f, s, n));
}

// NULL-terminated string vector: u32 count (DB_STR_NULL == NULL vector), then
// each (non-NULL) element as a string.
static bool
wr_strv(FILE *f, char **v)
{
  if(!v)
    return(wr_u32(f, DB_STR_NULL));

  uint32_t n = 0;

  while(v[n])
    n++;
  if(!wr_u32(f, n))
    return(false);
  for(uint32_t i = 0; i < n; i++)
    if(!wr_str(f, v[i]))
      return(false);
  return(true);
}

static bool
rd(FILE *f, void *p, size_t n)
{
  return(fread(p, 1, n, f) == n);
}

static bool rd_u8(FILE *f, uint8_t *v)   { return(rd(f, v, 1)); }
static bool rd_u32(FILE *f, uint32_t *v) { return(rd(f, v, 4)); }
static bool rd_u64(FILE *f, uint64_t *v) { return(rd(f, v, 8)); }
static bool rd_i32(FILE *f, int32_t *v)  { return(rd(f, v, 4)); }
static bool rd_i64(FILE *f, int64_t *v)  { return(rd(f, v, 8)); }
static bool rd_f64(FILE *f, double *v)   { return(rd(f, v, 8)); }

// Read a string into *out (g_malloc'd, caller g_free's), or NULL for the
// sentinel.  Returns false on a short/oversized read.
static bool
rd_str(FILE *f, char **out)
{
  uint32_t n;

  if(!rd_u32(f, &n))
    return(false);
  if(n == DB_STR_NULL)
  {
    *out = NULL;
    return(true);
  }
  if(n > DB_MAX_STR)
    return(false);

  char *s = g_malloc(n + 1);

  if(n && !rd(f, s, n))
  {
    g_free(s);
    return(false);
  }
  s[n] = '\0';
  *out = s;
  return(true);
}

// Read a NULL-terminated string vector into *out (g_strfreev'able), or NULL for
// the sentinel.  Elements must be non-NULL; a NULL element is treated as
// corruption.
static bool
rd_strv(FILE *f, char ***out)
{
  uint32_t n;

  if(!rd_u32(f, &n))
    return(false);
  if(n == DB_STR_NULL)
  {
    *out = NULL;
    return(true);
  }
  if(n > DB_MAX_LIST)
    return(false);

  char **v = g_new0(char *, (size_t)n + 1);

  for(uint32_t i = 0; i < n; i++)
    if(!rd_str(f, &v[i]) || !v[i])
    {
      g_strfreev(v);
      return(false);
    }
  *out = v;
  return(true);
}

// -- Paths + header ---------------------------------------------------------

static char *
db_cache_path_new(void)
{
  char *dir = tqvc_cache_dir_new();
  char *path = g_build_filename(dir, DB_CACHE_FILE, NULL);

  g_free(dir);
  return(path);
}

// stat <game>/Database/database.arz for the size+mtime used to validate the
// cache against the live game install.
static bool
db_arz_stat(const char *game_folder, uint64_t *size, int64_t *mtime)
{
  if(!game_folder)
    return(false);

  char *p = g_build_filename(game_folder, "Database", "database.arz", NULL);
  GStatBuf sb;
  bool ok = (g_stat(p, &sb) == 0);

  g_free(p);
  if(!ok)
    return(false);

  *size  = (uint64_t)sb.st_size;
  *mtime = (int64_t)sb.st_mtime;
  return(true);
}

static bool
wr_header(FILE *f, uint64_t size, int64_t mtime, const char *game_folder)
{
  return(wr(f, DB_CACHE_MAGIC, 4) && wr_u32(f, DB_CACHE_VERSION) &&
         wr_u64(f, size) && wr_i64(f, mtime) &&
         wr_str(f, game_folder ? game_folder : ""));
}

// Validate the header against (size, mtime, game_folder).  Returns true on a
// full match.  game_folder is part of the key so correcting the game path
// (e.g. in Settings) invalidates a cache built for the old folder.
static bool
rd_header_ok(FILE *f, uint64_t size, int64_t mtime, const char *game_folder)
{
  char magic[4];
  uint32_t ver;
  uint64_t csize;
  int64_t cmtime;
  char *cdir = NULL;

  if(!rd(f, magic, 4) || memcmp(magic, DB_CACHE_MAGIC, 4) != 0)
    return(false);
  if(!rd_u32(f, &ver) || ver != DB_CACHE_VERSION)
    return(false);
  if(!rd_u64(f, &csize) || csize != size)
    return(false);
  if(!rd_i64(f, &cmtime) || cmtime != mtime)
    return(false);
  if(!rd_str(f, &cdir))
    return(false);

  bool match = (g_strcmp0(cdir, game_folder ? game_folder : "") == 0);

  g_free(cdir);
  return(match);
}

// -- Categories (DbBrowseItem lists) ----------------------------------------

// Pack the five item-kind booleans into one byte.
static uint8_t
db_item_flags(const DbBrowseItem *it)
{
  return((uint8_t)((it->is_set      ? 1 : 0) |
                   (it->is_affix    ? 2 : 0) |
                   (it->is_skill    ? 4 : 0) |
                   (it->is_creature ? 8 : 0) |
                   (it->is_quest    ? 16 : 0)));
}

static bool
wr_categories(FILE *f, const DbBrowserState *st)
{
  if(!wr_u32(f, CAT_COUNT))   // guard against CAT enum drift
    return(false);

  for(int c = 0; c < CAT_COUNT; c++)
  {
    GListModel *m = st->cat_stores[c] ? G_LIST_MODEL(st->cat_stores[c]) : NULL;
    guint n = m ? g_list_model_get_n_items(m) : 0;

    if(!wr_u32(f, n))
      return(false);

    for(guint i = 0; i < n; i++)
    {
      DbBrowseItem *it = g_list_model_get_item(m, i);
      bool ok =
        wr_str(f, it->path) &&
        wr_str(f, it->name) &&
        wr_str(f, it->name_lc) &&
        wr_str(f, it->search_blob) &&
        wr_str(f, it->icon_path) &&
        wr_u8(f, db_color_to_index(it->color)) &&
        wr_u32(f, it->var1) &&
        wr_u8(f, db_item_flags(it)) &&
        wr_i32(f, it->src_idx) &&
        wr_str(f, it->equip_types) &&
        wr_strv(f, it->variants);

      g_object_unref(it);
      if(!ok)
        return(false);
    }
  }
  return(true);
}

static bool
rd_categories(FILE *f, DbBrowserState *st)
{
  uint32_t ncat;

  if(!rd_u32(f, &ncat) || ncat != CAT_COUNT)
    return(false);

  for(int c = 0; c < CAT_COUNT; c++)
  {
    uint32_t n;

    if(!rd_u32(f, &n) || n > DB_MAX_ITEMS)
      return(false);

    for(uint32_t i = 0; i < n; i++)
    {
      char *path = NULL, *name = NULL, *name_lc = NULL, *blob = NULL, *icon = NULL;
      char *equip = NULL, **variants = NULL;
      uint8_t color_idx = 0, flags = 0;
      uint32_t var1 = 0;
      int32_t src_idx = 0;

      bool ok =
        rd_str(f, &path) &&
        rd_str(f, &name) &&
        rd_str(f, &name_lc) &&
        rd_str(f, &blob) &&
        rd_str(f, &icon) &&
        rd_u8(f, &color_idx) &&
        rd_u32(f, &var1) &&
        rd_u8(f, &flags) &&
        rd_i32(f, &src_idx) &&
        rd_str(f, &equip) &&
        rd_strv(f, &variants);

      if(!ok)
      {
        g_free(path); g_free(name); g_free(name_lc); g_free(blob); g_free(icon);
        g_free(equip); g_strfreev(variants);
        return(false);
      }

      DbBrowseItem *it = g_object_new(DB_TYPE_BROWSE_ITEM, NULL);

      it->path        = path;
      it->name        = name;
      it->name_lc     = name_lc;
      it->search_blob = blob;
      it->icon_path   = icon;
      it->color       = db_color_from_index(color_idx);
      it->var1        = var1;
      it->is_set      = (flags & 1)  != 0;
      it->is_affix    = (flags & 2)  != 0;
      it->is_skill    = (flags & 4)  != 0;
      it->is_creature = (flags & 8)  != 0;
      it->is_quest    = (flags & 16) != 0;
      it->src_idx     = src_idx;
      it->equip_types = equip;
      it->variants    = variants;

      g_list_store_append(st->cat_stores[c], it);
      g_object_unref(it);
      st->cat_counts[c]++;
    }
  }
  return(true);
}

// -- Creatures --------------------------------------------------------------

static bool
wr_creatures(FILE *f, const DbCreatureIndex *idx)
{
  if(!wr_u8(f, idx ? 1 : 0))
    return(false);
  if(!idx)
    return(true);

  if(!wr_u32(f, idx->creatures->len))
    return(false);

  for(guint i = 0; i < idx->creatures->len; i++)
  {
    DbCreature *c = g_ptr_array_index(idx->creatures, i);

    if(!wr_str(f, c->path) || !wr_str(f, c->name_tag) ||
       !wr_str(f, c->classification) || !wr_str(f, c->race))
      return(false);
    for(int d = 0; d < 3; d++)
      if(!wr_i32(f, c->level[d]))
        return(false);
    for(int d = 0; d < 3; d++)
      if(!wr_u8(f, c->active[d] ? 1 : 0))
        return(false);
    if(!wr_u8(f, c->has_drops ? 1 : 0))
      return(false);

    guint dn = c->drops ? c->drops->len : 0;

    if(!wr_u32(f, dn))
      return(false);
    for(guint d = 0; d < dn; d++)
    {
      DbItemChance *ic = g_ptr_array_index(c->drops, d);

      if(!wr_str(f, ic->item_lc))
        return(false);
      for(int k = 0; k < 3; k++)
        if(!wr_f64(f, ic->chance[k]))
          return(false);
    }
  }
  return(true);
}

static bool
rd_creatures(FILE *f, DbCreatureIndex **out)
{
  uint8_t present;

  if(!rd_u8(f, &present))
    return(false);
  if(!present)
  {
    *out = NULL;
    return(true);
  }

  uint32_t n;

  if(!rd_u32(f, &n) || n > DB_MAX_LIST)
    return(false);

  DbCreatureIndex *idx = db_creature_index_new_empty();

  for(uint32_t i = 0; i < n; i++)
  {
    char *path = NULL, *name_tag = NULL, *cls = NULL, *race = NULL;
    int level[3] = {0, 0, 0};
    bool active[3] = {false, false, false};
    uint8_t has_drops = 0;
    bool ok = rd_str(f, &path) && rd_str(f, &name_tag) &&
              rd_str(f, &cls) && rd_str(f, &race);

    for(int d = 0; d < 3 && ok; d++)
    {
      int32_t v;
      ok = rd_i32(f, &v);
      level[d] = v;
    }
    for(int d = 0; d < 3 && ok; d++)
    {
      uint8_t a;
      ok = rd_u8(f, &a);
      active[d] = (a != 0);
    }
    ok = ok && rd_u8(f, &has_drops);

    DbCreature *c = NULL;

    if(ok)
      c = db_creature_index_append(idx, path, name_tag, cls, race,
                                   level, active, has_drops != 0);
    g_free(path); g_free(name_tag); g_free(cls); g_free(race);
    if(!ok)
      goto fail;

    uint32_t dn;

    if(!rd_u32(f, &dn) || dn > DB_MAX_LIST)
      goto fail;
    for(uint32_t d = 0; d < dn; d++)
    {
      char *item_lc = NULL;
      double chance[3] = {0, 0, 0};
      bool dok = rd_str(f, &item_lc);

      for(int k = 0; k < 3 && dok; k++)
        dok = rd_f64(f, &chance[k]);
      if(dok)
        db_creature_append_drop(c, item_lc, chance);
      g_free(item_lc);
      if(!dok)
        goto fail;
    }
  }

  db_creature_index_finalize_reverse(idx);
  *out = idx;
  return(true);

fail:
  db_creature_index_free(idx);
  return(false);
}

// -- Quests -----------------------------------------------------------------

static bool
wr_quests(FILE *f, const DbQuestIndex *idx)
{
  if(!wr_u8(f, idx ? 1 : 0))
    return(false);
  if(!idx)
    return(true);

  if(!wr_u32(f, idx->quests->len))
    return(false);

  for(guint i = 0; i < idx->quests->len; i++)
  {
    DbQuest *q = g_ptr_array_index(idx->quests, i);

    if(!wr_str(f, q->title_tag) || !wr_str(f, q->label))
      return(false);

    for(int d = 0; d < 3; d++)
    {
      guint rn = q->rewards[d] ? g_hash_table_size(q->rewards[d]) : 0;

      if(!wr_u32(f, rn))
        return(false);
      if(!rn)
        continue;

      GHashTableIter it;
      gpointer k, v;

      g_hash_table_iter_init(&it, q->rewards[d]);
      while(g_hash_table_iter_next(&it, &k, &v))
        if(!wr_str(f, (const char *)k) || !wr_f64(f, *(double *)v))
          return(false);
    }
  }
  return(true);
}

static bool
rd_quests(FILE *f, DbQuestIndex **out)
{
  uint8_t present;

  if(!rd_u8(f, &present))
    return(false);
  if(!present)
  {
    *out = NULL;
    return(true);
  }

  uint32_t n;

  if(!rd_u32(f, &n) || n > DB_MAX_LIST)
    return(false);

  DbQuestIndex *idx = db_quest_index_new_empty();

  for(uint32_t i = 0; i < n; i++)
  {
    char *title = NULL, *label = NULL;

    if(!rd_str(f, &title) || !rd_str(f, &label))
    {
      g_free(title); g_free(label);
      goto fail;
    }

    DbQuest *q = db_quest_index_append(idx, title, label);

    g_free(title);
    g_free(label);

    for(int d = 0; d < 3; d++)
    {
      uint32_t rn;

      if(!rd_u32(f, &rn) || rn > DB_MAX_LIST)
        goto fail;
      for(uint32_t r = 0; r < rn; r++)
      {
        char *item_lc = NULL;
        double percent = 0.0;
        bool rok = rd_str(f, &item_lc) && rd_f64(f, &percent);

        if(rok)
          db_quest_append_reward(q, d, item_lc, percent);
        g_free(item_lc);
        if(!rok)
          goto fail;
      }
    }
  }

  db_quest_index_finalize_reverse(idx);
  *out = idx;
  return(true);

fail:
  db_quest_index_free(idx);
  return(false);
}

// -- Public API -------------------------------------------------------------

bool
db_browser_cache_valid(const char *game_folder)
{
  uint64_t size;
  int64_t mtime;

  if(!db_arz_stat(game_folder, &size, &mtime))
    return(false);

  char *path = db_cache_path_new();
  FILE *f = g_fopen(path, "rb");

  g_free(path);
  if(!f)
    return(false);

  bool ok = rd_header_ok(f, size, mtime, game_folder);

  fclose(f);
  return(ok);
}

bool
db_browser_cache_save(const DbBrowserState *st, const char *game_folder)
{
  uint64_t size;
  int64_t mtime;

  if(!st || !db_arz_stat(game_folder, &size, &mtime))
    return(false);

  char *path = db_cache_path_new();
  char *tmp = g_strconcat(path, ".tmp", NULL);
  FILE *f = g_fopen(tmp, "wb");

  if(!f)
  {
    g_warning("db_browser_cache: cannot write %s: %s", tmp, g_strerror(errno));
    g_free(tmp);
    g_free(path);
    return(false);
  }

  bool ok = wr_header(f, size, mtime, game_folder) &&
            wr_categories(f, st) &&
            wr_creatures(f, st->creatures) &&
            wr_quests(f, st->quests);

  if(fclose(f) != 0)
    ok = false;

  // Atomically swap the new cache into place (or discard a partial write).
  if(ok)
    ok = (g_rename(tmp, path) == 0);
  if(!ok)
    g_unlink(tmp);

  g_free(tmp);
  g_free(path);
  return(ok);
}

bool
db_browser_cache_load(DbBrowserState *st, const char *game_folder)
{
  uint64_t size;
  int64_t mtime;

  if(!st || !db_arz_stat(game_folder, &size, &mtime))
    return(false);

  char *path = db_cache_path_new();
  FILE *f = g_fopen(path, "rb");

  g_free(path);
  if(!f)
    return(false);

  bool ok = rd_header_ok(f, size, mtime, game_folder) &&
            rd_categories(f, st) &&
            rd_creatures(f, &st->creatures) &&
            rd_quests(f, &st->quests);

  fclose(f);

  if(!ok)
  {
    // Restore st to the empty state we received so the caller can fall back to
    // a live build without double-populating.
    for(int c = 0; c < CAT_COUNT; c++)
    {
      if(st->cat_stores[c])
        g_list_store_remove_all(st->cat_stores[c]);
      st->cat_counts[c] = 0;
    }
    if(st->creatures)
    {
      db_creature_index_free(st->creatures);
      st->creatures = NULL;
    }
    if(st->quests)
    {
      db_quest_index_free(st->quests);
      st->quests = NULL;
    }
    return(false);
  }
  return(true);
}

// -- Self-test --------------------------------------------------------------

// Build, save, reload, and compare — verifies the round-trip preserves every
// count.  Used headlessly via `tqvaultc --db-cache-selftest`.

// Short label per category, for the count dump.
static const char *
db_cat_label(int c)
{
  static const char *const N[CAT_COUNT] = {
    "Sword", "Axe", "Mace", "Spear", "Bow", "Staff", "Thrown",
    "Head", "Torso", "Arm", "Leg", "Shield",
    "Ring", "Amulet",
    "Relic", "Charm", "Artifact", "Scroll",
    "Set",
    "Prefix", "Suffix",
    "Skill:Defense", "Skill:Earth", "Skill:Hunting", "Skill:Nature",
    "Skill:Spirit", "Skill:Storm", "Skill:Warfare", "Skill:Dream",
    "Skill:Rune", "Skill:Rogue", "Skill:Neidan",
    "Creature", "Quest",
  };

  return((c >= 0 && c < CAT_COUNT) ? N[c] : "?");
}

static void
db_print_counts(const char *tag, const DbBrowserState *st)
{
  guint total = 0;

  printf("[%s] per-category item counts:\n", tag);
  for(int c = 0; c < CAT_COUNT; c++)
  {
    guint n = st->cat_stores[c]
              ? g_list_model_get_n_items(G_LIST_MODEL(st->cat_stores[c])) : 0;

    total += n;
    printf("  %-16s %6u\n", db_cat_label(c), n);
  }
  printf("  %-16s %6u\n", "TOTAL items", total);

  guint cre = st->creatures ? st->creatures->creatures->len : 0;
  guint cre_rev = (st->creatures && st->creatures->by_item)
                  ? g_hash_table_size(st->creatures->by_item) : 0;
  guint qst = st->quests ? st->quests->quests->len : 0;
  guint qst_rev = (st->quests && st->quests->by_item)
                  ? g_hash_table_size(st->quests->by_item) : 0;

  printf("  creatures: %u  (sourced items: %u)\n", cre, cre_rev);
  printf("  quests:    %u  (reward items:  %u)\n", qst, qst_rev);
}

// Allocate empty per-category stores on a zeroed state.
static void
db_alloc_stores(DbBrowserState *st)
{
  for(int c = 0; c < CAT_COUNT; c++)
    st->cat_stores[c] = g_list_store_new(DB_TYPE_BROWSE_ITEM);
}

// Minimal teardown for a selftest-owned state (no GTK widgets).
static void
db_free_state(DbBrowserState *st)
{
  for(int c = 0; c < CAT_COUNT; c++)
    g_clear_object(&st->cat_stores[c]);
  if(st->creatures)
    db_creature_index_free(st->creatures);
  if(st->quests)
    db_quest_index_free(st->quests);
  if(st->arz && st->owns_arz)
    arz_free(st->arz);
  g_free(st);
}

// True if the two states have identical per-category + creature + quest counts.
static bool
db_counts_match(const DbBrowserState *a, const DbBrowserState *b)
{
  for(int c = 0; c < CAT_COUNT; c++)
  {
    guint na = a->cat_stores[c]
               ? g_list_model_get_n_items(G_LIST_MODEL(a->cat_stores[c])) : 0;
    guint nb = b->cat_stores[c]
               ? g_list_model_get_n_items(G_LIST_MODEL(b->cat_stores[c])) : 0;

    if(na != nb)
    {
      printf("MISMATCH cat %s: %u vs %u\n", db_cat_label(c), na, nb);
      return(false);
    }
  }

  guint ca = a->creatures ? a->creatures->creatures->len : 0;
  guint cb = b->creatures ? b->creatures->creatures->len : 0;
  guint cra = (a->creatures && a->creatures->by_item)
              ? g_hash_table_size(a->creatures->by_item) : 0;
  guint crb = (b->creatures && b->creatures->by_item)
              ? g_hash_table_size(b->creatures->by_item) : 0;
  guint qa = a->quests ? a->quests->quests->len : 0;
  guint qb = b->quests ? b->quests->quests->len : 0;
  guint qra = (a->quests && a->quests->by_item)
              ? g_hash_table_size(a->quests->by_item) : 0;
  guint qrb = (b->quests && b->quests->by_item)
              ? g_hash_table_size(b->quests->by_item) : 0;

  if(ca != cb || cra != crb || qa != qb || qra != qrb)
  {
    printf("MISMATCH index: creatures %u/%u rev %u/%u  quests %u/%u rev %u/%u\n",
           ca, cb, cra, crb, qa, qb, qra, qrb);
    return(false);
  }
  return(true);
}

// True if every item's search_blob survives the round-trip byte-for-byte.
// Assumes the per-category counts already matched (db_counts_match).
static bool
db_blobs_match(const DbBrowserState *a, const DbBrowserState *b)
{
  for(int c = 0; c < CAT_COUNT; c++)
  {
    GListModel *ma = G_LIST_MODEL(a->cat_stores[c]);
    GListModel *mb = G_LIST_MODEL(b->cat_stores[c]);
    guint n = g_list_model_get_n_items(ma);

    for(guint i = 0; i < n; i++)
    {
      DbBrowseItem *ia = g_list_model_get_item(ma, i);
      DbBrowseItem *ib = g_list_model_get_item(mb, i);
      const char *sa = ia->search_blob ? ia->search_blob : "";
      const char *sb = ib->search_blob ? ib->search_blob : "";
      bool eq = (strcmp(sa, sb) == 0);

      g_object_unref(ia);
      g_object_unref(ib);
      if(!eq)
      {
        printf("MISMATCH blob cat %s item %u\n", db_cat_label(c), i);
        return(false);
      }
    }
  }
  return(true);
}

// Build a fully-indexed DbBrowserState live (all categories + creatures +
// quests + search blobs), exactly as the browser's idle loader does.  `tr` must
// outlive the returned state; free it with db_free_state().
static DbBrowserState *
db_selftest_build_state(TQTranslation *tr)
{
  DbBrowserState *st = g_new0(DbBrowserState, 1);

  st->tr = tr;
  // Mirror db_load_arz(): reuse the shared handle, else load our own copy
  // (asset_get_database_arz() returns NULL where the resource index stores
  // forward-slash paths, e.g. this Linux test install).
  st->arz      = asset_get_database_arz();
  st->owns_arz = false;
  if(!st->arz)
  {
    char arz_path[1024];

    snprintf(arz_path, sizeof(arz_path), "%s/Database/database.arz",
             global_config.game_folder);
    st->arz = arz_load(arz_path);
    st->owns_arz = true;
  }
  db_alloc_stores(st);

  build_category_index(st);
  build_set_index(st);
  build_affix_index(st);
  build_skill_index(st);
  build_creature_index_grid(st);
  build_quest_index_grid(st);
  build_search_blobs(st);
  return(st);
}

// Load an independent translation table (the index builders need it; mirrors
// the --tooltip one-shot).  No window required.  Returns NULL if unavailable.
static TQTranslation *
db_selftest_translations(void)
{
  TQTranslation *tr = translation_init();

  if(tr)
  {
    char p[1024];

    snprintf(p, sizeof(p), "%s/Text/Text_EN.arc", global_config.game_folder);
    translation_load_from_arc(tr, p);
  }
  return(tr);
}

int
db_browser_cache_selftest(void)
{
  const char *gf = global_config.game_folder;

  if(!gf)
  {
    fprintf(stderr, "db-cache-selftest: game_folder not configured\n");
    return(1);
  }

  TQTranslation *tr = db_selftest_translations();

  // 1. Build the index live, exactly as the browser's idle loader does.
  printf("Building index...\n");
  DbBrowserState *st = db_selftest_build_state(tr);

  db_print_counts("built", st);

  // 2. Save.
  if(!db_browser_cache_save(st, gf))
  {
    fprintf(stderr, "db-cache-selftest: save FAILED\n");
    db_free_state(st);
    if(tr)
      translation_free(tr);
    return(1);
  }
  printf("Saved cache OK.\n");

  // 3. Reload into a fresh state.
  DbBrowserState *st2 = g_new0(DbBrowserState, 1);

  st2->tr = tr;
  db_alloc_stores(st2);

  bool loaded = db_browser_cache_load(st2, gf);

  if(!loaded)
  {
    fprintf(stderr, "db-cache-selftest: load FAILED\n");
    db_free_state(st);
    db_free_state(st2);
    if(tr)
      translation_free(tr);
    return(1);
  }
  db_print_counts("loaded", st2);

  // 4. Compare counts, then assert every search_blob round-trips intact.
  bool ok = db_counts_match(st, st2) && db_blobs_match(st, st2);

  printf("\nROUNDTRIP %s\n", ok ? "OK" : "FAIL");

  db_free_state(st);
  db_free_state(st2);
  if(tr)
    translation_free(tr);
  return(ok ? 0 : 1);
}

int
db_browser_search_selftest(const char *keywords)
{
  const char *gf = global_config.game_folder;

  if(!gf)
  {
    fprintf(stderr, "db-search-selftest: game_folder not configured\n");
    return(1);
  }
  if(!keywords || !keywords[0])
  {
    fprintf(stderr, "db-search-selftest: usage: --db-search-selftest "
                    "\"<keywords>\" [config]\n");
    return(1);
  }

  TQTranslation *tr = db_selftest_translations();

  // Build the real full index + search blobs, exactly as the browser does.
  DbBrowserState *st = db_selftest_build_state(tr);

  // Compile the query with the SAME shared matcher the GUI search boxes use, so
  // this exercises regex (alternation/grouping/class) and multi-token AND alike.
  SearchQuery *q = search_query_compile(keywords);

  printf("Search: \"%s\"  ->  mode: %s\n\n", keywords, search_query_mode_name(q));

  int total = 0;
  int per_cat[CAT_COUNT] = { 0 };

  for(int c = 0; c < CAT_COUNT; c++)
  {
    GListModel *m = G_LIST_MODEL(st->cat_stores[c]);
    guint n = g_list_model_get_n_items(m);

    for(guint i = 0; i < n; i++)
    {
      DbBrowseItem *bi = g_list_model_get_item(m, i);
      const char *hay = bi->search_blob ? bi->search_blob : bi->name_lc;

      if(hay && search_query_match(q, hay))
      {
        printf("  %-14s %s\n", db_cat_label(c), bi->name ? bi->name : bi->path);
        per_cat[c]++;
        total++;
      }
      g_object_unref(bi);
    }
  }

  // Per-category tally (only the non-empty buckets), then the grand total.
  printf("\nby category:");
  for(int c = 0; c < CAT_COUNT; c++)
    if(per_cat[c] > 0)
      printf(" %s=%d", db_cat_label(c), per_cat[c]);
  printf("\n\n%d match%s for \"%s\"\n", total, total == 1 ? "" : "es", keywords);

  search_query_free(q);
  db_free_state(st);
  if(tr)
    translation_free(tr);
  return(0);
}

// Short label for a difficulty tier rank (see db_item_tier_rank).
static const char *
db_tier_label(int rank)
{
  switch(rank)
  {
    case 3: return("Legendary");
    case 2: return("Epic");
    case 1: return("Normal");
    default: return("-");
  }
}

int
db_browser_sort_selftest(void)
{
  const char *gf = global_config.game_folder;

  if(!gf)
  {
    fprintf(stderr, "db-sort-selftest: game_folder not configured\n");
    return(1);
  }

  TQTranslation *tr = db_selftest_translations();

  // Build the real full index live, then sort it exactly as the browser does
  // when it opens a window.
  DbBrowserState *st = db_selftest_build_state(tr);

  db_browser_sort_stores(st);

  // 1. Assert the invariants on every non-skill category: names non-decreasing
  //    (case-insensitive), and within an equal-name run the tier never rises
  //    (legendary -> epic -> normal).
  int violations = 0;

  for(int c = 0; c < CAT_COUNT; c++)
  {
    if(c >= CAT_SKILL_DEFENSE && c <= CAT_SKILL_NEIDAN)
      continue;  // skills keep their in-game tree order

    GListModel *m = G_LIST_MODEL(st->cat_stores[c]);
    guint n = g_list_model_get_n_items(m);
    char *prev_name = NULL;
    int prev_tier = 0;

    for(guint i = 0; i < n; i++)
    {
      DbBrowseItem *bi = g_list_model_get_item(m, i);
      const char *name = bi->name ? bi->name : "";
      int tier = db_item_tier_rank(bi->path);

      if(prev_name)
      {
        int nc = g_ascii_strcasecmp(prev_name, name);

        if(nc > 0)
        {
          if(violations < 10)
            printf("  ORDER %s: \"%s\" before \"%s\"\n",
                   db_cat_label(c), prev_name, name);
          violations++;
        }
        else if(nc == 0 && tier > prev_tier)
        {
          if(violations < 10)
            printf("  TIER  %s: \"%s\" %s before %s\n", db_cat_label(c), name,
                   db_tier_label(prev_tier), db_tier_label(tier));
          violations++;
        }
      }

      g_free(prev_name);
      prev_name = g_strdup(name);
      prev_tier = tier;
      g_object_unref(bi);
    }
    g_free(prev_name);
  }

  // 2. Demonstrate the tiering: print a few same-named runs that span more than
  //    one difficulty, confirming the legendary -> epic -> normal order.
  printf("Sample multi-tier runs (legendary->epic->normal):\n");
  int shown = 0;

  for(int c = 0; c < CAT_COUNT && shown < 6; c++)
  {
    if(c >= CAT_SKILL_DEFENSE && c <= CAT_SKILL_NEIDAN)
      continue;

    GListModel *m = G_LIST_MODEL(st->cat_stores[c]);
    guint n = g_list_model_get_n_items(m);

    for(guint i = 0; i < n && shown < 6; )
    {
      DbBrowseItem *a = g_list_model_get_item(m, i);
      guint j = i + 1;

      // Extend the run while the display name stays the same.
      while(j < n)
      {
        DbBrowseItem *b = g_list_model_get_item(m, j);
        int same = g_ascii_strcasecmp(a->name ? a->name : "",
                                      b->name ? b->name : "") == 0;
        g_object_unref(b);
        if(!same)
          break;
        j++;
      }

      if(j - i >= 2)
      {
        printf("  [%s] %s\n", db_cat_label(c), a->name ? a->name : "");
        for(guint k = i; k < j; k++)
        {
          DbBrowseItem *b = g_list_model_get_item(m, k);

          printf("      %-10s %s\n", db_tier_label(db_item_tier_rank(b->path)),
                 b->path);
          g_object_unref(b);
        }
        shown++;
      }

      g_object_unref(a);
      i = j;
    }
  }

  printf("\nSORT %s (%d violation%s)\n", violations == 0 ? "OK" : "FAIL",
         violations, violations == 1 ? "" : "s");

  db_free_state(st);
  if(tr)
    translation_free(tr);
  return(violations == 0 ? 0 : 1);
}
