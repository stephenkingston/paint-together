/*
 * Paint (C port) - tests of the kernels (core/kernels.h)
 *
 * The kernels replace stb code, so the stb code as it came is built in here too
 * (static, the public names renamed ref_*), and everything is checked against it:
 * the same PNG and zlib bytes written, and the same pixels, bytes and errors read
 * back, also from damaged streams. Each kernel's assembly is also checked against
 * its C, on random input.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/img.h"
#include "core/kernels.h"

/* ---- stb as it came ---- */

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#define stbi_load_from_memory ref_load_from_memory
#define stbi_image_free ref_image_free
#define stbi_failure_reason ref_failure_reason
#define stbi_zlib_decode_buffer ref_zlib_decode_buffer
#define stbi_zlib_decode_noheader_buffer ref_zlib_decode_noheader_buffer
#define stbi_zlib_decode_malloc ref_zlib_decode_malloc
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD        /* stb's own C everywhere: the app's build uses SIMD for JPEG */
#include "stb_image.h"
#undef stbi_load_from_memory
#undef stbi_image_free
#undef stbi_failure_reason
#undef stbi_zlib_decode_buffer
#undef stbi_zlib_decode_noheader_buffer
#undef stbi_zlib_decode_malloc
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* The app's stb (src/core/stb_impl.c), with the kernels in it */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *p);
const char *stbi_failure_reason(void);
int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);
char *stbi_zlib_decode_malloc(const char *buffer, int len, int *outlen);

static int failures;
#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            printf("FAIL %s:%d: %s: ", __func__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static uint32_t seed = 12345;
static uint32_t rnd(void)
{
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return seed;
}
static uint32_t below(uint32_t n)
{
    return n ? rnd() % n : 0;
}

/* ---- inputs ---- */

/* Bytes of many kinds: what deflate finds matches in, and what it doesn't */
static uint8_t *make_data(int kind, size_t n)
{
    uint8_t *d = malloc(n + 1);
    static const char *words[] = { "paint ", "brush ", "colour ", "the ", "of ", "picture ", "\n", "fill ", "a ", "and " };
    size_t i = 0;
    switch (kind)
    {
    case 0: /* random: stb stores it instead */
        for (; i < n; i++)
            d[i] = (uint8_t)rnd();
        break;
    case 1: /* one byte over and over: matches of 258 */
        memset(d, 7, n);
        break;
    case 2: /* words */
        while (i < n)
        {
            const char *w = words[below(10)];
            for (; *w && i < n; w++)
                d[i++] = (uint8_t)*w;
        }
        break;
    case 3: /* two symbols: every hash chain full */
        for (; i < n; i++)
            d[i] = (uint8_t)(rnd() & 1);
        break;
    case 4: /* a random block repeated at a distance around the window's edge */
    {
        size_t period = 32766 + below(4);
        for (; i < n; i++)
            d[i] = i < period ? (uint8_t)rnd() : d[i - period];
        break;
    }
    case 5: /* runs and noise */
        while (i < n)
        {
            size_t run = 1 + below(300);
            uint8_t v = (uint8_t)rnd();
            for (size_t k = 0; k < run && i < n; k++)
                d[i++] = below(16) ? v : (uint8_t)rnd();
        }
        break;
    default: /* a smooth picture's rows */
        for (; i < n; i++)
            d[i] = (uint8_t)(i % 4 == 3 ? 255 : (i / 4) % 997 / 4 + (i % 4) * 30 + below(3));
        break;
    }
    return d;
}

static img *make_picture(int kind, int w, int h)
{
    img *im = img_new(w, h, (rgba){ 0, 0, 0, 0 });
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            uint8_t *p = img_at(im, x, y);
            switch (kind)
            {
            case 0:
                p[0] = (uint8_t)rnd(), p[1] = (uint8_t)rnd(), p[2] = (uint8_t)rnd(), p[3] = (uint8_t)rnd();
                break;
            case 1:
                p[0] = (uint8_t)(x * 3), p[1] = (uint8_t)(y * 5), p[2] = (uint8_t)(x ^ y), p[3] = x < w / 3 ? 0 : 255;
                break;
            case 2:
                p[0] = p[1] = p[2] = (x / 9 + y / 7) % 3 ? 255 : 0, p[3] = 255;
                break;
            default:
                p[0] = (uint8_t)(128 + x / 2 + below(5)), p[1] = (uint8_t)(64 + y / 3 + below(5)), p[2] = (uint8_t)((x + y) / 5);
                p[3] = 255;
                break;
            }
        }
    return im;
}

