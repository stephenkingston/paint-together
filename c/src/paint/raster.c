/*
 * Paint (C port) - coverage masks, compositing and fills (Rust: raster.rs)
 *
 * The maths follow the Rust line by line (f32 throughout, round-half-away
 * rounding, saturating float-to-int casts), so strokes and fills come out
 * with the same pixels.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "paint/raster.h"

/* Rust's `as i32` on a float: truncate, saturate, NaN is 0 */
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

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

frect frect_from_two(pt a, pt b)
{
    frect r = { fminf(a.x, b.x), fminf(a.y, b.y), fmaxf(a.x, b.x), fmaxf(a.y, b.y) };
    return r;
}

/* ---- masks ---- */

mask *mask_new(int w, int h)
{
    mask *m = malloc(sizeof *m);
    if (!m)
        return NULL;
    m->w = w > 0 ? w : 0;
    m->h = h > 0 ? h : 0;
    m->tw = (m->w + MASK_TILE - 1) >> MASK_TILE_SHIFT;
    m->th = (m->h + MASK_TILE - 1) >> MASK_TILE_SHIFT;
    m->tiles = calloc((size_t)m->tw * m->th + 1, sizeof(float *));
    if (!m->tiles)
    {
        free(m);
        return NULL;
    }
    return m;
}

void mask_reset(mask *m)
{
    for (int i = 0; i < m->tw * m->th; i++)
    {
        free(m->tiles[i]);
        m->tiles[i] = NULL;
    }
}

void mask_free(mask *m)
{
    if (m)
    {
        mask_reset(m);
        free(m->tiles);
        free(m);
    }
}

float *mask_tile_alloc(mask *m, int tile)
{
    m->tiles[tile] = calloc(MASK_TILE * MASK_TILE, sizeof(float));
    return m->tiles[tile];
}

void mask_clear(mask *m, irect r)
{
    r = irect_intersect(r, mask_bounds(m));
    if (irect_empty(r))
        return;
    for (int ty = r.y0 >> MASK_TILE_SHIFT; ty <= (r.y1 - 1) >> MASK_TILE_SHIFT; ty++)
    {
        for (int tx = r.x0 >> MASK_TILE_SHIFT; tx <= (r.x1 - 1) >> MASK_TILE_SHIFT; tx++)
        {
            float **t = &m->tiles[ty * m->tw + tx];
            if (!*t)
                continue;
            irect tr = irect_make(tx << MASK_TILE_SHIFT, ty << MASK_TILE_SHIFT, (tx + 1) << MASK_TILE_SHIFT, (ty + 1) << MASK_TILE_SHIFT);
            irect c = irect_intersect(tr, r);
            if (irect_contains_rect(c, irect_intersect(tr, mask_bounds(m))))
            {
                free(*t);
                *t = NULL;
                continue;
            }
            for (int y = c.y0; y < c.y1; y++)
                memset(*t + ((y - tr.y0) << MASK_TILE_SHIFT) + (c.x0 - tr.x0), 0, (size_t)(c.x1 - c.x0) * sizeof(float));
            /* A tile left all zero goes too (a moving shape's outline would otherwise leave a trail of them) */
            int i = 0;
            while (i < MASK_TILE * MASK_TILE && (*t)[i] == 0.0f)
                i++;
            if (i == MASK_TILE * MASK_TILE)
            {
                free(*t);
                *t = NULL;
            }
        }
    }
}

irect mask_sweep(mask *m, pt a, pt b, float reach, mask_sweep_fn f, void *user)
{
    irect r = irect_intersect(irect_around(a.x, a.y, b.x, b.y, reach + 1.0f), mask_bounds(m));
    if (irect_empty(r))
        return r;
    float abx = b.x - a.x, aby = b.y - a.y;
    float len2 = abx * abx + aby * aby;
    float nx = 0.0f, ny = 0.0f;
    if (len2 > 0.0f)
    {
        float len = sqrtf(len2);
        nx = -aby / len;
        ny = abx / len;
    }
    for (int y = r.y0; y < r.y1; y++)
    {
        for (int x = r.x0; x < r.x1; x++)
        {
            float apx = (float)x + 0.5f - a.x, apy = (float)y + 0.5f - a.y;
            float t = len2 > 0.0f ? clampf((apx * abx + apy * aby) / len2, 0.0f, 1.0f) : 0.0f;
            float d = hypotf(apx - abx * t, apy - aby * t);
            if (d > reach + 1.0f)
                continue;
            float v = f(x, y, d, apx * nx + apy * ny, user);
            if (v > 0.0f)
                mask_max_at(m, x, y, v);
        }
    }
    return r;
}

