#include "dds_decode.h"
#include <stdlib.h>
#include <string.h>

// DDS spec: https://learn.microsoft.com/windows/win32/direct3ddds/dx-graphics-dds-pguide
//
// We only consume the legacy 124-byte DDS_HEADER plus its DDS_PIXELFORMAT.
// Layout (offsets from start of magic):
//   0   "DDS "
//   4   dwSize (=124)
//   8   dwFlags
//   12  dwHeight
//   16  dwWidth
//   ...
//   76  ddspf.dwSize (=32)
//   80  ddspf.dwFlags
//   84  ddspf.dwFourCC
//   88  ddspf.dwRGBBitCount
//   92  ddspf.dwRBitMask
//   96  ddspf.dwGBitMask
//   100 ddspf.dwBBitMask
//   104 ddspf.dwABitMask

#define DDPF_ALPHAPIXELS 0x1
#define DDPF_FOURCC      0x4
#define DDPF_RGB         0x40

static uint32_t
read_le32(const uint8_t *p)
{
  return((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Decode a single BC1 block (8 bytes) into a 4x4 RGBA tile.
// out_stride is the row stride in pixels of the destination image.
static void
decode_bc1_block(const uint8_t *block, uint8_t *out, int x, int y,
                 int width, int height, int out_stride, int bc1_alpha_punchthrough)
{
  uint16_t c0 = (uint16_t)(block[0] | (block[1] << 8));
  uint16_t c1 = (uint16_t)(block[2] | (block[3] << 8));

  uint8_t palette[4][4];

  // Expand RGB565 -> RGB888 with bit replication
  palette[0][0] = (uint8_t)(((c0 >> 11) & 0x1F) * 255 / 31);
  palette[0][1] = (uint8_t)(((c0 >> 5) & 0x3F) * 255 / 63);
  palette[0][2] = (uint8_t)((c0 & 0x1F) * 255 / 31);
  palette[0][3] = 255;

  palette[1][0] = (uint8_t)(((c1 >> 11) & 0x1F) * 255 / 31);
  palette[1][1] = (uint8_t)(((c1 >> 5) & 0x3F) * 255 / 63);
  palette[1][2] = (uint8_t)((c1 & 0x1F) * 255 / 31);
  palette[1][3] = 255;

  if(c0 > c1)
  {
    // 4-color mode (no alpha)
    palette[2][0] = (uint8_t)((2 * palette[0][0] + palette[1][0] + 1) / 3);
    palette[2][1] = (uint8_t)((2 * palette[0][1] + palette[1][1] + 1) / 3);
    palette[2][2] = (uint8_t)((2 * palette[0][2] + palette[1][2] + 1) / 3);
    palette[2][3] = 255;

    palette[3][0] = (uint8_t)((palette[0][0] + 2 * palette[1][0] + 1) / 3);
    palette[3][1] = (uint8_t)((palette[0][1] + 2 * palette[1][1] + 1) / 3);
    palette[3][2] = (uint8_t)((palette[0][2] + 2 * palette[1][2] + 1) / 3);
    palette[3][3] = 255;
  }
  else
  {
    // 3-color mode + transparent black (only honored in BC1, not BC2/3)
    palette[2][0] = (uint8_t)((palette[0][0] + palette[1][0]) / 2);
    palette[2][1] = (uint8_t)((palette[0][1] + palette[1][1]) / 2);
    palette[2][2] = (uint8_t)((palette[0][2] + palette[1][2]) / 2);
    palette[2][3] = 255;

    palette[3][0] = 0;
    palette[3][1] = 0;
    palette[3][2] = 0;
    palette[3][3] = (uint8_t)(bc1_alpha_punchthrough ? 0 : 255);
  }

  uint32_t indices = read_le32(block + 4);

  for(int by = 0; by < 4; by++)
  {
    for(int bx = 0; bx < 4; bx++)
    {
      int px = x + bx;
      int py = y + by;

      if(px >= width || py >= height)
        continue;

      int idx = (indices >> (2 * (4 * by + bx))) & 0x3;
      uint8_t *p = out + (py * out_stride + px) * 4;
      p[0] = palette[idx][0];
      p[1] = palette[idx][1];
      p[2] = palette[idx][2];
      p[3] = palette[idx][3];
    }
  }
}

// Decode the BC2 explicit alpha (8 bytes). Writes alpha into 4x4 tile.
static void
decode_bc2_alpha(const uint8_t *alpha_block, uint8_t *out, int x, int y,
                 int width, int height, int out_stride)
{
  for(int by = 0; by < 4; by++)
  {
    uint16_t row = (uint16_t)(alpha_block[2 * by] | (alpha_block[2 * by + 1] << 8));

    for(int bx = 0; bx < 4; bx++)
    {
      int px = x + bx;
      int py = y + by;

      if(px >= width || py >= height)
        continue;

      uint8_t a4 = (uint8_t)((row >> (4 * bx)) & 0xF);
      uint8_t *p = out + (py * out_stride + px) * 4;
      p[3] = (uint8_t)(a4 * 17);  // 4-bit -> 8-bit (a*255/15)
    }
  }
}

// Decode the BC3-style interpolated alpha (8 bytes). Used for BC3 alpha.
static void
decode_bc3_alpha(const uint8_t *alpha_block, uint8_t *out, int x, int y,
                 int width, int height, int out_stride)
{
  uint8_t a0 = alpha_block[0];
  uint8_t a1 = alpha_block[1];
  uint8_t alpha[8];

  alpha[0] = a0;
  alpha[1] = a1;

  if(a0 > a1)
  {
    for(int i = 1; i <= 6; i++)
      alpha[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1 + 3) / 7);
  }
  else
  {
    for(int i = 1; i <= 4; i++)
      alpha[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1 + 2) / 5);
    alpha[6] = 0;
    alpha[7] = 255;
  }

  // 16 3-bit indices packed into 6 bytes
  uint64_t bits = 0;

  for(int i = 0; i < 6; i++)
    bits |= ((uint64_t)alpha_block[2 + i]) << (8 * i);

  for(int i = 0; i < 16; i++)
  {
    int bx = i % 4;
    int by = i / 4;
    int px = x + bx;
    int py = y + by;

    if(px >= width || py >= height)
      continue;

    int idx = (int)((bits >> (3 * i)) & 0x7);
    uint8_t *p = out + (py * out_stride + px) * 4;
    p[3] = alpha[idx];
  }
}

static uint8_t *
decode_bcn(const uint8_t *blocks, size_t blocks_size, int width, int height, int variant)
{
  // variant: 1=BC1, 2=BC2, 3=BC3
  int block_bytes = (variant == 1) ? 8 : 16;
  int blocks_w = (width + 3) / 4;
  int blocks_h = (height + 3) / 4;

  if(blocks_size < (size_t)(blocks_w * blocks_h * block_bytes))
    return(NULL);

  uint8_t *out = calloc((size_t)width * (size_t)height, 4);

  if(!out)
    return(NULL);

  for(int by = 0; by < blocks_h; by++)
  {
    for(int bx = 0; bx < blocks_w; bx++)
    {
      const uint8_t *blk = blocks + ((size_t)by * blocks_w + bx) * block_bytes;
      int x = bx * 4;
      int y = by * 4;

      if(variant == 1)
      {
        decode_bc1_block(blk, out, x, y, width, height, width, 1);
      }
      else if(variant == 2)
      {
        // BC2: 8 bytes alpha, then 8 bytes BC1-style color (color always 4-color mode)
        decode_bc1_block(blk + 8, out, x, y, width, height, width, 0);
        decode_bc2_alpha(blk, out, x, y, width, height, width);
      }
      else  // BC3
      {
        decode_bc1_block(blk + 8, out, x, y, width, height, width, 0);
        decode_bc3_alpha(blk, out, x, y, width, height, width);
      }
    }
  }
  return(out);
}

// Decode uncompressed BGR(A) -> RGBA. bpp is 24 or 32. Source is row-packed.
static uint8_t *
decode_uncompressed(const uint8_t *src, size_t src_size, int width, int height, int bpp)
{
  size_t bytes_per_pixel = (size_t)bpp / 8;
  size_t need = (size_t)width * (size_t)height * bytes_per_pixel;

  if(src_size < need)
    return(NULL);

  uint8_t *out = malloc((size_t)width * (size_t)height * 4);

  if(!out)
    return(NULL);

  size_t n = (size_t)width * (size_t)height;

  if(bpp == 24)
  {
    for(size_t i = 0; i < n; i++)
    {
      out[i * 4 + 0] = src[i * 3 + 2];  // R
      out[i * 4 + 1] = src[i * 3 + 1];  // G
      out[i * 4 + 2] = src[i * 3 + 0];  // B
      out[i * 4 + 3] = 255;
    }
  }
  else  // 32
  {
    for(size_t i = 0; i < n; i++)
    {
      out[i * 4 + 0] = src[i * 4 + 2];  // R
      out[i * 4 + 1] = src[i * 4 + 1];  // G
      out[i * 4 + 2] = src[i * 4 + 0];  // B
      out[i * 4 + 3] = src[i * 4 + 3];  // A
    }
  }
  return(out);
}

uint8_t *
dds_decode(const uint8_t *data, size_t size, uint32_t *out_width, uint32_t *out_height)
{
  if(size < 128)
    return(NULL);

  if(memcmp(data, "DDS ", 4) != 0)
    return(NULL);

  uint32_t hdr_size = read_le32(data + 4);

  if(hdr_size != 124)
    return(NULL);

  uint32_t height = read_le32(data + 12);
  uint32_t width = read_le32(data + 16);

  if(width == 0 || height == 0 || width > 16384 || height > 16384)
    return(NULL);

  uint32_t pf_size = read_le32(data + 76);

  if(pf_size != 32)
    return(NULL);

  uint32_t pf_flags = read_le32(data + 80);
  uint32_t fourcc = read_le32(data + 84);
  uint32_t bit_count = read_le32(data + 88);

  const uint8_t *payload = data + 128;
  size_t payload_size = size - 128;

  uint8_t *pixels = NULL;

  if(pf_flags & DDPF_FOURCC)
  {
    // 'D'|'X'|'T'|'n' little-endian
    uint32_t dxt1 = (uint32_t)('D' | ('X' << 8) | ('T' << 16) | ('1' << 24));
    uint32_t dxt3 = (uint32_t)('D' | ('X' << 8) | ('T' << 16) | ('3' << 24));
    uint32_t dxt5 = (uint32_t)('D' | ('X' << 8) | ('T' << 16) | ('5' << 24));

    if(fourcc == dxt1)
      pixels = decode_bcn(payload, payload_size, (int)width, (int)height, 1);
    else if(fourcc == dxt3)
      pixels = decode_bcn(payload, payload_size, (int)width, (int)height, 2);
    else if(fourcc == dxt5)
      pixels = decode_bcn(payload, payload_size, (int)width, (int)height, 3);
    else
      return(NULL);
  }
  else if(pf_flags & DDPF_RGB)
  {
    if(bit_count != 24 && bit_count != 32)
      return(NULL);

    pixels = decode_uncompressed(payload, payload_size, (int)width, (int)height, (int)bit_count);
  }
  else
  {
    return(NULL);
  }

  if(!pixels)
    return(NULL);

  *out_width = width;
  *out_height = height;
  return(pixels);
}
