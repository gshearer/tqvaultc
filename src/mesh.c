#include "mesh.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Little-endian readers (mesh files are always little-endian).
static uint32_t
rd_u32(const uint8_t *p)
{
  return((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint16_t
rd_u16(const uint8_t *p)
{
  return((uint16_t)(p[0] | (p[1] << 8)));
}

static float
rd_f32(const uint8_t *p)
{
  uint32_t u = rd_u32(p);
  float f;

  memcpy(&f, &u, sizeof(f));
  return(f);
}

// Standard skinned-vertex attribute byte offsets within the stride.
#define MESH_OFF_POS     0
#define MESH_OFF_UV     12
#define MESH_OFF_NORMAL 20

static void
free_skin(TQMeshSkin *s)
{
  if(!s)
    return;
  free(s->bone_names);
  free(s->bone_parent);
  free(s->bone_local);
  if(s->group_palette)
  {
    for(int g = 0; g < s->num_groups; g++)
      free(s->group_palette[g]);
    free(s->group_palette);
  }
  free(s->group_pal_len);
  free(s->vtx_bone);
  free(s->vtx_weight);
  free(s->vtx_group);
  free(s);
}

void
tq_mesh_free(TQMesh *m)
{
  if(!m)
    return;
  free(m->verts);
  free(m->indices);
  free(m->mif_path);
  free(m->material_texture);
  free_skin(m->skin);
  free(m);
}

// True if `s` ends in ".tex" (case-insensitive); no glib dependency.
static bool
ends_with_tex(const char *s)
{
  size_t n = strlen(s);

  if(n < 4)
    return(false);

  const char *e = s + n - 4;

  return(e[0] == '.' &&
         (e[1] == 't' || e[1] == 'T') &&
         (e[2] == 'e' || e[2] == 'E') &&
         (e[3] == 'x' || e[3] == 'X'));
}

// Read a length-prefixed string ([u32 len][len bytes]) from `data` at `at`.
// Returns a malloc'd NUL-terminated copy, or NULL if the bounds are implausible.
static char *
read_lp_string(const uint8_t *data, uint32_t size, uint32_t at)
{
  if(at + 4 > size)
    return(NULL);

  uint32_t len = rd_u32(data + at);

  if(len == 0 || len > 1024 || (uint64_t)at + 4 + len > size)
    return(NULL);

  char *s = malloc(len + 1);

  if(!s)
    return(NULL);

  memcpy(s, data + at + 4, len);
  s[len] = '\0';
  return(s);
}

// Parse the chunk-7 material footer for the model's own `baseTexture` (its
// diffuse .tex).  The value follows the length-prefixed key, after a 4-byte
// type tag in the observed files; we try with and without the tag and keep
// whichever yields a real .tex path.  Sets m->material_texture, or leaves it
// NULL.
static void
parse_material(TQMesh *m, const uint8_t *data, uint32_t size)
{
  static const char KEY[] = "baseTexture";
  const uint32_t klen = (uint32_t)(sizeof(KEY) - 1);

  if(size < klen)
    return;

  for(uint32_t i = 0; i + klen <= size; i++)
  {
    if(memcmp(data + i, KEY, klen) != 0)
      continue;

    uint32_t after = i + klen;
    char *with_tag = read_lp_string(data, size, after + 4);  // observed layout
    char *no_tag   = read_lp_string(data, size, after);      // defensive
    char *pick = NULL;

    if(with_tag && ends_with_tex(with_tag))
      pick = with_tag;
    else if(no_tag && ends_with_tex(no_tag))
      pick = no_tag;

    if(with_tag && with_tag != pick)
      free(with_tag);
    if(no_tag && no_tag != pick)
      free(no_tag);

    m->material_texture = pick;   // may be NULL
    return;
  }
}

// Decode the first vertex chunk (type 4) into m->verts.
static bool
parse_vertices(TQMesh *m, const uint8_t *data, uint32_t size)
{
  if(size < 16)
    return(false);

  uint32_t stride = rd_u32(data + 4);
  uint32_t count  = rd_u32(data + 8);

  // Sanity: a plausible interleaved stride that actually fits the chunk.
  if(stride < 20 || stride > 4096 || count == 0)
    return(false);
  if((uint64_t)count * stride > size)
    return(false);

  // The vertices occupy the trailing count*stride bytes of the chunk; whatever
  // precedes them (header + vertex-element declaration) we don't need.
  const uint8_t *vp = data + size - (size_t)count * stride;
  bool have_normal = (stride >= MESH_OFF_NORMAL + 12);

  m->verts = calloc(count, sizeof(*m->verts));
  if(!m->verts)
    return(false);
  m->num_verts = count;

  for(uint32_t i = 0; i < count; i++)
  {
    const uint8_t *v = vp + (size_t)i * stride;
    TQMeshVertex *out = &m->verts[i];

    out->px = rd_f32(v + MESH_OFF_POS + 0);
    out->py = rd_f32(v + MESH_OFF_POS + 4);
    out->pz = rd_f32(v + MESH_OFF_POS + 8);
    out->u  = rd_f32(v + MESH_OFF_UV + 0);
    out->v  = rd_f32(v + MESH_OFF_UV + 4);

    if(have_normal)
    {
      out->nx = rd_f32(v + MESH_OFF_NORMAL + 0);
      out->ny = rd_f32(v + MESH_OFF_NORMAL + 4);
      out->nz = rd_f32(v + MESH_OFF_NORMAL + 8);
    }
  }

  return(true);
}

// Decode the first index chunk (type 5) into m->indices.  Requires the vertex
// count to already be known so we can drop out-of-range / degenerate triangles.
static bool
parse_indices(TQMesh *m, const uint8_t *data, uint32_t size)
{
  if(size < 8 || m->num_verts == 0)
    return(false);

  uint32_t num_tris = rd_u32(data + 0);  // total across all material groups
  const uint8_t *ip = data + 8;          // [numTris][numGroups] then u16 triples

  // Clamp to what the chunk can actually hold.
  uint32_t max_tris = (size - 8) / 6;

  if(num_tris > max_tris)
    num_tris = max_tris;
  if(num_tris == 0)
    return(false);

  m->indices = malloc((size_t)num_tris * 3 * sizeof(*m->indices));
  if(!m->indices)
    return(false);

  uint32_t out = 0;

  for(uint32_t t = 0; t < num_tris; t++)
  {
    uint32_t a = rd_u16(ip + (size_t)t * 6 + 0);
    uint32_t b = rd_u16(ip + (size_t)t * 6 + 2);
    uint32_t c = rd_u16(ip + (size_t)t * 6 + 4);

    if(a >= m->num_verts || b >= m->num_verts || c >= m->num_verts)
      continue;                       // out of range -> skip
    if(a == b || b == c || a == c)
      continue;                       // degenerate -> skip

    m->indices[out * 3 + 0] = a;
    m->indices[out * 3 + 1] = b;
    m->indices[out * 3 + 2] = c;
    out++;
  }

  // Every triangle was out-of-range or degenerate.  The caller keys off the
  // return value and leaves have_idx false, so a later type-5 chunk would
  // overwrite m->indices and leak this buffer -- a failed parse has to leave
  // the mesh as it found it.
  if(out == 0)
  {
    free(m->indices);
    m->indices = NULL;
    return(false);
  }

  m->num_tris = out;
  return(true);
}

static void
compute_bounds(TQMesh *m)
{
  m->bbmin[0] = m->bbmin[1] = m->bbmin[2] =  INFINITY;
  m->bbmax[0] = m->bbmax[1] = m->bbmax[2] = -INFINITY;

  for(uint32_t i = 0; i < m->num_verts; i++)
  {
    float p[3] = { m->verts[i].px, m->verts[i].py, m->verts[i].pz };

    for(int k = 0; k < 3; k++)
    {
      if(p[k] < m->bbmin[k])
        m->bbmin[k] = p[k];
      if(p[k] > m->bbmax[k])
        m->bbmax[k] = p[k];
    }
  }
}

// Parse the optional skeletal-skinning data: the chunk-6 skeleton, the
// per-group bone palettes at the tail of the chunk-5 index buffer, and the
// per-vertex bone indices/weights inside the chunk-4 vertex buffer.  On any
// missing or malformed piece we leave m->skin == NULL and the renderer keeps
// the bind pose (v1 behaviour).  Requires m->num_verts to already be set.
static void
build_skin(TQMesh *m, const uint8_t *c4, uint32_t s4,
           const uint8_t *c5, uint32_t s5,
           const uint8_t *c6, uint32_t s6)
{
  // --- vertex bone indices(@32) + weights(@36) need a 52+ byte stride ---
  if(!c4 || !c5 || !c6 || s4 < 16)
    return;

  uint32_t stride = rd_u32(c4 + 4);
  uint32_t count  = rd_u32(c4 + 8);

  if(count != m->num_verts || stride < 52)
    return;
  if((uint64_t)count * stride > s4)
    return;

  const uint8_t *vp = c4 + s4 - (size_t)count * stride;

  // --- skeleton (chunk 6): [u32 count] + count*88-byte records ---
  if(s6 < 4)
    return;

  uint32_t nb = rd_u32(c6);

  if(nb == 0 || nb > 4096 || 4 + (uint64_t)nb * 88 > s6)
    return;

  // --- index buffer (chunk 5): [u32 numTris][u32 numGroups] + tris + groups ---
  if(s5 < 8)
    return;

  uint32_t num_tris   = rd_u32(c5);
  uint32_t num_groups = rd_u32(c5 + 4);

  if(num_groups == 0 || num_groups > 4096)
    return;

  uint64_t tri_bytes = (uint64_t)num_tris * 6;

  if(8 + tri_bytes > s5)
    return;

  TQMeshSkin *sk = calloc(1, sizeof(*sk));

  if(!sk)
    return;

  // -- skeleton bones --
  sk->num_bones   = (int)nb;
  sk->bone_names  = calloc(nb, sizeof(*sk->bone_names));
  sk->bone_parent = malloc((size_t)nb * sizeof(*sk->bone_parent));
  sk->bone_local  = malloc((size_t)nb * 12 * sizeof(float));

  uint32_t *first_child = malloc((size_t)nb * sizeof(uint32_t));
  uint32_t *child_count = malloc((size_t)nb * sizeof(uint32_t));

  if(!sk->bone_names || !sk->bone_parent || !sk->bone_local ||
     !first_child || !child_count)
  {
    free(first_child);
    free(child_count);
    free_skin(sk);
    return;
  }

  for(uint32_t i = 0; i < nb; i++)
  {
    const uint8_t *p = c6 + 4 + (size_t)i * 88;

    memcpy(sk->bone_names[i], p, 32);
    sk->bone_names[i][32] = '\0';
    first_child[i] = rd_u32(p + 32);
    child_count[i] = rd_u32(p + 36);
    for(int k = 0; k < 12; k++)
      sk->bone_local[i * 12 + k] = rd_f32(p + 40 + k * 4);
    sk->bone_parent[i] = -1;
  }

  // Hierarchy: bone i's children are [first_child[i] .. +child_count[i]).
  for(uint32_t i = 0; i < nb; i++)
    for(uint32_t c = 0; c < child_count[i]; c++)
    {
      uint32_t ch = first_child[i] + c;

      if(ch < nb)
        sk->bone_parent[ch] = (int)i;
    }

  free(first_child);
  free(child_count);

  // -- per-group records (after the triangle list): each is
  //    4 u32 (matId, startTri, numTris, ?) + 6 f32 bounds + u32 palCount +
  //    palCount u32 skeleton bone indices. --
  sk->num_groups    = (int)num_groups;
  sk->group_palette = calloc(num_groups, sizeof(int *));
  sk->group_pal_len = calloc(num_groups, sizeof(int));

  int *g_start = malloc((size_t)num_groups * sizeof(int));
  int *g_ntris = malloc((size_t)num_groups * sizeof(int));

  if(!sk->group_palette || !sk->group_pal_len || !g_start || !g_ntris)
  {
    free(g_start);
    free(g_ntris);
    free_skin(sk);
    return;
  }

  const uint8_t *g    = c5 + 8 + (size_t)tri_bytes;
  const uint8_t *gend = c5 + s5;
  bool ok = true;

  for(uint32_t grp = 0; grp < num_groups; grp++)
  {
    if(g + 4 * 4 + 6 * 4 + 4 > gend)
    {
      ok = false;
      break;
    }

    g_start[grp] = (int)rd_u32(g + 4);
    g_ntris[grp] = (int)rd_u32(g + 8);
    g += 4 * 4 + 6 * 4;

    uint32_t pc = rd_u32(g);
    g += 4;

    if(pc > 100000 || g + (size_t)pc * 4 > gend)
    {
      ok = false;
      break;
    }

    sk->group_pal_len[grp] = (int)pc;
    sk->group_palette[grp] = malloc((size_t)pc * sizeof(int));

    if(!sk->group_palette[grp])
    {
      ok = false;
      break;
    }

    for(uint32_t k = 0; k < pc; k++)
    {
      sk->group_palette[grp][k] = (int)rd_u32(g);
      g += 4;
    }
  }

  if(!ok)
  {
    free(g_start);
    free(g_ntris);
    free_skin(sk);
    return;
  }

  // -- per-vertex bone indices/weights --
  sk->vtx_bone   = malloc((size_t)count * sizeof(*sk->vtx_bone));
  sk->vtx_weight = malloc((size_t)count * sizeof(*sk->vtx_weight));
  sk->vtx_group  = calloc(count, sizeof(int));

  if(!sk->vtx_bone || !sk->vtx_weight || !sk->vtx_group)
  {
    free(g_start);
    free(g_ntris);
    free_skin(sk);
    return;
  }

  for(uint32_t i = 0; i < count; i++)
  {
    const uint8_t *x = vp + (size_t)i * stride;

    for(int k = 0; k < 4; k++)
    {
      sk->vtx_bone[i][k]   = x[32 + k];
      sk->vtx_weight[i][k] = rd_f32(x + 36 + k * 4);
    }
  }

  // Assign each vertex to its material group via the RAW triangle list (the
  // rendered m->indices drops degenerate tris, so re-read the raw triangles).
  for(int grp = 0; grp < (int)num_groups; grp++)
  {
    int t1 = g_start[grp] + g_ntris[grp];

    for(int t = g_start[grp]; t < t1 && (uint32_t)t < num_tris; t++)
    {
      if(t < 0)
        continue;

      const uint8_t *tp = c5 + 8 + (size_t)t * 6;

      for(int k = 0; k < 3; k++)
      {
        uint32_t v = rd_u16(tp + k * 2);

        if(v < count)
          sk->vtx_group[v] = grp;
      }
    }
  }

  free(g_start);
  free(g_ntris);

  m->skin = sk;
}

float
tq_mesh_suggest_yaw(const TQMesh *m)
{
  if(!m)
    return(20.0f);

  float xext = m->bbmax[0] - m->bbmin[0];
  float zext = m->bbmax[2] - m->bbmin[2];

  // Look down the narrower horizontal axis (presents the wider one as width),
  // plus a 20-degree turn so the result is a three-quarter rather than a flat
  // head-on view.
  float base = (xext >= zext) ? 0.0f : 90.0f;

  return(base + 20.0f);
}

TQMesh *
tq_mesh_parse(const uint8_t *data, size_t len)
{
  if(!data || len < 8 || memcmp(data, "MSH", 3) != 0)
    return(NULL);

  TQMesh *m = calloc(1, sizeof(*m));

  if(!m)
    return(NULL);

  bool have_verts = false;
  bool have_idx   = false;
  size_t off = 4;   // skip "MSH" + version byte

  // Raw chunk pointers retained for the optional skin pass (built once at the
  // end, when num_verts is known): the vertex (4), index (5) and skeleton (6)
  // chunks the renderer / skinner draw from.
  const uint8_t *c4 = NULL, *c5 = NULL, *c6 = NULL;
  uint32_t s4 = 0, s5 = 0, s6 = 0;

  // Walk the flat chunk list.  We want the first vertex chunk and the first
  // index chunk; everything else (material name, attach-point text, bbox) is
  // skipped.  Vertices must be parsed before indices (range checking).
  while(off + 8 <= len)
  {
    uint32_t type = rd_u32(data + off);
    uint32_t size = rd_u32(data + off + 4);
    const uint8_t *cdata = data + off + 8;

    if((uint64_t)off + 8 + size > len)
      break;                          // truncated / malformed -> stop

    if(type == 0 && !m->mif_path && size >= 4)
    {
      uint32_t slen = rd_u32(cdata);

      if(slen > 0 && 4 + (uint64_t)slen <= size)
      {
        m->mif_path = malloc(slen + 1);
        if(m->mif_path)
        {
          memcpy(m->mif_path, cdata + 4, slen);
          m->mif_path[slen] = '\0';
        }
      }
    }
    else if(type == 4 && !have_verts)
    {
      have_verts = parse_vertices(m, cdata, size);
      c4 = cdata;
      s4 = size;
    }
    else if(type == 5 && have_verts && !have_idx)
    {
      have_idx = parse_indices(m, cdata, size);
      c5 = cdata;
      s5 = size;
    }
    else if(type == 6 && !c6)
    {
      c6 = cdata;
      s6 = size;
    }
    else if(type == 7 && !m->material_texture)
    {
      parse_material(m, cdata, size);
    }

    off += 8 + size;
  }

  if(!have_verts || !have_idx)
  {
    tq_mesh_free(m);
    return(NULL);
  }

  compute_bounds(m);
  build_skin(m, c4, s4, c5, s5, c6, s6);   // optional; leaves m->skin NULL if absent
  return(m);
}
