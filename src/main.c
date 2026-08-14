#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include "version.h"
#include "build_number.h"
#include "ui.h"
#include "config.h"
#include "arc.h"
#include "arz.h"
#include "texture.h"
#include "asset_lookup.h"
#include "affix_table.h"
#include "item_stats.h"
#include "prefetch.h"
#include "translation.h"
#include "db_browser_cache.h"
#include "db_creatures.h"
#include "creature_thumbs.h"
#ifndef _WIN32
#include <glib-unix.h>
#include <signal.h>
#endif

static int g_saved_argc;
static char **g_saved_argv;

// Strips Pango markup tags from a string in-place.
// s: nul-terminated string to clean.
static void
strip_markup_inplace(char *s)
{
  char *r = s, *w = s;

  while(*r)
  {
    if(*r == '<')
    {
      while(*r && *r != '>')
        r++;

      if(*r == '>')
        r++;
    }
    else
      *w++ = *r++;
  }

  *w = '\0';
}

// Dumps all variables and the rendered tooltip from a DBR record.
// record_path: the backslash-delimited DBR path (e.g. "records\\...\\foo.dbr")
static void
dump_dbr(const char *record_path, const char *prefix_path, const char *suffix_path)
{
  TQArzRecordData *data = asset_get_dbr(record_path);

  if(!data)
  {
    printf("DBR not found: %s\n", record_path);
    return;
  }

  printf("\n--- DBR Dump: %s ---\n", record_path);
  for(uint32_t i = 0; i < data->num_vars; i++)
  {
    TQVariable *v = &data->vars[i];

    if(!v->name)
      continue;

    if(v->type == TQ_VAR_STRING && v->count > 0 && v->value.str)
    {
      printf("  %s =", v->name);
      for(uint32_t j = 0; j < v->count; j++)
        printf(" \"%s\"", v->value.str[j] ? v->value.str[j] : "(null)");
      printf("\n");
    }
    else if(v->type == TQ_VAR_INT && v->count > 0 && v->value.i32)
    {
      printf("  %s =", v->name);
      for(uint32_t j = 0; j < v->count; j++)
        printf(" %d", v->value.i32[j]);
      printf("\n");
    }
    else if(v->type == TQ_VAR_FLOAT && v->count > 0 && v->value.f32)
    {
      printf("  %s =", v->name);
      for(uint32_t j = 0; j < v->count; j++)
        printf(" %.4f", v->value.f32[j]);
      printf("\n");
    }
    else
    {
      printf("  %s = (type=%d count=%u)\n", v->name, v->type, v->count);
    }
  }

  TQTranslation *tr = translation_init();
  if(tr && global_config.game_folder)
  {
    char trans_path[1024];

    snprintf(trans_path, sizeof(trans_path), "%s/Text/Text_EN.arc", global_config.game_folder);
    translation_load_from_arc(tr, trans_path);
  }

  char buf[16384];
  BufWriter w;

  buf_init(&w, buf, sizeof(buf));
  add_stats_from_record(record_path, tr, &w, "white", 0);

  printf("\n--- Tooltip render: %s ---\n", record_path);
  strip_markup_inplace(buf);
  printf("%s", buf);

  // Full item-card render (the GUI tooltip): includes sections that live in the
  // formatter rather than in add_stats_from_record -- e.g. the "Grants Skill"
  // block and the summoned-pet attributes/abilities.  Seed is hidden (the item
  // is never spawned here, so it would always be zero and meaningless).
  {
    char card[16384];

    vault_item_format_stats_flags(
        &(TQVaultItem){ .base_name = (char *)record_path,
                        .prefix_name = (char *)prefix_path,
                        .suffix_name = (char *)suffix_path }, tr,
        NULL, ITEM_FMT_HIDE_SEED, card, sizeof(card));

    printf("\n--- Full item card: %s ---\n", record_path);
    strip_markup_inplace(card);
    printf("%s", card);
  }

  // Classification of the raw (un-normalized) path, exactly as the GUI sees it.
  // Lets us verify separator/case handling drives colour + context-menu items.
  printf("\n--- Classification (raw path) ---\n");
  printf("colour=%s relic_or_charm=%d artifact=%d stackable=%d "
         "single_piece=%d max_shards=%d\n",
         get_item_color(record_path, NULL, NULL),
         item_is_relic_or_charm(record_path),
         item_is_artifact(record_path),
         item_is_stackable_type(&(TQVaultItem){ .base_name = (char *)record_path }),
         relic_is_single_piece(record_path),
         relic_max_shards(record_path));

  // Which bitmap the grid would draw, at zero shards vs one shard.  Single-piece
  // relics/charms must resolve to the same (complete) bitmap regardless of var1.
  char *bmp0 = item_resolve_bitmap(record_path, 0);
  char *bmp1 = item_resolve_bitmap(record_path, 1);

  printf("bitmap(var1=0)=%s\nbitmap(var1=1)=%s\n",
         bmp0 ? bmp0 : "(none)", bmp1 ? bmp1 : "(none)");
  free(bmp0);
  free(bmp1);

  // Affix-manager eligibility: the context menu shows "Modify Affixes" only when
  // the item passes the classification/type gate AND has applicable affix tables
  // (affix_available); fixed-magical reward/set items pass the former but not the
  // latter, so the menu item is correctly hidden for them.
  bool affix_moddable  = item_can_modify_affixes(record_path);
  bool affix_available = affix_table_get(record_path, tr) != NULL;

  printf("affix_moddable=%d affix_available=%d modify_affixes_menu=%d\n",
         affix_moddable, affix_available, affix_moddable && affix_available);

  // Aggregate requirements (level/str/dex/int) computed the same way the
  // equippability highlight does, for quick verification.
  int req[4];

  item_requirements(record_path, NULL, NULL, NULL, NULL, req);
  printf("\nRequirements: level=%d str=%d dex=%d int=%d\n",
         req[0], req[3], req[1], req[2]);

  if(tr)
    translation_free(tr);
}

