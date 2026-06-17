#include "creature_thumbs.h"
#include "config.h"
#include "asset_lookup.h"
#include "texture.h"
#include "mesh.h"
#include "mesh_render.h"
#include "mesh_skin.h"
#include "anm.h"
#include "arz.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Camera pitch (degrees) shared by all thumbnails; yaw is per-mesh (auto).
#define THUMB_PITCH 12.0f

// -- Paths + key ------------------------------------------------------------

// The creatures/ subdir of the per-user cache; created on demand.
static char *
thumb_dir_new(void)
{
  char *cache = tqvc_cache_dir_new();
  char *dir = g_build_filename(cache, "creatures", NULL);

  g_free(cache);
  g_mkdir_with_parents(dir, 0755);
  return(dir);
}

// Version marker file (alongside the browser cache, not inside creatures/).
static char *
thumb_marker_new(void)
{
  char *cache = tqvc_cache_dir_new();
  char *p = g_build_filename(cache, "tqvc-creature-thumbs.ver", NULL);

  g_free(cache);
  return(p);
}

// Stable dedup key for a (mesh, texture, anm) triple: case-insensitive MD5 hex.
// The anm is part of the key because the same model posed by different idle
// clips renders differently.
static char *
thumb_key(const char *mesh, const char *tex, const char *anm)
{
  char *joined = g_strdup_printf("%s|%s|%s", mesh ? mesh : "",
                                 tex ? tex : "", anm ? anm : "");

  for(char *p = joined; *p; p++)
    *p = g_ascii_tolower(*p);

  char *hex = g_compute_checksum_for_string(G_CHECKSUM_MD5, joined, -1);

  g_free(joined);
  return(hex);
}

// PNG path for a (mesh, texture, anm) triple: <cache>/creatures/<key>.png.
static char *
thumb_png_path(const char *dir, const char *mesh, const char *tex, const char *anm)
{
  char *key = thumb_key(mesh, tex, anm);
  char *file = g_strdup_printf("%s.png", key);
  char *path = g_build_filename(dir, file, NULL);

  g_free(key);
  g_free(file);
  return(path);
}

// Renderer version recorded in the marker, or -1 if absent/unparseable.
static int
thumb_marker_version(void)
{
  char *marker = thumb_marker_new();
  char *contents = NULL;
  bool ok = g_file_get_contents(marker, &contents, NULL, NULL);

  g_free(marker);
  if(!ok)
    return(-1);

  unsigned ver = 0;
  int n = sscanf(contents, "%u", &ver);

  g_free(contents);
  return(n == 1 ? (int)ver : -1);
}

// Delete every cached PNG in `dir`.  Used when the marker records a different
// renderer version: jobs_render skips any PNG already on disk, so without this
// a rebuild would keep stale renders at reused (mesh+tex+anm) keys.
static void
thumb_clear_dir(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);

  if(!d)
    return;

  const char *name;

  while((name = g_dir_read_name(d)))
  {
    if(!g_str_has_suffix(name, ".png"))
      continue;

    char *p = g_build_filename(dir, name, NULL);

    g_unlink(p);
    g_free(p);
  }

  g_dir_close(d);
}

static bool
arz_stat(const char *game_folder, guint64 *size, gint64 *mtime)
{
  if(!game_folder)
    return(false);

  char *p = g_build_filename(game_folder, "Database", "database.arz", NULL);
  GStatBuf sb;
  bool ok = (g_stat(p, &sb) == 0);

  g_free(p);
  if(!ok)
    return(false);

  *size  = (guint64)sb.st_size;
  *mtime = (gint64)sb.st_mtime;
  return(true);
}

