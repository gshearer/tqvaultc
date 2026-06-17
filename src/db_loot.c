// db_loot.c -- shared loot-table resolver (see db_loot.h).
//
// Mirrors reference/tqdb/tqdb/parsers/loot.py.  The five loot-table classes:
//
//   LootMasterTable            lootWeight{i}/lootName{i}, each name -> a table
//   LootItemTable_FixedWeight  lootWeight{i}/lootName{i}, each name -> an item
//   LootItemTable_DynWeight    itemNames[] gated by a level window + bell slope
//   FixedItemContainer         a single 'tables' reference to another table
//   FixedItemLoot              6 slots x 6 weighted names, scaled by chance/spawn
//
// We unify FixedWeight and MasterTable: a referenced child is recursed into when
// it is itself a loot table, otherwise treated as a droppable item.  That single
// rule reproduces tqdb's split (FixedWeight children parse to items, MasterTable
// children parse to tables) without needing per-class child handling.

#include "db_loot.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Loot chains in the shipped database are shallow (<= 4); this is a generous
// backstop against a malformed cyclic reference.
#define DB_LOOT_MAX_DEPTH 16

// -- resolve context (record cache) -----------------------------------------

struct DbLootCtx {
  TQArzFile  *arz;
  GHashTable *cache;  // lc path -> TQArzRecordData* (owned); cached reads
};

DbLootCtx *
db_loot_ctx_new(TQArzFile *arz)
{
  DbLootCtx *ctx = g_new0(DbLootCtx, 1);
  ctx->arz = arz;
  ctx->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                     (GDestroyNotify)arz_record_data_free);
  return(ctx);
}

void
db_loot_ctx_free(DbLootCtx *ctx)
{
  if(!ctx)
    return;

  g_hash_table_destroy(ctx->cache);
  g_free(ctx);
}

// Drop the context's cached records, reclaiming their heap.  LootRandomizer
// records are large (~594 vars each), so resolving every creature's loot can
// accumulate hundreds of MB here -- a separate balloon from the global DBR
// cache.  Safe to call BETWEEN top-level resolves (a caller must hold no
// borrowed ctx_read() pointer across it); the shared subtables simply re-inflate
// on the next resolve.  Lets the first-run creature pass stay bounded.
void
db_loot_ctx_clear_cache(DbLootCtx *ctx)
{
  if(ctx && ctx->cache)
    g_hash_table_remove_all(ctx->cache);
}

// Read a record through the context cache.  Returns a BORROWED pointer (owned
// by the cache); do not free.  Records inflate once per resolve session.
static TQArzRecordData *
ctx_read(DbLootCtx *ctx, const char *path)
{
  char *key = g_ascii_strdown(path, -1);
  for(char *q = key; *q; q++)
    if(*q == '/')
      *q = '\\';

  gpointer val = NULL;
  if(g_hash_table_lookup_extended(ctx->cache, key, NULL, &val))
  {
    g_free(key);
    return(val);  // cached (possibly NULL for a known-missing record)
  }

  TQArzRecordData *d = arz_read_record(ctx->arz, path);
  g_hash_table_insert(ctx->cache, key, d);  // takes ownership of key + d
  return(d);
}

// -- record value helpers ---------------------------------------------------

static TQVariable *
rec_var(TQArzRecordData *d, const char *name)
{
  return(arz_record_get_var(d, arz_intern(name)));
}

// First string value of a variable, or NULL (also NULL for empty strings).
static const char *
rec_str(TQArzRecordData *d, const char *name)
{
  TQVariable *v = rec_var(d, name);

  if(v && v->type == TQ_VAR_STRING && v->count > 0 && v->value.str &&
     v->value.str[0] && v->value.str[0][0])
    return(v->value.str[0]);

  return(NULL);
}

// String value at array index idx, or NULL.
static const char *
rec_str_at(TQArzRecordData *d, const char *name, uint32_t idx)
{
  TQVariable *v = rec_var(d, name);

  if(v && v->type == TQ_VAR_STRING && v->value.str && idx < v->count &&
     v->value.str[idx] && v->value.str[idx][0])
    return(v->value.str[idx]);

  return(NULL);
}

// Numeric value (int or float) at array index idx, else 0.
static double
var_num_at(TQVariable *v, uint32_t idx)
{
  if(!v || idx >= v->count)
    return(0.0);

  if(v->type == TQ_VAR_INT && v->value.i32)
    return((double)v->value.i32[idx]);

  if(v->type == TQ_VAR_FLOAT && v->value.f32)
    return((double)v->value.f32[idx]);

  return(0.0);
}

