// tq_dbr_tool.c -- Universal DBR/ARC inspection tool for TQVaultC development.
//
// Works directly against testdata/database.arz and game arc files without
// requiring the resource index or game installation path.
//
// Usage:
//   tq-dbr-tool <command> [options]
//
// Commands:
//   dump    <arz> <record_path>            Dump all variables from a DBR record
//   search  <arz> <pattern>                List records matching a path pattern
//   fields  <arz> <pattern> <field,...>     Show specific fields for matching records
//   stats   <arz> <pattern>                Show non-zero numeric variables for matching records
//   arctxt  <arc> <search_term>            Search for text in arc text files (UTF-16 aware)
//   arcls   <arc>                          List all files in an arc archive
//   archex  <arc> <file_pattern>           Extract and hex-dump a file from an arc archive
//   bonus   <arz> <item_path>              Follow bonus table chain for a relic/charm/artifact


#include "tq_dbr_tool.h"
#include "../parse_num.h"
#include "cli_args.h"

// Prints usage information for all commands to stderr.
// prog: the program name (argv[0]).
static void
usage(const char *prog)
{
  fprintf(stderr,
    "Usage: %s <command> [options]\n"
    "\n"
    "Commands:\n"
    "  dump    <arz> <record_path>          Dump all variables from a DBR record\n"
    "  search  <arz> <pattern>              List records matching path substring\n"
    "  fields  <arz> <pattern> <field,...>   Show specific fields for matching records\n"
    "  stats   <arz> <pattern>              Show non-zero numeric vars for matching records\n"
    "  arctxt  <arc> <search_term>          Search text in arc files (UTF-16 aware)\n"
    "  arcls   <arc>                        List all files in an arc archive\n"
    "  archex  <arc> <file_pattern>         Extract and hex-dump a file from an arc archive\n"
    "  bonus   <arz> <item_path>            Follow bonus table chain for relic/charm/artifact\n"
    "  coverage <arz> [path_substr]         Sorted list of all vars with non-zero values\n"
    "  categories <arz>                     Count items per Database Browser category\n"
    "  sets <arz>                           List item sets, members and bonus tiers\n"
    "  affixes <arz>                        List prefixes/suffixes, their gear and stats\n"
    "  skills <arz>                         List masteries and their skills (max level, tier)\n"
    "  loot <arz> <table> [level]           Flatten a loot table to its items + chances\n"
    "  creatures <arz>                      Summarize boss/hero loot index ('dropped by')\n"
    "  droppedby <arz> <item>               List creatures that drop an item, per difficulty\n"
    "  quests <arz> <resources_dir>         Summarize quest item-reward index\n"
    "\n"
    "Examples:\n"
    "  %s dump testdata/database.arz records/xpack4/item/relics/x4_relic05.dbr\n"
    "  %s search testdata/database.arz xpack4/item/relics/\n"
    "  %s fields testdata/database.arz xpack4/item/lootmagicalaffixes/ description,lootRandomizerName,FileDescription\n"
    "  %s stats testdata/database.arz xpack4/item/lootmagicalaffixes/x4_relic05\n"
    "  %s arctxt /path/to/Text_EN.arc x4tagU_Relic\n"
    "  %s arcls /path/to/Text_EN.arc\n"
    "  %s archex testdata/gamefiles/Resources/Items.arc items/equipmenthead\n"
    "  %s bonus testdata/database.arz records/xpack4/item/relics/x4_relic05.dbr\n"
    "  %s categories testdata/gamefiles/Database/database.arz\n"
    "  %s sets testdata/gamefiles/Database/database.arz\n"
    "  %s affixes testdata/gamefiles/Database/database.arz\n"
    "  %s skills testdata/gamefiles/Database/database.arz\n",
    prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
    prog);
}