typedef struct
{
    float radius, weight;
} capsule_args;

static float capsule_cover(int x, int y, float d, float perp, void *user)
{
    const capsule_args *c = user;
    return clampf(c->radius + 0.5f - d, 0.0f, 1.0f) * c->weight;
}

irect mask_capsule(mask *m, pt a, pt b, float radius, float weight)
{
    capsule_args c = { radius, weight };
    return mask_sweep(m, a, b, radius, capsule_cover, &c);
}

irect mask_square_line(mask *m, pt a, pt b, int size)
{
    if (size < 1)
        size = 1;
    float dx = b.x - a.x, dy = b.y - a.y;
    int steps = f2i(fmaxf(ceilf(fmaxf(fabsf(dx), fabsf(dy))), 1.0f));
    irect out = IRECT_EMPTY;
    float half = (float)size * 0.5f;
    for (int i = 0; i <= steps; i++)
    {
        float k = (float)i / (float)steps;
        float px = a.x + dx * k, py = a.y + dy * k;
        int x0 = f2i(roundf(px - half)), y0 = f2i(roundf(py - half));
        for (int y = y0; y < y0 + size; y++)
            for (int x = x0; x < x0 + size; x++)
                mask_max_at(m, x, y, 1.0f);
        out = irect_union(out, irect_make(x0, y0, x0 + size, y0 + size));
    }
    return irect_intersect(out, mask_bounds(m));
}

static void add_span(float *acc, size_t len, float xa, float xb, float w)
{
    size_t ia = (size_t)floorf(xa);
    size_t ib = (size_t)floorf(xb);
    if (ia == ib)
    {
        acc[ia] += (xb - xa) * w;
        return;
    }
    acc[ia] += ((float)ia + 1.0f - xa) * w;
    for (size_t i = ia + 1; i < ib; i++)
        acc[i] += w;
    if (ib < len)
        acc[ib] += (xb - (float)ib) * w;
}

static int cmp_float(const void *pa, const void *pb)
{
    float a = *(const float *)pa, b = *(const float *)pb;
    return a < b ? -1 : a > b ? 1 : 0;
}

irect mask_fill_polygon(mask *m, const pt *pts, int n, float weight)
{
    if (n < 3)
        return IRECT_EMPTY;
    pt lo = pts[0], hi = pts[0];
    for (int i = 0; i < n; i++)
    {
        lo.x = fminf(lo.x, pts[i].x);
        lo.y = fminf(lo.y, pts[i].y);
        hi.x = fmaxf(hi.x, pts[i].x);
        hi.y = fmaxf(hi.y, pts[i].y);
    }
    irect r = irect_intersect(irect_around(lo.x, lo.y, hi.x, hi.y, 0.0f), mask_bounds(m));
    if (irect_empty(r))
        return r;
    enum { SUB = 4 };
    size_t bw = (size_t)irect_w(r);
    float stack_acc[256 + 2], stack_xs[64];
    float *acc = bw + 2 <= 258 ? stack_acc : malloc((bw + 2) * sizeof(float));
    float *xs = n <= 64 ? stack_xs : malloc((size_t)n * sizeof(float));
    for (int y = r.y0; y < r.y1; y++)
    {
        memset(acc, 0, (bw + 2) * sizeof(float));
        for (int s = 0; s < SUB; s++)
        {
            float sy = (float)y + ((float)s + 0.5f) / (float)SUB;
            int nx = 0;
            for (int i = 0; i < n; i++)
            {
                pt p = pts[i], q = pts[(i + 1) % n];
                if ((p.y <= sy) != (q.y <= sy))
                    xs[nx++] = p.x + (sy - p.y) * (q.x - p.x) / (q.y - p.y);
            }
            qsort(xs, (size_t)nx, sizeof(float), cmp_float);
            for (int i = 0; i + 1 < nx; i += 2)
            {
                float xa = fmaxf(xs[i] - (float)r.x0, 0.0f);
                float xb = fminf(xs[i + 1] - (float)r.x0, (float)bw);
                if (xb > xa)
                    add_span(acc, bw + 2, xa, xb, 1.0f / (float)SUB);
            }
        }
        for (size_t i = 0; i < bw; i++)
            if (acc[i] > 0.0f)
                mask_max_at(m, r.x0 + (int)i, y, fminf(acc[i], 1.0f) * weight);
    }
    if (acc != stack_acc)
        free(acc);
    if (xs != stack_xs)
        free(xs);
    return r;
}

