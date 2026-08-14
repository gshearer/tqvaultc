#include "fuzz_tmpfile.h"
#include "character.h"

// Player.chr -- the save file most likely to be hand-edited or passed between
// players, and the one the 2026-07-21 audit found the cur_alternate OOB write
// and the num_inv_sacks wild free in.
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t len)
{
  const char *path = fuzz_tmpfile_write(data, len);

  if(!path)
    return(0);

  TQCharacter *chr = character_load(path);

  if(chr)
    character_free(chr);

  return(0);
}
