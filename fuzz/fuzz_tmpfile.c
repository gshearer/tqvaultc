#include "fuzz_tmpfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_path[] = "/tmp/tqvc-fuzz-XXXXXX";
static int  g_fd = -1;

// atexit handler: drop the staging file.
static void
fuzz_tmpfile_cleanup(void)
{
  if(g_fd < 0)
    return;

  close(g_fd);
  unlink(g_path);
  g_fd = -1;
}

const char *
fuzz_tmpfile_write(const uint8_t *data, size_t len)
{
  if(g_fd < 0)
  {
    g_fd = mkstemp(g_path);

    if(g_fd < 0)
      return(NULL);

    // A failed registration only leaves one file in /tmp, so it is not worth
    // aborting the run over -- but it is not swallowed either.
    if(atexit(fuzz_tmpfile_cleanup) != 0)
      fprintf(stderr, "fuzz: atexit failed; %s will not be removed\n", g_path);
  }

  if(ftruncate(g_fd, 0) != 0)
    return(NULL);

  size_t off = 0;

  while(off < len)
  {
    ssize_t n = pwrite(g_fd, data + off, len - off, (off_t)off);

    if(n <= 0)
      return(NULL);

    off += (size_t)n;
  }

  return(g_path);
}