// Stamp the version marker (renderer version + database.arz size/mtime) so the
// cache validates against the current binary + game install.
static void
thumb_marker_write(const char *game_folder)
{
  guint64 size = 0;
  gint64 mtime = 0;

  if(!arz_stat(game_folder, &size, &mtime))
    return;

  char *marker = thumb_marker_new();
  // Line 1: version + db.arz size/mtime.  Line 2: the game folder, so a path
  // correction (e.g. in Settings) invalidates a marker built for the old one.
  char *text = g_strdup_printf("%u %" G_GUINT64_FORMAT " %" G_GINT64_FORMAT "\n%s\n",
                               CREATURE_THUMB_VERSION, size, mtime,
                               game_folder ? game_folder : "");

  g_file_set_contents(marker, text, -1, NULL);
  g_free(text);
  g_free(marker);
}

// -- Rendering --------------------------------------------------------------

// Resolve a game asset path to its arc + in-arc entry index.  The asset index
// records only a file's first-part offset, which truncates multi-part (large)
// entries when extracted with arc_extract_file_at(); finding the entry index
// lets callers use the multi-part-aware arc_extract_file()/texture_load_by_index.
// Returns true and fills *arc/*index on success.
static bool
resolve_arc_entry(const char *path, TQArcFile **arc_out, int *index_out)
{
  const TQAssetEntry *e = asset_lookup(path);

  if(!e)
    return(false);

  TQArcFile *arc = asset_get_arc(e->file_id);

  if(!arc)
    return(false);

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    TQArcEntry *en = &arc->entries[i];

    if(en->num_parts > 0 && en->first_part_index < arc->num_parts &&
       arc->parts[en->first_part_index].file_offset == e->offset)
    {
      *arc_out = arc;
      *index_out = (int)i;
      return(true);
    }
  }

  return(false);
}

// Resolve a mesh by game path, extract (all parts) and parse it.
static TQMesh *
load_mesh(const char *mesh_path)
{
  TQArcFile *arc = NULL;
  int idx = -1;

  if(!resolve_arc_entry(mesh_path, &arc, &idx))
    return(NULL);

  size_t sz = 0;
  uint8_t *data = arc_extract_file(arc, (uint32_t)idx, &sz);

  if(!data)
    return(NULL);

  TQMesh *m = tq_mesh_parse(data, sz);

  free(data);
  return(m);
}

// Resolve + decode a texture by game path (multi-part safe), or NULL.
static GdkPixbuf *
load_texture(const char *tex_path)
{
  TQArcFile *arc = NULL;
  int idx = -1;

  if(!tex_path || !tex_path[0] || !resolve_arc_entry(tex_path, &arc, &idx))
    return(NULL);

  return(texture_load_by_index(arc, (uint32_t)idx));
}

// Resolve a .anm by game path (multi-part safe), extract and parse it, or NULL.
static TQAnm *
load_anm(const char *anm_path)
{
  TQArcFile *arc = NULL;
  int idx = -1;

  if(!anm_path || !anm_path[0] || !resolve_arc_entry(anm_path, &arc, &idx))
    return(NULL);

  size_t sz = 0;
  uint8_t *data = arc_extract_file(arc, (uint32_t)idx, &sz);

  if(!data)
    return(NULL);

  TQAnm *a = tq_anm_parse(data, sz);

  free(data);
  return(a);
}