// Headless unit test for stack_merge_onto(): relic/charm completion, the 100
// potion/scroll cap, overflow remainders, and the already-full no-op.  Needs an
// initialized asset manager (relic_max_shards reads completedRelicLevel).
// Returns 0 if every case passes, 1 otherwise.
static int
stack_merge_selftest(void)
{
  const char *RELIC  = "records\\item\\relics\\03_act1_zeusthunderbolt.dbr";   // max 3
  const char *CHARM  = "records\\xpack\\item\\charms\\03_act4_erebancrystal.dbr"; // max 5
  const char *POTION = "records\\item\\miscellaneous\\oneshot\\potionhealth_02.dbr"; // cap 100

  struct {
    const char *name;
    const char *path;
    bool        is_relic;       // count lives in var1 (relic/charm) vs stack_size
    int         tcur, hcur;     // starting target / held counts
    StackMergeResult exp_ret;   // expected outcome
    int         exp_t, exp_h;   // expected target / held counts afterwards
  } cases[] = {
    { "relic 0+0 (lone shards)",   RELIC,  true,   0, 0, STACK_MERGE_ABSORBED,      2,   0 },
    { "relic 0+0 charm style",     CHARM,  true,   0, 0, STACK_MERGE_ABSORBED,      2,   0 },
    { "relic 2+0 -> complete",     RELIC,  true,   2, 0, STACK_MERGE_ABSORBED,      3,   0 },
    { "relic 2+1 -> complete",     RELIC,  true,   2, 1, STACK_MERGE_ABSORBED,      3,   1 },
    { "relic 1+1 -> partial",      RELIC,  true,   1, 1, STACK_MERGE_ABSORBED,      2,   1 },
    { "relic 2+2 -> overflow",     RELIC,  true,   2, 2, STACK_MERGE_PARTIAL,       3,   1 },
    { "relic 3 full -> no-op",     RELIC,  true,   3, 1, STACK_MERGE_TARGET_FULL,   3,   1 },
    { "charm 4+1 -> complete",     CHARM,  true,   4, 1, STACK_MERGE_ABSORBED,      5,   1 },
    { "potion 30+20",              POTION, false, 30,20, STACK_MERGE_ABSORBED,     50,  20 },
    { "potion 80+20 -> cap",       POTION, false, 80,20, STACK_MERGE_ABSORBED,    100,  20 },
    { "potion 80+30 -> overflow",  POTION, false, 80,30, STACK_MERGE_PARTIAL,     100,  10 },
    { "potion 100 full -> no-op",  POTION, false,100, 5, STACK_MERGE_TARGET_FULL, 100,   5 },
  };

  int fails = 0;

  for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    TQVaultItem t, h;

    memset(&t, 0, sizeof(t));
    memset(&h, 0, sizeof(h));
    t.base_name = (char *)cases[i].path;
    h.base_name = (char *)cases[i].path;

    if(cases[i].is_relic)
    {
      t.var1 = (uint32_t)cases[i].tcur;
      h.var1 = (uint32_t)cases[i].hcur;
    }
    else
    {
      t.stack_size = cases[i].tcur;
      h.stack_size = cases[i].hcur;
    }

    StackMergeResult ret = stack_merge_onto(&t, &h);
    int gt  = cases[i].is_relic ? (int)t.var1 : t.stack_size;
    int gh  = cases[i].is_relic ? (int)h.var1 : h.stack_size;
    bool ok = (ret == cases[i].exp_ret && gt == cases[i].exp_t && gh == cases[i].exp_h);

    printf("[%s] %-26s ret=%d target=%d held=%d  (expect ret=%d target=%d held=%d)\n",
           ok ? "PASS" : "FAIL", cases[i].name, ret, gt, gh,
           cases[i].exp_ret, cases[i].exp_t, cases[i].exp_h);

    if(!ok)
      fails++;
  }

  printf("stack-merge-selftest: %s\n", fails ? "FAILURES" : "all passed");
  return(fails ? 1 : 0);
}

