/*
 * Paint (C port) - where the vendored stb libraries are compiled (also stb_write.c and stb_font.c)
 *
 * third_party/stb: stb_image 2.30, stb_image_write 1.16, stb_truetype 1.26
 * (public domain / MIT). Only the decoders of the formats the Rust Paint
 * opens (through the image crate) are built: no TGA or PNM. PNGs are written
 * by img.c (the same bytes as stb_image_write), and stb_image's two loops that
 * open them run in the kernels (core/kernels.h), through two hooks added to
 * stb_image.h (marked "Paint").
 */
#include "core/kernels.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#if defined(__aarch64__) || defined(__arm64__)
#define STBI_NEON       /* (on x86-64 stb uses SSE2 by itself) */
#endif
#pragma GCC diagnostic ignored "-Wunused-function"   /* what the hooks replace */
#pragma GCC diagnostic ignored "-Wunused-variable"
static int zfast_block(void *zbuf);
#define STBI_ZFAST_BLOCK(a) zfast_block(a)
#define STBI_PNG_UNFILTER(cur, raw, prior, bpp, n, filter) k_unfilter(cur, raw, prior, (size_t)(bpp), n, filter)
#include "stb_image.h"

_Static_assert(offsetof(stbi__zhuffman, firstcode) == K_ZH_FIRSTCODE && offsetof(stbi__zhuffman, maxcode) == K_ZH_MAXCODE
                   && offsetof(stbi__zhuffman, firstsymbol) == K_ZH_FIRSTSYMBOL && offsetof(stbi__zhuffman, size) == K_ZH_SIZE
                   && offsetof(stbi__zhuffman, value) == K_ZH_VALUE,
               "stbi__zhuffman");

/* stbi__parse_huffman_block's start: 1 at the end of the block, 0 (stb's error set) if it's bad,
 * 2 to let stb finish it */
static int zfast_block(void *zbuf)
{
    stbi__zbuf *a = zbuf;
    k_inflate_t s = { a->zbuffer, a->zbuffer_end, (uint8_t *)a->zout, (uint8_t *)a->zout_start, (uint8_t *)a->zout_end,
                      &a->z_length, &a->z_distance, a->code_buffer, a->num_bits };
    int r = k_inflate(&s);
    a->zbuffer = (stbi_uc *)s.in;
    a->zout = (char *)s.out;
    a->code_buffer = s.bits;
    a->num_bits = s.nbits;
    if (r == K_INFLATE_BAD_CODE)
        return stbi__err("bad huffman code", "Corrupt PNG");
    if (r == K_INFLATE_BAD_DIST)
        return stbi__err("bad dist", "Corrupt PNG");
    return r == K_INFLATE_END ? 1 : 2;
}
