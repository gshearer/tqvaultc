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
dump_dbr(const char *record_path)
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

  // Aggregate requirements (level/str/dex/int) computed the same way the
  // equippability highlight does, for quick verification.
  int req[4];

  item_requirements(record_path, NULL, NULL, NULL, NULL, req);
  printf("\nRequirements: level=%d str=%d dex=%d int=%d\n",
         req[0], req[3], req[1], req[2]);

  if(tr)
    translation_free(tr);
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

  printf("Card requirement lines:\n");
  for(char *line = strtok(card, "\n"); line; line = strtok(NULL, "\n"))
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
    dump_dbr(argv[i]);
  }

  printf("\n--- Debug Tests Complete ---\n");
}

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
    dump_dbr(tooltip_path);
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