// Headless unit test for the shared SearchQuery matcher: one case per distinct
// code path through search_query_compile()/search_query_match() -- each of the
// four detected modes plus the semantics that separate them (a PHRASE is
// contiguous, '|' OR's alternatives, a broken regex degrades to a literal).
// Needs no game files.  Returns 0 if every case passes, 1 otherwise.
static int
search_query_selftest(void)
{
  const char *HAY = "+15% attack damage and 20 elemental damage to undead";

  struct {
    const char     *name;
    const char     *pattern;
    const char     *haystack;
    SearchQueryMode exp_mode;
    bool            exp_match;
  } cases[] = {
    { "empty matches everything", "",                       HAY, SEARCH_QUERY_EMPTY,   true  },
    { "whitespace-only is empty", "   ",                    HAY, SEARCH_QUERY_EMPTY,   true  },
    { "phrase single token",      "elemental",              HAY, SEARCH_QUERY_PHRASE,  true  },
    { "phrase contiguous hit",    "attack damage",          HAY, SEARCH_QUERY_PHRASE,  true  },
    { "phrase not token-AND",     "attack undead",          HAY, SEARCH_QUERY_PHRASE,  false },
    { "phrase keeps + and %",     "+15% attack",            HAY, SEARCH_QUERY_PHRASE,  true  },
    { "OR first alternative",     "attack damage|frostburn",HAY, SEARCH_QUERY_PHRASE,  true  },
    { "OR second alternative",    "frostburn|to undead",    HAY, SEARCH_QUERY_PHRASE,  true  },
    { "OR neither alternative",   "frostburn|bleeding",     HAY, SEARCH_QUERY_PHRASE,  false },
    { "OR alternatives trimmed",  "frostburn | elemental",  HAY, SEARCH_QUERY_PHRASE,  true  },
    { "regex group hit",          "(attack|pierce) damage", HAY, SEARCH_QUERY_REGEX,   true  },
    { "regex group miss",         "(cold|fire) damage",     HAY, SEARCH_QUERY_REGEX,   false },
    { "regex class hit",          "[0-9]+% attack",         HAY, SEARCH_QUERY_REGEX,   true  },
    { "regex unanchored",         "(undead)",               HAY, SEARCH_QUERY_REGEX,   true  },
    { "broken regex -> literal",  "(attack",                HAY, SEARCH_QUERY_LITERAL, false },
    { "NULL haystack no match",   "elemental",              NULL,SEARCH_QUERY_PHRASE,  false },
  };

  int fails = 0;

  for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    SearchQuery *q = search_query_compile(cases[i].pattern);
    char *hay = cases[i].haystack ? g_ascii_strdown(cases[i].haystack, -1) : NULL;
    bool match = search_query_match(q, hay);
    SearchQueryMode mode = search_query_mode(q);
    bool ok = (mode == cases[i].exp_mode && match == cases[i].exp_match);

    printf("[%s] %-26s mode=%-7s match=%-3s  (expect mode=%d match=%s)\n",
           ok ? "PASS" : "FAIL", cases[i].name, search_query_mode_name(q),
           match ? "yes" : "no", cases[i].exp_mode,
           cases[i].exp_match ? "yes" : "no");

    if(!ok)
      fails++;

    g_free(hay);
    search_query_free(q);
  }

  printf("search-query-selftest: %s\n", fails ? "FAILURES" : "all passed");
  return(fails ? 1 : 0);
}