// First numeric value of a variable, else 0.
static double
rec_num(TQArzRecordData *d, const char *name)
{
  return(var_num_at(rec_var(d, name), 0));
}

// Lowercase + backslash-normalize a record path (caller g_free's).
static char *
lc_path(const char *p)
{
  char *out = g_ascii_strdown(p, -1);

  for(char *q = out; *q; q++)
    if(*q == '/')
      *q = '\\';

  return(out);
}

// -- equation evaluator -----------------------------------------------------

typedef struct {
  const char *p;
  double      level;  // parentLevel / averagePlayerLevel
} EvalCtx;

static double eval_expr(EvalCtx *c);

static void
eval_skipws(EvalCtx *c)
{
  while(*c->p == ' ' || *c->p == '\t')
    c->p++;
}

static double
eval_factor(EvalCtx *c)
{
  eval_skipws(c);

  if(*c->p == '(')
  {
    c->p++;  // consume '('
    double v = eval_expr(c);
    eval_skipws(c);
    if(*c->p == ')')
      c->p++;
    return(v);
  }

  if(*c->p == '-')
  {
    c->p++;
    return(-eval_factor(c));
  }

  if(*c->p == '+')
  {
    c->p++;
    return(eval_factor(c));
  }

  if(isdigit((unsigned char)*c->p) || *c->p == '.')
  {
    char *end = NULL;
    double v = strtod(c->p, &end);
    if(end && end != c->p)
      c->p = end;
    return(v);
  }

  if(isalpha((unsigned char)*c->p) || *c->p == '_')
  {
    const char *start = c->p;
    while(isalnum((unsigned char)*c->p) || *c->p == '_')
      c->p++;
    size_t len = (size_t)(c->p - start);

    if((len == 11 && g_ascii_strncasecmp(start, "parentLevel", 11) == 0) ||
       (len == 18 && g_ascii_strncasecmp(start, "averagePlayerLevel", 18) == 0))
      return(c->level);

    if(len == 15 && g_ascii_strncasecmp(start, "numberOfPlayers", 15) == 0)
      return(1.0);

    return(0.0);  // unknown identifier
  }

  return(0.0);
}

static double
eval_term(EvalCtx *c)
{
  double v = eval_factor(c);

  for(;;)
  {
    eval_skipws(c);
    char op = *c->p;
    if(op != '*' && op != '/')
      break;
    c->p++;
    double rhs = eval_factor(c);
    if(op == '*')
      v *= rhs;
    else
      v = (rhs != 0.0) ? v / rhs : 0.0;
  }

  return(v);
}

static double
eval_expr(EvalCtx *c)
{
  double v = eval_term(c);

  for(;;)
  {
    eval_skipws(c);
    char op = *c->p;
    if(op != '+' && op != '-')
      break;
    c->p++;
    double rhs = eval_term(c);
    if(op == '+')
      v += rhs;
    else
      v -= rhs;
  }

  return(v);
}

double
db_loot_eval(const char *expr, double level)
{
  if(!expr || !expr[0])
    return(0.0);

  EvalCtx c = { .p = expr, .level = level };

  return(eval_expr(&c));
}

// -- resolver ---------------------------------------------------------------

static bool
is_loot_table_class(const char *cls)
{
  return(cls &&
    (g_ascii_strcasecmp(cls, "LootMasterTable") == 0 ||
     g_ascii_strcasecmp(cls, "LootItemTable_FixedWeight") == 0 ||
     g_ascii_strcasecmp(cls, "LootItemTable_DynWeight") == 0 ||
     g_ascii_strcasecmp(cls, "FixedItemContainer") == 0 ||
     g_ascii_strcasecmp(cls, "FixedItemLoot") == 0));
}

// Accumulate chance for an item path into out (out owns the key/value).
static void
loot_add(GHashTable *out, const char *item_path_lc, double chance)
{
  double *cur = g_hash_table_lookup(out, item_path_lc);

  if(cur)
  {
    *cur += chance;
  }
  else
  {
    double *v = g_malloc(sizeof(double));
    *v = chance;
    g_hash_table_insert(out, g_strdup(item_path_lc), v);
  }
}

static void resolve_table(DbLootCtx *ctx, const char *path, int level,
                          double mult, GHashTable *out, int depth);

