#ifndef IO_ATOMIC_H
#define IO_ATOMIC_H

// io_atomic -- crash-safe replacement of a user-data file.
//
// Every writer in this tree used to fopen(path, "wb") the live save: that
// truncates the real file to zero before a single new byte exists, so a failure
// mid-write destroys the user's data.  Worse, stdio buffers -- a small fwrite
// never touches the fd and returns full success, and the real error (ENOSPC,
// EIO) surfaces only at the flush inside fclose, which almost nobody checked.
//
// The functions here write to "<path>.tmp", check both fwrite AND fclose, and
// rename into place only when both succeeded.  On any failure the temp file is
// removed and the original file is left byte-identical.  Generalized from
// db_browser_cache_save(), which already did this correctly.
//
// GTK-free; needs only GLib (g_fopen/g_rename/g_unlink give us the Windows
// wide-char paths and a rename that replaces an existing destination).

#include <stdbool.h>
#include <stddef.h>

typedef struct TQAtomicFile TQAtomicFile;

// One-shot form: replace `path` with `size` bytes of `data`.  Returns false if
// the file was not fully written and renamed, in which case `path` is unchanged.
bool tq_write_file_atomic(const char *path, const void *data, size_t size);

// Streaming form, for writers that cannot hand over a single buffer.  Errors
// are sticky: after a failed write every later write is a no-op and the commit
// fails, so callers check once at the end instead of at every write.
// Returns NULL if the temp file could not be created.
TQAtomicFile *tq_atomic_open(const char *path);
bool tq_atomic_write(TQAtomicFile *af, const void *data, size_t size);

// Both consume the handle: after either call `af` is freed and unusable.
// commit() renames into place on success and removes the temp otherwise;
// abort() always discards.
bool tq_atomic_commit(TQAtomicFile *af);
void tq_atomic_abort(TQAtomicFile *af);

// Copy src_path over dst_path atomically (the .bak / .dxg safety nets).
bool tq_copy_file_atomic(const char *src_path, const char *dst_path);

#endif
