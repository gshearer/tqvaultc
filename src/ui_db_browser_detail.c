// Database Browser: detail-pane renderers (set/affix/skill/consumable/
// creature/quest + cross-references).  Shared decls in the internal header.

#include "ui_db_browser_internal.h"

#include "item_stats.h"
#include "asset_lookup.h"
#include "texture.h"
#include "creature_thumbs.h"
#include "vault.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// -- Detail pane ------------------------------------------------------------

// Show the given pixbuf in the large detail-pane picture (NULL clears it).
// Borrows pb -- the caller retains ownership.
static void
db_detail_set_pixbuf(DbBrowserState *st, GdkPixbuf *pb)
{
  if(pb)
  {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    G_GNUC_END_IGNORE_DEPRECATIONS

    gtk_picture_set_paintable(GTK_PICTURE(st->detail_pic), GDK_PAINTABLE(tex));
    g_object_unref(tex);
  }
  else
  {
    gtk_picture_set_paintable(GTK_PICTURE(st->detail_pic), NULL);
  }
}

// Set the large detail-pane icon from a DBR path (NULL or no texture clears it).
static void
db_detail_set_icon(DbBrowserState *st, const char *path, uint32_t var1)
{
  GdkPixbuf *pb = path ? load_item_texture(st->widgets, path, var1) : NULL;

  db_detail_set_pixbuf(st, pb);
  if(pb)
    g_object_unref(pb);
}

// True for the int routing flags (offensive*Global, *XOR) that aren't real
// stats, so they don't establish a set-bonus tier.
static bool
db_is_routing_flag(const char *name)
{
  size_t n = strlen(name);

  return((n >= 6 && strcasecmp(name + n - 6, "Global") == 0) ||
         (n >= 3 && strcasecmp(name + n - 3, "XOR") == 0));
}

// Tier depth of a set = the longest numeric stat array (excluding routing
// flags).  The array is indexed by (set items - 1), so index t is the bonus
// for wearing t+1 pieces.  Returns 0 when the record carries no stat arrays.
static int
db_set_tier_depth(TQArzRecordData *data)
{
  int depth = 0;

  if(!data)
    return(0);

  for(uint32_t v = 0; v < data->num_vars; v++)
  {
    TQVariable *var = &data->vars[v];

    if((var->type == TQ_VAR_INT || var->type == TQ_VAR_FLOAT) &&
       !db_is_routing_flag(var->name) && (int)var->count > depth)
      depth = (int)var->count;
  }

  return(depth);
}

// Build the set detail markup (text only, no widget/icon access): the set name,
// its members (each in its own rarity color) and the tiered set bonuses.  Bonus
// tiers reuse add_stats_from_record at shard_index = (set items - 1); empty
// tiers (the one-piece slot and any all-zero rows) are skipped.
static void
build_set_markup(DbBrowserState *st, DbBrowseItem *bi, char *out, size_t outsz)
{
  TQTranslation *tr = st->tr;
  TQArzRecordData *set_data = asset_get_dbr(bi->path);
  BufWriter w;

  buf_init(&w, out, outsz);

  // Set name.
  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='#40FF40'><b>%s</b></span>\n", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  // Members, each in its own rarity color.
  TQVariable *members = set_data ? arz_record_get_var(set_data, INT_setMembers)
                                 : NULL;

  int member_count = 0;

  if(members && members->type == TQ_VAR_STRING)
  {
    buf_write(&w, "\n<span color='#B0B0B0'>Set Members:</span>\n");

    for(uint32_t m = 0; m < members->count; m++)
    {
      const char *mp = members->value.str[m];
      const char *mname = db_set_member_name(st, mp);

      if(!mname)
        continue;

      member_count++;

      const char *color = get_item_color(mp, NULL, NULL);
      char *e_member = escape_markup(mname);
      char *e_href = escape_markup(mp);

      // Clickable: jump to this member item (Phase 7 cross-pane navigation).
      buf_write(&w, "    <a href='i:%s'><span color='%s'>%s</span></a>\n",
                e_href ? e_href : "",
                color ? color : "white", e_member ? e_member : "");
      if(e_member)
        free(e_member);
      if(e_href)
        free(e_href);
    }
  }

  // Tiered set bonuses.  The stat arrays are indexed by (set items - 1); a set
  // with only scalar bonuses applies them to the full set, so the piece count
  // falls back to the member count.  Each tier reuses add_stats_from_record at
  // shard_index = (pieces - 1); empty tiers and ones identical to the tier
  // below (nothing new granted) are skipped.
  int depth = db_set_tier_depth(set_data);
  int full_pieces = (depth > 1) ? depth : member_count;
  bool any_tier = false;
  char prev_buf[8192] = "";

  for(int p = 2; p <= full_pieces; p++)
  {
    char tier_buf[8192];
    BufWriter tw;

    buf_init(&tw, tier_buf, sizeof(tier_buf));
    add_stats_from_record(bi->path, tr, &tw, "#40FF40", p - 1);

    if(tier_buf[0] == '\0' || strcmp(tier_buf, prev_buf) == 0)
      continue;  // no bonus, or nothing new granted at this many pieces

    memcpy(prev_buf, tier_buf, sizeof(prev_buf));

    if(!any_tier)
    {
      buf_write(&w, "\n<span color='#B0B0B0'>Set Bonuses:</span>\n");
      any_tier = true;
    }

    buf_write(&w, "\n<span color='#9F9F9F'>Required Set Items: %d</span>\n", p);
    buf_write(&w, "%s", tier_buf);
  }
}

// Rebuild the detail pane for an item set (markup + first-member icon).
static void
update_detail_set(DbBrowserState *st, DbBrowseItem *bi)
{
  char markup[32768];

  build_set_markup(st, bi, markup, sizeof(markup));
  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, bi->icon_path, 0);
}

// -- Relic / charm / artifact extra detail (browser-only, tq-db style) ------