// Follow one child reference: recurse if it is a table, else add it as an item.
static void
follow_child(DbLootCtx *ctx, const char *path, int level, double mult,
             GHashTable *out, int depth)
{
  if(!path || !path[0] || depth > DB_LOOT_MAX_DEPTH)
    return;

  TQArzRecordData *d = ctx_read(ctx, path);

  if(!d)
    return;

  if(is_loot_table_class(rec_str(d, "Class")))
  {
    resolve_table(ctx, path, level, mult, out, depth);
  }
  else
  {
    char *lc = lc_path(path);
    loot_add(out, lc, mult);
    g_free(lc);
  }
}

// LootMasterTable / LootItemTable_FixedWeight: weighted lootName{i} entries.
static void
handle_weighted(DbLootCtx *ctx, TQArzRecordData *d, int level, double mult,
                GHashTable *out, int depth)
{
  double sum = rec_num(d, "lootWeight");  // optional bare entry

  for(int i = 1; i <= 30; i++)
  {
    char k[32];
    g_snprintf(k, sizeof(k), "lootWeight%d", i);
    sum += rec_num(d, k);
  }

  if(sum <= 0.0)
    return;

  const char *bare = rec_str(d, "lootName");
  double bw = rec_num(d, "lootWeight");
  if(bare && bw > 0.0)
    follow_child(ctx, bare, level, mult * (bw / sum), out, depth + 1);

  for(int i = 1; i <= 30; i++)
  {
    char wk[32], nk[32];
    g_snprintf(wk, sizeof(wk), "lootWeight%d", i);
    g_snprintf(nk, sizeof(nk), "lootName%d", i);

    double w = rec_num(d, wk);
    if(w <= 0.0)
      continue;

    const char *name = rec_str(d, nk);
    if(!name)
      continue;

    follow_child(ctx, name, level, mult * (w / sum), out, depth + 1);
  }
}

// LootItemTable_DynWeight: itemNames[] gated by a level window, weighted by a
// bell-curve slope keyed on (itemLevel - targetLevel).
static void
handle_dynweight(DbLootCtx *ctx, TQArzRecordData *d, int level, double mult,
                 GHashTable *out, int depth)
{
  (void)depth;

  // No level reference (quest rewards): tqdb's level equations raise and the
  // table contributes nothing.
  if(level == DB_LOOT_NO_LEVEL)
    return;

  TQVariable *names = rec_var(d, "itemNames");
  if(!names || names->type != TQ_VAR_STRING || names->count == 0)
    return;

  const char *minEq = rec_str(d, "minItemLevelEquation");
  const char *maxEq = rec_str(d, "maxItemLevelEquation");
  const char *tgtEq = rec_str(d, "targetLevelEquation");
  if(!minEq || !maxEq || !tgtEq)
    return;  // tqdb: logs "Missing parentLevel" and returns

  double minL = db_loot_eval(minEq, level);
  double maxL = db_loot_eval(maxEq, level);
  double tgtL = db_loot_eval(tgtEq, level);

  TQVariable *slope = rec_var(d, "bellSlope");
  int slope_n = (slope && (slope->type == TQ_VAR_FLOAT || slope->type == TQ_VAR_INT))
                ? (int)slope->count : 0;
  double dflt = rec_num(d, "defaultWeight");

  // First pass: per-item adjusted weight (parallel to a path list).
  GPtrArray *paths = g_ptr_array_new();
  GArray    *weights = g_array_new(FALSE, FALSE, sizeof(double));

  for(uint32_t i = 0; i < names->count; i++)
  {
    const char *ip = names->value.str ? names->value.str[i] : NULL;
    if(!ip || !ip[0])
      continue;

    TQArzRecordData *id = ctx_read(ctx, ip);
    if(!id)
      continue;

    bool found = false;
    int  ilvl  = arz_record_get_int(id, "itemLevel", 0, &found);

    if(!found || ilvl < minL || ilvl > maxL)
      continue;

    int target = (int)(ilvl - tgtL);
    double adj = 1.0;
    if(slope_n > 0)
    {
      int idx;
      if(target >= 0)
        idx = (target < slope_n) ? target : slope_n - 1;
      else
      {
        idx = slope_n + target;  // tqdb relies on Python negative indexing
        if(idx < 0)
          idx = 0;
      }
      adj = var_num_at(slope, (uint32_t)idx);
    }

    double w = dflt * adj;
    g_ptr_array_add(paths, (gpointer)ip);
    g_array_append_val(weights, w);
  }

  double sum = 0.0;
  for(guint i = 0; i < weights->len; i++)
    sum += g_array_index(weights, double, i);

  if(sum > 0.0)
  {
    for(guint i = 0; i < paths->len; i++)
    {
      char *lc = lc_path((const char *)g_ptr_array_index(paths, i));
      loot_add(out, lc, mult * (g_array_index(weights, double, i) / sum));
      g_free(lc);
    }
  }

  g_ptr_array_free(paths, TRUE);
  g_array_free(weights, TRUE);
}

