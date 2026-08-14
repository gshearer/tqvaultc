// tq-dbr-tool: .arc archive commands (text search, listing, hex dump,
// extraction) and the software mesh-render harness.

#include "tq_dbr_tool.h"
#include "../arc.h"
#include "../texture.h"
#include "../mesh.h"
#include "../mesh_render.h"
#include "../mesh_skin.h"
#include "../anm.h"
#include "../parse_num.h"

// Searches for text in an arc text file, with UTF-16 awareness.
// arc_path: path to the .arc archive file.
// search_term: text to search for (case-insensitive).
// Returns 0 on success, 1 on failure.
int
cmd_arctxt(const char *arc_path, const char *search_term)
{
  TQArcFile *arc = arc_load(arc_path);
  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  char *lower_search = g_ascii_strdown(search_term, -1);
  int total_matches = 0;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    size_t size;
    uint8_t *data = arc_extract_file(arc, i, &size);

    if(!data)
      continue;

    // Convert to UTF-8 if UTF-16LE BOM detected
    char *content = NULL;
    bool content_is_glib = false;

    if(size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
      GError *err = NULL;
      gsize bw;

      content = g_convert((const gchar*)(data+2), size-2,
                          "UTF-8", "UTF-16LE", NULL, &bw, &err);

      if(!content)
      {
        if(err)
        {
          fprintf(stderr, "Warning: encoding error in %s: %s\n",
                  arc->entries[i].path, err->message);
          g_error_free(err);
        }

        free(data);
        continue;
      }

      content_is_glib = true;
    }
    else
    {
      content = malloc(size + 1);
      if(!content)
      {
        free(data);
        continue;
      }

      memcpy(content, data, size);
      content[size] = '\0';
    }

    free(data);

    // Case-insensitive search
    char *lower = g_ascii_strdown(content, -1);
    char *pos = lower;

    while((pos = strstr(pos, lower_search)) != NULL)
    {
      int offset = pos - lower;

      // Find line boundaries in original content
      int ls = offset;

      while(ls > 0 && content[ls-1] != '\n')
        ls--;

      int le = offset;

      while(content[le] && content[le] != '\n' && content[le] != '\r')
        le++;

      printf("[%s] %.*s\n", arc->entries[i].path, le - ls, content + ls);
      total_matches++;
      pos++;
    }

    g_free(lower);

    if(content_is_glib)
      g_free(content);
    else
      free(content);
  }

  printf("\n%d matches found.\n", total_matches);

  g_free(lower_search);
  arc_free(arc);
  return(0);
}

// Lists all files in an arc archive with their sizes.
// arc_path: path to the .arc archive file.
// Returns 0 on success, 1 on failure.
int
cmd_arcls(const char *arc_path)
{
  TQArcFile *arc = arc_load(arc_path);
  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  for(uint32_t i = 0; i < arc->num_files; i++)
    printf("%s (%u bytes)\n", arc->entries[i].path, arc->entries[i].real_size);

  printf("\n%u files total.\n", arc->num_files);
  arc_free(arc);
  return(0);
}

// Extracts a file from an arc archive and prints a hex dump.
// arc_path: path to the .arc archive file.
// file_pattern: case-insensitive substring to match against file paths in the archive.
// Returns 0 on success, 1 if pattern matches zero or multiple files.
int
cmd_archex(const char *arc_path, const char *file_pattern)
{
  TQArcFile *arc = arc_load(arc_path);
  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  // Case-insensitive substring match (normalize / to backslash for arc paths)
  char *lower_pattern = g_ascii_strdown(file_pattern, -1);

  for(char *p = lower_pattern; *p; p++)
    if(*p == '/')
      *p = '\\';

  int match_idx = -1;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    char *lower_path = g_ascii_strdown(arc->entries[i].path, -1);

    if(strstr(lower_path, lower_pattern))
    {
      if(match_idx >= 0)
      {
        fprintf(stderr, "Pattern '%s' matches multiple files:\n", file_pattern);
        // Print the previous match
        fprintf(stderr, "  %s\n", arc->entries[match_idx].path);
        // Print this match and all remaining
        fprintf(stderr, "  %s\n", arc->entries[i].path);
        g_free(lower_path);

        for(uint32_t j = i + 1; j < arc->num_files; j++)
        {
          char *lp = g_ascii_strdown(arc->entries[j].path, -1);

          if(strstr(lp, lower_pattern))
            fprintf(stderr, "  %s\n", arc->entries[j].path);

          g_free(lp);
        }

        fprintf(stderr, "Use a more specific pattern.\n");
        g_free(lower_pattern);
        arc_free(arc);
        return(1);
      }

      match_idx = i;
    }

    g_free(lower_path);
  }

  if(match_idx < 0)
  {
    fprintf(stderr, "No file matching '%s' in %s\n", file_pattern, arc_path);
    g_free(lower_pattern);
    arc_free(arc);
    return(1);
  }

  printf("File: %s (%u bytes)\n\n", arc->entries[match_idx].path,
         arc->entries[match_idx].real_size);

  size_t size;
  uint8_t *data = arc_extract_file(arc, match_idx, &size);

  if(!data)
  {
    fprintf(stderr, "Failed to extract file\n");
    g_free(lower_pattern);
    arc_free(arc);
    return(1);
  }

  // Hex dump: 16 bytes per line with ASCII sidebar
  for(size_t off = 0; off < size; off += 16)
  {
    printf("%08zx  ", off);
    size_t n = (size - off < 16) ? size - off : 16;

    for(size_t j = 0; j < 16; j++)
    {
      if(j < n)
        printf("%02x ", data[off + j]);
      else
        printf("   ");

      if(j == 7)
        printf(" ");
    }

    printf(" |");

    for(size_t j = 0; j < n; j++)
    {
      uint8_t c = data[off + j];
      printf("%c", (c >= 0x20 && c <= 0x7e) ? c : '.');
    }

    printf("|\n");
  }

  printf("\n%zu bytes total.\n", size);

  free(data);
  g_free(lower_pattern);
  arc_free(arc);
  return(0);
}

