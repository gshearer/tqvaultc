// Visual skill tree: cairo rendering (icons, counters, connectors, mastery
// level bar, tooltips).  Shared types in ui_skills_tree_internal.h.

#include "ui_skills_tree_internal.h"

#include "asset_lookup.h"
#include "texture.h"
#include "translation.h"
#include "item_stats.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static GdkPixbuf *
skill_icon(AppWidgets *w, const char *tex)
{
  if(!tex || !tex[0])
    return(NULL);

  GdkPixbuf *cached = g_hash_table_lookup(w->texture_cache, tex);

  if(cached)
    return(g_object_ref(cached));

  GdkPixbuf *pb = texture_load(tex);

  if(pb)
    g_hash_table_insert(w->texture_cache, strdup(tex), g_object_ref(pb));

  return(pb);
}

// ── Tooltip composer ─────────────────────────────────────────────────────

// Replace every occurrence of `from` with `to`, returning a new string.
static char *
str_replace_all(const char *s, const char *from, const char *to)
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

// Build in-game-style Pango markup for a skill (or the mastery) at `level`.
// The gear_* arguments carry the character's equipment +skill bonuses for this
// skill (0 for the mastery node); when present, the level lines show the
// boosted total, the stat previews reflect the effective level, and a breakdown
// of the bonus sources is appended.  mastery_name labels the mastery-wide
// bonus (may be NULL).
char *
skill_tooltip_markup(AppWidgets *w, const char *skill_path, int level,
                     int max_level, int ultimate_level, int gear_all,
                     int gear_mastery, int gear_skill, const char *mastery_name)
{
  char raw[16384];
  BufWriter bw;

  buf_init(&bw, raw, sizeof(raw));

  char name[128];

  resolve_skill_name(w, skill_path, name, sizeof(name));
  char *ename = escape_markup(name);

  buf_write(&bw, "<b>%s</b>\n", ename ? ename : name);
  free(ename);

  // Buff/toggle/aura skills keep their description + stats in the referenced
  // record, not the shell the tree points at.  Resolve that record for both.
  char eff_path[256];

  resolve_effective_skill_path(skill_path, eff_path, sizeof(eff_path));

  TQArzRecordData *dbr = asset_get_dbr(eff_path);

  if(dbr)
  {
    char *dtag = arz_record_get_string(dbr, "skillBaseDescription", NULL);

    if(dtag)
    {
      const char *d = translation_get(w->translations, dtag);

      if(d && d[0])
      {
        char *ed = escape_markup(d);

        buf_write(&bw, "%s\n", ed ? ed : d);
        free(ed);
      }

      free(dtag);
    }
  }

  buf_write(&bw, "\n");

  const char *WHITE = "#E0E0E0";
  int raw_bonus = gear_all + gear_mastery + gear_skill;

  if(level > 0)
  {
    int eff = effective_level(level, raw_bonus, ultimate_level);
    int shown = eff - level;

    if(shown > 0)
      buf_write(&bw, "<span color='#FFD200'>Current Level: %d "
                     "<span color='#5FE85F'>(+%d = %d)</span></span>\n",
                level, shown, eff);
    else
      buf_write(&bw, "<span color='#FFD200'>Current Level: %d</span>\n", level);

    add_stats_from_record(eff_path, w->translations, &bw, WHITE, eff - 1);
  }

  if(level < max_level)
  {
    if(level > 0)
      buf_write(&bw, "\n");

    int nxt = level + 1;
    int eff = effective_level(nxt, raw_bonus, ultimate_level);
    int shown = eff - nxt;

    if(shown > 0)
      buf_write(&bw, "<span color='#FFD200'>Next Level: %d "
                     "<span color='#5FE85F'>(+%d = %d)</span></span>\n",
                nxt, shown, eff);
    else
      buf_write(&bw, "<span color='#FFD200'>Next Level: %d</span>\n", nxt);

    add_stats_from_record(eff_path, w->translations, &bw, WHITE, eff - 1);
  }

  // Equipment +skill bonus breakdown (which gear sources feed this skill, and a
  // reminder that a point must be spent for them to take effect).
  if(raw_bonus > 0)
  {
    buf_write(&bw, "\n<span color='#5FE85F'>Equipment Bonus:</span>\n");

    if(gear_all > 0)
      buf_write(&bw, "<span color='%s'>  +%d to All Skills</span>\n", WHITE, gear_all);

    if(gear_mastery > 0)
      buf_write(&bw, "<span color='%s'>  +%d to %s Mastery Skills</span>\n",
                WHITE, gear_mastery, mastery_name ? mastery_name : "this");

    if(gear_skill > 0)
      buf_write(&bw, "<span color='%s'>  +%d to this Skill</span>\n", WHITE, gear_skill);

    if(level < 1)
      buf_write(&bw, "<span color='#AAAAAA'>  (spend at least 1 point to apply)</span>\n");
  }

  buf_write(&bw, "\n<span color='#40FF40'>Left click to add unused skill points. "
                 "Right click to remove.</span>");

  // Skill tooltips use "Intellect" where item stats say "Intelligence".
  return(str_replace_all(raw, "Intelligence", "Intellect"));
}