// Render one mesh+texture to a PNG, posed to frame 0 of `anm_path` when the
// mesh has a skeleton and the clip resolves (else the bind pose).  True on
// success.
static bool
render_to_png(const char *mesh_path, const char *tex_path, const char *anm_path,
              const char *out_png)
{
  TQMesh *m = load_mesh(mesh_path);

  if(!m)
  {
    if(g_getenv("TQVC_THUMB_DEBUG"))
      fprintf(stderr, "  MESH-FAIL %s (lookup=%s)\n", mesh_path,
              asset_lookup(mesh_path) ? "ok" : "MISS");
    return(false);
  }

  if(m->skin && anm_path && anm_path[0])
  {
    TQAnm *anm = load_anm(anm_path);

    if(anm)
    {
      tq_mesh_pose(m, anm, 0);   // recomputes bounds; no-op on failure
      tq_anm_free(anm);
    }
    else if(g_getenv("TQVC_THUMB_DEBUG"))
      fprintf(stderr, "  ANM-FAIL %s (bind pose)\n", anm_path);
  }

  // The creature DBR's baseTexture overrides the model's own material texture;
  // when the DBR omits it (many bosses do), fall back to the .tex named in the
  // mesh's chunk-7 material footer so the creature isn't rendered untextured.
  const char *use_tex = (tex_path && tex_path[0]) ? tex_path : m->material_texture;
  GdkPixbuf *tex = load_texture(use_tex);

  if(!tex && g_getenv("TQVC_THUMB_DEBUG"))
    fprintf(stderr, "  TEX-MISS dbr='%s' material='%s'\n",
            tex_path ? tex_path : "(none)",
            m->material_texture ? m->material_texture : "(none)");
  float yaw = tq_mesh_suggest_yaw(m);   // from posed bounds when posing succeeded
  GdkPixbuf *pb = tq_mesh_render(m, tex, CREATURE_THUMB_SIZE, yaw, THUMB_PITCH);

  if(tex)
    g_object_unref(tex);
  tq_mesh_free(m);

  if(!pb)
    return(false);

  gboolean ok = gdk_pixbuf_save(pb, out_png, "png", NULL, NULL);

  g_object_unref(pb);
  return(ok);
}

// Choose an idle animation for a creature record (caller frees the result, or
// NULL if none).  Priority mirrors how the game picks a relaxed stance: the
// explicit unarmed/one-handed/spear attack-idle fields first, then any field
// whose name mentions "idle" pointing at a .anm, then any .anm at all.  Frame 0
// of an idle gives a clean relaxed pose.
static char *
pick_idle_anm(TQArzRecordData *rec)
{
  static const char *const priority[] = {
    "unarmedAttackIdleAnim", "sHandedAttackIdleAnim", "spearAttackIdleAnim"
  };

  for(size_t i = 0; i < G_N_ELEMENTS(priority); i++)
  {
    char *v = arz_record_get_string(rec, priority[i], NULL);

    if(v && v[0])
      return(v);
    g_free(v);
  }

  // Fallback: scan every string var for a .anm, preferring an "idle"-named one.
  char *any_idle = NULL, *any_anm = NULL;

  for(uint32_t i = 0; i < rec->num_vars; i++)
  {
    const TQVariable *var = &rec->vars[i];

    if(var->type != TQ_VAR_STRING || var->count == 0 || !var->value.str)
      continue;

    const char *val = var->value.str[0];
    size_t len = val ? strlen(val) : 0;

    if(len < 4 || g_ascii_strcasecmp(val + len - 4, ".anm") != 0)
      continue;

    if(!any_anm)
      any_anm = g_strdup(val);

    if(var->name)
    {
      char *lname = g_ascii_strdown(var->name, -1);
      bool is_idle = (strstr(lname, "idle") != NULL);

      g_free(lname);
      if(is_idle)
      {
        any_idle = g_strdup(val);
        break;
      }
    }
  }

  if(any_idle)
  {
    g_free(any_anm);
    return(any_idle);
  }

  return(any_anm);
}

// Read a creature's "mesh" + "baseTexture" + chosen idle anm DBR fields (caller
// frees all three; tex/anm may come back NULL).  Returns false (and frees) if
// the record has no mesh.
static bool
creature_assets(const char *dbr_path, char **mesh, char **tex, char **anm)
{
  *mesh = NULL;
  *tex  = NULL;
  *anm  = NULL;

  TQArzRecordData *rec = asset_get_dbr(dbr_path);

  if(!rec)
    return(false);

  *mesh = arz_record_get_string(rec, "mesh", NULL);
  *tex  = arz_record_get_string(rec, "baseTexture", NULL);
  *anm  = pick_idle_anm(rec);

  if(!*mesh || !(*mesh)[0])
  {
    g_free(*mesh);
    g_free(*tex);
    g_free(*anm);
    *mesh = *tex = *anm = NULL;
    return(false);
  }

  return(true);
}

// -- Public API -------------------------------------------------------------