// Headless driver for the one genuinely concurrent part of the app: the
// prefetch worker hitting asset_get_dbr()/arz_intern() while the main thread
// does the same, and asset_dbr_cache_clear() freeing records under both.  In
// the app that interleaving only happens with a window open, so without this
// there is nothing for the TSan build to watch -- and the 2026-06-26 SIGSEGVs
// (40c0d5a) were exactly these races, found by reading rather than by a tool.
//
// Asserts nothing itself: the verdict is the sanitizer's.  Run it under
// build-tsan (races) and build-asan (the use-after-free the clear hook
// exists to prevent).  Returns 0 unless the character will not load.
static int
prefetch_selftest(const char *chr_path)
{
  const int ROUNDS = 4;
  TQCharacter *chr = character_load(chr_path);
  int hits = 0;

  if(!chr)
  {
    fprintf(stderr, "prefetch-selftest: cannot load %s\n", chr_path);
    return(1);
  }

  for(int round = 0; round < ROUNDS; round++)
  {
    // Restart each round: the cache clear below joins the worker via the
    // hook, so the thread has to be brought back up to keep the race live.
    prefetch_for_character(chr);

    // Race the worker from this thread over the same records it is loading.
    for(int s = 0; s < chr->num_inv_sacks; s++)
    {
      TQVaultSack *sack = &chr->inv_sacks[s];

      for(int i = 0; i < sack->num_items; i++)
      {
        const char *paths[] = {
          sack->items[i].base_name, sack->items[i].prefix_name,
          sack->items[i].suffix_name, sack->items[i].relic_name
        };

        for(int p = 0; p < 4; p++)
        {
          if(paths[p] && paths[p][0] && asset_get_dbr(paths[p]))
            hits++;
        }
      }
    }

    // Pull the rug out mid-flight; the hook must join the worker before any
    // record it still points into is freed.
    asset_dbr_cache_clear();
  }

  prefetch_cancel();
  character_free(chr);
  printf("prefetch-selftest: %d rounds, %d main-thread lookups, no sanitizer report\n",
         ROUNDS, hits);
  return(0);
}

