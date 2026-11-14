/*
 * Paint (C port) - editable shapes (Rust: live_shape.rs)
 *
 * The two coverage masks are tiled (raster.h), and so is what's under the
 * shape (snap.h), so a live shape costs only the tiles it has covered.
 */
#include <math.h>
#include <stdlib.h>
#include "paint/live_shape.h"

/* Handle directions around the box, clockwise from the top-left corner */
static const signed char HANDLES[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 } };

/* Takes ownership of snapshot (NULL: kept by tile from pic as the shape draws) */
static live_shape *start(shape_kind kind, pt at, int swapped, img *snapshot, const img *pic)
{
    live_shape *s = calloc(1, sizeof *s);
    if (s)
    {
        s->snapshot = snapshot;
        s->under = snapshot ? NULL : snap_new(pic);
        s->fill_mask = mask_new(pic->w, pic->h);
        s->line_mask = mask_new(pic->w, pic->h);
    }
    if (!s || (!snapshot && !s->under) || !s->fill_mask || !s->line_mask)
    {
        if (s)
            live_shape_free(s);
        else
            img_free(snapshot);
        return NULL;
    }
    s->kind = kind;
    s->a = s->b = at;
    s->swapped = swapped;
    s->drawn = s->touched = IRECT_EMPTY;
    return s;
}

live_shape *live_shape_new(shape_kind kind, pt at, int swapped, img *snapshot)
{
    return snapshot ? start(kind, at, swapped, snapshot, snapshot) : NULL;
}

live_shape *live_shape_over(shape_kind kind, pt at, int swapped, const img *pic)
{
    return start(kind, at, swapped, NULL, pic);
}

void live_shape_free(live_shape *s)
{
    if (s)
    {
        img_free(s->snapshot);
        snap_free(s->under);
        mask_free(s->fill_mask);
        mask_free(s->line_mask);
        free(s);
    }
}

/* The shape draws on a tile only where its masks have one */
static int drawn_on(irect tile, void *user)
{
    const live_shape *s = user;
    return snap_mask_has(s->fill_mask, tile) || snap_mask_has(s->line_mask, tile);
}

/* Put back what was under the shape in r */
static void restore(const live_shape *s, img *im, irect r)
{
    if (s->under)
        snap_restore(s->under, im, r);
    else
        raster_copy_rect(im, s->snapshot, r);
}

change *live_shape_change(const live_shape *s, const img *im)
{
    return s->under ? snap_change(s->under, im, s->touched) : change_patch(s->snapshot, im, s->touched);
}

int live_shape_is_line(const live_shape *s)
{
    return s->kind == SHAPE_LINE;
}

int live_shape_is_empty(const live_shape *s)
{
    return fabsf(s->b.x - s->a.x) < 1.0f && fabsf(s->b.y - s->a.y) < 1.0f;
}

frect live_shape_rect(const live_shape *s)
{
    return frect_from_two(s->a, s->b);
}

static int style_eq(shape_style x, shape_style y)
{
    return x.outline == y.outline && x.fill == y.fill && x.size == y.size && rgba_eq(x.line_color, y.line_color)
        && rgba_eq(x.fill_color, y.fill_color);
}

irect live_shape_render(live_shape *s, img *im, shape_style style)
{
    if (s->rendered && s->r_kind == s->kind && s->r_a.x == s->a.x && s->r_a.y == s->a.y && s->r_b.x == s->b.x
        && s->r_b.y == s->b.y && style_eq(s->r_style, style))
        return IRECT_EMPTY;
    int is_line = live_shape_is_line(s);
    int outline = style.outline || is_line;
    int fill = style.fill && !is_line;
    float radius = style.size * 0.5f;
    mask_clear(s->fill_mask, s->drawn);
    mask_clear(s->line_mask, s->drawn);
    irect r = IRECT_EMPTY;
    if (is_line)
        r = mask_capsule(s->line_mask, s->a, s->b, radius, 1.0f);
    else
    {
        pt pts[SHAPE_MAX_CONTOURS * SHAPE_MAX_POINTS];
        int counts[SHAPE_MAX_CONTOURS];
        int nc = shape_contours(s->kind, s->a, s->b, pts, counts);
        for (int c = 0; c < nc; c++)
        {
            const pt *p = pts + c * SHAPE_MAX_POINTS;
            if (fill)
                r = irect_union(r, mask_fill_polygon(s->fill_mask, p, counts[c], 1.0f));
            if (outline)
                r = irect_union(r, mask_stroke_path(s->line_mask, p, counts[c], 1, radius));
        }
    }
    irect region = irect_union(s->drawn, r);
    if (s->under)
    {
        snap_keep(s->under, im, r, s->fill_mask);
        snap_keep(s->under, im, r, s->line_mask);
    }
    restore(s, im, region);
    if (s->under)
        snap_drop(s->under, region, drawn_on, s);
    if (fill)
        raster_blend_mask(im, s->fill_mask, style.fill_color, 1.0f, r);
    if (outline)
        raster_blend_mask(im, s->line_mask, style.line_color, 1.0f, r);
    s->drawn = r;
    s->touched = irect_union(s->touched, region);
    s->rendered = 1;
    s->r_kind = s->kind;
    s->r_a = s->a;
    s->r_b = s->b;
    s->r_style = style;
    return region;
}

