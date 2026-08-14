#include "dds_decode.h"
#include <stdlib.h>

// DDS textures unpacked out of the game's .arc archives.  Already a buffer
// parser, so no staging file.
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t len)
{
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t *pixels = dds_decode(data, len, &width, &height);

  free(pixels);
  return(0);
}
