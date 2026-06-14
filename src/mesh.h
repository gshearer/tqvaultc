#ifndef MESH_H
#define MESH_H

// mesh -- minimal parser for Titan Quest's proprietary .msh model format.
//
// We decode what a thumbnail needs: the single skinned vertex buffer (position
// + texcoord + normal, plus bone indices/weights for posing) and the triangle
// index buffer with its per-group bone palettes, and -- optionally -- the
// skeleton, so the model can be posed into a natural stance (see mesh_skin.c).
// Without a skeleton the stored bind pose is rendered as-is (v1 behaviour).
//
// File layout (little-endian): "MSH" + version byte, then a flat chunk list of
// [type:u32][size:u32][size bytes of data].  Relevant chunk types:
//   0  material reference: [strlen:u32][path]
//   3  text section (attach points / ragdoll) -- skipped
//   4  vertex buffer: [numElem:u32][stride:u32][count:u32][0]<decl><vertices>;
//      the vertices are the trailing count*stride bytes of the chunk.
//   5  index buffer: [numTris:u32][numGroups:u32]<numTris*3 u16 indices>
//      followed by per-group records that end with a bone palette.
//   6  skeleton: [u32 count] + count*88-byte bone records (name, firstChild,
//      childCount, local bind transform).
//   7  material footer: length-prefixed keys/values incl. `baseTexture` (the
//      model's diffuse .tex) and `bumpTexture`.
// The standard skinned vertex stride is 76 bytes: position(12) at offset 0,
// texcoord(8) at 12, normal(12) at 20, bone indices(4) at 32, weights(16) at
// 36, then tangents.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
  float px, py, pz;   // position
  float u, v;         // texture coordinate
  float nx, ny, nz;   // normal (zeroed if the stride is too small to hold one)
} TQMeshVertex;

// Optional skeletal-skinning data for the natural-pose (v2) path.  Present
// (TQMesh.skin != NULL) only when the mesh carries a chunk-6 skeleton AND
// per-vertex skin weights; absent for unskinned meshes (which render in their
// stored bind pose, unchanged from v1).  See mesh_skin.c for the math.
//
//   skeleton bone for vertex i's k-th influence =
//     group_palette[ vtx_group[i] ][ vtx_bone[i][k] ]
//
typedef struct {
  int       num_bones;
  char    (*bone_names)[33];   // [num_bones] NUL-terminated (32-char fixed field)
  int      *bone_parent;       // [num_bones], -1 for roots
  float    *bone_local;        // [num_bones*12] local bind: xAxis,yAxis,zAxis,origin

  int       num_groups;
  int     **group_palette;     // [num_groups][group_pal_len[g]] -> skeleton bone idx
  int      *group_pal_len;     // [num_groups]

  uint8_t (*vtx_bone)[4];      // [num_verts] group-relative palette indices (255=unused)
  float   (*vtx_weight)[4];    // [num_verts] influence weights (sum -> 1)
  int      *vtx_group;         // [num_verts] owning material group
} TQMeshSkin;

typedef struct {
  uint32_t      num_verts;
  TQMeshVertex *verts;
  uint32_t      num_tris;
  uint32_t     *indices;     // 3 * num_tris vertex indices
  float         bbmin[3];    // axis-aligned bounds (computed from positions)
  float         bbmax[3];
  char         *mif_path;    // material reference (chunk 0), or NULL
  char         *material_texture; // diffuse .tex from the chunk-7 material footer,
                                  // or NULL — the model's own texture, used when a
                                  // creature DBR provides no baseTexture override
  TQMeshSkin   *skin;        // skeletal data for posing, or NULL (bind pose only)
} TQMesh;

// Parse a .msh blob (caller still owns `data`).  Returns NULL on failure.
TQMesh *tq_mesh_parse(const uint8_t *data, size_t len);

void tq_mesh_free(TQMesh *m);

// Suggest a camera yaw (degrees) that frames the creature well: the camera
// looks along the model's narrower horizontal axis so the wider one becomes
// screen width.  For bipeds (shoulders span X) this yields a front view; for
// long quadrupeds (body spans Z) a side profile.  A small turn is added for a
// three-quarter look.  Front/back is not disambiguated (TQ models generally
// face +Z); override per creature if one comes out backwards.
float tq_mesh_suggest_yaw(const TQMesh *m);

#endif
