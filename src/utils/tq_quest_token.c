// tq-quest-tool: the QuestToken.myw commands -- listing, searching, the
// quest_defs[] cross-checks, and the add/remove/complete/clear edits.

#include "tq_quest_tool.h"

// ── Commands ─────────────────────────────────────────────────────────────

// cmd_dump -- list all tokens in a QuestToken.myw file, sorted.
// path: path to QuestToken.myw file.
// returns 0 on success, 1 on error.
int
cmd_dump(const char *path)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  // Sort for consistent output
  qsort(set.tokens, set.count, sizeof(char *), cmp_str);

  for(int i = 0; i < set.count; i++)
    printf("%s\n", set.tokens[i]);

  quest_token_set_free(&set);
  return(0);
}

// cmd_count -- count tokens in a QuestToken.myw file.
// path: path to QuestToken.myw file.
// returns 0 on success, 1 on error.
int
cmd_count(const char *path)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  printf("%d tokens\n", set.count);
  quest_token_set_free(&set);
  return(0);
}

// cmd_search -- list tokens matching a substring (case-insensitive).
// path: path to QuestToken.myw file.
// pattern: substring to search for.
// returns 0 on success, 1 on error.
int
cmd_search(const char *path, const char *pattern)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  qsort(set.tokens, set.count, sizeof(char *), cmp_str);
  int hits = 0;

  for(int i = 0; i < set.count; i++)
  {
    if(ci_contains(set.tokens[i], pattern))
    {
      printf("%s\n", set.tokens[i]);
      hits++;
    }
  }

  printf("--- %d matches\n", hits);
  quest_token_set_free(&set);
  return(0);
}

// cmd_has -- check if a specific token exists (exact match).
// path: path to QuestToken.myw file.
// token: token string to search for.
// returns 0 if found, 1 if not found or on error.
int
cmd_has(const char *path, const char *token)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  bool found = quest_token_set_contains(&set, token);

  printf("%s: %s\n", token, found ? "YES" : "NO");
  quest_token_set_free(&set);
  return(found ? 0 : 1);
}

// cmd_acts -- group tokens by act using prefix heuristics.
// path: path to QuestToken.myw file.
// returns 0 on success, 1 on error.
int
cmd_acts(const char *path)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  qsort(set.tokens, set.count, sizeof(char *), cmp_str);

  for(int a = 0; a < NUM_ACTS; a++)
  {
    printf("\n=== %s ===\n", quest_act_name((QuestAct)a));
    int act_count = 0;

    for(int i = 0; i < set.count; i++)
    {
      if(guess_act(set.tokens[i]) == (QuestAct)a)
      {
        printf("  %s\n", set.tokens[i]);
        act_count++;
      }
    }

    if(act_count == 0)
      printf("  (none)\n");
    else
      printf("  --- %d tokens\n", act_count);
  }

  quest_token_set_free(&set);
  return(0);
}

// cmd_quests -- show quest completion status against quest_defs[].
// path: path to QuestToken.myw file.
// returns 0 on success, 1 on error.
int
cmd_quests(const char *path)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  int count;
  const QuestDef *defs = quest_get_defs(&count);
  QuestAct last_act = -1;
  bool last_main = true;
  int complete = 0, total = 0;

  for(int i = 0; i < count; i++)
  {
    if(defs[i].act != last_act)
    {
      printf("\n=== %s ===\n", quest_act_name(defs[i].act));
      last_act = defs[i].act;
      last_main = true;
    }

    if(!defs[i].is_main && last_main)
    {
      printf("  --- Side Quests ---\n");
      last_main = false;
    }

    bool done = quest_token_set_contains(&set, defs[i].completion_token);

    // Count how many of this quest's tokens are present
    int present = 0, quest_total = 0;

    for(const char *const *t = defs[i].tokens; *t; t++)
    {
      quest_total++;
      if(quest_token_set_contains(&set, *t))
        present++;
    }

    printf("  [%s] %-35s (%d/%d tokens)\n",
           done ? "x" : " ", defs[i].name, present, quest_total);
    total++;
    if(done)
      complete++;
  }

  printf("\n--- %d/%d quests completed\n", complete, total);
  quest_token_set_free(&set);
  return(0);
}

