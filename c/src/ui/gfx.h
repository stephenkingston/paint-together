/*
 * Paint (C port) - drawing the window into 32-bit pixels
 *
 * A pixel is 0xAARRGGBB in a uint32_t (bytes B G R A in memory: what a
 * Windows DIB and an X11 TrueColor ZPixmap take). The window's surface is
 * opaque; colours passed in have straight alpha and are blended when the
 * alpha is below 255. Everything is clipped to the surface's clip rectangle.
 *
 * Integer calls (fills, rounded rectangles, gradients) cover whole pixels
 * with coverage on rounded corners only. Float calls (lines, polygons,
 * circles) are anti-aliased: they are what the vector icons, the tool
 * pointers and the friends' pointers are drawn with. Coordinates are pixels
 * with (0,0) at the top-left corner of the top-left pixel, so the centre of
 * pixel (x,y) is (x+0.5, y+0.5).
 */
#ifndef UI_GFX_H
#define UI_GFX_H

#include <stdint.h>
#include "core/img.h"

typedef uint32_t color;
#define COLOR(rgb) ((color)(0xFF000000u | (uint32_t)(rgb)))
#define COLOR_A(rgb, alpha) ((color)((uint32_t)(alpha) << 24 | ((uint32_t)(rgb) & 0xFFFFFFu)))
#define RGB3(r, g, b) ((color)(0xFF000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b)))
#define C_ALPHA(c) ((int)((c) >> 24))
#define C_R(c) ((int)((c) >> 16 & 255))
#define C_G(c) ((int)((c) >> 8 & 255))
#define C_B(c) ((int)((c) & 255))

typedef struct
{
    int x, y, w, h;
} rect;

typedef struct
{
    uint32_t *px;
    int w, h, stride;   /* stride in pixels */
    rect clip;
    /* Window position of pixel (0,0): 0 for the window's surface. An overlay
     * (a menu, a dialog) is a small surface at its place in the window, so
     * the same window coordinates draw into it. The clip is in window
     * coordinates too. */
    int ox, oy;
} surface;

/* ---- rectangles ---- */
rect rect_make(int x, int y, int w, int h);
rect rect_inset(rect r, int by);            /* negative grows */
rect rect_cut(rect a, rect b);              /* the intersection (w or h 0 when empty) */
rect rect_union(rect a, rect b);            /* an empty one is ignored */
int rect_has(rect r, int x, int y);
int rect_empty(rect r);

/* ---- surfaces (the host owns the window's; these are for off-screen work) ---- */
surface *surface_new(int w, int h);         /* opaque white */
void surface_free(surface *s);
void surface_resize(surface *s, int w, int h);  /* contents undefined afterwards */
rect gfx_clip(surface *dst, rect r);        /* narrows the clip to r; returns the old clip to put back */
/* A surface covering window rectangle r (ox, oy = r.x, r.y), opaque white; resizing keeps the struct */
surface *surface_at(rect r);
void surface_move(surface *s, rect r);      /* a new place and size; contents undefined afterwards */

/* ---- colour ---- */
color color_mix(color a, color b, int t);   /* t 0..256: a to b */
color color_with_alpha(color c, int alpha);

/* ---- whole pixels ---- */
void gfx_fill(surface *dst, rect r, color c);
void gfx_frame(surface *dst, rect r, int width, color c);         /* the border only, inside r */
void gfx_hline(surface *dst, int x0, int x1, int y, color c);     /* x0..x1 inclusive */
void gfx_vline(surface *dst, int x, int y0, int y1, color c);
void gfx_round(surface *dst, rect r, int radius, color fill);
void gfx_round_frame(surface *dst, rect r, int radius, int width, color c);
void gfx_round_both(surface *dst, rect r, int radius, color fill, color border);    /* fill inside a 1 px border */
void gfx_vgradient(surface *dst, rect r, int radius, color top, color bottom);
void gfx_hgradient(surface *dst, rect r, color left, color right);
/* Alternating dashes of a and b along the rectangle's border (marching ants), phase in pixels */
void gfx_dashed_frame(surface *dst, rect r, int dash, int phase, color a, color b);
void gfx_checker(surface *dst, rect r, int cell, color a, color b);

