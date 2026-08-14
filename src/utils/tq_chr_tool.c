// tq_chr_tool.c -- Player.chr debugging/troubleshooting CLI
//
// Independent binary parser for Titan Quest .chr files. Does NOT reuse
// character_load() for analysis commands -- has its own raw stream walker
// with a known-key table to avoid the heuristic bug (Bug 1 in TODO.md).
//
// Usage:
//   tq-chr-tool <command> [args...]
//
// Commands:
//   dump      <chr>                Raw key-value dump with offsets and types
//   inv       <chr>                Inventory listing: per-sack items
//   equip     <chr>                Equipment listing: 12 slots
//   compare   <chr_a> <chr_b>     Structural diff (flagship feature)
//   validate  <chr>                Structural integrity checks
//   hex       <chr> <section|offset> [len]   Hex dump of sections or offsets
//   roundtrip <chr>                Load via character_load(), save, compare

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <glib.h>


// Stubs -- character.c references these but we don't need them for analysis
#include "../character.h"

// vault_item_free_strings -- stub to free item string fields.
// item: pointer to TQVaultItem (may be NULL).
void
vault_item_free_strings(TQVaultItem *item)
{
  if(!item)
    return;

  free(item->base_name);
  free(item->prefix_name);
  free(item->suffix_name);
  free(item->relic_name);
  free(item->relic_bonus);
  free(item->relic_name2);
  free(item->relic_bonus2);
  free(item->stack_seeds);
  free(item->stack_var2);
}

bool tqvc_debug = false;


#include "tq_chr_tool.h"
#include "cli_args.h"

// ═══════════════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════════════

// usage -- print program usage to stderr.
// prog: program name (argv[0]).
static void
usage(const char *prog)
{
  fprintf(stderr,
    "Usage: %s <command> [args...]\n"
    "\n"
    "Player.chr debugging/troubleshooting tool.\n"
    "Independent binary parser -- does NOT reuse character_load() bugs.\n"
    "\n"
    "Commands:\n"
    "  dump      <chr>                      Raw key-value dump with offsets\n"
    "  inv       <chr>                      Inventory listing per sack\n"
    "  equip     <chr>                      Equipment listing (12 slots)\n"
    "  compare   <chr_a> <chr_b>            Structural diff\n"
    "  validate  <chr>                      Structural integrity checks\n"
    "  hex       <chr> <section|offset> [len]\n"
    "                                       Hex dump (sections: prefix,\n"
    "                                       inventory, middle, equipment,\n"
    "                                       suffix; or numeric offset)\n"
    "  roundtrip <chr>                      Load/save via character_load(),\n"
    "                                       compare output to input\n"
    "  add-skill <chr> <skillDBR> <level>   Splice a new skill record in,\n"
    "                                       reload, verify (writes in place!)\n"
    "\n"
    "Examples:\n"
    "  %s dump testdata/Player.chr | head -50\n"
    "  %s inv Player_working.chr\n"
    "  %s compare Player_working.chr Player_broken.chr\n"
    "  %s validate Player_working.chr\n"
    "  %s hex Player_working.chr equipment\n"
    "  %s hex Player_working.chr 0x1a00 128\n"
    "  %s roundtrip Player_working.chr\n",
    prog, prog, prog, prog, prog, prog, prog, prog);
}

// Every subcommand, with the argc it needs and the argument list its usage
// line prints.
static const CliCommand COMMANDS[] = {
  {"dump",      3, "<chr>"},
  {"inv",       3, "<chr>"},
  {"equip",     3, "<chr>"},
  {"compare",   4, "<chr_a> <chr_b>"},
  {"validate",  3, "<chr>"},
  {"hex",       4, "<chr> <section|offset> [len]"},
  {"roundtrip", 3, "<chr>"},
  {"add-skill", 5, "<chr> <skillDBR> <level>"},
  {NULL, 0, NULL}
};

// Runs the named subcommand.  The caller has already validated argc against
// COMMANDS, so every argv index used here exists.
// returns the process exit code.
static int
run_command(const char *cmd, int argc, char **argv)
{
  if(strcmp(cmd, "dump") == 0)
    return(cmd_dump(argv[2]));

  if(strcmp(cmd, "inv") == 0)
    return(cmd_inv(argv[2]));

  if(strcmp(cmd, "equip") == 0)
    return(cmd_equip(argv[2]));

  if(strcmp(cmd, "compare") == 0)
    return(cmd_compare(argv[2], argv[3]));

  if(strcmp(cmd, "validate") == 0)
    return(cmd_validate(argv[2]));

  if(strcmp(cmd, "hex") == 0)
    return(cmd_hex(argv[2], argv[3], argc > 4 ? argv[4] : NULL));

  if(strcmp(cmd, "roundtrip") == 0)
    return(cmd_roundtrip(argv[2]));

  if(strcmp(cmd, "add-skill") == 0)
    return(cmd_add_skill(argv[2], argv[3], (uint32_t)strtoul(argv[4], NULL, 10)));

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

  if(strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
  {
    usage(argv[0]);
    return(0);
  }

  const char *cmd = argv[1];
  CliArgsResult ck = cli_check_args(COMMANDS, argv[0], cmd, argc);

  if(ck == CLI_ARGS_TOO_FEW)
    return(1);

  if(ck == CLI_ARGS_UNKNOWN)
  {
    fprintf(stderr, "error: unknown command '%s'\n\n", cmd);
    usage(argv[0]);
    return(1);
  }

  return(run_command(cmd, argc, argv));
}
