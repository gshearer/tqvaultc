#ifndef PLATFORM_MMAP_H
#define PLATFORM_MMAP_H

#include <stddef.h>

// platform_mmap_readonly - memory-map a file for reading
// path: filesystem path to the file
// out_size: on success, set to the file size in bytes
// returns: pointer to mapped region, or NULL on failure
void *platform_mmap_readonly(const char *path, size_t *out_size);

// platform_munmap - unmap a previously mapped region
// addr: pointer returned by platform_mmap_readonly
// size: size of the mapped region
void platform_munmap(void *addr, size_t size);

#endif