// ── Drawing ──────────────────────────────────────────────────────────────

static void
blit_icon(cairo_t *cr, GdkPixbuf *pb, double cx, double cy, double size, double alpha)
{
  if(!pb)
    return;

  int pw = gdk_pixbuf_get_width(pb);
  int ph = gdk_pixbuf_get_height(pb);

  if(pw < 1 || ph < 1)
    return;

  cairo_save(cr);
  cairo_translate(cr, cx - size / 2.0, cy - size / 2.0);
  cairo_scale(cr, size / (double)pw, size / (double)ph);
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
  G_GNUC_END_IGNORE_DEPRECATIONS

  if(alpha >= 1.0)
    cairo_paint(cr);
  else
    cairo_paint_with_alpha(cr, alpha);

  cairo_restore(cr);
}

// Draw a counter string centered in the given colour, with a dark outline for
// legibility.  Used for the mastery "cur / max" line and each skill's level.
static void
draw_counter(cairo_t *cr, const char *txt, double cx, double top_y,
             double r, double g, double b)
{
  cairo_save(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 12.0);

  cairo_text_extents_t te;

  cairo_text_extents(cr, txt, &te);
  double tx = cx - te.width / 2.0 - te.x_bearing;
  double ty = top_y + SK_COUNTER_DY;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.85);

  for(int dx = -1; dx <= 1; dx++)
    for(int dy = -1; dy <= 1; dy++)
    {
      if(dx == 0 && dy == 0)
        continue;

      cairo_move_to(cr, tx + dx, ty + dy);
      cairo_show_text(cr, txt);
    }

  cairo_set_source_rgb(cr, r, g, b);
  cairo_move_to(cr, tx, ty);
  cairo_show_text(cr, txt);
  cairo_restore(cr);
}

