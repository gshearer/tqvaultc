#include "platform_mmap.h"

#ifdef _WIN32

#include <windows.h>

// platform_mmap_readonly - memory-map a file for reading (Windows)
// path: filesystem path to the file
// out_size: on success, set to the file size in bytes
// returns: pointer to mapped region, or NULL on failure
void *
platform_mmap_readonly(const char *path, size_t *out_size)
{
  if(!path || !out_size)
    return(NULL);

  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if(file == INVALID_HANDLE_VALUE)
    return(NULL);

  LARGE_INTEGER file_size;

  if(!GetFileSizeEx(file, &file_size))
  {
    CloseHandle(file);
    return(NULL);
  }

  HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);

  if(!mapping)
  {
    CloseHandle(file);
    return(NULL);
  }

  void *addr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);

  CloseHandle(mapping);
  CloseHandle(file);

  if(!addr)
    return(NULL);

  *out_size = (size_t)file_size.QuadPart;
  return(addr);
}

// platform_munmap - unmap a previously mapped region (Windows)
// addr: pointer returned by platform_mmap_readonly
// size: unused on Windows
void
platform_munmap(void *addr, size_t size)
{
  (void)size;
  if(addr)
    UnmapViewOfFile(addr);
}

#else // POSIX

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// platform_mmap_readonly - memory-map a file for reading (POSIX)
// path: filesystem path to the file
// out_size: on success, set to the file size in bytes
// returns: pointer to mapped region, or NULL on failure
void *
platform_mmap_readonly(const char *path, size_t *out_size)
{
  if(!path || !out_size)
    return(NULL);

  int fd = open(path, O_RDONLY);

  if(fd < 0)
    return(NULL);

  struct stat st;

  if(fstat(fd, &st) < 0)
  {
    close(fd);
    return(NULL);
  }

  void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  close(fd);

  if(addr == MAP_FAILED)
    return(NULL);

  *out_size = (size_t)st.st_size;
  return(addr);
}

// platform_munmap - unmap a previously mapped region (POSIX)
// addr: pointer returned by platform_mmap_readonly
// size: size of the mapped region
void
platform_munmap(void *addr, size_t size)
{
  if(addr)
    munmap(addr, size);
}

#endif
