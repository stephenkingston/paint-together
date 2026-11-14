/*
 * Paint (C port) - tests of picture files and raw deflate (core/img.c)
 *
 * Every format Paint writes is written and read back; the readers of our own
 * (ICO, TIFF) are also fed hand-made files with the variants they promise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/img.h"
#include "os/os.h"

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

static char tmp_dir[512];

static char *tmp_path(const char *name)
{
    return os_path_join(tmp_dir, name);
}

/* A picture with a gradient, a few flat areas and some transparency */
static img *sample(int w, int h, int alpha)
{
    img *im = img_new(w, h, (rgba){ 0, 0, 0, 0 });
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            uint8_t *p = img_at(im, x, y);
            p[0] = (uint8_t)(x * 255 / (w - 1));
            p[1] = (uint8_t)(y * 255 / (h - 1));
            p[2] = (uint8_t)((x + y) * 4);
            p[3] = alpha ? (uint8_t)(x < w / 4 ? 0 : x < w / 2 ? 128 : 255) : 255;
        }
    return im;
}

/* A picture of at most 8 colours, transparent in one corner */
static img *flat_sample(int w, int h)
{
    static const rgba cols[8] = { { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, { 237, 28, 36, 255 }, { 0, 162, 232, 255 },
                                  { 34, 177, 76, 255 }, { 255, 242, 0, 255 }, { 163, 73, 164, 255 }, { 1, 2, 3, 255 } };
    img *im = img_new(w, h, cols[0]);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            rgba c = cols[(x / 7 + y / 5) % 8];
            if (x < 5 && y < 5)
                c = (rgba){ 0, 0, 0, 0 };
            memcpy(img_at(im, x, y), &c, 4);
        }
    return im;
}

static int same(const img *a, const img *b)
{
    return a && b && a->w == b->w && a->h == b->h && !memcmp(a->px, b->px, (size_t)a->w * a->h * 4);
}

/* Mean absolute difference of the colour channels */
static double colour_error(const img *a, const img *b)
{
    double e = 0;
    size_t n = (size_t)a->w * a->h;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++)
            e += abs(a->px[i * 4 + c] - b->px[i * 4 + c]);
    return e / (double)(n * 3);
}

static img *round_trip(const img *im, const char *name)
{
    char *path = tmp_path(name);
    const char *err = NULL;
    int ok = img_save(im, path, &err);
    CHECK(ok == 0, "saving %s: %s", name, err ? err : "?");
    img *back = ok == 0 ? img_load(path, &err) : NULL;
    CHECK(back != NULL, "reading %s back: %s", name, err ? err : "?");
    remove(path);
    free(path);
    return back;
}

static void png_keeps_everything(void)
{
    img *im = sample(37, 21, 1), *back = round_trip(im, "t.png");
    CHECK(same(im, back), "png differs");
    img *other = round_trip(im, "t.unknown");
    CHECK(same(im, other), "unknown extension is png");
    size_t len = 0;
    uint8_t *png = img_png(im, &len);
    img *mem = png ? img_load_mem(png, len, NULL) : NULL;
    CHECK(png && !memcmp(png, "\x89PNG", 4) && same(im, mem), "png in memory");
    free(png);
    img_free(mem);
    img_free(im);
    img_free(back);
    img_free(other);
}

/* JPEG and BMP have no alpha: the colours are kept as they are, made opaque */
static void bmp_and_jpeg_drop_alpha(void)
{
    img *im = sample(40, 30, 1), *opaque = img_dup(im);
    for (int i = 0; i < 40 * 30; i++)
        opaque->px[i * 4 + 3] = 255;
    img *bmp = round_trip(im, "t.BMP");
    CHECK(same(opaque, bmp), "bmp is the colours, opaque");
    img *jpg = round_trip(im, "t.jpeg");
    CHECK(jpg && jpg->w == 40 && jpg->h == 30 && jpg->px[3] == 255, "jpeg size and alpha");
    if (jpg)
        CHECK(colour_error(opaque, jpg) < 6.0, "jpeg error %.2f", colour_error(opaque, jpg));
    img_free(im);
    img_free(opaque);
    img_free(bmp);
    img_free(jpg);
}