// Resolve a best-effort display name for an item record (translated
// description or itemNameTag, else a path-derived name).  Returns a malloc'd
// string the caller frees with free().
static char *
db_consumable_name(DbBrowserState *st, const char *path)
{
  const char *tag = dbr_get_string(path, "description");

  if(!tag || !tag[0])
    tag = dbr_get_string(path, "itemNameTag");

  if(tag && tag[0])
  {
    const char *name = translation_get(st->tr, tag);

    if(name && name[0] && strcmp(name, tag) != 0)
      return(strdup(name));
  }

  return(pretty_name_from_path(path));
}

// Find the completion-bonus randomizer table for a relic/charm (its own
// `bonusTableName`) or an artifact (its formula's `artifactBonusTableName`).
// Mirrors get_bonus_table_path() in ui_context_menu.c.  Returns an internal
// pointer (do not free), or NULL.
static const char *
db_bonus_table_path(const char *item_path)
{
  if(item_is_artifact(item_path))
  {
    const char *sep = strrchr(item_path, '\\');

    if(!sep)
      return(NULL);

    int dir_len = (int)(sep - item_path);
    const char *fname = sep + 1;
    int name_len = (int)strlen(fname);

    if(name_len > 4 && strcasecmp(fname + name_len - 4, ".dbr") == 0)
      name_len -= 4;

    char formula[1024];

    snprintf(formula, sizeof(formula), "%.*s\\arcaneformulae\\%.*s_formula.dbr",
             dir_len, item_path, name_len, fname);

    return(dbr_get_string(formula, "artifactBonusTableName"));
  }

  return(dbr_get_string(item_path, "bonusTableName"));
}

// True for one-shot scroll items.
static bool
db_is_scroll(const char *path)
{
  const char *cls = dbr_get_string(path, "Class");

  return(cls && strcasecmp(cls, "OneShot_Scroll") == 0);
}

// Append the full list of possible completion bonuses (each with its drop
// chance) for a relic/charm/artifact.  Enumerates the randomizer table's
// SPARSE randomizerName/Weight pairs, dedupes by path (summing weights) and
// renders each as "<chance>%  <name>" sorted most-likely first.
static void
db_append_bonus_table(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  const char *table_path = db_bonus_table_path(item_path);

  if(!table_path || !table_path[0])
    return;

  TQArzRecordData *table = asset_get_dbr(table_path);

  if(!table)
    return;

  const char *paths[256] = { 0 };
  float weights[256] = { 0 };

  for(uint32_t v = 0; v < table->num_vars; v++)
  {
    TQVariable *var = &table->vars[v];

    if(!var->name)
      continue;

    if(strncasecmp(var->name, "randomizerName", 14) == 0 &&
       var->type == TQ_VAR_STRING && var->count > 0 && var->value.str &&
       var->value.str[0] && var->value.str[0][0])
    {
      int idx = atoi(var->name + 14);

      if(idx >= 0 && idx < 256)
        paths[idx] = var->value.str[0];
    }
    else if(strncasecmp(var->name, "randomizerWeight", 16) == 0)
    {
      int idx = atoi(var->name + 16);
      float wt = 0;

      if(var->type == TQ_VAR_INT && var->count > 0 && var->value.i32)
        wt = (float)var->value.i32[0];
      else if(var->type == TQ_VAR_FLOAT && var->count > 0 && var->value.f32)
        wt = var->value.f32[0];

      if(idx >= 0 && idx < 256)
        weights[idx] = wt;
    }
  }

  // Dedup by path (sum weights); accumulate the grand total for percentages.
  const char *uniq_path[256];
  float uniq_w[256];
  int n = 0;
  float total = 0;

  for(int i = 0; i < 256; i++)
  {
    if(!paths[i] || weights[i] <= 0)
      continue;

    total += weights[i];

    int found = -1;

    for(int j = 0; j < n; j++)
      if(strcasecmp(uniq_path[j], paths[i]) == 0)
      {
        found = j;
        break;
      }

    if(found >= 0)
      uniq_w[found] += weights[i];
    else
    {
      uniq_path[n] = paths[i];
      uniq_w[n] = weights[i];
      n++;
    }
  }

  if(n == 0)
    return;

  // Sort by descending weight (most likely first).
  for(int i = 0; i < n - 1; i++)
    for(int j = i + 1; j < n; j++)
      if(uniq_w[j] > uniq_w[i])
      {
        const char *tp = uniq_path[i]; uniq_path[i] = uniq_path[j]; uniq_path[j] = tp;
        float tw = uniq_w[i]; uniq_w[i] = uniq_w[j]; uniq_w[j] = tw;
      }

  buf_write(w, "\n<span color='#B0B0B0'>Possible Completion Bonuses:</span>\n");

  for(int i = 0; i < n; i++)
  {
    float pct = total > 0 ? uniq_w[i] / total * 100.0f : 0;
    char *name = NULL;
    const char *desc = dbr_get_string(uniq_path[i], "description");

    if(desc && desc[0])
    {
      const char *t = translation_get(st->tr, desc);

      if(t && t[0] && strcmp(t, desc) != 0)
        name = strdup(t);
    }

    if(!name)
      name = item_bonus_stat_summary(uniq_path[i], st->tr);
    if(!name)
      name = pretty_name_from_path(uniq_path[i]);

    char *e = escape_markup(name ? name : "");

    buf_write(w, "<span color='#C1A472'>    %.1f%%  %s</span>\n", pct, e ? e : "");
    if(e)
      free(e);
    free(name);
  }
}

// Append an artifact's arcane-formula reagents (the recipe).
static void
db_append_artifact_formula(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  const char *sep = strrchr(item_path, '\\');

  if(!sep)
    return;

  int dir_len = (int)(sep - item_path);
  const char *fname = sep + 1;
  int name_len = (int)strlen(fname);

  if(name_len > 4 && strcasecmp(fname + name_len - 4, ".dbr") == 0)
    name_len -= 4;

  char formula[1024];

  snprintf(formula, sizeof(formula), "%.*s\\arcaneformulae\\%.*s_formula.dbr",
           dir_len, item_path, name_len, fname);

  TQArzRecordData *fd = asset_get_dbr(formula);

  if(!fd)
    return;

  const char *keys[] = { "reagent1BaseName", "reagent2BaseName", "reagent3BaseName" };
  bool header = false;

  for(int i = 0; i < 3; i++)
  {
    const char *rp = record_get_string_fast(fd, arz_intern(keys[i]));

    if(!rp || !rp[0])
      continue;

    if(!header)
    {
      buf_write(w, "\n<span color='#B0B0B0'>Arcane Formula — Reagents:</span>\n");
      header = true;
    }

    char *name = db_consumable_name(st, rp);
    char *e = escape_markup(name ? name : "");

    buf_write(w, "<span color='#C1A472'>    %s</span>\n", e ? e : "");
    if(e)
      free(e);
    free(name);
  }
}