// cmd_add -- add a token to a QuestToken.myw file (saves in-place, .bak created).
// path: path to QuestToken.myw file.
// token: token string to add.
// returns 0 on success, 1 on error.
int
cmd_add(const char *path, const char *token)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  if(quest_token_set_contains(&set, token))
  {
    printf("Token already present: %s\n", token);
    quest_token_set_free(&set);
    return(0);
  }

  quest_token_set_add(&set, token);

  if(quest_tokens_save(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to save %s\n", path);
    quest_token_set_free(&set);
    return(1);
  }

  printf("Added token: %s (now %d tokens)\n", token, set.count);
  quest_token_set_free(&set);
  return(0);
}

// cmd_remove -- remove a token from a QuestToken.myw file (saves in-place, .bak created).
// path: path to QuestToken.myw file.
// token: token string to remove.
// returns 0 on success, 1 on error.
int
cmd_remove(const char *path, const char *token)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  if(!quest_token_set_contains(&set, token))
  {
    printf("Token not found: %s\n", token);
    quest_token_set_free(&set);
    return(0);
  }

  quest_token_set_remove(&set, token);

  if(quest_tokens_save(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to save %s\n", path);
    quest_token_set_free(&set);
    return(1);
  }

  printf("Removed token: %s (now %d tokens)\n", token, set.count);
  quest_token_set_free(&set);
  return(0);
}