static void gif_exact_when_few_colours(void)
{
    img *im = flat_sample(300, 200), *back = round_trip(im, "t.gif");
    int ok = back && back->w == 300 && back->h == 200;
    for (int i = 0; ok && i < 300 * 200; i++)
    {
        const uint8_t *a = im->px + i * 4, *b = back->px + i * 4;
        ok = a[3] == 0 ? b[3] == 0 : !memcmp(a, b, 4);
    }
    CHECK(ok, "gif with few colours isn't exact");
    /* Half-transparent pixels become opaque; the clear ones stay clear */
    img *s = sample(20, 20, 1);
    img *g = round_trip(s, "t2.gif");
    CHECK(g && g->px[3] == 0 && g->px[(10 * 20 + 6) * 4 + 3] == 255, "gif alpha %d %d", g ? g->px[3] : -1, g ? g->px[(10 * 20 + 6) * 4 + 3] : -1);
    img_free(s);
    img_free(g);
    /* Two colours: the smallest palette */
    img *bw = img_new(9, 9, (rgba){ 255, 255, 255, 255 });
    bw->px[0] = bw->px[1] = bw->px[2] = 0;
    img *bwb = round_trip(bw, "t3.gif");
    CHECK(same(bw, bwb), "two-colour gif");
    img_free(bw);
    img_free(bwb);
    img_free(im);
    img_free(back);
}

static void gif_quantizes_many_colours(void)
{
    img *im = sample(256, 256, 0), *back = round_trip(im, "t.gif");
    CHECK(back && back->w == 256, "size");
    if (back)
        /* A hard case (65536 scattered colours): ImageMagick's -colors 256 gets 8.9 */
        CHECK(colour_error(im, back) < 9.0, "quantization error %.2f", colour_error(im, back));
    /* Noise: LZW tables fill up and restart */
    img *noise = img_new(400, 300, (rgba){ 0, 0, 0, 255 });
    uint32_t r = 12345;
    for (int i = 0; i < 400 * 300; i++)
    {
        r = r * 1103515245u + 12345u;
        uint8_t v = (uint8_t)((r >> 16) % 200);
        noise->px[i * 4] = v, noise->px[i * 4 + 1] = (uint8_t)(v * 3), noise->px[i * 4 + 2] = (uint8_t)(255 - v);
    }
    img *nb = round_trip(noise, "noise.gif");
    CHECK(same(noise, nb), "noisy gif with 200 colours isn't exact");
    img_free(noise);
    img_free(nb);
    img_free(im);
    img_free(back);
}

static void tiff_round_trip(void)
{
    img *im = sample(33, 17, 1), *back = round_trip(im, "t.tif");
    CHECK(same(im, back), "tiff differs");
    img_free(im);
    img_free(back);
}

/* ---- hand-made TIFFs ---- */

typedef struct
{
    uint8_t d[4096];
    size_t n;
    int be;
} bytes;

static void b8(bytes *b, unsigned v)
{
    b->d[b->n++] = (uint8_t)v;
}

static void b16(bytes *b, unsigned v)
{
    if (b->be)
        b8(b, v >> 8), b8(b, v);
    else
        b8(b, v), b8(b, v >> 8);
}

static void b32(bytes *b, uint32_t v)
{
    if (b->be)
        b16(b, v >> 16), b16(b, v);
    else
        b16(b, v), b16(b, v >> 16);
}

/* A TIFF with the given strip data, tags in order; bps per sample; palette if cmap */
static void make_tiff(bytes *b, int be, int w, int h, int spp, int bps, int photo, int comp, const uint8_t *data, size_t len,
                      const uint16_t *cmap, int extra)
{
    memset(b, 0, sizeof *b);
    b->be = be;
    b8(b, be ? 'M' : 'I'), b8(b, be ? 'M' : 'I'), b16(b, 42), b32(b, 8);
    int ntags = 9 + (cmap ? 1 : 0) + (extra ? 1 : 0);
    uint32_t after = 8 + 2 + 12 * (uint32_t)ntags + 4;
    uint32_t bits_off = after, data_off = bits_off + 8, cmap_off = data_off + (uint32_t)len;
    b16(b, (unsigned)ntags);
#define TAG(t, type, count, val)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        b16(b, t), b16(b, type), b32(b, count);                                                                        \
        if ((type) == 3 && (count) == 1)                                                                               \
            b16(b, val), b16(b, 0);                                                                                    \
        else                                                                                                           \
            b32(b, val);                                                                                               \
    } while (0)
    TAG(256, 3, 1, (unsigned)w);
    TAG(257, 3, 1, (unsigned)h);
    if (spp == 1)
        TAG(258, 3, 1, (unsigned)bps);
    else
        TAG(258, 3, (unsigned)spp, bits_off);
    TAG(259, 3, 1, (unsigned)comp);
    TAG(262, 3, 1, (unsigned)photo);
    TAG(273, 4, 1, data_off);
    TAG(277, 3, 1, (unsigned)spp);
    TAG(278, 3, 1, (unsigned)h);
    TAG(279, 4, 1, (uint32_t)len);
    if (cmap)
        TAG(320, 3, 3u << bps, cmap_off);
    if (extra)
        TAG(338, 3, 1, (unsigned)extra);