// Append the browser-only, tq-db-style extra detail for a relic/charm
// (per-shard progression + completion-bonus table) or artifact (formula
// reagents + completion-bonus table).
static void
db_append_consumable_detail(DbBrowserState *st, const char *path, BufWriter *w)
{
  TQTranslation *tr = st->tr;

  if(item_is_relic_or_charm(path))
  {
    // Per-shard progression: the relic's stat arrays are indexed by shard-1.
    int max_shards = relic_max_shards(path);

    if(max_shards > 1)
    {
      char prev[4096] = "";
      bool header = false;

      for(int s = 1; s <= max_shards; s++)
      {
        char sb[4096];
        BufWriter sw;

        buf_init(&sw, sb, sizeof(sb));
        add_stats_from_record(path, tr, &sw, "#C1A472", s - 1);

        if(sb[0] == '\0' || strcmp(sb, prev) == 0)
          continue;

        memcpy(prev, sb, sizeof(prev));

        if(!header)
        {
          buf_write(w, "\n<span color='#B0B0B0'>Shard Progression:</span>\n");
          header = true;
        }

        buf_write(w, "\n<span color='#9F9F9F'>%d Shard%s:</span>\n", s, s > 1 ? "s" : "");
        buf_write(w, "%s", sb);
      }
    }

    db_append_bonus_table(st, path, w);
  }
  else if(item_is_artifact(path))
  {
    db_append_artifact_formula(st, path, w);
    db_append_bonus_table(st, path, w);
  }
  else if(db_is_scroll(path))
  {
    // The granted-skill section of the shared tooltip keys off itemSkillName;
    // scrolls use skillName, so render the granted skill here (name +
    // description + first-tier properties), mirroring that logic.
    const char *skill = dbr_get_string(path, "skillName");

    if(skill && skill[0])
    {
      TQArzRecordData *sd = asset_get_dbr(skill);
      const char *buff = sd ? record_get_string_fast(sd, INT_buffSkillName) : NULL;
      const char *effect = (buff && buff[0]) ? buff : skill;
      TQArzRecordData *ed = asset_get_dbr(effect);

      const char *ntag = ed ? record_get_string_fast(ed, INT_skillDisplayName) : NULL;

      if(!ntag && sd)
        ntag = record_get_string_fast(sd, INT_skillDisplayName);

      buf_write(w, "\n<span color='#B0B0B0'>Grants Skill:</span>\n");

      const char *sname = ntag ? translation_get(tr, ntag) : NULL;

      if(sname && sname[0])
      {
        char *e = escape_markup(sname);

        buf_write(w, "<span color='#DAA520'>%s</span>\n", e ? e : "");
        if(e)
          free(e);
      }

      const char *dtag = ed ? record_get_string_fast(ed, INT_skillBaseDescription) : NULL;

      if(!dtag && sd)
        dtag = record_get_string_fast(sd, INT_skillBaseDescription);

      if(dtag)
      {
        const char *dt = translation_get(tr, dtag);

        if(dt && dt[0])
        {
          char *e = escape_markup(dt);

          buf_write(w, "<span color='#DAA520'>%s</span>\n", e ? e : "");
          if(e)
            free(e);
        }
      }

      add_stats_from_record(effect, tr, w, "#DAA520", 0);
    }
  }
}

// Replace every occurrence of `from` with `to`.  Returns a newly g_malloc'd
// string (caller g_free's).  Used for the skill-tooltip "Intelligence" ->
// "Intellect" swap (skills phrase that attribute differently from item stats).
static char *
db_str_replace(const char *s, const char *from, const char *to)
{
  GString *g = g_string_new(NULL);
  size_t fl = strlen(from);

  for(const char *p = s; *p;)
  {
    if(strncmp(p, from, fl) == 0)
    {
      g_string_append(g, to);
      p += fl;
    }
    else
    {
      g_string_append_c(g, *p++);
    }
  }

  return(g_string_free(g, FALSE));
}

// Append a skill's leveled properties to the detail buffer.  Stat arrays are
// indexed by (level - 1); each distinct, non-empty tier is shown via
// add_stats_from_record (which follows buff/pet/skill refs).  Masteries scale
// linearly across many levels, so only their first and max tiers are shown;
// regular skills show every distinct tier.  eff_path is the description-bearing
// record (db_skill_effective_path).
static void
db_append_skill_levels(DbBrowserState *st, const char *eff_path, int max_level,
                       bool is_mastery, BufWriter *w)
{
  TQTranslation *tr = st->tr;
  const char *WHITE = "#E0E0E0";

  if(max_level < 1)
    max_level = 1;

  // Masteries: just the two endpoints (the bonus is uniform between them).
  int levels[64];
  int nlevels = 0;

  if(is_mastery && max_level > 2)
  {
    levels[nlevels++] = 1;
    levels[nlevels++] = max_level;
  }
  else
  {
    for(int L = 1; L <= max_level && nlevels < 64; L++)
      levels[nlevels++] = L;
  }

  char prev[8192] = "";
  bool any = false;

  for(int i = 0; i < nlevels; i++)
  {
    int L = levels[i];
    char lb[8192];
    BufWriter lw;

    buf_init(&lw, lb, sizeof(lb));
    add_stats_from_record(eff_path, tr, &lw, WHITE, L - 1);

    if(lb[0] == '\0' || strcmp(lb, prev) == 0)
      continue;  // no properties, or unchanged from the previous tier

    memcpy(prev, lb, sizeof(prev));

    if(!any)
    {
      buf_write(w, "\n<span color='#B0B0B0'>Properties by Level:</span>\n");
      any = true;
    }

    buf_write(w, "\n<span color='#FFD200'>Level %d:</span>\n", L);
    buf_write(w, "%s", lb);
  }
}

