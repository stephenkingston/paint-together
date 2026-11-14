/*
 * Paint (C port) - drawing the window into 32-bit pixels
 *
 * Whole-pixel calls write rows directly. The anti-aliased calls build their
 * outline as polygon edges and fill it with one scanline rasteriser: each
 * pixel row is sampled on 16 sub-scanlines, the crossings on a sub-scanline
 * are walked in x with a non-zero winding count, and every span where the
 * count is not zero adds its exact horizontal coverage to a row of floats.
 * So overlapping pieces (a stroke is its segments' quads plus round joins)
 * union exactly instead of doubling up at the seams.
 *
 * Blending is "over" with straight alpha. The window's pixels are opaque,
 * which takes the quick path; icon tiles start transparent and take the
 * general one.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui/gfx.h"

#define SUB 16          /* sub-scanlines per pixel row */

/* ---- rectangles ---- */

rect rect_make(int x, int y, int w, int h)
{
    rect r = { x, y, w, h };
    return r;
}

rect rect_inset(rect r, int by)
{
    r.x += by;
    r.y += by;
    r.w -= 2 * by;
    r.h -= 2 * by;
    if (r.w < 0)
        r.w = 0;
    if (r.h < 0)
        r.h = 0;
    return r;
}

rect rect_cut(rect a, rect b)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    rect r = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return r;
}

