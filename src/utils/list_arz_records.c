#include "../arz.h"
#include <stdio.h>
#include <string.h>

// Print usage information for this utility.
// prog: the program name (argv[0]).
static void
usage(const char *prog)
{
  fprintf(stderr,
    "Usage: %s <database.arz>\n"
    "\n"
    "List all record paths in a .arz database file. Output is one\n"
    "record path per line, suitable for piping to grep or wc.\n"
    "\n"
    "Examples:\n"
    "  %s testdata/gamefiles/Database/database.arz\n"
    "  %s testdata/gamefiles/Database/database.arz | grep -i relic\n"
    "  %s testdata/gamefiles/Database/database.arz | wc -l\n",
    prog, prog, prog, prog);
}

// Entry point. Loads a .arz database file and prints all record paths,
// one per line.
// argc: argument count.
// argv: argument vector; argv[1] is the .arz file path.
// Returns 0 on success, 1 on error.
int
main(int argc, char **argv)
{
  if(argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
  {
    usage(argv[0]);
    return(argc < 2 ? 1 : 0);
  }

  TQArzFile *arz = arz_load(argv[1]);

  if(!arz)
  {
    printf("Failed to load ARZ: %s\n", argv[1]);
    return(1);
  }

  for(uint32_t i = 0; i < arz->num_records; i++)
  {
    if(arz->records[i].path)
      printf("%s\n", arz->records[i].path);
  }

  arz_free(arz);
  return(0);
}