// Build the mastery/skill detail markup (text only): its name (skill gold),
// description, max level + mastery-level requirement, and its leveled
// properties.  Reuses the same DBR machinery as the visual skill manager
// (add_stats_from_record over the effective skill record).
static void
build_skill_markup(DbBrowserState *st, DbBrowseItem *bi, char *out, size_t outsz)
{
  TQTranslation *tr = st->tr;
  char markup[32768];
  BufWriter w;

  buf_init(&w, markup, sizeof(markup));

  // Name.
  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='%s'><b>%s</b></span>\n",
            bi->color ? bi->color : "#DAA520", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  const char *eff = db_skill_effective_path(bi->path, 0);

  if(!eff)
    eff = bi->path;

  // Description (the buff/pet/toggle shell keeps it in the effective record).
  const char *dtag = dbr_get_string(eff, "skillBaseDescription");

  if(dtag && dtag[0])
  {
    const char *d = translation_get(tr, dtag);

    if(d && d[0] && strcmp(d, dtag) != 0)
    {
      char *e = escape_markup(d);

      buf_write(&w, "<span color='#C8C8C8'>%s</span>\n", e ? e : "");
      if(e)
        free(e);
    }
  }

  // Max level + mastery-level requirement.
  int max_level = db_skill_max_level(bi->path, 0);

  if(max_level < 1)
    max_level = 1;

  bool is_mastery = strcasestr(bi->path, "mastery") != NULL;

  buf_write(&w, "\n<span color='#9F9F9F'>Max Level: %d</span>\n", max_level);

  TQArzRecordData *sd = asset_get_dbr(bi->path);
  int mreq = sd ? arz_record_get_int(sd, "skillMasteryLevelRequired", 0, NULL) : 0;

  if(mreq > 0)
    buf_write(&w, "<span color='#9F9F9F'>Required Mastery Level: %d</span>\n", mreq);

  // Leveled properties.
  db_append_skill_levels(st, eff, max_level, is_mastery, &w);

  // Skills say "Intellect" where item-stat formatting says "Intelligence".
  char *fixed = db_str_replace(markup, "Intelligence", "Intellect");

  g_strlcpy(out, fixed, outsz);
  g_free(fixed);
}

// Rebuild the detail pane for a mastery/skill (icon = the skill's .tex bitmap).
static void
update_detail_skill(DbBrowserState *st, DbBrowseItem *bi)
{
  char markup[32768];

  build_skill_markup(st, bi, markup, sizeof(markup));
  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);

  // Icon: the skill's .tex bitmap.
  GdkPixbuf *pb = bi->icon_path ? texture_load(bi->icon_path) : NULL;

  db_detail_set_pixbuf(st, pb);
  if(pb)
    g_object_unref(pb);
}

// Build the prefix/suffix affix detail markup (text only): its name (in rarity
// color), prefix/suffix kind + classification, level requirement, the granted
// properties (via add_stats_from_record, which also follows pet/skill bonuses)
// and the equipment types it can roll on.  Stat-less affixes (the Tinkerer's
// extra relic slot) fall back to their special text.
static void
build_affix_markup(DbBrowserState *st, DbBrowseItem *bi, char *out, size_t outsz)
{
  TQTranslation *tr = st->tr;
  BufWriter w;

  buf_init(&w, out, outsz);

  // Name in its rarity color.
  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='%s'><b>%s</b></span>\n",
            bi->color ? bi->color : "white", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  // Kind (prefix/suffix) + classification, derived from the record.
  const char *kind = path_contains_ci(bi->path, "\\suffix\\") ? "Suffix" : "Prefix";
  const char *cls = dbr_get_string(bi->path, "itemClassification");

  if(cls && cls[0])
    buf_write(&w, "<span color='#9F9F9F'>%s — %s</span>\n", kind, cls);
  else
    buf_write(&w, "<span color='#9F9F9F'>%s</span>\n", kind);

  // Level requirement, if any.
  int req[4];

  item_record_requirements(bi->path, req);
  if(req[0] > 0)
    buf_write(&w, "<span color='#9F9F9F'>Required Level: %d</span>\n", req[0]);

  // Properties: one block per distinct variant roll (a merged affix such as
  // "Allfather's" carries several).  add_stats_from_record follows petBonusName
  // and skill grants.  Distinct blocks are separated as alternatives.
  buf_write(&w, "\n<span color='#B0B0B0'>Properties:</span>\n");

  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  int shown = 0;

  for(char **vp = bi->variants; vp && *vp; vp++)
  {
    char props[8192];
    BufWriter pw;

    buf_init(&pw, props, sizeof(props));
    add_stats_from_record(*vp, tr, &pw, "#00A3FF", 0);

    if(props[0] == '\0' || g_hash_table_contains(seen, props))
      continue;  // empty, or identical to a variant already shown

    g_hash_table_add(seen, g_strdup(props));

    if(shown > 0)
      buf_write(&w, "<span color='#707070'>   — or —</span>\n");
    buf_write(&w, "%s", props);
    shown++;
  }

  g_hash_table_destroy(seen);

  if(shown == 0)
  {
    // Stat-less affix (e.g. "of the Tinkerer": engine-level extra relic slot).
    const char *rtag = dbr_get_string(bi->path, "lootRandomizerName");
    const char *special = (rtag && strcasecmp(rtag, "x3tagSuffix01") == 0)
                          ? translation_get(tr, "x3tagExtraRelic") : NULL;

    if(special && special[0])
    {
      char *e = escape_markup(special);

      buf_write(&w, "<span color='#00A3FF'>%s</span>\n", e ? e : "");
      if(e)
        free(e);
    }
    else
    {
      buf_write(&w, "<span color='#808080'>(no direct properties)</span>\n");
    }
  }

  // Equipment types the affix can roll on.
  if(bi->equip_types && bi->equip_types[0])
  {
    char *e = escape_markup(bi->equip_types);

    buf_write(&w, "\n<span color='#B0B0B0'>Can appear on:</span>\n");
    buf_write(&w, "<span color='#C8C8C8'>%s</span>\n", e ? e : "");
    if(e)
      free(e);
  }
}