bool
creature_thumbs_cache_valid(const char *game_folder)
{
  guint64 size = 0;
  gint64 mtime = 0;

  if(!arz_stat(game_folder, &size, &mtime))
    return(false);

  char *marker = thumb_marker_new();
  char *contents = NULL;
  bool ok = g_file_get_contents(marker, &contents, NULL, NULL);

  g_free(marker);
  if(!ok)
    return(false);

  // Line 1: "<version> <size> <mtime>".  Line 2: the game folder it was built
  // for (paths may contain spaces, so it gets its own line).
  unsigned ver = 0;
  guint64 m_size = 0;
  gint64 m_mtime = 0;
  int n = sscanf(contents, "%u %" G_GUINT64_FORMAT " %" G_GINT64_FORMAT,
                 &ver, &m_size, &m_mtime);

  bool dir_ok = false;
  const char *nl = strchr(contents, '\n');

  if(nl)
  {
    char *line2 = g_strdup(nl + 1);
    g_strchomp(line2);   // drop the trailing newline/whitespace
    dir_ok = (g_strcmp0(line2, game_folder ? game_folder : "") == 0);
    g_free(line2);
  }

  g_free(contents);
  return(n == 3 && ver == CREATURE_THUMB_VERSION &&
         m_size == size && m_mtime == mtime && dir_ok);
}

void
creature_thumbs_cache_prepare(const char *game_folder)
{
  // Thumbnails render lazily on demand now (creature_thumbs_load), so there is
  // no eager batch build to invalidate the on-disk cache on a renderer-version
  // bump or a game update.  Do that invalidation here -- cheaply, with NO
  // rendering: if the marker is missing or stale, wipe any cached PNGs and
  // stamp a fresh marker.  After this the cache validates, and every subsequent
  // on-demand render is for the current renderer version + game install.  Call
  // once at startup, before browsing.
  if(creature_thumbs_cache_valid(game_folder))
    return;

  char *dir = thumb_dir_new();   // also creates it

  if(dir)
  {
    thumb_clear_dir(dir);
    g_free(dir);
  }

  thumb_marker_write(game_folder);
}

// Deduplicated render work list (one entry per distinct mesh+texture+anm).
struct CreatureThumbJobs {
  char **mesh;   // mesh game path  [count]
  char **tex;    // baseTexture path (may be "") [count]
  char **anm;    // idle anm path (may be "") [count]
  char **png;    // output PNG path [count]
  int    count;
};

CreatureThumbJobs *
creature_thumbs_jobs_new(DbCreatureIndex *idx)
{
  CreatureThumbJobs *j = g_new0(CreatureThumbJobs, 1);

  if(!idx || !idx->creatures)
    return(j);

  char *dir = thumb_dir_new();

  // If the cached renders were made by a different renderer version, wipe them
  // first: the rebuild may reuse the same (mesh+tex+anm) keys, and jobs_render
  // skips any PNG already on disk — so stale renders would otherwise survive.
  int mv = thumb_marker_version();

  if(mv >= 0 && mv != CREATURE_THUMB_VERSION)
    thumb_clear_dir(dir);

  // Dedup hero variants that share a model+pose: key = "mesh\ttex\tanm".
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *meshes = g_ptr_array_new();
  GPtrArray *texes = g_ptr_array_new();
  GPtrArray *anms = g_ptr_array_new();

  for(guint i = 0; i < idx->creatures->len; i++)
  {
    DbCreature *c = g_ptr_array_index(idx->creatures, i);
    char *mesh = NULL, *tex = NULL, *anm = NULL;

    if(!creature_assets(c->path, &mesh, &tex, &anm))
      continue;

    char *k = g_strdup_printf("%s\t%s\t%s", mesh, tex ? tex : "", anm ? anm : "");

    if(g_hash_table_add(seen, k))    // TRUE if newly inserted
    {
      g_ptr_array_add(meshes, mesh);
      g_ptr_array_add(texes, tex ? tex : g_strdup(""));
      g_ptr_array_add(anms, anm ? anm : g_strdup(""));
    }
    else
    {
      g_free(mesh);
      g_free(tex);
      g_free(anm);
    }
  }

  j->count = (int)meshes->len;
  j->mesh = g_new0(char *, j->count);
  j->tex = g_new0(char *, j->count);
  j->anm = g_new0(char *, j->count);
  j->png = g_new0(char *, j->count);

  for(int i = 0; i < j->count; i++)
  {
    j->mesh[i] = g_ptr_array_index(meshes, i);
    j->tex[i]  = g_ptr_array_index(texes, i);
    j->anm[i]  = g_ptr_array_index(anms, i);
    j->png[i]  = thumb_png_path(dir, j->mesh[i], j->tex[i], j->anm[i]);
  }

  g_ptr_array_free(meshes, TRUE);
  g_ptr_array_free(texes, TRUE);
  g_ptr_array_free(anms, TRUE);
  g_hash_table_destroy(seen);
  g_free(dir);
  return(j);
}