#undef TAG
    b32(b, 0);
    for (int i = 0; i < 4; i++)
        b16(b, (unsigned)bps);
    memcpy(b->d + b->n, data, len);
    b->n += len;
    if (cmap)
        for (int i = 0; i < 3 << bps; i++)
            b16(b, cmap[i]);
}

/* TIFF LZW of `in`, every byte a literal: Clear, the bytes, End (9-bit codes, MSB first) */
static size_t lzw_literals(const uint8_t *in, size_t n, uint8_t *out)
{
    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n + 2; i++)
    {
        unsigned code = i == 0 ? 256 : i == n + 1 ? 257 : in[i - 1];
        acc = acc << 9 | code;
        nbits += 9;
        while (nbits >= 8)
            out[o++] = (uint8_t)(acc >> (nbits - 8)), nbits -= 8;
    }
    if (nbits)
        out[o++] = (uint8_t)(acc << (8 - nbits));
    return o;
}

/* A 4x2 picture, red and blue alternating, saved by ImageMagick with -compress LZW
 * (which also uses the horizontal-differencing predictor) */
static const uint8_t magick_lzw_tiff[] = {
    0x49, 0x49, 0x2a, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x80, 0x3f, 0xc0, 0x00, 0x00, 0x08, 0x01, 0xff, 0x02, 0x00, 0xc1, 0x60,
    0x50, 0x68, 0x44, 0x2a, 0x11, 0x01, 0x10, 0x00, 0x00, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00,
    0xe0, 0x00, 0x00, 0x00, 0x03, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x06, 0x01, 0x03, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0a, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x11, 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x15, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x16, 0x01, 0x03, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x17, 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
    0x1c, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x29, 0x01, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x3d, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x3e, 0x01, 0x05, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x16, 0x01, 0x00, 0x00, 0x3f, 0x01, 0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0xe6, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00, 0x85, 0xeb, 0x51, 0x00, 0x00, 0x00, 0x80, 0x00, 0xc3, 0xf5,
    0xa8, 0x00, 0x00, 0x00, 0x00, 0x02, 0xcd, 0xcc, 0x4c, 0x00, 0x00, 0x00, 0x00, 0x01, 0xcd, 0xcc, 0x4c, 0x00, 0x00, 0x00,
    0x80, 0x00, 0xcd, 0xcc, 0x4c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x8f, 0xc2, 0xf5, 0x00, 0x00, 0x00, 0x00, 0x10, 0x37, 0x1a,
    0xa0, 0x00, 0x00, 0x00, 0x00, 0x02, 0x2b, 0x87, 0x0a, 0x00, 0x00, 0x00, 0x20, 0x00
};