// cmd_complete -- add all tokens for a named quest.
// path: path to QuestToken.myw file.
// quest_name: quest name to complete (substring match).
// returns 0 on success, 1 on error.
int
cmd_complete(const char *path, const char *quest_name)
{
  const QuestDef *qd = find_quest_by_name(quest_name);

  if(!qd)
  {
    fprintf(stderr, "Error: no quest matching '%s'\n", quest_name);
    fprintf(stderr, "Use 'defs' command to list available quest names.\n");
    return(1);
  }

  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  int added = 0;

  for(const char *const *t = qd->tokens; *t; t++)
  {
    if(!quest_token_set_contains(&set, *t))
    {
      quest_token_set_add(&set, *t);
      added++;
    }
  }

  if(added == 0)
  {
    printf("Quest '%s' already complete (all tokens present)\n", qd->name);
    quest_token_set_free(&set);
    return(0);
  }

  if(quest_tokens_save(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to save %s\n", path);
    quest_token_set_free(&set);
    return(1);
  }

  printf("Completed quest '%s': added %d tokens (now %d total)\n",
         qd->name, added, set.count);
  quest_token_set_free(&set);
  return(0);
}

// cmd_clear -- remove all tokens for a named quest.
// path: path to QuestToken.myw file.
// quest_name: quest name to clear (substring match).
// returns 0 on success, 1 on error.
int
cmd_clear(const char *path, const char *quest_name)
{
  const QuestDef *qd = find_quest_by_name(quest_name);

  if(!qd)
  {
    fprintf(stderr, "Error: no quest matching '%s'\n", quest_name);
    fprintf(stderr, "Use 'defs' command to list available quest names.\n");
    return(1);
  }

  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  int removed = 0;

  for(const char *const *t = qd->tokens; *t; t++)
  {
    if(quest_token_set_contains(&set, *t))
    {
      quest_token_set_remove(&set, *t);
      removed++;
    }
  }

  if(removed == 0)
  {
    printf("Quest '%s' already clear (no tokens present)\n", qd->name);
    quest_token_set_free(&set);
    return(0);
  }

  if(quest_tokens_save(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to save %s\n", path);
    quest_token_set_free(&set);
    return(1);
  }

  printf("Cleared quest '%s': removed %d tokens (now %d total)\n",
         qd->name, removed, set.count);
  quest_token_set_free(&set);
  return(0);
}


// cmd_roundtrip -- load, save to /tmp, and byte-compare with original.
// path: path to QuestToken.myw file.
// returns 0 on success, 1 on mismatch or error.
int
cmd_roundtrip(const char *path)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  printf("Loaded %d tokens from %s\n", set.count, path);

  char *tmp = g_build_filename(g_get_tmp_dir(), "tq_quest_roundtrip.myw", NULL);

  if(quest_tokens_save(tmp, &set) != 0)
  {
    fprintf(stderr, "Error: failed to save to %s\n", tmp);
    quest_token_set_free(&set);
    g_free(tmp);
    return(1);
  }

  // Byte-compare original and round-tripped file
  FILE *fa = fopen(path, "rb");
  FILE *fb = fopen(tmp, "rb");

  if(!fa || !fb)
  {
    fprintf(stderr, "Error: cannot open files for comparison\n");
    if(fa) fclose(fa);
    if(fb) fclose(fb);
    quest_token_set_free(&set);
    g_free(tmp);
    return(1);
  }

  fseek(fa, 0, SEEK_END);
  fseek(fb, 0, SEEK_END);
  long sa = ftell(fa);
  long sb = ftell(fb);
  rewind(fa);
  rewind(fb);

  if(sa != sb)
  {
    printf("DIFFER: size mismatch (%ld vs %ld bytes)\n", sa, sb);
    fclose(fa); fclose(fb);
    quest_token_set_free(&set);
    g_free(tmp);
    return(1);
  }

  uint8_t *ba = slurp_stream(fa, sa);
  uint8_t *bb = slurp_stream(fb, sb);

  fclose(fa);
  fclose(fb);

  if(!ba || !bb)
  {
    fprintf(stderr, "Error: cannot read files for comparison\n");
    free(ba);
    free(bb);
    quest_token_set_free(&set);
    g_free(tmp);
    return(1);
  }

  bool identical = memcmp(ba, bb, sa) == 0;

  free(ba);
  free(bb);

  if(identical)
  {
    printf("PASS: round-trip produces identical %ld-byte file\n", sa);
  }
  else
  {
    printf("FAIL: files differ!\n");
    quest_token_set_free(&set);
    g_free(tmp);
    return(1);
  }

  // Also verify re-load
  QuestTokenSet set2;

  if(quest_tokens_load(tmp, &set2) != 0)
  {
    fprintf(stderr, "Error: failed to reload %s\n", tmp);
    quest_token_set_free(&set);
    g_free(tmp);
    return(1);
  }

  g_free(tmp);

  if(set.count != set2.count)
  {
    printf("FAIL: token count mismatch (%d vs %d)\n", set.count, set2.count);
    quest_token_set_free(&set);
    quest_token_set_free(&set2);
    return(1);
  }

  for(int i = 0; i < set.count; i++)
  {
    if(strcmp(set.tokens[i], set2.tokens[i]) != 0)
    {
      printf("FAIL: token[%d] mismatch: '%s' vs '%s'\n",
             i, set.tokens[i], set2.tokens[i]);
      quest_token_set_free(&set);
      quest_token_set_free(&set2);
      return(1);
    }
  }

  printf("PASS: re-loaded %d tokens match original order\n", set2.count);

  quest_token_set_free(&set);
  quest_token_set_free(&set2);
  return(0);
}

// cmd_defs -- list all quest definitions with token counts.
// returns 0.
int
cmd_defs(void)
{
  int count;
  const QuestDef *defs = quest_get_defs(&count);
  QuestAct last_act = -1;
  bool last_main = true;

  for(int i = 0; i < count; i++)
  {
    if(defs[i].act != last_act)
    {
      printf("\n=== %s ===\n", quest_act_name(defs[i].act));
      last_act = defs[i].act;
      last_main = true;
    }

    if(!defs[i].is_main && last_main)
    {
      printf("  --- Side Quests ---\n");
      last_main = false;
    }

    int ntokens = 0;

    for(const char *const *t = defs[i].tokens; *t; t++)
      ntokens++;

    printf("  %-35s [%s] %d tokens  (check: %s)\n",
           defs[i].name,
           defs[i].is_main ? "MAIN" : "SIDE",
           ntokens,
           defs[i].completion_token);
  }

  printf("\n--- %d quest definitions total\n", count);
  return(0);
}

// cmd_diff -- show tokens present in one file but not the other.
// path_a: path to first QuestToken.myw file.
// path_b: path to second QuestToken.myw file.
// returns 0 on success, 1 on error.
int
cmd_diff(const char *path_a, const char *path_b)
{
  QuestTokenSet sa, sb;

  if(quest_tokens_load(path_a, &sa) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path_a);
    return(1);
  }

  if(quest_tokens_load(path_b, &sb) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path_b);
    quest_token_set_free(&sa);
    return(1);
  }

  // Tokens only in A
  printf("--- Only in %s (%d tokens) ---\n", path_a, sa.count);
  qsort(sa.tokens, sa.count, sizeof(char *), cmp_str);
  int only_a = 0;

  for(int i = 0; i < sa.count; i++)
  {
    if(!quest_token_set_contains(&sb, sa.tokens[i]))
    {
      printf("  - %s\n", sa.tokens[i]);
      only_a++;
    }
  }

  if(only_a == 0)
    printf("  (none)\n");

  // Tokens only in B
  printf("\n--- Only in %s (%d tokens) ---\n", path_b, sb.count);
  qsort(sb.tokens, sb.count, sizeof(char *), cmp_str);
  int only_b = 0;

  for(int i = 0; i < sb.count; i++)
  {
    if(!quest_token_set_contains(&sa, sb.tokens[i]))
    {
      printf("  + %s\n", sb.tokens[i]);
      only_b++;
    }
  }

  if(only_b == 0)
    printf("  (none)\n");

  // Count shared
  int shared = 0;

  for(int i = 0; i < sa.count; i++)
  {
    if(quest_token_set_contains(&sb, sa.tokens[i]))
      shared++;
  }

  printf("\n--- Summary: %d shared, %d only-A, %d only-B\n", shared, only_a, only_b);

  quest_token_set_free(&sa);
  quest_token_set_free(&sb);
  return(0);
}