irect mask_stroke_path(mask *m, const pt *pts, int n, int closed, float radius)
{
    irect r = IRECT_EMPTY;
    if (n == 1)
        return mask_capsule(m, pts[0], pts[0], radius, 1.0f);
    int segs = closed ? n : (n > 0 ? n - 1 : 0);
    for (int i = 0; i < segs; i++)
        r = irect_union(r, mask_capsule(m, pts[i], pts[(i + 1) % n], radius, 1.0f));
    return r;
}

/* ---- pixels ---- */

rgba rgba_over(rgba dst, rgba c, float a)
{
    if (a <= 0.0f)
        return dst;
    if (a > 1.0f)
        a = 1.0f;
    float da = (float)dst.a / 255.0f;
    float oa = a + da * (1.0f - a);
    if (oa <= 0.0f)
        return (rgba){ 0, 0, 0, 0 };
    const uint8_t cs[3] = { c.r, c.g, c.b }, ds[3] = { dst.r, dst.g, dst.b };
    uint8_t out[3];
    for (int i = 0; i < 3; i++)
        out[i] = (uint8_t)clampf(roundf(((float)cs[i] * a + (float)ds[i] * da * (1.0f - a)) / oa), 0.0f, 255.0f);
    return (rgba){ out[0], out[1], out[2], (uint8_t)roundf(oa * 255.0f) };
}

static inline rgba get_px(const img *im, int x, int y)
{
    const uint8_t *p = img_at(im, x, y);
    return (rgba){ p[0], p[1], p[2], p[3] };
}

