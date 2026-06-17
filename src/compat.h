#ifndef COMPAT_H
#define COMPAT_H

// Small, GTK-free portability shims for GNU/POSIX string functions that mingw
// (and other non-glibc targets) lack.  Keep this header free of GTK/GLib so the
// CLI utilities under src/utils can include it without pulling in the whole UI
// layer (ui.h, which also includes this, used to carry these inline).

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if defined(__GLIBC__) || defined(_WIN32)
#include <malloc.h>   // malloc_trim (glibc) / _heapmin (Windows) for tq_heap_trim
#endif

// Return free heap pages to the OS after a bulk free.  glibc keeps freed small
// chunks in its arena and Windows keeps them committed, so without this an
// index-build clear drops *live* memory but not RSS / Windows commit charge —
// which is exactly what locked up low-RAM Windows on first run.  No-op on
// targets that expose neither call.
static inline void
tq_heap_trim(void)
{
#if defined(__GLIBC__)
  malloc_trim(0);
#elif defined(_WIN32)
  _heapmin();
#endif
}

// strcasestr is a GNU extension — provide a portable fallback for mingw
// and other non-glibc targets.
#ifndef HAVE_STRCASESTR
static inline char *
tqvc_strcasestr(const char *haystack, const char *needle)
{
  if(!*needle)
    return((char *)haystack);

  size_t nlen = strlen(needle);

  for(; *haystack; haystack++)
  {
    size_t i = 0;

    while(i < nlen
          && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
      i++;

    if(i == nlen)
      return((char *)haystack);
  }
  return(NULL);
}
#define strcasestr tqvc_strcasestr
#endif

#ifdef _WIN32
static inline char *
tqvc_strndup(const char *s, size_t n)
{
  size_t len = 0;

  while(len < n && s[len])
    len++;

  char *out = (char *)malloc(len + 1);

  if(!out)
    return(NULL);

  memcpy(out, s, len);
  out[len] = '\0';
  return(out);
}
#define strndup tqvc_strndup
#endif

#endif // COMPAT_H