// Connector between a parent (lower) and a child (higher).  In-game, every
// skill in a group shares a column and the link runs as a vertical trunk in
// the gap just to the RIGHT of the column, tapped by a short horizontal stub
// into each icon -- so the line never passes through an unrelated icon that
// merely shares the column.  Stubs start a few px inside the icon edge; since
// connectors are drawn under the icons, that overlap is hidden and leaves no
// gap.  A cross-column link (rare; only from a parent-detection fallback) keeps
// the old orthogonal bracket.
static void
draw_edge(cairo_t *cr, double px, double py, double cx, double cy, bool on)
{
  if(on)
    cairo_set_source_rgb(cr, 0.85, 0.78, 0.45);
  else
    cairo_set_source_rgb(cr, 0.34, 0.34, 0.38);

  cairo_set_line_width(cr, on ? 2.5 : 1.5);

  if(fabs(px - cx) < 1.0)
  {
    double stub_in = px + SK_ICON / 2.0 - 4.0;  // a touch inside the icon edge
    double trunk_x = px + SK_ICON / 2.0 + SK_STUB;

    cairo_move_to(cr, stub_in, py);   // stub into the parent
    cairo_line_to(cr, trunk_x, py);
    cairo_move_to(cr, stub_in, cy);   // stub into the child
    cairo_line_to(cr, trunk_x, cy);
    cairo_move_to(cr, trunk_x, py);   // vertical trunk in the column's gap
    cairo_line_to(cr, trunk_x, cy);
    cairo_stroke(cr);
    return;
  }

  double py_top = py - SK_ICON / 2.0;
  double cy_bot = cy + SK_ICON / 2.0;
  double mid = (py_top + cy_bot) / 2.0;

  cairo_move_to(cr, px, py_top);
  cairo_line_to(cr, px, mid);
  cairo_line_to(cr, cx, mid);
  cairo_line_to(cr, cx, cy_bot);
  cairo_stroke(cr);
}

// Screen Y for a mastery level on the bar (level 0 at the bottom, MAX at top).
static double
bar_level_y(double bar_bottom, double span, int level)
{
  if(level < 0) level = 0;
  if(level > MAX_MASTERY_LEVEL) level = MAX_MASTERY_LEVEL;

  return(bar_bottom - ((double)level / MAX_MASTERY_LEVEL) * span);
}

// The mastery level bar in the left gutter: a vertical track filled from the
// bottom up to the current mastery level, with a tick + point-requirement label
// for each tier (records/game/gameengine.dbr skillMasteryTierLevel, mirrored in
// tier_mastery_req).  Mirrors the in-game skill window's left-edge bar.
static void
draw_mastery_bar(cairo_t *cr, MasteryPane *mp)
{
  if(mp->num_nodes <= 0)
    return;

  // Vertical span: top of the highest skill row down to just above the mastery
  // icon.  The bar is a linear level scale (0 at the bottom, MAX at the top).
  double top_y = mp->nodes[0].y, bot_y = mp->nodes[0].y;

  for(int i = 1; i < mp->num_nodes; i++)
  {
    if(mp->nodes[i].y < top_y) top_y = mp->nodes[i].y;
    if(mp->nodes[i].y > bot_y) bot_y = mp->nodes[i].y;
  }

  double bar_top = top_y - SK_ICON / 2.0;
  double bar_bottom = mp->mastery_y - SK_ICON / 2.0 - 4.0;

  if(bar_bottom <= bar_top + 4.0)
    return;

  double span = bar_bottom - bar_top;
  double bar_l = mp->mastery_x - SK_BAR_W / 2.0;

  // Track + fill + border.
  cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
  cairo_rectangle(cr, bar_l, bar_top, SK_BAR_W, span);
  cairo_fill(cr);

  double fill_y = bar_level_y(bar_bottom, span, mp->mastery_level);

  cairo_set_source_rgb(cr, 0.85, 0.72, 0.30);
  cairo_rectangle(cr, bar_l, fill_y, SK_BAR_W, bar_bottom - fill_y);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 0.45, 0.40, 0.25);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, bar_l, bar_top, SK_BAR_W, span);
  cairo_stroke(cr);

  // Tier ticks + point-requirement labels.
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 9.0);

  for(int t = 1; t <= MAX_TIER; t++)
  {
    int lvl = tier_mastery_req[t];
    double ty = bar_level_y(bar_bottom, span, lvl);
    bool reached = mp->mastery_level >= lvl;

    cairo_set_source_rgb(cr, 0.30, 0.28, 0.20);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, bar_l, ty);
    cairo_line_to(cr, bar_l + SK_BAR_W, ty);
    cairo_stroke(cr);

    char buf[8];
    cairo_text_extents_t ext;

    snprintf(buf, sizeof(buf), "%d", lvl);
    cairo_text_extents(cr, buf, &ext);

    if(reached)
      cairo_set_source_rgb(cr, 0.92, 0.85, 0.55);
    else
      cairo_set_source_rgb(cr, 0.50, 0.50, 0.50);

    cairo_move_to(cr, bar_l - 3.0 - ext.width, ty + ext.height / 2.0);
    cairo_show_text(cr, buf);
  }
}

