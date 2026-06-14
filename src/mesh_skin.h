#ifndef MESH_SKIN_H
#define MESH_SKIN_H

// mesh_skin -- linear-blend skinning that poses a parsed TQMesh into a natural
// stance for its thumbnail, using one frame of an .anm clip, instead of the
// static bind-pose T-pose.  GPU-free pure math; GTK-free, links into
// tq-dbr-tool alongside mesh.c / anm.c.

#include <stdbool.h>
#include "mesh.h"
#include "anm.h"

// Pose `m` in place to `frame` of `anm` via linear-blend skinning, then
// recompute its axis-aligned bounds.  Requires m->skin (a parsed skeleton +
// per-vertex weights) and a usable `anm`; returns false and leaves the mesh
// untouched otherwise, so the caller simply renders the bind pose.  Bones not
// present in the clip keep their bind transform.
bool tq_mesh_pose(TQMesh *m, const TQAnm *anm, int frame);

#endif
