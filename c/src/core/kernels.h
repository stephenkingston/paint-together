/*
 * Paint (C port) - the loops that decide how fast pictures are saved and opened, in assembly
 *
 * kernels_x86_64.S (Linux, Windows x64, Intel Macs) and kernels_arm64.S (Apple
 * silicon) hold them; kernels.c holds the same in C, for other CPUs (the 32-bit
 * Windows build) and for tests/test_kernels.c, which checks that both give the
 * same bytes as each other and as the stb code they replace. Everything else is
 * built for size (-Oz): these loops are what kept stb's PNG, zlib and JPEG code
 * built for speed.
 *
 * Each kernel is k_<name> (assembly where the Makefile builds it, PAINT_ASM, and
 * otherwise a name for the C) and k_<name>_c (the C).
 */
#ifndef CORE_KERNELS_H
#define CORE_KERNELS_H

#include <stddef.h>
#include <stdint.h>

/* ---- deflate: stb_image_write's compressor (stbi_zlib_compress), the same bytes ---- */

/* Each 3-byte hash keeps up to `cap` (2 x quality) earlier positions, oldest
 * first; the longest match among them wins (the latest of equals), unless the
 * next byte starts a longer one ("lazy matching"); fixed Huffman codes. */
#define K_ZHASH 16384
typedef struct
{
    const uint8_t *data;
    uint8_t *out, *end;         /* the next whole byte goes to out; the kernel stops with fewer than K_ROOM bytes left */
    uint64_t bits;              /* bits not written yet: the low nbits (fewer than 8 between calls) */
    uint32_t nbits;
    int32_t i, len;             /* the next byte to code, and how many there are */
    uint32_t cap;
    uint32_t *chains;           /* K_ZHASH x cap positions */
    uint16_t *counts;           /* positions in each chain (start at 0) */
} k_deflate_t;
#define K_ROOM 16

/* Codes data[i] onwards (the last 3 bytes as literals, as stb does) and then the
 * end of the block, and returns 1; the caller writes out the last bits. Returns 0
 * if it runs out of room first: give it more and call again. */
int k_deflate(k_deflate_t *s);
int k_deflate_c(k_deflate_t *s);

/* zlib's crc32() and adler32(): start from 0 and 1 */
uint32_t k_crc32(uint32_t crc, const uint8_t *p, size_t n);
uint32_t k_crc32_c(uint32_t crc, const uint8_t *p, size_t n);
uint32_t k_adler32(uint32_t adler, const uint8_t *p, size_t n);
uint32_t k_adler32_c(uint32_t adler, const uint8_t *p, size_t n);

/* ---- PNG rows, RGBA (4 bytes a pixel), n bytes ---- */

/* PNG filters 0-4 of a row against the row above (a row of zeros for the first),
 * pixels left of the row being zeros: stb_image_write's encoder. cost[f] is the
 * sum of |byte| (as signed) of the row filtered with f, which stb picks by. */
void k_png_costs(const uint8_t *row, const uint8_t *prev, size_t n, uint32_t cost[5]);
void k_png_costs_c(const uint8_t *row, const uint8_t *prev, size_t n, uint32_t cost[5]);
void k_png_filter(uint8_t *out, const uint8_t *row, const uint8_t *prev, size_t n, int filter);
void k_png_filter_c(uint8_t *out, const uint8_t *row, const uint8_t *prev, size_t n, int filter);

/* Undoes a filter, any bytes a pixel (bpp): stb_image's decoder (filter 5 is its
 * average on the first row, which has no row above; prior isn't read for 0, 1 and 5) */
void k_unfilter(uint8_t *cur, const uint8_t *raw, const uint8_t *prior, size_t bpp, size_t n, int filter);
void k_unfilter_c(uint8_t *cur, const uint8_t *raw, const uint8_t *prior, size_t bpp, size_t n, int filter);

/* ---- inflate: stb_image's Huffman block decoder (stbi__parse_huffman_block) ---- */

/* The state stb keeps (stbi__zbuf), as the kernel takes it. The kernel stops
 * short of the end of the input and the output, and hands back exactly the state
 * stb would have had there (bits and nbits are stb's code_buffer and num_bits),
 * so that stb's own C finishes the block with the same results and errors. */
typedef struct
{
    const uint8_t *in, *in_end;
    uint8_t *out, *out_start, *out_end;
    const void *lengths, *distances;    /* stb's Huffman tables (stbi__zhuffman), laid out as K_ZH_ below */
    uint32_t bits;
    int32_t nbits;
} k_inflate_t;

/* stbi__zhuffman: uint16 fast[512] at 0, uint16 firstcode[16], int maxcode[17], uint16 firstsymbol[16],
 * uint8 size[288], uint16 value[288] */
#define K_ZH_FIRSTCODE 1024
#define K_ZH_MAXCODE 1056
#define K_ZH_FIRSTSYMBOL 1124
#define K_ZH_SIZE 1156
#define K_ZH_VALUE 1444

#define K_INFLATE_MORE 0        /* stopped near the end of the input or output: stb carries on */
#define K_INFLATE_END 1         /* end of the block */
#define K_INFLATE_BAD_CODE -1   /* stb: "bad huffman code" */
#define K_INFLATE_BAD_DIST -2   /* stb: "bad dist" */
int k_inflate(k_inflate_t *s);
int k_inflate_c(k_inflate_t *s);

#ifndef PAINT_ASM
#define k_deflate k_deflate_c
#define k_crc32 k_crc32_c
#define k_adler32 k_adler32_c
#define k_png_costs k_png_costs_c
#define k_png_filter k_png_filter_c
#define k_unfilter k_unfilter_c
#define k_inflate k_inflate_c
#endif

#endif
