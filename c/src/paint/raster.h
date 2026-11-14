/*
 * Paint (C port) - pixel operations on the canvas: coverage masks, compositing, fills (Rust: raster.rs)
 *
 * Drawing tools never paint straight onto the canvas. They rasterize coverage
 * (0..1 per pixel) into a mask, keeping the maximum per pixel, and the mask is
 * then composited over a snapshot taken when the operation started. That keeps
 * anti-aliased strokes free of the dark "beads" you get from re-blending
 * overlapping dabs, and lets shapes be re-rasterized on every mouse move.
 *
 * Points are canvas pixels as floats, (0,0) being the top-left corner of the
 * top-left pixel (egui's Pos2 in the Rust).
 */
#ifndef PAINT_RASTER_H
#define PAINT_RASTER_H

#include <stdint.h>
#include "core/img.h"

typedef struct
{
    float x, y;
} pt;

static inline pt pt_make(float x, float y)
{
    pt p = { x, y };
    return p;
}

/* A float rectangle, min corner (x0, y0) to max corner (x1, y1) (egui's Rect) */
typedef struct
{
    float x0, y0, x1, y1;
} frect;

frect frect_from_two(pt a, pt b);       /* the box spanned by two points (Rect::from_two_pos) */

static inline int rgba_eq(rgba a, rgba b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* ---- coverage masks ---- */

/* Per-pixel coverage for one drawing operation. The values are f32 as in the
 * Rust, but stored in 64 x 64 tiles that exist only where something was drawn,
 * so a mask the size of the picture costs little until it is painted on, and
 * clearing a tile hands its memory back. */
#define MASK_TILE_SHIFT 6
#define MASK_TILE (1 << MASK_TILE_SHIFT)
typedef struct
{
    int w, h;
    int tw, th;         /* tiles across and down */
    float **tiles;      /* tw * th, NULL where all zero */
} mask;

mask *mask_new(int w, int h);           /* all zero; NULL if out of memory */
void mask_free(mask *m);
float *mask_tile_alloc(mask *m, int tile); /* (internal to mask_max_at) */

static inline irect mask_bounds(const mask *m)
{
    return irect_make(0, 0, m->w, m->h);
}
static inline float mask_get(const mask *m, int x, int y)
{
    if (x < 0 || y < 0 || x >= m->w || y >= m->h)
        return 0.0f;
    const float *t = m->tiles[(y >> MASK_TILE_SHIFT) * m->tw + (x >> MASK_TILE_SHIFT)];
    return t ? t[((y & (MASK_TILE - 1)) << MASK_TILE_SHIFT) | (x & (MASK_TILE - 1))] : 0.0f;
}
/* Raise the pixel's coverage to v (at most 1) */
static inline void mask_max_at(mask *m, int x, int y, float v)
{
    if (x >= 0 && y >= 0 && x < m->w && y < m->h && v > 0.0f)
    {
        int tile = (y >> MASK_TILE_SHIFT) * m->tw + (x >> MASK_TILE_SHIFT);
        float *t = m->tiles[tile] ? m->tiles[tile] : mask_tile_alloc(m, tile);
        if (!t)
            return;
        float *p = &t[((y & (MASK_TILE - 1)) << MASK_TILE_SHIFT) | (x & (MASK_TILE - 1))];
        if (v > *p)
            *p = v < 1.0f ? v : 1.0f;
    }
}
/* Zero r; tiles wholly inside it are freed */
void mask_clear(mask *m, irect r);
void mask_reset(mask *m);               /* zero everything, freeing every tile */

/* Visit every pixel within `reach` of the segment a-b. The callback gets the
 * pixel, its distance to the segment and its signed distance across the
 * segment's direction, and returns the coverage to write. */
typedef float (*mask_sweep_fn)(int x, int y, float d, float perp, void *user);
irect mask_sweep(mask *m, pt a, pt b, float reach, mask_sweep_fn f, void *user);
/* Anti-aliased segment with round caps */
irect mask_capsule(mask *m, pt a, pt b, float radius, float weight);
/* Aliased line drawn with a size x size square pen (pencil, eraser) */
irect mask_square_line(mask *m, pt a, pt b, int size);
/* Anti-aliased even-odd polygon fill: 4 vertical subsamples per pixel with exact horizontal span coverage */
irect mask_fill_polygon(mask *m, const pt *pts, int n, float weight);
/* Stroke a polyline with round joins and caps */
irect mask_stroke_path(mask *m, const pt *pts, int n, int closed, float radius);

/* ---- pixels ---- */

/* Source-over blend of c at alpha a onto dst (both unpremultiplied) */
rgba rgba_over(rgba dst, rgba c, float a);

/* Copy the pixels in r from src to dst (pictures of the same size) */
void raster_copy_rect(img *dst, const img *src, irect r);
/* Blend color onto dst through the mask within r */
void raster_blend_mask(img *dst, const mask *m, rgba color, float opacity, irect r);
void raster_fill_rect(img *im, irect r, rgba color);
/* The pixels of r (clipped to the picture) as a new picture; 1x1 transparent if nothing is left */
img *raster_crop(const img *im, irect r);
/* Alpha-composite src onto dst with its top-left at (x, y). Pixels whose colour
 * matches key (3 bytes RGB, or NULL for none) are skipped: Paint's "transparent
 * selection". Returns the pixels touched. */
irect raster_blit_over(img *dst, const img *src, int x, int y, const uint8_t *key);
/* Scanline flood fill of the region connected to (x, y). Pixels whose channels
 * all lie within tolerance of the seed colour are replaced; then the blended
 * pixels of anti-aliased edges around the region are re-mixed with the new colour. */
irect raster_flood_fill(img *im, int x, int y, rgba color, uint8_t tolerance);
/* A w x h canvas holding im at the top-left, padded with fill */
img *raster_resize_canvas(const img *im, int w, int h, rgba fill);
img *raster_invert(const img *im);
/* Overwrite dst's pixels with src at (x, y), clipped (imageops::replace) */
void raster_replace(img *dst, const img *src, int x, int y);

#endif
