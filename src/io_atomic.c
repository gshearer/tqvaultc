// io_atomic -- write to a temp file, verify fwrite AND fclose, rename into
// place.  See io_atomic.h for why every user-data write goes through here.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "io_atomic.h"

struct TQAtomicFile
{
  FILE *fp;
  char *path;
  char *tmp;
  bool failed;
};

// Free the handle only; the caller has already dealt with fp and the temp file.
static void
af_free(TQAtomicFile *af)
{
  g_free(af->tmp);
  g_free(af->path);
  free(af);
}

TQAtomicFile *
tq_atomic_open(const char *path)
{
  if(!path)
    return(NULL);

  TQAtomicFile *af = calloc(1, sizeof(*af));

  if(!af)
    return(NULL);

  af->path = g_strdup(path);
  af->tmp  = g_strconcat(path, ".tmp", NULL);
  af->fp   = g_fopen(af->tmp, "wb");

  if(!af->fp)
  {
    g_warning("io_atomic: cannot create %s: %s", af->tmp, g_strerror(errno));
    af_free(af);
    return(NULL);
  }

  return(af);
}

bool
tq_atomic_write(TQAtomicFile *af, const void *data, size_t size)
{
  if(!af || af->failed)
    return(false);

  if(size == 0)
    return(true);

  if(!data || fwrite(data, 1, size, af->fp) != size)
  {
    af->failed = true;
    return(false);
  }

  return(true);
}

bool
tq_atomic_commit(TQAtomicFile *af)
{
  if(!af)
    return(false);

  bool ok = !af->failed;

  // The buffered bytes only reach the fd here, so this is where a full disk
  // reports itself -- never skip the fclose return.
  if(fclose(af->fp) != 0)
    ok = false;

  if(ok)
    ok = (g_rename(af->tmp, af->path) == 0);

  if(!ok)
  {
    g_warning("io_atomic: %s not written (%s); original left untouched",
              af->path, g_strerror(errno));
    g_unlink(af->tmp);
  }

  af_free(af);
  return(ok);
}

void
tq_atomic_abort(TQAtomicFile *af)
{
  if(!af)
    return;

  fclose(af->fp);
  g_unlink(af->tmp);
  af_free(af);
}

bool
tq_write_file_atomic(const char *path, const void *data, size_t size)
{
  TQAtomicFile *af = tq_atomic_open(path);

  if(!af)
    return(false);

  // The sticky error means one check at the commit covers the write too.
  tq_atomic_write(af, data, size);
  return(tq_atomic_commit(af));
}

bool
tq_copy_file_atomic(const char *src_path, const char *dst_path)
{
  FILE *src = g_fopen(src_path, "rb");

  if(!src)
    return(false);

  TQAtomicFile *af = tq_atomic_open(dst_path);

  if(!af)
  {
    fclose(src);
    return(false);
  }

  char   buf[8192];
  size_t n;
  bool   ok = true;

  while((n = fread(buf, 1, sizeof(buf), src)) > 0)
  {
    if(!tq_atomic_write(af, buf, n))
    {
      ok = false;
      break;
    }
  }

  if(ferror(src))
    ok = false;

  fclose(src);

  if(!ok)
  {
    tq_atomic_abort(af);
    return(false);
  }

  return(tq_atomic_commit(af));
}
