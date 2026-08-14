// tq_quest_tool.c -- Universal quest data inspection and manipulation tool
//
// Parses and analyzes all three quest state file types:
//   - QuestToken.myw -- flat token bag
//   - Quest.myw -- trigger log + rewards section
//   - *.que files -- per-quest state machines
//
// Usage:
//   tq-quest-tool <command> [options]
//
// Token Commands:
//   dump     <myw>                   List all tokens in a QuestToken.myw file
//   count    <myw>                   Count tokens
//   search   <myw> <pattern>         List tokens matching a substring (case-insensitive)
//   has      <myw> <token>           Check if a specific token exists
//   acts     <myw>                   Group tokens by act (using prefix heuristics)
//   quests   <myw>                   Show quest completion status against quest_defs[]
//   add      <myw> <token>           Add a token to the file
//   remove   <myw> <token>           Remove a token from the file
//   complete <myw> <quest_name>      Add all tokens for a named quest
//   clear    <myw> <quest_name>      Remove all tokens for a named quest
//   roundtrip <myw>                  Load, save to temp, compare (verify parser/writer)
//   defs                             List all quest definitions
//   diff     <myw_a> <myw_b>        Show tokens present in one file but not the other
//   coverage <myw>                   Report covered/orphaned/uncovered tokens vs quest_defs[]
//
// Quest State Commands:
//   dump-que      <file>             Full structural dump of .que file (all fields)
//   dump-quest-myw <file>            Full Quest.myw parser (triggers + rewards + MD5 mapping)
//   clear-que     <dir>              Zero all hasFired/isPendingFire in .que files
//   compare-que   <dir_a> <dir_b>    Compare .que flag differences between directories
//   que-info      <dir>              Identify/categorize all .que files (embedded paths, flags)
//
// Analysis Commands:
//   scan          <save_dir>         Full overview of a character's quest state across all files

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <glib.h>
#include "../quest_tokens.h"
#include "tq_quest_tool.h"
#include "cli_args.h"

// ── Helpers ──────────────────────────────────────────────────────────────

// usage -- print program usage to stderr.
// prog: program name (argv[0]).
static void
usage(const char *prog)
{
  fprintf(stderr,
    "Usage: %s <command> [options]\n"
    "\n"
    "Token Commands (QuestToken.myw):\n"
    "  dump     <myw>                 List all tokens in a QuestToken.myw file\n"
    "  count    <myw>                 Count tokens\n"
    "  search   <myw> <pattern>       List tokens matching substring (case-insensitive)\n"
    "  has      <myw> <token>         Check if a specific token exists (exact match)\n"
    "  acts     <myw>                 Group tokens by act using prefix heuristics\n"
    "  quests   <myw>                 Show quest completion status vs quest_defs[]\n"
    "  add      <myw> <token>         Add a token to the file (saves in-place, .bak created)\n"
    "  remove   <myw> <token>         Remove a token from the file (saves in-place, .bak created)\n"
    "  complete <myw> <quest_name>    Add all tokens for a named quest\n"
    "  clear    <myw> <quest_name>    Remove all tokens for a named quest\n"
    "  roundtrip <myw>                Load, save to /tmp, byte-compare with original\n"
    "  defs                           List all quest definitions\n"
    "  diff     <myw_a> <myw_b>      Show tokens present in one file but not the other\n"
    "  coverage <myw>                 Report covered/orphaned/uncovered tokens vs quest_defs[]\n"
    "\n"
    "Quest State Commands (.que files + Quest.myw):\n"
    "  dump-que       <file>          Full structural dump of .que file (all fields)\n"
    "  dump-quest-myw <file>          Full Quest.myw dump (triggers + rewards + MD5 mapping)\n"
    "  clear-que      <dir>           Zero all hasFired/isPendingFire in .que files\n"
    "  compare-que    <dir_a> <dir_b> Compare .que flag differences between directories\n"
    "  que-info       <dir>           Identify/categorize all .que files in directory\n"
    "\n"
    "Analysis Commands:\n"
    "  scan           <save_dir>      Full character quest state overview across all files\n"
    "\n"
    "Examples:\n"
    "  %s dump testdata/saves/_soothie/Levels_World_World01.map/Legendary/QuestToken.myw\n"
    "  %s dump-que testdata/saves/_soothie/Levels_World_World01.map/Legendary/0273f539*.que\n"
    "  %s dump-quest-myw testdata/saves/_soothie/Levels_World_World01.map/Legendary/Quest.myw\n"
    "  %s que-info testdata/saves/_soothie/Levels_World_World01.map/Legendary/\n"
    "  %s scan testdata/saves/_soothie/\n"
    "  %s defs\n",
    prog, prog, prog, prog, prog, prog, prog);
}