/* ---- anti-aliased vectors (float pixel coordinates) ---- */
void gfx_line(surface *dst, float x0, float y0, float x1, float y1, float width, color c);  /* round caps */
void gfx_polyline(surface *dst, const float *xy, int n, float width, color c, int closed);  /* round joins */
void gfx_polygon(surface *dst, const float *xy, int n, color c);       /* filled, non-zero winding */
/* Several closed contours filled as one shape (holes by winding); counts[i] points each */
void gfx_polygons(surface *dst, const float *xy, const int *counts, int contours, color c);
void gfx_circle(surface *dst, float cx, float cy, float r, color c);
void gfx_ring(surface *dst, float cx, float cy, float r, float width, color c);
void gfx_ellipse(surface *dst, float cx, float cy, float rx, float ry, color c);
void gfx_ellipse_ring(surface *dst, float cx, float cy, float rx, float ry, float width, color c);
/* A rectangle with float edges and corner radius, filled (anti-aliased) */
void gfx_rrect(surface *dst, float x, float y, float w, float h, float radius, color c);
void gfx_rrect_ring(surface *dst, float x, float y, float w, float h, float radius, float width, color c);

/* A stroke with a choice of joins and caps (gfx_polyline is GFX_CLOSED or not, round joins) */
enum
{
    GFX_CLOSED = 1,
    GFX_MITER = 2,          /* mitered joins (bevelled when very sharp); round otherwise */
    GFX_BUTT = 4,           /* flat ends on an open line; round otherwise */
};
void gfx_stroke(surface *dst, const float *xy, int n, float width, color c, int flags);
/* Filled contours (non-zero) coloured per pixel: shade(x, y) at the pixel's centre */
typedef color (*gfx_shader)(float x, float y, void *user);
void gfx_polygons_shaded(surface *dst, const float *xy, const int *counts, int contours, gfx_shader shade, void *user);
/* A soft shadow under rectangle r (egui's Shadow: offset, blur, colour); r itself is not filled */
void gfx_shadow(surface *dst, rect r, int dx, int dy, int blur, color c);
/* Copy (no blending) the pixels of src that fall in window rectangle r into dst */
void gfx_copy(surface *dst, const surface *src, rect r);

/* ---- pictures ---- */
/* An 8-bit coverage mask (a glyph) tinted with c; pitch in bytes */
void gfx_mask(surface *dst, int x, int y, const uint8_t *alpha, int w, int h, int pitch, color c);
/* 0xAARRGGBB pixels with straight alpha, blended (an icon cached as pixels) */
void gfx_blend(surface *dst, int x, int y, const uint32_t *src, int w, int h, int stride);
/* An RGBA img, blended */
void gfx_img(surface *dst, int x, int y, const img *im);
/*
 * The canvas view: the part of `im` at picture coordinates (sx, sy) and
 * zoom `zoom` (screen pixels per picture pixel) drawn into `to` on screen.
 * Screen pixel (to.x + i, to.y + j) shows picture point
 * (sx + (i + 0.5) / zoom, sy + (j + 0.5) / zoom): nearest neighbour when
 * zoom >= 1, an area average when zoomed out. Alpha is drawn over a
 * checkerboard. This is the hot path: it must be fast at 4K.
 */
void gfx_canvas(surface *dst, rect to, const img *im, double sx, double sy, double zoom);
/* The checkerboard's square in screen pixels (8 at 100%: theme sets it with the scale) */
void gfx_canvas_checker(int cell);
/* For the next gfx_canvas only: a picture of the same size whose pixels that aren't
 * fully transparent are shown instead of the picture's (friends' drawing in progress) */
void gfx_canvas_over(const img *over);

#endif