// cmd_coverage -- report covered/orphaned/uncovered tokens vs quest_defs[].
// path: path to QuestToken.myw file.
// returns 0 on success, 1 on error.
int
cmd_coverage(const char *path)
{
  QuestTokenSet set;

  if(quest_tokens_load(path, &set) != 0)
  {
    fprintf(stderr, "Error: failed to load %s\n", path);
    return(1);
  }

  int count;
  const QuestDef *defs = quest_get_defs(&count);

  // Build a set of all tokens covered by quest definitions.
  // Use a simple array + linear scan since counts are small.
  int total_def_tokens = 0;

  for(int i = 0; i < count; i++)
    for(const char *const *t = defs[i].tokens; *t; t++)
      total_def_tokens++;

  const char **def_tokens = malloc(total_def_tokens * sizeof(char *));

  if(!def_tokens)
  {
    fprintf(stderr, "Error: malloc failed\n");
    quest_token_set_free(&set);
    return(1);
  }

  int idx = 0;

  for(int i = 0; i < count; i++)
    for(const char *const *t = defs[i].tokens; *t; t++)
      def_tokens[idx++] = *t;

  // 1. Orphaned tokens: in file but not in any quest definition
  printf("=== Orphaned Tokens (in file, not in any quest def) ===\n");
  qsort(set.tokens, set.count, sizeof(char *), cmp_str);
  int orphaned = 0;

  for(int i = 0; i < set.count; i++)
  {
    bool found = false;

    for(int j = 0; j < total_def_tokens; j++)
    {
      if(strcmp(set.tokens[i], def_tokens[j]) == 0)
      {
        found = true;
        break;
      }
    }

    if(!found)
    {
      printf("  %s\n", set.tokens[i]);
      orphaned++;
    }
  }

  if(orphaned == 0)
    printf("  (none)\n");

  printf("--- %d orphaned tokens\n\n", orphaned);

  // 2. Covered tokens: in file and in a quest definition
  int covered = 0;

  for(int i = 0; i < set.count; i++)
  {
    for(int j = 0; j < total_def_tokens; j++)
    {
      if(strcmp(set.tokens[i], def_tokens[j]) == 0)
      {
        covered++;
        break;
      }
    }
  }

  printf("=== Coverage Summary ===\n");
  printf("  File tokens:    %d\n", set.count);
  printf("  Covered:        %d\n", covered);
  printf("  Orphaned:       %d\n", orphaned);

  // 3. Uncovered quests: quest defs with zero matching tokens in the file
  printf("\n=== Uncovered Quests (zero tokens in file) ===\n");
  int uncovered = 0;

  for(int i = 0; i < count; i++)
  {
    int present = 0;

    for(const char *const *t = defs[i].tokens; *t; t++)
    {
      if(quest_token_set_contains(&set, *t))
        present++;
    }

    if(present == 0)
    {
      printf("  [%s] %s\n", quest_act_name(defs[i].act), defs[i].name);
      uncovered++;
    }
  }

  if(uncovered == 0)
    printf("  (none)\n");

  printf("--- %d uncovered quests\n", uncovered);

  free(def_tokens);
  quest_token_set_free(&set);
  return(0);
}