/* ---- deflate and PNG writing ---- */

static void deflate_matches_stb(void)
{
    static const size_t sizes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 31, 258, 259, 300, 1000, 4096, 40000, 100000, 300000 };
    for (int kind = 0; kind < 7; kind++)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
        {
            size_t n = sizes[s];
            uint8_t *d = make_data(kind, n);
            int rl = 0;
            uint8_t *ref = stbi_zlib_compress(d, (int)n, &rl, 8);
            size_t zl = 0;
            uint8_t *z = zip_deflate(d, n, &zl);
            if (n == 0)
                CHECK(z && zl == 2 && z[0] == 3 && z[1] == 0, "nothing: one empty block");
            else
                CHECK(z && ref && zl == (size_t)rl - 6 && !memcmp(z, ref + 2, zl), "kind %d, %u bytes: %u bytes, stb %d", kind,
                      (unsigned)n, (unsigned)zl, rl - 6);
            free(z);
            STBIW_FREE(ref);
            free(d);
        }
}

static void deflate_kernel_matches_its_c(void)
{
    for (int round = 0; round < 60; round++)
    {
        size_t n = 1 + below(round < 30 ? 2000 : 200000);
        uint8_t *d = make_data((int)below(7), n);
        k_deflate_t s[2];
        uint8_t *out[2];
        uint32_t cap = 2 * (5 + below(8));
        size_t room = n * 2 + 64, step = 16 + below(64);    /* a little room at a time: it stops and goes on everywhere */
        for (int k = 0; k < 2; k++)
        {
            out[k] = malloc(room);
            s[k] = (k_deflate_t){ .data = d, .len = (int32_t)n, .bits = 3, .nbits = 3, .cap = cap };
            s[k].chains = malloc(sizeof(uint32_t) * K_ZHASH * s[k].cap);
            s[k].counts = calloc(K_ZHASH, 2);
            s[k].out = out[k];
            s[k].end = out[k] + step;
            while (!(k ? k_deflate_c : k_deflate)(&s[k]))
                s[k].end = s[k].end + step < out[k] + room ? s[k].end + step : out[k] + room;
        }
        size_t l0 = (size_t)(s[0].out - out[0]), l1 = (size_t)(s[1].out - out[1]);
        CHECK(l0 == l1 && !memcmp(out[0], out[1], l0) && s[0].nbits == s[1].nbits && s[0].bits == s[1].bits && s[0].i == s[1].i,
              "%u bytes: %u vs %u", (unsigned)n, (unsigned)l0, (unsigned)l1);
        for (int k = 0; k < 2; k++)
            free(out[k]), free(s[k].chains), free(s[k].counts);
        free(d);
    }
}

static void png_matches_stb(void)
{
    static const int sizes[][2] = { { 1, 1 }, { 1, 9 }, { 9, 1 }, { 3, 3 }, { 64, 64 }, { 257, 13 }, { 500, 300 } };
    for (int kind = 0; kind < 4; kind++)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
        {
            img *im = make_picture(kind, sizes[s][0], sizes[s][1]);
            int rl = 0;
            uint8_t *ref = stbi_write_png_to_mem(im->px, im->w * 4, im->w, im->h, 4, &rl);
            size_t n = 0;
            uint8_t *png = img_png(im, &n);
            CHECK(png && ref && n == (size_t)rl && !memcmp(png, ref, n), "kind %d, %dx%d: %u bytes, stb %d", kind, im->w, im->h, (unsigned)n,
                  rl);
            free(png);
            STBIW_FREE(ref);
            img_free(im);
        }
}

