/*
 * Paint (C port) - RGBA pictures, picture files and raw deflate (Rust: raster::IRect, the image crate, flate2)
 *
 * stb_image reads PNG, JPEG, BMP and GIF; small readers here do ICO
 * (PNG or BMP entries, the largest wins) and baseline TIFF. stb_image_write
 * writes PNG, JPEG and BMP; GIF and TIFF writers are here. Saving does what
 * the Rust app does through the image crate: JPEG and BMP drop the alpha
 * channel, GIF keeps fully transparent pixels transparent and uses an exact
 * palette when there are 256 colours or fewer, TIFF is uncompressed RGBA.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "core/img.h"
#include "core/kernels.h"
#include "os/os.h"
#include "stb_image.h"
#include "stb_image_write.h"

/* ---- rectangles ---- */

static int f2i(float v)
{
    if (v != v)
        return 0;
    if (v >= 2147483647.0f)
        return 2147483647;
    if (v <= -2147483648.0f)
        return -2147483647 - 1;
    return (int)v;
}

static int imin(int a, int b)
{
    return a < b ? a : b;
}

static int imax(int a, int b)
{
    return a > b ? a : b;
}

irect irect_union(irect a, irect b)
{
    if (irect_empty(a))
        return b;
    if (irect_empty(b))
        return a;
    return irect_make(imin(a.x0, b.x0), imin(a.y0, b.y0), imax(a.x1, b.x1), imax(a.y1, b.y1));
}

irect irect_intersect(irect a, irect b)
{
    return irect_make(imax(a.x0, b.x0), imax(a.y0, b.y0), imin(a.x1, b.x1), imin(a.y1, b.y1));
}

int irect_contains_rect(irect o, irect i)
{
    return i.x0 >= o.x0 && i.y0 >= o.y0 && i.x1 <= o.x1 && i.y1 <= o.y1;
}

int irect_contains(irect r, float x, float y)
{
    return x >= (float)r.x0 && x < (float)r.x1 && y >= (float)r.y0 && y < (float)r.y1;
}

irect irect_around(float ax, float ay, float bx, float by, float pad)
{
    float x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
    float y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;
    return irect_make(f2i(floorf(x0 - pad)), f2i(floorf(y0 - pad)), f2i(ceilf(x1 + pad)) + 1,
                      f2i(ceilf(y1 + pad)) + 1);
}

irect irect_of(const img *im)
{
    return irect_make(0, 0, im->w, im->h);
}

/* ---- pictures ---- */

img *img_new(int w, int h, rgba fill)
{
    if (w < 0 || h < 0 || (h && (size_t)w > ((size_t)-1 / 4) / (size_t)h))
        return NULL;
    img *im = malloc(sizeof *im);
    size_t n = (size_t)w * h;
    /* Transparent: a big picture comes zeroed from the system, its pages taking memory only once written
     * (a friends' drawing overlay, mostly empty, stays small) */
    int clear = !fill.r && !fill.g && !fill.b && !fill.a;
    uint8_t *px = clear ? calloc(n * 4 + 4, 1) : malloc(n * 4 + 4);
    if (!im || !px)
    {
        free(im);
        free(px);
        return NULL;
    }
    im->w = w;
    im->h = h;
    im->px = px;
    if (!clear && fill.r == fill.g && fill.g == fill.b && fill.b == fill.a)
        memset(px, fill.r, n * 4);
    else if (!clear)
        for (size_t i = 0; i < n; i++)
            memcpy(px + i * 4, &fill, 4);
    return im;
}

img *img_dup(const img *src)
{
    img *im = img_new(src->w, src->h, (rgba){ 0, 0, 0, 0 });
    if (im)
        memcpy(im->px, src->px, (size_t)src->w * src->h * 4);
    return im;
}

img *img_crop(const img *src, int x, int y, int w, int h)
{
    img *out = img_new(w, h, (rgba){ 0, 0, 0, 0 });
    if (!out)
        return NULL;
    irect r = irect_intersect(irect_make(x, y, x + w, y + h), irect_of(src));
    for (int py = r.y0; py < r.y1; py++)
        memcpy(img_at(out, r.x0 - x, py - y), img_at(src, r.x0, py), (size_t)(r.x1 - r.x0) * 4);
    return out;
}

void img_free(img *im)
{
    if (im)
    {
        free(im->px);
        free(im);
    }
}

/* Wrap malloc'd pixels (w * h * 4 bytes) */
static img *img_wrap(uint8_t *px, int w, int h)
{
    img *im = malloc(sizeof *im);
    if (!im)
    {
        free(px);
        return NULL;
    }
    im->w = w;
    im->h = h;
    im->px = px;
    return im;
}

/* ---- little helpers for the codecs ---- */

static unsigned rd16(const uint8_t *p, int be)
{
    return be ? (unsigned)p[0] << 8 | p[1] : (unsigned)p[1] << 8 | p[0];
}

static uint32_t rd32(const uint8_t *p, int be)
{
    return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
              : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* A growing byte buffer; `bad` is set if memory runs out */
typedef struct
{
    uint8_t *p;
    size_t n, cap;
    int bad;
} buf;

static void put(buf *b, const void *data, size_t len)
{
    if (b->bad)
        return;
    if (b->n + len > b->cap)
    {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + len)
            cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p)
        {
            b->bad = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, len);
    b->n += len;
}

static void put8(buf *b, unsigned v)
{
    uint8_t c = (uint8_t)v;
    put(b, &c, 1);
}

