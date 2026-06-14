#include "mesh_render.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Supersampling factor: render this many times larger, then box-downscale for
// cheap anti-aliasing.  Internal resolution is capped (see below) to bound the
// per-creature cost when the whole bestiary is rendered at startup.
#define MESH_SS         4
#define MESH_SS_MAXDIM  1024

// A vertex after model transform: rotated view-space coords, screen pixel
// position, texcoord and a per-vertex shade factor.
typedef struct {
  float rx, ry;    // rotated coords used for framing (pre-projection)
  float sx, sy;    // screen pixel position
  float depth;     // view-space depth (larger = nearer the camera)
  float u, v;      // texcoord
  float shade;     // lighting term (may exceed 1; clamped at write)
} RVert;

// Free callback matching GdkPixbufDestroyNotify's signature (pixels, data).
static void
free_pixels(guchar *pixels, gpointer data)
{
  (void)data;
  g_free(pixels);
}

static inline float
edge(float ax, float ay, float bx, float by, float px, float py)
{
  return((px - ax) * (by - ay) - (py - ay) * (bx - ax));
}

// Sample `tex` (RGB or RGBA pixbuf) at normalized (u,v), bilinear.  V is
// flipped to match the texture's top-left origin.  Writes RGB 0..1.
static void
sample_tex(GdkPixbuf *tex, float u, float v, float out[3])
{
  if(!tex)
  {
    out[0] = out[1] = out[2] = 0.72f;
    return;
  }

  int tw = gdk_pixbuf_get_width(tex);
  int th = gdk_pixbuf_get_height(tex);
  int nc = gdk_pixbuf_get_n_channels(tex);
  int rs = gdk_pixbuf_get_rowstride(tex);
  const guchar *px = gdk_pixbuf_get_pixels(tex);

  // Wrap into [0,1) so tiled UVs (u can exceed 1) still sample sensibly.
  u = u - floorf(u);
  v = 1.0f - (v - floorf(v));

  float fx = u * tw - 0.5f;
  float fy = v * th - 0.5f;
  int x0 = (int)floorf(fx);
  int y0 = (int)floorf(fy);
  float dx = fx - x0;
  float dy = fy - y0;

  out[0] = out[1] = out[2] = 0.0f;

  for(int j = 0; j < 2; j++)
  {
    for(int i = 0; i < 2; i++)
    {
      int sx = x0 + i, sy = y0 + j;

      // Clamp to edge.
      if(sx < 0)
        sx = 0;
      if(sx >= tw)
        sx = tw - 1;
      if(sy < 0)
        sy = 0;
      if(sy >= th)
        sy = th - 1;

      const guchar *p = px + (size_t)sy * rs + (size_t)sx * nc;
      float w = (i ? dx : 1.0f - dx) * (j ? dy : 1.0f - dy);

      out[0] += w * p[0] / 255.0f;
      out[1] += w * p[1] / 255.0f;
      out[2] += w * p[2] / 255.0f;
    }
  }
}