// arcextract: extract a single (uniquely-matched) raw file from an arc to disk.
// Mirrors cmd_archex's matching, but writes the raw bytes instead of hex.
int
cmd_arcextract(const char *arc_path, const char *file_pattern, const char *out_path)
{
  TQArcFile *arc = arc_load(arc_path);

  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  char *lower_pattern = g_ascii_strdown(file_pattern, -1);

  for(char *p = lower_pattern; *p; p++)
    if(*p == '/')
      *p = '\\';

  int match_idx = -1;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    char *lower_path = g_ascii_strdown(arc->entries[i].path, -1);

    if(strstr(lower_path, lower_pattern))
    {
      if(match_idx >= 0)
      {
        fprintf(stderr, "Pattern '%s' matches multiple files; be more specific.\n",
                file_pattern);
        g_free(lower_path);
        g_free(lower_pattern);
        arc_free(arc);
        return(1);
      }
      match_idx = i;
    }

    g_free(lower_path);
  }

  g_free(lower_pattern);

  if(match_idx < 0)
  {
    fprintf(stderr, "No file matching '%s' in %s\n", file_pattern, arc_path);
    arc_free(arc);
    return(1);
  }

  size_t size;
  uint8_t *data = arc_extract_file(arc, (uint32_t)match_idx, &size);

  if(!data)
  {
    fprintf(stderr, "Failed to extract file\n");
    arc_free(arc);
    return(1);
  }

  FILE *f = fopen(out_path, "wb");

  if(!f || fwrite(data, 1, size, f) != size)
  {
    fprintf(stderr, "Failed to write %s\n", out_path);
    if(f)
      fclose(f);
    free(data);
    arc_free(arc);
    return(1);
  }

  fclose(f);
  printf("Wrote %s (%zu bytes) from %s\n", out_path, size,
         arc->entries[match_idx].path);
  free(data);
  arc_free(arc);
  return(0);
}

// Find the unique arc entry whose path contains `pattern` (case-insensitive).
// Returns the entry index, or -1 if there is no match or more than one.
static int
arc_find_unique(TQArcFile *arc, const char *pattern)
{
  char *lower = g_ascii_strdown(pattern, -1);
  int match = -1;

  for(uint32_t i = 0; i < arc->num_files; i++)
  {
    char *lp = g_ascii_strdown(arc->entries[i].path, -1);

    if(strstr(lp, lower))
      match = (match >= 0) ? -2 : (int)i;
    g_free(lp);
    if(match == -2)
      break;
  }

  g_free(lower);
  return(match < 0 ? -1 : match);
}