static void put16le(buf *b, unsigned v)
{
    uint8_t c[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put(b, c, 2);
}

static void put32le(buf *b, uint32_t v)
{
    uint8_t c[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(b, c, 4);
}

static void stb_put(void *ctx, void *data, int size)
{
    put(ctx, data, (size_t)size);
}

/* ---- ICO ---- */

/* A BMP entry of an icon: a BITMAPINFOHEADER, the palette, the colour pixels
 * bottom-up, then a 1-bit transparency (AND) mask; the stated height covers both. */
static img *ico_dib(const uint8_t *d, size_t len, const char **err)
{
    if (len < 40)
        return NULL;
    uint32_t hsize = rd32(d, 0);
    int w = (int)rd32(d + 4, 0), h = (int)rd32(d + 8, 0) / 2;
    unsigned bpp = rd16(d + 14, 0);
    uint32_t comp = rd32(d + 16, 0), used = rd32(d + 32, 0);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || hsize < 40 || hsize > len || comp != 0)
        return NULL;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return NULL;
    size_t colours = bpp <= 8 ? (used && used <= (1u << bpp) ? used : 1u << bpp) : 0;
    const uint8_t *pal = d + hsize;
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4, mstride = ((size_t)w + 31) / 32 * 4;
    const uint8_t *bits = pal + colours * 4, *andm = bits + stride * h;
    int has_mask = (size_t)(andm - d) + mstride * h <= len;
    if ((size_t)(andm - d) > len)
        return NULL;
    img *im = img_new(w, h, (rgba){ 0, 0, 0, 0 });
    if (!im)
        return NULL;
    int any_alpha = 0;
    for (int y = 0; y < h; y++)
    {
        const uint8_t *row = bits + stride * (h - 1 - y);
        for (int x = 0; x < w; x++)
        {
            uint8_t *o = img_at(im, x, y);
            if (bpp <= 8)
            {
                unsigned i = (row[x * bpp / 8] >> (8 - bpp - (x * bpp % 8))) & ((1u << bpp) - 1);
                if (i < colours)
                {
                    o[0] = pal[i * 4 + 2];
                    o[1] = pal[i * 4 + 1];
                    o[2] = pal[i * 4];
                }
                o[3] = 255;
            }
            else
            {
                const uint8_t *p = row + x * (bpp / 8);
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
                o[3] = bpp == 32 ? p[3] : 255;
                any_alpha |= bpp == 32 && p[3];
            }
        }
    }
    /* 32-bit entries carry their own alpha; older ones (or 32-bit with none) use the mask */
    if (has_mask && !any_alpha)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const uint8_t *row = andm + mstride * (h - 1 - y);
                img_at(im, x, y)[3] = (row[x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
            }
    (void)err;
    return im;
}

static img *load_ico(const uint8_t *d, size_t len, const char **err)
{
    unsigned n = rd16(d + 4, 0);
    if (len < 6 + 16 * (size_t)n || n == 0)
    {
        *err = "The icon file is damaged.";
        return NULL;
    }
    img *best = NULL;
    for (unsigned i = 0; i < n; i++)
    {
        const uint8_t *e = d + 6 + 16 * i;
        uint32_t size = rd32(e + 8, 0), off = rd32(e + 12, 0);
        if (off >= len || size > len - off)
            continue;
        img *im = NULL;
        if (size >= 8 && !memcmp(d + off, "\x89PNG", 4))
        {
            int w, h, c;
            uint8_t *px = stbi_load_from_memory(d + off, (int)size, &w, &h, &c, 4);
            if (px)
                im = img_wrap(px, w, h);
        }
        else
            im = ico_dib(d + off, size, err);
        if (!im)
            continue;
        if (!best || (int64_t)im->w * im->h > (int64_t)best->w * best->h)
        {
            img_free(best);
            best = im;
        }
        else
            img_free(im);
    }
    if (!best)
        *err = "Paint can't read any of the pictures in this icon file.";
    return best;
}

/* ---- TIFF reading: baseline, 8-bit (and 1/4-bit palette and grey), one IFD ---- */

typedef struct
{
    const uint8_t *d;
    size_t len;
    int be;
} tiff;

/* The values of a tag (SHORT or LONG), up to cap of them; returns how many */
static size_t tiff_values(const tiff *t, const uint8_t *entry, uint32_t *out, size_t cap)
{
    unsigned type = rd16(entry + 2, t->be);
    uint32_t count = rd32(entry + 4, t->be);
    unsigned sz = type == 3 ? 2 : type == 4 ? 4 : type == 1 ? 1 : 0;
    if (!sz || count == 0)
        return 0;
    const uint8_t *p = entry + 8;
    if ((uint64_t)count * sz > 4)
    {
        uint32_t off = rd32(entry + 8, t->be);
        if (off > t->len || (uint64_t)count * sz > t->len - off)
            return 0;
        p = t->d + off;
    }
    size_t n = count < cap ? count : cap;
    for (size_t i = 0; i < n; i++)
        out[i] = sz == 2 ? rd16(p + i * 2, t->be) : sz == 4 ? rd32(p + i * 4, t->be) : p[i];
    return n;
}

/* TIFF LZW: MSB-first codes of 9 to 12 bits, the width growing one code early */
static size_t tiff_lzw(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    static const int CLEAR = 256, EOI = 257;
    uint16_t *prefix = malloc(4096 * sizeof(uint16_t));
    uint8_t *suffix = malloc(4096), *stack = malloc(4096);
    size_t o = 0;
    if (!prefix || !suffix || !stack)
        goto done;
    uint32_t acc = 0;
    int nbits = 0, width = 9, next = 258, prev = -1;
    uint8_t first = 0;
    size_t i = 0;
    while (o < cap)
    {
        while (nbits < width && i < len)
        {
            acc = acc << 8 | in[i++];
            nbits += 8;
        }
        if (nbits < width)
            break;
        int code = (int)(acc >> (nbits - width)) & ((1 << width) - 1);
        nbits -= width;
        if (code == EOI)
            break;
        if (code == CLEAR)
        {
            width = 9;
            next = 258;
            prev = -1;
            continue;
        }
        int sp = 0, c = code;
        if (prev < 0)
        {
            if (code > 255)
                break;
            out[o++] = (uint8_t)code;
            first = (uint8_t)code;
            prev = code;
            continue;
        }
        if (code > next || (code == next && next >= 4096))
            break;
        if (code == next)
        {
            stack[sp++] = first;
            c = prev;
        }
        while (c > 255 && sp < 4095)
        {
            stack[sp++] = suffix[c];
            c = prefix[c];
        }
        stack[sp++] = (uint8_t)c;
        first = (uint8_t)c;
        while (sp && o < cap)
            out[o++] = stack[--sp];
        if (next < 4096)
        {
            prefix[next] = (uint16_t)prev;
            suffix[next] = first;
            next++;
        }
        prev = code;
        if (next + 1 >= (1 << width) && width < 12)
            width++;
    }
done:
    free(prefix);
    free(suffix);
    free(stack);
    return o;
}

static size_t tiff_packbits(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < len && o < cap)
    {
        int n = (int8_t)in[i++];
        if (n >= 0)
        {
            for (int k = 0; k <= n && i < len && o < cap; k++)
                out[o++] = in[i++];
        }
        else if (n != -128 && i < len)
        {
            for (int k = 0; k < 1 - n && o < cap; k++)
                out[o++] = in[i];
            i++;
        }
    }
    return o;
}

static img *load_tiff(const uint8_t *d, size_t len, const char **err)
{
    tiff t = { d, len, d[0] == 'M' };
    uint32_t ifd = rd32(d + 4, t.be);
    if (ifd > len - 2)
        goto bad;
    unsigned n = rd16(d + ifd, t.be);
    if ((uint64_t)ifd + 2 + 12ull * n > len)
        goto bad;
    uint32_t w = 0, h = 0, bits[4] = { 1, 1, 1, 1 }, comp = 1, photo = 2, spp = 1, rps = 0xFFFFFFFF, planar = 1;
    uint32_t pred = 1, extra = 0, nbits = 0;
    uint32_t *offs = NULL, *counts = NULL, *cmap = NULL;
    size_t noffs = 0, ncounts = 0, ncmap = 0;
    img *im = NULL;
    uint8_t *strip = NULL;
    for (unsigned i = 0; i < n; i++)
    {
        const uint8_t *e = d + ifd + 2 + 12 * i;
        unsigned tag = rd16(e, t.be);
        uint32_t count = rd32(e + 4, t.be), v = 0;
        tiff_values(&t, e, &v, 1);
        switch (tag)
        {
        case 256: w = v; break;
        case 257: h = v; break;
        case 258: nbits = (uint32_t)tiff_values(&t, e, bits, 4); break;
        case 259: comp = v; break;
        case 262: photo = v; break;
        case 277: spp = v; break;
        case 278: rps = v; break;
        case 284: planar = v; break;
        case 317: pred = v; break;
        case 338: extra = v; break;
        case 273:
        case 279:
        case 320:
            if (count > 1 << 24)
                goto bad_fail;
            {
                uint32_t *a = malloc((count + 1) * sizeof(uint32_t));
                size_t got = a ? tiff_values(&t, e, a, count) : 0;
                if (tag == 273)
                    offs = a, noffs = got;
                else if (tag == 279)
                    counts = a, ncounts = got;
                else
                    cmap = a, ncmap = got;
            }
            break;
        }
    }
    (void)nbits;
    unsigned bps = bits[0];
    int ok_bits = bps == 8 || ((photo <= 1 || photo == 3) && spp == 1 && (bps == 1 || bps == 4));
    if (!w || !h || w > 1 << 15 || h > 1 << 15 || !noffs || noffs != ncounts || planar != 1 || !ok_bits || spp < 1 || spp > 4
        || (photo == 2 && spp < 3) || photo > 3 || (photo == 3 && ncmap < 3u << bps))
    {
        *err = "Paint can only read baseline TIFF pictures (8-bit grey, RGB, RGBA or palette, one strip layout).";
        goto fail;
    }
    if (comp != 1 && comp != 5 && comp != 32773)
    {
        *err = "This TIFF picture uses a compression Paint can't read.";
        goto fail;
    }
    if (rps == 0 || rps > h)
        rps = h;
    size_t row = ((size_t)w * spp * bps + 7) / 8;
    size_t strip_cap = row * rps;
    strip = malloc(strip_cap + 1);
    im = img_new((int)w, (int)h, (rgba){ 0, 0, 0, 255 });
    if (!strip || !im)
    {
        *err = "There isn't enough memory to open this picture.";
        goto fail;
    }
    for (size_t s = 0; s < noffs && s * rps < h; s++)
    {
        if (offs[s] > len || counts[s] > len - offs[s])
            goto bad_fail;
        const uint8_t *src = d + offs[s];
        size_t got;
        memset(strip, 0, strip_cap);
        if (comp == 1)
            got = counts[s] < strip_cap ? counts[s] : strip_cap, memcpy(strip, src, got);
        else if (comp == 5)
            got = tiff_lzw(src, counts[s], strip, strip_cap);
        else
            got = tiff_packbits(src, counts[s], strip, strip_cap);
        (void)got;
        uint32_t y0 = (uint32_t)(s * rps), rows = h - y0 < rps ? h - y0 : rps;
        for (uint32_t r = 0; r < rows; r++)
        {
            uint8_t *line = strip + r * row;
            if (pred == 2 && bps == 8)
                for (size_t k = spp; k < (size_t)w * spp; k++)
                    line[k] = (uint8_t)(line[k] + line[k - spp]);
            for (uint32_t x = 0; x < w; x++)
            {
                uint8_t *o = img_at(im, (int)x, (int)(y0 + r));
                if (bps < 8)
                {
                    unsigned v = (line[x * bps / 8] >> (8 - bps - (x * bps % 8))) & ((1u << bps) - 1);
                    if (photo == 3)
                    {
                        o[0] = (uint8_t)(cmap[v] >> 8);
                        o[1] = (uint8_t)(cmap[(1u << bps) + v] >> 8);
                        o[2] = (uint8_t)(cmap[(2u << bps) + v] >> 8);
                    }
                    else
                    {
                        unsigned g = v * 255 / ((1u << bps) - 1);
                        o[0] = o[1] = o[2] = (uint8_t)(photo == 0 ? 255 - g : g);
                    }
                    continue;
                }
                const uint8_t *p = line + (size_t)x * spp;
                if (photo == 3)
                {
                    o[0] = (uint8_t)(cmap[p[0]] >> 8);
                    o[1] = (uint8_t)(cmap[256 + p[0]] >> 8);
                    o[2] = (uint8_t)(cmap[512 + p[0]] >> 8);
                }
                else if (photo <= 1)
                {
                    o[0] = o[1] = o[2] = photo == 0 ? 255 - p[0] : p[0];
                    if (spp >= 2)
                        o[3] = p[1];
                }
                else
                {
                    o[0] = p[0];
                    o[1] = p[1];
                    o[2] = p[2];
                    if (spp >= 4)
                        o[3] = p[3];
                }
                /* Associated (premultiplied) alpha: back to straight */
                if (extra == 1 && o[3] && o[3] < 255 && photo != 3)
                    for (int k = 0; k < 3; k++)
                        o[k] = (uint8_t)(o[k] * 255 / o[3] > 255 ? 255 : o[k] * 255 / o[3]);
            }
        }
    }
    free(strip);
    free(offs);
    free(counts);
    free(cmap);
    return im;
bad_fail:
    *err = "The TIFF file is damaged.";
fail:
    free(strip);
    free(offs);
    free(counts);
    free(cmap);
    img_free(im);
    return NULL;
bad:
    *err = "The TIFF file is damaged.";
    return NULL;
}

/* ---- reading ---- */

img *img_load_mem(const void *data, size_t len, const char **err)
{
    const char *dummy;
    if (!err)
        err = &dummy;
    const uint8_t *d = data;
    if (len < 12 || len > 0x7FFFFFFF)
    {
        *err = len < 12 ? "The file is too short to be a picture." : "The file is too big.";
        return NULL;
    }
    if (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
    {
        *err = "Paint can't open WebP pictures.";
        return NULL;
    }
    if (!memcmp(d, "\0\0\1\0", 4))
        return load_ico(d, len, err);
    if (!memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4))
        return load_tiff(d, len, err);
    int w, h, c;
    uint8_t *px = stbi_load_from_memory(d, (int)len, &w, &h, &c, 4);
    if (!px)
    {
        const char *why = stbi_failure_reason();
        *err = why && !strcmp(why, "unknown image type") ? "This is not a picture Paint can open." : why ? why : "The picture is damaged.";
        return NULL;
    }
    img *im = img_wrap(px, w, h);
    if (!im)
        *err = "There isn't enough memory to open this picture.";
    return im;
}

img *img_load(const char *path, const char **err)
{
    const char *dummy;
    if (!err)
        err = &dummy;
    /* Mapped, the file costs no memory of ours while it is decoded */
    size_t len;
    const void *map = os_map_file(path, &len);
    char *data = map ? NULL : os_read_file(path, &len);
    if (!map && !data)
    {
        *err = "The file can't be read.";
        return NULL;
    }
    img *im = img_load_mem(map ? map : data, len, err);
    os_unmap_file(map, len);
    free(data);
    return im;
}

/* ---- GIF writing ---- */

/* A box of colours for median cut: a run of the colour list */
typedef struct
{
    uint32_t *c;        /* 5-5-5 colour buckets */
    size_t len;
} cbox;

static int channel_of(uint32_t b, int ch)
{
    return (int)(b >> (10 - 5 * ch) & 31);
}

static int sort_ch;
static int cmp_ch(const void *a, const void *b)
{
    return channel_of(*(const uint32_t *)a, sort_ch) - channel_of(*(const uint32_t *)b, sort_ch);
}

static int nearest(const uint8_t *pal, int n, int r, int g, int b)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < n; i++)
    {
        int dr = pal[i * 3] - r, dg = pal[i * 3 + 1] - g, db = pal[i * 3 + 2] - b;
        int dd = dr * dr + dg * dg + db * db;
        if (dd < bd)
            bd = dd, best = i;
    }
    return best;
}