// Every subcommand, with the argc it needs and the argument list its usage
// line prints.
static const CliCommand COMMANDS[] = {
  {"dump",       4, "<arz> <record_path>"},
  {"search",     4, "<arz> <pattern>"},
  {"fields",     5, "<arz> <pattern> <field,...>"},
  {"stats",      4, "<arz> <pattern>"},
  {"arctxt",     4, "<arc> <search_term>"},
  {"arcls",      3, "<arc>"},
  {"arcextract", 5, "<arc> <file_pattern> <out_path>"},
  {"meshrender", 6, "<arc> <mesh_substr> <tex_substr|-> <out.png> "
                    "[size] [yaw] [pitch] [anm_substr|-] [frame]"},
  {"archex",     4, "<arc> <file_pattern>"},
  {"bonus",      4, "<arz> <item_path>"},
  {"coverage",   3, "<arz> [path_substr]"},
  {"categories", 3, "<arz>"},
  {"sets",       3, "<arz>"},
  {"skills",     3, "<arz>"},
  {"affixes",    3, "<arz>"},
  {"loot",       4, "<arz> <table_path> [level]"},
  {"creatures",  3, "<arz>"},
  {"droppedby",  4, "<arz> <item_path>"},
  {"quests",     4, "<arz> <resources_dir>"},
  {NULL, 0, NULL}
};

// Runs the named subcommand.  The caller has already validated argc against
// the COMMANDS table, so every argv index used here exists.
// Returns the process exit code.
static int
run_command(const char *cmd, int argc, char **argv)
{
  if(strcmp(cmd, "dump") == 0)
    return(cmd_dump(argv[2], argv[3]));

  if(strcmp(cmd, "search") == 0)
    return(cmd_search(argv[2], argv[3]));

  if(strcmp(cmd, "fields") == 0)
    return(cmd_fields(argv[2], argv[3], argv[4]));

  if(strcmp(cmd, "stats") == 0)
    return(cmd_stats(argv[2], argv[3]));

  if(strcmp(cmd, "arctxt") == 0)
    return(cmd_arctxt(argv[2], argv[3]));

  if(strcmp(cmd, "arcls") == 0)
    return(cmd_arcls(argv[2]));

  if(strcmp(cmd, "arcextract") == 0)
    return(cmd_arcextract(argv[2], argv[3], argv[4]));

  if(strcmp(cmd, "meshrender") == 0)
    return(cmd_meshrender(argc, argv));

  if(strcmp(cmd, "archex") == 0)
    return(cmd_archex(argv[2], argv[3]));

  if(strcmp(cmd, "bonus") == 0)
    return(cmd_bonus(argv[2], argv[3]));

  if(strcmp(cmd, "coverage") == 0)
    return(cmd_coverage(argv[2], argc >= 4 ? argv[3] : ""));

  if(strcmp(cmd, "categories") == 0)
    return(cmd_categories(argv[2]));

  if(strcmp(cmd, "sets") == 0)
    return(cmd_sets(argv[2]));

  if(strcmp(cmd, "skills") == 0)
    return(cmd_skills(argv[2]));

  if(strcmp(cmd, "affixes") == 0)
    return(cmd_affixes(argv[2]));

  if(strcmp(cmd, "loot") == 0)
  {
    int level = 30;

    if(argc >= 5 && !parse_int(argv[4], &level))
    {
      fprintf(stderr, "loot: level must be a number\n");
      return(1);
    }

    return(cmd_loot(argv[2], argv[3], level));
  }

  if(strcmp(cmd, "creatures") == 0)
    return(cmd_creatures(argv[2]));

  if(strcmp(cmd, "droppedby") == 0)
    return(cmd_droppedby(argv[2], argv[3]));

  if(strcmp(cmd, "quests") == 0)
    return(cmd_quests(argv[2], argv[3]));

  // Reachable only if a COMMANDS row was added without a branch above.
  fprintf(stderr, "internal error: no handler for command '%s'\n", cmd);
  return(1);
}

// Entry point. Dispatches to the appropriate subcommand handler.
// argc: argument count (must be >= 2).
// argv: argument vector; argv[1] is the command name.
// Returns 0 on success, 1 on failure or unknown command.
int
main(int argc, char **argv)
{
  if(argc < 2)
  {
    usage(argv[0]);
    return(1);
  }

  const char *cmd = argv[1];

  if(strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)
  {
    usage(argv[0]);
    return(0);
  }

  CliArgsResult ck = cli_check_args(COMMANDS, argv[0], cmd, argc);

  if(ck == CLI_ARGS_TOO_FEW)
    return(1);

  if(ck == CLI_ARGS_UNKNOWN)
  {
    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage(argv[0]);
    return(1);
  }

  return(run_command(cmd, argc, argv));
}