// meshrender: parse a .msh model from an arc and software-rasterize it (textured
// with a .tex from the same arc) into a PNG.  A prototype harness for the
// Database Browser creature-thumbnail feature.
int
cmd_meshrender(int argc, char **argv)
{
  // argv: <arc> <mesh_substr> <tex_substr> <out.png> [size] [yaw] [pitch]
  //       [anm_substr|-] [frame]
  const char *arc_path = argv[2];
  const char *mesh_pat = argv[3];
  const char *tex_pat  = argv[4];
  const char *out_path = argv[5];
  int   size  = 256;
  // yaw: a number, or "auto" (the default) to orient from the bounding box.
  bool  auto_yaw = (argc <= 7) || strcmp(argv[7], "auto") == 0;
  float yaw   = 0.0f;
  float pitch = 12.0f;
  // Optional skeletal pose: an .anm substring + frame (0 if omitted).
  const char *anm_pat = (argc > 9) ? argv[9] : NULL;
  int   frame = 0;

  if((argc > 6  && !parse_int(argv[6], &size))     ||
     (!auto_yaw && !parse_float(argv[7], &yaw))    ||
     (argc > 8  && !parse_float(argv[8], &pitch))  ||
     (argc > 10 && !parse_int(argv[10], &frame)))
  {
    fprintf(stderr, "meshrender: size/yaw/pitch/frame must be numbers\n");
    return(1);
  }

  if(size < 8 || size > 2048)
    size = 256;

  TQArcFile *arc = arc_load(arc_path);

  if(!arc)
  {
    fprintf(stderr, "Failed to load ARC: %s\n", arc_path);
    return(1);
  }

  int mi = arc_find_unique(arc, mesh_pat);

  if(mi < 0)
  {
    fprintf(stderr, "mesh pattern '%s' did not match exactly one file\n", mesh_pat);
    arc_free(arc);
    return(1);
  }

  size_t msize;
  uint8_t *mdata = arc_extract_file(arc, (uint32_t)mi, &msize);
  TQMesh *mesh = mdata ? tq_mesh_parse(mdata, msize) : NULL;

  free(mdata);

  if(!mesh)
  {
    fprintf(stderr, "Failed to parse mesh %s\n", arc->entries[mi].path);
    arc_free(arc);
    return(1);
  }

  // Optional natural pose: skin the mesh to a frame of an idle .anm from the
  // same arc (recomputes bounds, so it must run before auto-yaw + the print).
  if(anm_pat && anm_pat[0] && strcmp(anm_pat, "-") != 0)
  {
    int ai = arc_find_unique(arc, anm_pat);

    if(ai < 0)
      fprintf(stderr, "warning: anm '%s' not uniquely matched; bind pose\n", anm_pat);
    else if(!mesh->skin)
      fprintf(stderr, "warning: mesh has no skeleton/weights; bind pose\n");
    else
    {
      size_t asize;
      uint8_t *adata = arc_extract_file(arc, (uint32_t)ai, &asize);
      TQAnm *anm = adata ? tq_anm_parse(adata, asize) : NULL;

      free(adata);
      if(!anm)
        fprintf(stderr, "warning: failed to parse anm %s; bind pose\n",
                arc->entries[ai].path);
      else
      {
        if(tq_mesh_pose(mesh, anm, frame))
          printf("Posed with %s frame %d (of %d)\n", arc->entries[ai].path,
                 frame, tq_anm_num_frames(anm));
        tq_anm_free(anm);
      }
    }
  }

  if(auto_yaw)
    yaw = tq_mesh_suggest_yaw(mesh);

  printf("Mesh %s: %u verts, %u tris, bbox [%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
         arc->entries[mi].path, mesh->num_verts, mesh->num_tris,
         mesh->bbmin[0], mesh->bbmin[1], mesh->bbmin[2],
         mesh->bbmax[0], mesh->bbmax[1], mesh->bbmax[2]);

  GdkPixbuf *tex = NULL;

  // An explicit texture pattern is matched in the arc; "-" / omitted falls back
  // to the model's own material texture (chunk-7 baseTexture), mirroring how the
  // app textures creatures whose DBR provides no baseTexture override.
  bool tex_given = (tex_pat && tex_pat[0] && strcmp(tex_pat, "-") != 0);
  const char *tex_match = tex_pat;
  char *mat_base = NULL;

  if(!tex_given && mesh->material_texture)
  {
    // Match the material .tex in the arc by its basename (arc paths differ in
    // separator/case from the game path stored in the mesh).
    const char *b = strrchr(mesh->material_texture, '\\');
    const char *f = strrchr(mesh->material_texture, '/');

    if(f > b)
      b = f;
    mat_base = g_strdup(b ? b + 1 : mesh->material_texture);
    tex_match = mat_base;
    printf("No texture given; using mesh material texture %s\n",
           mesh->material_texture);
  }

  if(tex_match && tex_match[0] && strcmp(tex_match, "-") != 0)
  {
    int ti = arc_find_unique(arc, tex_match);

    if(ti < 0)
      fprintf(stderr, "warning: texture '%s' not uniquely matched; rendering untextured\n",
              tex_match);
    else
    {
      tex = texture_load_by_index(arc, (uint32_t)ti);
      if(!tex)
        fprintf(stderr, "warning: failed to decode texture %s\n", arc->entries[ti].path);
      else
        printf("Texture %s: %dx%d\n", arc->entries[ti].path,
               gdk_pixbuf_get_width(tex), gdk_pixbuf_get_height(tex));
    }
  }

  g_free(mat_base);

  GdkPixbuf *out = tq_mesh_render(mesh, tex, size, yaw, pitch);
  int rc = 0;

  if(!out)
  {
    fprintf(stderr, "Render failed\n");
    rc = 1;
  }
  else
  {
    GError *err = NULL;

    if(!gdk_pixbuf_save(out, out_path, "png", &err, NULL))
    {
      fprintf(stderr, "Failed to save %s: %s\n", out_path,
              err ? err->message : "?");
      if(err)
        g_error_free(err);
      rc = 1;
    }
    else
      printf("Wrote %s (%dx%d, yaw=%.0f pitch=%.0f)\n", out_path, size, size,
             yaw, pitch);
    g_object_unref(out);
  }

  if(tex)
    g_object_unref(tex);
  tq_mesh_free(mesh);
  arc_free(arc);
  return(rc);
}

