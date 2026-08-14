#include "anm.h"

// .anm skeletal animations, read alongside the meshes by the thumbnail
// renderer.  Already a buffer parser, so no staging file.
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t len)
{
  TQAnm *a = tq_anm_parse(data, len);

  if(a)
    tq_anm_free(a);

  return(0);
}
