// tq-quest-tool: helpers shared by the command modules.

#include "tq_quest_tool.h"

// ci_contains -- case-insensitive substring search.
// haystack: string to search in.
// needle: substring to find.
// returns true if needle is found within haystack (case-insensitive).
bool
ci_contains(const char *haystack, const char *needle)
{
  if(!haystack || !needle)
    return(false);

  size_t nlen = strlen(needle);
  size_t hlen = strlen(haystack);

  if(nlen > hlen)
    return(false);

  for(size_t i = 0; i <= hlen - nlen; i++)
  {
    bool match = true;

    for(size_t j = 0; j < nlen; j++)
    {
      if(tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j]))
      {
        match = false;
        break;
      }
    }

    if(match)
      return(true);
  }

  return(false);
}

// cmp_str -- qsort comparison for string pointer arrays.
// a: pointer to first string pointer.
// b: pointer to second string pointer.
// returns strcmp result.
int
cmp_str(const void *a, const void *b)
{
  return(strcmp(*(const char **)a, *(const char **)b));
}

// guess_act -- heuristic to determine which act a token belongs to based on prefix.
// token: quest token string.
// returns the QuestAct enum value.
QuestAct
guess_act(const char *token)
{
  if(strncasecmp(token, "x4", 2) == 0)
    return(ACT_ETERNAL_EMBERS);

  if(strncasecmp(token, "MQ", 2) == 0 ||
     strncasecmp(token, "X3_", 3) == 0 ||
     strncasecmp(token, "x2SF", 4) == 0 ||
     strncasecmp(token, "x3", 2) == 0 ||
     strcasecmp(token, "MapUnlockAtlantis") == 0)
    return(ACT_ATLANTIS);

  if(strncasecmp(token, "xQ", 2) == 0 ||
     strncasecmp(token, "xSQ", 3) == 0 ||
     strncasecmp(token, "xBossChest_", 11) == 0 ||
     strcasecmp(token, "Olympus - Typhon Defeated") == 0)
    return(ACT_IMMORTAL_THRONE);

  if(strncasecmp(token, "Q0", 2) == 0 && isdigit((unsigned char)token[2]) && token[2] >= '1' && token[2] <= '7' &&
     !strchr(token, '_'))
    return(ACT_RAGNAROK);

  if(strncasecmp(token, "SQ", 2) == 0 && isdigit((unsigned char)token[2]))
    return(ACT_RAGNAROK);

  if(strncasecmp(token, "EPILOGUE", 8) == 0)
    return(ACT_RAGNAROK);

  if(strncasecmp(token, "Q08_", 4) == 0 ||
     strncasecmp(token, "Q09_", 4) == 0 ||
     strncasecmp(token, "Q10_", 4) == 0 ||
     strncasecmp(token, "Q11_", 4) == 0 ||
     strncasecmp(token, "JE", 2) == 0 ||
     strcasecmp(token, "Egypt - Telkine Defeated") == 0 ||
     strcasecmp(token, "ImhotepKnown") == 0)
    return(ACT_EGYPT);

  if(strncasecmp(token, "Q12_", 4) == 0 ||
     strncasecmp(token, "JO", 2) == 0 ||
     strcasecmp(token, "Orient - Telkine Defeated") == 0 ||
     strcasecmp(token, "MapUnlockOrient") == 0 ||
     strcasecmp(token, "YellowEmperorKnown") == 0)
    return(ACT_ORIENT);

  if(strncasecmp(token, "Q1_", 3) == 0 ||
     strncasecmp(token, "Q2_", 3) == 0 ||
     strncasecmp(token, "JG", 2) == 0 ||
     strncasecmp(token, "SS_", 3) == 0 ||
     strncasecmp(token, "BossChest_", 10) == 0 ||
     strcasecmp(token, "Greece - Telkine Defeated") == 0)
    return(ACT_GREECE);

  // Misc tokens that don't clearly belong to any act
  return(ACT_GREECE); // default bucket
}

// find_quest_by_name -- find a quest definition by name.
// Tries exact match first, then case-insensitive substring match.
// name: quest name to search for.
// returns pointer to matching QuestDef, or NULL if not found.
const QuestDef *
find_quest_by_name(const char *name)
{
  int count;
  const QuestDef *defs = quest_get_defs(&count);

  // Try exact match first
  for(int i = 0; i < count; i++)
  {
    if(strcasecmp(defs[i].name, name) == 0)
      return(&defs[i]);
  }

  // Try substring
  for(int i = 0; i < count; i++)
  {
    if(ci_contains(defs[i].name, name))
      return(&defs[i]);
  }

  return(NULL);
}

// Read exactly `size` bytes from an open stream into a fresh buffer.  A short
// read is a failure, not a partial success: every caller byte-compares the
// result, and a partial fill would leave the tail uninitialized and the
// verdict meaningless.  Returns NULL on a bad size, OOM or a short read; the
// caller owns the buffer.
uint8_t *
slurp_stream(FILE *f, long size)
{
  uint8_t *buf;

  if(size < 0)
    return(NULL);

  buf = malloc((size_t)size);

  if(!buf)
    return(NULL);

  if(fread(buf, 1, (size_t)size, f) != (size_t)size)
  {
    free(buf);
    return(NULL);
  }

  return(buf);
}