static void png_rows_match_stb(void)
{
    for (int round = 0; round < 300; round++)
    {
        size_t n = 4 * (1 + below(round < 200 ? 40 : 3000));
        uint8_t *rows = malloc(2 * n), *line = malloc(n), *out = malloc(n), *out_c = malloc(n), *zero = calloc(n, 1);
        int smooth = (int)below(2);
        for (size_t i = 0; i < 2 * n; i++)
            rows[i] = smooth ? (uint8_t)(i % n / 4 + below(4) + (i % 4) * 50) : (uint8_t)rnd();
        for (int y = 0; y < 2; y++)
        {
            const uint8_t *row = rows + n * y, *prev = y ? rows : zero;
            uint32_t cost[5], cost_c[5];
            k_png_costs(row, prev, n, cost);
            k_png_costs_c(row, prev, n, cost_c);
            for (int f = 0; f < 5; f++)
            {
                stbiw__encode_png_line(rows, (int)n, (int)n / 4, 2, y, 4, f, (signed char *)line);
                uint32_t est = 0;
                for (size_t i = 0; i < n; i++)
                    est += (uint32_t)abs((signed char)line[i]);
                k_png_filter(out, row, prev, n, f);
                k_png_filter_c(out_c, row, prev, n, f);
                CHECK(!memcmp(out, line, n) && !memcmp(out_c, line, n), "row %d, filter %d, %u bytes", y, f, (unsigned)n);
                CHECK(cost[f] == est && cost_c[f] == est, "row %d, filter %d: cost %u, C %u, stb %u", y, f, cost[f], cost_c[f], est);
            }
        }
        free(rows), free(line), free(out), free(out_c), free(zero);
    }
}

static void checksums(void)
{
    const uint8_t *nine = (const uint8_t *)"123456789";
    CHECK(k_crc32(0, nine, 9) == 0xCBF43926u && k_crc32_c(0, nine, 9) == 0xCBF43926u, "crc32 of 123456789: %08x",
          k_crc32(0, nine, 9));
    CHECK(k_adler32(1, (const uint8_t *)"Wikipedia", 9) == 0x11E60398u, "adler32 of Wikipedia: %08x",
          k_adler32(1, (const uint8_t *)"Wikipedia", 9));
    for (int round = 0; round < 200; round++)
    {
        size_t n = below(round < 150 ? 100 : 300000), at = below(16);
        uint8_t *d = make_data((int)below(7), n + at);
        uint32_t c = rnd(), a = rnd() % 65521 | (rnd() % 65521) << 16;
        CHECK(k_crc32(c, d + at, n) == k_crc32_c(c, d + at, n), "crc32, %u bytes", (unsigned)n);
        CHECK(k_adler32(a, d + at, n) == k_adler32_c(a, d + at, n), "adler32, %u bytes", (unsigned)n);
        CHECK(k_crc32(0, d + at, n) == stbiw__crc32(d + at, (int)n), "crc32 against stb, %u bytes", (unsigned)n);
        free(d);
    }
    uint8_t *d = make_data(1, 6000000);     /* adler32's sums near their limits */
    memset(d, 255, 6000000);
    CHECK(k_adler32(1, d, 6000000) == k_adler32_c(1, d, 6000000), "adler32 of 6 MB of 255");
    free(d);
}

/* ---- streams to read: deflate blocks of every kind, made here ---- */

typedef struct
{
    uint8_t *p;
    size_t n;
    uint64_t bits;
    int nb;
} bw;

static void bput(bw *w, uint32_t v, int n)
{
    w->bits |= (uint64_t)v << w->nb, w->nb += n;
    while (w->nb >= 8)
        w->p[w->n++] = (uint8_t)w->bits, w->bits >>= 8, w->nb -= 8;
}

static uint32_t brev(uint32_t v, int n)
{
    uint32_t r = 0;
    while (n--)
        r = r << 1 | (v & 1), v >>= 1;
    return r;
}

/* A complete prefix code of n symbols, random code lengths up to 15 */
static void random_lengths(uint8_t *len, int n)
{
    int depth[320] = { 0 }, leaves = 1;
    while (leaves < n)
    {
        int k = (int)below((uint32_t)leaves);
        if (depth[k] >= 15)
            continue;
        depth[k]++;
        depth[leaves++] = depth[k];
    }
    for (int i = 0; i < n; i++)
    {
        int j = (int)below((uint32_t)(i + 1)), t = depth[i];
        depth[i] = depth[j], depth[j] = t;
    }
    for (int i = 0; i < n; i++)
        len[i] = (uint8_t)depth[i];
}

static void canonical(const uint8_t *len, int n, uint32_t *code)
{
    int count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    for (int b = 1, c = 0; b < 16; b++)
        c = (c + count[b - 1]) << 1, next[b] = c;
    for (int i = 0; i < n; i++)
        if (len[i])
            code[i] = brev((uint32_t)next[len[i]]++, len[i]);
}

