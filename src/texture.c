#include "texture.h"
#include "config.h"
#include "asset_lookup.h"
#include "dds_decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
pixbuf_free_pixels(guchar *pixels, gpointer user_data)
{
  (void)user_data;
  free(pixels);
}

// normalize_path - convert forward slashes to backslashes for ARC path matching
// path: input path string
// returns: newly allocated string with normalized separators (caller must free)
static char *
normalize_path(const char *path)
{
  char *res = strdup(path);

  if(!res)
    return(NULL);

  for(int i = 0; res[i]; i++)
  {
    if(res[i] == '/')
      res[i] = '\\';
  }
  return(res);
}

// texture_load_from_data - decode raw TEX/DDS data into a GdkPixbuf
// raw_data: raw TEX file bytes (ownership transferred, freed by this function)
// raw_size: size of raw_data in bytes
// returns: GdkPixbuf or NULL on failure
static GdkPixbuf *
texture_load_from_data(uint8_t *raw_data, size_t raw_size)
{
  if(tqvc_debug)
    printf("texture_load_from_data: size=%zu\n", raw_size);

  if(raw_size < 13)
  {
    free(raw_data);
    return(NULL);
  }

  int header_size = 12;

  if(memcmp(raw_data, "TEX", 3) == 0)
  {
    if(raw_data[3] == 2)
      header_size = 13;
  }

  if(raw_size < (size_t)header_size + 4)
  {
    free(raw_data);
    return(NULL);
  }

  // skip TEX header to get to DDS
  uint8_t *dds_data = raw_data + header_size;
  size_t dds_size = raw_size - header_size;

  // .tex files commonly use the variant magic "DDSR" instead of "DDS ";
  // normalise so dds_decode() recognises it.
  if(dds_size >= 4 && memcmp(dds_data, "DDSR", 4) == 0)
    dds_data[3] = ' ';

  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t *pixels = dds_decode(dds_data, dds_size, &width, &height);

  GdkPixbuf *pixbuf = NULL;

  if(pixels)
  {
    pixbuf = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8,
        (int)width, (int)height, (int)width * 4,
        pixbuf_free_pixels, NULL);
  }
  else if(tqvc_debug)
  {
    fprintf(stderr, "dds_decode: failed (size=%zu)\n", dds_size);
  }

  free(raw_data);
  return(pixbuf);
}

// texture_load - load a texture using the global asset index
// tex_path: normalized game path to the texture
// returns: GdkPixbuf or NULL on failure
GdkPixbuf *
texture_load(const char *tex_path)
{
  // First-call diagnostic: log the first few attempts (success or failure)
  // unconditionally so Windows users can debug without --debug.
  static int diag_count = 0;
  bool diag = (diag_count < 5);

  if(diag)
    diag_count++;

  const TQAssetEntry *entry = asset_lookup(tex_path);

  if(!entry)
  {
    if(diag)
      fprintf(stderr, "texture_load[%d]: asset_lookup(%s) = NULL\n", diag_count, tex_path);
    return(NULL);
  }

  TQArcFile *arc = asset_get_arc(entry->file_id);

  if(!arc)
  {
    if(diag)
      fprintf(stderr, "texture_load[%d]: asset_get_arc(file_id=%u) = NULL for %s\n",
              diag_count, entry->file_id, tex_path);
    return(NULL);
  }

  size_t raw_size;
  uint8_t *raw_data = arc_extract_file_at(arc, entry->offset, entry->size, entry->real_size, &raw_size);

  if(!raw_data)
  {
    if(diag)
      fprintf(stderr, "texture_load[%d]: arc_extract_file_at failed (offset=%u sz=%u real=%u) for %s\n",
              diag_count, entry->offset, entry->size, entry->real_size, tex_path);
    return(NULL);
  }

  GdkPixbuf *pb = texture_load_from_data(raw_data, raw_size);

  if(diag)
    fprintf(stderr, "texture_load[%d]: %s for %s\n",
            diag_count, pb ? "OK" : "decode-FAILED", tex_path);

  return(pb);
}

// texture_load_from_arc - load a .tex file from an ARC archive by path
// arc: the archive file to search
// tex_path: path to the texture within the archive
// returns: GdkPixbuf or NULL on failure
GdkPixbuf *
texture_load_from_arc(TQArcFile *arc, const char *tex_path)
{
  if(!arc)
    return(NULL);

  char *target = normalize_path(tex_path);
  int entry_index = -1;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    char *entry_norm = normalize_path(arc->entries[i].path);

    if(strcasecmp(entry_norm, target) == 0)
    {
      entry_index = (int)i;
      free(entry_norm);
      break;
    }
    free(entry_norm);
  }
  free(target);

  if(entry_index == -1)
  {
    if(tqvc_debug)
      printf("Texture not found in ARC: %s\n", tex_path);

    return(NULL);
  }

  return(texture_load_by_index(arc, (uint32_t)entry_index));
}

// texture_load_by_index - load a .tex file from an ARC archive by entry index
// arc: the archive file
// index: entry index within the archive
// returns: GdkPixbuf or NULL on failure
GdkPixbuf *
texture_load_by_index(TQArcFile *arc, uint32_t index)
{
  if(!arc || index >= arc->num_files)
    return(NULL);

  size_t raw_size;
  uint8_t *raw_data = arc_extract_file(arc, index, &raw_size);

  if(!raw_data)
    return(NULL);

  return(texture_load_from_data(raw_data, raw_size));
}

// texture_create_with_number - create a new pixbuf with a number drawn on it
// base: source pixbuf to composite onto
// number: the number to render as centered text
// returns: new GdkPixbuf with the number overlay, or NULL if base is NULL
GdkPixbuf *
texture_create_with_number(GdkPixbuf *base, int number)
{
  if(!base)
    return(NULL);

  int width = gdk_pixbuf_get_width(base);
  int height = gdk_pixbuf_get_height(base);

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(surface);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gdk_cairo_set_source_pixbuf(cr, base, 0, 0);
  G_GNUC_END_IGNORE_DEPRECATIONS
  cairo_paint(cr);

  char text[4];

  snprintf(text, sizeof(text), "%d", number);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 24);

  cairo_text_extents_t extents;

  cairo_text_extents(cr, text, &extents);
  double x = (width - extents.width) / 2 - extents.x_bearing;
  double y = (height - extents.height) / 2 - extents.y_bearing;

  // outer glow/shadow
  cairo_set_source_rgba(cr, 0, 0, 0, 0.8);

  for(int dx = -1; dx <= 1; dx++)
  {
    for(int dy = -1; dy <= 1; dy++)
    {
      if(dx == 0 && dy == 0)
        continue;

      cairo_move_to(cr, x + dx, y + dy);
      cairo_show_text(cr, text);
    }
  }

  // white text
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, text);

  cairo_destroy(cr);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GdkPixbuf *result = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
  G_GNUC_END_IGNORE_DEPRECATIONS

  cairo_surface_destroy(surface);

  return(result);
}
