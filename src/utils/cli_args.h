// Subcommand argument checking shared by tq-dbr-tool, tq-chr-tool and
// tq-quest-tool: one table per tool, one lookup that prints the usage line.

#ifndef CLI_ARGS_H
#define CLI_ARGS_H

#include <stddef.h>

// One subcommand.  min_argc counts argv[0] and the command name, so a command
// taking two operands needs 4.  args is the operand list its usage line prints
// after the command name.  Terminate the table with a NULL name.
typedef struct
{
  const char *name;
  int min_argc;
  const char *args;
} CliCommand;

// Outcome of matching argv[1] against a command table.  A two-state test of
// this separates CLI_ARGS_OK from the rest but cannot tell the two failures
// apart, and they leave the caller different work: CLI_ARGS_TOO_FEW has
// already printed its usage line, while CLI_ARGS_UNKNOWN has printed nothing
// and the caller still owes an unknown-command message and the full usage.
typedef enum
{
  CLI_ARGS_OK = 0,
  CLI_ARGS_TOO_FEW,
  CLI_ARGS_UNKNOWN
} CliArgsResult;

// Looks cmd up in table and checks argc against that command's minimum,
// printing "Usage: <prog> <cmd> <args>" to stderr when too few were given.
CliArgsResult cli_check_args(const CliCommand *table, const char *prog,
                             const char *cmd, int argc);

#endif