// Loads a character and reports whether it can equip the given item, showing
// the computed requirements, attributes, requirement reductions, and verdict.
// Used to verify the equippability highlight without launching the GUI.
// chr_path:  path to a Player.chr file.
// item_path: backslash-delimited DBR path of the item to test.
static void
equip_check(const char *chr_path, const char *item_path)
{
  TQCharacter *chr = character_load(chr_path);

  if(!chr)
  {
    printf("equip-check: failed to load character: %s\n", chr_path);
    return;
  }

  int req[4];

  item_requirements(item_path, NULL, NULL, NULL, NULL, req);

  float str = 0.0f, dex = 0.0f, intel = 0.0f;

  character_buffed_attributes(chr, &str, &dex, &intel);

  TQVaultItem it = {0};

  it.base_name = (char *)item_path;
  bool ok = item_is_equippable(chr, &it);

  printf("\n--- Equip check: %s ---\n", item_path);
  printf("Character: %s (level %u)\n",
         chr->character_name ? chr->character_name : "(unknown)", chr->level);
  printf("Raw requirements:   level=%d str=%d dex=%d int=%d\n",
         req[0], req[3], req[1], req[2]);
  printf("Effective attribs:  str=%d dex=%d int=%d\n",
         (int)(str + 0.5f), (int)(dex + 0.5f), (int)(intel + 0.5f));
  printf("Req reduction:      global=%.0f%% level=%.0f%%\n",
         character_stat_total(chr, "characterGlobalReqReduction"),
         character_stat_total(chr, "characterLevelReqReduction"));
  printf("Verdict: %s\n", ok ? "EQUIPPABLE (green)" : "NOT equippable (red)");

  // Render the item card's requirement lines exactly as the tooltip does, to
  // verify the "(<reduced> reduction)" annotation.
  ItemReqReduction red;

  item_req_reduction(chr, item_path, &red);

  char card[16384];

  vault_item_format_stats_ex(&it, NULL, &red, card, sizeof(card));
  strip_markup_inplace(card);

  char *save = NULL;

  printf("Card requirement lines:\n");
  for(char *line = strtok_r(card, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    if(strstr(line, "Required"))
      printf("  %s\n", line);

  character_free(chr);
}

// Runs debug tests: prints config paths, tests asset lookup, and dumps
// any DBR paths passed as command-line arguments.
// argc: argument count
// argv: argument vector (non-"--debug" args are treated as DBR paths)
static void
debug_run_tests(int argc, char **argv)
{
  printf("--- TQVaultC Debug Tests ---\n");
  printf("Game Folder: %s\n", global_config.game_folder ? global_config.game_folder : "NOT SET");
  printf("Save Folder: %s\n", global_config.save_folder ? global_config.save_folder : "NOT SET");

  if(!global_config.game_folder)
    return;

  const char *test_asset = "records\\items\\geararmor\\torso\\t_plate01.dbr";
  const TQAssetEntry *entry = asset_lookup(test_asset);

  if(entry)
    printf("SUCCESS: Found %s in %s at offset %u\n", test_asset, asset_get_file_path(entry->file_id), entry->offset);
  else
    printf("FAILURE: Could not find %s in index (this is expected if index is dummy)\n", test_asset);

  for(int i = 1; i < argc; i++)
  {
    if(strcmp(argv[i], "--debug") == 0)
      continue;
    dump_dbr(argv[i], NULL, NULL);
  }

  printf("\n--- Debug Tests Complete ---\n");
}

#ifndef _WIN32
// SIGINT/SIGTERM handler. g_unix_signal_add dispatches this from the GLib main
// loop, not from async-signal context, so ordinary GLib calls are safe here:
// stop the prefetch worker and quit so main()'s shutdown path still runs.
// user_data: the GApplication.
// Returns: G_SOURCE_REMOVE (one shutdown is enough).
static gboolean
on_term_signal(gpointer user_data)
{
  fprintf(stderr, "tqvaultc: signal received, shutting down\n");
  prefetch_cancel();
  g_application_quit(G_APPLICATION(user_data));
  return(G_SOURCE_REMOVE);
}
#endif

// GTK activate callback. Initializes the asset manager, string interning,
// item stats, and affix tables when a game folder is configured, then
// either shows the first-run setup or activates the main UI.
// app: the GtkApplication instance
// user_data: unused
static void
on_activate(GtkApplication *app, gpointer user_data)
{
  (void)user_data;

  // Debug builds keep the old synchronous init + probe path (the setup popup
  // and its bundled cache build are skipped here so debug_run_tests can probe
  // the freshly-initialized asset manager).
  if(tqvc_debug)
  {
    if(global_config.game_folder)
    {
      printf("Main: Initializing asset manager...\n");
      asset_manager_init(global_config.game_folder);
      arz_intern_init();
      item_stats_init();
      affix_table_init(NULL);
      printf("Main: Asset manager + interns + item stats + affix initialized.\n");
    }

    debug_run_tests(g_saved_argc, g_saved_argv);

    if(config_is_first_run())
      ui_first_run_setup(app);
    else
      ui_app_activate(app, NULL);
    return;
  }

  // Normal startup: first-run wizard collects folders (then on_first_run_save
  // runs the shared init+activate path); otherwise init + activate directly,
  // building the Database Browser cache behind a one-time popup if needed.
  if(config_is_first_run())
    ui_first_run_setup(app);
  else
    ui_startup_init_and_activate(app);
}

// Progress callback for --creature-thumbs-build: a periodic one-line counter.
static void
thumbs_build_progress(int done, int total, void *user)
{
  (void)user;
  if(done == total || done % 25 == 0)
    printf("\r  %d / %d", done, total), fflush(stdout);
  if(done == total)
    printf("\n");
}

// Program entry point. Parses command-line flags (--version, --debug),
// initializes config, creates the GTK application, and runs the main loop.
// argc: argument count
// argv: argument vector
// returns: GTK application exit status
int
main(int argc, char **argv)
{
#ifdef _WIN32
  // The Windows GUI subsystem detaches stdout/stderr — redirect them to a
  // logfile under our cache dir so init traces, GLib warnings, and
  // config_save errors are captured for support.
  {
    char *log_dir = tqvc_cache_dir_new();
    char *log_path = g_build_filename(log_dir, "tqvaultc.log", NULL);
    FILE *log = freopen(log_path, "w", stderr);
    if(log)
      setvbuf(log, NULL, _IOLBF, 0);
    freopen(log_path, "w", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "tqvaultc: log opened at %s\n", log_path);
    g_free(log_path);
    g_free(log_dir);
  }
#endif

  const char *config_override = NULL;
  bool debug_mode = false;

  bool tooltip_only = false;
  const char *tooltip_path = NULL;

  bool equip_check_only = false;
  const char *equip_chr_path = NULL;
  const char *equip_item_path = NULL;

  bool skill_bonus_only = false;
  const char *skill_bonus_chr_path = NULL;

  bool db_cache_selftest_only = false;
  bool db_search_selftest_only = false;
  bool db_sort_selftest_only = false;
  bool affix_items_selftest_only = false;
  const char *affix_items_query = NULL;
  const char *db_search_keywords = NULL;
  bool search_query_selftest_only = false;
  const char *sq_pattern = NULL;
  const char *sq_haystack = NULL;
  bool thumbs_build_only = false;
  bool first_run_build_only = false;
  bool stack_merge_selftest_only = false;
  bool prefetch_selftest_only = false;
  const char *prefetch_chr_path = NULL;
  const char *tooltip_prefix = NULL;   // optional --prefix for --tooltip
  const char *tooltip_suffix = NULL;   // optional --suffix for --tooltip

  for(int i = 1; i < argc; i++)
  {
    if(strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
    {
      printf("TQVaultC v%s (Build #%d)\n", TQVAULTC_VERSION, TQVAULTC_BUILD_NUMBER);
      return(0);
    }
    else if(strcmp(argv[i], "--debug") == 0)
    {
      debug_mode = true;
    }
    else if(strcmp(argv[i], "--tooltip") == 0 && i + 1 < argc)
    {
      tooltip_only = true;
      tooltip_path = argv[++i];
    }
    else if(strcmp(argv[i], "--prefix") == 0 && i + 1 < argc)
      tooltip_prefix = argv[++i];
    else if(strcmp(argv[i], "--suffix") == 0 && i + 1 < argc)
      tooltip_suffix = argv[++i];
    else if(strcmp(argv[i], "--equip-check") == 0 && i + 2 < argc)
    {
      equip_check_only = true;
      equip_chr_path = argv[++i];
      equip_item_path = argv[++i];
    }
    else if(strcmp(argv[i], "--skill-bonuses") == 0 && i + 1 < argc)
    {
      skill_bonus_only = true;
      skill_bonus_chr_path = argv[++i];
    }
    else if(strcmp(argv[i], "--db-cache-selftest") == 0)
    {
      db_cache_selftest_only = true;
    }
    else if(strcmp(argv[i], "--db-search-selftest") == 0 && i + 1 < argc)
    {
      db_search_selftest_only = true;
      db_search_keywords = argv[++i];
    }
    else if(strcmp(argv[i], "--db-sort-selftest") == 0)
    {
      db_sort_selftest_only = true;
    }
    else if(strcmp(argv[i], "--affix-items") == 0 && i + 1 < argc)
    {
      affix_items_selftest_only = true;
      affix_items_query = argv[++i];
    }
    else if(strcmp(argv[i], "--search-query-selftest") == 0)
    {
      // Two trailing args = the interactive "explain this one query" form.
      // Bare = the built-in case table, which is what `meson test` runs.
      search_query_selftest_only = true;

      if(i + 2 < argc)
      {
        sq_pattern = argv[++i];
        sq_haystack = argv[++i];
      }
    }
    else if(strcmp(argv[i], "--creature-thumbs-build") == 0)
    {
      thumbs_build_only = true;
    }
    else if(strcmp(argv[i], "--first-run-build") == 0)
    {
      first_run_build_only = true;
    }
    else if(strcmp(argv[i], "--stack-merge-selftest") == 0)
    {
      stack_merge_selftest_only = true;
    }
    else if(strcmp(argv[i], "--prefetch-selftest") == 0 && i + 1 < argc)
    {
      prefetch_selftest_only = true;
      prefetch_chr_path = argv[++i];
    }
    else
    {
      config_override = argv[i];
    }
  }

  tqvc_debug = debug_mode;
  config_init(config_override);

  g_saved_argc = argc;
  g_saved_argv = argv;

  if(tooltip_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --tooltip: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);
    dump_dbr(tooltip_path, tooltip_prefix, tooltip_suffix);
    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(0);
  }

  if(equip_check_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --equip-check: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);
    equip_check(equip_chr_path, equip_item_path);
    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(0);
  }

  if(skill_bonus_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --skill-bonuses: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);

    TQCharacter *chr = character_load(skill_bonus_chr_path);

    if(chr)
    {
      skills_debug_print_gear_bonuses(chr);
      character_free(chr);
    }
    else
    {
      fprintf(stderr, "tqvaultc --skill-bonuses: failed to load %s\n", skill_bonus_chr_path);
    }

    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(0);
  }

  if(db_cache_selftest_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --db-cache-selftest: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);

    int rc = db_browser_cache_selftest();

    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(rc);
  }

  if(db_search_selftest_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --db-search-selftest: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);

    int rc = db_browser_search_selftest(db_search_keywords);

    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(rc);
  }

  if(db_sort_selftest_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --db-sort-selftest: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);

    int rc = db_browser_sort_selftest();

    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(rc);
  }

  if(affix_items_selftest_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --affix-items: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);

    int rc = db_browser_affix_items_selftest(affix_items_query);

    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(rc);
  }

  if(stack_merge_selftest_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --stack-merge-selftest: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);

    int rc = stack_merge_selftest();

    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(rc);
  }

  if(prefetch_selftest_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --prefetch-selftest: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();

    int rc = prefetch_selftest(prefetch_chr_path);

    prefetch_free();
    item_stats_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(rc);
  }

  if(search_query_selftest_only)
  {
    if(!sq_pattern)
    {
      int rc = search_query_selftest();

      config_free();
      return(rc);
    }

    // Pure unit test for the shared matcher: no game files needed.  Lowercase
    // the haystack the same way the real callers do before matching.
    SearchQuery *q = search_query_compile(sq_pattern);
    char *hay = g_ascii_strdown(sq_haystack, -1);
    bool match = search_query_match(q, hay);

    printf("pattern : \"%s\"\n", sq_pattern);
    printf("mode    : %s\n", search_query_mode_name(q));
    printf("haystack: \"%s\"\n", sq_haystack);
    printf("result  : %s\n", match ? "MATCH" : "no match");

    g_free(hay);
    search_query_free(q);
    config_free();
    return(0);
  }

  if(thumbs_build_only)
  {
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --creature-thumbs-build: game_folder not configured\n");
      return(1);
    }

    asset_manager_init(global_config.game_folder);
    arz_intern_init();
    item_stats_init();
    affix_table_init(NULL);

    // Shared database handle if cached, else load our own (mirrors startup).
    TQArzFile *arz = asset_get_database_arz();
    TQArzFile *own_arz = NULL;

    if(!arz)
    {
      char arz_path[1024];

      snprintf(arz_path, sizeof(arz_path), "%s/Database/database.arz",
               global_config.game_folder);
      arz = own_arz = arz_load(arz_path);
    }

    int rc = 1;

    if(arz)
    {
      gint64 t0 = g_get_monotonic_time();
      DbCreatureIndex *idx = db_creature_index_build(arz);

      if(idx)
      {
        printf("Rendering thumbnails for %u creatures…\n", idx->creatures->len);
        creature_thumbs_build(idx, global_config.game_folder,
                              thumbs_build_progress, NULL);
        double secs = (g_get_monotonic_time() - t0) / 1e6;

        printf("Done in %.1fs.\n", secs);
        db_creature_index_free(idx);
        rc = 0;
      }
    }
    else
      fprintf(stderr, "tqvaultc --creature-thumbs-build: no database.arz\n");

    if(own_arz)
      arz_free(own_arz);
    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(rc);
  }

  if(first_run_build_only)
  {
    // Headless mirror of the first-run "Setting up TQVaultC…" build: runs the
    // exact index/blob/save sequence in one process and reports peak RSS, so we
    // can measure the first-run memory footprint without launching the GUI (the
    // low-RAM Windows lock-up this guards against is a memory problem).
    if(!global_config.game_folder)
    {
      fprintf(stderr, "tqvaultc --first-run-build: game_folder not configured\n");
      return(1);
    }

    gint64 t0 = g_get_monotonic_time();

    ui_startup_build_headless();   // initialises the asset subsystem and builds

    double secs = (g_get_monotonic_time() - t0) / 1e6;
    double ws_mb = -1.0, commit_mb = -1.0;

    tq_proc_peak_mem_mb(&ws_mb, &commit_mb);
    if(commit_mb >= 0.0)
      printf("first-run build done in %.1fs, peak working set %.1f MB, "
             "peak commit %.1f MB\n", secs, ws_mb, commit_mb);
    else
      printf("first-run build done in %.1fs, peak RSS %.1f MB\n", secs, ws_mb);

    item_stats_free();
    affix_table_free();
    arz_intern_free();
    asset_manager_free();
    config_free();
    return(0);
  }

  // Strip our custom flags so GTK doesn't see them
  int gtk_argc = 0;
  char **gtk_argv = malloc(sizeof(char *) * (argc + 1));

  if(!gtk_argv)
  {
    fprintf(stderr, "main: malloc failed for gtk_argv\n");
    return(1);
  }

  for(int i = 0; i < argc; i++)
  {
    if(strcmp(argv[i], "--debug") == 0)
      continue;
    gtk_argv[gtk_argc++] = argv[i];
  }
  gtk_argv[gtk_argc] = NULL;

  if(tqvc_debug)
    printf("Main: Creating GTK application...\n");

  GtkApplication *app = gtk_application_new("org.tqvaultinc.tqvaultc", G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

  // Ctrl-C / a session logout should end the prefetch thread and free the
  // caches rather than killing the process mid-work. (No Windows equivalent:
  // glib-unix is POSIX-only and the GUI build has no console to interrupt.)
#ifndef _WIN32
  g_unix_signal_add(SIGINT, on_term_signal, app);
  g_unix_signal_add(SIGTERM, on_term_signal, app);
#endif

  if(tqvc_debug)
    printf("Main: Running GTK application...\n");

  int status = g_application_run(G_APPLICATION(app), gtk_argc, gtk_argv);

  free(gtk_argv);
  if(tqvc_debug)
    printf("Main: GTK application finished with status %d.\n", status);

  prefetch_free();
  item_stats_free();
  affix_table_free();
  arz_intern_free();
  asset_manager_free();
  config_free();
  g_object_unref(app);
  return(status);
}