void
skill_canvas_draw_cb(GtkDrawingArea *da, cairo_t *cr, int width, int height, gpointer user_data)
{
  (void)width;
  (void)height;
  (void)user_data;
  SkillsState *st = g_object_get_data(G_OBJECT(da), "state");
  int pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(da), "pane"));
  MasteryPane *mp = &st->panes[pane];
  AppWidgets *w = st->widgets;

  cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
  cairo_paint(cr);

  if(mp->mastery_def_idx < 0)
    return;

  if(mp->num_nodes == 0 && mp->mastery_chr_skill_idx < 0)
  {
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_move_to(cr, 16, 30);
    cairo_show_text(cr, "No skill data in save -- select this mastery in-game first.");
    return;
  }

  // Mastery level bar in the left gutter (behind everything else).
  if(mp->use_db_layout)
    draw_mastery_bar(cr, mp);

  // Connection lines (under icons).
  for(int i = 0; i < mp->num_nodes; i++)
  {
    int p = mp->nodes[i].parent_idx;

    if(p < 0)
      continue;

    draw_edge(cr, mp->nodes[p].x, mp->nodes[p].y,
              mp->nodes[i].x, mp->nodes[i].y, mp->nodes[p].cur_level > 0);
  }

  // Mastery node.
  {
    bool on = mp->mastery_level > 0;
    GdkPixbuf *pb = skill_icon(w, on ? mp->mastery_up_tex : mp->mastery_down_tex);

    if(!pb)
      pb = skill_icon(w, on ? mp->mastery_down_tex : mp->mastery_up_tex);

    blit_icon(cr, pb, mp->mastery_x, mp->mastery_y, SK_ICON, 1.0);

    if(pb)
      g_object_unref(pb);

    char buf[32];

    snprintf(buf, sizeof(buf), "%d / %d", mp->mastery_level, MAX_MASTERY_LEVEL);
    draw_counter(cr, buf, mp->mastery_x, mp->mastery_y + SK_ICON / 2.0,
                 0.92, 0.92, 0.92);
  }

  // Skill nodes.
  for(int i = 0; i < mp->num_nodes; i++)
  {
    SkillNode *n = &mp->nodes[i];
    bool accessible = n->chr_skill_idx >= 0 && skill_is_accessible(mp, i);
    bool on = n->cur_level > 0;
    GdkPixbuf *pb = skill_icon(w, on ? n->up_tex : n->down_tex);

    if(!pb)
      pb = skill_icon(w, n->up_tex);

    blit_icon(cr, pb, n->x, n->y, SK_ICON, accessible ? 1.0 : 0.4);

    if(pb)
      g_object_unref(pb);

    // In-game style: show the skill's effective level as a single number
    // (allocated points plus applicable equipment bonuses, applied only with a
    // point spent and clamped to the ultimate cap).  Colour it green only when
    // equipment pushed it above the skill's natural cap (skillMaxLevel);
    // otherwise leave it plain.
    int raw_bonus = n->gear_all + n->gear_mastery + n->gear_skill;
    int eff = effective_level(n->cur_level, raw_bonus, n->ultimate_level);
    double counter_y = n->y + SK_ICON / 2.0;
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", eff);

    if(eff > n->max_level)
      draw_counter(cr, buf, n->x, counter_y, 0.42, 0.95, 0.42);
    else
      draw_counter(cr, buf, n->x, counter_y, 0.92, 0.92, 0.92);
  }
}

// ── Hit testing + interaction ────────────────────────────────────────────

