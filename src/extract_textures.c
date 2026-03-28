#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include "arc.h"
#include "texture.h"

// make_path - create directory hierarchy
// path: directory path to create (with parents)
static void
make_path(const char *path)
{
  g_mkdir_with_parents(path, 0755);
}

// normalize_to_forward_slashes - replace backslashes with forward slashes
// path: input path string
// returns: newly allocated string with normalized slashes (caller must free)
static char *
normalize_to_forward_slashes(const char *path)
{
  char *res = strdup(path);

  if(!res)
    return(NULL);

  for(int i = 0; res[i]; i++)
  {
    if(res[i] == '\\')
      res[i] = '/';
  }

  return(res);
}

// main - extract .tex textures from an ARC archive and save as PNG files
// argc: argument count
// argv: argument vector; argv[1] = arc file path, argv[2] = optional output dir
// returns: 0 on success, 1 on failure
int
main(int argc, char *argv[])
{
  if(argc < 2)
  {
    printf("Usage: %s <arc_file> [output_dir]\n", argv[0]);
    return(1);
  }

  const char *arc_path = argv[1];
  const char *out_base = (argc > 2) ? argv[2] : "extracted_textures";

  TQArcFile *arc = arc_load(arc_path);

  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  g_mkdir_with_parents(out_base, 0755);

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    const char *entry_path = arc->entries[i].path;

    if(strstr(entry_path, ".tex") || strstr(entry_path, ".TEX"))
    {
      printf("Extracting [%u/%u]: %s\n", i + 1, arc->num_files, entry_path);

      GdkPixbuf *pb = texture_load_by_index(arc, i);

      if(pb)
      {
        char *norm_path = normalize_to_forward_slashes(entry_path);
        char out_path[1024];

        snprintf(out_path, sizeof(out_path), "%s/%s", out_base, norm_path);

        // Change extension to .png
        char *dot = strrchr(out_path, '.');

        if(dot)
          snprintf(dot, sizeof(out_path) - (size_t)(dot - out_path), ".png");

        // Ensure directory exists
        char dir_path[1024];

        strncpy(dir_path, out_path, sizeof(dir_path) - 1);
        dir_path[sizeof(dir_path) - 1] = '\0';
        char *last_slash = strrchr(dir_path, '/');

        if(last_slash)
        {
          *last_slash = '\0';
          make_path(dir_path);
        }

        GError *error = NULL;

        if(!gdk_pixbuf_save(pb, out_path, "png", &error, NULL))
        {
          fprintf(stderr, "Failed to save %s: %s\n", out_path, error ? error->message : "Unknown error");
          if(error)
            g_error_free(error);
        }

        free(norm_path);
        g_object_unref(pb);
      }
      else
      {
        fprintf(stderr, "Failed to load texture at index %u: %s\n", i, entry_path);
      }
    }
  }

  arc_free(arc);
  printf("Extraction complete.\n");
  return(0);
}