static void tiff_variants(void)
{
    const char *err = NULL;
    bytes b;
    /* 2x2 RGB, uncompressed, big-endian */
    const uint8_t rgb[12] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30 };
    make_tiff(&b, 1, 2, 2, 3, 8, 2, 1, rgb, 12, NULL, 0);
    img *im = img_load_mem(b.d, b.n, &err);
    CHECK(im && im->w == 2 && !memcmp(img_at(im, 1, 1), "\x0a\x14\x1e\xff", 4), "plain rgb: %s", im ? "wrong pixels" : err);
    img_free(im);
    /* The same through LZW (literals only) and PackBits */
    uint8_t z[64];
    size_t zn = lzw_literals(rgb, 12, z);
    make_tiff(&b, 0, 2, 2, 3, 8, 2, 5, z, zn, NULL, 0);
    im = img_load_mem(b.d, b.n, &err);
    CHECK(im && !memcmp(img_at(im, 0, 1), "\x00\x00\xff\xff", 4), "lzw rgb: %s", im ? "wrong pixels" : err);
    img_free(im);
    const uint8_t pb[] = { 0xFD, 7, 9, 5, 255, 0, 9, 1, 2, 3, 4, 5, 6 };   /* 4 x 7, then 10 literals = 14 bytes, 2 rows of 7 */
    const uint8_t gray_expect[14] = { 7, 7, 7, 7, 5, 255, 0, 9, 1, 2, 3, 4, 5, 6 };
    make_tiff(&b, 0, 7, 2, 1, 8, 1, 32773, pb, sizeof pb, NULL, 0);
    im = img_load_mem(b.d, b.n, &err);
    int ok = im && im->w == 7;
    for (int i = 0; ok && i < 14; i++)
        ok = im->px[i * 4] == gray_expect[i] && im->px[i * 4 + 1] == gray_expect[i] && im->px[i * 4 + 3] == 255;
    CHECK(ok, "packbits grey: %s", im ? "wrong pixels" : err);
    img_free(im);
    /* 8-bit palette, and 4-bit palette */
    static uint16_t cmap[3 * 256];
    for (int i = 0; i < 256; i++)
        cmap[i] = (uint16_t)(i * 257), cmap[256 + i] = (uint16_t)((255 - i) * 257), cmap[512 + i] = 0x8080;
    const uint8_t idx[4] = { 0, 10, 200, 255 };
    make_tiff(&b, 0, 4, 1, 1, 8, 3, 1, idx, 4, cmap, 0);
    im = img_load_mem(b.d, b.n, &err);
    CHECK(im && !memcmp(img_at(im, 2, 0), "\xc8\x37\x80\xff", 4), "palette: %s", im ? "wrong pixels" : err);
    img_free(im);
    static uint16_t cmap4[3 * 16];
    for (int i = 0; i < 16; i++)
        cmap4[i] = (uint16_t)(i * 17 * 257), cmap4[16 + i] = 0, cmap4[32 + i] = 0xFFFF;
    const uint8_t idx4[2] = { 0x1F, 0x80 };     /* 1, 15, 8, 0 */
    make_tiff(&b, 1, 4, 1, 1, 4, 3, 1, idx4, 2, cmap4, 0);
    im = img_load_mem(b.d, b.n, &err);
    CHECK(im && im->px[0] == 17 && im->px[4] == 255 && im->px[8] == 136 && im->px[12] == 0 && im->px[2] == 255, "4-bit palette: %s",
          im ? "wrong pixels" : err);
    img_free(im);
    /* RGBA with premultiplied (associated) alpha */
    const uint8_t pm[4] = { 64, 32, 0, 128 };
    make_tiff(&b, 0, 1, 1, 4, 8, 2, 1, pm, 4, NULL, 1);
    im = img_load_mem(b.d, b.n, &err);
    CHECK(im && im->px[0] == 127 && im->px[1] == 63 && im->px[3] == 128, "associated alpha: %s", im ? "wrong pixels" : err);
    img_free(im);
    /* A real LZW stream with string references */
    im = img_load_mem(magick_lzw_tiff, sizeof magick_lzw_tiff, &err);
    ok = im != NULL;
    for (int i = 0; ok && i < 8; i++)
        ok = !memcmp(im->px + i * 4, (i + i / 4) % 2 ? "\x00\x00\xff\xff" : "\xff\x00\x00\xff", 4);
    CHECK(ok, "magick lzw: %s", im ? "wrong pixels" : err);
    img_free(im);
    /* Unsupported: 16-bit... is fine to refuse, with a message */
    make_tiff(&b, 0, 2, 2, 3, 2, 2, 1, rgb, 12, NULL, 0);
    im = img_load_mem(b.d, b.n, &err);
    CHECK(!im && err && strstr(err, "TIFF"), "2-bit rgb should be refused");
}

/* ---- ICO ---- */

static void le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v), le16(p + 2, v >> 16);
}

