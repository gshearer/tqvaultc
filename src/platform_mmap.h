#ifndef PLATFORM_MMAP_H
#define PLATFORM_MMAP_H

#include <stddef.h>

/**
 * platform_mmap_readonly - Memory-map a file for reading.
 * Returns a pointer to the mapped region, or NULL on failure.
 * On success, *out_size is set to the file size.
 */
void *platform_mmap_readonly(const char *path, size_t *out_size);

/**
 * platform_munmap - Unmap a previously mapped region.
 */
void platform_munmap(void *addr, size_t size);

#endif