static inline void put_px(img *im, int x, int y, rgba c)
{
    uint8_t *p = img_at(im, x, y);
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

void raster_copy_rect(img *dst, const img *src, irect r)
{
    r = irect_intersect(irect_intersect(r, irect_of(dst)), irect_of(src));
    if (irect_empty(r))
        return;
    for (int y = r.y0; y < r.y1; y++)
        memcpy(img_at(dst, r.x0, y), img_at(src, r.x0, y), (size_t)(r.x1 - r.x0) * 4);
}

void raster_blend_mask(img *dst, const mask *m, rgba color, float opacity, irect r)
{
    r = irect_intersect(irect_intersect(r, irect_of(dst)), mask_bounds(m));
    if (irect_empty(r))
        return;
    float ca = (float)color.a / 255.0f * opacity;
    for (int ty = r.y0 >> MASK_TILE_SHIFT; ty <= (r.y1 - 1) >> MASK_TILE_SHIFT; ty++)
    {
        for (int tx = r.x0 >> MASK_TILE_SHIFT; tx <= (r.x1 - 1) >> MASK_TILE_SHIFT; tx++)
        {
            const float *t = m->tiles[ty * m->tw + tx];
            if (!t)
                continue;
            int ox = tx << MASK_TILE_SHIFT, oy = ty << MASK_TILE_SHIFT;
            irect c = irect_intersect(r, irect_make(ox, oy, ox + MASK_TILE, oy + MASK_TILE));
            for (int y = c.y0; y < c.y1; y++)
            {
                for (int x = c.x0; x < c.x1; x++)
                {
                    float v = t[((y - oy) << MASK_TILE_SHIFT) + (x - ox)];
                    if (v > 0.0f)
                        put_px(dst, x, y, rgba_over(get_px(dst, x, y), color, v * ca));
                }
            }
        }
    }
}

void raster_fill_rect(img *im, irect r, rgba color)
{
    r = irect_intersect(r, irect_of(im));
    for (int y = r.y0 > 0 ? r.y0 : 0; y < r.y1; y++)
        for (int x = r.x0 > 0 ? r.x0 : 0; x < r.x1; x++)
            put_px(im, x, y, color);
}

img *raster_crop(const img *im, irect r)
{
    r = irect_intersect(r, irect_of(im));
    if (irect_empty(r))
        return img_new(1, 1, (rgba){ 0, 0, 0, 0 });
    img *out = img_new(irect_w(r), irect_h(r), (rgba){ 0, 0, 0, 0 });
    if (!out)
        return NULL;
    for (int y = r.y0; y < r.y1; y++)
        memcpy(img_at(out, 0, y - r.y0), img_at(im, r.x0, y), (size_t)out->w * 4);
    return out;
}

void raster_replace(img *dst, const img *src, int x, int y)
{
    irect r = irect_intersect(irect_make(x, y, x + src->w, y + src->h), irect_of(dst));
    if (irect_empty(r))
        return;
    for (int py = r.y0; py < r.y1; py++)
        memcpy(img_at(dst, r.x0, py), img_at(src, r.x0 - x, py - y), (size_t)(r.x1 - r.x0) * 4);
}

irect raster_blit_over(img *dst, const img *src, int x, int y, const uint8_t *key)
{
    irect r = irect_intersect(irect_make(x, y, x + src->w, y + src->h), irect_of(dst));
    if (irect_empty(r))
        return r;
    for (int py = r.y0; py < r.y1; py++)
    {
        for (int px = r.x0; px < r.x1; px++)
        {
            rgba s = get_px(src, px - x, py - y);
            if (s.a == 0 || (key && key[0] == s.r && key[1] == s.g && key[2] == s.b))
                continue;
            put_px(dst, px, py, s.a == 255 ? s : rgba_over(get_px(dst, px, py), s, (float)s.a / 255.0f));
        }
    }
    return r;
}

/* ---- flood fill ---- */

typedef struct
{
    int x, y;
    float t;
} seed;

typedef struct
{
    seed *v;
    size_t n, cap;
} seed_stack;

static void push(seed_stack *s, int x, int y, float t)
{
    if (s->n == s->cap)
    {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->v = realloc(s->v, s->cap * sizeof(seed));
    }
    s->v[s->n++] = (seed){ x, y, t };
}

static int px_matches(const uint8_t *p, rgba target, int tol)
{
    return abs(p[0] - target.r) <= tol && abs(p[1] - target.g) <= tol && abs(p[2] - target.b) <= tol
        && abs(p[3] - target.a) <= tol;
}

static float dist2(const uint8_t *c, rgba target)
{
    float d0 = (float)c[0] - target.r, d1 = (float)c[1] - target.g, d2 = (float)c[2] - target.b, d3 = (float)c[3] - target.a;
    return d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
}

/* A bit per pixel */
static inline int bit(const uint8_t *b, size_t i)
{
    return b[i >> 3] >> (i & 7) & 1;
}
static inline void set_bit(uint8_t *b, size_t i)
{
    b[i >> 3] |= (uint8_t)(1 << (i & 7));
}

/* Push the in-bounds 4-neighbours of (x, y) in the Rust's order: left, right, up, down */
static void push_neighbours(seed_stack *s, const uint8_t *filled, int w, int h, int x, int y, float t, int unfilled_only)
{
    const int nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
    for (int k = 0; k < 4; k++)
    {
        int nx = nb[k][0], ny = nb[k][1];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
            continue;
        if (unfilled_only && bit(filled, (size_t)ny * w + nx))
            continue;
        push(s, nx, ny, t);
    }
}

static int cmp_fill_px(const void *a, const void *b)
{
    const fill_px *p = a, *q = b;
    return p->y != q->y ? (p->y > q->y) - (p->y < q->y) : (p->x > q->x) - (p->x < q->x);
}

/* Anti-aliased edges are blends of the old colour and the boundary's colour, so a plain
 * fill stops short of them and leaves a pale ring. Walk outwards from the filled area and
 * give each blended pixel the same mix of the new colour and the boundary colour. The walk
 * only continues while pixels get closer to the boundary colour, so it never crosses an
 * outline, and pixels that simply match the old colour (unconnected areas) are left alone.
 * The re-mixed pixels go into the plan (sorted by row), for the fill to write. */
static irect fill_antialiased_edges(const img *im, fill_plan *fp, rgba target, uint8_t tolerance)
{
    int w = im->w, h = im->h;
    const uint8_t *filled = fp->filled;
    rgba color = fp->color;
    uint8_t *edge = calloc(((size_t)w * h + 7) / 8, 1);
    if (!edge)
        return IRECT_EMPTY;
    seed_stack stack = { 0 };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            size_t i = (size_t)y * w + x;
            if (!(i & 7) && x + 8 <= w && !filled[i >> 3])
            {
                x += 7;
                continue;
            }
            if (bit(filled, i))
                push_neighbours(&stack, filled, w, h, x, y, 0.0f, 1);
        }
    float min_dist = fmaxf((float)tolerance, 8.0f);
    size_t capedits = 0;
    while (stack.n)
    {
        seed sd = stack.v[--stack.n];
        int x = sd.x, y = sd.y;
        size_t i = (size_t)y * w + x;
        if (bit(filled, i) || bit(edge, i))
            continue;
        const uint8_t *n = img_at(im, x, y);
        if (sqrtf(dist2(n, target)) <= min_dist)
            continue;
        /* The boundary colour: whichever unfilled pixel around here is least like the old colour. */
        const uint8_t *boundary = n;
        for (int ny = y > 0 ? y - 1 : 0; ny <= (y + 1 < h - 1 ? y + 1 : h - 1); ny++)
        {
            for (int nx = x > 0 ? x - 1 : 0; nx <= (x + 1 < w - 1 ? x + 1 : w - 1); nx++)
            {
                size_t j = (size_t)ny * w + nx;
                const uint8_t *c = img_at(im, nx, ny);
                if (!bit(filled, j) && !bit(edge, j) && dist2(c, target) > dist2(boundary, target))
                    boundary = c;
            }
        }
        const uint8_t tg[4] = { target.r, target.g, target.b, target.a };
        float dot = 0.0f, len2 = 0.0f;
        for (int k = 0; k < 4; k++)
        {
            float dn = (float)n[k] - tg[k], db = (float)boundary[k] - tg[k];
            dot += dn * db;
            len2 += db * db;
        }
        float t = clampf(dot / len2, 0.0f, 1.0f);
        if (t >= 0.97f || t <= sd.t + 0.01f)
            continue;
        set_bit(edge, i);
        const uint8_t cc[4] = { color.r, color.g, color.b, color.a };
        uint8_t mix[4];
        for (int k = 0; k < 4; k++)
            mix[k] = (uint8_t)roundf((float)cc[k] * (1.0f - t) + (float)boundary[k] * t);
        if (fp->n_edges == capedits)
        {
            capedits = capedits ? capedits * 2 : 256;
            fp->edges = realloc(fp->edges, capedits * sizeof(fill_px));
        }
        fp->edges[fp->n_edges++] = (fill_px){ x, y, { mix[0], mix[1], mix[2], mix[3] } };
        push_neighbours(&stack, filled, w, h, x, y, t, 0);
    }
    irect out = IRECT_EMPTY;
    for (size_t k = 0; k < fp->n_edges; k++)
        out = irect_union(out, irect_make(fp->edges[k].x, fp->edges[k].y, fp->edges[k].x + 1, fp->edges[k].y + 1));
    if (fp->n_edges)
        qsort(fp->edges, fp->n_edges, sizeof(fill_px), cmp_fill_px);
    free(stack.v);
    free(edge);
    return out;
}