// Rebuild the detail pane for a prefix/suffix affix (affixes have no icon).
static void
update_detail_affix(DbBrowserState *st, DbBrowseItem *bi)
{
  char markup[32768];

  build_affix_markup(st, bi, markup, sizeof(markup));
  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, NULL, 0);
}

// -- Creature / quest detail + cross-reference (Phase 6) --------------------

// Format a per-difficulty chance triple compactly into out (e.g.
// "N 12.30%  E 4.50%"), showing only the non-zero tiers.
static void
db_fmt_chances(const double ch[3], char *out, size_t outsz)
{
  static const char *D[3] = { "N", "E", "L" };
  BufWriter w;

  buf_init(&w, out, outsz);
  for(int i = 0; i < 3; i++)
    if(ch[i] > 0.0)
      buf_write(&w, "%s%s %.2f%%", (w.pos > 0 ? "  " : ""), D[i], ch[i]);

  if(out[0] == '\0')
    snprintf(out, outsz, "—");
}

// Value rank for ordering a drop/reward list: higher == more desirable, so the
// most valuable items are the ones that survive a "+N more" truncation.  Based
// on the game's own itemClassification, with the special consumable/craftable
// types (relics, charms, artifacts, arcane formulae, scrolls — all classed
// "Common") promoted above mundane gear.
static int
db_item_value_rank(const char *item_lc)
{
  if(!item_lc)
    return(0);

  const char *cls = get_record_variable_string(item_lc, INT_itemClassification);

  if(cls)
  {
    if(strcasecmp(cls, "Legendary") == 0) return(7);
    if(strcasecmp(cls, "Epic") == 0)      return(6);
    if(strcasecmp(cls, "Rare") == 0)      return(5);
  }

  if(path_contains_ci(item_lc, "\\artifacts\\") ||
     path_contains_ci(item_lc, "\\arcaneformulae\\") ||
     path_contains_ci(item_lc, "\\relics\\") ||
     path_contains_ci(item_lc, "\\charms\\") ||
     path_contains_ci(item_lc, "\\scrolls\\"))
    return(4);

  if(cls)
  {
    if(strcasecmp(cls, "Quest") == 0)    return(3);
    if(strcasecmp(cls, "Magical") == 0)  return(2);
  }

  return(1);  // Common / Broken / unknown
}

// True if a dropped item is hidden from a creature's drop list.  We drop the
// clutter that buries the worthwhile loot:
//   - health / energy (mana) potions;
//   - mundane common (white) gear — get_item_color() returns "white" only for
//     common gear with no affixes that isn't one of the special path types
//     ranked above.
static bool
db_drop_is_excluded(const char *item_lc)
{
  if(path_contains_ci(item_lc, "\\oneshot\\potionhealth") ||
     path_contains_ci(item_lc, "\\oneshot\\potionmana"))
    return(true);

  const char *col = get_item_color(item_lc, NULL, NULL);

  return(col && strcmp(col, "white") == 0);
}

// One row to be sorted by db_append_item_rows: the source entry plus its
// precomputed value rank and best per-difficulty drop chance.
typedef struct {
  DbItemChance *ic;
  int           rank;
  double        best;
} DropSortRow;

static int
db_drop_sort_cmp(const void *a, const void *b)
{
  const DropSortRow *x = a;
  const DropSortRow *y = b;

  if(x->rank != y->rank)
    return(y->rank - x->rank);     // most valuable first
  if(x->best < y->best) return(1); // then best drop chance first (tiebreak)
  if(x->best > y->best) return(-1);
  return(0);
}

// Append a list of item rows (name in rarity color + chances) to w, sorted by
// item value (rarity) and capped at `limit` entries with a "+N more" note.
// Sorting by value means that when the list is truncated, the most valuable
// drops stay visible rather than whatever happened to have the highest drop
// chance.  A private copy is sorted so the caller's array (e.g. a creature's
// shared drop index) keeps its own order.
static void
db_append_item_rows(DbBrowserState *st, BufWriter *w, GPtrArray *paths,
                    int limit)
{
  guint n = paths->len;

  if(n == 0)
    return;

  DropSortRow *rows = g_new(DropSortRow, n);

  for(guint i = 0; i < n; i++)
  {
    DbItemChance *ic = g_ptr_array_index(paths, i);

    rows[i].ic   = ic;
    rows[i].rank = db_item_value_rank(ic->item_lc);
    rows[i].best = MAX(ic->chance[0], MAX(ic->chance[1], ic->chance[2]));
  }

  qsort(rows, n, sizeof(*rows), db_drop_sort_cmp);

  guint shown = (n > (guint)limit) ? (guint)limit : n;

  for(guint i = 0; i < shown; i++)
  {
    DbItemChance *ic = rows[i].ic;
    char *name = db_item_display_name(st, ic->item_lc);
    const char *color = get_item_color(ic->item_lc, NULL, NULL);
    char *e = escape_markup(name ? name : ic->item_lc);
    char *e_href = escape_markup(ic->item_lc);
    char chbuf[96];

    db_fmt_chances(ic->chance, chbuf, sizeof(chbuf));
    // Clickable: jump to the dropped/rewarded item (Phase 7).
    buf_write(w, "    <a href='i:%s'><span color='%s'>%s</span></a> "
                 "<span color='#808080'>%s</span>\n",
              e_href ? e_href : "", color ? color : "white", e ? e : "", chbuf);
    if(e)
      free(e);
    if(e_href)
      free(e_href);
    free(name);
  }

  if(n > shown)
    buf_write(w, "<span color='#808080'>    … +%u more</span>\n", n - shown);

  g_free(rows);
}

// Read a scalar numeric attribute (int or float) from a loaded record, 0 if
// absent.  Interns the name on each call (cheap: a hash lookup in the intern
// table), so callers can pass plain literals.
static float
db_creature_attr(TQArzRecordData *d, const char *name)
{
  return(d ? dbr_get_float_fast(d, arz_intern(name), 0) : 0.0f);
}

