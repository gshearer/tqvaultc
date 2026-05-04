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

  if(tr)
    translation_free(tr);
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

  // Initialize asset manager (loads index + pre-loads ARZ mmaps)
  if(global_config.game_folder)
  {
    if(tqvc_debug)
      printf("Main: Initializing asset manager...\n");
    asset_manager_init(global_config.game_folder);
    if(tqvc_debug)
      printf("Main: Asset manager initialized.\n");

    arz_intern_init();
    item_stats_init();
    if(tqvc_debug)
      printf("Main: String intern + item stats initialized.\n");

    affix_table_init(NULL);
    if(tqvc_debug)
      printf("Main: Affix table initialized.\n");
  }

  if(tqvc_debug)
    debug_run_tests(g_saved_argc, g_saved_argv);

  // Build the main UI, or show first-run setup if no config exists
  if(config_is_first_run())
    ui_first_run_setup(app);
  else
    ui_app_activate(app, NULL);
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