int raster_fill_plan(const img *im, int x, int y, rgba color, uint8_t tolerance, fill_plan *fp)
{
    int w = im->w, h = im->h;
    memset(fp, 0, sizeof *fp);
    fp->r = IRECT_EMPTY;
    fp->color = color;
    if (x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    rgba target = get_px(im, x, y);
    if (rgba_eq(target, color) && tolerance == 0)
        return 0;
    /* The fill is worked out without writing a pixel: what it has filled is in `visited` */
    uint8_t *visited = calloc(((size_t)w * h + 7) / 8, 1);
    if (!visited)
        return 0;
    seed_stack stack = { 0 };
    push(&stack, x, y, 0.0f);
    irect out = IRECT_EMPTY;
    int tol = tolerance;
    while (stack.n)
    {
        seed sd = stack.v[--stack.n];
        int sx = sd.x, sy = sd.y;
        size_t row = (size_t)sy * w;
        if (bit(visited, row + sx) || !px_matches(img_at(im, sx, sy), target, tol))
            continue;
        int lx = sx;
        while (lx > 0 && !bit(visited, row + lx - 1) && px_matches(img_at(im, lx - 1, sy), target, tol))
            lx--;
        int rx = sx;
        while (rx + 1 < w && !bit(visited, row + rx + 1) && px_matches(img_at(im, rx + 1, sy), target, tol))
            rx++;
        for (int px = lx; px <= rx; px++)
            set_bit(visited, row + px);
        out = irect_union(out, irect_make(lx, sy, rx + 1, sy + 1));
        const int nys[2] = { sy - 1, sy + 1 };
        for (int k = 0; k < 2; k++)
        {
            int ny = nys[k];
            if (ny < 0 || ny >= h)
                continue;
            size_t nrow = (size_t)ny * w;
            int px = lx;
            while (px <= rx)
            {
                if (!bit(visited, nrow + px) && px_matches(img_at(im, px, ny), target, tol))
                {
                    push(&stack, px, ny, 0.0f);
                    while (px <= rx && !bit(visited, nrow + px) && px_matches(img_at(im, px, ny), target, tol))
                        px++;
                }
                else
                    px++;
            }
        }
    }
    free(stack.v);
    fp->w = w;
    fp->filled = visited;
    fp->r = irect_union(out, fill_antialiased_edges(im, fp, target, tolerance));
    return 1;
}

void raster_fill_row(const fill_plan *fp, const img *im, int y, uint8_t *out)
{
    irect r = fp->r;
    if (out != img_at(im, r.x0, y))
        memcpy(out, img_at(im, r.x0, y), (size_t)irect_w(r) * 4);
    const uint8_t *row = fp->filled;
    size_t base = (size_t)y * fp->w;
    for (int x = r.x0; x < r.x1; x++)
        if (bit(row, base + x))
            memcpy(out + (size_t)(x - r.x0) * 4, &fp->color, 4);
    /* The edge pixels of this row: a binary search for its first */
    size_t lo = 0, hi = fp->n_edges;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (fp->edges[mid].y < y)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < fp->n_edges && fp->edges[lo].y == y; lo++)
        memcpy(out + (size_t)(fp->edges[lo].x - r.x0) * 4, &fp->edges[lo].c, 4);
}

