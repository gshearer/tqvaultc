#ifndef TEXTURE_H
#define TEXTURE_H

#include <gtk/gtk.h>
#include "arc.h"

// texture_load_from_arc - load a .tex file from an ARC archive
// arc: the archive file
// tex_path: path to the texture within the archive
// returns: GdkPixbuf or NULL on failure
GdkPixbuf *texture_load_from_arc(TQArcFile *arc, const char *tex_path);

// texture_load_by_index - load a .tex file from an ARC archive by entry index
// arc: the archive file
// index: entry index within the archive
// returns: GdkPixbuf or NULL on failure
GdkPixbuf *texture_load_by_index(TQArcFile *arc, uint32_t index);

// texture_load - load a texture using the asset index
// tex_path: normalized game path to the texture
// returns: GdkPixbuf or NULL on failure
GdkPixbuf *texture_load(const char *tex_path);

// texture_create_with_number - create a new pixbuf with a number drawn on it
// base: source pixbuf to composite onto
// number: the number to render
// returns: new GdkPixbuf with the number overlay
GdkPixbuf *texture_create_with_number(GdkPixbuf *base, int number);

#endif
