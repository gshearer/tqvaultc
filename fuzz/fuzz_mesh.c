#include "mesh.h"

// .msh creature/item meshes.  Already a buffer parser, so no staging file.
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t len)
{
  TQMesh *m = tq_mesh_parse(data, len);

  if(m)
    tq_mesh_free(m);

  return(0);
}