void raster_fill_apply(const fill_plan *fp, img *im)
{
    for (int y = fp->r.y0; y < fp->r.y1; y++)
        raster_fill_row(fp, im, y, img_at(im, fp->r.x0, y));
}

void raster_fill_free(fill_plan *fp)
{
    free(fp->filled);
    free(fp->edges);
    memset(fp, 0, sizeof *fp);
}

irect raster_flood_fill(img *im, int x, int y, rgba color, uint8_t tolerance)
{
    fill_plan fp;
    if (!raster_fill_plan(im, x, y, color, tolerance, &fp))
        return IRECT_EMPTY;
    raster_fill_apply(&fp, im);
    irect r = fp.r;
    raster_fill_free(&fp);
    return r;
}

img *raster_resize_canvas(const img *im, int w, int h, rgba fill)
{
    img *out = img_new(w > 1 ? w : 1, h > 1 ? h : 1, fill);
    if (out)
        raster_replace(out, im, 0, 0);
    return out;
}

img *raster_invert(const img *im)
{
    img *out = img_dup(im);
    if (!out)
        return NULL;
    size_t n = (size_t)out->w * out->h * 4;
    for (size_t i = 0; i < n; i += 4)
    {
        out->px[i] = 255 - out->px[i];
        out->px[i + 1] = 255 - out->px[i + 1];
        out->px[i + 2] = 255 - out->px[i + 2];
    }
    return out;
}