/* A block of `count` random symbols (matches reach only as far back as there's output); kind 0
 * stored, 1 fixed codes, 2 random codes */
static void put_block(bw *w, int kind, int last, size_t *made, int count)
{
    bput(w, (uint32_t)last, 1);
    bput(w, (uint32_t)kind, 2);
    if (kind == 0)
    {
        if (w->nb)
            bput(w, 0, 8 - w->nb);
        uint32_t n = below(2000);
        bput(w, n, 16), bput(w, n ^ 0xFFFF, 16);
        for (uint32_t i = 0; i < n; i++)
            bput(w, rnd() & 255, 8);
        *made += n;
        return;
    }
    uint8_t len[320];
    uint32_t code[320];
    if (kind == 1)
    {
        for (int i = 0; i < 288; i++)
            len[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        for (int i = 0; i < 30; i++)
            len[288 + i] = 5;
    }
    else
    {
        random_lengths(len, 286);
        len[286] = len[287] = 0;
        random_lengths(len + 288, 30);
        bput(w, 29, 5), bput(w, 29, 5), bput(w, 15, 4);    /* 286 and 30 codes, 19 code-length codes */
        for (int i = 0; i < 19; i++)
            bput(w, 5, 3);
        /* code lengths 0-18 have 5-bit codes: the ith is brev(i, 5) */
        for (int i = 0; i < 286; i++)
            bput(w, brev(len[i], 5), 5);
        for (int i = 0; i < 30; i++)
            bput(w, brev(len[288 + i], 5), 5);
    }
    canonical(len, 288, code);
    canonical(len + 288, 30, code + 288);
    static const int lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const int lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    for (int s = 0; s < count; s++)
    {
        if (*made == 0 || below(3) == 0)
        {
            int c = (int)below(256);
            bput(w, code[c], len[c]);
            *made += 1;
            continue;
        }
        int l = (int)below(29), d;
        bput(w, code[257 + l], len[257 + l]);
        if (lext[l])
            bput(w, below(1u << lext[l]), lext[l]);
        size_t dist = 1 + below((uint32_t)(*made < 32768 ? *made : 32768));
        size_t dm = dist - 1;
        int e = 0;
        if (dm < 4)
            d = (int)dm;
        else
        {
            int b = 31 - __builtin_clz((uint32_t)dm);
            e = b - 1, d = 2 * b + (int)((dm >> e) & 1);
        }
        bput(w, code[288 + d], len[288 + d]);
        if (e)
            bput(w, (uint32_t)(dm & ((1u << e) - 1)), e);
        *made += (size_t)(lbase[l] + 0);    /* (roughly: the extra bits add a little) */
    }
    bput(w, code[256], len[256]);
}

/* A raw deflate stream of a few blocks of random kinds */
static uint8_t *make_stream(size_t *n)
{
    bw w = { malloc(4 << 20), 0, 0, 0 };
    size_t made = 0;
    int blocks = 1 + (int)below(4);
    for (int b = 0; b < blocks; b++)
        put_block(&w, (int)below(3), b == blocks - 1, &made, (int)below(4000));
    if (w.nb)
        bput(&w, 0, 8 - w.nb);
    *n = w.n;
    return w.p;
}

/* The app's stb and stb as it came read the same stream the same way: the same
 * bytes, or the same error */
static int same_reading(const uint8_t *z, size_t n, size_t cap, int header)
{
    char *a = malloc(cap + 1), *b = malloc(cap + 1);
    int na = header ? stbi_zlib_decode_buffer(a, (int)cap, (const char *)z, (int)n)
                    : stbi_zlib_decode_noheader_buffer(a, (int)cap, (const char *)z, (int)n);
    const char *wa = na < 0 ? stbi_failure_reason() : "";
    int nb = header ? ref_zlib_decode_buffer(b, (int)cap, (const char *)z, (int)n)
                    : ref_zlib_decode_noheader_buffer(b, (int)cap, (const char *)z, (int)n);
    const char *wb = nb < 0 ? ref_failure_reason() : "";
    int same = na == nb && (na < 0 ? !strcmp(wa, wb) : !memcmp(a, b, (size_t)na));
    if (!same)
        printf("  read %d (%s), stb %d (%s), %u bytes in, room %u\n", na, wa, nb, wb, (unsigned)n, (unsigned)cap);
    free(a), free(b);
    return same;
}

static void inflate_matches_stb(void)
{
    for (int round = 0; round < 400; round++)
    {
        size_t n;
        uint8_t *z = make_stream(&n);
        size_t cap = 4 << 20;
        CHECK(same_reading(z, n, cap, 0), "stream %d", round);
        /* too little room */
        CHECK(same_reading(z, n, below(60000), 0), "stream %d, little room", round);
        /* cut short */
        CHECK(same_reading(z, below((uint32_t)n + 1), cap, 0), "stream %d, cut short", round);
        /* damaged */
        for (int k = 0; k < 6; k++)
        {
            size_t at = below((uint32_t)n);
            uint8_t was = z[at];
            z[at] ^= (uint8_t)(1 << below(8));
            CHECK(same_reading(z, n, cap, 0), "stream %d, byte %u damaged", round, (unsigned)at);
            z[at] = was;
        }
        free(z);
    }
    /* zlib streams of our own deflate, whole and in pieces, also through a growing buffer */
    for (int kind = 0; kind < 7; kind++)
    {
        size_t n = 1 + below(400000), zl;
        uint8_t *d = make_data(kind, n);
        uint8_t *z = zip_deflate(d, n, &zl);
        CHECK(same_reading(z, zl, n, 0), "kind %d", kind);
        CHECK(same_reading(z, zl / 2, n, 0), "kind %d, half", kind);
        int rl;
        uint8_t *zz = stbi_zlib_compress(d, (int)n, &rl, 8);
        int ga = 0, gb = 0;
        char *a = stbi_zlib_decode_malloc((const char *)zz, rl, &ga), *b = ref_zlib_decode_malloc((const char *)zz, rl, &gb);
        CHECK(a && b && ga == gb && ga == (int)n && !memcmp(a, d, n) && !memcmp(b, d, n), "kind %d, growing", kind);
        free(a), free(b), STBIW_FREE(zz), free(z), free(d);
    }
}

/* ---- PNG reading ---- */

/* A PNG of raw (already filtered) rows */
static uint8_t *png_of_rows(const uint8_t *rows, size_t len, int w, int h, int depth, int colour, int *n)
{
    int zl;
    uint8_t *z = stbi_zlib_compress((unsigned char *)rows, (int)len, &zl, 8);
    uint8_t *png = malloc((size_t)zl + 64), *o = png;
    memcpy(o, "\x89PNG\r\n\x1a\n", 8), o += 8;
    stbiw__wp32(o, 13);
    stbiw__wptag(o, "IHDR");
    stbiw__wp32(o, w);
    stbiw__wp32(o, h);
    *o++ = (uint8_t)depth, *o++ = (uint8_t)colour, *o++ = 0, *o++ = 0, *o++ = 0;
    stbiw__wpcrc(&o, 13);
    stbiw__wp32(o, zl);
    stbiw__wptag(o, "IDAT");
    memcpy(o, z, (size_t)zl), o += zl;
    stbiw__wpcrc(&o, zl);
    stbiw__wp32(o, 0);
    stbiw__wptag(o, "IEND");
    stbiw__wpcrc(&o, 0);
    *n = (int)(o - png);
    STBIW_FREE(z);
    return png;
}

static int same_picture(const uint8_t *png, int n, int want)
{
    int w1, h1, c1, w2, h2, c2;
    uint8_t *a = stbi_load_from_memory(png, n, &w1, &h1, &c1, want);
    uint8_t *b = ref_load_from_memory(png, n, &w2, &h2, &c2, want);
    int same = (!a && !b) || (a && b && w1 == w2 && h1 == h2 && c1 == c2
                              && !memcmp(a, b, (size_t)w1 * h1 * (want ? want : c1)));
    if (!same)
        printf("  read %s, stb %s\n", a ? "a picture" : stbi_failure_reason(), b ? "a picture" : ref_failure_reason());
    stbi_image_free(a), ref_image_free(b);
    return same;
}

static void png_reading_matches_stb(void)
{
    /* every colour type and depth, rows filtered at random */
    static const int kinds[][2] = { { 0, 1 }, { 0, 2 }, { 0, 4 }, { 0, 8 }, { 0, 16 }, { 2, 8 }, { 2, 16 }, { 4, 8 }, { 4, 16 }, { 6, 8 }, { 6, 16 } };
    for (int round = 0; round < 220; round++)
    {
        int colour = kinds[round % 11][0], depth = kinds[round % 11][1];
        int chans = colour == 0 ? 1 : colour == 2 ? 3 : colour == 4 ? 2 : 4;
        int w = 1 + (int)below(round < 110 ? 9 : 300), h = 1 + (int)below(40);
        size_t row = ((size_t)w * chans * depth + 7) / 8, len = (row + 1) * h;
        uint8_t *rows = malloc(len);
        for (size_t i = 0; i < len; i++)
            rows[i] = i % (row + 1) == 0 ? (uint8_t)below(5) : below(4) ? (uint8_t)(i / 7) : (uint8_t)rnd();
        int n;
        uint8_t *png = png_of_rows(rows, len, w, h, depth, colour, &n);
        CHECK(same_picture(png, n, 0), "colour type %d, %d bits, %dx%d", colour, depth, w, h);
        CHECK(same_picture(png, n, 4), "colour type %d, %d bits, %dx%d, as RGBA", colour, depth, w, h);
        rows[(row + 1) * below((uint32_t)h)] = 5;     /* a filter that doesn't exist */
        free(png);
        png = png_of_rows(rows, len, w, h, depth, colour, &n);
        CHECK(same_picture(png, n, 4), "colour type %d, %d bits: bad filter", colour, depth);
        free(png), free(rows);
    }
    /* pictures stb's writer filters, each filter in turn */
    for (int f = -1; f < 5; f++)
        for (int kind = 0; kind < 4; kind++)
        {
            img *im = make_picture(kind, 1 + (int)below(400), 1 + (int)below(200));
            stbi_write_force_png_filter = f;
            for (int chans = 1; chans <= 4; chans++)
            {
                int n;
                uint8_t *png = stbi_write_png_to_mem(im->px, im->w * 4, im->w * 4 / chans, im->h, chans, &n);
                CHECK(same_picture(png, n, 4), "filter %d, %d channels", f, chans);
                STBIW_FREE(png);
            }
            stbi_write_force_png_filter = -1;
            img_free(im);
        }
}

typedef struct
{
    uint8_t *p;
    int n;
} mem;

static void put(void *ctx, void *data, int size)
{
    mem *o = ctx;
    memcpy(o->p + o->n, data, (size_t)size);
    o->n += size;
}

static void jpeg_reading_matches_stb(void)
{
    for (int kind = 0; kind < 4; kind++)
    {
        img *im = make_picture(kind, 1 + (int)below(500), 1 + (int)below(300));
        mem out = { malloc(4 << 20), 0 };
        stbi_write_jpg_to_func(put, &out, im->w, im->h, 4, im->px, 90);
        CHECK(same_picture(out.p, out.n, 3), "kind %d, %dx%d", kind, im->w, im->h);
        free(out.p), img_free(im);
    }
}

static void unfilter_matches_its_c(void)
{
    for (int round = 0; round < 3000; round++)
    {
        size_t bpp = 1 + below(8), n = bpp * (1 + below(round < 2000 ? 20 : 2000));
        uint8_t *raw = malloc(n), *prior = malloc(n), *a = malloc(n), *b = malloc(n);
        for (size_t i = 0; i < n; i++)
            raw[i] = (uint8_t)rnd(), prior[i] = (uint8_t)rnd();
        int f = (int)below(6);
        k_unfilter(a, raw, prior, bpp, n, f);
        k_unfilter_c(b, raw, prior, bpp, n, f);
        CHECK(!memcmp(a, b, n), "filter %d, %u bytes a pixel, %u bytes", f, (unsigned)bpp, (unsigned)n);
        free(raw), free(prior), free(a), free(b);
    }
}

int main(void)
{
    struct
    {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "deflate_matches_stb", deflate_matches_stb },
        { "deflate_kernel_matches_its_c", deflate_kernel_matches_its_c },
        { "png_matches_stb", png_matches_stb },
        { "png_rows_match_stb", png_rows_match_stb },
        { "checksums", checksums },
        { "inflate_matches_stb", inflate_matches_stb },
        { "png_reading_matches_stb", png_reading_matches_stb },
        { "jpeg_reading_matches_stb", jpeg_reading_matches_stb },
        { "unfilter_matches_its_c", unfilter_matches_its_c },
    };
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++)
    {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all kernel tests passed\n");
    return 0;
}
