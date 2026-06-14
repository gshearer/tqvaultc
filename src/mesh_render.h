#ifndef MESH_RENDER_H
#define MESH_RENDER_H

// mesh_render -- tiny software rasterizer that turns a parsed TQMesh + its
// base texture into a flat thumbnail (no GPU; works headless and cross
// platform).  Renders the bind pose with an orthographic camera, a z-buffer,
// nearest-neighbour texture sampling and simple directional + ambient shading.
// The background is left transparent so the result composites like any icon.

#include <gtk/gtk.h>
#include "mesh.h"

// Render `m` textured with `texture` (may be NULL -> flat grey shading) into a
// size x size RGBA pixbuf.  yaw/pitch are the camera orbit angles in degrees
// (yaw about the vertical Y axis, pitch about X).  Returns a new pixbuf the
// caller must unref, or NULL on failure.
GdkPixbuf *tq_mesh_render(const TQMesh *m, GdkPixbuf *texture, int size,
                          float yaw_deg, float pitch_deg);

#endif
