#include "mesh_skin.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 4x4 column-major matrix (translation in m[12..14]).
typedef struct { float m[16]; } Mat;

static Mat
mat_id(void)
{
  Mat r = {{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }};
  return(r);
}

static Mat
mat_mul(Mat a, Mat b)
{
  Mat r;

  for(int c = 0; c < 4; c++)
    for(int rr = 0; rr < 4; rr++)
    {
      float s = 0;

      for(int k = 0; k < 4; k++)
        s += a.m[k * 4 + rr] * b.m[c * 4 + k];
      r.m[c * 4 + rr] = s;
    }
  return(r);
}

// Transform a point (w=1).
static void
mat_xf(const Mat *M, const float *p, float *o)
{
  for(int r = 0; r < 3; r++)
    o[r] = M->m[r] * p[0] + M->m[4 + r] * p[1] + M->m[8 + r] * p[2] + M->m[12 + r];
}

// Transform a direction (w=0); good enough for normals (skin matrices are
// near-rigid, so we skip the inverse-transpose).
static void
mat_xf3(const Mat *M, const float *n, float *o)
{
  for(int r = 0; r < 3; r++)
    o[r] = M->m[r] * n[0] + M->m[4 + r] * n[1] + M->m[8 + r] * n[2];
}

static Mat
mat_inv(Mat a)
{
  float *m = a.m, inv[16], det;

  inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
             m[9]*m[7]*m[14]  + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
  inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14]  - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
  inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13]  + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
  inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
             m[8]*m[6]*m[13]  - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
  inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14]  - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
  inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14]  + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
  inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13]  - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
  inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
             m[8]*m[2]*m[13]  + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
  inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14]  + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
  inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14]  - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
  inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] +
             m[4]*m[3]*m[13]  + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
  inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] -
             m[4]*m[2]*m[13]  - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
  inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10]  - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
  inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10]  + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
  inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] -
             m[4]*m[3]*m[9]   - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
  inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] +
             m[4]*m[2]*m[9]   + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

  det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];

  if(fabsf(det) < 1e-20f)
    return(mat_id());

  det = 1.0f / det;

  Mat r;

  for(int i = 0; i < 16; i++)
    r.m[i] = inv[i] * det;
  return(r);
}

// Rotation matrix from a (normalized) quaternion x,y,z,w.
static Mat
quat_mat(float x, float y, float z, float w)
{
  float n = sqrtf(x*x + y*y + z*z + w*w);

  if(n > 1e-9f)
  {
    x /= n;
    y /= n;
    z /= n;
    w /= n;
  }

  Mat r = mat_id();

  r.m[0] = 1 - 2*(y*y + z*z);  r.m[1] = 2*(x*y + z*w);      r.m[2]  = 2*(x*z - y*w);
  r.m[4] = 2*(x*y - z*w);      r.m[5] = 1 - 2*(x*x + z*z);  r.m[6]  = 2*(y*z + x*w);
  r.m[8] = 2*(x*z + y*w);      r.m[9] = 2*(y*z - x*w);      r.m[10] = 1 - 2*(x*x + y*y);
  return(r);
}

// Local bind transform of bone i as a matrix (axes in columns, origin in m[12..]).
static Mat
bone_local_mat(const TQMeshSkin *sk, int i)
{
  const float *L = &sk->bone_local[i * 12];
  Mat M = mat_id();

  M.m[0]  = L[0];  M.m[1]  = L[1];  M.m[2]  = L[2];
  M.m[4]  = L[3];  M.m[5]  = L[4];  M.m[6]  = L[5];
  M.m[8]  = L[6];  M.m[9]  = L[7];  M.m[10] = L[8];
  M.m[12] = L[9];  M.m[13] = L[10]; M.m[14] = L[11];
  return(M);
}