// Render the mesh into an `S`x`S` RGBA buffer (caller owns it via the returned
// pixbuf).  Internal; tq_mesh_render wraps this with supersampling.
static guchar *
render_at(const TQMesh *m, GdkPixbuf *texture, int S,
          float yaw_deg, float pitch_deg)
{
  float cx = 0.5f * (m->bbmin[0] + m->bbmax[0]);
  float cy = 0.5f * (m->bbmin[1] + m->bbmax[1]);
  float cz = 0.5f * (m->bbmin[2] + m->bbmax[2]);

  float yaw = yaw_deg * (float)M_PI / 180.0f;
  float pit = pitch_deg * (float)M_PI / 180.0f;
  float cyaw = cosf(yaw), syaw = sinf(yaw);
  float cpit = cosf(pit), spit = sinf(pit);

  // Key + fill light directions (view space), normalized.
  float k[3] = { -0.35f, 0.55f, 0.78f };
  float f[3] = {  0.55f, 0.10f, 0.30f };

  for(int i = 0; i < 1; i++)   // normalize both
  {
    float kl = sqrtf(k[0]*k[0] + k[1]*k[1] + k[2]*k[2]);
    float fl = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);

    for(int c = 0; c < 3; c++)
    {
      k[c] /= kl;
      f[c] /= fl;
    }
  }

  RVert *rv = malloc((size_t)m->num_verts * sizeof(*rv));
  float *zbuf = malloc((size_t)S * S * sizeof(*zbuf));
  guchar *pix = calloc((size_t)S * S, 4);

  if(!rv || !zbuf || !pix)
  {
    free(rv);
    free(zbuf);
    free(pix);
    return(NULL);
  }

  for(int i = 0; i < S * S; i++)
    zbuf[i] = -INFINITY;

  // Pass 1: rotate every vertex; track the projected (rx,ry) extent for tight
  // framing, and shade from the rotated normal.
  float minx =  INFINITY, maxx = -INFINITY;
  float miny =  INFINITY, maxy = -INFINITY;

  for(uint32_t i = 0; i < m->num_verts; i++)
  {
    const TQMeshVertex *vtx = &m->verts[i];
    float x = vtx->px - cx, y = vtx->py - cy, z = vtx->pz - cz;

    float x1 =  cyaw * x + syaw * z;
    float z1 = -syaw * x + cyaw * z;
    float y2 =  cpit * y - spit * z1;
    float z2 =  spit * y + cpit * z1;

    rv[i].rx = x1;
    rv[i].ry = y2;
    rv[i].depth = z2;
    rv[i].u = vtx->u;
    rv[i].v = vtx->v;

    if(x1 < minx) minx = x1;
    if(x1 > maxx) maxx = x1;
    if(y2 < miny) miny = y2;
    if(y2 > maxy) maxy = y2;

    float nx = vtx->nx, ny = vtx->ny, nz = vtx->nz;
    float nl = sqrtf(nx*nx + ny*ny + nz*nz);

    if(nl > 1e-6f)
    {
      nx /= nl;
      ny /= nl;
      nz /= nl;
    }

    float rnx =  cyaw * nx + syaw * nz;
    float rnz =  -syaw * nx + cyaw * nz;
    float rny =  cpit * ny - spit * rnz;
    float rnz2 = spit * ny + cpit * rnz;

    float kd = rnx*k[0] + rny*k[1] + rnz2*k[2];
    float fd = rnx*f[0] + rny*f[1] + rnz2*f[2];

    if(kd < 0.0f) kd = -kd;          // two-sided
    if(fd < 0.0f) fd = -fd;
    rv[i].shade = 0.46f + 0.72f * kd + 0.26f * fd;
  }

  // Fit the projected bounding box into the frame (contain, ~90% with margin).
  float projw = maxx - minx, projh = maxy - miny;
  float span = fmaxf(projw, projh);

  if(span <= 0.0f)
    span = 1.0f;

  float scale = (S * 0.90f) / span;
  float ccx = 0.5f * (minx + maxx);
  float ccy = 0.5f * (miny + maxy);
  float half = S * 0.5f;

  for(uint32_t i = 0; i < m->num_verts; i++)
  {
    rv[i].sx = half + (rv[i].rx - ccx) * scale;
    rv[i].sy = half - (rv[i].ry - ccy) * scale;   // screen Y grows downward
  }

  // Pass 2: rasterize.
  for(uint32_t t = 0; t < m->num_tris; t++)
  {
    RVert *A = &rv[m->indices[t*3 + 0]];
    RVert *B = &rv[m->indices[t*3 + 1]];
    RVert *C = &rv[m->indices[t*3 + 2]];

    float area = edge(A->sx, A->sy, B->sx, B->sy, C->sx, C->sy);

    if(fabsf(area) < 1e-6f)
      continue;

    int x0 = (int)floorf(fminf(A->sx, fminf(B->sx, C->sx)));
    int x1 = (int)ceilf(fmaxf(A->sx, fmaxf(B->sx, C->sx)));
    int y0 = (int)floorf(fminf(A->sy, fminf(B->sy, C->sy)));
    int y1 = (int)ceilf(fmaxf(A->sy, fmaxf(B->sy, C->sy)));

    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 >= S) x1 = S - 1;
    if(y1 >= S) y1 = S - 1;

    float inv_area = 1.0f / area;

    for(int py = y0; py <= y1; py++)
    {
      for(int px = x0; px <= x1; px++)
      {
        float fx = px + 0.5f, fy = py + 0.5f;
        float w0 = edge(B->sx, B->sy, C->sx, C->sy, fx, fy) * inv_area;
        float w1 = edge(C->sx, C->sy, A->sx, A->sy, fx, fy) * inv_area;
        float w2 = edge(A->sx, A->sy, B->sx, B->sy, fx, fy) * inv_area;

        if((w0 < 0 || w1 < 0 || w2 < 0) && (w0 > 0 || w1 > 0 || w2 > 0))
          continue;

        float depth = w0*A->depth + w1*B->depth + w2*C->depth;
        int idx = py * S + px;

        if(depth <= zbuf[idx])
          continue;
        zbuf[idx] = depth;

        float u = w0*A->u + w1*B->u + w2*C->u;
        float v = w0*A->v + w1*B->v + w2*C->v;
        float shade = w0*A->shade + w1*B->shade + w2*C->shade;
        float rgb[3];

        sample_tex(texture, u, v, rgb);

        guchar *o = pix + (size_t)idx * 4;

        o[0] = (guchar)(fminf(rgb[0] * shade, 1.0f) * 255.0f);
        o[1] = (guchar)(fminf(rgb[1] * shade, 1.0f) * 255.0f);
        o[2] = (guchar)(fminf(rgb[2] * shade, 1.0f) * 255.0f);
        o[3] = 255;
      }
    }
  }

  free(rv);
  free(zbuf);
  return(pix);
}

GdkPixbuf *
tq_mesh_render(const TQMesh *m, GdkPixbuf *texture, int size,
               float yaw_deg, float pitch_deg)
{
  if(!m || m->num_verts == 0 || m->num_tris == 0 || size < 8)
    return(NULL);

  float ex = m->bbmax[0] - m->bbmin[0];
  float ey = m->bbmax[1] - m->bbmin[1];
  float ez = m->bbmax[2] - m->bbmin[2];

  if(ex <= 0 && ey <= 0 && ez <= 0)
    return(NULL);

  // Supersample, then downscale for anti-aliasing.
  int S = size * MESH_SS;

  if(S > MESH_SS_MAXDIM)
    S = MESH_SS_MAXDIM;
  if(S < size)
    S = size;

  guchar *pix = render_at(m, texture, S, yaw_deg, pitch_deg);

  if(!pix)
    return(NULL);

  GdkPixbuf *big = gdk_pixbuf_new_from_data(pix, GDK_COLORSPACE_RGB, TRUE, 8,
                                            S, S, S * 4, free_pixels, NULL);

  if(!big)
  {
    g_free(pix);
    return(NULL);
  }

  if(S == size)
    return(big);

  GdkPixbuf *out = gdk_pixbuf_scale_simple(big, size, size, GDK_INTERP_BILINEAR);

  g_object_unref(big);
  return(out);
}
