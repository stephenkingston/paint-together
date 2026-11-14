/*
 * Paint (C port) - drawing the app icon (Rust: logo.rs)
 *
 * Shapes are given in 512-unit coordinates and scaled to the picture; each
 * is filled through a coverage mask with a colour that may vary across it
 * (a function of the 512-unit position).
 */
#include <math.h>
#include <stdlib.h>
#include "paint/logo.h"
#include "paint/raster.h"
#include "paint/transform.h"

#define BASE 512
#define TAU 6.28318530717958647692f
#define PI_F 3.14159265358979323846f

static rgba hex(uint32_t rgb)
{
    return (rgba){ (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255 };
}

static rgba hexa(uint32_t rgb, uint8_t a)
{
    return (rgba){ (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, a };
}

/* Rust's `as u8` after round(): saturating, NaN is 0 */
static uint8_t sat_u8(float v)
{
    return v != v || v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
}

static rgba mix(rgba a, rgba b, float t)
{
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;      /* NaN stays NaN, as with f32::clamp */
    const uint8_t x[4] = { a.r, a.g, a.b, a.a }, y[4] = { b.r, b.g, b.b, b.a };
    uint8_t o[4];
    for (int i = 0; i < 4; i++)
        o[i] = sat_u8(roundf((float)x[i] + ((float)y[i] - (float)x[i]) * t));
    return (rgba){ o[0], o[1], o[2], o[3] };
}

/* The colour of a fill at a point (512 units) */
typedef rgba (*paint_fn)(float x, float y, const void *ctx);

typedef struct
{
    img *im;
    mask *m;
    float k;            /* pixels per unit */
    pt *buf;            /* scaled points */
} canvas;

static void composite(canvas *cv, irect r, paint_fn color, const void *ctx)
{
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++)
        {
            float cov = mask_get(cv->m, x, y);
            if (cov <= 0.0f)
                continue;
            rgba c = color(((float)x + 0.5f) / cv->k, ((float)y + 0.5f) / cv->k, ctx);
            uint8_t *p = img_at(cv->im, x, y);
            rgba o = rgba_over((rgba){ p[0], p[1], p[2], p[3] }, c, cov * (float)c.a / 255.0f);
            p[0] = o.r, p[1] = o.g, p[2] = o.b, p[3] = o.a;
        }
    mask_reset(cv->m);
}

static const pt *scaled(canvas *cv, const pt *pts, int n)
{
    for (int i = 0; i < n; i++)
        cv->buf[i] = pt_make(pts[i].x * cv->k, pts[i].y * cv->k);
    return cv->buf;
}

static void fill(canvas *cv, const pt *pts, int n, paint_fn color, const void *ctx)
{
    irect r = mask_fill_polygon(cv->m, scaled(cv, pts, n), n, 1.0f);
    composite(cv, r, color, ctx);
}

static rgba solid(float x, float y, const void *ctx)
{
    return *(const rgba *)ctx;
}

static void fill_solid(canvas *cv, const pt *pts, int n, rgba c)
{
    fill(cv, pts, n, solid, &c);
}

static void stroke(canvas *cv, const pt *pts, int n, float width, rgba c)
{
    irect r = mask_stroke_path(cv->m, scaled(cv, pts, n), n, 1, width * cv->k * 0.5f);
    composite(cv, r, solid, &c);
}

static float signum(float v)
{
    return copysignf(1.0f, v);
}

static int superellipse(pt c, float a, float n, pt *out)
{
    for (int i = 0; i < 720; i++)
    {
        float t = (float)i / 720.0f * TAU, s = sinf(t), co = cosf(t);
        out[i] = pt_make(c.x + a * signum(co) * powf(fabsf(co), 2.0f / n), c.y + a * signum(s) * powf(fabsf(s), 2.0f / n));
    }
    return 720;
}

static pt rotate(float x, float y, float a)
{
    float s = sinf(a), c = cosf(a);
    return pt_make(x * c - y * s, x * s + y * c);
}

static int ellipse(pt c, float rx, float ry, float rot, pt *out)
{
    for (int i = 0; i < 180; i++)
    {
        float t = (float)i / 180.0f * TAU;
        pt v = rotate(rx * cosf(t), ry * sinf(t), rot);
        out[i] = pt_make(c.x + v.x, c.y + v.y);
    }
    return 180;
}

static void shift(pt *pts, int n, float dx, float dy)
{
    for (int i = 0; i < n; i++)
        pts[i].x += dx, pts[i].y += dy;
}

/* ---- the colour functions ---- */

typedef struct
{
    float top, a;
} tile_ctx;

static rgba background(float x, float y, const void *ctx)
{
    const tile_ctx *t = ctx;
    return mix(hex(0xFFFFFF), hex(0xE2E8F2), powf((y - t->top) / (2.0f * t->a), 1.3f));
}

static rgba wood(float x, float y, const void *ctx)
{
    return mix(hex(0xFCE9C6), hex(0xD99E52), ((x - 90.0f) + (y - 160.0f)) / 500.0f);
}

typedef struct
{
    rgba light, dark;
    pt c;
} dab_ctx;

static rgba dab(float x, float y, const void *ctx)
{
    const dab_ctx *d = ctx;
    float dx = x - (d->c.x - 11.0f), dy = y - (d->c.y - 12.0f);
    return mix(d->light, d->dark, sqrtf(dx * dx + dy * dy) / 55.0f);
}

typedef struct
{
    pt end, d, n;
    float len;
} brush_ctx;

static float across(const brush_ctx *b, float x, float y)
{
    return (x - b->end.x) * b->n.x + (y - b->end.y) * b->n.y;
}

static float along(const brush_ctx *b, float x, float y)
{
    return ((x - b->end.x) * b->d.x + (y - b->end.y) * b->d.y) / b->len;
}

static rgba handle(float x, float y, const void *ctx)
{
    return mix(hex(0x4A68B0), hex(0x14203D), (across(ctx, x, y) / 11.0f + 1.0f) / 2.0f);
}

static rgba ferrule(float x, float y, const void *ctx)
{
    return mix(hex(0xF4F6F9), hex(0x8691A0), (across(ctx, x, y) / 11.5f + 1.0f) / 2.0f);
}

static rgba bristles(float x, float y, const void *ctx)
{
    float paint = (along(ctx, x, y) - 0.76f) / 0.08f;
    paint = paint < 0.0f ? 0.0f : paint > 1.0f ? 1.0f : paint;
    rgba base = mix(hex(0x5A3E2B), hex(0xE5483D), paint);
    return mix(mix(base, hex(0xFFFFFF), 0.18f), mix(base, hex(0x000000), 0.25f), (across(ctx, x, y) / 12.0f + 1.0f) / 2.0f);
}

/* ---- the brush ---- */

/* (position along the brush, width) */
static const float PROFILE[9][2] = {
    { 0.0f, 12.0f }, { 0.08f, 14.0f }, { 0.55f, 21.0f }, { 0.555f, 23.0f }, { 0.70f, 23.0f },
    { 0.705f, 20.0f }, { 0.80f, 26.0f }, { 0.92f, 16.0f }, { 1.0f, 0.0f },
};

static pt brush_at(const brush_ctx *b, float s, float w)
{
    return pt_make(b->end.x + b->d.x * (b->len * s) + b->n.x * w, b->end.y + b->d.y * (b->len * s) + b->n.y * w);
}

/* The outline of the part of the brush from `from` to `to` along it */
static int brush_outline(const brush_ctx *b, float from, float to, pt *out)
{
    int idx[9], n = 0, k = 0;
    for (int i = 0; i < 9; i++)
        if (PROFILE[i][0] >= from - 1e-4f && PROFILE[i][0] <= to + 1e-4f)
            idx[n++] = i;
    for (int i = 0; i < n; i++)
        out[k++] = brush_at(b, PROFILE[idx[i]][0], -PROFILE[idx[i]][1] / 2.0f);
    for (int i = n - 1; i >= 0; i--)
        out[k++] = brush_at(b, PROFILE[idx[i]][0], PROFILE[idx[i]][1] / 2.0f);
    return k;
}

static void draw_brush(canvas *cv, pt *pts)
{
    brush_ctx b;
    b.end = pt_make(436.0f, 70.0f);
    pt tip = pt_make(250.0f, 266.0f);
    b.len = hypotf(tip.x - b.end.x, tip.y - b.end.y);
    b.d = pt_make((tip.x - b.end.x) / b.len, (tip.y - b.end.y) / b.len);
    b.n = pt_make(-b.d.y, b.d.x);

    /* Shadow cast on the palette and tile. */
    int n = brush_outline(&b, 0.0f, 1.0f, pts);
    shift(pts, n, 9.0f, 13.0f);
    fill_solid(cv, pts, n, (rgba){ 40, 30, 20, 55 });
    /* Lacquered handle with a rounded end. */
    n = ellipse(brush_at(&b, 0.0f, 0.0f), 6.0f, 6.0f, 0.0f, pts);
    fill(cv, pts, n, handle, &b);
    n = brush_outline(&b, 0.0f, 0.555f, pts);
    fill(cv, pts, n, handle, &b);
    pt glint[2] = { brush_at(&b, 0.06f, -3.0f), brush_at(&b, 0.5f, -6.0f) };
    stroke(cv, glint, 2, 2.0f, (rgba){ 255, 255, 255, 60 });
    /* Metal ferrule with crimp lines. */
    n = brush_outline(&b, 0.555f, 0.70f, pts);
    fill(cv, pts, n, ferrule, &b);
    const float crimps[2] = { 0.6f, 0.645f };
    for (int i = 0; i < 2; i++)
    {
        pt line[2] = { brush_at(&b, crimps[i], -11.0f), brush_at(&b, crimps[i], 11.0f) };
        stroke(cv, line, 2, 1.6f, hexa(0x5A6472, 150));
    }
    /* Bristles, dipped in red paint. */
    n = brush_outline(&b, 0.705f, 1.0f, pts);
    fill(cv, pts, n, bristles, &b);
    n = ellipse(brush_at(&b, 0.86f, -4.0f), 7.0f, 3.0f, atan2f(b.d.y, b.d.x), pts);
    fill_solid(cv, pts, n, (rgba){ 255, 255, 255, 110 });
}

/* A point of the palette: (u, v) on its oval, rotated */
static pt palette_at(float u, float v)
{
    pt r = rotate(u * 150.0f, v * 116.0f, -(14.0f * (PI_F / 180.0f)));
    return pt_make(236.0f + r.x, 276.0f + r.y);
}

static img *draw(int size)
{
    canvas cv = { img_new(size, size, (rgba){ 0, 0, 0, 0 }), mask_new(size, size), (float)size / (float)BASE, malloc(720 * sizeof(pt)) };
    pt *pts = malloc(720 * sizeof(pt));
    if (!cv.im || !cv.m || !cv.buf || !pts)
    {
        img_free(cv.im);
        cv.im = NULL;
        goto done;
    }

    /* Tile with a soft drop shadow. */
    pt tile_c = pt_make(256.0f, 248.0f);
    float a = 206.0f;
    for (int i = 0; i < 14; i++)
    {
        float k = (float)i;
        int n = superellipse(pt_make(tile_c.x, tile_c.y + (7.0f + k * 0.6f)), a + k * 1.4f, 5.0f, pts);
        fill_solid(&cv, pts, n, (rgba){ 0, 0, 0, 5 });
    }
    tile_ctx tile = { tile_c.y - a, a };
    int n = superellipse(tile_c, a, 5.0f, pts);
    fill(&cv, pts, n, background, &tile);
    n = superellipse(tile_c, a - 0.75f, 5.0f, pts);
    stroke(&cv, pts, n, 1.5f, (rgba){ 0, 0, 0, 24 });

    /* Palette: an oval with a notch beside the thumb hole. */
    float notch = 200.0f * (PI_F / 180.0f);
    pt outline[360];
    for (int i = 0; i < 360; i++)
    {
        float t = (float)i / 360.0f * TAU;
        float dt = fmodf(t - notch + TAU / 2.0f, TAU);
        if (dt < 0.0f)
            dt += TAU;
        dt -= TAU / 2.0f;
        float k = 1.0f - 0.27f * expf(-(dt * dt) / (2.0f * 0.19f * 0.19f));
        outline[i] = palette_at(cosf(t) * k, sinf(t) * k);
    }
    const float shadows[3][3] = { { 6.0f, 18.0f, 14 }, { 4.0f, 12.0f, 18 }, { 2.0f, 6.0f, 24 } };
    for (int s = 0; s < 3; s++)
    {
        for (int i = 0; i < 360; i++)
            pts[i] = pt_make(outline[i].x + shadows[s][0], outline[i].y + shadows[s][1]);
        fill_solid(&cv, pts, 360, (rgba){ 60, 40, 20, (uint8_t)shadows[s][2] });
    }
    fill(&cv, outline, 360, wood, NULL);
    stroke(&cv, outline, 360, 3.0f, hexa(0xA86F33, 150));
    pt hole_c = palette_at(-0.42f, 0.3f);
    n = ellipse(hole_c, 25.0f, 25.0f, 0.0f, pts);
    fill(&cv, pts, n, background, &tile);
    n = ellipse(pt_make(hole_c.x, hole_c.y - 3.0f), 25.0f, 22.0f, 0.0f, pts);
    fill_solid(&cv, pts, n, (rgba){ 90, 60, 30, 26 });
    n = ellipse(hole_c, 25.0f, 25.0f, 0.0f, pts);
    stroke(&cv, pts, n, 3.0f, hexa(0xA86F33, 170));

    /* Paint dabs with a shadow, a shaded body and a glint. */
    const float dabs[5][2] = { { -0.04f, -0.56f }, { 0.38f, -0.5f }, { 0.66f, -0.06f }, { 0.5f, 0.43f }, { 0.08f, 0.56f } };
    const uint32_t colours[5] = { 0xE5483D, 0xF6C232, 0x2FB37A, 0x2F80ED, 0x9B51E0 };
    for (int i = 0; i < 5; i++)
    {
        pt c = palette_at(dabs[i][0], dabs[i][1]);
        n = ellipse(pt_make(c.x + 2.0f, c.y + 6.0f), 31.0f, 29.0f, 0.0f, pts);
        fill_solid(&cv, pts, n, (rgba){ 70, 45, 20, 50 });
        dab_ctx d = { mix(hex(colours[i]), hex(0xFFFFFF), 0.28f), mix(hex(colours[i]), hex(0x000000), 0.22f), c };
        n = ellipse(c, 31.0f, 29.0f, 0.0f, pts);
        fill(&cv, pts, n, dab, &d);
        n = ellipse(pt_make(c.x - 11.0f, c.y - 12.0f), 10.0f, 6.5f, -0.6f, pts);
        fill_solid(&cv, pts, n, (rgba){ 255, 255, 255, 170 });
    }

    draw_brush(&cv, pts);
done:
    mask_free(cv.m);
    free(cv.buf);
    free(pts);
    return cv.im;
}

img *logo_render(int size)
{
    if (size >= BASE)
        return draw(size);
    img *big = draw(BASE);
    if (!big)
        return NULL;
    img *out = img_resize(big, size, size, RESIZE_LANCZOS3);
    img_free(big);
    return out;
}