// The creature resistance fields we surface, in the game's display order, each
// with a friendly label.  A field absent from a creature reads 0 and is skipped,
// so this list can stay comprehensive without harm.  The flat-absorption fields
// (defensiveAbsorption/Protection) are intentionally omitted — they aren't %
// resistances.
static const struct { const char *field; const char *label; }
DB_CREATURE_RESISTS[] = {
  { "defensivePhysical",   "Physical" },
  { "defensivePierce",     "Pierce" },
  { "defensiveFire",       "Fire" },
  { "defensiveCold",       "Cold" },
  { "defensiveLightning",  "Lightning" },
  { "defensivePoison",     "Poison" },
  { "defensiveLife",       "Vitality" },
  { "defensiveBleeding",   "Bleeding" },
  { "defensiveElemental",  "Elemental" },
  { "defensiveDisruption", "Skill Disruption" },
  { "defensiveStun",       "Stun" },
  { "defensiveFreeze",     "Freeze" },
  { "defensivePetrify",    "Petrify" },
  { "defensiveTrap",       "Entrapment" },
  { "defensiveConvert",    "Conversion" },
  { "defensiveFear",       "Fear" },
  { "defensiveConfusion",  "Confusion" },
  { "defensiveSleep",      "Sleep" },
  { "defensiveLifeLeach",  "Life Leech" },
  { "defensiveManaLeach",  "Mana Leech" },
  { "defensiveTotalSpeed", "Slow" },
};

// True for utility skill records that aren't "cast" by the creature: passives
// (the GlobalProperties_* stat blocks, Armor_Passive, immunities) all carry a
// Skill_Passive class, so one Class check drops them all.
static bool
db_cast_skill_skip(const char *path)
{
  if(!path || !path[0])
    return(true);

  const char *cls = dbr_get_string(path, "Class");

  return(cls && strncasecmp(cls, "Skill_Passive", 13) == 0);
}

// Append the tq-db-style "Properties" block for a creature: Life/Mana (+ regen),
// Str/Dex/Int, a Resistances line (non-zero % resists) and a Casts line (the
// active skills it uses, resolved to display names).  Levels + race already
// print in the header line above, so they aren't repeated here.
static void
db_append_creature_properties(DbBrowserState *st, DbCreature *c, BufWriter *w)
{
  TQArzRecordData *d = asset_get_dbr(c->path);

  if(!d)
    return;

  buf_write(w, "\n<span color='#B0B0B0'>Properties:</span>\n");

  // Life / Mana, with regen appended when non-zero.
  float life = db_creature_attr(d, "characterLife");
  float mana = db_creature_attr(d, "characterMana");
  float life_rg = db_creature_attr(d, "characterLifeRegen");
  float mana_rg = db_creature_attr(d, "characterManaRegen");

  if(life > 0 || mana > 0)
  {
    buf_write(w, "<span color='#C8C8C8'>Life %d", (int)(life + 0.5f));
    if(life_rg > 0)
      buf_write(w, " (+%d/s)", (int)(life_rg + 0.5f));
    if(mana > 0)
    {
      buf_write(w, " · Mana %d", (int)(mana + 0.5f));
      if(mana_rg > 0)
        buf_write(w, " (+%d/s)", (int)(mana_rg + 0.5f));
    }
    buf_write(w, "</span>\n");
  }

  // Strength / Dexterity / Intelligence.
  int str = (int)(db_creature_attr(d, "characterStrength") + 0.5f);
  int dex = (int)(db_creature_attr(d, "characterDexterity") + 0.5f);
  int intl = (int)(db_creature_attr(d, "characterIntelligence") + 0.5f);

  if(str > 0 || dex > 0 || intl > 0)
    buf_write(w, "<span color='#C8C8C8'>Str %d · Dex %d · Int %d</span>\n",
              str, dex, intl);

  // Resistances: non-zero % resists in display order.
  bool any_res = false;

  for(size_t i = 0; i < G_N_ELEMENTS(DB_CREATURE_RESISTS); i++)
  {
    float v = db_creature_attr(d, DB_CREATURE_RESISTS[i].field);

    if(v <= 0)
      continue;

    if(!any_res)
    {
      buf_write(w, "<span color='#B0B0B0'>Resistances:</span> "
                   "<span color='#C8C8C8'>");
      any_res = true;
    }
    else
      buf_write(w, " · ");

    buf_write(w, "%s %d%%", DB_CREATURE_RESISTS[i].label, (int)(v + 0.5f));
  }
  if(any_res)
    buf_write(w, "</span>\n");

  // Casts: the active skills it uses (skillName1..N + attack/special slots),
  // deduped by path then by resolved name, skipping passive/utility records.
  static const char *ATTACK_FIELDS[] = {
    "attackSkillName", "specialAttackSkillName", "specialAttack2SkillName",
  };
  char fld[32];
  GPtrArray *paths = g_ptr_array_new();   // borrowed const char* into the record

  for(int n = 1; n <= 32; n++)
  {
    snprintf(fld, sizeof(fld), "skillName%d", n);

    const char *sp = record_get_string_fast(d, arz_intern(fld));

    if(sp && sp[0])
      g_ptr_array_add(paths, (gpointer)sp);
  }
  for(size_t i = 0; i < G_N_ELEMENTS(ATTACK_FIELDS); i++)
  {
    const char *sp = record_get_string_fast(d, arz_intern(ATTACK_FIELDS[i]));

    if(sp && sp[0])
      g_ptr_array_add(paths, (gpointer)sp);
  }

  GHashTable *seen_path = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, NULL);
  GHashTable *seen_name = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, NULL);
  int shown = 0;

  for(guint i = 0; i < paths->len; i++)
  {
    const char *sp = g_ptr_array_index(paths, i);
    char *lc = g_ascii_strdown(sp, -1);

    if(g_hash_table_contains(seen_path, lc))
    {
      g_free(lc);
      continue;
    }
    g_hash_table_add(seen_path, lc);  // takes ownership of lc

    if(db_cast_skill_skip(sp))
      continue;

    char *name = db_skill_display_name(st, sp);
    char *name_lc = g_ascii_strdown(name, -1);

    if(g_hash_table_contains(seen_name, name_lc))
    {
      g_free(name_lc);
      g_free(name);
      continue;
    }
    g_hash_table_add(seen_name, name_lc);  // takes ownership

    char *e = escape_markup(name);

    if(shown == 0)
      buf_write(w, "<span color='#B0B0B0'>Casts:</span> "
                   "<span color='#C8C8C8'>%s", e ? e : "");
    else
      buf_write(w, ", %s", e ? e : "");
    shown++;

    if(e)
      free(e);
    g_free(name);
  }
  if(shown > 0)
    buf_write(w, "</span>\n");

  g_ptr_array_free(paths, TRUE);
  g_hash_table_destroy(seen_path);
  g_hash_table_destroy(seen_name);
}