// FixedItemContainer: a single 'tables' reference passed straight through.
static void
handle_container(DbLootCtx *ctx, TQArzRecordData *d, int level, double mult,
                 GHashTable *out, int depth)
{
  const char *t = rec_str_at(d, "tables", 0);
  if(t)
    follow_child(ctx, t, level, mult, out, depth + 1);
}

// FixedItemLoot: 6 slots, each with a drop chance and up to 6 weighted tables,
// scaled by the average spawn count.
static void
handle_fixeditemloot(DbLootCtx *ctx, TQArzRecordData *d, int level, double mult,
                     GHashTable *out, int depth)
{
  const char *minEq = rec_str(d, "numSpawnMinEquation");
  const char *maxEq = rec_str(d, "numSpawnMaxEquation");
  double spawn = 1.0;
  if(minEq && maxEq)
    spawn = (db_loot_eval(minEq, level) + db_loot_eval(maxEq, level)) / 2.0;

  for(int slot = 1; slot <= 6; slot++)
  {
    char ck[24];
    g_snprintf(ck, sizeof(ck), "loot%dChance", slot);
    double chance = rec_num(d, ck);
    if(chance <= 0.0)
      continue;

    double sum = 0.0;
    for(int i = 1; i <= 6; i++)
    {
      char wk[24];
      g_snprintf(wk, sizeof(wk), "loot%dWeight%d", slot, i);
      sum += rec_num(d, wk);
    }
    if(sum <= 0.0)
      continue;

    for(int i = 1; i <= 6; i++)
    {
      char wk[24], nk[24];
      g_snprintf(wk, sizeof(wk), "loot%dWeight%d", slot, i);
      g_snprintf(nk, sizeof(nk), "loot%dName%d", slot, i);

      double w = rec_num(d, wk);
      if(w <= 0.0)
        continue;

      const char *name = rec_str_at(d, nk, 0);
      if(!name)
        continue;

      follow_child(ctx, name, level, mult * (w / sum) * chance * spawn,
                   out, depth + 1);
    }
  }
}

static void
resolve_table(DbLootCtx *ctx, const char *path, int level, double mult,
              GHashTable *out, int depth)
{
  if(depth > DB_LOOT_MAX_DEPTH)
    return;

  TQArzRecordData *d = ctx_read(ctx, path);
  if(!d)
    return;

  const char *cls = rec_str(d, "Class");

  if(cls && (g_ascii_strcasecmp(cls, "LootMasterTable") == 0 ||
             g_ascii_strcasecmp(cls, "LootItemTable_FixedWeight") == 0))
    handle_weighted(ctx, d, level, mult, out, depth);
  else if(cls && g_ascii_strcasecmp(cls, "LootItemTable_DynWeight") == 0)
    handle_dynweight(ctx, d, level, mult, out, depth);
  else if(cls && g_ascii_strcasecmp(cls, "FixedItemContainer") == 0)
    handle_container(ctx, d, level, mult, out, depth);
  else if(cls && g_ascii_strcasecmp(cls, "FixedItemLoot") == 0)
    handle_fixeditemloot(ctx, d, level, mult, out, depth);
}

GHashTable *
db_loot_resolve_ctx(DbLootCtx *ctx, const char *table_path, int level)
{
  if(!ctx || !table_path || !table_path[0])
    return(NULL);

  TQArzRecordData *d = ctx_read(ctx, table_path);
  if(!d || !is_loot_table_class(rec_str(d, "Class")))
    return(NULL);

  GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_free);
  resolve_table(ctx, table_path, level, 1.0, out, 0);

  return(out);
}

GHashTable *
db_loot_resolve(TQArzFile *arz, const char *table_path, int level)
{
  if(!arz)
    return(NULL);

  DbLootCtx *ctx = db_loot_ctx_new(arz);
  GHashTable *out = db_loot_resolve_ctx(ctx, table_path, level);
  db_loot_ctx_free(ctx);

  return(out);
}
