// Subcommand argument checking shared by the three CLI tools.

#include "cli_args.h"

#include <stdio.h>
#include <string.h>

CliArgsResult
cli_check_args(const CliCommand *table, const char *prog, const char *cmd,
               int argc)
{
  const CliCommand *c;

  for(c = table; c->name; c++)
  {
    if(strcmp(c->name, cmd) != 0)
      continue;

    if(argc >= c->min_argc)
      return(CLI_ARGS_OK);

    fprintf(stderr, "Usage: %s %s %s\n", prog, cmd, c->args);
    return(CLI_ARGS_TOO_FEW);
  }

  return(CLI_ARGS_UNKNOWN);
}