// Build the creature detail markup (text only): name, classification/race/
// levels, the Properties block, and the items it can drop (per-difficulty
// chance).  No widget/thumbnail access.
static void
build_creature_markup(DbBrowserState *st, DbBrowseItem *bi, char *out, size_t outsz)
{
  DbCreature *c = g_ptr_array_index(st->creatures->creatures, bi->src_idx);
  BufWriter w;

  buf_init(&w, out, outsz);

  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='%s'><b>%s</b></span>\n",
            bi->color ? bi->color : "white", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  buf_write(&w, "<span color='#9F9F9F'>%s%s%s — Levels %d / %d / %d</span>\n",
            c->classification ? c->classification : "",
            c->race ? " · " : "", c->race ? c->race : "",
            c->level[0], c->level[1], c->level[2]);

  // tq-db-style Properties block (HP/mana, attributes, resistances, casts).
  db_append_creature_properties(st, c, &w);

  // Curate the drop list: hide mundane common (white) gear and health/energy
  // potions (heroes/bosses drop heaps of both), keeping the worthwhile loot.
  // Sort + truncation then happen in db_append_item_rows over this view.
  GPtrArray *shown = g_ptr_array_new();

  for(guint i = 0; i < c->drops->len; i++)
  {
    DbItemChance *ic = g_ptr_array_index(c->drops, i);

    if(!db_drop_is_excluded(ic->item_lc))
      g_ptr_array_add(shown, ic);
  }

  buf_write(&w, "\n<span color='#B0B0B0'>Drops (%u):</span>\n", shown->len);
  db_append_item_rows(st, &w, shown, 80);
  // Free the array segment but not the elements: `shown` has no free-func, so
  // the borrowed DbItemChance* (owned by c->drops) are left intact.  (TRUE, not
  // FALSE — FALSE returns pdata without freeing it, leaking the segment.)
  g_ptr_array_free(shown, TRUE);
}

// Rebuild the detail pane for a creature (markup + rendered model thumbnail).
static void
update_detail_creature(DbBrowserState *st, DbBrowseItem *bi)
{
  DbCreature *c = g_ptr_array_index(st->creatures->creatures, bi->src_idx);
  char markup[32768];

  build_creature_markup(st, bi, markup, sizeof(markup));
  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);

  // Show the creature's rendered model thumbnail (cached at startup), if any.
  GdkPixbuf *pb = creature_thumbs_load(c->path);

  db_detail_set_pixbuf(st, pb);
  if(pb)
    g_object_unref(pb);
}

// Build the quest detail markup (text only): name + its item rewards per
// difficulty.
static void
build_quest_markup(DbBrowserState *st, DbBrowseItem *bi, char *out, size_t outsz)
{
  DbQuest *q = g_ptr_array_index(st->quests->quests, bi->src_idx);
  BufWriter w;

  buf_init(&w, out, outsz);

  char *e_name = escape_markup(bi->name);

  buf_write(&w, "<span color='#DAA520'><b>%s</b></span>\n", e_name ? e_name : "");
  if(e_name)
    free(e_name);

  static const char *DIFF[3] = { "Normal", "Epic", "Legendary" };

  for(int d = 0; d < 3; d++)
  {
    if(!q->rewards[d] || g_hash_table_size(q->rewards[d]) == 0)
      continue;

    // Collect this difficulty's rewards into a sortable list.
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter it;
    gpointer k, v;

    g_hash_table_iter_init(&it, q->rewards[d]);
    while(g_hash_table_iter_next(&it, &k, &v))
    {
      DbItemChance *ic = g_new0(DbItemChance, 1);
      ic->item_lc = (char *)k;           // borrowed; not freed by us
      ic->chance[d] = *(double *)v;
      g_ptr_array_add(rows, ic);
    }

    buf_write(&w, "\n<span color='#B0B0B0'>%s rewards:</span>\n", DIFF[d]);
    db_append_item_rows(st, &w, rows, 40);
    g_ptr_array_free(rows, TRUE);  // frees the DbItemChance shells, not item_lc
  }
}

// Rebuild the detail pane for a quest (quests have no icon).
static void
update_detail_quest(DbBrowserState *st, DbBrowseItem *bi)
{
  char markup[32768];

  build_quest_markup(st, bi, markup, sizeof(markup));
  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, NULL, 0);
}

// Append the "Dropped by" cross-reference for an item: the creatures that can
// drop it, best chance first, capped.
static void
db_append_dropped_by(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  if(!st->creatures)
    return;

  GPtrArray *drops = db_creature_drops_for_item(st->creatures, item_path);

  if(!drops || drops->len == 0)
    return;

  buf_write(w, "\n<span color='#B0B0B0'>Dropped by:</span>\n");

  guint limit = 20;
  guint shown = (drops->len > limit) ? limit : drops->len;

  for(guint i = 0; i < shown; i++)
  {
    DbDrop *d = g_ptr_array_index(drops, i);
    DbCreature *c = g_ptr_array_index(st->creatures->creatures, d->creature_idx);
    char *name = db_creature_display_name(st, c);
    char *e = escape_markup(name);
    char chbuf[96];

    db_fmt_chances(d->chance, chbuf, sizeof(chbuf));
    // Clickable: jump to this creature's Monsters row (Phase 7).
    buf_write(w, "    <a href='c:%d'><span color='%s'>%s</span></a> "
                 "<span color='#808080'>%s</span>\n",
              d->creature_idx, db_creature_color(c->classification),
              e ? e : "", chbuf);
    if(e)
      free(e);
    g_free(name);
  }

  if(drops->len > shown)
    buf_write(w, "<span color='#808080'>    … +%u more</span>\n",
              drops->len - shown);
}

