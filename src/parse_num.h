#ifndef TQ_PARSE_NUM_H
#define TQ_PARSE_NUM_H

// parse_num -- checked string-to-number parsing, the atoi/atof replacement.

#include <stdbool.h>

// Both reject an empty string, leading garbage, trailing garbage and overflow,
// and leave *out untouched on failure.  Leading whitespace and a sign are
// accepted, matching strtol/strtod.
bool parse_int(const char *s, int *out);
bool parse_float(const char *s, float *out);

#endif
