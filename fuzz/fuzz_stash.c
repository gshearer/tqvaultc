#include "fuzz_tmpfile.h"
#include "stash.h"

// .dxb / .dxg stash files.  stash_load does not branch on stashVersion, so
// this one harness covers the desktop version 5 and the iOS version 6 layout
// (the trailing unlockedInventory footer) alike.
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t len)
{
  const char *path = fuzz_tmpfile_write(data, len);

  if(!path)
    return(0);

  TQStash *stash = stash_load(path);

  if(stash)
    stash_free(stash);

  return(0);
}
