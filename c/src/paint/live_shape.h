/*
 * Paint (C port) - a shape that stays editable after it is drawn (Rust: live_shape.rs)
 *
 * As in Paint, until it is committed the shape can be moved, resized from its
 * handles and restyled, and is re-rasterised from scratch each time over a
 * snapshot of the picture taken when it was started.
 */
#ifndef PAINT_LIVE_SHAPE_H
#define PAINT_LIVE_SHAPE_H

#include "paint/raster.h"
#include "paint/shapes.h"
#include "paint/snap.h"

/* How the shape is drawn; taken from the ribbon, so changing it there restyles the shape */
typedef struct
{
    int outline, fill;
    float size;
    rgba line_color, fill_color;
} shape_style;

typedef struct
{
    shape_kind kind;
    pt a, b;            /* corners of the box (or a line's end points), in canvas pixels */
    int swapped;        /* drawn with the right button: Color 1 and Color 2 swap roles */
    img *snapshot;      /* the picture under the shape (owned)... */
    snap *under;        /* ...or, when there's none, the parts of it the shape has covered */
    mask *fill_mask, *line_mask;
    irect drawn, touched;
    int rendered;       /* the key below is valid */
    shape_kind r_kind;
    pt r_a, r_b;
    shape_style r_style;
} live_shape;

/* Takes ownership of snapshot. NULL if out of memory (snapshot is then freed). */
live_shape *live_shape_new(shape_kind kind, pt at, int swapped, img *snapshot);
/* The same keeping only what the shape covers of pic (snap.h: until the shape is
 * committed, only it and live_shape_rebase may change pic) */
live_shape *live_shape_over(shape_kind kind, pt at, int swapped, const img *pic);
void live_shape_free(live_shape *s);

int live_shape_is_line(const live_shape *s);
int live_shape_is_empty(const live_shape *s);       /* nothing was dragged out (a plain click) */
frect live_shape_rect(const live_shape *s);
static inline irect live_shape_touched(const live_shape *s)    /* every pixel the shape has ever covered */
{
    return s->touched;
}
/* Redraw onto im if the geometry or style changed; returns the pixels to refresh */
irect live_shape_render(live_shape *s, img *im, shape_style style);
/* The picture under the shape changed in r (a friend drew there): use theirs as
 * the background and redraw on the next render */
void live_shape_rebase(live_shape *s, const img *base, irect r);
/* The undo step for the shape drawn on im */
change *live_shape_change(const live_shape *s, const img *im);
/* Put back what was under the shape; returns the pixels to refresh */
irect live_shape_erase(const live_shape *s, img *im);
/* Handle positions in canvas pixels: the ends of a line (2), or eight around a
 * box clockwise from the top-left corner. Returns how many. */
int live_shape_handles(const live_shape *s, pt out[8]);
int live_shape_handle_at(const live_shape *s, pt p, float tol);   /* -1: none */
/* Whether p grabs the shape's body (within its box, or near a line) */
int live_shape_contains(const live_shape *s, pt p, float tol);
/* Drag handle h to p, starting from the geometry (a, b) it had when grabbed.
 * Shift keeps lines at 45 degree steps and corner drags square. */
void live_shape_drag_handle(live_shape *s, int h, pt a, pt b, pt p, int shift);
void live_shape_translate(live_shape *s, pt a, pt b, float dx, float dy);

/* The pointer for a handle: corners diagonal, edges straight, a line's ends a cross */
enum
{
    SHAPE_CURSOR_CROSS,
    SHAPE_CURSOR_NWSE,
    SHAPE_CURSOR_NESW,
    SHAPE_CURSOR_NS,
    SHAPE_CURSOR_EW,
};
int live_shape_handle_cursor(int is_line, int h);

#endif
