// tq-quest-tool: the whole-character quest state overview.

#include "tq_quest_tool.h"

// ── scan: full character quest state overview ────────────────────────────

// cmd_scan -- full overview of a character's quest state across all files.
// Scans QuestToken.myw, Quest.myw, and .que files for each difficulty.
// save_dir: path to the character save directory.
// returns 0 on success, 1 on error.
int
cmd_scan(const char *save_dir)
{
  static const char *diffs[] = { "Normal", "Epic", "Legendary" };
  static const char *map_subdir = "Levels_World_World01.map";

  char *test_path = g_build_filename(save_dir, map_subdir, NULL);
  bool has_map_dir = g_file_test(test_path, G_FILE_TEST_EXISTS);

  g_free(test_path);

  if(!has_map_dir)
  {
    fprintf(stderr, "Error: %s does not contain %s/\n", save_dir, map_subdir);
    fprintf(stderr, "Expected a character save directory (e.g. testdata/saves/_soothie/)\n");
    return(1);
  }

  printf("=== Character Quest State Scan ===\n");
  printf("Directory: %s\n\n", save_dir);

  for(int di = 0; di < 3; di++)
  {
    char *diff_dir = g_build_filename(save_dir, map_subdir, diffs[di], NULL);

    if(!g_file_test(diff_dir, G_FILE_TEST_EXISTS))
    {
      printf("--- %s: (not present)\n\n", diffs[di]);
      g_free(diff_dir);
      continue;
    }

    printf("=== %s ===\n", diffs[di]);

    char *qt_path = g_build_filename(diff_dir, "QuestToken.myw", NULL);

    if(g_file_test(qt_path, G_FILE_TEST_EXISTS))
    {
      QuestTokenSet set;

      if(quest_tokens_load(qt_path, &set) == 0)
      {
        int qcount;
        const QuestDef *qdefs = quest_get_defs(&qcount);
        int complete = 0;

        for(int i = 0; i < qcount; i++)
        {
          if(quest_token_set_contains(&set, qdefs[i].completion_token))
            complete++;
        }

        int act_complete[NUM_ACTS] = {0};
        int act_total[NUM_ACTS] = {0};

        for(int i = 0; i < qcount; i++)
        {
          act_total[qdefs[i].act]++;
          if(quest_token_set_contains(&set, qdefs[i].completion_token))
            act_complete[qdefs[i].act]++;
        }

        printf("  QuestToken.myw: %d tokens, %d/%d quests complete\n",
               set.count, complete, qcount);
        printf("    Per act:");

        for(int a = 0; a < NUM_ACTS; a++)
          printf(" %s=%d/%d", quest_act_name((QuestAct)a),
                 act_complete[a], act_total[a]);

        printf("\n");

        quest_token_set_free(&set);
      }
      else
      {
        printf("  QuestToken.myw: (parse error)\n");
      }
    }
    else
    {
      printf("  QuestToken.myw: (not present)\n");
    }

    g_free(qt_path);

    char *qm_path = g_build_filename(diff_dir, "Quest.myw", NULL);

    if(g_file_test(qm_path, G_FILE_TEST_EXISTS))
    {
      long fsize;
      uint8_t *data = read_file(qm_path, &fsize);

      if(data)
      {
        size_t off = 0;

        qmyw_expect_key(data, fsize, &off, "begin_block");
        off += 4;
        qmyw_expect_key(data, fsize, &off, "numberOfTriggers");

        uint32_t num_trig = que_read_u32(data, &off);
        uint32_t num_rew = 0;
        const char *nr_key = "numRewards";
        size_t nr_len = strlen(nr_key);

        for(size_t i = 0; i + 4 + nr_len + 4 <= (size_t)fsize; i++)
        {
          uint32_t slen;

          memcpy(&slen, data + i, 4);
          if(slen == (uint32_t)nr_len && memcmp(data + i + 4, nr_key, nr_len) == 0)
          {
            memcpy(&num_rew, data + i + 4 + nr_len, 4);
            break;
          }
        }

        printf("  Quest.myw: %u triggers, %u rewards (%ld bytes)\n",
               num_trig, num_rew, fsize);
        free(data);
      }
    }
    else
    {
      printf("  Quest.myw: (not present)\n");
    }

    g_free(qm_path);

    GDir *dd = g_dir_open(diff_dir, 0, NULL);

    if(dd)
    {
      int que_count = 0, que_fired = 0;
      long total_bytes = 0;
      const gchar *ent_name;

      while((ent_name = g_dir_read_name(dd)) != NULL)
      {
        size_t nlen = strlen(ent_name);

        if(nlen < 5 || strcmp(ent_name + nlen - 4, ".que") != 0)
          continue;

        que_count++;

        char *fpath = g_build_filename(diff_dir, ent_name, NULL);

        long fsz;
        uint8_t *fdata = read_file(fpath, &fsz);

        g_free(fpath);

        if(!fdata)
          continue;

        total_bytes += fsz;

        bool any_fired = false;

        for(size_t off = 0; off + 16 <= (size_t)fsz; off++)
        {
          uint32_t slen;

          memcpy(&slen, fdata + off, 4);
          if(slen == 8 && memcmp(fdata + off + 4, "hasFired", 8) == 0)
          {
            uint32_t val;

            memcpy(&val, fdata + off + 12, 4);
            if(val)
            {
              any_fired = true;
              break;
            }
          }
        }

        if(any_fired)
          que_fired++;

        free(fdata);
      }

      g_dir_close(dd);
      printf("  .que files: %d total, %d with fired triggers (%ld KB)\n",
             que_count, que_fired, total_bytes / 1024);
    }

    printf("\n");
    g_free(diff_dir);
  }

  return(0);
}
