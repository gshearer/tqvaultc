#ifndef ANM_H
#define ANM_H

// anm -- minimal parser for Titan Quest's proprietary .anm skeletal-animation
// format, used to pose a creature mesh in a natural stance for its thumbnail
// (see mesh_skin.c) instead of the static bind-pose T-pose.  GTK-free; links
// into tq-dbr-tool alongside mesh.c.
//
// File layout (little-endian): "ANM" + 1 version byte (0x02 observed), then
//   [u32 numBones][u32 numFrames][u32 fps]
// followed, per bone, by [u32 strlen][name] and numFrames * 14 floats:
//   translation(3) | quaternion(4, xyzw) | scale(3) | (0,0,0,1)
// The per-frame transform is a delta from the bind pose; we keep only the
// translation and quaternion (scale is ignored, matching the renderer).
// Bone order differs from the .msh skeleton, so transforms are looked up by
// name.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct TQAnm TQAnm;

// Parse an .anm blob (caller still owns `data`).  Returns NULL on failure.
TQAnm *tq_anm_parse(const uint8_t *data, size_t len);

void tq_anm_free(TQAnm *a);

// Number of frames in the clip (0 if `a` is NULL).
int tq_anm_num_frames(const TQAnm *a);

// Fetch bone `name`'s transform at `frame`: translation (3) and quaternion
// (4, xyzw).  Returns false (and leaves the outputs untouched) if the bone is
// absent or the frame is out of range.
bool tq_anm_bone_frame(const TQAnm *a, const char *name, int frame,
                       float trans[3], float quat[4]);

#endif