rect rect_union(rect a, rect b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

int rect_has(rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

int rect_empty(rect r)
{
    return r.w <= 0 || r.h <= 0;
}

/* ---- surfaces ---- */

static uint32_t *row_at(surface *s, int x, int y)
{
    return s->px + (size_t)(y - s->oy) * (size_t)s->stride + (size_t)(x - s->ox);
}

surface *surface_at(rect r)
{
    surface *s = calloc(1, sizeof *s);
    surface_move(s, r);
    if (s->px)
        for (size_t i = 0; i < (size_t)s->w * (size_t)s->h; i++)
            s->px[i] = 0xFFFFFFFFu;
    return s;
}

void surface_move(surface *s, rect r)
{
    if (r.w < 1)
        r.w = 1;
    if (r.h < 1)
        r.h = 1;
    if (!s->px || (size_t)r.w * (size_t)r.h > (size_t)s->stride * (size_t)s->h)
    {
        free(s->px);
        s->px = malloc((size_t)r.w * (size_t)r.h * 4);
    }
    s->w = r.w;
    s->h = r.h;
    s->stride = r.w;
    s->ox = r.x;
    s->oy = r.y;
    s->clip = r;
}

surface *surface_new(int w, int h)
{
    return surface_at(rect_make(0, 0, w, h));
}

void surface_free(surface *s)
{
    if (s)
        free(s->px);
    free(s);
}

void surface_resize(surface *s, int w, int h)
{
    surface_move(s, rect_make(s->ox, s->oy, w, h));
}

rect gfx_clip(surface *dst, rect r)
{
    rect old = dst->clip;
    dst->clip = rect_cut(old, r);
    return old;
}

/* ---- colour ---- */

color color_mix(color a, color b, int t)
{
    int u = 256 - t;
    int ca = (C_ALPHA(a) * u + C_ALPHA(b) * t) >> 8;
    int r = (C_R(a) * u + C_R(b) * t) >> 8;
    int g = (C_G(a) * u + C_G(b) * t) >> 8;
    int bl = (C_B(a) * u + C_B(b) * t) >> 8;
    return (color)ca << 24 | (color)r << 16 | (color)g << 8 | (color)bl;
}

color color_with_alpha(color c, int alpha)
{
    return COLOR_A(c, alpha < 0 ? 0 : alpha > 255 ? 255 : alpha);
}

static inline int mix8(int d, int s, int a)
{
    int t = (s - d) * a + 128;
    return d + ((t + (t >> 8)) >> 8);
}

/* `c` (its alpha ignored) at alpha `a` (0..255) over the pixel d */
static inline uint32_t over(uint32_t d, color c, int a)
{
    if (a >= 255)
        return 0xFF000000u | (c & 0xFFFFFFu);
    if (a <= 0)
        return d;
    int da = (int)(d >> 24);
    if (da == 255)
        return 0xFF000000u | (uint32_t)mix8(C_R(d), C_R(c), a) << 16 | (uint32_t)mix8(C_G(d), C_G(c), a) << 8
               | (uint32_t)mix8(C_B(d), C_B(c), a);
    int k = da * (255 - a) / 255;
    int oa = a + k;
    int r = (C_R(c) * a + C_R(d) * k + oa / 2) / oa;
    int g = (C_G(c) * a + C_G(d) * k + oa / 2) / oa;
    int b = (C_B(c) * a + C_B(d) * k + oa / 2) / oa;
    return (uint32_t)oa << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static inline void put(surface *dst, int x, int y, color c, int a)
{
    uint32_t *p = row_at(dst, x, y);
    *p = over(*p, c, a);
}

/* ---- whole pixels ---- */

void gfx_fill(surface *dst, rect r, color c)
{
    r = rect_cut(r, dst->clip);
    int a = C_ALPHA(c);
    if (rect_empty(r) || a == 0)
        return;
    for (int y = r.y; y < r.y + r.h; y++)
    {
        uint32_t *p = row_at(dst, r.x, y);
        if (a == 255)
            for (int i = 0; i < r.w; i++)
                p[i] = c;
        else
            for (int i = 0; i < r.w; i++)
                p[i] = over(p[i], c, a);
    }
}

void gfx_frame(surface *dst, rect r, int width, color c)
{
    if (width * 2 >= r.w || width * 2 >= r.h)
    {
        gfx_fill(dst, r, c);
        return;
    }
    gfx_fill(dst, rect_make(r.x, r.y, r.w, width), c);
    gfx_fill(dst, rect_make(r.x, r.y + r.h - width, r.w, width), c);
    gfx_fill(dst, rect_make(r.x, r.y + width, width, r.h - 2 * width), c);
    gfx_fill(dst, rect_make(r.x + r.w - width, r.y + width, width, r.h - 2 * width), c);
}

void gfx_hline(surface *dst, int x0, int x1, int y, color c)
{
    if (x1 < x0)
    {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    gfx_fill(dst, rect_make(x0, y, x1 - x0 + 1, 1), c);
}

void gfx_vline(surface *dst, int x, int y0, int y1, color c)
{
    if (y1 < y0)
    {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    gfx_fill(dst, rect_make(x, y0, 1, y1 - y0 + 1), c);
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Coverage of pixel (i, j) by a w x h rectangle at (0,0) with corner radius rad */
static float round_cov(int i, int j, int w, int h, float rad)
{
    if (i < 0 || j < 0 || i >= w || j >= h)
        return 0;
    float px = i + 0.5f, py = j + 0.5f;
    float dx = rad - px > px - (w - rad) ? rad - px : px - (w - rad);
    float dy = rad - py > py - (h - rad) ? rad - py : py - (h - rad);
    if (dx <= 0 || dy <= 0)
        return 1;
    return clamp01(rad + 0.5f - sqrtf(dx * dx + dy * dy));
}

/* Rounded rectangle, filled with a colour per row (a gradient), or only its
 * border `width` wide when width > 0 */
static void round_rows(surface *dst, rect r, int rad, int width, color top, color bottom)
{
    if (rect_empty(r))
        return;
    int lim = (r.w < r.h ? r.w : r.h) / 2;
    if (rad > lim)
        rad = lim;
    if (rad < 0)
        rad = 0;
    rect v = rect_cut(r, dst->clip);
    int band = rad > width ? rad : width;
    int irad = rad - width > 0 ? rad - width : 0;
    for (int y = v.y; y < v.y + v.h; y++)
    {
        int j = y - r.y;
        color c = top == bottom || r.h < 2 ? top : color_mix(top, bottom, j * 256 / (r.h - 1));
        int a = C_ALPHA(c);
        int edge_row = j < band || j >= r.h - band;
        int x0 = v.x, x1 = v.x + v.w;
        uint32_t *p = row_at(dst, x0, y) - x0;
        if (!edge_row)
        {
            /* Straight sides: the corners don't reach this row */
            if (width > 0)
            {
                for (int x = x0; x < x1; x++)
                    if (x - r.x < width || x - r.x >= r.w - width)
                        p[x] = over(p[x], c, a);
                continue;
            }
            for (int x = x0; x < x1; x++)
                p[x] = over(p[x], c, a);
            continue;
        }
        for (int x = x0; x < x1; x++)
        {
            int i = x - r.x;
            float cov = round_cov(i, j, r.w, r.h, (float)rad);
            if (width > 0)
                cov -= round_cov(i - width, j - width, r.w - 2 * width, r.h - 2 * width, (float)irad);
            if (cov > 0)
                p[x] = over(p[x], c, (int)(cov * a + 0.5f));
        }
    }
}

void gfx_round(surface *dst, rect r, int radius, color fill)
{
    round_rows(dst, r, radius, 0, fill, fill);
}

void gfx_round_frame(surface *dst, rect r, int radius, int width, color c)
{
    round_rows(dst, r, radius, width < 1 ? 1 : width, c, c);
}

void gfx_round_both(surface *dst, rect r, int radius, color fill, color border)
{
    gfx_round(dst, rect_inset(r, 1), radius > 1 ? radius - 1 : 0, fill);
    gfx_round_frame(dst, r, radius, 1, border);
}

void gfx_vgradient(surface *dst, rect r, int radius, color top, color bottom)
{
    round_rows(dst, r, radius, 0, top, bottom);
}

void gfx_hgradient(surface *dst, rect r, color left, color right)
{
    rect v = rect_cut(r, dst->clip);
    for (int x = v.x; x < v.x + v.w; x++)
    {
        color c = r.w < 2 ? left : color_mix(left, right, (x - r.x) * 256 / (r.w - 1));
        gfx_fill(dst, rect_make(x, v.y, 1, v.h), c);
    }
}

void gfx_dashed_frame(surface *dst, rect r, int dash, int phase, color a, color b)
{
    if (rect_empty(r) || dash < 1)
        return;
    /* Walk the border clockwise from the top-left corner */
    int len = 2 * (r.w + r.h) - 4, x = r.x, y = r.y;
    if (r.w == 1 || r.h == 1)
        len = r.w * r.h;
    for (int s = 0; s < len; s++)
    {
        int k = ((s + phase) % (2 * dash) + 2 * dash) % (2 * dash);
        if (rect_has(dst->clip, x, y))
        {
            color c = k < dash ? a : b;
            put(dst, x, y, c, C_ALPHA(c));
        }
        if (y == r.y && x < r.x + r.w - 1)
            x++;
        else if (x == r.x + r.w - 1 && y < r.y + r.h - 1)
            y++;
        else if (y == r.y + r.h - 1 && x > r.x)
            x--;
        else
            y--;
    }
}

void gfx_checker(surface *dst, rect r, int cell, color a, color b)
{
    rect v = rect_cut(r, dst->clip);
    if (cell < 1)
        cell = 1;
    for (int y = v.y; y < v.y + v.h; y++)
    {
        uint32_t *p = row_at(dst, v.x, y) - v.x;
        int ry = (y - r.y) / cell;
        for (int x = v.x; x < v.x + v.w; x++)
        {
            color c = (((x - r.x) / cell + ry) & 1) ? b : a;
            p[x] = over(p[x], c, C_ALPHA(c));
        }
    }
}

/* ---- the rasteriser ---- */

typedef struct
{
    float ya, yb, x, dxdy;
    int dir;
} edge;

typedef struct
{
    float x;
    int dir;
} crossing;

static edge *edges;
static int n_edges, cap_edges;
static int *active;
static crossing *cross;
static int cap_active;
static float *acc;
static int cap_acc;
static float bx0, by0, bx1, by1;
static float *pts;      /* scratch outline points */
static int cap_pts;

static void path_begin(void)
{
    n_edges = 0;
    bx0 = by0 = 1e30f;
    bx1 = by1 = -1e30f;
}

static void edge_add(float x0, float y0, float x1, float y1)
{
    if (!(y0 != y1) || !isfinite(x0 + x1 + y0 + y1))
        return;
    int dir = 1;
    if (y0 > y1)
    {
        float t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
        dir = -1;
    }
    if (n_edges == cap_edges)
    {
        cap_edges = cap_edges ? cap_edges * 2 : 256;
        edges = realloc(edges, (size_t)cap_edges * sizeof *edges);
    }
    edge e = { y0, y1, x0, (x1 - x0) / (y1 - y0), dir };
    edges[n_edges++] = e;
    if (x0 < bx0)
        bx0 = x0;
    if (x1 < bx0)
        bx0 = x1;
    if (x0 > bx1)
        bx1 = x0;
    if (x1 > bx1)
        bx1 = x1;
    if (y0 < by0)
        by0 = y0;
    if (y1 > by1)
        by1 = y1;
}

static void contour(const float *xy, int n)
{
    for (int i = 0; i < n; i++)
    {
        int j = i + 1 == n ? 0 : i + 1;
        edge_add(xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1]);
    }
}

/* A contour turned to positive orientation, so the pieces of a stroke add up */
static void contour_pos(const float *xy, int n)
{
    float area = 0;
    for (int i = 0; i < n; i++)
    {
        int j = i + 1 == n ? 0 : i + 1;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    if (area >= 0)
    {
        contour(xy, n);
        return;
    }
    for (int i = 0; i < n; i++)
    {
        int j = i + 1 == n ? 0 : i + 1;
        edge_add(xy[2 * j], xy[2 * j + 1], xy[2 * i], xy[2 * i + 1]);
    }
}

static int by_top(const void *a, const void *b)
{
    float d = ((const edge *)a)->ya - ((const edge *)b)->ya;
    return d < 0 ? -1 : d > 0;
}

static void add_span(float *row, float xa, float xb, float w)
{
    int ia = (int)xa, ib = (int)xb;
    if (ia == ib)
    {
        row[ia] += (xb - xa) * w;
        return;
    }
    row[ia] += (ia + 1 - xa) * w;
    for (int i = ia + 1; i < ib; i++)
        row[i] += w;
    row[ib] += (xb - ib) * w;
}

static void path_fill(surface *dst, color c, gfx_shader shade, void *user)
{
    rect clip = dst->clip;
    if (n_edges == 0)
        return;
    int x0 = (int)floorf(bx0), x1 = (int)ceilf(bx1), y0 = (int)floorf(by0), y1 = (int)ceilf(by1);
    if (x0 < clip.x)
        x0 = clip.x;
    if (y0 < clip.y)
        y0 = clip.y;
    if (x1 > clip.x + clip.w)
        x1 = clip.x + clip.w;
    if (y1 > clip.y + clip.h)
        y1 = clip.y + clip.h;
    if (x1 <= x0 || y1 <= y0)
        return;
    int bw = x1 - x0;
    if (bw + 2 > cap_acc)
    {
        cap_acc = bw + 2;
        acc = realloc(acc, (size_t)cap_acc * sizeof *acc);
    }
    memset(acc, 0, (size_t)(bw + 2) * sizeof *acc);
    if (n_edges > cap_active)
    {
        cap_active = n_edges;
        active = realloc(active, (size_t)cap_active * sizeof *active);
        cross = realloc(cross, (size_t)cap_active * sizeof *cross);
    }
    qsort(edges, (size_t)n_edges, sizeof *edges, by_top);
    int next = 0, n_active = 0;
    int alpha = C_ALPHA(c);
    for (int y = y0; y < y1; y++)
    {
        int lo = bw, hi = 0;
        for (int s = 0; s < SUB; s++)
        {
            float sy = y + (s + 0.5f) / SUB;
            while (next < n_edges && edges[next].ya <= sy)
                active[n_active++] = next++;
            int n = 0;
            for (int i = 0; i < n_active;)
            {
                edge *e = &edges[active[i]];
                if (e->yb <= sy)
                {
                    active[i] = active[--n_active];
                    continue;
                }
                crossing k = { e->x + (sy - e->ya) * e->dxdy, e->dir };
                int at = n++;
                while (at > 0 && cross[at - 1].x > k.x)
                {
                    cross[at] = cross[at - 1];
                    at--;
                }
                cross[at] = k;
                i++;
            }
            int wind = 0;
            float start = 0;
            for (int i = 0; i < n; i++)
            {
                int was = wind;
                wind += cross[i].dir;
                if (was == 0 && wind != 0)
                    start = cross[i].x;
                else if (was != 0 && wind == 0)
                {
                    float a = start - x0, b = cross[i].x - x0;
                    if (a < 0)
                        a = 0;
                    if (b > bw)
                        b = (float)bw;
                    if (b > a)
                    {
                        add_span(acc, a, b, 1.0f / SUB);
                        if ((int)a < lo)
                            lo = (int)a;
                        if ((int)b + 1 > hi)
                            hi = (int)b + 1;
                    }
                }
            }
        }
        if (hi > bw)
            hi = bw;
        uint32_t *p = row_at(dst, x0, y);
        for (int i = lo; i < hi; i++)
        {
            float v = acc[i];
            acc[i] = 0;
            if (v < 0.002f)
                continue;
            color cc = shade ? shade(x0 + i + 0.5f, y + 0.5f, user) : c;
            int a = shade ? C_ALPHA(cc) : alpha;
            p[i] = over(p[i], cc, v >= 0.998f ? a : (int)(v * a + 0.5f));
        }
        acc[bw] = acc[bw + 1] = 0;
    }
}

static float *pts_room(int n)
{
    if (n * 2 > cap_pts)
    {
        cap_pts = n * 2;
        pts = realloc(pts, (size_t)cap_pts * sizeof *pts);
    }
    return pts;
}

static int circle_steps(float r)
{
    if (r <= 0.25f)
        return 8;
    int n = (int)ceilf(3.14159265f / acosf(1 - 0.08f / r));
    return n < 8 ? 8 : n > 256 ? 256 : n;
}

/* The radius of an n-gon with the same area as the circle of radius r */
static float equal_area(float r, int n)
{
    float a = 6.2831853f / n;
    return r * sqrtf(a / sinf(a));
}

/* Ellipse outline; reverse for a hole */
static void ellipse_contour(float cx, float cy, float rx, float ry, int reverse)
{
    int n = circle_steps(rx > ry ? rx : ry);
    float *p = pts_room(n);
    float k = equal_area(1, n);
    rx *= k;
    ry *= k;
    for (int i = 0; i < n; i++)
    {
        float t = (reverse ? -i : i) * 6.2831853f / n;
        p[2 * i] = cx + rx * cosf(t);
        p[2 * i + 1] = cy + ry * sinf(t);
    }
    contour(p, n);
}

static void rrect_contour(float x, float y, float w, float h, float rad, int reverse)
{
    if (w <= 0 || h <= 0)
        return;
    float lim = (w < h ? w : h) / 2;
    if (rad > lim)
        rad = lim;
    int k = rad > 0.01f ? circle_steps(rad) / 4 + 1 : 1;
    float *p = pts_room(4 * k);
    float cx[4] = { x + w - rad, x + w - rad, x + rad, x + rad }, cy[4] = { y + rad, y + h - rad, y + h - rad, y + rad };
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < k; i++)
        {
            float t = (c - 1) * 1.5707963f + (k > 1 ? i * 1.5707963f / (k - 1) : 0.7853982f);
            float r = k > 1 ? rad : 0;
            p[2 * n] = cx[c] + r * cosf(t);
            p[2 * n + 1] = cy[c] + r * sinf(t);
            n++;
        }
    if (k == 1)     /* a plain rectangle: put the corners exactly */
    {
        float q[8] = { x + w, y, x + w, y + h, x, y + h, x, y };
        memcpy(p, q, sizeof q);
    }
    if (!reverse)
    {
        contour(p, n);
        return;
    }
    for (int i = 0; i < n; i++)
    {
        int j = i + 1 == n ? 0 : i + 1;
        edge_add(p[2 * j], p[2 * j + 1], p[2 * i], p[2 * i + 1]);
    }
}

/* ---- anti-aliased vectors ---- */

void gfx_polygon(surface *dst, const float *xy, int n, color c)
{
    if (n < 3)
        return;
    path_begin();
    contour(xy, n);
    path_fill(dst, c, NULL, NULL);
}

void gfx_polygons(surface *dst, const float *xy, const int *counts, int contours, color c)
{
    path_begin();
    for (int i = 0; i < contours; i++)
    {
        contour(xy, counts[i]);
        xy += 2 * counts[i];
    }
    path_fill(dst, c, NULL, NULL);
}

void gfx_polygons_shaded(surface *dst, const float *xy, const int *counts, int contours, gfx_shader shade, void *user)
{
    path_begin();
    for (int i = 0; i < contours; i++)
    {
        contour(xy, counts[i]);
        xy += 2 * counts[i];
    }
    path_fill(dst, 0, shade, user);
}

static void disc_piece(float cx, float cy, float r)
{
    int n = circle_steps(r);
    float p[2 * 256];
    r = equal_area(r, n);
    for (int i = 0; i < n; i++)
    {
        float t = i * 6.2831853f / n;
        p[2 * i] = cx + r * cosf(t);
        p[2 * i + 1] = cy + r * sinf(t);
    }
    contour(p, n);
}

void gfx_stroke(surface *dst, const float *xy, int n, float width, color c, int flags)
{
    float hw = width / 2;
    if (n < 1 || !(hw > 0))
        return;
    int closed = (flags & GFX_CLOSED) && n >= 3;
    path_begin();
    int segs = closed ? n : n - 1, drew = 0;
    for (int i = 0; i < segs; i++)
    {
        int j = (i + 1) % n;
        float dx = xy[2 * j] - xy[2 * i], dy = xy[2 * j + 1] - xy[2 * i + 1];
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f)
            continue;
        float nx = -dy / len * hw, ny = dx / len * hw;
        float q[8] = { xy[2 * i] + nx, xy[2 * i + 1] + ny, xy[2 * j] + nx, xy[2 * j + 1] + ny,
                       xy[2 * j] - nx, xy[2 * j + 1] - ny, xy[2 * i] - nx, xy[2 * i + 1] - ny };
        contour_pos(q, 4);
        drew = 1;
    }
    /* Joins (every point of a closed line), then the two ends */
    for (int v = closed ? 0 : 1; v < (closed ? n : n - 1); v++)
    {
        float px = xy[2 * v], py = xy[2 * v + 1];
        if (!(flags & GFX_MITER))
        {
            disc_piece(px, py, hw);
            continue;
        }
        /* The segments either side, skipping repeated points */
        int a = v, b = v;
        do
            a = (a + n - 1) % n;
        while (a != v && xy[2 * a] == px && xy[2 * a + 1] == py && (closed || a > 0));
        do
            b = (b + 1) % n;
        while (b != v && xy[2 * b] == px && xy[2 * b + 1] == py && (closed || b < n - 1));
        float d0x = px - xy[2 * a], d0y = py - xy[2 * a + 1], d1x = xy[2 * b] - px, d1y = xy[2 * b + 1] - py;
        float l0 = sqrtf(d0x * d0x + d0y * d0y), l1 = sqrtf(d1x * d1x + d1y * d1y);
        if (l0 < 1e-6f || l1 < 1e-6f)
            continue;
        d0x /= l0, d0y /= l0, d1x /= l1, d1y /= l1;
        float cr = d0x * d1y - d0y * d1x;
        if (fabsf(cr) < 1e-6f)
            continue;
        float sg = cr > 0 ? -hw : hw;
        float n0x = -d0y * sg, n0y = d0x * sg, n1x = -d1y * sg, n1y = d1x * sg;
        float mx = n0x + n1x, my = n0y + n1y, ml = mx * mx + my * my;
        if (ml > 1e-12f && 2 * hw * hw / ml * sqrtf(ml) <= 4 * hw)
        {
            float k = 2 * hw * hw / ml;
            float q[8] = { px, py, px + n0x, py + n0y, px + mx * k, py + my * k, px + n1x, py + n1y };
            contour_pos(q, 4);
        }
        else
        {
            float q[6] = { px, py, px + n0x, py + n0y, px + n1x, py + n1y };
            contour_pos(q, 3);
        }
    }
    if ((!closed && !(flags & GFX_BUTT)) || !drew)
    {
        disc_piece(xy[0], xy[1], hw);
        if (n > 1)
            disc_piece(xy[2 * n - 2], xy[2 * n - 1], hw);
    }
    path_fill(dst, c, NULL, NULL);
}

void gfx_line(surface *dst, float x0, float y0, float x1, float y1, float width, color c)
{
    float xy[4] = { x0, y0, x1, y1 };
    gfx_stroke(dst, xy, 2, width, c, 0);
}

void gfx_polyline(surface *dst, const float *xy, int n, float width, color c, int closed)
{
    gfx_stroke(dst, xy, n, width, c, closed ? GFX_CLOSED : 0);
}

void gfx_circle(surface *dst, float cx, float cy, float r, color c)
{
    gfx_ellipse(dst, cx, cy, r, r, c);
}

void gfx_ring(surface *dst, float cx, float cy, float r, float width, color c)
{
    gfx_ellipse_ring(dst, cx, cy, r, r, width, c);
}

void gfx_ellipse(surface *dst, float cx, float cy, float rx, float ry, color c)
{
    if (!(rx > 0 && ry > 0))
        return;
    path_begin();
    ellipse_contour(cx, cy, rx, ry, 0);
    path_fill(dst, c, NULL, NULL);
}

void gfx_ellipse_ring(surface *dst, float cx, float cy, float rx, float ry, float width, color c)
{
    float h = width / 2;
    if (!(rx + h > 0 && ry + h > 0))
        return;
    path_begin();
    ellipse_contour(cx, cy, rx + h, ry + h, 0);
    if (rx - h > 0 && ry - h > 0)
        ellipse_contour(cx, cy, rx - h, ry - h, 1);
    path_fill(dst, c, NULL, NULL);
}

void gfx_rrect(surface *dst, float x, float y, float w, float h, float radius, color c)
{
    path_begin();
    rrect_contour(x, y, w, h, radius, 0);
    path_fill(dst, c, NULL, NULL);
}

void gfx_rrect_ring(surface *dst, float x, float y, float w, float h, float radius, float width, color c)
{
    path_begin();
    rrect_contour(x, y, w, h, radius, 0);
    rrect_contour(x + width, y + width, w - 2 * width, h - 2 * width, radius - width > 0 ? radius - width : 0, 1);
    path_fill(dst, c, NULL, NULL);
}

/* Fraction of a soft edge reached at distance t inside it (smoothstep over the blur) */
static float soft(float t, float blur)
{
    if (blur <= 0)
        return t >= 0 ? 1.0f : 0.0f;
    float u = clamp01(t / blur + 0.5f);
    return u * u * (3 - 2 * u);
}

void gfx_shadow(surface *dst, rect r, int dx, int dy, int blur, color c)
{
    rect s = rect_make(r.x + dx, r.y + dy, r.w, r.h);
    int reach = (blur + 1) / 2;
    rect v = rect_cut(rect_inset(s, -reach), dst->clip);
    if (rect_empty(v))
        return;
    int a = C_ALPHA(c);
    for (int y = v.y; y < v.y + v.h; y++)
    {
        float fy = soft(y + 0.5f - s.y, (float)blur) * soft(s.y + s.h - (y + 0.5f), (float)blur);
        uint32_t *p = row_at(dst, v.x, y) - v.x;
        for (int x = v.x; x < v.x + v.w; x++)
        {
            if (rect_has(r, x, y))
            {
                x = r.x + r.w - 1;
                continue;
            }
            float f = fy * soft(x + 0.5f - s.x, (float)blur) * soft(s.x + s.w - (x + 0.5f), (float)blur);
            int k = (int)(f * a + 0.5f);
            if (k > 0)
                p[x] = over(p[x], c, k);
        }
    }
}

void gfx_copy(surface *dst, const surface *src, rect r)
{
    r = rect_cut(rect_cut(r, rect_make(src->ox, src->oy, src->w, src->h)), dst->clip);
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(row_at(dst, r.x, y), src->px + (size_t)(y - src->oy) * (size_t)src->stride + (size_t)(r.x - src->ox), (size_t)r.w * 4);
}

/* ---- pictures ---- */

void gfx_mask(surface *dst, int x, int y, const uint8_t *alpha, int w, int h, int pitch, color c)
{
    rect v = rect_cut(rect_make(x, y, w, h), dst->clip);
    int ca = C_ALPHA(c);
    for (int yy = v.y; yy < v.y + v.h; yy++)
    {
        const uint8_t *m = alpha + (size_t)(yy - y) * (size_t)pitch + (v.x - x);
        uint32_t *p = row_at(dst, v.x, yy);
        for (int i = 0; i < v.w; i++)
            if (m[i])
                p[i] = over(p[i], c, ca == 255 ? m[i] : m[i] * ca / 255);
    }
}

void gfx_blend(surface *dst, int x, int y, const uint32_t *src, int w, int h, int stride)
{
    rect v = rect_cut(rect_make(x, y, w, h), dst->clip);
    for (int yy = v.y; yy < v.y + v.h; yy++)
    {
        const uint32_t *s = src + (size_t)(yy - y) * (size_t)stride + (v.x - x);
        uint32_t *p = row_at(dst, v.x, yy);
        for (int i = 0; i < v.w; i++)
            p[i] = over(p[i], s[i], (int)(s[i] >> 24));
    }
}

void gfx_img(surface *dst, int x, int y, const img *im)
{
    rect v = rect_cut(rect_make(x, y, im->w, im->h), dst->clip);
    for (int yy = v.y; yy < v.y + v.h; yy++)
    {
        const uint8_t *s = img_at(im, v.x - x, yy - y);
        uint32_t *p = row_at(dst, v.x, yy);
        for (int i = 0; i < v.w; i++, s += 4)
            p[i] = over(p[i], RGB3(s[0], s[1], s[2]), s[3]);
    }
}

/* ---- the canvas view ---- */

static int checker_cell = 8;
static int *cols;       /* per screen column: first source column (and count, stride when zoomed out) */
static int cap_cols;

void gfx_canvas_checker(int cell)
{
    checker_cell = cell < 1 ? 1 : cell;
}

static inline int checker_at(int x, int y, int ax, int ay)
{
    /* Floor division, so the squares stay put left of and above the picture's corner too */
    int cx = x - ax, cy = y - ay;
    cx = cx >= 0 ? cx / checker_cell : -((-cx + checker_cell - 1) / checker_cell);
    cy = cy >= 0 ? cy / checker_cell : -((-cy + checker_cell - 1) / checker_cell);
    return ((cx + cy) & 1) ? 204 : 255;
}

/* Source range [first, first + count) of screen pixel i, and up to four samples in it */
static void source_span(double s, double zoom, int i, int limit, int *first, int *count)
{
    int a = (int)floor(s + i / zoom), b = (int)floor(s + (i + 1) / zoom);
    if (b <= a)
        b = a + 1;
    if (a < 0)
        a = 0;
    if (b > limit)
        b = limit;
    *first = a;
    *count = b - a;
}

void gfx_canvas(surface *dst, rect to, const img *im, double sx, double sy, double zoom)
{
    rect v = rect_cut(to, dst->clip);
    if (rect_empty(v) || im->w < 1 || im->h < 1 || !(zoom > 0))
        return;
    int ax = (int)floor(to.x - sx * zoom + 0.5), ay = (int)floor(to.y - sy * zoom + 0.5);
    if (2 * v.w > cap_cols)
    {
        cap_cols = 2 * v.w;
        cols = realloc(cols, (size_t)cap_cols * sizeof *cols);
    }
    if (zoom >= 1)
    {
        /* Nearest neighbour: each screen column's source column once, and a row
         * whose source row and checker band match the one above is copied */
        for (int i = 0; i < v.w; i++)
        {
            int c = (int)floor(sx + (v.x - to.x + i + 0.5) / zoom);
            cols[i] = c < 0 || c >= im->w ? -1 : c;
        }
        int prev_row = -1, prev_band = 0;
        for (int y = v.y; y < v.y + v.h; y++)
        {
            int sr = (int)floor(sy + (y - to.y + 0.5) / zoom);
            if (sr < 0 || sr >= im->h)
                continue;
            int band = (y - ay) / checker_cell;
            uint32_t *p = row_at(dst, v.x, y);
            if (sr == prev_row && band == prev_band && y > v.y)
            {
                memcpy(p, p - dst->stride, (size_t)v.w * 4);
                continue;
            }
            prev_row = sr;
            prev_band = band;
            const uint8_t *row = im->px + (size_t)sr * (size_t)im->w * 4;
            for (int i = 0; i < v.w; i++)
            {
                if (cols[i] < 0)
                    continue;
                const uint8_t *s = row + (size_t)cols[i] * 4;
                if (s[3] == 255)
                    p[i] = 0xFF000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
                else
                {
                    int k = checker_at(v.x + i, y, ax, ay);
                    p[i] = 0xFF000000u | (uint32_t)mix8(k, s[0], s[3]) << 16 | (uint32_t)mix8(k, s[1], s[3]) << 8
                           | (uint32_t)mix8(k, s[2], s[3]);
                }
            }
        }
        return;
    }
    /* Zoomed out: the average of the source pixels under each screen pixel
     * (premultiplied, so transparent ones don't darken it), at most 4 x 4
     * samples spread evenly over a big footprint */
    for (int i = 0; i < v.w; i++)
        source_span(sx, zoom, v.x - to.x + i, im->w, &cols[2 * i], &cols[2 * i + 1]);
    for (int y = v.y; y < v.y + v.h; y++)
    {
        int r0, rn;
        source_span(sy, zoom, y - to.y, im->h, &r0, &rn);
        if (rn <= 0)
            continue;
        int ry = rn > 4 ? 4 : rn;
        uint32_t *p = row_at(dst, v.x, y);
        for (int i = 0; i < v.w; i++)
        {
            int c0 = cols[2 * i], cn = cols[2 * i + 1];
            if (cn <= 0)
                continue;
            int cx = cn > 4 ? 4 : cn;
            unsigned sr = 0, sg = 0, sb = 0, sa = 0, n = 0;
            for (int j = 0; j < ry; j++)
            {
                const uint8_t *row = im->px + (size_t)(r0 + j * rn / ry) * (size_t)im->w * 4;
                for (int k = 0; k < cx; k++)
                {
                    const uint8_t *s = row + (size_t)(c0 + k * cn / cx) * 4;
                    sr += s[0] * s[3];
                    sg += s[1] * s[3];
                    sb += s[2] * s[3];
                    sa += s[3];
                    n++;
                }
            }
            unsigned full = 255 * n, bg = (unsigned)checker_at(v.x + i, y, ax, ay) * (full - sa);
            p[i] = 0xFF000000u | (sr + bg + full / 2) / full << 16 | (sg + bg + full / 2) / full << 8 | (sb + bg + full / 2) / full;
        }
    }
}
