#include "anm.h"
#include <stdlib.h>
#include <string.h>

// Per-frame data we keep: 3 translation + 4 quaternion (xyzw) floats.
#define ANM_FRAME_FLOATS 7
// Floats actually present per frame in the file (trans3 + quat4 + scale3 + 4).
#define ANM_FILE_FLOATS  14

struct TQAnm {
  int     num_bones;
  int     num_frames;
  char  **names;    // [num_bones]
  float  *frames;   // [num_bones * num_frames * ANM_FRAME_FLOATS]
};

static uint32_t
rd_u32(const uint8_t *p)
{
  return((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static float
rd_f32(const uint8_t *p)
{
  uint32_t u = rd_u32(p);
  float f;

  memcpy(&f, &u, sizeof(f));
  return(f);
}

void
tq_anm_free(TQAnm *a)
{
  if(!a)
    return;

  if(a->names)
  {
    for(int i = 0; i < a->num_bones; i++)
      free(a->names[i]);
    free(a->names);
  }
  free(a->frames);
  free(a);
}

TQAnm *
tq_anm_parse(const uint8_t *data, size_t len)
{
  if(!data || len < 16 || memcmp(data, "ANM", 3) != 0)
    return(NULL);

  uint32_t nb = rd_u32(data + 4);
  uint32_t nf = rd_u32(data + 8);

  // Sanity bounds (well-formed clips are far below these).
  if(nb == 0 || nb > 4096 || nf == 0 || nf > 1000000)
    return(NULL);

  TQAnm *a = calloc(1, sizeof(*a));

  if(!a)
    return(NULL);

  a->num_bones  = (int)nb;
  a->num_frames = (int)nf;
  a->names  = calloc(nb, sizeof(*a->names));
  a->frames = malloc((size_t)nb * nf * ANM_FRAME_FLOATS * sizeof(float));

  if(!a->names || !a->frames)
  {
    tq_anm_free(a);
    return(NULL);
  }

  size_t off = 16;   // "ANM" + ver byte + numBones + numFrames + fps

  for(uint32_t b = 0; b < nb; b++)
  {
    if(off + 4 > len)
      goto fail;

    uint32_t slen = rd_u32(data + off);
    off += 4;

    if(slen > 1024 || off + slen > len)
      goto fail;

    a->names[b] = malloc(slen + 1);
    if(!a->names[b])
      goto fail;
    memcpy(a->names[b], data + off, slen);
    a->names[b][slen] = '\0';
    off += slen;

    uint64_t fr_bytes = (uint64_t)nf * ANM_FILE_FLOATS * 4;

    if(off + fr_bytes > len)
      goto fail;

    for(uint32_t f = 0; f < nf; f++)
    {
      const uint8_t *fr = data + off + (size_t)f * ANM_FILE_FLOATS * 4;
      float *dst = &a->frames[((size_t)b * nf + f) * ANM_FRAME_FLOATS];

      // translation(3) + quaternion(4, xyzw); scale + trailing row ignored.
      for(int k = 0; k < ANM_FRAME_FLOATS; k++)
        dst[k] = rd_f32(fr + k * 4);
    }

    off += (size_t)fr_bytes;
  }

  return(a);

fail:
  tq_anm_free(a);
  return(NULL);
}

int
tq_anm_num_frames(const TQAnm *a)
{
  return(a ? a->num_frames : 0);
}

bool
tq_anm_bone_frame(const TQAnm *a, const char *name, int frame,
                  float trans[3], float quat[4])
{
  if(!a || !name || frame < 0 || frame >= a->num_frames)
    return(false);

  for(int b = 0; b < a->num_bones; b++)
  {
    if(strcmp(a->names[b], name) != 0)
      continue;

    const float *src =
      &a->frames[((size_t)b * a->num_frames + frame) * ANM_FRAME_FLOATS];

    trans[0] = src[0];
    trans[1] = src[1];
    trans[2] = src[2];
    quat[0]  = src[3];
    quat[1]  = src[4];
    quat[2]  = src[5];
    quat[3]  = src[6];
    return(true);
  }

  return(false);
}
