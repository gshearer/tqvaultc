// parse_num -- checked string-to-number parsing, the atoi/atof replacement.
// See parse_num.h for the contract.

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "parse_num.h"

bool
parse_int(const char *s, int *out)
{
  if(!s || !out)
    return(false);

  char *end = NULL;
  long  v;

  errno = 0;
  v = strtol(s, &end, 10);

  if(end == s || *end || errno == ERANGE || v < INT_MIN || v > INT_MAX)
    return(false);

  *out = (int)v;

  return(true);
}

bool
parse_float(const char *s, float *out)
{
  if(!s || !out)
    return(false);

  char  *end = NULL;
  double v;

  errno = 0;
  v = strtod(s, &end);

  if(end == s || *end || errno == ERANGE)
    return(false);

  *out = (float)v;

  return(true);
}
