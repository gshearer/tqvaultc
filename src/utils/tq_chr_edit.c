// tq-chr-tool: the commands that go through character_load/character_save
// -- roundtrip verification and the skill-record splice (writes in place).

#include "tq_chr_tool.h"

// ── cmd_roundtrip ────────────────────────────────────────────────────────

// cmd_roundtrip -- load via character_load(), save to /tmp, then compare.
// path: path to .chr file.
// returns 0 if round-trip is identical, 1 if differences found or on error.
int
cmd_roundtrip(const char *path)
{
  printf("=== Roundtrip: %s ===\n\n", path);

  // Load original file size for later comparison
  size_t orig_size;
  uint8_t *orig_data = load_file(path, &orig_size);

  if(!orig_data)
    return(1);

  // Use character_load() / character_save()
  tqvc_debug = true;
  TQCharacter *chr = character_load(path);

  if(!chr)
  {
    fprintf(stderr, "error: character_load() failed\n");
    free(orig_data);
    return(1);
  }

  printf("\ncharacter_load() succeeded: %s level %u, %d sacks\n",
         chr->character_name, chr->level, chr->num_inv_sacks);
  printf("  inv_block: [%zu..%zu)  equip_block: [%zu..%zu)\n\n",
         chr->inv_block_start, chr->inv_block_end,
         chr->equip_block_start, chr->equip_block_end);

  // Save to temp file
  char *tmp_path = g_build_filename(g_get_tmp_dir(), "tq_chr_tool_roundtrip.chr", NULL);
  int ret = character_save(chr, tmp_path);

  if(ret != 0)
  {
    fprintf(stderr, "error: character_save() returned %d\n", ret);
    character_free(chr);
    free(orig_data);
    g_free(tmp_path);
    return(1);
  }

  printf("character_save() wrote %s\n\n", tmp_path);
  character_free(chr);
  free(orig_data);

  // Now compare original vs roundtripped using our independent parser
  printf("------------------------------------------------------------\n\n");
  int cmp_ret = cmd_compare(path, tmp_path);

  g_free(tmp_path);
  return(cmp_ret);
}

// cmd_add_skill -- splice a brand-new skill record into the save via
// character_save_skills_ex(), then reload and verify it parses back.
// WARNING: writes to the .chr in place (a .bak is made on first save). Use a
// copy. This exercises the same code path the skill manager uses to enable a
// never-learned skill.
// path: the .chr to modify. skill_path: the skill DBR path. level: skill level.
// returns 0 on success, 1 on error.
int
cmd_add_skill(const char *path, const char *skill_path, uint32_t level)
{
  TQCharacter *chr = character_load(path);

  if(!chr)
  {
    fprintf(stderr, "error: character_load() failed\n");
    return(1);
  }

  printf("Before: num_skills=%d  off_skill_max=%zu  skill_list_end_off=%zu\n",
         chr->num_skills, chr->off_skill_max, chr->skill_list_end_off);

  const char *paths[1]  = { skill_path };
  uint32_t    levels[1] = { level };
  int         ret       = character_save_skills_ex(chr, paths, levels, 1);

  character_free(chr);

  if(ret != 0)
  {
    fprintf(stderr, "error: character_save_skills_ex() returned %d\n", ret);
    return(1);
  }

  printf("Added '%s' level %u, wrote %s\n\n", skill_path, level, path);

  // Reload and confirm the new record round-trips.
  TQCharacter *chr2 = character_load(path);

  if(!chr2)
  {
    fprintf(stderr, "error: reload failed (corrupt write?)\n");
    return(1);
  }

  printf("After reload: num_skills=%d\n", chr2->num_skills);

  int found = 0;

  for(int i = 0; i < chr2->num_skills; i++)
  {
    if(chr2->skills[i].skill_name &&
       strcasecmp(chr2->skills[i].skill_name, skill_path) == 0)
    {
      found = 1;
      printf("  verified: skills[%d] = \"%s\" level %u\n",
             i, chr2->skills[i].skill_name, chr2->skills[i].skill_level);
    }
  }

  character_free(chr2);

  if(!found)
  {
    fprintf(stderr, "FAIL: new skill not present after reload\n");
    return(1);
  }

  printf("OK\n");
  return(0);
}