int
creature_thumbs_jobs_total(const CreatureThumbJobs *jobs)
{
  return(jobs ? jobs->count : 0);
}

void
creature_thumbs_jobs_render(CreatureThumbJobs *jobs, int i)
{
  if(!jobs || i < 0 || i >= jobs->count)
    return;

  // Idempotent: skip work already on disk (lets an interrupted build resume).
  if(g_file_test(jobs->png[i], G_FILE_TEST_EXISTS))
    return;

  render_to_png(jobs->mesh[i], jobs->tex[i][0] ? jobs->tex[i] : NULL,
                jobs->anm[i][0] ? jobs->anm[i] : NULL, jobs->png[i]);
}

void
creature_thumbs_jobs_finish(CreatureThumbJobs *jobs, const char *game_folder)
{
  // Write the version marker last so an interrupted build stays invalid.
  thumb_marker_write(game_folder);

  if(jobs)
  {
    for(int i = 0; i < jobs->count; i++)
    {
      g_free(jobs->mesh[i]);
      g_free(jobs->tex[i]);
      g_free(jobs->anm[i]);
      g_free(jobs->png[i]);
    }
    g_free(jobs->mesh);
    g_free(jobs->tex);
    g_free(jobs->anm);
    g_free(jobs->png);
    g_free(jobs);
  }
}

bool
creature_thumbs_build(DbCreatureIndex *idx, const char *game_folder,
                      CreatureThumbProgress cb, void *user)
{
  CreatureThumbJobs *jobs = creature_thumbs_jobs_new(idx);
  int total = creature_thumbs_jobs_total(jobs);

  for(int i = 0; i < total; i++)
  {
    creature_thumbs_jobs_render(jobs, i);
    if(cb)
      cb(i + 1, total, user);
  }

  creature_thumbs_jobs_finish(jobs, game_folder);
  return(true);
}

GdkPixbuf *
creature_thumbs_load(const char *creature_dbr_path)
{
  char *mesh = NULL, *tex = NULL, *anm = NULL;

  if(!creature_assets(creature_dbr_path, &mesh, &tex, &anm))
    return(NULL);

  char *dir = thumb_dir_new();
  char *png = thumb_png_path(dir, mesh, tex, anm);
  GdkPixbuf *pb = gdk_pixbuf_new_from_file(png, NULL);

  // Not pre-rendered by the startup batch build?  Render it on demand now and
  // cache it for next time.  This keeps thumbnails working regardless of cache
  // state — the batch build skipped (e.g. --debug), interrupted, a stale marker,
  // or a key the build didn't produce.  We render to the very path we just
  // looked up, so it can't miss again.  ~40ms for one creature, then instant.
  if(!pb)
  {
    if(render_to_png(mesh, (tex && tex[0]) ? tex : NULL,
                     (anm && anm[0]) ? anm : NULL, png))
      pb = gdk_pixbuf_new_from_file(png, NULL);
  }

  g_free(mesh);
  g_free(tex);
  g_free(anm);
  g_free(dir);
  g_free(png);
  return(pb);
}