/* A BMP icon entry: header, palette, pixels bottom-up, AND mask */
static size_t ico_bmp(uint8_t *out, int w, int h, int bpp, int x_opaque_from)
{
    memset(out, 0, 40);
    le32(out, 40), le32(out + 4, (uint32_t)w), le32(out + 8, (uint32_t)h * 2), le16(out + 12, 1), le16(out + 14, (unsigned)bpp);
    size_t o = 40;
    if (bpp == 8)
        for (int i = 0; i < 256; i++)
            out[o++] = (uint8_t)i, out[o++] = 0, out[o++] = (uint8_t)(255 - i), out[o++] = 0;   /* B G R x */
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4, mstride = ((size_t)w + 31) / 32 * 4;
    for (int y = h - 1; y >= 0; y--)
    {
        memset(out + o, 0, stride);
        for (int x = 0; x < w; x++)
        {
            if (bpp == 32)
            {
                uint8_t *p = out + o + x * 4;
                p[0] = (uint8_t)x, p[1] = (uint8_t)y, p[2] = 200, p[3] = x < x_opaque_from ? 0 : 255;
            }
            else
                out[o + x] = (uint8_t)(x * 16);
        }
        o += stride;
    }
    for (int y = h - 1; y >= 0; y--)
    {
        memset(out + o, 0, mstride);
        for (int x = 0; x < w; x++)
            if (x < x_opaque_from)
                out[o + x / 8] |= (uint8_t)(0x80 >> (x % 8));
        o += mstride;
    }
    return o;
}

static void ico_picks_the_largest(void)
{
    static uint8_t ico[65536];
    img *small = sample(16, 16, 1);
    size_t png_len = 0;
    uint8_t *png = img_png(small, &png_len);
    uint8_t e32[40 + 32 * 32 * 4 + 32 * 4], e8[40 + 1024 + 8 * 8 + 8 * 4];
    size_t n32 = ico_bmp(e32, 32, 32, 32, 3), n8 = ico_bmp(e8, 8, 8, 8, 2);
    le16(ico, 0), le16(ico + 2, 1), le16(ico + 4, 3);
    size_t off = 6 + 3 * 16;
    const uint8_t *data[3] = { png, e32, e8 };
    size_t lens[3] = { png_len, n32, n8 };
    const int sizes[3] = { 16, 32, 8 };
    for (int i = 0; i < 3; i++)
    {
        uint8_t *e = ico + 6 + 16 * i;
        memset(e, 0, 16);
        e[0] = e[1] = (uint8_t)sizes[i];
        le16(e + 4, 1), le16(e + 6, 32), le32(e + 8, (uint32_t)lens[i]), le32(e + 12, (uint32_t)off);
        memcpy(ico + off, data[i], lens[i]);
        off += lens[i];
    }
    const char *err = NULL;
    img *im = img_load_mem(ico, off, &err);
    CHECK(im && im->w == 32 && im->h == 32, "largest entry: %s", im ? "wrong size" : err);
    if (im)
    {
        CHECK(!memcmp(img_at(im, 5, 7), "\xc8\x07\x05\xff", 4), "32-bit pixel %d %d %d %d", img_at(im, 5, 7)[0], img_at(im, 5, 7)[1],
              img_at(im, 5, 7)[2], img_at(im, 5, 7)[3]);
        CHECK(img_at(im, 1, 0)[3] == 0, "32-bit alpha");
    }
    img_free(im);
    /* Just the 8-bit entry: palette colours, transparency from the AND mask */
    le16(ico + 4, 1);
    memcpy(ico + 6, ico + 6 + 32, 16);
    le32(ico + 6 + 12, 6 + 16);
    memcpy(ico + 6 + 16, e8, n8);
    im = img_load_mem(ico, 6 + 16 + n8, &err);
    CHECK(im && im->w == 8 && img_at(im, 0, 0)[3] == 0 && !memcmp(img_at(im, 3, 4), "\xcf\x00\x30\xff", 4), "8-bit entry: %s",
          im ? "wrong pixels" : err);
    img_free(im);
    /* Just the PNG */
    le32(ico + 6 + 8, (uint32_t)png_len);
    memcpy(ico + 6 + 16, png, png_len);
    im = img_load_mem(ico, 6 + 16 + png_len, &err);
    CHECK(im && !memcmp(im->px, small->px, 16 * 16 * 4), "png entry: %s", im ? "wrong pixels" : err);
    img_free(im);
    img_free(small);
    free(png);
}

static void unreadable_files_say_why(void)
{
    const char *err = NULL;
    uint8_t webp[32] = "RIFF\x18\0\0\0WEBPVP8L";
    CHECK(!img_load_mem(webp, sizeof webp, &err) && err && strstr(err, "WebP"), "webp: %s", err ? err : "?");
    uint8_t junk[64] = "this is not a picture at all, just some text";
    err = NULL;
    CHECK(!img_load_mem(junk, sizeof junk, &err) && err, "junk");
    err = NULL;
    CHECK(!img_load("/no/such/file.png", &err) && err, "missing file");
    img *im = img_new(4, 4, (rgba){ 1, 2, 3, 4 });
    char *path = tmp_path("t.webp");
    err = NULL;
    CHECK(img_save(im, path, &err) != 0 && err && strstr(err, "WebP"), "saving webp");
    free(path);
    img_free(im);
}

