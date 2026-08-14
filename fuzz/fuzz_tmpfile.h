#ifndef FUZZ_TMPFILE_H
#define FUZZ_TMPFILE_H

#include <stddef.h>
#include <stdint.h>

// Stage `len` bytes of `data` in a temp file and return its path, or NULL if
// the file could not be written.  The path stays valid until the process
// exits, at which point the file is removed.
//
// One file is created per process and rewritten on every call: libFuzzer runs
// a harness hundreds of thousands of times, and creating and unlinking a file
// each iteration would cost more than the parser under test.  The returned
// path is therefore the same every call, and the previous iteration's
// contents are gone -- callers must not hold it across calls.
const char *
fuzz_tmpfile_write(const uint8_t *data, size_t len);

#endif
