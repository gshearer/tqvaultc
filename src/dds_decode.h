#ifndef DDS_DECODE_H
#define DDS_DECODE_H

#include <stddef.h>
#include <stdint.h>

// Decode a DDS blob (starting at the "DDS " magic) into a freshly allocated
// RGBA8 pixel buffer. Caller frees with free().
//
// Supported pixel formats (sufficient for Titan Quest .tex assets):
//   - DXT1 / BC1   (RGB or RGB+1bit alpha)
//   - DXT3 / BC2   (RGBA, explicit 4-bit alpha)
//   - DXT5 / BC3   (RGBA, interpolated alpha)
//   - Uncompressed B8G8R8   (24bpp)
//   - Uncompressed B8G8R8A8 (32bpp)
//
// Only the top mip level is decoded; mip chains, cubemaps, volumes and the
// DX10 extended header are not supported.
//
// On success: returns RGBA pixels and writes width/height. On failure: NULL.
uint8_t *dds_decode(const uint8_t *data, size_t size,
                    uint32_t *out_width, uint32_t *out_height);

#endif