static void crop_and_rects(void)
{
    img *im = sample(10, 10, 0);
    img *c = img_crop(im, 8, -1, 4, 3);
    CHECK(c->w == 4 && c->h == 3 && c->px[3] == 0 && !memcmp(img_at(c, 0, 1), img_at(im, 8, 0), 4) && img_at(c, 2, 1)[3] == 0, "crop");
    img_free(c);
    img_free(im);
    irect a = irect_make(0, 0, 10, 10), e = IRECT_EMPTY;
    irect u = irect_union(e, a);
    CHECK(u.x0 == 0 && u.x1 == 10 && irect_empty(irect_intersect(a, irect_make(20, 20, 30, 30))), "union and intersect");
    irect ar = irect_around(5.5f, 2.0f, 1.25f, 7.0f, 1.0f);
    CHECK(ar.x0 == 0 && ar.y0 == 1 && ar.x1 == 8 && ar.y1 == 9, "around %d %d %d %d", ar.x0, ar.y0, ar.x1, ar.y1);
    CHECK(irect_contains(a, 9.9f, 0.0f) && !irect_contains(a, 10.0f, 0.0f) && irect_contains_rect(a, irect_make(1, 1, 10, 10)), "contains");
}

static void deflate_round_trips(void)
{
    size_t n = 200000;
    uint8_t *data = malloc(n);
    uint32_t r = 1;
    for (size_t i = 0; i < n; i++)
    {
        r = r * 1664525u + 1013904223u;
        data[i] = i < n / 2 ? (uint8_t)(i / 1000) : (uint8_t)(r >> 24);
    }
    size_t zl = 0, ol = 0;
    uint8_t *z = zip_deflate(data, n, &zl);
    CHECK(z && zl < n, "compressed %lu to %lu", (unsigned long)n, (unsigned long)zl);
    uint8_t *back = z ? zip_inflate(z, zl, n, &ol) : NULL;
    CHECK(back && ol == n && !memcmp(back, data, n), "round trip");
    free(back);
    CHECK(z && !zip_inflate(z, zl, n - 1, &ol), "over the limit is refused");
    /* Damaged data */
    if (z)
    {
        for (size_t i = 0; i < zl; i += 7)
            z[i] ^= 0x5A;
        back = zip_inflate(z, zl, n, &ol);
        CHECK(!back || ol != n || memcmp(back, data, n), "damaged data decoded as the original");
        free(back);
    }
    free(z);
    /* Empty input */
    z = zip_deflate(data, 0, &zl);
    back = z ? zip_inflate(z, zl, 100, &ol) : NULL;
    CHECK(back && ol == 0, "empty round trip");
    free(z);
    free(back);
    /* A stream the box's zlib made: raw deflate of "hello hello hello hello" (python zlib, wbits -15) */
    const uint8_t hello[] = { 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01 };
    back = zip_inflate(hello, sizeof hello, 1000, &ol);
    CHECK(back && ol == 23 && !memcmp(back, "hello hello hello hello", 23), "foreign stream");
    free(back);
    free(data);
}

int main(void)
{
    os_init();
    const char *t = getenv("TMPDIR");
    if (!t || !*t)
        t = getenv("TEMP");
    snprintf(tmp_dir, sizeof tmp_dir, "%s", t && *t ? t : "/tmp");
    struct
    {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "png_keeps_everything", png_keeps_everything },
        { "bmp_and_jpeg_drop_alpha", bmp_and_jpeg_drop_alpha },
        { "gif_exact_when_few_colours", gif_exact_when_few_colours },
        { "gif_quantizes_many_colours", gif_quantizes_many_colours },
        { "tiff_round_trip", tiff_round_trip },
        { "tiff_variants", tiff_variants },
        { "ico_picks_the_largest", ico_picks_the_largest },
        { "unreadable_files_say_why", unreadable_files_say_why },
        { "crop_and_rects", crop_and_rects },
        { "deflate_round_trips", deflate_round_trips },
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
    printf("all format tests passed\n");
    return 0;
}
