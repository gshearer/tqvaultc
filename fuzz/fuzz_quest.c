#include "fuzz_tmpfile.h"
#include "quest_tokens.h"

// QuestToken.myw -- where a corrupt count drove the multi-GB allocation the
// 2026-07-21 audit clamped.
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t len)
{
  const char *path = fuzz_tmpfile_write(data, len);

  if(!path)
    return(0);

  QuestTokenSet set;

  // quest_tokens_load calls quest_token_set_init() before anything can fail,
  // so the set is safe to free on the error paths too.  A rejected file is
  // the expected outcome for nearly every input -- the fuzzer is looking for
  // crashes, not for verdicts -- so the return value is deliberately unused.
  (void)quest_tokens_load(path, &set);
  quest_token_set_free(&set);

  return(0);
}