// Append the "Reward from" cross-reference for an item: the quests that grant
// it (with the best per-difficulty percentage).
static void
db_append_reward_from(DbBrowserState *st, const char *item_path, BufWriter *w)
{
  if(!st->quests)
    return;

  GPtrArray *rewards = db_quest_rewards_for_item(st->quests, item_path);

  if(!rewards || rewards->len == 0)
    return;

  buf_write(w, "\n<span color='#B0B0B0'>Quest reward from:</span>\n");

  for(guint i = 0; i < rewards->len && i < 20; i++)
  {
    DbQuestReward *rw = g_ptr_array_index(rewards, i);
    DbQuest *q = g_ptr_array_index(st->quests->quests, rw->quest_idx);
    const char *name = translation_get(st->tr, q->title_tag);
    char *e = escape_markup((name && name[0]) ? name : q->title_tag);
    char chbuf[96];

    db_fmt_chances(rw->percent, chbuf, sizeof(chbuf));
    // Clickable: jump to this quest's Quests row (Phase 7).
    buf_write(w, "    <a href='q:%d'><span color='#DAA520'>%s</span></a> "
                 "<span color='#808080'>%s</span>\n",
              rw->quest_idx, e ? e : "", chbuf);
    if(e)
      free(e);
  }
}

// Build the regular-item detail markup (text only): the full formatted tooltip
// (built from a synthetic vault item, the same path the in-app tooltips use)
// plus browser-only tq-db-style sections for relics/charms/artifacts/scrolls
// (shard progression, completion bonuses, formula reagents) and the "Dropped
// by" / "Quest reward from" cross-references.
static void
build_item_markup(DbBrowserState *st, DbBrowseItem *bi, char *out, size_t outsz)
{
  // Full tooltip via the shared formatter (reads base_name only).
  TQVaultItem vi;

  memset(&vi, 0, sizeof(vi));
  vi.base_name  = (char *)bi->path;  // formatter only reads it
  vi.var1       = bi->var1;
  vi.stack_size = 1;

  vault_item_format_stats(&vi, st->tr, out, outsz);

  size_t len = strlen(out);
  BufWriter w;

  buf_init(&w, out + len, outsz - len);

  // Append browser-only detail for relics/charms/artifacts/scrolls.
  if(item_is_relic_or_charm(bi->path) || item_is_artifact(bi->path) ||
     db_is_scroll(bi->path))
    db_append_consumable_detail(st, bi->path, &w);

  // Cross-references: which creatures drop it and which quests grant it.
  db_append_dropped_by(st, bi->path, &w);
  db_append_reward_from(st, bi->path, &w);
}

// Dispatch to the matching text-only build_*_markup for an item, into out.
// Mirrors update_detail's flag dispatch but produces only markup (no widgets) —
// the basis for both the live detail pane and the search-blob generation.
static void
build_detail_markup(DbBrowserState *st, DbBrowseItem *bi, char *out, size_t outsz)
{
  if(bi->is_set)
    build_set_markup(st, bi, out, outsz);
  else if(bi->is_affix)
    build_affix_markup(st, bi, out, outsz);
  else if(bi->is_skill)
    build_skill_markup(st, bi, out, outsz);
  else if(bi->is_creature)
    build_creature_markup(st, bi, out, outsz);
  else if(bi->is_quest)
    build_quest_markup(st, bi, out, outsz);
  else
    build_item_markup(st, bi, out, outsz);
}

// Build the lowercased plain-text search blob for every indexed item: render
// each entry's whole detail card to text, strip the Pango markup and lowercase
// it (so a keyword search covers everything shown on the card, with no
// per-field logic).  The display name is prefixed so name matches always hit
// even for entries whose card is otherwise sparse.  Window-less: it only needs
// st->tr (no widgets), so it runs on the startup-build path too.
void
build_search_blobs(DbBrowserState *st)
{
  char markup[32768];
  char plain[32768];

  for(int c = 0; c < CAT_COUNT; c++)
  {
    GListModel *m = G_LIST_MODEL(st->cat_stores[c]);
    guint n = m ? g_list_model_get_n_items(m) : 0;

    for(guint i = 0; i < n; i++)
    {
      DbBrowseItem *bi = g_list_model_get_item(m, i);  // owns a ref

      markup[0] = '\0';
      build_detail_markup(st, bi, markup, sizeof(markup));
      strip_pango_markup(plain, sizeof(plain), markup);

      // Prefix the display name so names always match (the card may not repeat
      // it verbatim, e.g. set/skill headers carry markup the strip removes).
      GString *blob = g_string_new(bi->name ? bi->name : "");

      g_string_append_c(blob, '\n');
      g_string_append(blob, plain);

      char *lc = g_ascii_strdown(blob->str, -1);

      g_free(bi->search_blob);
      bi->search_blob = lc;

      g_string_free(blob, TRUE);
      g_object_unref(bi);
    }
  }
}

// Rebuild the right-hand detail pane for the selected item: large icon plus the
// full formatted detail card.  Dispatches by flag to the matching renderer
// (set/affix/skill/creature/quest or the regular-item path).
void
update_detail(DbBrowserState *st, DbBrowseItem *bi)
{
  if(!bi)
  {
    gtk_picture_set_paintable(GTK_PICTURE(st->detail_pic), NULL);
    gtk_label_set_markup(GTK_LABEL(st->detail_label), "");
    return;
  }

  if(bi->is_set)     { update_detail_set(st, bi);      return; }
  if(bi->is_affix)   { update_detail_affix(st, bi);    return; }
  if(bi->is_skill)   { update_detail_skill(st, bi);    return; }
  if(bi->is_creature){ update_detail_creature(st, bi); return; }
  if(bi->is_quest)   { update_detail_quest(st, bi);    return; }

  // Regular item: markup via the shared builder, plus the item icon.
  char markup[32768];

  build_item_markup(st, bi, markup, sizeof(markup));
  gtk_label_set_markup(GTK_LABEL(st->detail_label), markup);
  db_detail_set_icon(st, bi->path, bi->var1);
}
