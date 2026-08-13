// tq_limitrun -- run a command with a file-size limit, to test disk-full paths.
//
// Lowers the RLIMIT_FSIZE soft limit and ignores SIGXFSZ, then execs the given
// command: every write past the limit fails with EFBIG instead of killing the
// process, which is the ENOSPC path without needing a full filesystem. Both the
// rlimit and an ignored signal disposition survive execve, so the child
// inherits them.
//
// Used to verify that a save reports failure and leaves the original file
// byte-identical, e.g.:
//   tq-limitrun 0 ./build/tq-chr-tool add-skill copy.chr "records\...dbr" 4
//   tq-limitrun 0 ./build/tq-quest-tool add copy.myw sometoken
//
// POSIX only (no Windows rlimits). Note the limit applies to EVERY file the
// child writes, including a redirected stdout -- redirect to /dev/null or a
// pipe, not to a file, or the output is lost too.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static void
usage(const char *argv0)
{
  printf("Usage: %s <max-bytes> <command> [args...]\n\n", argv0);
  printf("Run <command> with RLIMIT_FSIZE soft-limited to <max-bytes> and\n"
         "SIGXFSZ ignored, so writes past the limit fail with EFBIG rather\n"
         "than killing the process -- a disk-full simulation that needs no\n"
         "full filesystem.  Only the soft limit is lowered (an unprivileged\n"
         "process cannot raise the hard limit back).\n\n");
  printf("The limit applies to every file the child writes, including a\n"
         "redirected stdout -- redirect to a pipe or /dev/null, not a file.\n\n");
  printf("Exit status is the command's own, or 2 if the setup or exec failed.\n\n");
  printf("Examples:\n");
  printf("  %s 0 ./build/tq-chr-tool add-skill copy.chr "
         "'records\\skills\\defensive\\battleawareness.dbr' 4\n", argv0);
  printf("  %s 0 ./build/tq-quest-tool add copy.myw zz_test\n", argv0);
}

int
main(int argc, char **argv)
{
  if(argc < 3 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
  {
    usage(argv[0]);
    return(argc < 3 ? 2 : 0);
  }

  char             *end = NULL;
  unsigned long long max;
  struct rlimit     rl;

  errno = 0;
  max = strtoull(argv[1], &end, 10);

  if(errno != 0 || end == argv[1] || *end != '\0')
  {
    fprintf(stderr, "%s: '%s' is not a byte count\n", argv[0], argv[1]);
    return(2);
  }

  if(getrlimit(RLIMIT_FSIZE, &rl) != 0)
  {
    perror("getrlimit");
    return(2);
  }

  rl.rlim_cur = (rlim_t)max;

  if(signal(SIGXFSZ, SIG_IGN) == SIG_ERR)
  {
    perror("signal");
    return(2);
  }

  if(setrlimit(RLIMIT_FSIZE, &rl) != 0)
  {
    perror("setrlimit");
    return(2);
  }

  execvp(argv[2], argv + 2);
  fprintf(stderr, "%s: cannot run %s: %s\n", argv[0], argv[2], strerror(errno));
  return(2);
}
