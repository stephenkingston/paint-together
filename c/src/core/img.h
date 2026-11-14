/*
 * Paint (C port) - RGBA pictures, and reading and writing picture files
 *
 * An img is straight (not premultiplied) RGBA, 8 bits a channel, rows top
 * to bottom with no padding: byte (y * w + x) * 4 is red. That is the byte
 * order of Rust's RgbaImage, of the drawing-together patches on the wire,
 * and of stb_image. The screen's surface (ui/gfx.h) is a different type.
 */
#ifndef CORE_IMG_H
#define CORE_IMG_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int w, h;
    uint8_t *px;        /* w * h * 4 bytes */
} img;

typedef struct
{
    uint8_t r, g, b, a;
} rgba;


/* Integer pixel rectangle, half-open: [x0, x1) x [y0, y1) (Rust's raster::IRect) */
typedef struct
{
    int x0, y0, x1, y1;
} irect;

#define IRECT_EMPTY ((irect){ 2147483647, 2147483647, -2147483647 - 1, -2147483647 - 1 })
static inline irect irect_make(int x0, int y0, int x1, int y1)
{
    irect r = { x0, y0, x1, y1 };
    return r;
}
static inline int irect_empty(irect r)
{
    return r.x0 >= r.x1 || r.y0 >= r.y1;
}
static inline int irect_w(irect r)
{
    return r.x1 > r.x0 ? r.x1 - r.x0 : 0;
}
static inline int irect_h(irect r)
{
    return r.y1 > r.y0 ? r.y1 - r.y0 : 0;
}
irect irect_union(irect a, irect b);       /* an empty one is ignored */
irect irect_intersect(irect a, irect b);
int irect_contains_rect(irect outer, irect inner);
int irect_contains(irect r, float x, float y);
irect irect_around(float ax, float ay, float bx, float by, float pad);
irect irect_of(const img *im);

img *img_new(int w, int h, rgba fill);
img *img_dup(const img *src);
img *img_crop(const img *src, int x, int y, int w, int h);     /* the part inside src; outside is transparent */
void img_free(img *im);
static inline uint8_t *img_at(const img *im, int x, int y)
{
    return im->px + ((size_t)y * (size_t)im->w + (size_t)x) * 4;
}

/* Files. The format is sniffed when reading and taken from the extension when
 * writing (.png .jpg .jpeg .bmp .gif .tif .tiff; others: PNG). On failure,
 * NULL / -1 and *err (static text) says why. */
img *img_load(const char *path, const char **err);
img *img_load_mem(const void *data, size_t len, const char **err);
int img_save(const img *im, const char *path, const char **err);
/* PNG in memory (for the clipboard); malloc'd */
uint8_t *img_png(const img *im, size_t *len);

/* Raw deflate (RFC 1951, no zlib header), as the drawing-together frames use */
uint8_t *zip_deflate(const uint8_t *data, size_t len, size_t *out_len);            /* malloc'd */
uint8_t *zip_inflate(const uint8_t *data, size_t len, size_t limit, size_t *out_len);   /* NULL if bad or over limit */

#endif
