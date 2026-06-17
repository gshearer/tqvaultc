#ifndef CREATURE_THUMBS_H
#define CREATURE_THUMBS_H

// creature_thumbs -- on-disk cache of rendered creature thumbnails.
//
// Each creature's DBR names a `mesh` (.msh) and a `baseTexture` (.tex); we
// software-rasterize the model (see mesh.c / mesh_render.c) into a small PNG
// once, behind the first-run "Setting up TQVaultC…" popup, and load it on
// demand in the Database Browser creature detail pane.  Thumbnails are keyed
// (and deduplicated) by mesh+texture, so the many hero variants that share a
// model render only once.
//
// Validity is gated on a version marker + the live database.arz size/mtime,
// mirroring db_browser_cache.c, so a game update or a CREATURE_THUMB_VERSION
// bump transparently forces a rebuild.

#include <gtk/gtk.h>
#include <stdbool.h>
#include "db_creatures.h"

// Bump on any change to the renderer output or the cache layout.
// v2: natural-pose skinning (idle .anm) replaces the bind-pose T-pose.
// v3: fall back to the mesh's own material texture when a creature DBR has no
//     baseTexture (fixes all-white renders, e.g. Telkine / "The Broodmother").
#define CREATURE_THUMB_VERSION 3

// Pixel size of cached thumbnails (square).
#define CREATURE_THUMB_SIZE 256

// True iff the thumbnail cache marker matches the current binary + game install
// (a build would be a no-op).
bool creature_thumbs_cache_valid(const char *game_folder);

// Ensure the on-demand thumbnail cache directory matches the current renderer
// version + game install, WITHOUT rendering anything: if the marker is missing
// or stale, wipe any cached PNGs and stamp a fresh marker.  Call once at startup
// so lazily-rendered thumbnails (creature_thumbs_load) are always current --
// this replaces the eager batch build's version invalidation now that
// thumbnails render on demand.  Cheap (a stat + maybe a dir wipe).
void creature_thumbs_cache_prepare(const char *game_folder);

// Render a thumbnail for every distinct mesh+texture among idx's creatures into
// the cache, skipping any already present.  Requires the asset manager to be
// initialized (reads creature DBRs + meshes + textures from the arcs).  The
// optional progress callback fires after each unique render.  Returns true once
// the version marker is written.
typedef void (*CreatureThumbProgress)(int done, int total, void *user);
bool creature_thumbs_build(DbCreatureIndex *idx, const char *game_folder,
                           CreatureThumbProgress cb, void *user);

// Incremental rendering API for the startup popup, which must render across
// idle ticks so the progress bar animates and the window stays responsive.
// Build the deduplicated job list from idx, render one job at a time, then
// write the version marker.  Typical use:
//   jobs = creature_thumbs_jobs_new(idx);
//   for(i = 0; i < creature_thumbs_jobs_total(jobs); i++)
//     creature_thumbs_jobs_render(jobs, i);
//   creature_thumbs_jobs_finish(jobs, game_folder);  // frees jobs + writes marker
typedef struct CreatureThumbJobs CreatureThumbJobs;

CreatureThumbJobs *creature_thumbs_jobs_new(DbCreatureIndex *idx);
int  creature_thumbs_jobs_total(const CreatureThumbJobs *jobs);
void creature_thumbs_jobs_render(CreatureThumbJobs *jobs, int i);
void creature_thumbs_jobs_finish(CreatureThumbJobs *jobs, const char *game_folder);

// Load the cached thumbnail for a creature given its DBR record path, or NULL
// if the creature has no mesh / the render is missing.  Caller unrefs.
GdkPixbuf *creature_thumbs_load(const char *creature_dbr_path);

#endif