void live_shape_rebase(live_shape *s, const img *base, irect r)
{
    if (s->under)
        snap_rebase(s->under, base, r);
    else
        raster_copy_rect(s->snapshot, base, r);
    s->rendered = 0;
}

irect live_shape_erase(const live_shape *s, img *im)
{
    restore(s, im, s->touched);
    return s->touched;
}

int live_shape_handles(const live_shape *s, pt out[8])
{
    if (live_shape_is_line(s))
    {
        out[0] = s->a;
        out[1] = s->b;
        return 2;
    }
    frect r = live_shape_rect(s);
    float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, w = r.x1 - r.x0, h = r.y1 - r.y0;
    for (int i = 0; i < 8; i++)
        out[i] = pt_make(cx + (float)HANDLES[i][0] * w * 0.5f, cy + (float)HANDLES[i][1] * h * 0.5f);
    return 8;
}

int live_shape_handle_at(const live_shape *s, pt p, float tol)
{
    pt hs[8];
    int n = live_shape_handles(s, hs);
    for (int i = 0; i < n; i++)
        if (fmaxf(fabsf(p.x - hs[i].x), fabsf(p.y - hs[i].y)) <= tol)
            return i;
    return -1;
}

int live_shape_contains(const live_shape *s, pt p, float tol)
{
    if (live_shape_is_line(s))
    {
        float abx = s->b.x - s->a.x, aby = s->b.y - s->a.y, len2 = abx * abx + aby * aby;
        float t = 0.0f;
        if (len2 > 0.0f)
        {
            t = ((p.x - s->a.x) * abx + (p.y - s->a.y) * aby) / len2;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        }
        return hypotf(p.x - (s->a.x + abx * t), p.y - (s->a.y + aby * t)) <= tol;
    }
    frect r = live_shape_rect(s);
    return r.x0 - tol <= p.x && p.x <= r.x1 + tol && r.y0 - tol <= p.y && p.y <= r.y1 + tol;
}

void live_shape_drag_handle(live_shape *s, int h, pt a, pt b, pt p, int shift)
{
    if (live_shape_is_line(s))
    {
        if (h == 0)
            s->a = shape_constrain(SHAPE_LINE, b, p, shift);
        else
            s->b = shape_constrain(SHAPE_LINE, a, p, shift);
        return;
    }
    if (h < 0 || h >= 8)
        return;
    frect r = frect_from_two(a, b);
    int dx = HANDLES[h][0], dy = HANDLES[h][1];
    float x0 = r.x0, y0 = r.y0, x1 = r.x1, y1 = r.y1;
    if (dx != 0 && dy != 0 && shift)
    {
        float ox = dx < 0 ? x1 : x0, oy = dy < 0 ? y1 : y0;
        pt corner = shape_constrain(SHAPE_RECTANGLE, pt_make(ox, oy), p, 1);
        x0 = ox, y0 = oy, x1 = corner.x, y1 = corner.y;
    }
    else
    {
        if (dx < 0)
            x0 = p.x;
        else if (dx > 0)
            x1 = p.x;
        if (dy < 0)
            y0 = p.y;
        else if (dy > 0)
            y1 = p.y;
    }
    /* Dragging past the opposite edge just turns the box inside out. */
    s->a = pt_make(x0, y0);
    s->b = pt_make(x1, y1);
}

void live_shape_translate(live_shape *s, pt a, pt b, float dx, float dy)
{
    s->a = pt_make(a.x + dx, a.y + dy);
    s->b = pt_make(b.x + dx, b.y + dy);
}

int live_shape_handle_cursor(int is_line, int h)
{
    if (is_line)
        return SHAPE_CURSOR_CROSS;
    switch (h)
    {
    case 0: case 4: return SHAPE_CURSOR_NWSE;
    case 2: case 6: return SHAPE_CURSOR_NESW;
    case 1: case 5: return SHAPE_CURSOR_NS;
    default: return SHAPE_CURSOR_EW;
    }
}