/* The middle of a 5-5-5 bucket in 8 bits */
static int bucket_value(uint32_t b, int ch)
{
    return channel_of(b, ch) << 3 | 4;
}

/* Up to `want` palette colours for the opaque pixels: median cut over the 5-5-5
 * colours present, weighted by how many pixels have them, then two rounds of
 * k-means. (The image crate uses NeuQuant; any good palette will do.) */
static int median_cut(const img *im, uint8_t *pal, int want)
{
    uint32_t *hist = calloc(32768, sizeof(uint32_t));
    if (!hist)
        return 0;
    size_t n = (size_t)im->w * im->h, used = 0;
    for (size_t i = 0; i < n; i++)
    {
        const uint8_t *p = im->px + i * 4;
        if (p[3] && !hist[(p[0] >> 3) << 10 | (p[1] >> 3) << 5 | p[2] >> 3]++)
            used++;
    }
    uint32_t *cols = malloc((used + 1) * sizeof(uint32_t));
    if (!cols)
    {
        free(hist);
        return 0;
    }
    size_t k = 0;
    for (uint32_t i = 0; i < 32768; i++)
        if (hist[i])
            cols[k++] = i;
    cbox boxes[256];
    int nb = 0;
    boxes[nb++] = (cbox){ cols, used };
    while (nb < want)
    {
        /* Split the box with the widest channel range at its weighted median */
        int best = -1, best_range = 0, best_ch = 0;
        for (int b = 0; b < nb; b++)
            for (int ch = 0; ch < 3 && boxes[b].len > 1; ch++)
            {
                int lo = 31, hi = 0;
                for (size_t i = 0; i < boxes[b].len; i++)
                {
                    int v = channel_of(boxes[b].c[i], ch);
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
                if (hi - lo > best_range)
                    best = b, best_range = hi - lo, best_ch = ch;
            }
        if (best < 0)
            break;
        cbox *bx = &boxes[best];
        sort_ch = best_ch;
        qsort(bx->c, bx->len, sizeof(uint32_t), cmp_ch);
        uint64_t total = 0, run = 0;
        for (size_t i = 0; i < bx->len; i++)
            total += hist[bx->c[i]];
        size_t half = 1;
        for (size_t i = 0; i + 1 < bx->len; i++)
        {
            run += hist[bx->c[i]];
            half = i + 1;
            if (run * 2 >= total)
                break;
        }
        boxes[nb++] = (cbox){ bx->c + half, bx->len - half };
        bx->len = half;
    }
    for (int round = 0; round <= 2; round++)
    {
        /* Round 0: each box's weighted mean. Then: each colour to its nearest entry. */
        uint64_t (*sum)[4] = calloc((size_t)nb, sizeof *sum);
        if (!sum)
            break;
        for (int b = 0; b < nb; b++)
            for (size_t i = 0; i < boxes[b].len; i++)
            {
                uint32_t c = boxes[b].c[i];
                int to = round ? nearest(pal, nb, bucket_value(c, 0), bucket_value(c, 1), bucket_value(c, 2)) : b;
                for (int ch = 0; ch < 3; ch++)
                    sum[to][ch] += (uint64_t)bucket_value(c, ch) * hist[c];
                sum[to][3] += hist[c];
            }
        for (int b = 0; b < nb; b++)
            if (sum[b][3])
                for (int ch = 0; ch < 3; ch++)
                    pal[b * 3 + ch] = (uint8_t)((sum[b][ch] + sum[b][3] / 2) / sum[b][3]);
        free(sum);
    }
    free(cols);
    free(hist);
    return nb;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* GIF's LZW, codes packed LSB first into 255-byte sub-blocks. The string table
 * is a (prefix code, byte) -> code hash. */
typedef struct
{
    buf *out;
    uint8_t block[255];
    int nblock;
    uint32_t acc;
    int nbits;
} gif_bits;

static void gif_code(gif_bits *g, int code, int width)
{
    g->acc |= (uint32_t)code << g->nbits;
    g->nbits += width;
    while (g->nbits >= 8)
    {
        g->block[g->nblock++] = (uint8_t)g->acc;
        g->acc >>= 8;
        g->nbits -= 8;
        if (g->nblock == 255)
        {
            put8(g->out, 255);
            put(g->out, g->block, 255);
            g->nblock = 0;
        }
    }
}

static int gif_lzw(buf *out, const uint8_t *idx, size_t n, int min_bits)
{
    enum { HSIZE = 8192 };
    int32_t *keys = malloc(HSIZE * sizeof(int32_t));
    int16_t *vals = malloc(HSIZE * sizeof(int16_t));
    if (!keys || !vals)
    {
        free(keys);
        free(vals);
        return -1;
    }
    gif_bits g = { out, { 0 }, 0, 0, 0 };
    int clear = 1 << min_bits, eoi = clear + 1, next = clear + 2, width = min_bits + 1;
    memset(keys, 0xFF, HSIZE * sizeof(int32_t));
    put8(out, (unsigned)min_bits);
    gif_code(&g, clear, width);
    int cur = n ? idx[0] : 0;
    for (size_t i = 1; i < n; i++)
    {
        int32_t key = cur << 8 | idx[i];
        unsigned h = ((unsigned)key * 2654435761u) >> 19;
        while (keys[h] >= 0 && keys[h] != key)
            h = (h + 1) & (HSIZE - 1);
        if (keys[h] == key)
        {
            cur = vals[h];
            continue;
        }
        gif_code(&g, cur, width);
        if (next < 4096)
        {
            keys[h] = key;
            vals[h] = (int16_t)next;
            if (next == (1 << width) && width < 12)
                width++;
            next++;
        }
        else
        {
            gif_code(&g, clear, width);
            memset(keys, 0xFF, HSIZE * sizeof(int32_t));
            next = clear + 2;
            width = min_bits + 1;
        }
        cur = idx[i];
    }
    if (n)
        gif_code(&g, cur, width);
    /* The decoder widens its codes after adding the entry for the code just sent */
    if (next == (1 << width) && width < 12 && n)
        width++;
    gif_code(&g, eoi, width);
    if (g.nbits)
        gif_code(&g, 0, 8 - g.nbits);
    if (g.nblock)
    {
        put8(out, (unsigned)g.nblock);
        put(out, g.block, (size_t)g.nblock);
    }
    put8(out, 0);
    free(keys);
    free(vals);
    return 0;
}

static int write_gif(const img *im, buf *out, const char **err)
{
    if (im->w > 65535 || im->h > 65535)
    {
        *err = "GIF pictures can be at most 65535 pixels wide and high.";
        return -1;
    }
    size_t n = (size_t)im->w * im->h;
    uint8_t *idx = malloc(n + 1);
    uint32_t *cols = malloc(257 * sizeof(uint32_t));
    uint8_t pal[256 * 3];
    if (!idx || !cols)
    {
        free(idx);
        free(cols);
        *err = "There isn't enough memory to save this picture.";
        return -1;
    }
    /* Exact palette when there are 256 colours or fewer, sorted as the gif crate sorts them:
     * by (r, g, b, a) where alpha is 255 for any visible pixel and the transparent pixels
     * all share one entry (the colour of the first of them). */
    int ncols = 0, exact = 1, transparent = -1;
    uint32_t tkey = 0;
    uint32_t set[1024];                 /* open addressing; colour keys always have a nonzero byte */
    memset(set, 0, sizeof set);
    for (size_t i = 0; i < n && exact; i++)
    {
        const uint8_t *p = im->px + i * 4;
        uint32_t key;
        if (p[3])
            key = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | 255;
        else
        {
            if (transparent < 0)
                tkey = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | 0x1u, transparent = 0;
            key = tkey;
        }
        unsigned h = (key * 2654435761u) >> 22;
        while (set[h] && set[h] != key)
            h = (h + 1) & 1023;
        if (set[h])
            continue;
        if (ncols == 256)
            exact = 0;
        else
            set[h] = key, cols[ncols++] = key;
    }
    if (exact)
    {
        qsort(cols, (size_t)ncols, sizeof(uint32_t), cmp_u32);
        for (int k = 0; k < ncols; k++)
        {
            pal[k * 3] = (uint8_t)(cols[k] >> 24);
            pal[k * 3 + 1] = (uint8_t)(cols[k] >> 16);
            pal[k * 3 + 2] = (uint8_t)(cols[k] >> 8);
            if (transparent >= 0 && cols[k] == tkey)
                transparent = k;
        }
        int last = 0;
        uint32_t last_key = ncols ? cols[0] : 0;
        for (size_t i = 0; i < n; i++)
        {
            const uint8_t *p = im->px + i * 4;
            uint32_t key = p[3] ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | 255 : tkey;
            if (key != last_key)
            {
                int lo = 0, hi = ncols - 1;
                while (lo < hi)
                {
                    int mid = (lo + hi) / 2;
                    if (cols[mid] < key)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                last = lo;
                last_key = key;
            }
            idx[i] = (uint8_t)last;
        }
    }
    else
    {
        int has_t = 0;
        for (size_t i = 0; i < n && !has_t; i++)
            has_t = im->px[i * 4 + 3] == 0;
        ncols = median_cut(im, pal, has_t ? 255 : 256);
        if (has_t)
        {
            transparent = ncols;
            memset(pal + ncols * 3, 0, 3);
            ncols++;
        }
        /* Nearest palette entry per 5-5-5 colour, worked out the first time it's needed */
        int16_t *near = malloc(32768 * sizeof(int16_t));
        if (!near)
        {
            free(idx);
            free(cols);
            *err = "There isn't enough memory to save this picture.";
            return -1;
        }
        memset(near, 0xFF, 32768 * sizeof(int16_t));
        int nopaque = has_t ? ncols - 1 : ncols;
        for (size_t i = 0; i < n; i++)
        {
            const uint8_t *p = im->px + i * 4;
            if (!p[3])
            {
                idx[i] = (uint8_t)transparent;
                continue;
            }
            int b = (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | p[2] >> 3;
            if (near[b] < 0)
                near[b] = (int16_t)nearest(pal, nopaque, bucket_value((uint32_t)b, 0), bucket_value((uint32_t)b, 1), bucket_value((uint32_t)b, 2));
            idx[i] = (uint8_t)near[b];
        }
        free(near);
    }
    free(cols);
    int bits = 1;
    while ((1 << bits) < ncols)
        bits++;
    put(out, "GIF89a", 6);
    put16le(out, (unsigned)im->w);
    put16le(out, (unsigned)im->h);
    put8(out, 0x80 | (unsigned)(bits - 1));      /* global colour table of 2^bits entries */
    put8(out, 0);
    put8(out, 0);
    uint8_t zero[3] = { 0, 0, 0 };
    put(out, pal, (size_t)ncols * 3);
    for (int k = ncols; k < 1 << bits; k++)
        put(out, zero, 3);
    if (transparent >= 0)
    {
        const uint8_t gce[8] = { 0x21, 0xF9, 4, 1, 0, 0, (uint8_t)transparent, 0 };
        put(out, gce, 8);
    }
    put8(out, 0x2C);
    put16le(out, 0);
    put16le(out, 0);
    put16le(out, (unsigned)im->w);
    put16le(out, (unsigned)im->h);
    put8(out, 0);
    int ok = gif_lzw(out, idx, n, bits < 2 ? 2 : bits);
    put8(out, 0x3B);
    free(idx);
    if (ok < 0 || out->bad)
    {
        *err = "There isn't enough memory to save this picture.";
        return -1;
    }
    return 0;
}

/* ---- TIFF writing: little-endian, one uncompressed strip of RGBA with unassociated alpha ---- */

static int write_tiff(const img *im, buf *out)
{
    enum { NTAGS = 11 };
    uint32_t data = (uint32_t)(8 + 2 + NTAGS * 12 + 4 + 8);
    uint32_t bytes = (uint32_t)((size_t)im->w * im->h * 4);
    put(out, "II*\0", 4);
    put32le(out, 8);
    put16le(out, NTAGS);
    const uint32_t tags[NTAGS][3] = {
        { 256, 4, (uint32_t)im->w },        /* width */
        { 257, 4, (uint32_t)im->h },        /* height */
        { 258, 3, 0 },                      /* bits per sample: 4 x 8, stored after the IFD */
        { 259, 3, 1 },                      /* no compression */
        { 262, 3, 2 },                      /* RGB */
        { 273, 4, data },                   /* strip offset */
        { 277, 3, 4 },                      /* samples per pixel */
        { 278, 4, (uint32_t)im->h },        /* rows per strip */
        { 279, 4, bytes },                  /* strip byte count */
        { 284, 3, 1 },                      /* chunky */
        { 338, 3, 2 },                      /* the extra sample is unassociated alpha */
    };
    for (int i = 0; i < NTAGS; i++)
    {
        put16le(out, tags[i][0]);
        put16le(out, tags[i][1]);
        put32le(out, tags[i][0] == 258 ? 4 : 1);
        if (tags[i][0] == 258)
            put32le(out, data - 8);
        else if (tags[i][1] == 3)
        {
            put16le(out, tags[i][2]);
            put16le(out, 0);
        }
        else
            put32le(out, tags[i][2]);
    }
    put32le(out, 0);
    for (int i = 0; i < 4; i++)
        put16le(out, 8);
    put(out, im->px, bytes);
    return out->bad ? -1 : 0;
}

/* ---- zlib and PNG writing: the bytes stb_image_write makes, by the kernels in kernels.h ---- */

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* A zlib stream of data as stbi_zlib_compress codes it at quality 8 (Paint's only one), `head` bytes
 * into the buffer, with `tail` bytes of room after it; *n is its length (adler: with the checksum) */
static uint8_t *zlib_compress(const uint8_t *data, size_t len, size_t head, size_t tail, int adler, size_t *n)
{
    if (len > 0x7FFFFFF0)
        return NULL;
    k_deflate_t s = { .data = data, .len = (int32_t)len, .bits = 3, .nbits = 3, .cap = 16 };    /* final block, fixed codes */
    size_t room = head + len / 4 + 64 + tail;
    uint8_t *z = malloc(room), *ok = NULL;
    s.chains = malloc(sizeof *s.chains * K_ZHASH * s.cap);
    s.counts = calloc(K_ZHASH, sizeof *s.counts);
    if (!z || !s.chains || !s.counts)
        goto done;
    z[head] = 0x78, z[head + 1] = 0x5e;
    s.out = z + head + 2;
    for (;;)
    {
        s.end = z + room - tail - 4;
        if (k_deflate(&s))
            break;
        size_t at = (size_t)(s.out - z);
        uint8_t *p = realloc(z, room *= 2);
        if (!p)
            goto done;
        z = p, s.out = z + at;
    }
    for (; (int32_t)s.nbits > 0; s.nbits -= 8, s.bits >>= 8)
        *s.out++ = (uint8_t)s.bits;
    size_t zlen = (size_t)(s.out - z) - head, blocks = (len + 32766) / 32767;
    /* stored blocks instead, if those are smaller */
    if (zlen > len + 2 + blocks * 5)
    {
        size_t need = head + 2 + blocks * 5 + len + 4 + tail;
        if (need > room)
        {
            uint8_t *p = realloc(z, room = need);
            if (!p)
                goto done;
            z = p;
        }
        uint8_t *o = z + head + 2;
        for (size_t j = 0; j < len;)
        {
            size_t b = len - j < 32767 ? len - j : 32767;
            *o++ = len - j == b, *o++ = (uint8_t)b, *o++ = (uint8_t)(b >> 8), *o++ = (uint8_t)~b, *o++ = (uint8_t)(~b >> 8);
            memcpy(o, data + j, b);
            o += b, j += b;
        }
        zlen = (size_t)(o - z) - head;
    }
    if (adler)
        be32(z + head + zlen, k_adler32(1, data, len)), zlen += 4;
    *n = zlen;
    ok = z, z = NULL;
done:
    free(z);
    free(s.chains);
    free(s.counts);
    return ok;
}

/* A PNG of the picture, as stbi_write_png_to_mem makes it: each row filtered by the filter whose bytes
 * add up (as signed) the least, all in one IDAT */
static uint8_t *png_encode(const img *im, size_t *len)
{
    size_t row = (size_t)im->w * 4, n = 0;
    uint8_t *filt = malloc((row + 1) * (size_t)im->h), *zero = calloc(row, 1), *png = NULL;
    if (filt && zero)
    {
        for (int y = 0; y < im->h; y++)
        {
            const uint8_t *cur = im->px + row * (size_t)y, *prev = y ? cur - row : zero;
            uint8_t *o = filt + (row + 1) * (size_t)y;
            uint32_t cost[5];
            int best = 0;
            k_png_costs(cur, prev, row, cost);
            for (int f = 1; f < 5; f++)
                if (cost[f] < cost[best])
                    best = f;
            *o = (uint8_t)best;
            k_png_filter(o + 1, cur, prev, row, best);
        }
        png = zlib_compress(filt, (row + 1) * (size_t)im->h, 41, 16, 1, &n);
    }
    free(filt);
    free(zero);
    if (!png)
        return NULL;
    static const uint8_t head[29] = { 137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', [24] = 8, 6 };
    memcpy(png, head, sizeof head);
    be32(png + 16, (uint32_t)im->w);
    be32(png + 20, (uint32_t)im->h);
    be32(png + 29, k_crc32(0, png + 12, 17));
    be32(png + 33, (uint32_t)n);
    memcpy(png + 37, "IDAT", 4);
    uint8_t *t = png + 41 + n;
    be32(t, k_crc32(0, png + 37, n + 4));
    memcpy(t + 4, "\0\0\0\0IEND\xae\x42\x60\x82", 12);
    *len = 41 + n + 16;
    return png;
}

/* ---- writing ---- */

static int ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/'), *bslash = strrchr(path, '\\');
    if (!dot || (slash && slash > dot) || (bslash && bslash > dot))
        return 0;
    dot++;
    for (; *dot && *ext; dot++, ext++)
        if ((*dot | 0x20) != *ext)
            return 0;
    return !*dot && !*ext;
}

/* The picture without its alpha channel (JPEG and BMP have none: the image crate's to_rgb8) */
static uint8_t *rgb_of(const img *im)
{
    size_t n = (size_t)im->w * im->h;
    uint8_t *rgb = malloc(n * 3 + 1);
    if (rgb)
        for (size_t i = 0; i < n; i++)
            memcpy(rgb + i * 3, im->px + i * 4, 3);
    return rgb;
}

int img_save(const img *im, const char *path, const char **err)
{
    const char *dummy;
    if (!err)
        err = &dummy;
    buf out = { 0 };
    int ok;
    if (im->w <= 0 || im->h <= 0)
    {
        *err = "The picture is empty.";
        return -1;
    }
    if (ext_is(path, "webp"))
    {
        *err = "Paint can't save WebP pictures.";
        return -1;
    }
    if (ext_is(path, "jpg") || ext_is(path, "jpeg") || ext_is(path, "bmp"))
    {
        uint8_t *rgb = rgb_of(im);
        if (!rgb)
        {
            *err = "There isn't enough memory to save this picture.";
            return -1;
        }
        ok = ext_is(path, "bmp") ? stbi_write_bmp_to_func(stb_put, &out, im->w, im->h, 3, rgb)
                                 : stbi_write_jpg_to_func(stb_put, &out, im->w, im->h, 3, rgb, 75);
        free(rgb);
        ok = ok && !out.bad ? 0 : -1;
        if (ok < 0)
            *err = "The picture couldn't be encoded.";
    }
    else if (ext_is(path, "gif"))
        ok = write_gif(im, &out, err);
    else if (ext_is(path, "tif") || ext_is(path, "tiff"))
    {
        ok = write_tiff(im, &out);
        if (ok < 0)
            *err = "There isn't enough memory to save this picture.";
    }
    else
    {
        out.p = png_encode(im, &out.n);
        ok = out.p ? 0 : -1;
        if (ok < 0)
            *err = "The picture couldn't be encoded.";
    }
    if (ok == 0 && os_write_file(path, out.p, out.n) != 0)
    {
        *err = "The file couldn't be written. Is the folder writable, and is there room on the disk?";
        ok = -1;
    }
    free(out.p);
    return ok;
}

uint8_t *img_png(const img *im, size_t *len)
{
    return png_encode(im, len);
}

/* ---- raw deflate ---- */

uint8_t *zip_deflate(const uint8_t *data, size_t len, size_t *out_len)
{
    if (len > 0x7FFFFFF0)
        return NULL;
    if (len == 0)
    {
        /* stb writes no block at all for nothing: one empty final block */
        uint8_t *z = malloc(2);
        if (z)
            z[0] = 0x03, z[1] = 0x00, *out_len = 2;
        return z;
    }
    size_t n = 0;
    /* stb's zlib stream, without its 2-byte header (or the Adler-32 after it) */
    uint8_t *z = zlib_compress(data, len, 0, 0, 0, &n);
    if (z)
        memmove(z, z + 2, n - 2), *out_len = n - 2;
    return z;
}

uint8_t *zip_inflate(const uint8_t *data, size_t len, size_t limit, size_t *out_len)
{
    if (len > 0x7FFFFFF0)
        return NULL;
    if (limit > 0x7FFFFFF0)
        limit = 0x7FFFFFF0;
    /* stb can't tell "damaged" from "output buffer full", so grow the buffer up to the limit */
    size_t cap = len * 4 + 1024;
    for (;;)
    {
        if (cap > limit + 1)
            cap = limit + 1;
        char *out = malloc(cap);
        if (!out)
            return NULL;
        int n = stbi_zlib_decode_noheader_buffer(out, (int)cap, (const char *)data, (int)len);
        if (n >= 0 && (size_t)n <= limit)
        {
            *out_len = (size_t)n;
            return (uint8_t *)out;
        }
        free(out);
        if (n >= 0 || cap == limit + 1)
            return NULL;
        cap *= 2;
    }
}