// ── Main ─────────────────────────────────────────────────────────────────

// Every subcommand, with the argc it needs and the argument list its usage
// line prints.
static const CliCommand COMMANDS[] = {
  {"dump",           3, "<myw>"},
  {"count",          3, "<myw>"},
  {"search",         4, "<myw> <pattern>"},
  {"has",            4, "<myw> <token>"},
  {"acts",           3, "<myw>"},
  {"quests",         3, "<myw>"},
  {"add",            4, "<myw> <token>"},
  {"remove",         4, "<myw> <token>"},
  {"complete",       4, "<myw> <quest_name>"},
  {"clear",          4, "<myw> <quest_name>"},
  {"roundtrip",      3, "<myw>"},
  {"defs",           2, ""},
  {"diff",           4, "<myw_a> <myw_b>"},
  {"coverage",       3, "<myw>"},
  {"dump-que",       3, "<file>"},
  {"dump-quest-myw", 3, "<file>"},
  {"clear-que",      3, "<dir>"},
  {"compare-que",    4, "<dir_a> <dir_b>"},
  {"que-info",       3, "<dir>"},
  {"scan",           3, "<save_dir>"},
  {NULL, 0, NULL}
};

// Runs the named subcommand.  The caller has already validated argc against
// COMMANDS, so every argv index used here exists.
// returns the process exit code.
static int
run_command(const char *cmd, char **argv)
{
  if(strcmp(cmd, "dump") == 0)
    return(cmd_dump(argv[2]));

  if(strcmp(cmd, "count") == 0)
    return(cmd_count(argv[2]));

  if(strcmp(cmd, "search") == 0)
    return(cmd_search(argv[2], argv[3]));

  if(strcmp(cmd, "has") == 0)
    return(cmd_has(argv[2], argv[3]));

  if(strcmp(cmd, "acts") == 0)
    return(cmd_acts(argv[2]));

  if(strcmp(cmd, "quests") == 0)
    return(cmd_quests(argv[2]));

  if(strcmp(cmd, "add") == 0)
    return(cmd_add(argv[2], argv[3]));

  if(strcmp(cmd, "remove") == 0)
    return(cmd_remove(argv[2], argv[3]));

  if(strcmp(cmd, "complete") == 0)
    return(cmd_complete(argv[2], argv[3]));

  if(strcmp(cmd, "clear") == 0)
    return(cmd_clear(argv[2], argv[3]));

  if(strcmp(cmd, "roundtrip") == 0)
    return(cmd_roundtrip(argv[2]));

  if(strcmp(cmd, "defs") == 0)
    return(cmd_defs());

  if(strcmp(cmd, "diff") == 0)
    return(cmd_diff(argv[2], argv[3]));

  if(strcmp(cmd, "coverage") == 0)
    return(cmd_coverage(argv[2]));

  if(strcmp(cmd, "dump-que") == 0)
    return(cmd_dump_que(argv[2]));

  if(strcmp(cmd, "dump-quest-myw") == 0)
    return(cmd_dump_quest_myw(argv[2]));

  if(strcmp(cmd, "clear-que") == 0)
    return(cmd_clear_que(argv[2]));

  if(strcmp(cmd, "compare-que") == 0)
    return(cmd_compare_que(argv[2], argv[3]));

  if(strcmp(cmd, "que-info") == 0)
    return(cmd_que_info(argv[2]));

  if(strcmp(cmd, "scan") == 0)
    return(cmd_scan(argv[2]));

  // Reachable only if a COMMANDS row was added without a branch above.
  fprintf(stderr, "internal error: no handler for command '%s'\n", cmd);
  return(1);
}

// main -- entry point. Dispatches to subcommands based on argv[1].
// argc: argument count.
// argv: argument array.
// returns 0 on success, 1 on error.
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

  return(run_command(cmd, argv));
}
