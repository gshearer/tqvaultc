#ifndef TEXTURE_H
#define TEXTURE_H

#include <gtk/gtk.h>
#include "arc.h"

/**
 * texture_load_from_arc - Load a .tex file from an ARC archive and return a GdkPixbuf
 */
GdkPixbuf* texture_load_from_arc(TQArcFile *arc, const char *tex_path);

/**
 * texture_load_by_index - Load a .tex file from an ARC archive by its index
 */
GdkPixbuf* texture_load_by_index(TQArcFile *arc, uint32_t index);

/**
 * texture_load - Load a texture using the asset index
 */
GdkPixbuf* texture_load(const char *tex_path);

/**
 * texture_create_with_number - Create a new pixbuf with a number drawn on it
 */
GdkPixbuf* texture_create_with_number(GdkPixbuf *base, int number);

#endif