bool
tq_mesh_pose(TQMesh *m, const TQAnm *anm, int frame)
{
  if(!m || !m->skin || !anm)
    return(false);

  TQMeshSkin *sk = m->skin;
  int nb = sk->num_bones;

  if(nb <= 0)
    return(false);

  Mat *local_bind = malloc((size_t)nb * sizeof(Mat));
  Mat *anim_local = malloc((size_t)nb * sizeof(Mat));
  Mat *bind_world = malloc((size_t)nb * sizeof(Mat));
  Mat *anim_world = malloc((size_t)nb * sizeof(Mat));
  Mat *skin       = malloc((size_t)nb * sizeof(Mat));
  char *done      = calloc((size_t)nb, 1);

  if(!local_bind || !anim_local || !bind_world || !anim_world || !skin || !done)
  {
    free(local_bind);
    free(anim_local);
    free(bind_world);
    free(anim_world);
    free(skin);
    free(done);
    return(false);
  }

  // Local transforms: bind, then overlaid with the clip's per-bone delta.
  // animLocal.rot   = bindRot * conjugate(anmQuat)  (quat conjugated: TQ handedness)
  // animLocal.trans = bindOrigin + anmTrans
  for(int i = 0; i < nb; i++)
  {
    Mat b = bone_local_mat(sk, i);
    float t[3], q[4];

    local_bind[i] = b;
    anim_local[i] = b;   // default: no clip data for this bone -> keep bind

    if(tq_anm_bone_frame(anm, sk->bone_names[i], frame, t, q))
    {
      Mat rot = quat_mat(-q[0], -q[1], -q[2], q[3]);   // conjugate
      Mat br  = b;

      br.m[12] = br.m[13] = br.m[14] = 0;              // bind rotation only

      Mat l = mat_mul(br, rot);

      l.m[12] = b.m[12] + t[0];
      l.m[13] = b.m[13] + t[1];
      l.m[14] = b.m[14] + t[2];
      anim_local[i] = l;
    }
  }

  // Accumulate world transforms (bones are not guaranteed parent-before-child,
  // so resolve iteratively until no further progress).
  int rem = nb;

  while(rem > 0)
  {
    int progress = 0;

    for(int i = 0; i < nb; i++)
    {
      if(done[i])
        continue;

      int p = sk->bone_parent[i];

      if(p < 0)
      {
        bind_world[i] = local_bind[i];
        anim_world[i] = anim_local[i];
        done[i] = 1;
        rem--;
        progress = 1;
      }
      else if(done[p])
      {
        bind_world[i] = mat_mul(bind_world[p], local_bind[i]);
        anim_world[i] = mat_mul(anim_world[p], anim_local[i]);
        done[i] = 1;
        rem--;
        progress = 1;
      }
    }

    if(!progress)
      break;   // cyclic / dangling parent -> leave the rest at identity-ish
  }

  for(int i = 0; i < nb; i++)
  {
    if(done[i])
      skin[i] = mat_mul(anim_world[i], mat_inv(bind_world[i]));
    else
      skin[i] = mat_id();
  }

  // Linear-blend skin every vertex (and its normal): the vertex's group maps
  // its group-relative bone index through the group palette to a skeleton bone.
  for(uint32_t i = 0; i < m->num_verts; i++)
  {
    int grp = sk->vtx_group[i];

    if(grp < 0 || grp >= sk->num_groups)
      continue;

    const int *pal = sk->group_palette[grp];
    int plen = sk->group_pal_len[grp];

    float p[3] = { m->verts[i].px, m->verts[i].py, m->verts[i].pz };
    float n[3] = { m->verts[i].nx, m->verts[i].ny, m->verts[i].nz };
    float acc[3] = { 0, 0, 0 }, accn[3] = { 0, 0, 0 }, ws = 0;

    for(int k = 0; k < 4; k++)
    {
      float w = sk->vtx_weight[i][k];
      int si = sk->vtx_bone[i][k];

      if(w <= 0 || si >= plen)
        continue;

      int bi = pal[si];

      if(bi < 0 || bi >= nb)
        continue;

      float o[3], on[3];

      mat_xf(&skin[bi], p, o);
      mat_xf3(&skin[bi], n, on);
      for(int c = 0; c < 3; c++)
      {
        acc[c]  += w * o[c];
        accn[c] += w * on[c];
      }
      ws += w;
    }

    if(ws > 1e-6f)
    {
      m->verts[i].px = acc[0];
      m->verts[i].py = acc[1];
      m->verts[i].pz = acc[2];
      m->verts[i].nx = accn[0];
      m->verts[i].ny = accn[1];
      m->verts[i].nz = accn[2];
    }
  }

  free(local_bind);
  free(anim_local);
  free(bind_world);
  free(anim_world);
  free(skin);
  free(done);

  // Recompute the bounds from the posed positions so framing/auto-yaw fit.
  float mn[3] = {  INFINITY,  INFINITY,  INFINITY };
  float mx[3] = { -INFINITY, -INFINITY, -INFINITY };

  for(uint32_t i = 0; i < m->num_verts; i++)
  {
    float P[3] = { m->verts[i].px, m->verts[i].py, m->verts[i].pz };

    for(int c = 0; c < 3; c++)
    {
      if(P[c] < mn[c]) mn[c] = P[c];
      if(P[c] > mx[c]) mx[c] = P[c];
    }
  }

  for(int c = 0; c < 3; c++)
  {
    m->bbmin[c] = mn[c];
    m->bbmax[c] = mx[c];
  }

  return(true);
}
